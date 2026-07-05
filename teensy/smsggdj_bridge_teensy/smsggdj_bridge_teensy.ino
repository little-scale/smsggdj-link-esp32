// smsggdj_bridge_teensy.ino -- wired USB-MIDI -> SMS/Mega Drive sync bridge (Teensy 4.1)
//
// The wired sibling of the ESP32 firmware (../../main): no WiFi, no Ableton Link.
// A DAW sends USB-MIDI over USB-C; this drives the same two DE-9 controller-port
// wires, so SMSGGDJ (SMS/GG) and genmddj (Mega Drive) follow it identically.
//
// Two mutually-exclusive uses of the two wires (same as the ESP32-S3):
//   * MIDI clock  -> a rolling 2-bit tick counter the tracker reads in SYNC: IN.
//   * MIDI notes  -> a console-clocked serial link (MIDI takeover) the tracker
//                    plays live on its own voices, sequencer stopped.
//
// BUILD: Arduino IDE / Teensyduino. Board = Teensy 4.1. Tools > USB Type =
//        "Serial + MIDI" (Serial keeps the tuning console; MIDI is the clock/notes
//        input). See ../README.md for wiring + the 5 V level-shifter requirement.
//
// The wire contract lives in ../../MIDI.md and ../../main; keep this in sync.

#include <EEPROM.h>
#include "config.h"
#include "midi_proto.h"

// ---------------------------------------------------------------------------
// Shared state. loop() produces (MIDI tick, event queue); two ISRs consume
// (the 1 kHz presenter IntervalTimer, the CLK falling-edge interrupt). Cortex-M7
// single-word access is atomic, but multi-field updates are guarded with
// noInterrupts()/interrupts() -- the Teensy equivalent of the ESP32 portMUX.
// ---------------------------------------------------------------------------
IntervalTimer g_presenter;

// MIDI-clock transport -> counter target. target < 0 means stopped/armed (freeze).
volatile int32_t g_midiTick   = 0;
volatile bool    g_playing    = false;
volatile int32_t g_target     = -1;
volatile uint32_t g_lastClockUs = 0;
int32_t g_offsetTicks = DEFAULT_OFFSET_TICKS;

// Wire role: the counter presenter owns the pins, or the takeover responder does.
enum WireMode { WIRE_COUNTER = 0, WIRE_TAKEOVER = 1 };
volatile WireMode g_wireMode = WIRE_COUNTER;

// --- Takeover event ring + shift register (see midi_proto.h for the frame) ----
volatile MidiEvt  g_evtq[MIDI_EVTQ_SIZE];
volatile uint16_t g_eHead = 0, g_eTail = 0;   // free-running; index = x & (SIZE-1)
volatile uint32_t g_sr = 0;                   // shift register, next bit in bit 31
volatile int      g_srBits = 0;               // bits left in g_sr (0 => reload)
volatile uint32_t g_lastClkEdgeUs = 0;        // last CLK edge (idle-gap detection)
volatile bool     g_idleArmed = false;        // flag already presented this idle gap
volatile uint32_t g_clkEdges = 0;             // HUD
volatile uint32_t g_dropped  = 0;             // HUD

// Takeover engagement, matching the ESP32's `k` command.
enum TakeoverMode { TK_AUTO = 0, TK_ON = 1, TK_OFF = 2 };
volatile int      g_takeoverMode = TK_AUTO;
volatile uint32_t g_takeoverLastUs = 0;

static inline uint16_t evtq_count() { return (uint16_t)(g_eHead - g_eTail); }

// ---------------------------------------------------------------------------
// Open-drain wire I/O. Teensy OUTPUT_OPENDRAIN matches the ESP32 semantics:
// level 1 = release (the port's pull-up -> logic high), 0 = drive to ground.
// (5 V is kept off the Teensy by the external level shifter -- see config.h.)
// ---------------------------------------------------------------------------
static inline void writeCounter(uint8_t count) {
  digitalWriteFast(PIN_TR, (count & 0x1) ? HIGH : LOW);   // bit 0
  digitalWriteFast(PIN_TH, (count & 0x2) ? HIGH : LOW);   // bit 1
}

// ---------------------------------------------------------------------------
// Counter presenter (1 kHz). Advances a rolling count toward g_target: never
// backward, <= MAX_TICKS_PER_FRAME between two SMS frame reads. Byte-for-byte the
// logic of onPresenterTimer() in ../../main/main.cpp. While takeover owns the
// pins it steps aside and services the idle-gap resync instead.
// ---------------------------------------------------------------------------
static void present_bit_locked();     // fwd
static void takeover_idle_check();    // fwd

