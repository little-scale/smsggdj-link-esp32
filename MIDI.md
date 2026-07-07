# MIDI takeover — the bridge's cross-platform MIDI contract

Status: **concept.** This is the shared architecture for turning the bridge into a
**USB-MIDI → console** path that serves **both** trackers — **SMSGGDJ** (SMS / Game
Gear) and **genmddj** (Mega Drive) — from **one** firmware image. It reuses the same
two port wires the bridge already drives for Link / MIDI-clock tempo sync. Nothing is
implemented yet; this is the contract each side builds against.

Companion docs:
- This repo — the **one firmware** that implements the pump + responder below.
- SMSGGDJ tracker repo — its own `MIDI.md` (Z80 port I/O via `$DD`/`$3F`, engine reuse).
- genmddj tracker repo — its own `MIDI.md` (**the canonical wire spec, §3**) + its
  already-built console side (`midi_dispatch` etc.); only `midi_poll` (the shift-in) is left.
- `WIRING.md` / `README.md` — the existing 2-bit sync counter this sits alongside.

## 1. The idea

A **MIDI takeover** mode on the tracker: the console's internal sequencer stops, and
its sound chips become a **live multi-part MIDI sound module** played from a DAW over
USB. It reuses the **existing 2-wire port link** that the bridge already drives for
Link / MIDI-clock **tempo sync on both consoles** — no new hardware.

- **MIDI channel → voice/track** (each console maps to its own voices).
- **One channel = one monophonic voice** — no polyphony, no voice-stealing; a new note
  on a channel replaces that channel's current note. Matches each tracker's
  mono-per-track model and keeps the console side trivial.
- **Program change → instrument select; velocity → volume; note-off → envelope release.**
- Each note plays exactly as a **sequenced step** would on that console, so the voice's
  instrument decides its character (PSG tone / noise / FM / wave / PCM, per platform).

## 2. The one-firmware principle

**The bridge is a platform-agnostic MIDI→compact-frame *normaliser*.** It knows nothing
about how many voices a console has or how channels map to them — it just cleans MIDI up
into a uniform stream both consoles read the same way:

- Enumerate as a **USB-MIDI device** (needs the ESP32-**S3**; the C3 has no USB-OTG
  and stays Link-only).
