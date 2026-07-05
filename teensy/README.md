# Teensy 4.1 — wired USB-MIDI bridge

The **wired sibling** of the ESP32 firmware in [`../main`](../main): a **Teensy 4.1**
takes **USB-MIDI over USB-C from a DAW** and drives the *same two DE-9 controller-port
wires* the ESP32 does — so **SMSGGDJ** (SMS / Game Gear) and **genmddj** (Mega Drive)
follow it in their `SYNC: IN` mode. **No WiFi, no Ableton Link** — just a cable.

It implements the same wire contract as the rest of this repo:

- **MIDI clock → a rolling 2-bit tick counter** (24 PPQN = 1:1 with the sync rate).
  Start/Continue/Stop drive the transport; the counter never runs backward and applies
  ≤ 3 ticks per SMS frame.
- **MIDI notes → MIDI takeover** — a console-clocked serial link the tracker plays live
  on its own voices (sequencer stopped). Same frames as [`../MIDI.md`](../MIDI.md)
  (canonical spec: `genmddj/MIDI.md §3`).

The two modes are mutually exclusive on the two wires and auto-arbitrated (note traffic
→ takeover, else the counter), exactly like the ESP32-S3.

## Why a Teensy, and why in this repo

Teensy 4.1's built-in `usbMIDI` replaces the ESP32-S3's hand-rolled TinyUSB composite
descriptor, and there's no WiFi/Link coexistence to manage — so this is a *simplification*
of the S3's USB-MIDI path. It lives in this repo (not a separate one) because the value is
the **shared wire contract**: keeping this implementation next to `MIDI.md` and the ESP32
reference is what stops the two from drifting. See the repo `CLAUDE.md`.

## Files

```
teensy/smsggdj_bridge_teensy/
  smsggdj_bridge_teensy.ino   sketch: USB-MIDI in, presenter timer, takeover responder, serial console
  config.h                    pins + sync constants (mirror of ../../main/config.h)
  midi_proto.h                portable takeover frame layout + normalisation (mirror of MIDI.md 3)
```

## Build & flash

1. Install **Arduino IDE + Teensyduino**.
2. **Tools → Board → Teensy 4.1.**
3. **Tools → USB Type → "Serial + MIDI"** — *required*. `Serial` is the tuning console;
   `MIDI` is the clock/note input. (Plain "MIDI" works but drops the console.)
4. Open `smsggdj_bridge_teensy/smsggdj_bridge_teensy.ino`, **Upload**.

The board appears to your DAW as a USB-MIDI device named *Teensy MIDI*; send it clock
(Start/Stop/Continue + 0xF8) and/or notes.

## Wiring — ⚠️ a logic-level shifter is REQUIRED

**Do not wire a Teensy 4.x directly to the controller port.** This is the one real
difference from the ESP32, and it can destroy the board if you skip it:

- Teensy 4.x pins run at **3.3 V and are *not* 5 V tolerant** (absolute max ≈ 3.6 V).
- The console's controller lines **idle at 5 V** (internal pull-ups), and in takeover
  mode the console actively **drives CLK to 5 V**.
- So a bare Teensy pin on either line sees 5 V and **can be damaged**. (The ESP32
  marginally tolerates this on the same pads; the Teensy does not — don't copy the
  ESP32 wiring.)

### Use a BSS138 bidirectional level shifter

Get a **BSS138-based bidirectional level shifter** — e.g. Adafruit #757 or SparkFun
BOB-12009 (4 channels; you use 2). It's the correct part here because each channel is
**open-drain / passive-pull-up on both sides**, which is *exactly* the electrical model
the firmware and the console already use: a side either pulls the line **low** or
**releases** it (the pull-up brings it high). So it drops in without changing any of the
counter/takeover logic — it just isolates the Teensy's 3.3 V world from the port's 5 V.

**A resistor divider is NOT a substitute** — the CLK line is bidirectional-in-effect and
open-drain, and a divider can't pull up or pass an open-drain low cleanly. Use the BSS138
board.

Two rails set the shift ratio — get these right first:

- **LV = Teensy 3.3 V** → the board's `LV` (low-voltage reference).
- **HV = console 5 V** → the board's `HV`. Take 5 V from **DE-9 port-2 pin 5**, but use it
  **only** as the shifter's HV reference — **never** to power the Teensy.
- **GND** common to all three (Teensy, shifter, console).

Then one signal per channel:

```
   TEENSY (3.3 V)          BSS138 shifter            SEGA DE-9 port 2 (5 V)
   ─────────────           ───────────────           ──────────────────────
   3.3 V ───────────────►  LV        HV  ◄─────────── pin 5  (+5 V, HV ref only)
   GND ─────────────────►  GND      GND  ◄─────────── pin 8  (GND)
   PIN_TR (pin 2) ◄─────►  A1        B1  ◄─────────►  pin 9  (TR / bit0 / CLK)
   PIN_TH (pin 3) ◄─────►  A2        B2  ◄─────────►  pin 7  (TH / bit1 / DAT)
```

| Signal | Teensy pin | Shifter ch (LV↔HV) | DE-9 port-2 pin | Direction |
|---|---|---|---|---|
| **TR / bit0 / CLK** | `PIN_TR` = 2 | A1 ↔ B1 | 9 | counter: Teensy→port · takeover: **port→Teensy** |
| **TH / bit1 / DAT** | `PIN_TH` = 3 | A2 ↔ B2 | 7 | Teensy→port (open-drain) |
| **GND** | any GND | GND | 8 | shared ground |
| — | 3.3 V → LV | HV ← 5 V (pin 5) | 5 (ref only) | rails |

Power the Teensy from **its own USB** (the DAW cable). Share **only GND** (and the 5 V HV
*reference*) with the console — do **not** run the board off port pin 5. Pins are set in
[`config.h`](smsggdj_bridge_teensy/config.h); move `PIN_TR`/`PIN_TH` there if you prefer a
different pair (keep them adjacent — any two plain digital pins on the Teensy 4.1 work).

## Serial console

Open the Serial Monitor at 115200 baud:

| cmd | effect |
|---|---|
| `t <n>` | tick offset — whole 1/24-beat latency steps (persisted) |
| `p` | print current offset + takeover mode |
| `s` | save offset + takeover mode to EEPROM |
| `k <auto\|on\|off>` | MIDI-takeover mode (auto follows recent note traffic) |
| `h` | help |

There is **no `m` (ms) offset** — MIDI clock is reactive, with no Link timeline to sample
earlier against, so sub-tick nudging isn't possible (same limitation as the ESP32's MIDI
path; use `t` for whole-tick alignment). No `c` (source) or `w` (WiFi) — wired MIDI is the
only source.

## Status

**Coded, not yet hardware-verified.** The counter path is a direct port of the proven ESP32
presenter; the takeover responder mirrors the ESP32's (itself still bench-pending — see
`../MIDI.md §7`). First bring-up: confirm the DAW enumerates the board, that MIDI clock
advances a real console in `SYNC: IN`, and (with a level shifter) the takeover frame stream
on a logic analyser.
