/*
 * Pico Deck Wakeup BT
 *
 * A clean Bluetooth HID replacement for the original USB HID project.
 *
 * The HTTP wake API is:
 *     GET http://<pico-ip>:5000/w
 *
 * The Pico receives that request over Wi-Fi and sends one Bluetooth
 * gamepad-button report to a paired Steam Deck.  No USB HID descriptor,
 * USB remote-wakeup call, udev rule, or Steam Deck system-file change is
 * required by this firmware.
 */

#include <WiFi.h>
#include <JoystickBT.h>
#include <PicoBluetoothHID.h>
#include <BluetoothLock.h>
#include <btstack.h>
#include <btstack_tlv.h>
#include "config.h"

// -----------------------------------------------------------------------------
// Configuration
// -----------------------------------------------------------------------------

constexpr uint16_t HTTP_PORT = 5000;
constexpr char BLUETOOTH_NAME[] = "Pico Deck Wake";
constexpr uint8_t WAKE_BUTTON = 1;

// BOOTSEL is used as a convenience button only after the firmware is running.
// Holding it for this long clears the Pico's Classic Bluetooth link keys and
// puts the already-running HID device back into discoverable mode.
constexpr uint32_t PAIRING_RESET_HOLD_MS = 4000;
constexpr uint32_t BUTTON_DEBOUNCE_MS = 35;
constexpr uint32_t PAIRING_LED_BLINK_MS = 250;
constexpr uint32_t WAKE_PRESS_MS = 120;
constexpr uint32_t BLUETOOTH_RECONNECT_TIMEOUT_MS = 8000;
constexpr uint32_t HTTP_READ_TIMEOUT_MS = 1500;
constexpr uint32_t WIFI_RETRY_INTERVAL_MS = 5000;

// Store the most recently connected HID host separately from BTstack's link
// keys. On the first upgraded boot, the existing link-key database is used as
// a fallback so an already-paired Steam Deck normally does not need re-pairing.
constexpr uint32_t DECK_ADDRESS_TLV_TAG =
    (static_cast<uint32_t>('P') << 24) |
    (static_cast<uint32_t>('D') << 16) |
    (static_cast<uint32_t>('W') << 8) |
    static_cast<uint32_t>('A');

WiFiServer server(HTTP_PORT);

// -----------------------------------------------------------------------------
// Status helpers
// -----------------------------------------------------------------------------

uint32_t lastWiFiAttempt = 0;
bool lastBluetoothConnected = false;
bd_addr_t pairedDeckAddress = {0};
bool pairedDeckAddressKnown = false;
bool pairingModeActive = false;
uint16_t outgoingHidCid = 0;

void setStatusLed(bool on) {
  static bool initialized = false;
  static bool previousState = false;

  // LED_BUILTIN is driven through the CYW43 radio chip on Pico W boards.
  // Avoid sending the same command repeatedly while Wi-Fi is connecting so
  // Bluetooth pairing gets as much radio time as possible.
  if (initialized && on == previousState) {
    return;
  }

  initialized = true;
  previousState = on;

  // The Pico LED is active-low on the Pico W family.
  digitalWrite(LED_BUILTIN, on ? LOW : HIGH);
}

void printBluetoothState() {
  const bool connected = PicoBluetoothHID.connected();
  if (connected == lastBluetoothConnected) {
    return;
  }

  lastBluetoothConnected = connected;
  Serial.println(connected ? "Bluetooth HID connected."
                           : "Bluetooth HID disconnected; waiting for pairing/reconnect.");
}

void updateStatusLed() {
  if (pairingModeActive) {
    setStatusLed(((millis() / PAIRING_LED_BLINK_MS) & 1) == 0);
    return;
  }

  setStatusLed(WiFi.status() == WL_CONNECTED);
}

// -----------------------------------------------------------------------------
// Bluetooth HID
// -----------------------------------------------------------------------------

bool addressesMatch(const bd_addr_t first, const bd_addr_t second) {
  return memcmp(first, second, sizeof(bd_addr_t)) == 0;
}

