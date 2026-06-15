# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32 firmware: an **Ableton Link → SMS hardware-sync bridge**. The board joins
an Ableton Link session over WiFi, derives a tick clock from the shared beat
timeline, and drives a **2-bit tick counter** onto two GPIOs into a Sega Master
System controller port, so the **SMSGGDJ** tracker (`~/Documents/sms_tracker`,
its `SYNC: IN` mode) follows Ableton Live's tempo and transport.

Status: **working on hardware.** The ESP-IDF firmware in `main/` joins Link,
follows tempo/transport, launches on the next bar after start, and drives a real
SMS via `SYNC: IN` in time (latency tuned out with a +75 ms offset). This file is
the spec + decisions + integration notes.

Sibling reference project: `~/Documents/gba-link-sync` (same idea for a GBA).

## The wire contract (this is the whole protocol — get it exact)

SMSGGDJ's `SYNC: IN` reads controller **port 2** once per video frame and
advances its engine by `(counter − last) & 3` ticks. So the bridge presents a
**rolling 2-bit counter** (0→1→2→3→0…), incremented once per tick:

- **TR = port-2 pin 9 = counter bit 0** ← XIAO **D1 / GPIO3**
- **TH = port-2 pin 7 = counter bit 1** ← XIAO **D2 / GPIO4**
- **GND = port-2 pin 8** (shared) ← XIAO **GND**

Rules:
- **1 tick = 1/24 beat (24 PPQN).** 24 ticks = one Link beat = one bar-quarter.
  (Tracker side: 24 ticks = 4 rows at groove 6; as of tracker v0.24, `SYNC IN`
  forces groove 6 so the song stays aligned regardless of its stored groove.)
- **The counter must never run backward** — the SMS decodes deltas mod 4, so a
  backward step reads as +3. A monotonic, rate-limited presenter enforces this.
- **≤ 3 ticks may be applied per SMS frame read** (the 2-bit delta saturates at
  3). At 24 PPQN this is only a limit above ~450 BPM (NTSC), so it's not a
  practical ceiling — but the presenter must respect it.
- **Bar-aligned launch:** start emitting ticks at the first Link bar line (phase
  0 of the 4-beat quantum) at/after transport start, so the tracker's row 0
  lands on Live's beat 1. Counter freezes while Link transport is stopped
  (tracker holds); resumes on the next count change (the tracker arms on WAIT
  and starts on the first change).
- **Jitter-immune by construction** — it's *counted*, not edge-timed. WiFi/host
  jitter just delays the next count; nothing glitches.

## Hardware (decided)

- **Board: Seeed XIAO ESP32-C3** (target) — single-core RISC-V, ~400 KB RAM,
  2.4 GHz WiFi; good fit because the sync is jitter-tolerant. Dual-core
  **WROOM-32** is the proven-safe fallback (isolate Link's networking from the
  tick timer). WiFi is mandatory (Link = LAN UDP multicast).
- **Pin map (chosen):** TR/bit0 = **D1 (GPIO3)**, TH/bit1 = **D2 (GPIO4)**,
  shared ground = the XIAO **GND** pad. D1/D2 are adjacent non-strapping GPIOs on
  the left rail; GND is on the right rail (ground wire crosses over). **Avoid**
  the C3 strapping pins for these outputs: D0/GPIO2, D8/GPIO8, D9/GPIO9 (GPIO9 =
  boot-mode) — driving them at boot can brick the flash sequence. Full pin tables
  and the DE-9 pinout are in **`WIRING.md`**.
- **Output: open-drain** on the two signal GPIOs (`GPIO_MODE_OUTPUT_OD`;
  `gpio_set_level` 1 = release/high-Z, 0 = drive low). The ESP32 (3.3 V) only
  pulls the lines **low**; the SMS's internal pull-ups supply the 5 V high. No
  level shifter needed.
- **Power: USB (own supply); share only GND with port 2.** Do **not** tap the
  port's +5 V (pin 5) — WiFi current peaks risk sagging the console's 5 V rail.
- Tick edges should come from a **hardware timer ISR** (toggling the two GPIOs),
  independent of the WiFi/main loop.

## Architecture (implemented in `main/main.cpp`)

Ported from the ares emulator fork (see References):
- Join Link (`ableton::Link`), `enableStartStopSync(true)`, `enable(true)`.
- Per scheduling instant, `target = floor(beatsSinceLaunch × 24)` (see
  `computeTarget()`). No frame-PLL — that was emulator-only (vsync pacing).
- A **monotonic presenter** in the `gptimer` ISR (`onPresenterTimer`): advance
  `presented` toward `target`, never backward, ≤ 3 per SMS frame; output
  `presented & 3` on TR/TH (open-drain `gpio_set_level`).
- `beatsSinceLaunch` uses the captured session state + `phaseAtTime` to find the
  bar line; negative = armed (pre-bar) and freezes the counter.
