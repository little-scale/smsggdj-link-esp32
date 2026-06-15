// main.cpp
//
// Ableton Link -> SMS hardware-sync bridge for the Seeed XIAO ESP32-C3 (ESP-IDF).
// Joins a Link session over WiFi, derives a 24-PPQN tick clock from the shared
// beat timeline, and presents a rolling 2-bit counter on two open-drain GPIOs
// into SMS controller port 2, which SMSGGDJ's `SYNC: IN` reads once per frame.
//
// The whole protocol is the wire contract in CLAUDE.md; pinout in WIRING.md.

#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdio>
#include <cstdlib>

#include "config.h"
#include "secrets.h"  // copy secrets.h.example -> secrets.h

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_event.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "nvs.h"
#include "nvs_flash.h"

#include <ableton/Link.hpp>

static const char* TAG = "bridge";

// The ESP32-C3 is rv32imc (no atomic extension), so IDF's newlib emulates
// atomics with spinlocks but doesn't provide __atomic_is_lock_free for it.
// Link's TripleBuffer references it only via assert(mState.is_lock_free()) on a
// std::atomic<uint32_t>. Provide it with IDF's own convention (size <= int is
// "lock-free"); the 4-byte ops still route through IDF's emulated atomics.
extern "C" bool __atomic_is_lock_free(std::size_t size, const volatile void*) {
  return size <= sizeof(int);
}

// ---------------------------------------------------------------------------
// Presenter: the main loop computes the tick `target` from Link time; the timer
// ISR advances the wire counter toward it -- never backward, at most
// MAX_TICKS_PER_FRAME per SMS frame. A spinlock guards the 64-bit target (a
// 64-bit atomic is not lock-free / ISR-safe on the 32-bit RISC-V core).
// ---------------------------------------------------------------------------
static portMUX_TYPE g_mux = portMUX_INITIALIZER_UNLOCKED;
static volatile long long g_target = -1;  // <0 = stopped/armed (freeze)

static inline void writeCounter(uint8_t count) {
  // Open-drain: level 1 releases the line (SMS pull-up -> logic 1); level 0
  // drives it to ground (logic 0). So the bit value maps straight to the level.
  gpio_set_level(PIN_TR, (count & 0x1) ? 1 : 0);  // bit 0
  gpio_set_level(PIN_TH, (count & 0x2) ? 1 : 0);  // bit 1
}

static bool IRAM_ATTR onPresenterTimer(gptimer_handle_t,
                                       const gptimer_alarm_event_data_t*, void*) {
  long long target;
  portENTER_CRITICAL_ISR(&g_mux);
  target = g_target;
  portEXIT_CRITICAL_ISR(&g_mux);

  static long long presented = 0;
  static bool valid = false;

  // Stopped or armed-before-the-bar: hold the current count (tracker waits) and
  // re-arm so the next start snaps cleanly.
  if (target < 0) {
    valid = false;
    return false;
  }

  // First valid target after a (re)start: snap, don't ramp. SYNC IN re-arms on
  // transport start, so the discontinuity is absorbed.
  if (!valid) {
    presented = target;
    valid = true;
    writeCounter((uint8_t)(presented & 0x3));
    return false;
  }

  // Refill a per-SMS-frame budget so we never present more than
  // MAX_TICKS_PER_FRAME between two SMS port reads (the 2-bit delta saturates).
  static int64_t lastRefill = 0;
  static int budget = MAX_TICKS_PER_FRAME;
  const int64_t now = esp_timer_get_time();
  if (now - lastRefill >= 1000000 / SMS_FRAME_HZ) {
    budget = MAX_TICKS_PER_FRAME;
    lastRefill = now;
  }

  // Advance toward the target, never backward, capped by the budget.
  if (target > presented && budget > 0) {
    long long step = target - presented;
    if (step > budget) step = budget;
    presented += step;
    budget -= (int)step;
    writeCounter((uint8_t)(presented & 0x3));
  }
  return false;
}

// ---------------------------------------------------------------------------
// Ableton Link -> tick target (bar-aligned launch, 24 PPQN).
// Ported from the ares reference (link-session.cpp). No frame-PLL is needed on
// hardware -- unlike the emulator, nothing paces us to vsync.
// ---------------------------------------------------------------------------
static ableton::Link* g_link = nullptr;
static double g_launchBeat = 0;
static std::chrono::microseconds g_lastStart{-1};

