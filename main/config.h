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

// --- Alignment offsets (by-ear latency compensation) ---
// Fallback defaults used only when nothing is stored in NVS. Tune live over
// serial (see main.cpp), `s` to persist; or bake your final value in here.
// +ms = emit earlier (compensates output latency); ms is tempo-independent.
// +ticks = whole 1/24-beat steps (tempo-dependent). Both may be negative.
#define DEFAULT_OFFSET_MS     75   // tuned by ear against a real SMS
#define DEFAULT_OFFSET_TICKS  0