void printBluetoothAddress(const bd_addr_t address) {
  Serial.println(bd_addr_to_str(address));
}

void rememberDeckAddressFromBluetoothCallback(const bd_addr_t address) {
  const bool changed =
      !pairedDeckAddressKnown || !addressesMatch(pairedDeckAddress, address);

  memcpy(pairedDeckAddress, address, sizeof(bd_addr_t));
  pairedDeckAddressKnown = true;

  // HID callbacks already execute inside BTstack's context, so taking a
  // BluetoothLock here would deadlock. This is the same storage pattern used
  // by BTstack's official Classic HID keyboard example.
  if (changed) {
    const btstack_tlv_t *tlv = nullptr;
    void *tlvContext = nullptr;
    btstack_tlv_get_instance(&tlv, &tlvContext);
    if (tlv != nullptr) {
      tlv->store_tag(tlvContext, DECK_ADDRESS_TLV_TAG,
                     reinterpret_cast<const uint8_t *>(pairedDeckAddress),
                     sizeof(bd_addr_t));
    }
  }
}

void handleBluetoothOpened(uint8_t, uint16_t, uint8_t *packet, uint16_t) {
  bd_addr_t address;
  hid_subevent_connection_opened_get_bd_addr(packet, address);
  outgoingHidCid = hid_subevent_connection_opened_get_hid_cid(packet);
  rememberDeckAddressFromBluetoothCallback(address);

  // This callback already runs inside BTstack's context. Stop advertising to
  // new hosts as soon as the selected host completes its HID connection.
  pairingModeActive = false;
  gap_discoverable_control(0);

  Serial.print("Bluetooth HID connected to ");
  printBluetoothAddress(address);
}

void handleBluetoothClosed(uint8_t, uint16_t, uint8_t *, uint16_t) {
  outgoingHidCid = 0;
  Serial.println("Bluetooth HID connection closed.");
}

bool loadPairedDeckAddress() {
  if (pairedDeckAddressKnown) {
    return true;
  }

  BluetoothLock lock;

  const btstack_tlv_t *tlv = nullptr;
  void *tlvContext = nullptr;
  btstack_tlv_get_instance(&tlv, &tlvContext);

  if (tlv != nullptr) {
    const int length =
        tlv->get_tag(tlvContext, DECK_ADDRESS_TLV_TAG,
                     reinterpret_cast<uint8_t *>(pairedDeckAddress),
                     sizeof(bd_addr_t));
    if (length == sizeof(bd_addr_t)) {
      pairedDeckAddressKnown = true;
      return true;
    }
  }

  // Firmware versions before active reconnect support did not have the custom
  // address tag. Recover the address from their existing Classic link key.
  btstack_link_key_iterator_t iterator;
  if (!gap_link_key_iterator_init(&iterator)) {
    return false;
  }

  bd_addr_t address;
  link_key_t linkKey;
  link_key_type_t keyType;
  const bool found =
      gap_link_key_iterator_get_next(&iterator, address, linkKey, &keyType);
  gap_link_key_iterator_done(&iterator);

  if (!found) {
    return false;
  }

  memcpy(pairedDeckAddress, address, sizeof(bd_addr_t));
  pairedDeckAddressKnown = true;

  if (tlv != nullptr) {
    tlv->store_tag(tlvContext, DECK_ADDRESS_TLV_TAG,
                   reinterpret_cast<const uint8_t *>(pairedDeckAddress),
                   sizeof(bd_addr_t));
  }

  return true;
}

