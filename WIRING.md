# Wiring — XIAO ESP32-C3 → SMS controller port 2

Three wires total: two signal lines (open-drain) plus a shared ground. The board
is **USB-powered separately** — do **not** connect the SMS +5 V (port-2 pin 5).

## Connection table

| Counter bit | XIAO pad | XIAO GPIO | → | SMS port-2 pin | SMS signal |
|-------------|----------|-----------|---|----------------|------------|
| bit 0       | D1       | GPIO3     | → | pin 9          | TR         |
| bit 1       | D2       | GPIO4     | → | pin 7          | TH         |
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

D1 (GPIO3) and D2 (GPIO4) are plain, non-strapping GPIOs. **Avoid** the ESP32-C3
strapping pins for these outputs — D0/GPIO2, D8/GPIO8, and especially D9/GPIO9
(boot-mode select): driving them at boot can disrupt the flash/boot sequence.

## Power & ground

- Power the XIAO from its own USB supply.
- Share **only GND** (XIAO GND ↔ SMS pin 8). Leaving +5 V (pin 5) disconnected
  avoids WiFi current peaks sagging the console's 5 V rail.
