# MIDI takeover — the bridge's cross-platform MIDI contract

Status: **concept.** This is the shared architecture for turning the bridge into a
**USB-MIDI → console** path that serves **both** trackers — **SMSGGDJ** (SMS / Game
Gear) and **genmddj** (Mega Drive) — from **one** firmware image. It reuses the same
two port wires the bridge already drives for Link / MIDI-clock tempo sync. Nothing is
implemented yet; this is the contract each side builds against.

Companion docs:
- This repo — the **one firmware** that implements the pump + responder below.
- SMSGGDJ tracker repo — its own `MIDI.md` (Z80 port I/O via `$DD`/`$3F`, engine reuse).
- genmddj tracker repo — its own `MIDI.md`, to be written (68000 port I/O, its voices).
- `WIRING.md` / `README.md` — the existing 2-bit sync counter this sits alongside.

## 1. The idea

A **MIDI takeover** mode on the tracker: the console's internal sequencer stops, and
its sound chips become a **live multi-part MIDI sound module** played from a DAW over
USB. It reuses the **existing 2-wire port link** that the bridge already drives for
Link / MIDI-clock **tempo sync on both consoles** — no new hardware.

- **MIDI channel → voice/track** (each console maps to its own voices).
- **Program change → instrument select; velocity → volume; note-off → envelope release.**
- Each note plays exactly as a **sequenced step** would on that console, so the voice's
  instrument decides its character (PSG tone / noise / FM / wave / PCM, per platform).

## 2. The one-firmware principle

**The bridge is a dumb, platform-agnostic MIDI→serial pump.** It has **zero**
knowledge of how many voices a console has or how channels map to them:

- Enumerate as a **USB-MIDI device** (needs the ESP32-**S3**; the C3 has no USB-OTG
  and stays Link-only).
- Forward **standard MIDI channel-voice messages** (note on/off, program change, CC,
  pitch bend — filter out clock/sysex/realtime it doesn't need) as their **raw
  status + data bytes** into a ring FIFO. **No per-console remapping, no voice-count
  assumptions.**
- A **console-clocked serial responder** shifts the FIFO out on demand.

**All platform knowledge lives on the console side.** SMSGGDJ and genmddj each map
the MIDI channels they care about to their own voices and trigger notes in their own
engines. That's what lets one firmware serve two consoles with different chips and
different voice counts. (It mirrors how the existing **2-bit sync counter** is already
console-agnostic — both consoles read the same counter the same way.)

## 3. Shared wire protocol

Same two port lines as the sync bridge, **repurposed by direction** in MIDI mode:

- **CLK** — the *console* drives it (console is clock master).
- **DAT** — the *bridge* drives it open-drain; the console reads it.

Console-clocked → **nothing is ever missed or duplicated**, whatever the console's
polling timing. The console drains the FIFO each frame.

**Byte stream = raw MIDI voice messages**, self-framing exactly like MIDI:
- **status byte** has bit 7 **set** (`0x8n/0x9n/0xBn/0xCn/0xEn`), low nibble = **MIDI
  channel 0–15**;
- **data byte** has bit 7 **clear** (0–127);
- **`0x00`** = FIFO-empty sentinel; **DAT-while-CLK-idle** = events-pending flag.

**Key cross-platform point: the low nibble carries the MIDI channel (0–15), not a
narrowed track index.** Each console filters/maps:
- **SMSGGDJ:** channels 1–4 → T1 / T2 / T3 / N; drop 5–16.
- **genmddj:** channels 1–N → its YM2612 FM (×6) + SN76489 PSG (×4) voices.

## 4. Per-console layer (each tracker implements)

1. **Port I/O for MIDI mode** — set the two lines to CLK-in-from-console / DAT-in, and
   bit-bang the console-clocked receive.
   - SMSGGDJ: Z80, `$DD` read / `$3F` direction; TR = CLK, TH = DAT.
   - genmddj: 68000, controller **data**/**control** registers (`$A10003…`, `$A10009…`)
     — the same TR/TH lines the sync already uses.
2. **Mode entry/exit** — stop the sequencer, lock transport, init/teardown, silence.
3. **Channel → voice map** + per-voice MIDI state (current instrument, active note).
4. **Note → pitch** (platform note tables), **velocity → volume**, **PC → instrument**.
5. **Trigger / release** reusing the existing note-trigger and note-release/envelope
   paths — and, while the transport is stopped, the existing envelope/render pass
   draws it (true on SMSGGDJ; genmddj should have an equivalent stopped-render path).

## 5. Platform differences to account for

| | SMSGGDJ (SMS/GG) | genmddj (Mega Drive) |
|---|---|---|
| CPU | Z80 | 68000 |
| Voices | 3 tone + noise (SN76489), +opt YM2413 FM | YM2612 (6 FM) + SN76489 (3 tone + noise) |
| Usable MIDI ch | 1–4 | up to ~1–10 |
| Port access | `$DD` / `$3F` | `$A100xx` data/control regs |
| Note tables / instruments | per platform | per platform |

Both are the **clock master** in MIDI mode, so each tolerates its own polling cadence.
Export-console / port-electrical caveats (driving outputs) apply per platform as they
already do for `SYNC: OUT`.

## 6. Bridge firmware work (one, shared — this repo)

- Extend USB-MIDI RX to capture note/PC/CC/pitch-bend on **all 16 channels** → ring
  FIFO of **raw MIDI bytes** (no remap).
- **Console-clocked TX responder** — CLK-edge ISR shifts the next DAT bit; hold DAT as
  the pending flag while idle. Open-drain, same electrical contract as the counter.
- **Mode arbitration** — note traffic → serial responder (drive DAT only, let the
  console own CLK); Link / MIDI-clock → the existing 2-bit counter presenter (drive
  both). Mutually exclusive on the 2 wires; identical for both consoles.
- **S3-only** (USB-OTG). One S3 firmware image drives either console.

## 7. Milestones

1. **Lock the shared protocol** — channel-preserving byte stream; each tracker's
   `MIDI.md` masks the low nibble to its channel range.
2. **One firmware** — RX→FIFO + console-clocked responder (platform-agnostic), bench-
   verified over the CDC console + a logic analyser (drive CLK from a second MCU).
3. **SMSGGDJ MIDI takeover** — per the SMSGGDJ `MIDI.md`.
4. **genmddj MIDI takeover** — mirror it with 68000 port I/O + the MD voice map; write
   the genmddj `MIDI.md`.
5. **Hardware bring-up** on both consoles + the S3.

## 8. Open questions

- genmddj's exact voice/track model and how many MIDI channels it exposes.
- A **shared channel convention** so a DAW setup ports between consoles — e.g. ch 1–4
  always the SN76489 PSG (works on both), MD adding ch 5–10 for YM2612 FM.
- CC / pitch-bend mapping per platform (volume trim, pitch bend → the `P`-command feel).
- **Clock-in-stream:** fold 24-PPQN clock into the same serial stream as note events, so
  one link carries **tempo *and* notes** — a path to MIDI-driven playback (not just
  takeover) later, again shared by both consoles.