void startBluetoothHID() {
  Serial.print("Starting Bluetooth HID as ");
  Serial.println(BLUETOOTH_NAME);

  // This device has no display or keyboard. BTstack disables automatic SSP
  // confirmation by default, while Arduino-Pico's Classic HID wrapper does
  // not answer the confirmation event itself. Enable Just Works pairing so a
  // Steam Deck pairing request cannot time out waiting for a response.
  gap_ssp_set_io_capability(SSP_IO_CAPABILITY_NO_INPUT_NO_OUTPUT);
  gap_ssp_set_auto_accept(1);

  // JoystickBT is deliberate: SteamOS documents controller wake support, and
  // a gamepad button is more likely to be accepted as a wake source than an
  // arbitrary F13 key from a generic Bluetooth keyboard.
  PicoBluetoothHID.setOpenedCB(handleBluetoothOpened);
  PicoBluetoothHID.setClosedCB(handleBluetoothClosed);
  JoystickBT.begin(BLUETOOTH_NAME, BLUETOOTH_NAME);

  {
    BluetoothLock lock;
    gap_connectable_control(1);
  }

  delay(250);
  if (loadPairedDeckAddress()) {
    pairingModeActive = false;
    Serial.print("Paired HID host: ");
    printBluetoothAddress(pairedDeckAddress);
  } else {
    pairingModeActive = true;
    Serial.println("No paired HID host is stored yet.");
  }

  {
    BluetoothLock lock;
    gap_discoverable_control(pairingModeActive ? 1 : 0);
  }

  Serial.println(pairingModeActive
                     ? "Pairing mode active; Bluetooth discovery enabled."
                     : "Pairing mode idle; Bluetooth discovery disabled.");
  printBluetoothState();
}

void clearBluetoothPairingAndEnablePairing() {
  Serial.println("Clearing Bluetooth pairing keys and enabling pairing mode...");

  // This is the Classic Bluetooth equivalent of the clearPairing helper used
  // by Arduino-Pico's Bluetooth HID master.  It clears the Pico-side keys;
  // the old device may also need to be removed from SteamOS Bluetooth settings
  // before pairing again.
  {
    BluetoothLock lock;
    if (PicoBluetoothHID.connected()) {
      hid_device_disconnect(PicoBluetoothHID.getCID());
    }

    gap_delete_all_link_keys();

    const btstack_tlv_t *tlv = nullptr;
    void *tlvContext = nullptr;
    btstack_tlv_get_instance(&tlv, &tlvContext);
    if (tlv != nullptr) {
      tlv->delete_tag(tlvContext, DECK_ADDRESS_TLV_TAG);
    }

    memset(pairedDeckAddress, 0, sizeof(bd_addr_t));
    pairedDeckAddressKnown = false;
    outgoingHidCid = 0;
    pairingModeActive = true;

    gap_connectable_control(1);
    gap_discoverable_control(1);
  }

  Serial.println(
      "Pairing mode is available. Remove the old Pico device from SteamOS, "
      "then pair it again.");
}

constexpr uint8_t WAKE_RESULT_SENT = 0;
constexpr uint8_t WAKE_RESULT_NOT_PAIRED = 1;
constexpr uint8_t WAKE_RESULT_RECONNECT_FAILED = 2;

bool connectToPairedDeck() {
  if (PicoBluetoothHID.connected()) {
    return true;
  }

  if (!loadPairedDeckAddress()) {
    Serial.println("No paired Bluetooth host address is available.");
    return false;
  }

  Serial.print("Reconnecting Bluetooth HID to ");
  printBluetoothAddress(pairedDeckAddress);

  uint8_t status;
  {
    BluetoothLock lock;
    if (PicoBluetoothHID.connected()) {
      return true;
    }
    status = hid_device_connect(pairedDeckAddress, &outgoingHidCid);
  }

  if (status != ERROR_CODE_SUCCESS) {
    Serial.print("Bluetooth reconnect could not start; status 0x");
    Serial.println(status, HEX);
    return false;
  }

  const uint32_t startedAt = millis();
  while (!PicoBluetoothHID.connected() &&
         (millis() - startedAt < BLUETOOTH_RECONNECT_TIMEOUT_MS)) {
    delay(10);
  }

  if (!PicoBluetoothHID.connected()) {
    Serial.println("Bluetooth reconnect timed out.");
    return false;
  }

  Serial.println("Bluetooth HID reconnect completed.");
  return true;
}

