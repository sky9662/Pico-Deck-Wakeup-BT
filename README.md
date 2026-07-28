# Pico Deck Wakeup BT

Wake a Steam Deck over Bluetooth from a local HTTP request using a Raspberry
Pi Pico 2 W.

This project keeps the HTTP interface used by the original
[Pico Deck Wakeup](https://github.com/sky9662/Pico-Deck-Wakeup):

```text
GET http://<PICO_IP>:5000/w
```

The Pico receives the request over Wi-Fi, actively reconnects its Bluetooth
Classic HID gamepad when necessary, and sends one gamepad button press. The
complete sleep-to-wake path has been tested successfully on real Pico 2 W and
Steam Deck hardware.

No Steam Deck system-file changes, `udev` rules, USB HID spoofing, or USB
connection to the Deck are required.

## Features

- Provides a simple `GET /w` wake API
- Uses a Bluetooth gamepad report instead of an F13 keyboard report
- Actively reconnects Bluetooth after the Steam Deck goes to sleep
- Saves the paired host address across normal power cycles
- Uses the onboard BOOTSEL button for testing and re-pairing
- Disables Bluetooth discovery after pairing
- Provides plain-text status and meaningful HTTP response codes
- Keeps Wi-Fi credentials in a Git-ignored private file

## Requirements

- Raspberry Pi Pico 2 W
- Steam Deck with Bluetooth enabled
- A stable USB power source for the Pico
- A 2.4 GHz Wi-Fi network
- [Earle Philhower's Arduino-Pico core](https://github.com/earlephilhower/arduino-pico)

Arduino-Pico 6.0.0 is the tested core version. With that version, the sketch
uses approximately 480 KB of flash and 94 KB of RAM.

## Configure Wi-Fi

Copy the example configuration:

```sh
cp config.h.example config.h
```

Edit the private `config.h`:

```cpp
#pragma once

constexpr char WIFI_SSID[] = "YOUR_WIFI_SSID";
constexpr char WIFI_PASSWORD[] = "YOUR_WIFI_PASSWORD";
```

`config.h` is excluded by `.gitignore`.

> [!WARNING]
> Wi-Fi credentials are embedded in the compiled firmware. Do not publish a
> `.uf2`, `.bin`, or `.elf` built with your private `config.h`.

## Build

Use these Arduino IDE settings:

1. Board: **Raspberry Pi Pico 2 W**
2. IP/Bluetooth Stack: **IPv4 + Bluetooth**
3. TinyUSB: not required

The equivalent Arduino CLI board identifier is:

```text
rp2040:rp2040:rpipico2w:ipbtstack=ipv4btcble
```

Example CLI build:

```sh
arduino-cli compile \
  --fqbn rp2040:rp2040:rpipico2w:ipbtstack=ipv4btcble \
  .
```

## Flash

1. Unplug the Pico.
2. Hold BOOTSEL while reconnecting USB.
3. Release BOOTSEL when the `RP2350` drive appears.
4. Copy the generated `.uf2` file to the drive.
5. The drive disconnects automatically and the firmware starts.

The Pico may be powered by a charger, power bank, or always-powered dock port
after flashing. It does not need to be connected to the Steam Deck.

## Pair with Steam Deck

1. Keep the Steam Deck awake and unlocked.
2. Power the Pico and wait for its onboard LED to blink.
3. Open the Steam Deck Bluetooth settings.
4. Pair with **Pico Deck Wake**.
5. Enable **Allow this device to wake Steam Deck** if SteamOS displays that
   option.
6. Open `http://<PICO_IP>:5000/` and confirm:

   ```text
   Paired host: known
   Pairing mode: idle
   ```

7. Call `http://<PICO_IP>:5000/w` while the Deck is awake.
8. Put the Deck to sleep, wait for it to settle, and call `/w` again.

The Pico uses Bluetooth Classic Secure Simple Pairing in the "Just Works"
mode. No PIN needs to be entered on the Pico.

Installing a new UF2 may require pairing again if the firmware flash layout
changes. Ordinary power cycles preserve the paired host.

## BOOTSEL button

BOOTSEL has two runtime functions:

- Short press: send one test wake-button event
- Hold for four seconds: disconnect the current HID host, clear Pico-side
  Bluetooth pairing data, and enter discoverable pairing mode

The onboard LED blinks while pairing mode is active. It returns to its normal
solid state after a host completes the HID connection and Wi-Fi is connected.

After a long press, remove the old **Pico Deck Wake** entry from SteamOS,
pair it again, and re-enable the wake permission.

BOOTSEL behaves differently during power-on: holding it while connecting USB
enters the RP2350 UF2 bootloader. This does not erase Wi-Fi or pairing data by
itself. Release the button and power-cycle normally to run the firmware.

## HTTP API

| Request | Description |
| --- | --- |
| `GET /w` | Reconnect Bluetooth if needed and send the wake button |
| `GET /` | Return device status |
| `GET /status` | Return the same device status |

`GET /w` returns:

| Status | Meaning |
| --- | --- |
| `200 OK` | Bluetooth button report was sent |
| `409 Conflict` | No paired Bluetooth host is stored |
| `503 Service Unavailable` | Bluetooth reconnect timed out |

A timed-out reconnect may still have started waking the Deck. Wait for the Deck
to finish resuming and call `/w` again.

Use a DHCP reservation if another device or service depends on the Pico
remaining at a stable IP address.

## LED behavior

| LED | State |
| --- | --- |
| Solid | Wi-Fi connected and pairing mode idle |
| Blinking | Wi-Fi startup or Bluetooth pairing mode |
| Off | Wi-Fi disconnected and pairing mode idle |

## Troubleshooting

- If the status page is unreachable, verify the Pico's DHCP lease, 2.4 GHz
  Wi-Fi availability, and credentials in `config.h`.
- If status reports `Paired host: not found`, remove the old device from
  SteamOS and pair **Pico Deck Wake** again.
- If `/w` returns `503`, wait for the Deck to finish resuming and retry.
- If the device repeatedly appears and disappears while pairing, use a stable
  power supply, move the Pico closer to the Deck, and reduce nearby 2.4 GHz
  interference.
- If `/w` returns `200` but the Deck remains asleep, verify that SteamOS still
  allows **Pico Deck Wake** to wake the system.
- If the `RP2350` drive appears instead of the firmware starting, unplug the
  Pico and reconnect it without holding BOOTSEL.

## Security

The HTTP server has no authentication or TLS. Any device that can reach port
5000 on the Pico can trigger a wake attempt. Use it only on a trusted LAN and
do not expose it directly to the internet.

## Compatibility

Wake behavior is controlled by SteamOS and the Steam Deck Bluetooth firmware.
Results may differ between SteamOS releases or on other host devices.

## License

MIT. See [LICENSE](LICENSE).