// By-ear alignment offsets, live-tunable over serial, persisted in NVS.
// +ms shifts the sampled Link time later -> ticks emitted earlier (compensates
// output latency); tempo-independent. +ticks adds whole 1/24-beat steps. Both
// signed. 32-bit atomics are lock-free on the C3, and these are read only from
// the loop task (not the ISR), so plain atomics are fine.
static std::atomic<int> g_offsetMs{DEFAULT_OFFSET_MS};
static std::atomic<int> g_offsetTicks{DEFAULT_OFFSET_TICKS};

static long long computeTarget() {
  auto state = g_link->captureAppSessionState();
  if (!state.isPlaying()) {
    g_lastStart = std::chrono::microseconds{-1};
    return -1;  // freeze while stopped
  }

  // Snap launchBeat to the first bar line (phase 0 of the quantum) at/after the
  // transport start, so the tracker's row 0 lands on Live's beat 1.
  const auto start = state.timeForIsPlaying();
  if (start != g_lastStart) {
    g_lastStart = start;
    const double b0 = state.beatAtTime(start, LINK_QUANTUM);
    const double phase = state.phaseAtTime(start, LINK_QUANTUM);
    g_launchBeat = b0 - phase + (phase > 1e-9 ? LINK_QUANTUM : 0.0);
  }

  const auto now = g_link->clock().micros()
                   + std::chrono::microseconds((int64_t)g_offsetMs.load() * 1000);
  const double beats = state.beatAtTime(now, LINK_QUANTUM) - g_launchBeat;
  if (beats < 0.0) return -1;  // armed, before the bar line
  long long t = (long long)std::floor(beats * PPQN) + g_offsetTicks.load();
  return t < 0 ? 0 : t;
}

// ---------------------------------------------------------------------------
// WiFi (station, blocking until connected).
// ---------------------------------------------------------------------------
static EventGroupHandle_t s_wifi_events;
static const int WIFI_CONNECTED_BIT = BIT0;

static void wifi_event_handler(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    esp_wifi_connect();
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    ESP_LOGW(TAG, "WiFi disconnected, retrying");
    esp_wifi_connect();
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto* event = static_cast<ip_event_got_ip_t*>(data);
    ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
  }
}

static void wifi_connect() {
  s_wifi_events = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));

  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));

  wifi_config_t wc = {};
  std::snprintf(reinterpret_cast<char*>(wc.sta.ssid), sizeof(wc.sta.ssid), "%s", WIFI_SSID);
  std::snprintf(reinterpret_cast<char*>(wc.sta.password), sizeof(wc.sta.password), "%s", WIFI_PASS);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());

  ESP_LOGI(TAG, "WiFi connecting to \"%s\"...", WIFI_SSID);
  xEventGroupWaitBits(s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE, portMAX_DELAY);
}

// ---------------------------------------------------------------------------
// Offset persistence (NVS) + live tuning over the USB serial console.
// ---------------------------------------------------------------------------
static const char* NVS_NS = "bridge";

static void offsets_load() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;
  int32_t v;
  if (nvs_get_i32(h, "ms", &v) == ESP_OK) g_offsetMs = v;
  if (nvs_get_i32(h, "ticks", &v) == ESP_OK) g_offsetTicks = v;
  nvs_close(h);
}

static void offsets_save() {
  nvs_handle_t h;
  if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) {
    printf(">> save FAILED (nvs_open)\n");
    return;
  }
  nvs_set_i32(h, "ms", g_offsetMs.load());
  nvs_set_i32(h, "ticks", g_offsetTicks.load());
  const esp_err_t e = nvs_commit(h);
  nvs_close(h);
  printf(">> %s offset = %d ms, %d ticks\n",
         e == ESP_OK ? "SAVED" : "save FAILED", g_offsetMs.load(), g_offsetTicks.load());
}

static void offsets_print() {
  printf(">> offset = %d ms, %d ticks\n", g_offsetMs.load(), g_offsetTicks.load());
}

static void offsets_help() {
  printf("cmds: m <ms> (set) | t <ticks> (set) | + / - (nudge ms +-1) | "
         "z (zero) | s (save default) | p (print)\n");
}

