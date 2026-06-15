// smsggdj-link-esp32.ino
//
// Ableton Link -> SMS hardware-sync bridge for the Seeed XIAO ESP32-C3.
// Joins a Link session over WiFi, derives a 24-PPQN tick clock from the shared
// beat timeline, and presents a rolling 2-bit counter on two open-drain GPIOs
// into SMS controller port 2, which SMSGGDJ's `SYNC: IN` reads once per frame.
//
// The whole protocol is the wire contract in CLAUDE.md; pinout in WIRING.md.
//
// Status: SCAFFOLD. The presenter, GPIO, and timer paths are complete; the
// Ableton Link integration is stubbed (see link_update_target()).

#include <Arduino.h>
#include <WiFi.h>
#include "config.h"
#include "secrets.h"   // copy secrets.h.example -> secrets.h

// ---------------------------------------------------------------------------
// Shared state between the main loop (computes the tick target from Link time)
// and the presenter ISR (advances the wire counter toward it). The counter must
// never run backward and may advance at most MAX_TICKS_PER_FRAME per SMS frame.
// ---------------------------------------------------------------------------
volatile long long g_target    = -1;   // desired tick count; <0 = stopped/armed (freeze)
volatile long long g_presented = 0;    // count currently on the wire
volatile bool      g_valid     = false;

static hw_timer_t* g_presenterTimer = nullptr;

// Drive the two open-drain lines to bits [1:0] of `count`.
// Open-drain: HIGH releases the line (SMS pull-up -> logic 1); LOW drives it to
// ground (logic 0). So digitalWrite(pin, bit) is exactly right.
static inline void writeCounter(uint8_t count) {
  digitalWrite(PIN_TR, (count & 0x1) ? HIGH : LOW);   // bit 0
  digitalWrite(PIN_TH, (count & 0x2) ? HIGH : LOW);   // bit 1
}

// Presenter ISR: monotonic, rate-limited advance of the wire counter.
void IRAM_ATTR onPresenterTimer() {
  const long long target = g_target;

  // Stopped or armed-before-the-bar: hold the current count (tracker waits).
  if (target < 0) return;

  // First valid target after a (re)start: snap, don't ramp.
  if (!g_valid) {
    g_presented = target;
    g_valid = true;
    writeCounter((uint8_t)(g_presented & 0x3));
    return;
  }

  // Refill a per-SMS-frame budget so we never present more than
  // MAX_TICKS_PER_FRAME between two SMS port reads (the 2-bit delta saturates).
  static uint32_t lastRefill = 0;
  static int budget = MAX_TICKS_PER_FRAME;
  const uint32_t now = micros();
  if (now - lastRefill >= 1000000UL / SMS_FRAME_HZ) {
    budget = MAX_TICKS_PER_FRAME;
    lastRefill = now;
  }

  // Advance toward the target, never backward, capped by the budget.
  if (target > g_presented && budget > 0) {
    long long step = target - g_presented;
    if (step > budget) step = budget;
    g_presented += step;
    budget -= (int)step;
    writeCounter((uint8_t)(g_presented & 0x3));
  }
}

// ---------------------------------------------------------------------------
// Ableton Link
// ---------------------------------------------------------------------------
// TODO: integrate the Ableton Link library into the Arduino build (see the SDK
// at ~/Documents/ares-link-sync/link/ and the reference port in CLAUDE.md ->
// ares .../link-sync/link-session.cpp). Once linked, this is the whole job:
//
//   auto state = link.captureAppSessionState();
//   if (!state.isPlaying()) { g_target = -1; return; }          // freeze
//   // bar-aligned launch: snap launchBeat to phase 0 of the quantum at start
//   const double beats = state.beatAtTime(now, LINK_QUANTUM) - launchBeat;
//   if (beats < 0.0) { g_target = -1; return; }                 // armed pre-bar
//   long long t = (long long)floor(beats * PPQN) + g_offsetTicks;
//   g_target = t < 0 ? 0 : t;
//
// `now` is link.clock().micros() (+ optional g_offsetMs latency). No frame-PLL
// is needed on hardware — unlike the emulator, nothing paces us to vsync.

static void link_setup() {
  // link.enableStartStopSync(true);
  // link.enable(true);
}

static void link_update_target() {
  // STUB: keep the counter frozen until Link is wired in.
  g_target = -1;
}

// ---------------------------------------------------------------------------
// WiFi
// ---------------------------------------------------------------------------
static void wifi_connect() {
  Serial.printf("WiFi: connecting to \"%s\"", WIFI_SSID);
  WiFi.mode(WIFI_STA);
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  while (WiFi.status() != WL_CONNECTED) {
    delay(250);
    Serial.print(".");
  }
  Serial.printf(" ok, ip=%s\n", WiFi.localIP().toString().c_str());
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  delay(200);

  // Open-drain outputs; release the lines (idle = pull-up high) at boot.
  pinMode(PIN_TR, OUTPUT_OPEN_DRAIN);
  pinMode(PIN_TH, OUTPUT_OPEN_DRAIN);
  writeCounter(0);

  wifi_connect();
  link_setup();

  // Presenter timer: a 1 MHz time base, alarm every (1e6/PRESENTER_HZ) us.
  g_presenterTimer = timerBegin(1000000);
  timerAttachInterrupt(g_presenterTimer, &onPresenterTimer);
  timerAlarm(g_presenterTimer, 1000000UL / PRESENTER_HZ, true, 0);

  Serial.println("bridge: ready");
}

void loop() {
  link_update_target();

  // Light HUD over serial, ~4 Hz.
  static uint32_t lastHud = 0;
  if (millis() - lastHud >= 250) {
    lastHud = millis();
    Serial.printf("target=%lld presented=%lld valid=%d\n",
                  g_target, g_presented, (int)g_valid);
  }
  delay(1);
}
