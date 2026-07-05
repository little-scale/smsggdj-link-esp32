// config.h — board pins and sync constants for the Link->SMS bridge.
// See WIRING.md for the full pinout and CLAUDE.md for the wire contract.
#pragma once

// --- Pin map ---
// Open-drain outputs into SMS controller port 2. The ESP32 only pulls low;
// the SMS's internal pull-ups supply the 5 V high. The XIAO silkscreen pads
// (D1/D3/...) map to different GPIO numbers on the C3 vs the S3, so the pin map
// is target-conditional. Ground: XIAO GND pad -> port-2 pin 8. Do NOT connect
// port-2 +5 V (pin 5). Full tables in WIRING.md.
#if defined(CONFIG_IDF_TARGET_ESP32S3)
// Seeed XIAO ESP32-S3: D3=GPIO4, D4=GPIO5 (adjacent, both non-strapping). We
// avoid the S3 strapping pins GPIO0/3/45/46 -- note D2=GPIO3 is strapping, so
// the S3 shifts the lines one pad over to D3/D4 vs the C3's D1/D2.
#define PIN_TR   GPIO_NUM_4   // XIAO S3 D3 -> port-2 pin 9 (TR) = counter bit 0
#define PIN_TH   GPIO_NUM_5   // XIAO S3 D4 -> port-2 pin 7 (TH) = counter bit 1
#else
// Seeed XIAO ESP32-C3: D1=GPIO3, D2=GPIO4 (adjacent, non-strapping).
#define PIN_TR   GPIO_NUM_3   // XIAO C3 D1 -> port-2 pin 9 (TR) = counter bit 0
#define PIN_TH   GPIO_NUM_4   // XIAO C3 D2 -> port-2 pin 7 (TH) = counter bit 1
#endif

// --- Sync constants ---
#define PPQN            24.0   // ticks per beat (fixed; matches tracker groove-6 lock)
#define LINK_QUANTUM    4.0    // beats per bar (4/4); bar-aligned launch on phase 0
#define MAX_TICKS_PER_FRAME 3  // 2-bit delta saturates at 3 per SMS frame read
#define SMS_FRAME_HZ    60     // NTSC; the SMS polls port 2 ~once per video frame

// --- Presenter timer ---
#define PRESENTER_HZ    1000   // how often the timer advances the counter / writes pins

// --- WiFi provisioning (SoftAP captive portal) ---
// Where the portal stores the chosen network (separate from the "bridge" NVS
// namespace used for offsets). Cleared by the serial `w` command.
#define WIFI_NVS_NS         "wifi"
#define AP_SSID_PREFIX      "SMSGGDJ-setup"  // a -XXXX MAC suffix is appended
#define AP_CHANNEL          1
#define STA_CONNECT_TIMEOUT_MS 20000  // give up joining a saved net -> open portal

// --- Clock source (ESP32-S3 only: USB-MIDI vs Ableton Link) ---
// MIDI clock is 24 PPQN -- identical to the SMS sync rate -- so each MIDI clock
// byte (0xF8) maps 1:1 to one counter tick. In AUTO mode the bridge follows USB
// MIDI whenever clock is actively flowing, else falls back to Link over WiFi.
#define MIDI_ACTIVE_TIMEOUT_US 500000  // no clock for this long -> MIDI inactive

// A MIDI message can open the WiFi setup portal for users without a serial
// console: a **pitch bend on channel 16** (status 0xE0|15 = 0xEF). One-shot per
// boot. (Channel 16 / pitch bend is an uncommon control, so it won't fire by
// accident; change the status byte here to remap it.)
#define MIDI_PORTAL_TRIGGER_STATUS 0xEF

// --- MIDI takeover (ESP32-S3 only): live console-clocked note stream ----------
// In takeover mode the two port wires stop being the sync counter and become a
// console-clocked serial link that streams MIDI note events to the tracker, which
// plays them live on its own voices (its sequencer stepped aside). Same two pins
// as the counter -- mutually exclusive. Protocol = genmddj's MIDI.md 3 (the
// console side is already built to it). Roles:
//   PIN_MIDI_CLK  the CONSOLE drives (bridge reads) -- clock master, idle low
//   PIN_MIDI_DAT  the BRIDGE drives open-drain (console reads) -- data, MSB-first
// The console pulses CLK low->high->low and samples DAT on the RISING edge; the
// bridge changes DAT on the FALLING edge (so it's stable for the next rising
// sample). After an idle gap (CLK low, no edges) the bridge presents a leading
// FLAG bit: 1 => a fixed 3-byte event frame follows (status,d1,d2), 0 => queue
// empty. status = type<<4 | channel (type: 1=NoteOff 2=NoteOn 3=CC 4=PgmChange
// 5=PitchBend 7=Panic). The bridge normalises raw MIDI into these frames and
// coalesces CC; the console parser is a bounded type switch. Each event = 25 bits.
//
// ELECTRICAL: reading CLK puts the console's (5 V) drive on an ESP32 input --
// the same 5 V that already sits on the counter's open-drain pads when released,
// so it's the same regime the C3 counter already survives on hardware. Verify on
// the S3; add a series resistor / level shift on CLK if a real console misbehaves.
#define PIN_MIDI_CLK   PIN_TR   // console -> bridge : clock (TR=pin9); genmddj MIDI.md 3.1
#define PIN_MIDI_DAT   PIN_TH   // bridge  -> console : data (TH=pin7), open-drain, MSB-first
#define TAKEOVER_TIMEOUT_US  500000  // no channel-voice msg for this long -> back to counter (AUTO)
#define TAKEOVER_IDLE_US     1500    // CLK quiet this long -> idle gap: present a fresh flag bit
#define MIDI_EVTQ_SIZE       32      // pending compact events awaiting the console (power of two)

// --- Alignment offsets (by-ear latency compensation) ---
// Fallback defaults used only when nothing is stored in NVS. Tune live over
// serial (see main.cpp), `s` to persist; or bake your final value in here.
// +ms = emit earlier (compensates output latency); ms is tempo-independent.
// +ticks = whole 1/24-beat steps (tempo-dependent). Both may be negative.
#define DEFAULT_OFFSET_MS     75   // tuned by ear against a real SMS
#define DEFAULT_OFFSET_TICKS  0