static void handle_command(char* s) {
  while (*s == ' ' || *s == '\t') s++;
  const char c = *s;
  switch (c) {
    case '\0': offsets_print(); break;
    case 'm': g_offsetMs = (int)strtol(s + 1, nullptr, 10); offsets_print(); break;
    case 't': g_offsetTicks = (int)strtol(s + 1, nullptr, 10); offsets_print(); break;
    case '+': g_offsetMs += (s[1] ? (int)strtol(s, nullptr, 10) : 1); offsets_print(); break;
    case '-': g_offsetMs += (s[1] ? (int)strtol(s, nullptr, 10) : -1); offsets_print(); break;
    case 'z': g_offsetMs = 0; g_offsetTicks = 0; offsets_print(); break;
    case 's': offsets_save(); break;
    case 'p': offsets_print(); break;
    default: offsets_help(); break;
  }
}

static void serial_task(void*) {
  // Console input over the USB-Serial/JTAG peripheral.
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  usb_serial_jtag_driver_install(&cfg);
  usb_serial_jtag_vfs_use_driver();
  usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
  usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

  offsets_help();
  char line[48];
  size_t len = 0;
  while (true) {
    const int ch = fgetc(stdin);
    if (ch == EOF) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    putchar(ch);  // echo (the IDF monitor doesn't echo locally)
    fflush(stdout);
    if (ch == '\n' || ch == '\r') {
      line[len] = '\0';
      handle_command(line);
      len = 0;
    } else if (len < sizeof(line) - 1) {
      line[len++] = (char)ch;
    }
  }
}

// ---------------------------------------------------------------------------
static void gpio_setup() {
  gpio_config_t io = {};
  io.pin_bit_mask = (1ULL << PIN_TR) | (1ULL << PIN_TH);
  io.mode = GPIO_MODE_OUTPUT_OD;  // open-drain
  io.pull_up_en = GPIO_PULLUP_DISABLE;
  io.pull_down_en = GPIO_PULLDOWN_DISABLE;
  io.intr_type = GPIO_INTR_DISABLE;
  ESP_ERROR_CHECK(gpio_config(&io));
  writeCounter(0);  // release the lines (idle high via SMS pull-ups)
}

static void presenter_timer_start() {
  gptimer_handle_t timer = nullptr;
  gptimer_config_t tcfg = {};
  tcfg.clk_src = GPTIMER_CLK_SRC_DEFAULT;
  tcfg.direction = GPTIMER_COUNT_UP;
  tcfg.resolution_hz = 1000000;  // 1 MHz -> 1 tick = 1 us
  ESP_ERROR_CHECK(gptimer_new_timer(&tcfg, &timer));

  gptimer_event_callbacks_t cbs = {};
  cbs.on_alarm = onPresenterTimer;
  ESP_ERROR_CHECK(gptimer_register_event_callbacks(timer, &cbs, nullptr));
  ESP_ERROR_CHECK(gptimer_enable(timer));

  gptimer_alarm_config_t acfg = {};
  acfg.reload_count = 0;
  acfg.alarm_count = 1000000 / PRESENTER_HZ;
  acfg.flags.auto_reload_on_alarm = true;
  ESP_ERROR_CHECK(gptimer_set_alarm_action(timer, &acfg));
  ESP_ERROR_CHECK(gptimer_start(timer));
}

extern "C" void app_main() {
  ESP_ERROR_CHECK(nvs_flash_init());
  offsets_load();  // override compiled defaults with any saved value
  xTaskCreate(serial_task, "serial", 4096, nullptr, 5, nullptr);
  wifi_connect();
  gpio_setup();

  static ableton::Link link(120.0);
  link.enableStartStopSync(true);
  link.enable(true);
  g_link = &link;

  presenter_timer_start();
  ESP_LOGI(TAG, "bridge: ready");

  int64_t lastHud = 0;
  while (true) {
    const long long t = computeTarget();
    portENTER_CRITICAL(&g_mux);
    g_target = t;
    portEXIT_CRITICAL(&g_mux);

    const int64_t now = esp_timer_get_time();
    if (now - lastHud > 1000000) {
      lastHud = now;
      auto s = link.captureAppSessionState();
      ESP_LOGI(TAG, "peers=%u tempo=%.1f playing=%d target=%lld off=%dms/%dt",
               (unsigned)link.numPeers(), s.tempo(), (int)s.isPlaying(), t,
               g_offsetMs.load(), g_offsetTicks.load());
    }
    vTaskDelay(1);  // ~1 ms
  }
}