- **Normalise** channel-voice messages (note on/off, PC, CC, pitch bend; filter out
  clock/sysex/realtime) into fixed **compact `type<<4|channel` + 3-byte frames** (§3):
  running status expanded, NoteOn-vel0 → NoteOff, all-notes-off CC → Panic, and **CC
  coalesced** (latest value per controller, so a knob sweep can't flood the wire).
- A **console-clocked responder** presents those frames bit-by-bit on demand (§3).

**The compact frames are console-agnostic**, so it's still one firmware: the `type` and
channel are generic; only the *meaning* of a channel or a PC number is console-side. That
normalisation is the deliberate division of labour — it keeps each console's parser a
small bounded type-switch (genmddj's is already built that way) and moves MIDI's messiness
(running status, CC floods, vel-0 note-offs) to the one place with cycles to spare.
(It mirrors how the **2-bit sync counter** is already console-agnostic — both consoles read
the same wire the same way.)

## 3. Shared wire protocol

**Canonical spec: `genmddj/MIDI.md` §3** — genmddj's console side is already built to it,
and the S3 responder + both consoles implement it. This section is the summary; that doc
is the source of truth for any detail.

Two wires + ground, **console is the clock master**, one-directional data:

- **CLK** — the *console* drives it (idle **low**; pulses low→high→low, samples DAT on the
  **rising** edge). On the DE-9 this is **TR (pin 9)**.
- **DAT** — the *bridge* drives it open-drain and changes it on the **falling** edge
  (stable before the next rising sample), **MSB-first**. This is **TH (pin 7)**.

Keeping to **exactly two wires** is deliberate hardware-minimisation; it's fast enough
because takeover is monophonic-per-channel and carries no concurrent clock.

**Framing — flag bit + fixed 3-byte frame, idle-gap resync:**
- After an **idle gap** (CLK quiet) the bridge presents a leading **flag bit** while idle.
  Every console frame's burst is preceded by such a gap, so each burst **re-aligns from
  scratch** — a glitch can't desync for more than one frame.
- `flag = 1` → a fixed **3-byte event frame** follows; `flag = 0` → queue empty, stop.

```
per event slot (clocked, MSB first):
   [1]  flag=1  -> [status=type<<4|chan] [data1] [data2]     ; 25 bits total
   ...repeat back-to-back within the burst...
   [0]  flag=0  -> queue empty
```

**`status = type<<4 | channel`** (channel = MIDI 0–15). `type`: 1 NoteOff · 2 NoteOn ·
3 CC · 4 PgmChange · 5 PitchBend · 7 Panic. The bridge does the MIDI cleanup so the console
parser is a bounded type-switch: **NoteOn vel 0 → NoteOff**, **CC 120/123 → Panic**, no
running status, **CC coalesced** to the latest value per controller.

**Key cross-platform point: the low nibble carries the MIDI channel (0–15), not a
narrowed track index.** Each console filters/maps:
- **SMSGGDJ:** channels 1–4 → T1 / T2 / T3 / N; drop 5–16.
- **genmddj:** channels 1–10 → F1–F6 (FM) / T1–T3 (square) / NO (noise).

## 4. Per-console layer (each tracker implements)

1. **Port I/O for MIDI mode** — set the two lines to CLK-in-from-console / DAT-in, and
   bit-bang the console-clocked receive.
   - SMSGGDJ: Z80, `$DD` read / `$3F` direction; TR = CLK, TH = DAT.
   - genmddj: 68000, controller **data**/**control** registers (`$A10003…`, `$A10009…`)
     — the same TR/TH lines the sync already uses.
2. **Mode entry/exit** — stop the sequencer, lock transport, init/teardown, silence.
   The sequencer is **fully stopped** — no tracker playback during takeover — so there's
   no clock to interleave and timing stays simple. Consequence: tempo-synced effects
   (LFO-to-tempo, arps) **free-run** rather than lock to the DAW. On exit, drain the FIFO
   and kill any held notes (handle CC 123 all-notes-off / panic too).
3. **Channel → voice map** + per-voice MIDI state (current instrument, active note).
4. **Note → pitch** (platform note tables), **velocity → volume**, **PC → instrument**.
5. **Trigger / release** reusing the existing note-trigger and note-release/envelope
   paths — and, while the transport is stopped, the existing envelope/render pass
   draws it (true on SMSGGDJ; on genmddj the **INSTR note-audition** path already
   triggers + renders a note with the sequencer stopped — generalise that trigger and
   the per-tick envelope/LFO/table/SCB pass to run from the MIDI source).

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

- Capture note/PC/CC/pitch-bend on **all 16 channels** → **normalise** to compact
  `type<<4|chan` + 3-byte frames (NoteOn-vel0 → NoteOff, CC 120/123 → Panic), **coalesce
  CC**, into an event ring.
- **Console-clocked responder** — a CLK falling-edge ISR presents the next DAT bit; a
  short idle-gap watchdog presents the leading flag bit before each burst. Open-drain,
  same electrical contract as the counter.
  - ⚠ The idle-gap watchdog must **only (re)present a fresh flag when nothing is loaded**
    (`srBits == 0`). `load_frame_locked()` pops the event from the queue as it loads it,
    so if the console missed the poll that loaded a frame, an unconditional resync
    (`srBits = 0` then reload) would discard that frame and reload from an empty queue —
    silently losing **isolated** note-ons/offs (bursts survive because the queue holds
    backups). This was the 2026-07-07 "need overlapping notes to hear anything" bug.
- **Mode arbitration** — note traffic → responder (drive DAT only, console owns CLK);
  Link / MIDI-clock → the existing 2-bit counter presenter (drive both). Mutually
  exclusive on the 2 wires; identical for both consoles. Takeover never needs both at
  once (it stops tracker playback), so the exclusivity costs nothing.
- **S3-only** (USB-OTG). One S3 firmware image drives either console.

## 7. Milestones

1. **Lock the shared protocol** — ✅ compact `type<<4|chan` flag-framed 3-byte frames with
   idle-gap resync, defined canonically in `genmddj/MIDI.md` §3; each console maps the
   channel to its own voices.
2. **One firmware** — normalise→event-ring + console-clocked responder. **Built (S3):**
   `forward_channel_voice` normalises + coalesces into `g_evtq`; `present_bit_locked` +
   `clk_isr` shift the flag-framed frames; `takeover_idle_check` (off the presenter timer)
   does the idle-gap resync; `wire_set_mode` arbitrates (channel-voice → takeover, else the
   Link/MIDI-clock counter). Serial `k <auto|on|off>`; HUD reports evtq depth / CLK edges /
   drops. **Bench-pending:** drive CLK from a second MCU + logic analyser to confirm the
   frame stream, then check the CLK-input electrical (5 V on an ESP32 input).
3. **SMSGGDJ MIDI takeover** — per the SMSGGDJ `MIDI.md` (to be written).
4. **genmddj MIDI takeover** — its console side is **already built** (`midi_dispatch` +
   note/CC/PC/bend/panic, takeover `engine_tick` branch, mode entry/exit; `genmddj/MIDI.md`).
   The remaining piece is `midi_poll` — the 2-wire shift-in that clocks these frames in and
   feeds `midi_dispatch`.
5. **Hardware bring-up** on both consoles + the S3.

## 8. Settled decisions (2026-07-05)

Load-bearing choices that later work builds against — don't re-litigate without changing
them deliberately here:

- **Monophonic per channel.** No polyphony, no voice-stealing (§1). A DAW addresses each
  console voice as its own MIDI channel.
- **Exactly two wires, 1-bit serial** (§3). No wider parallel transfer — hardware stays
  minimal; monophonic + clockless takeover keeps it fast enough.
- **Pure takeover: no concurrent clock.** The sequencer stops entirely; the wires carry
  only note data, and tempo-synced effects free-run (§2, §4.2). This is what makes the
  timing simple.
- **≤ 128 instruments**, so **Program Change 0–127 maps 1:1** to the instrument pool — no
  bank-select / NRPN needed.

## 9. Open questions

- genmddj's exact voice/track model and how many MIDI channels it exposes.
- A **shared channel convention** so a DAW setup ports between consoles — e.g. ch 1–4
  always the SN76489 PSG (works on both), MD adding ch 5–10 for YM2612 FM.
- CC / pitch-bend mapping per platform (volume trim, pitch bend → the `P`-command feel).
- **Clock-in-stream — not pursued.** Takeover stops tracker playback, so the wires never
  carry tempo and notes together; tempo-synced effects free-run. A future MIDI-*playback*
  mode (not just takeover) could revisit folding 24-PPQN clock into the stream, but that
  is explicitly out of scope here.
