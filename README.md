# smsggdj-link-esp32

ESP32 firmware that bridges **Ableton Link → Sega Master System**, so the
[SMSGGDJ](https://github.com/) tracker's `SYNC: IN` mode follows Ableton Live's
tempo and transport.

A Seeed **XIAO ESP32-C3** joins a Link session over WiFi, derives a 24-PPQN tick
clock from the shared beat timeline, and presents a rolling **2-bit counter** on
two open-drain GPIOs into SMS controller **port 2**. The tracker reads the port
once per video frame and advances by the counter delta — so the sync is *counted,
not edge-timed*, making it immune to WiFi/host jitter.

## Hardware

- **Board:** Seeed XIAO ESP32-C3 (WiFi required — Link is LAN UDP multicast).
- **Output:** open-drain into SMS port 2; the ESP32 only pulls low, the SMS
  pull-ups supply the 5 V high (no level shifter).
- **Power:** USB; share **only GND** with the console — do *not* tap port-2 +5 V.

| Counter bit | XIAO pin | GPIO | SMS port-2 pin |
|-------------|----------|------|----------------|
| bit 0 (TR)  | D1       | 3    | 9              |
| bit 1 (TH)  | D2       | 4    | 7              |
| ground      | GND      | —    | 8              |

Full pinout and the DE-9 wiring are in [`WIRING.md`](WIRING.md).

## Build & flash

Arduino-ESP32 via `arduino-cli`:

```sh
# one-time
arduino-cli core install esp32:esp32

# WiFi credentials (git-ignored)
cp secrets.h.example secrets.h   # then edit secrets.h

# compile + upload (replace PORT, e.g. /dev/cu.usbmodem*)
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C3 .
arduino-cli upload  --fqbn esp32:esp32:XIAO_ESP32C3 -p PORT .
arduino-cli monitor -p PORT -c baudrate=115200
```

## Status

Scaffold. The open-drain GPIO output, hardware-timer presenter (monotonic,
≤3 ticks/SMS-frame), and WiFi join are in place. **The Ableton Link integration
is stubbed** — see `link_update_target()` in the sketch and `CLAUDE.md` for the
port plan. Until it's wired in, the counter stays frozen.

See [`CLAUDE.md`](CLAUDE.md) for the full wire contract, hardware decisions, and
architecture.
