# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32 firmware: an **Ableton Link → SMS hardware-sync bridge**. The board joins
an Ableton Link session over WiFi, derives a tick clock from the shared beat
timeline, and drives a **2-bit tick counter** onto two GPIOs into a Sega Master
System controller port, so the **SMSGGDJ** tracker (`~/Documents/sms_tracker`,
its `SYNC: IN` mode) follows Ableton Live's tempo and transport.

Status: **new, no code yet.** This file is the spec + decisions; build it out.

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
- **Output: open-drain** on the two signal GPIOs (`pinMode(pin,
  OUTPUT_OPEN_DRAIN)` under Arduino-ESP32). The ESP32 (3.3 V) only pulls the
  lines **low**; the SMS's internal pull-ups supply the 5 V high. No level
  shifter needed.
- **Power: USB (own supply); share only GND with port 2.** Do **not** tap the
  port's +5 V (pin 5) — WiFi current peaks risk sagging the console's 5 V rail.
- Tick edges should come from a **hardware timer ISR** (toggling the two GPIOs),
  independent of the WiFi/main loop.

## Architecture (planned)

Port the proven logic from the ares emulator fork (see References):
- Join Link (`ableton::Link`), `enableStartStopSync(true)`, `enable(true)`.
- Per scheduling instant, `target = floor(beatsSinceLaunch × 24) (+ offsets)`.
- A **monotonic presenter**: advance `presented` toward `target`, never
  backward, ≤ 3 per step; output `presented & 3` on TR/TH.
- `beatsSinceLaunch` uses the captured session state + `phaseAtTime` to find the
  bar line; negative = armed (pre-bar).
- Optional **user offsets** (ms latency + ticks phase), persisted; increases
  feed in ≤3/step, decreases freeze until time catches up (keeps it monotonic).

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