static void onPresenterTimer() {
  if (g_wireMode != WIRE_COUNTER) { takeover_idle_check(); return; }

  int32_t target = g_target;   // single word; atomic read

  static int32_t presented = 0;
  static bool valid = false;

  if (target < 0) { valid = false; return; }        // stopped/armed: hold

  if (!valid) {                                      // first target after (re)start: snap
    presented = target;
    valid = true;
    writeCounter((uint8_t)(presented & 0x3));
    return;
  }

  // Per-SMS-frame budget so we never present > MAX_TICKS_PER_FRAME between reads.
  static uint32_t lastRefill = 0;
  static int budget = MAX_TICKS_PER_FRAME;
  const uint32_t now = micros();
  if ((uint32_t)(now - lastRefill) >= 1000000UL / SMS_FRAME_HZ) {
    budget = MAX_TICKS_PER_FRAME;
    lastRefill = now;
  }

  if (target > presented && budget > 0) {            // advance, never backward, capped
    int32_t step = target - presented;
    if (step > budget) step = budget;
    presented += step;
    budget -= (int)step;
    writeCounter((uint8_t)(presented & 0x3));
  }
}

// ---------------------------------------------------------------------------
// MIDI takeover: stream notes to the console over the two wires, console-clocked.
// The console is the clock master (idle CLK low; pulses low->high->low, samples
// DAT on the RISING edge). We drive DAT open-drain and change it on the FALLING
// edge (MSB-first). After an idle gap we present a leading FLAG bit. Mirrors the
// ESP32's takeover module. See midi_proto.h for the frame packing.
// ---------------------------------------------------------------------------

// Load the next wire frame into the shift register (caller in a critical region):
// a queued event -> flag 1 + status:d1:d2 (25 bits), else flag 0 (1 bit).
static void load_frame_locked() {
  if (evtq_count() > 0) {
    const uint16_t idx = g_eTail++ & (MIDI_EVTQ_SIZE - 1);
    MidiEvt e;
    e.status = g_evtq[idx].status;
    e.d1 = g_evtq[idx].d1;
    e.d2 = g_evtq[idx].d2;
    g_sr = midi_frame_word(e);
    g_srBits = MIDI_FRAME_BITS;
  } else {
    g_sr = 0;          // flag = 0 (empty)
    g_srBits = 1;
  }
}

// Present the next DAT bit and advance. Reloads a fresh frame when exhausted --
// within a burst events run back-to-back; a new burst resyncs via the idle
// watchdog forcing g_srBits = 0 first.
static void present_bit_locked() {
  if (g_srBits == 0) load_frame_locked();
  const int bit = (int)((g_sr >> 31) & 1u);
  g_sr <<= 1;
  g_srBits--;
  digitalWriteFast(PIN_MIDI_DAT, bit ? HIGH : LOW);   // OD: 1 = release, 0 = low
}

// CLK falling edge: the console has sampled the current bit -> present the next.
static void clk_isr() {
  g_lastClkEdgeUs = micros();
  g_idleArmed = false;
  present_bit_locked();
  g_clkEdges++;
}

// From the presenter timer while in takeover: once CLK has been quiet for
// TAKEOVER_IDLE_US we're between bursts -> resync and present a fresh flag bit so
// it's stable before the console's next rising edge (idle-gap resync).
static void takeover_idle_check() {
  const uint32_t now = micros();
  if (!g_idleArmed && (uint32_t)(now - g_lastClkEdgeUs) > TAKEOVER_IDLE_US) {
    g_idleArmed = true;
    g_srBits = 0;              // drop any partial frame; reload from the flag
    present_bit_locked();
  }
}

// Enqueue a normalised event. CC coalesces (an unread same-channel same-controller
// CC has its value replaced in place, so a knob sweep can't flood the wire). A
// NoteOff/panic is `critical` -> never dropped (evict the oldest to fit). Called
// from loop(); guards against the two ISRs with a critical region.
static void evtq_push(const MidiEvt& ev, bool critical) {
  noInterrupts();
  if ((ev.status & 0xF0) == (EVT_CC << 4)) {
    for (uint16_t i = g_eTail; i != g_eHead; i++) {
      volatile MidiEvt& e = g_evtq[i & (MIDI_EVTQ_SIZE - 1)];
      if (e.status == ev.status && e.d1 == ev.d1) { e.d2 = ev.d2; interrupts(); return; }
    }
  }
  if (evtq_count() >= MIDI_EVTQ_SIZE) {
    if (!critical) { g_dropped++; interrupts(); return; }
    g_eTail++; g_dropped++;                 // evict the oldest so the note-off fits
  }
  volatile MidiEvt& slot = g_evtq[g_eHead & (MIDI_EVTQ_SIZE - 1)];
  slot.status = ev.status; slot.d1 = ev.d1; slot.d2 = ev.d2;
  g_eHead++;
  interrupts();
}

