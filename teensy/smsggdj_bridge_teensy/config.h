// config.h -- pins and sync constants for the Teensy 4.1 USB-MIDI -> SMS/MD bridge.
//
// This is the *wired* sibling of the ESP32 firmware in ../../main: no WiFi, no
// Ableton Link. A DAW sends USB-MIDI clock + notes over USB-C; the Teensy drives
// the same two controller-port wires the ESP32 does, so SMSGGDJ (SMS/GG) and
// genmddj (Mega Drive) follow it identically. The wire contract (2-bit counter +
// the console-clocked MIDI-takeover frames) is the same as ../../MIDI.md and
// ../../main/config.h -- keep them in sync.
#pragma once

// --- Pin map (Teensy 4.1) -----------------------------------------------------
// Two adjacent digital pins into DE-9 controller *port 2*. Roles match the ESP32:
//   counter mode : both are open-drain OUTPUTS  (TR = bit0, TH = bit1)
//   takeover mode: CLK is an INPUT (console-driven), DAT is an open-drain OUTPUT
//
// !!! HARDWARE WARNING -- Teensy 4.x pins are NOT 5 V tolerant (Vmax ~3.6 V) !!!
// The console's controller lines idle at 5 V via its internal pull-ups, and in
// takeover the console *drives* CLK to 5 V. Unlike the ESP32 (which marginally
// survives 5 V on these pads), a Teensy 4.x pin exposed to 5 V can be damaged.
// Put a BSS138 bidirectional level shifter (e.g. Adafruit/SparkFun 4-ch) on both
// lines: it is open-drain on both sides so it preserves the exact semantics below
// (release = pulled to the port's 5 V; drive = low) while clamping the Teensy
// side to 3.3 V. See ../README.md.
#define PIN_TR   2   // -> port-2 pin 9 (TR) = counter bit 0 ; also MIDI CLK (input)
#define PIN_TH   3   // -> port-2 pin 7 (TH) = counter bit 1 ; also MIDI DAT (output)
// GND: any Teensy GND pad -> port-2 pin 8. Do NOT connect port-2 +5 V (pin 5).

#define PIN_MIDI_CLK  PIN_TR   // console -> bridge : clock master, idle low, rising-edge sample
#define PIN_MIDI_DAT  PIN_TH   // bridge  -> console : data, open-drain, MSB-first, changes on falling edge

// --- Sync constants (identical to ../../main/config.h) ------------------------
#define PPQN                 24     // ticks per beat (fixed; matches tracker groove-6 lock)
#define MAX_TICKS_PER_FRAME  3      // 2-bit delta saturates at 3 per SMS frame read
#define SMS_FRAME_HZ         60     // NTSC; the console polls port 2 ~once per video frame
#define PRESENTER_HZ         1000   // how often the timer advances the counter / writes pins

// --- MIDI-clock source --------------------------------------------------------
// MIDI clock is 24 PPQN -- identical to the sync rate -- so each 0xF8 advances the
// counter by exactly one tick. Start/Continue/Stop drive the transport.
#define MIDI_ACTIVE_TIMEOUT_US 500000  // no clock for this long -> "not playing" idle

// --- MIDI takeover ------------------------------------------------------------
// The two wires stop being the counter and become a console-clocked serial link
// streaming live MIDI notes; the tracker plays them on its own voices with its
// sequencer stopped. Protocol = ../../MIDI.md 3 (canonical: genmddj/MIDI.md 3).
#define TAKEOVER_TIMEOUT_US  500000  // no channel-voice msg for this long -> back to counter (AUTO)
#define TAKEOVER_IDLE_US     1500    // CLK quiet this long -> idle gap: present a fresh flag bit
#define MIDI_EVTQ_SIZE       32      // pending compact events awaiting the console (power of two)

// --- Alignment offset ---------------------------------------------------------
// Whole 1/24-beat steps of latency compensation (tempo-dependent). Live-tunable
// over Serial (`t <n>`), persisted in EEPROM. There is no *ms* offset here: MIDI
// clock is reactive and there is no Link timeline to sample earlier against, so
// sub-tick nudging isn't possible (documented, not a bug -- same as the ESP32's
// MIDI path). Bake a default here; 0 unless a real console shows a fixed skew.
#define DEFAULT_OFFSET_TICKS 0
