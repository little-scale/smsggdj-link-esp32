// midi_proto.h -- the portable MIDI-takeover wire contract.
//
// Pure logic, no hardware/platform APIs: the compact-frame layout, the type enum,
// and the raw-MIDI -> compact-frame normalisation. It mirrors the ESP32
// implementation in ../../main/main.cpp and the spec in ../../MIDI.md 3 (canonical
// source: genmddj/MIDI.md 3). Both firmwares MUST agree here bit-for-bit -- the
// console-side parser is built to exactly this. Keep this header and the ESP32
// code in lockstep with MIDI.md.
//
// Frame (clocked MSB-first over one wire, console drives the clock):
//   after an idle gap the bridge presents a leading FLAG bit --
//     flag = 1  -> a fixed 3-byte event follows: [status][d1][d2]   (25 bits)
//     flag = 0  -> queue empty                                       (1 bit)
//   status = type<<4 | channel   (channel = MIDI 0..15)
#pragma once
#include <stdint.h>

// Event types (low nibble of status carries the MIDI channel).
enum MidiEvtType {
  EVT_NOTE_OFF = 1,
  EVT_NOTE_ON  = 2,
  EVT_CC       = 3,
  EVT_PGM      = 4,
  EVT_BEND     = 5,
  EVT_PANIC    = 7,
};

struct MidiEvt { uint8_t status, d1, d2; };

// Total bits an event occupies on the wire: 1 flag + 24 payload.
static const int MIDI_FRAME_BITS = 25;

// Pack an event into a left-justified 32-bit shift word: the flag sits in bit 31,
// then status:d1:d2, MSB-first. (Empty-queue flag=0 is a separate 1-bit case the
// caller handles.) Identical to load_frame_locked() in ../../main/main.cpp.
static inline uint32_t midi_frame_word(const MidiEvt& e) {
  const uint32_t f = (1u << 24) | ((uint32_t)e.status << 16) |
                     ((uint32_t)e.d1 << 8) | e.d2;
  return f << (32 - MIDI_FRAME_BITS);   // flag -> bit 31
}

// Normalise a raw channel-voice message into a compact event. Returns false if the
// message should be dropped (poly aftertouch 0xA0 / channel pressure 0xD0). This is
// the exact cleanup forward_channel_voice() does on the ESP32:
//   NoteOn vel 0 -> NoteOff ; CC 120/123 (all-sound/all-notes-off) -> Panic ;
//   running status is already expanded by USB-MIDI. `critical` marks a NoteOff/panic
//   the queue must never drop (evict the oldest to fit instead).
//   statusHi = status & 0xF0 ; ch = 0..15 ; d1/d2 = raw data bytes.
static inline bool midi_normalise(uint8_t statusHi, uint8_t ch,
                                  uint8_t d1, uint8_t d2,
                                  MidiEvt& out, bool& critical) {
  uint8_t type;
  critical = false;
  switch (statusHi) {
    case 0x80:                                   // NoteOff
      type = EVT_NOTE_OFF; critical = true; break;
    case 0x90:                                   // NoteOn (vel 0 -> NoteOff)
      if (d2 == 0) { type = EVT_NOTE_OFF; critical = true; }
      else         { type = EVT_NOTE_ON; }
      break;
    case 0xB0:                                   // CC (120/123 -> Panic)
      if (d1 == 120 || d1 == 123) { type = EVT_PANIC; d1 = 0; d2 = 0; }
      else                        { type = EVT_CC; }
      break;
    case 0xC0:                                   // Program Change
      type = EVT_PGM; d2 = 0; break;
    case 0xE0:                                   // Pitch Bend (d1 = LSB, d2 = MSB)
      type = EVT_BEND; break;
    default:                                     // drop 0xA0 / 0xD0
      return false;
  }
  out.status = (uint8_t)((type << 4) | (ch & 0x0F));
  out.d1 = d1;
  out.d2 = d2;
  return true;
}