static bool takeover_active() {
  const int m = g_takeoverMode;
  if (m == TK_OFF) return false;
  if (m == TK_ON)  return true;
  return (uint32_t)(micros() - g_takeoverLastUs) < TAKEOVER_TIMEOUT_US;
}

// Reconfigure the two wires for the requested role. Only touches hardware on a
// real transition. Called from loop() (attach/detachInterrupt, pinMode -- not
// ISR-safe).
static void wire_set_mode(WireMode m) {
  if (m == g_wireMode) return;
  if (m == WIRE_TAKEOVER) {
    pinMode(PIN_MIDI_DAT, OUTPUT_OPENDRAIN);
    digitalWriteFast(PIN_MIDI_DAT, HIGH);      // DAT idle = released high
    noInterrupts();
    g_srBits = 0; g_idleArmed = false; g_lastClkEdgeUs = micros();
    present_bit_locked();                      // put a defined (flag) bit on DAT now
    interrupts();
    pinMode(PIN_MIDI_CLK, INPUT_PULLDOWN);     // console drives CLK; idle-low when unplugged
    attachInterrupt(digitalPinToInterrupt(PIN_MIDI_CLK), clk_isr, FALLING);
  } else {
    detachInterrupt(digitalPinToInterrupt(PIN_MIDI_CLK));
    pinMode(PIN_TR, OUTPUT_OPENDRAIN);
    pinMode(PIN_TH, OUTPUT_OPENDRAIN);
    writeCounter(0);                           // release both lines
    g_target = -1;                             // force the presenter to re-snap
  }
  g_wireMode = m;
}

// ---------------------------------------------------------------------------
// USB-MIDI input (device). USB Type must be "Serial + MIDI". We use the polling
// API (usbMIDI.read + getType/getChannel/getData*) so we act on the raw bytes,
// exactly like the ESP32's packet scan. Real-time clock/transport -> counter;
// channel-voice -> takeover FIFO.
// ---------------------------------------------------------------------------
static void handle_realtime(uint8_t status) {
  const uint32_t now = micros();
  switch (status) {
    case 0xF8:  if (g_playing) g_midiTick++;  g_lastClockUs = now; break;  // Clock
    case 0xFA:  g_midiTick = 0; g_playing = true;  g_lastClockUs = now; break;  // Start
    case 0xFB:  g_playing = true;              g_lastClockUs = now; break;  // Continue
    case 0xFC:  g_playing = false;             break;                       // Stop
    default: break;                                                         // ignore 0xFE/0xFF
  }
}

static void poll_usb_midi() {
  while (usbMIDI.read()) {
    const uint8_t type = usbMIDI.getType();   // status-byte form (see midi_proto.h notes)
    if (type >= 0xF8) { handle_realtime(type); continue; }   // System Real-Time

    // Channel-voice: getChannel() is 1..16 -> wire channel 0..15.
    const uint8_t statusHi = type & 0xF0;
    const uint8_t ch = (uint8_t)(usbMIDI.getChannel() - 1) & 0x0F;
    const uint8_t d1 = (uint8_t)usbMIDI.getData1();
    const uint8_t d2 = (uint8_t)usbMIDI.getData2();
    MidiEvt ev; bool critical;
    if (!midi_normalise(statusHi, ch, d1, d2, ev, critical)) continue;  // dropped 0xA0/0xD0
    evtq_push(ev, critical);
    g_takeoverLastUs = micros();
  }
}

// ---------------------------------------------------------------------------
// Serial tuning console (mirrors the ESP32's subset that still applies):
//   t <n>  tick offset (whole 1/24-beat steps)   |  p  print   |  s  save (EEPROM)
//   k <auto|on|off>  MIDI takeover mode           |  h  help
// (No `m` ms offset -- MIDI clock is reactive; no `c` source / `w` WiFi -- wired
// MIDI is the only source.)
// ---------------------------------------------------------------------------
struct Persist { uint32_t magic; int32_t offTicks; int32_t takeover; };
static const uint32_t PERSIST_MAGIC = 0x53474454;  // 'SGDT'

