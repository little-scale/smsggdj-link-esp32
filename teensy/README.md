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

## Wiring — ⚠️ Teensy 4.x is NOT 5 V tolerant

This is the one real difference from the ESP32. The console's controller lines idle at
**5 V** (internal pull-ups), and in takeover the console **drives CLK to 5 V**. A Teensy 4.x
pin (Vmax ≈ 3.6 V) exposed to 5 V **can be damaged** — the ESP32 marginally survives this,
the Teensy will not.

**Put a level shifter on both lines.** A **BSS138 bidirectional level shifter** (Adafruit
#757 / SparkFun BOB-12009, or any BSS138 4-channel board) is the drop-in: it's open-drain
on both sides, so it preserves the exact open-drain semantics the firmware assumes (release
= pulled to the port's 5 V; drive = low) while clamping the Teensy side to 3.3 V. Wire the
Teensy 3.3 V to the shifter's LV rail and the console's 5 V (port pin 5 — used *only* as the
shifter's HV reference, not to power the Teensy) to HV.

| Signal | Teensy pin | Shifter | DE-9 port 2 | Direction |
|---|---|---|---|---|
| **TR / bit0 / CLK** | `PIN_TR` (D2) | ch A | pin 9 | counter: out · takeover: **in** |
| **TH / bit1 / DAT** | `PIN_TH` (D3) | ch B | pin 7 | out (open-drain) |
| **GND** | any GND | — | pin 8 | shared ground |

Power the Teensy from **its own USB** (the DAW cable). Share **only GND** with the console;
do **not** tap port pin 5 to power the board. Pins are set in `config.h` — move `PIN_TR`/
`PIN_TH` there if you prefer a different pair (keep them adjacent, and both are fine on
Teensy 4.1's plain digital pins).

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
