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

[ESP-IDF](https://docs.espressif.com/projects/esp-idf/) v5.5. Ableton Link's
networking depends on the `espressif/asio` managed component, which the IDF
component manager fetches automatically — vanilla asio will not compile for
ESP32, so this is an IDF project, not Arduino.

```sh
. ~/esp/esp-idf/export.sh                  # per shell (after installing IDF)
git submodule update --init lib/link       # Ableton Link (header-only)
cp main/secrets.h.example main/secrets.h   # then edit WiFi creds

idf.py set-target esp32c3                   # once
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor   # replace with your port
```

## Status

Running on a XIAO ESP32-C3: it joins a Link session over WiFi, follows tempo and
transport, and advances the 24-PPQN tick target at the correct rate (verified at
135 BPM = 54 ticks/s). Implemented: open-drain GPIO output, the monotonic
≤3-ticks/SMS-frame presenter on a hardware `gptimer` ISR, WiFi station join, and
bar-aligned launch (`main/main.cpp`).

Not yet verified: the electrical output into a real SMS (scope on TR/TH + the
tracker following in `SYNC: IN`). Not yet done: by-ear user offsets + persistence.

See [`CLAUDE.md`](CLAUDE.md) for the full wire contract, hardware decisions, and
architecture, and [`WIRING.md`](WIRING.md) for the pinout.
