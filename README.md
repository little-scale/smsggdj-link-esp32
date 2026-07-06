# smsggdj-link-esp32

ESP32 firmware that bridges **Ableton Link → Sega Master System & Mega Drive**, so
the [SMSGGDJ](https://github.com/little-scale/smsggdj) tracker (SMS/Game Gear) and
[**genmddj**](https://github.com/little-scale/genmddj) (its Mega Drive port) can
follow Ableton Live's tempo and transport in their `SYNC: IN` mode. The Mega Drive
path is **hardware-confirmed** — see [`WIRING.md`](WIRING.md#mega-drive--genesis--genmddj).

A Seeed **XIAO ESP32-C3** joins a Link session over WiFi, derives a 24-PPQN tick
clock from the shared beat timeline, and presents a rolling **2-bit counter** on
two open-drain GPIOs into SMS — or Mega Drive — controller **port 2**. The tracker reads the port
once per video frame and advances by the counter delta — so the sync is *counted,
not edge-timed*, making it immune to WiFi/host jitter.

On a Seeed **XIAO ESP32-S3** the same firmware adds a **USB-MIDI clock** source:
the board enumerates as a USB-MIDI device and follows MIDI clock
(Start/Stop/Continue + 0xF8 ticks) from a DAW over USB — no WiFi required. MIDI
clock is 24 PPQN, exactly the SMS sync rate, so each clock byte is one tick. The
S3 runs both sources and [picks between them](#clock-source-c3-vs-s3) (USB-MIDI
when clock is flowing, else Link). The C3 has no USB-OTG, so it is Link-only.

**Wired-only alternative — Teensy 4.1** ([`teensy/`](teensy/)): a separate
Arduino/Teensyduino sketch for a **wired USB-MIDI** bridge with **no WiFi and no
Link** — a DAW's USB-MIDI clock and notes drive the same two console wires. It
implements just the wired half (MIDI clock → counter, MIDI notes →
[MIDI takeover](MIDI.md)); Teensy's built-in `usbMIDI` makes it simpler than the
S3. Note Teensy 4.x isn't 5 V tolerant, so it needs a level shifter — see
[`teensy/README.md`](teensy/README.md).

## Hardware

- **Board:** Seeed XIAO **ESP32-C3** (Link over WiFi) or **ESP32-S3** (Link +
  USB-MIDI). WiFi is required for Link (LAN UDP multicast); USB-MIDI needs the
  S3's USB-OTG, which the C3 lacks.
- **Output:** open-drain into SMS port 2; the ESP32 only pulls low, the SMS
  pull-ups supply the 5 V high (no level shifter).
- **Power:** USB; share **only GND** with the console — do *not* tap port-2 +5 V.

The two signal lines land on different pads/GPIOs per board (the S3 dodges its
GPIO3 strapping pin); `config.h` selects them by build target:

| Counter bit | C3 pad / GPIO | S3 pad / GPIO | SMS port-2 pin |
|-------------|---------------|---------------|----------------|
| bit 0 (TR)  | D1 / GPIO3    | D3 / GPIO4    | 9              |
| bit 1 (TH)  | D2 / GPIO4    | D4 / GPIO5    | 7              |
| ground      | GND           | GND           | 8              |

Full pinout and the DE-9 wiring are in [`WIRING.md`](WIRING.md).

## Flash a prebuilt binary (no toolchain)

If you just want to use the bridge, you don't need ESP-IDF — flash the released
binary and set WiFi from your phone.

**Easiest — web flasher (Chrome or Edge):** open the
[flasher page](https://little-scale.github.io/smsggdj-link-esp32/), plug in the
XIAO with a USB-C **data** cable, and click **Flash**. Web Serial isn't in
Safari/Firefox, so use desktop Chrome/Edge.

**Or with `esptool`** (needs Python, but no ESP-IDF). Download
`smsggdj-bridge-esp32c3.bin` from the latest
[release](https://github.com/little-scale/smsggdj-link-esp32/releases), then:

```sh
pip install esptool
esptool.py --chip esp32c3 -p PORT write_flash 0x0 smsggdj-bridge-esp32c3.bin
# PORT: /dev/cu.usbmodem* (macOS), /dev/ttyACM* (Linux), or COMx (Windows)
```

The single `.bin` already bundles the bootloader, partition table, and app, so
it flashes at offset `0x0`.

### First-time WiFi setup

A freshly flashed board has no WiFi credentials, so it hosts its own setup
network:

1. On a phone or laptop, join the WiFi network **`SMSGGDJ-setup-XXXX`** (open, no
   password).
2. A setup page opens automatically (captive portal). If it doesn't, browse to
   **`http://192.168.4.1`**.
3. Pick your network, enter its password, tap **Save & connect**. The bridge
   reboots and joins it.

The board must be on the **same LAN** as the computer running Ableton Live —
Link discovers peers over UDP multicast. Credentials are stored on the board; it
reconnects automatically on every boot.

**Change networks later:** open the USB serial console (any serial monitor at
the board's `usbmodem`/`COM` port) and type **`w`** — the board forgets its WiFi
and reboots back into the setup portal.

**No serial console? (S3)** Send the board a **pitch bend on MIDI channel 16**
from any DAW/controller — that opens the same setup portal. It's a deliberately
uncommon message (so it won't fire by accident) and fires once per boot. Remap it
via `MIDI_PORTAL_TRIGGER_STATUS` in `main/config.h`.

## Clock source (C3 vs S3)

The **C3** is Link-only. The **S3** runs two clock sources and switches between
them:

- **Ableton Link** over WiFi (as above), and
- **USB-MIDI clock** — the S3 enumerates as a USB-MIDI device. Enable "MIDI
  clock out" to it in your DAW; **Start** launches from row 0, **Stop** freezes,
  **Continue** resumes, and each clock tick advances the SMS by one 1/24-beat
  tick. No WiFi needed for MIDI-only use.

By default the source is **`auto`**: USB-MIDI takes over whenever clock is
actively flowing, otherwise the bridge follows Link. Force it from the serial
console with **`c link`**, **`c midi`**, or **`c auto`** (`s` saves the choice as
the power-on default). On the S3 the serial console rides the **same USB-C cable**
as the MIDI port (a composite CDC device), so one connection gives you both.

> ⚠️ **Send MIDI Clock *or* notes to the S3 — not both at once.** The bridge serves
> the two port wires in one of two mutually-exclusive roles, auto-selected by
> whether channel-voice MIDI (notes/CC) is arriving:
> - **Clock sync** (console `SYNC: IN24`): the S3 presents the **24-PPQN counter**.
>   Send **MIDI Clock only** — no notes/CC on that port.
> - **Note takeover** (console `SYNC: MIDI`): the S3 streams **note events** on the
>   wire and the tracker plays them on its own voices.
>
> If notes/CC arrive while you're trying to clock-sync, they **auto-engage takeover**
> and hijack the wire from the counter — the tracker (IN24) then reads note-bits as
> counter ticks, giving erratic/irregular movement. Fixes: mute the note track so
> only clock flows, or force the role on the console — **`k off`** (never takeover,
> always counter) or **`k on`** (always takeover). `k auto` (default) picks by
> traffic.

> Latency note: in MIDI mode the **tick** offset (`t`) works as usual; the **ms**
> offset (`m`) is Link-only (MIDI clock is reactive, so it can't be nudged
> *earlier* in time). USB-MIDI latency is sub-millisecond, so it rarely needs
> tuning.

## Build from source (developers)

This is an [ESP-IDF](https://docs.espressif.com/projects/esp-idf/) **v5.5**
project (not Arduino): Ableton Link's networking depends on the `espressif/asio`
managed component, which only the IDF component manager can fetch — vanilla asio
will not compile for ESP32.

### 1. Install ESP-IDF v5.5 (once per machine)

Follow Espressif's [getting-started
guide](https://docs.espressif.com/projects/esp-idf/en/v5.5/esp32c3/get-started/index.html),
or on macOS/Linux:

```sh
mkdir -p ~/esp && cd ~/esp
git clone -b v5.5 --recursive https://github.com/espressif/esp-idf.git
cd ~/esp/esp-idf
./install.sh esp32c3,esp32s3   # toolchains: C3 = RISC-V, S3 = Xtensa
```

### 2. Activate the IDF environment (once per terminal)

Every new shell where you build must source the export script — it puts `idf.py`
and the compiler on your `PATH`:

```sh
. ~/esp/esp-idf/export.sh
```

### 3. Get this project's code

```sh
git clone https://github.com/little-scale/smsggdj-link-esp32.git
cd smsggdj-link-esp32
git submodule update --init lib/link     # Ableton Link (header-only)
```

### 4. (Optional) hard-code WiFi for development

You can skip this — a dev build with no `secrets.h` brings up the same
[setup portal](#first-time-wifi-setup) as a shipped binary. But if you'd rather
your dev board connect instantly without the portal, drop in compile-time creds:

```sh
cp main/secrets.h.example main/secrets.h
# then open main/secrets.h and set WIFI_SSID / WIFI_PASS
```

`secrets.h` is git-ignored, and release builds (`-DSHIP=1`, below) ignore it
entirely — they always provision over the portal, so your creds never ship in a
distributed binary.

### 5. Select the chip target (once per checkout)

```sh
idf.py set-target esp32c3      # or: idf.py set-target esp32s3
```

The S3 build adds the USB-MIDI clock source (the `espressif/esp_tinyusb`
component is fetched automatically and only for that target).

### 6. Build

```sh
idf.py build
```

The first build is slow — the component manager downloads `espressif/asio` and
compiles Link + asio + WiFi. Later builds are incremental.

### 7. Flash and watch the serial log

Plug the XIAO in over USB. Find its port, then flash + open the monitor:

```sh
ls /dev/cu.usbmodem*                       # macOS — note the device name
# Linux: ls /dev/ttyACM*   |   Windows: check Device Manager for COMx

idf.py -p /dev/cu.usbmodem1101 flash monitor   # use your port
```

`monitor` stays open and prints the WiFi join and sync state — keep this one
session open for live latency tuning (below). Press `Ctrl-]` to exit.

> If the build can't find `idf.py`, you skipped step 2 in this terminal.

> **S3 console:** the interactive console (`m`/`t`/`c`/`w`/…) and runtime logs
> come up over **USB-CDC** once enumerated; early-boot logs go to UART0. If
> `idf.py monitor` shows nothing at boot, the board is just provisioning USB —
> the prompt appears after it enumerates.

## Status

**Working on hardware.** A XIAO ESP32-C3 joins a Link session over WiFi, follows
Live's tempo and transport, launches on beat 1 of the next bar after start, and
drives a real SMS via `SYNC: IN` in time. Tick rate verified exact (135 BPM = 54
ticks/s) and output latency tuned out with a +75 ms alignment offset (the default
in `main/config.h`).

Implemented: open-drain GPIO output, the monotonic ≤3-ticks/SMS-frame presenter
on a hardware `gptimer` ISR, WiFi station join, bar-aligned launch, and live
serial offset tuning persisted in NVS (`main/main.cpp`).

The **ESP32-S3 USB-MIDI** mode is **verified on hardware**: it enumerates as a
composite USB device (`SMSGGDJ Link Bridge` — MIDI + CDC console on one cable),
follows MIDI clock (Start/Stop + 0xF8 → the SMS counter), auto-switches between
USB-MIDI and Link, and opens the WiFi setup portal from a pitch bend on MIDI
ch16. The only S3 piece not yet checked against a real console is the **GPIO
output into an actual SMS** (same wiring story as the C3).

> Note: Ableton's **Start Stop Sync** must be enabled (separate from the Link
> toggle) for the bridge to follow transport — otherwise it sees `playing=0` and
> holds.

### Planned: MIDI takeover

Beyond clock, the same S3 and the same two wires can carry **MIDI notes** into the
tracker — turning the console into a live multi-part MIDI sound module played from a
DAW (channel → voice, program change → instrument, velocity → volume, note-off →
release). It's a **console-clocked serial** layer over the existing link, designed so
**one firmware** serves both SMSGGDJ (SMS/GG) and genmddj (Mega Drive). Concept and
wire contract in [`MIDI.md`](MIDI.md) — designed, not built yet.

## Tuning latency

Output a touch late/early? Adjust the alignment offset live over the serial
console (`idf.py -p PORT monitor`), then persist your chosen value as the
power-on default:

| cmd        | effect                                                        |
|------------|---------------------------------------------------------------|
| `m <ms>`   | set ms offset (signed). **+ms = emit earlier** (fixes "late") |
| `t <ticks>`| set offset in whole 1/24-beat ticks (signed)                  |
| `+` / `-`  | nudge ms by ±1 for fine dialing                               |
| `z`        | zero both                                                     |
| `s`        | save current offsets to NVS as the power-on default           |
| `p`        | print current offsets                                         |
| `c <src>`  | **S3:** clock source `auto` / `link` / `midi`                  |
| `w`        | WiFi setup (C3: reboot into portal; S3: open portal on demand) |

ms is tempo-independent and finest; ticks are coarse/structural. You can also
bake a final value into `DEFAULT_OFFSET_MS` / `DEFAULT_OFFSET_TICKS` in
`main/config.h`.

See [`CLAUDE.md`](CLAUDE.md) for the full wire contract, hardware decisions, and
architecture, [`WIRING.md`](WIRING.md) for the pinout, and [`MIDI.md`](MIDI.md) for
the planned MIDI-takeover concept (USB-MIDI notes → the console, one firmware for
both trackers).

## Cut a release (maintainers)

[`tools/make-release.sh`](tools/make-release.sh) produces the shippable,
credential-free binaries (both boards by default) and stages the web flasher:

```sh
. ~/esp/esp-idf/export.sh
tools/make-release.sh            # both targets; or: tools/make-release.sh esp32s3
```

For each target it clean-builds with `-DSHIP=1` (forces provisioning regardless
of any local `secrets.h`), merges bootloader + partition table + app into one
image, writes `dist/smsggdj-bridge-<chip>.bin`, and copies it to `docs/firmware/`
with the version stamped into [`docs/manifest.json`](docs/manifest.json). The web
flasher lists both builds and auto-selects the one matching the connected board.

To publish the [web flasher](https://little-scale.github.io/smsggdj-link-esp32/):
commit `docs/` (the merged `.bin` under `docs/firmware/` is intentionally not
git-ignored) and enable **GitHub Pages → Source: `main` `/docs`**. Attach the
same `.bin` to a GitHub Release for the `esptool` path.

## License

**GPL-2.0-or-later** — full text in [`LICENSE`](LICENSE). The ESP32 firmware links
[Ableton Link](https://github.com/Ableton/link), which is GPL-2.0-or-later, so the
firmware and its binary are too. You may share the binary freely, but distributing
it carries the GPL obligation to make the corresponding source available; keeping
this repository public and linked from the flasher/release satisfies that.

The **Teensy subproject** ([`teensy/`](teensy/)) does *not* link Ableton Link, so it
isn't independently obligated to the GPL — but it's distributed under the **same
GPL-2.0-or-later** as the rest of this repository, for simplicity and consistency.