uint8_t sendWakeButton(const char *source) {
  printBluetoothState();

  Serial.print("Wake request from ");
  Serial.print(source);
  Serial.print("; Bluetooth ");
  Serial.println(PicoBluetoothHID.connected() ? "connected." : "not connected.");

  if (!PicoBluetoothHID.connected() && !loadPairedDeckAddress()) {
    Serial.println("Wake request rejected because no paired host was found.");
    return WAKE_RESULT_NOT_PAIRED;
  }

  if (!connectToPairedDeck()) {
    return WAKE_RESULT_RECONNECT_FAILED;
  }

  setStatusLed(false);
  JoystickBT.button(WAKE_BUTTON, true);
  delay(WAKE_PRESS_MS);
  JoystickBT.button(WAKE_BUTTON, false);
  setStatusLed(WiFi.status() == WL_CONNECTED);
  Serial.println("Bluetooth wake button sent.");
  return WAKE_RESULT_SENT;
}

// -----------------------------------------------------------------------------
// BOOTSEL button
// -----------------------------------------------------------------------------

bool bootselWasDown = false;
bool pairingResetHandled = false;
uint32_t bootselPressedAt = 0;
uint32_t ignoreBootselUntil = 0;

void initializeBootselButton() {
  // If the button is held while reset/USB insertion occurs, the RP boot ROM
  // may enter UF2 bootloader before this sketch starts.  Ignore the initial
  // state so a normal firmware start cannot be mistaken for a pairing action.
  bootselWasDown = (bool)BOOTSEL;
  ignoreBootselUntil = millis() + 1500;
}

void pollBootselButton() {
  const uint32_t now = millis();
  const bool down = (bool)BOOTSEL;

  if (now < ignoreBootselUntil) {
    bootselWasDown = down;
    return;
  }

  if (down && !bootselWasDown) {
    bootselPressedAt = now;
    pairingResetHandled = false;
  }

  if (down && !pairingResetHandled &&
      (now - bootselPressedAt >= PAIRING_RESET_HOLD_MS)) {
    pairingResetHandled = true;
    clearBluetoothPairingAndEnablePairing();
  }

  if (!down && bootselWasDown) {
    const uint32_t heldFor = now - bootselPressedAt;
    if (!pairingResetHandled && heldFor >= BUTTON_DEBOUNCE_MS) {
      sendWakeButton("BOOTSEL");
    }
  }

  bootselWasDown = down;
}

// -----------------------------------------------------------------------------
// Wi-Fi and HTTP
// -----------------------------------------------------------------------------

void printWiFiAddress() {
  Serial.print("IP Address: ");
  Serial.println(WiFi.localIP());
  Serial.print("Wake URL: http://");
  Serial.print(WiFi.localIP());
  Serial.print(":");
  Serial.print(HTTP_PORT);
  Serial.println("/w");
}

void maintainWiFi() {
  if (WiFi.status() == WL_CONNECTED) {
    return;
  }

  const uint32_t now = millis();
  if (now - lastWiFiAttempt < WIFI_RETRY_INTERVAL_MS) {
    return;
  }

  lastWiFiAttempt = now;
  Serial.println("Wi-Fi disconnected; attempting reconnect...");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
}

bool readHttpRequest(WiFiClient &client, String &requestLine) {
  const uint32_t startedAt = millis();
  String currentLine;
  bool gotRequestLine = false;

  while (client.connected() && (millis() - startedAt < HTTP_READ_TIMEOUT_MS)) {
    while (client.available()) {
      const char c = static_cast<char>(client.read());
      if (c == '\n') {
        if (!gotRequestLine) {
          requestLine = currentLine;
          gotRequestLine = true;
        } else if (currentLine.length() == 0) {
          return true;
        }
        currentLine = "";
        continue;
      }
      if (c != '\r' && currentLine.length() < 256) {
        currentLine += c;
      }
    }
    delay(1);
  }

  return false;
}

bool isWakeRequest(const String &requestLine) {
  return requestLine.startsWith("GET /w ") || requestLine.startsWith("GET /w?");
}