static void persist_load() {
  Persist p; EEPROM.get(0, p);
  if (p.magic != PERSIST_MAGIC) return;            // unwritten EEPROM -> compiled defaults
  g_offsetTicks = p.offTicks;
  g_takeoverMode = p.takeover;
}

static void persist_save() {
  Persist p = { PERSIST_MAGIC, g_offsetTicks, g_takeoverMode };
  EEPROM.put(0, p);
  Serial.printf(">> SAVED offset = %d ticks, takeover = %s\n",
                g_offsetTicks,
                g_takeoverMode == TK_ON ? "on" : g_takeoverMode == TK_OFF ? "off" : "auto");
}

static void print_help() {
  Serial.println(F(">> commands: t <n> (tick offset) | p (print) | s (save) | "
                   "k <auto|on|off> (MIDI takeover) | h (help)"));
}

static void set_takeover(const char* arg) {
  while (*arg == ' ') arg++;
  int m = TK_AUTO;
  if      (!strncmp(arg, "on", 2))  m = TK_ON;
  else if (!strncmp(arg, "off", 3)) m = TK_OFF;
  g_takeoverMode = m;
  Serial.printf(">> MIDI takeover: %s\n", m == TK_ON ? "on" : m == TK_OFF ? "off" : "auto");
}

static void handle_command(char* s) {
  while (*s == ' ' || *s == '\t') s++;
  switch (*s) {
    case 't': g_offsetTicks = atoi(s + 1);
              Serial.printf(">> tick offset = %d\n", g_offsetTicks); break;
    case 'p': Serial.printf(">> offset = %d ticks, takeover = %s\n", g_offsetTicks,
                            g_takeoverMode == TK_ON ? "on" : g_takeoverMode == TK_OFF ? "off" : "auto");
              break;
    case 's': persist_save(); break;
    case 'k': set_takeover(s + 1); break;
    case 'h': case '?': print_help(); break;
    case '\0': break;
    default: Serial.printf(">> unknown command '%c' (h for help)\n", *s); break;
  }
}

static void poll_serial() {
  static char line[32];
  static uint8_t len = 0;
  while (Serial.available()) {
    const char c = (char)Serial.read();
    if (c == '\n' || c == '\r') { line[len] = '\0'; if (len) handle_command(line); len = 0; }
    else if (len < sizeof(line) - 1) line[len++] = c;
  }
}

// ---------------------------------------------------------------------------
void setup() {
  Serial.begin(115200);
  pinMode(PIN_TR, OUTPUT_OPENDRAIN);
  pinMode(PIN_TH, OUTPUT_OPENDRAIN);
  writeCounter(0);                       // release both lines
  persist_load();
  g_presenter.begin(onPresenterTimer, 1000000 / PRESENTER_HZ);   // 1 kHz
  print_help();
}

void loop() {
  poll_usb_midi();
  poll_serial();

  // Arbitrate the two wires: channel-voice traffic -> takeover, else counter.
  wire_set_mode(takeover_active() ? WIRE_TAKEOVER : WIRE_COUNTER);

  // Recompute the counter target from the MIDI-clock transport (mirrors the
  // ESP32 loop). Playing but no recent clock byte -> treat as stopped (freeze).
  bool playing = g_playing && ((uint32_t)(micros() - g_lastClockUs) < MIDI_ACTIVE_TIMEOUT_US);
  int32_t t = playing ? (g_midiTick + g_offsetTicks) : -1;
  if (t < 0 && playing) t = 0;
  g_target = t;

  // 1 Hz HUD.
  static uint32_t lastHud = 0;
  if ((uint32_t)(millis() - lastHud) >= 1000) {
    lastHud = millis();
    Serial.printf("tick=%ld playing=%d target=%ld off=%dt%s",
                  (long)g_midiTick, (int)playing, (long)g_target, g_offsetTicks,
                  g_wireMode == WIRE_TAKEOVER || g_takeoverMode != TK_AUTO ? "" : "\n");
    if (g_wireMode == WIRE_TAKEOVER || g_takeoverMode != TK_AUTO)
      Serial.printf("  takeover=%s evtq=%u edges=%lu drop=%lu\n",
                    g_wireMode == WIRE_TAKEOVER ? "ON" : "off",
                    (unsigned)evtq_count(), (unsigned long)g_clkEdges, (unsigned long)g_dropped);
  }
}