- The 64-bit `target` is shared loop→ISR via a `portMUX` spinlock (a 64-bit
  `std::atomic` is not lock-free / ISR-safe on the single-core rv32imc C3).
- **User offsets** (signed ms + signed ticks) for by-ear latency alignment:
  `+ms` samples Link time later → ticks emitted earlier. Live-tunable over the
  USB serial console (`serial_task`/`handle_command`), persisted in NVS
  (namespace `bridge`), with compiled fallbacks `DEFAULT_OFFSET_*` in config.h
  (currently +75 ms, tuned by ear against a real SMS). Changing an offset stays
  monotonic for free — the presenter only ever steps the wire forward, so
  lowering an offset just freezes until time catches up.

## Gotchas (learned the hard way)

- **Ableton "Start Stop Sync"** is a separate toggle from "Link". If it's off,
  `isPlaying()` is always false → the bridge sees `playing=0` and never launches.
- Opening the USB-Serial/JTAG port pulses DTR/RTS and **resets the C3**. Use one
  persistent `idf.py monitor` session for live tuning; one-shot opens reboot the
  board (which then reloads the NVS/compiled default).
- Console output needs `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` — the XIAO's only
  USB is the USB-Serial/JTAG peripheral, not a UART bridge.

## Toolchain (decided: ESP-IDF)

**ESP-IDF v5.5.** Arduino-ESP32 was attempted first but dead-ends: Ableton Link's
networking needs `espressif/asio` (an ESP32-patched asio with lwip shims),
distributed only as an IDF managed component — vanilla asio will not compile for
ESP32. IDF's component manager fetches it automatically. Install IDF once
(`~/esp/esp-idf`, `install.sh esp32c3`); each shell needs `. ~/esp/esp-idf/export.sh`.

Build/flash:

```
. ~/esp/esp-idf/export.sh            # per shell
git submodule update --init lib/link # Ableton Link (header-only)
cp main/secrets.h.example main/secrets.h   # then edit WiFi creds

idf.py set-target esp32c3            # once
idf.py build
idf.py -p /dev/cu.usbmodem* flash monitor
```

Integration specifics worth knowing (all in `main/CMakeLists.txt` /
`sdkconfig.defaults` / top of `main.cpp`):
- `espressif/asio` is pulled via `main/idf_component.yml`; we use it instead of
  Link's bundled asio (the submodule's `modules/asio-standalone` is unused —
  that's why `lib/link` is added non-recursively).
- `CONFIG_COMPILER_CXX_EXCEPTIONS=y` (Link needs exceptions) and
  `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y` (Link + asio + WiFi are large).
- `-fexceptions` + `LINK_ESP_TASK_CORE_ID` set on the main component.
- We define `__atomic_is_lock_free` ourselves: IDF only provides it for RISC-V
  *with* the atomic extension, but the C3 is rv32imc (none) — Link's TripleBuffer
  references it via an assert.

## Toolchain (decided: Arduino-ESP32)

**Arduino-ESP32** (community Ableton Link Arduino ports exist; faster to first
blink than ESP-IDF). ESP-IDF remains the fallback if timer/core placement needs
finer control than the Arduino layer allows. The Ableton **Link SDK** is checked
out at `~/Documents/ares-link-sync/link/` (with the `ableton/` headers) and can
be reused/submoduled.

No sketch scaffold exists yet. Once it does, the intended workflow is `arduino-cli`
for the ESP32-C3 target (verify/replace these once the sketch is in place):

```
# one-time: install the core
arduino-cli core install esp32:esp32

# compile (FQBN for Seeed XIAO ESP32-C3)
arduino-cli compile --fqbn esp32:esp32:XIAO_ESP32C3 .

# flash + open serial monitor (replace PORT, e.g. /dev/cu.usbserial-*)
arduino-cli upload --fqbn esp32:esp32:XIAO_ESP32C3 -p PORT .
arduino-cli monitor -p PORT -c baudrate=115200
```

## References (read these before writing code)

- **`~/Documents/ares-link-sync/ares/ares/ms/controller/link-sync/link-session.cpp`**
  — the reference Link→counter implementation (target/presenter/offsets/PLL).
  `link-sync.cpp` shows the pin mapping on the emulator side.
- **`~/Documents/sms_tracker/HARDWARE.md`** — SMS controller port 2 pinout +
  register map (the slave side: port `$DD` bits, the counter protocol).
- **`~/Documents/sms_tracker/DESIGN.md` §11** — the sync design (OUT/PULSE/IN).
- **`~/Documents/ares-link-sync/link/`** — the Ableton Link SDK.

## Invariants

- The counter **never runs backward** and applies **≤3 ticks per SMS frame**.
- The bridge only ever **pulls the wire low** (open-drain); never drives 5 V.
- 24 PPQN is fixed — it matches the tracker's groove-6 lock.
