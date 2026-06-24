# Wiring — XIAO ESP32 → SMS / Mega Drive controller port 2

Three wires total: two signal lines (open-drain) plus a shared ground. The board
is **USB-powered separately** — do **not** connect the console +5 V (port-2 pin 5).
The same three wires also clock a **Mega Drive** running genmddj — see the Mega
Drive section below.

The signal lines map to different GPIO numbers (and pads) on the **C3** vs the
**S3** — pick the table for your board. The firmware's `config.h` selects the
right pins automatically by build target.

## Connection table — XIAO ESP32-C3 (Link over WiFi)

| Counter bit | XIAO pad | XIAO GPIO | → | SMS port-2 pin | SMS signal |
|-------------|----------|-----------|---|----------------|------------|
| bit 0       | D1       | GPIO3     | → | pin 9          | TR         |
| bit 1       | D2       | GPIO4     | → | pin 7          | TH         |
| (ground)    | GND      | —         | → | pin 8          | GND        |

## Connection table — XIAO ESP32-S3 (Link + USB-MIDI)

The S3's pad **D2 is GPIO3, which is a strapping pin**, so the S3 shifts the two
signal lines one pad over to **D3/D4** (GPIO4/GPIO5 — adjacent, non-strapping):

| Counter bit | XIAO pad | XIAO GPIO | → | SMS port-2 pin | SMS signal |
|-------------|----------|-----------|---|----------------|------------|
| bit 0       | D3       | GPIO4     | → | pin 9          | TR         |
| bit 1       | D4       | GPIO5     | → | pin 7          | TH         |
| (ground)    | GND      | —         | → | pin 8          | GND        |

The bridge presents a rolling 2-bit counter `presented & 3`: TR = bit 0, TH =
bit 1. Both signal lines are **open-drain** (`pinMode(pin, OUTPUT_OPEN_DRAIN)`):
the ESP32 only pulls low; the SMS's internal pull-ups supply the 5 V high. No
level shifter.

## SMS controller port 2 — DE-9 (console side)

A standard DE-9. Only **TR and TH are bidirectional**, which is why the protocol
is a 2-bit counter. (From `~/Documents/sms_tracker/HARDWARE.md`.)

| pin | signal | sync use                         |
|-----|--------|----------------------------------|
| 1   | Up     | — (input only)                   |
| 2   | Down   | — (input only)                   |
| 3   | Left   | — (input only)                   |
| 4   | Right  | — (input only)                   |
| 5   | +5V    | **do not connect**               |
| 6   | TL     | leave unconnected (floats high)  |
| **7** | **TH** | **counter bit 1**              |
| 8   | GND    | shared ground                    |
| **9** | **TR** | **counter bit 0**              |

This is the straight SMS↔SMS 3-wire scenario (pins 9/7/8). The tracker reads bit
0 as `TR AND TL`; with TL left floating-high, TR alone carries bit 0. All lines
are pulled up, so an idle/unplugged port reads high (counter = 3) — `SYNC IN`
latches the line state when armed and counts only *changes*, so a clean start
requires the bridge to be presenting a stable count before the tracker arms.

## Mega Drive / Genesis — genmddj

> ✅ **Confirmed working on Mega Drive hardware (2026-06-25)** — the bridge clocks
> genmddj over controller port 2 with no MD-side changes.

The **same bridge, same wiring, no firmware change** also clocks **genmddj** (the
Mega Drive port of SMSGGDJ). The MD shares the DE-9 controller-port family, and
genmddj's `SYNC: IN` reads the identical 2-bit counter — `$A10005` bits 5/6, i.e.
**TR (pin 9) = bit 0, TH (pin 7) = bit 1**, at 24 PPQN. Wire it exactly as the
tables above, into MD **controller port 2** (a normal pad in port 1 for editing).
In genmddj: **OPTIONS → SYNC: IN**, press Start (shows `WAIT`), then start the
Link/MIDI transport.

Two MD-specific notes:

- **TL (pin 6) is unused here.** The SMS reads bit 0 as `TR AND TL`; genmddj reads
  **TR alone** (`$A10005` bit 5), so pin 6 just stays floating (as above).
- **Check the TH (pin 7) pull-up.** ⚠ The open-drain bridge leans on the console's
  pull-ups for the high level. An idle MD port reads `$7F` (pull-ups present), so
  **TR (pin 9)** is fine — but **TH (pin 7)** is normally the MD's *select output*,
  and genmddj reconfigures it as an input (`$A1000B = 0`). If its input-mode pull-up
  is weak, counter bit 1 can read flaky — symptom: genmddj won't advance, or runs
  erratically / double-time. Fix: a **10 kΩ pull-up on pin 7** to a logic-high rail —
  the XIAO **3V3** pad (3.3 V is a valid MD logic high) or the console **+5 V (pin
  5)**. A resistor pull-up draws microamps, so this does *not* break the "don't power
  from +5 V" rule above (that rule is about the board's WiFi-peak supply current).
  One on pin 9 won't hurt either.

Counted-not-timed, so PAL/NTSC region doesn't matter — genmddj's slave locks to flat
groove-6 (24 PPQN) to match the wire. The same 3-wire cable also cross-syncs a Mega
Drive directly to an SMS / Game Gear — genmddj ↔ SMSGGDJ.

## XIAO ESP32-C3 pad layout (for orientation)

USB connector at the top. D1/D2 are adjacent on the **left** rail; GND is on the
**right** rail (so the ground wire crosses to the other side — they are not all
on one edge).

```
            ┌──────[USB-C]──────┐
   D0/GPIO2 │ 1               14 │ 5V
   D1/GPIO3 │ 2  ← TR (bit0)  13 │ GND  → SMS pin 8
   D2/GPIO4 │ 3  ← TH (bit1)  12 │ 3V3
   D3/GPIO5 │ 4               11 │ D10/GPIO10
   D4/GPIO6 │ 5               10 │ D9/GPIO9
   D5/GPIO7 │ 6                9 │ D8/GPIO8
  D6/GPIO21 │ 7                8 │ D7/GPIO20
            └───────────────────┘
```

### Why these pins

**C3:** D1 (GPIO3) and D2 (GPIO4) are plain, non-strapping GPIOs. **Avoid** the
ESP32-C3 strapping pins for these outputs — D0/GPIO2, D8/GPIO8, and especially
D9/GPIO9 (boot-mode select): driving them at boot can disrupt the flash/boot
sequence.

**S3:** the strapping pins to avoid are **GPIO0, GPIO3, GPIO45, GPIO46**. On the
XIAO S3 that rules out pad D2 (= GPIO3), so the bridge uses D3 (GPIO4) and D4
(GPIO5) — the next two adjacent, safe pads. The USB-C connector carries the
USB-OTG peripheral (used for the USB-MIDI port + serial console); don't repurpose
the USB data lines.

## Power & ground

- Power the XIAO from its own USB supply.
- Share **only GND** (XIAO GND ↔ SMS pin 8). Leaving +5 V (pin 5) disconnected
  avoids WiFi current peaks sagging the console's 5 V rail.
