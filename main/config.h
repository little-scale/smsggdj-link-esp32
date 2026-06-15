// config.h — board pins and sync constants for the Link->SMS bridge.
// See WIRING.md for the full pinout and CLAUDE.md for the wire contract.
#pragma once

// --- Pin map (Seeed XIAO ESP32-C3) ---
// Open-drain outputs into SMS controller port 2. The ESP32 only pulls low;
// the SMS's internal pull-ups supply the 5 V high.
#define PIN_TR   GPIO_NUM_3   // XIAO D1 -> port-2 pin 9 (TR) = counter bit 0
#define PIN_TH   GPIO_NUM_4   // XIAO D2 -> port-2 pin 7 (TH) = counter bit 1
// Ground: XIAO GND pad -> port-2 pin 8. Do NOT connect port-2 +5 V (pin 5).

// --- Sync constants ---
#define PPQN            24.0   // ticks per beat (fixed; matches tracker groove-6 lock)
#define LINK_QUANTUM    4.0    // beats per bar (4/4); bar-aligned launch on phase 0
#define MAX_TICKS_PER_FRAME 3  // 2-bit delta saturates at 3 per SMS frame read
#define SMS_FRAME_HZ    60     // NTSC; the SMS polls port 2 ~once per video frame

// --- Presenter timer ---
#define PRESENTER_HZ    1000   // how often the timer advances the counter / writes pins

// --- Alignment offsets (by-ear latency compensation) ---
// Fallback defaults used only when nothing is stored in NVS. Tune live over
// serial (see main.cpp), `s` to persist; or bake your final value in here.
// +ms = emit earlier (compensates output latency); ms is tempo-independent.
// +ticks = whole 1/24-beat steps (tempo-dependent). Both may be negative.
#define DEFAULT_OFFSET_MS     0
#define DEFAULT_OFFSET_TICKS  0