bool isStatusRequest(const String &requestLine) {
  return requestLine.startsWith("GET / ") ||
         requestLine.startsWith("GET /status ") ||
         requestLine.startsWith("GET /status?");
}

String statusMessage() {
  const bool paired = loadPairedDeckAddress();
  String message = "Pico Deck Wake\n";
  message += "Wi-Fi: connected\n";
  message += "Bluetooth HID: ";
  message += PicoBluetoothHID.connected() ? "connected\n" : "disconnected\n";
  message += "Paired host: ";
  message += paired ? "known\n" : "not found\n";
  message += "Pairing mode: ";
  message += pairingModeActive ? "active\n" : "idle\n";
  message += "Wake endpoint: GET /w\n";
  return message;
}

void sendHttpResponse(WiFiClient &client, int statusCode,
                      const char *reason, const String &message) {
  client.print("HTTP/1.1 ");
  client.print(statusCode);
  client.print(" ");
  client.print(reason);
  client.print("\r\n");
  client.print("Content-Type: text/plain; charset=utf-8\r\n");
  client.print("Connection: close\r\n");
  client.print("Cache-Control: no-store\r\n");
  client.print("Content-Length: ");
  client.print(message.length());
  client.print("\r\n");
  client.print("\r\n");
  client.print(message);

  // Wait for ACK before closing, and consume all request headers above. This
  // avoids the TCP reset that HTTP clients could otherwise report even after
  // receiving the response body.
  client.stop(1000);
}

void handleHttp() {
  if (WiFi.status() != WL_CONNECTED) {
    return;
  }

  WiFiClient client = server.accept();
  if (!client) {
    return;
  }

  String requestLine;
  const bool gotRequest = readHttpRequest(client, requestLine);
  const bool wakeRequest = gotRequest && isWakeRequest(requestLine);

  Serial.print("HTTP request: ");
  Serial.println(requestLine);

  if (wakeRequest) {
    const uint8_t result = sendWakeButton("HTTP /w");
    if (result == WAKE_RESULT_SENT) {
      sendHttpResponse(client, 200, "OK",
                       "Pico Deck Wake: Bluetooth wake button sent.\n");
    } else if (result == WAKE_RESULT_NOT_PAIRED) {
      sendHttpResponse(client, 409, "Conflict",
                       "No paired Bluetooth host was found. Pair the Steam Deck first.\n");
    } else {
      sendHttpResponse(
          client, 503, "Service Unavailable",
          "Bluetooth reconnect timed out. The Deck may still be waking; try /w again.\n");
    }
  } else if (gotRequest && isStatusRequest(requestLine)) {
    sendHttpResponse(client, 200, "OK", statusMessage());
  } else if (!gotRequest) {
    sendHttpResponse(client, 400, "Bad Request",
                     "Incomplete HTTP request.\n");
  } else {
    sendHttpResponse(client, 404, "Not Found",
                     "Not found. Use GET /w.\n");
  }
}

// -----------------------------------------------------------------------------
// Arduino lifecycle
// -----------------------------------------------------------------------------

void setup() {
  Serial.begin(115200);
  pinMode(LED_BUILTIN, OUTPUT);
  setStatusLed(false);

  initializeBootselButton();
  startBluetoothHID();

  Serial.println("\n--- Connecting to Wi-Fi ---");
  WiFi.begin(WIFI_SSID, WIFI_PASSWORD);
  lastWiFiAttempt = millis();

  const uint32_t startedAt = millis();
  while (WiFi.status() != WL_CONNECTED && (millis() - startedAt < 30000)) {
    setStatusLed(((millis() / 250) & 1) == 0);
    delay(10);
  }

  server.begin();

  if (WiFi.status() == WL_CONNECTED) {
    setStatusLed(true);
    Serial.println("Wi-Fi connected.");
    printWiFiAddress();
  } else {
    setStatusLed(false);
    Serial.println("Wi-Fi connection timed out; background reconnect is enabled.");
  }
}

void loop() {
  maintainWiFi();
  handleHttp();
  pollBootselButton();
  printBluetoothState();
  updateStatusLed();
  delay(2);
}
