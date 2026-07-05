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
#include <cstring>

#include "config.h"

// secrets.h is now OPTIONAL: it's a developer fast-path only. When present (and
// not a release build), an unprovisioned board connects with these compile-time
// creds instead of opening the setup portal. Shipped binaries are built without
// it (or with -DSHIP=1), so they always provision over the captive portal.
#if defined(__has_include)
#  if __has_include("secrets.h")
#    include "secrets.h"  // copy secrets.h.example -> secrets.h
#    define HAVE_SECRETS 1
#  endif
#endif

#include "driver/gpio.h"
#include "driver/gptimer.h"
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#include "esp_event.h"
#include "esp_http_server.h"
#include "esp_log.h"
#include "esp_mac.h"
#include "esp_netif.h"
#include "esp_system.h"
#include "esp_timer.h"
#include "esp_wifi.h"
#include "freertos/FreeRTOS.h"
#include "freertos/event_groups.h"
#include "freertos/task.h"
#include "lwip/sockets.h"
#include "nvs.h"
#include "nvs_flash.h"

#if defined(CONFIG_IDF_TARGET_ESP32S3)
// USB-MIDI clock input rides on the S3's USB-OTG peripheral (TinyUSB). The
// console shares the same USB-C as a composite CDC interface.
#include "tinyusb.h"
#include "tusb_cdc_acm.h"
#include "tusb_console.h"
#endif

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

#if defined(CONFIG_IDF_TARGET_ESP32S3)
// Which role the two port wires currently serve. COUNTER = the Link/MIDI-clock
// sync presenter owns them; TAKEOVER = the console-clocked MIDI note responder
// owns them (mutually exclusive). Declared here so the presenter ISR can bail.
enum WireMode { WIRE_COUNTER = 0, WIRE_TAKEOVER = 1 };
static volatile WireMode g_wireMode = WIRE_COUNTER;
static void IRAM_ATTR takeover_idle_check();  // fwd; defined in the takeover module
#endif

static inline void writeCounter(uint8_t count) {
  // Open-drain: level 1 releases the line (SMS pull-up -> logic 1); level 0
  // drives it to ground (logic 0). So the bit value maps straight to the level.
  gpio_set_level(PIN_TR, (count & 0x1) ? 1 : 0);  // bit 0
  gpio_set_level(PIN_TH, (count & 0x2) ? 1 : 0);  // bit 1
}

static bool IRAM_ATTR onPresenterTimer(gptimer_handle_t,
                                       const gptimer_alarm_event_data_t*, void*) {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  if (g_wireMode != WIRE_COUNTER) { takeover_idle_check(); return false; }  // takeover owns the pins
#endif
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
// Clock source selection. The presenter is source-agnostic -- it just chases
// `g_target`. On the S3 that target can come from Ableton Link (above) or from
// USB-MIDI clock (below); on the C3 (no USB-OTG) it's always Link.
// ---------------------------------------------------------------------------
enum SourceMode { SRC_AUTO = 0, SRC_LINK = 1, SRC_MIDI = 2 };
static std::atomic<int> g_sourceMode{SRC_AUTO};

static const char* sourceModeName(int m) {
  return m == SRC_LINK ? "link" : m == SRC_MIDI ? "midi" : "auto";
}

// ---------------------------------------------------------------------------
// WiFi. Credentials come from (in priority order): NVS (set by the captive
// portal), then -- dev builds only -- compile-time secrets.h. With none, the
// board hosts a SoftAP "SMSGGDJ-setup" + captive portal so any phone/laptop
// browser can pick the network; the choice is saved to NVS and the board
// reboots onto it. No Bluetooth, no app, no host toolchain -- this is what
// makes a single binary shippable to anyone.
// ---------------------------------------------------------------------------
static EventGroupHandle_t s_wifi_events;
static const int WIFI_CONNECTED_BIT = BIT0;
static volatile bool s_provisioning = false;  // AP-portal phase: ignore STA flaps

static void wifi_event_handler(void*, esp_event_base_t base, int32_t id, void* data) {
  if (base == WIFI_EVENT && id == WIFI_EVENT_STA_START) {
    if (!s_provisioning) esp_wifi_connect();  // don't auto-join old creds during the portal
  } else if (base == WIFI_EVENT && id == WIFI_EVENT_STA_DISCONNECTED) {
    if (s_provisioning) return;  // portal is up; not trying to be a station
    ESP_LOGW(TAG, "WiFi disconnected, retrying");
    esp_wifi_connect();          // keep retrying (initial connect + steady state)
  } else if (base == IP_EVENT && id == IP_EVENT_STA_GOT_IP) {
    auto* event = static_cast<ip_event_got_ip_t*>(data);
    ESP_LOGI(TAG, "got ip: " IPSTR, IP2STR(&event->ip_info.ip));
    xEventGroupSetBits(s_wifi_events, WIFI_CONNECTED_BIT);
  }
}

// --- WiFi credential storage (NVS namespace "wifi") ------------------------
static bool wifi_creds_load(char* ssid, size_t ssid_cap, char* pass, size_t pass_cap) {
  nvs_handle_t h;
  if (nvs_open(WIFI_NVS_NS, NVS_READONLY, &h) != ESP_OK) return false;
  size_t sl = ssid_cap, pl = pass_cap;
  const esp_err_t e1 = nvs_get_str(h, "ssid", ssid, &sl);
  const esp_err_t e2 = nvs_get_str(h, "pass", pass, &pl);
  nvs_close(h);
  return e1 == ESP_OK && e2 == ESP_OK && ssid[0] != '\0';
}

static void wifi_creds_save(const char* ssid, const char* pass) {
  nvs_handle_t h;
  if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_set_str(h, "ssid", ssid);
  nvs_set_str(h, "pass", pass);
  nvs_commit(h);
  nvs_close(h);
}

static void wifi_creds_clear(void) {
  nvs_handle_t h;
  if (nvs_open(WIFI_NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
  nvs_erase_key(h, "ssid");
  nvs_erase_key(h, "pass");
  nvs_commit(h);
  nvs_close(h);
}

// Try to join as a station; returns true once an IP is acquired, false on
// timeout (wrong password / network gone) so the caller can open the portal.
// C3 only -- the S3 connects in the background (see wifi_start_background).
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
static bool wifi_try_sta(const char* ssid, const char* pass) {
  wifi_config_t wc = {};
  std::snprintf(reinterpret_cast<char*>(wc.sta.ssid), sizeof(wc.sta.ssid), "%s", ssid);
  std::snprintf(reinterpret_cast<char*>(wc.sta.password), sizeof(wc.sta.password), "%s", pass);
  s_provisioning = false;
  xEventGroupClearBits(s_wifi_events, WIFI_CONNECTED_BIT);
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGI(TAG, "WiFi connecting to \"%s\"...", ssid);
  const EventBits_t bits = xEventGroupWaitBits(
    s_wifi_events, WIFI_CONNECTED_BIT, pdFALSE, pdTRUE,
    pdMS_TO_TICKS(STA_CONNECT_TIMEOUT_MS));
  if (bits & WIFI_CONNECTED_BIT) return true;
  ESP_LOGW(TAG, "WiFi connect timed out");
  ESP_ERROR_CHECK(esp_wifi_stop());
  return false;
}
#endif  // !CONFIG_IDF_TARGET_ESP32S3

// --- Captive portal (SoftAP + DNS hijack + HTTP setup form) ----------------
static const char* PORTAL_HEAD =
  "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
  "<title>SMSGGDJ WiFi setup</title>"
  "<style>body{font-family:system-ui,sans-serif;max-width:24rem;margin:2rem auto;padding:0 1rem}"
  "input,button{font-size:1rem;width:100%;box-sizing:border-box;padding:.5rem;margin:.25rem 0}"
  "button{background:#111;color:#fff;border:0;border-radius:.4rem}</style>"
  "<h2>SMSGGDJ &rarr; SMS bridge</h2><p>Pick your WiFi network:</p>"
  "<form method=POST action=/save>"
  "<input name=ssid list=nets placeholder='network name' autofocus required>"
  "<datalist id=nets>";
static const char* PORTAL_TAIL =
  "</datalist>"
  "<input name=pass type=password placeholder='password (blank if open)'>"
  "<button type=submit>Save &amp; connect</button></form>"
  "<p style=color:#888;font-size:.85rem>The bridge reboots and joins this "
  "network. To change it later, type <code>w</code> on the serial console.</p>";

static esp_err_t portal_root_get(httpd_req_t* req) {
  // Scan for nearby networks to populate the datalist (manual entry still works).
  wifi_scan_config_t sc = {};
  esp_wifi_scan_start(&sc, true);
  uint16_t n = 0;
  esp_wifi_scan_get_ap_num(&n);
  if (n > 20) n = 20;
  wifi_ap_record_t recs[20];
  esp_wifi_scan_get_ap_records(&n, recs);

  httpd_resp_set_type(req, "text/html");
  httpd_resp_sendstr_chunk(req, PORTAL_HEAD);
  for (uint16_t i = 0; i < n; i++) {
    if (recs[i].ssid[0] == '\0') continue;
    httpd_resp_sendstr_chunk(req, "<option value=\"");
    httpd_resp_sendstr_chunk(req, reinterpret_cast<const char*>(recs[i].ssid));
    httpd_resp_sendstr_chunk(req, "\">");
  }
  httpd_resp_sendstr_chunk(req, PORTAL_TAIL);
  httpd_resp_sendstr_chunk(req, nullptr);  // end response
  return ESP_OK;
}

// In-place URL-decode of an application/x-www-form-urlencoded value.
static void url_decode(char* s) {
  char* o = s;
  for (char* p = s; *p; p++) {
    if (*p == '+') {
      *o++ = ' ';
    } else if (*p == '%' && p[1] && p[2]) {
      auto hex = [](char c) -> int {
        if (c >= '0' && c <= '9') return c - '0';
        if (c >= 'a' && c <= 'f') return c - 'a' + 10;
        if (c >= 'A' && c <= 'F') return c - 'A' + 10;
        return 0;
      };
      *o++ = (char)((hex(p[1]) << 4) | hex(p[2]));
      p += 2;
    } else {
      *o++ = *p;
    }
  }
  *o = '\0';
}

static esp_err_t portal_save_post(httpd_req_t* req) {
  char body[256];
  int len = httpd_req_recv(req, body, sizeof(body) - 1);
  if (len <= 0) return ESP_FAIL;
  body[len] = '\0';

  char ssid[33] = {0};
  char pass[65] = {0};
  if (httpd_query_key_value(body, "ssid", ssid, sizeof(ssid)) == ESP_OK) url_decode(ssid);
  if (httpd_query_key_value(body, "pass", pass, sizeof(pass)) == ESP_OK) url_decode(pass);

  if (ssid[0] == '\0') {
    httpd_resp_send_err(req, HTTPD_400_BAD_REQUEST, "network name required");
    return ESP_OK;
  }
  ESP_LOGI(TAG, "portal: saving creds for \"%s\"", ssid);
  wifi_creds_save(ssid, pass);
  httpd_resp_set_type(req, "text/html");
  httpd_resp_sendstr(req,
    "<!doctype html><meta name=viewport content='width=device-width,initial-scale=1'>"
    "<body style='font-family:system-ui;text-align:center;margin-top:3rem'>"
    "<h2>Saved \xe2\x9c\x93</h2><p>The bridge is rebooting and joining your "
    "network&hellip;</p>");
  vTaskDelay(pdMS_TO_TICKS(800));  // let the response flush before we reboot
  esp_restart();
  return ESP_OK;  // unreachable
}

// Catch-all: redirect every other URL to the form so OS captive-portal probes
// (Android /generate_204, iOS /hotspot-detect.html, Windows /ncsi.txt) pop it.
static esp_err_t portal_redirect_get(httpd_req_t* req) {
  httpd_resp_set_status(req, "302 Found");
  httpd_resp_set_hdr(req, "Location", "http://192.168.4.1/");
  httpd_resp_send(req, nullptr, 0);
  return ESP_OK;
}

// Minimal DNS hijack: answer every A query with the AP's IP so phones auto-open
// the captive portal instead of waiting for a navigation.
static void dns_hijack_task(void*) {
  int sock = socket(AF_INET, SOCK_DGRAM, IPPROTO_UDP);
  struct sockaddr_in addr = {};
  addr.sin_family = AF_INET;
  addr.sin_port = htons(53);
  addr.sin_addr.s_addr = htonl(INADDR_ANY);
  bind(sock, (struct sockaddr*)&addr, sizeof(addr));

  uint8_t buf[512];
  while (true) {
    struct sockaddr_in src = {};
    socklen_t slen = sizeof(src);
    const int n = recvfrom(sock, buf, sizeof(buf), 0, (struct sockaddr*)&src, &slen);
    if (n < (int)sizeof(uint16_t) * 6) continue;  // smaller than a DNS header+
    // Turn the query into an answer in place: set QR/AA flags, ANCOUNT=1, then
    // append a pointer-to-question A record for 192.168.4.1.
    buf[2] |= 0x84;          // QR=1, AA=1
    buf[3] = 0x00;           // RA=0, RCODE=0
    buf[6] = 0x00; buf[7] = 0x01;  // ANCOUNT = 1
    int p = n;
    const uint8_t answer[] = {
      0xc0, 0x0c,            // name: pointer to question
      0x00, 0x01, 0x00, 0x01,// type A, class IN
      0x00, 0x00, 0x00, 0x3c,// TTL 60s
      0x00, 0x04,            // RDLENGTH 4
      192, 168, 4, 1,        // RDATA: AP IP
    };
    if (p + (int)sizeof(answer) <= (int)sizeof(buf)) {
      memcpy(buf + p, answer, sizeof(answer));
      p += sizeof(answer);
      sendto(sock, buf, p, 0, (struct sockaddr*)&src, slen);
    }
  }
}

static void start_captive_portal(void) {
  s_provisioning = true;
  esp_wifi_stop();  // tolerate being called while a STA connect is already running
  uint8_t mac[6] = {0};
  esp_wifi_get_mac(WIFI_IF_AP, mac);
  char ap_ssid[32];
  int ssid_len = std::snprintf(ap_ssid, sizeof(ap_ssid), "%s-%02X%02X",
                               AP_SSID_PREFIX, mac[4], mac[5]);
  if (ssid_len > (int)sizeof(ap_ssid) - 1) ssid_len = sizeof(ap_ssid) - 1;

  wifi_config_t ap = {};
  std::memcpy(ap.ap.ssid, ap_ssid, ssid_len);
  ap.ap.ssid_len = ssid_len;
  ap.ap.channel = AP_CHANNEL;
  ap.ap.max_connection = 4;
  ap.ap.authmode = WIFI_AUTH_OPEN;  // open AP: no password to join the setup net
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_APSTA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_AP, &ap));
  ESP_ERROR_CHECK(esp_wifi_start());
  ESP_LOGW(TAG, "no WiFi creds -- open setup portal: join \"%s\", browse to http://192.168.4.1", ap_ssid);

  xTaskCreate(dns_hijack_task, "dns", 3072, nullptr, 4, nullptr);

  httpd_handle_t server = nullptr;
  httpd_config_t hc = HTTPD_DEFAULT_CONFIG();
  hc.uri_match_fn = httpd_uri_match_wildcard;
  hc.lru_purge_enable = true;
  ESP_ERROR_CHECK(httpd_start(&server, &hc));
  const httpd_uri_t root = {"/", HTTP_GET, portal_root_get, nullptr};
  const httpd_uri_t save = {"/save", HTTP_POST, portal_save_post, nullptr};
  const httpd_uri_t any  = {"/*", HTTP_GET, portal_redirect_get, nullptr};
  httpd_register_uri_handler(server, &root);
  httpd_register_uri_handler(server, &save);
  httpd_register_uri_handler(server, &any);

  // Block here forever: the POST handler reboots once the user submits creds.
  while (true) vTaskDelay(pdMS_TO_TICKS(1000));
}

static void wifi_init_common() {
  s_wifi_events = xEventGroupCreate();
  ESP_ERROR_CHECK(esp_netif_init());
  ESP_ERROR_CHECK(esp_event_loop_create_default());
  esp_netif_create_default_wifi_sta();
  esp_netif_create_default_wifi_ap();

  wifi_init_config_t cfg = WIFI_INIT_CONFIG_DEFAULT();
  ESP_ERROR_CHECK(esp_wifi_init(&cfg));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    WIFI_EVENT, ESP_EVENT_ANY_ID, &wifi_event_handler, nullptr, nullptr));
  ESP_ERROR_CHECK(esp_event_handler_instance_register(
    IP_EVENT, IP_EVENT_STA_GOT_IP, &wifi_event_handler, nullptr, nullptr));
}

// Resolve creds from NVS, falling back to compile-time secrets.h on dev builds.
static bool wifi_resolve_creds(char* ssid, size_t ssid_cap, char* pass, size_t pass_cap) {
  bool have = wifi_creds_load(ssid, ssid_cap, pass, pass_cap);
#if defined(HAVE_SECRETS) && !defined(BRIDGE_PROVISIONING)
  if (!have) {  // developer fast-path: compile-time creds when NVS is empty
    std::snprintf(ssid, ssid_cap, "%s", WIFI_SSID);
    std::snprintf(pass, pass_cap, "%s", WIFI_PASS);
    have = true;
  }
#endif
  return have;
}

// C3 (Link-only): WiFi is mandatory, so block until connected; with no/bad
// creds, host the captive portal until the user provisions one.
#if !defined(CONFIG_IDF_TARGET_ESP32S3)
static void wifi_connect() {
  wifi_init_common();
  char ssid[33] = {0};
  char pass[65] = {0};
  const bool have = wifi_resolve_creds(ssid, sizeof(ssid), pass, sizeof(pass));
  if (have && wifi_try_sta(ssid, pass)) return;  // connected as a station
  start_captive_portal();                        // no/bad creds -> setup portal
}
#endif

// S3 (Link + USB-MIDI): WiFi is optional -- a MIDI-only user may never set it.
// Bring Link up in the background if creds exist; never block, never auto-open
// the portal (the serial `w` command opens it on demand).
static void wifi_start_background() {
  wifi_init_common();
  char ssid[33] = {0};
  char pass[65] = {0};
  if (!wifi_resolve_creds(ssid, sizeof(ssid), pass, sizeof(pass))) {
    ESP_LOGW(TAG, "no WiFi creds -- USB-MIDI only; type 'w' to set up WiFi for Link");
    return;
  }
  wifi_config_t wc = {};
  // Bounded copy (creds buffers are zero-initialized, so this stays terminated);
  // memcpy avoids the false -Wformat-truncation on the fixed-size driver field.
  std::memcpy(wc.sta.ssid, ssid, sizeof(wc.sta.ssid));
  std::memcpy(wc.sta.password, pass, sizeof(wc.sta.password));
  s_provisioning = false;
  ESP_ERROR_CHECK(esp_wifi_set_mode(WIFI_MODE_STA));
  ESP_ERROR_CHECK(esp_wifi_set_config(WIFI_IF_STA, &wc));
  ESP_ERROR_CHECK(esp_wifi_start());  // event handler connects + retries forever
  ESP_LOGI(TAG, "WiFi connecting to \"%s\" in background (for Link)...", ssid);
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
  if (nvs_get_i32(h, "src", &v) == ESP_OK) g_sourceMode = v;
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
  nvs_set_i32(h, "src", g_sourceMode.load());
  const esp_err_t e = nvs_commit(h);
  nvs_close(h);
  printf(">> %s offset = %d ms, %d ticks, source = %s\n",
         e == ESP_OK ? "SAVED" : "save FAILED", g_offsetMs.load(), g_offsetTicks.load(),
         sourceModeName(g_sourceMode.load()));
}

static void offsets_print() {
  printf(">> offset = %d ms, %d ticks, source = %s\n",
         g_offsetMs.load(), g_offsetTicks.load(), sourceModeName(g_sourceMode.load()));
}

static void offsets_help() {
  printf("cmds: m <ms> | t <ticks> | + / - (nudge ms) | z (zero) | s (save) | "
         "p (print)"
#if defined(CONFIG_IDF_TARGET_ESP32S3)
         " | c <auto|link|midi> (clock source)"
         " | k <auto|on|off> (MIDI takeover)"
#endif
         " | w (WiFi setup)\n");
}

// Parse the `c` clock-source command argument (S3 only).
static void set_source(const char* arg) {
  while (*arg == ' ') arg++;
  if (!std::strncmp(arg, "link", 4)) g_sourceMode = SRC_LINK;
  else if (!std::strncmp(arg, "midi", 4)) g_sourceMode = SRC_MIDI;
  else g_sourceMode = SRC_AUTO;
  offsets_print();
}

#if defined(CONFIG_IDF_TARGET_ESP32S3)
static void start_portal_task(void*) { start_captive_portal(); }  // blocks, reboots on save

// Launch the captive portal once (guarded so a held pitch-bend stream or a
// repeated `w` can't spawn it twice). Shared by the serial `w` command and the
// USB-MIDI trigger, so a user with no serial console can still set up WiFi.
static void open_portal_once() {
  static std::atomic<bool> launched{false};
  if (launched.exchange(true)) return;  // already up
  printf(">> WiFi setup portal starting -- join \"%s-XXXX\", browse to http://192.168.4.1\n",
         AP_SSID_PREFIX);
  xTaskCreate(start_portal_task, "portal", 4096, nullptr, 5, nullptr);
}
#endif

#if defined(CONFIG_IDF_TARGET_ESP32S3)
static void set_takeover(const char* arg);  // defined in the takeover module below
#endif

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
    case 'c': set_source(s + 1); break;
    case 'k':
#if defined(CONFIG_IDF_TARGET_ESP32S3)
      set_takeover(s + 1);
#endif
      break;
    case 'w':
      wifi_creds_clear();
#if defined(CONFIG_IDF_TARGET_ESP32S3)
      // S3 doesn't auto-portal at boot (MIDI may be the only source), so open it
      // now in its own task; the portal reboots once the user submits creds.
      open_portal_once();
#else
      printf(">> forgetting WiFi creds, rebooting into setup portal...\n");
      vTaskDelay(pdMS_TO_TICKS(200));
      esp_restart();
#endif
      break;
    default: offsets_help(); break;
  }
}

// Accumulate console input one char at a time: echo, and dispatch on newline.
// Shared by the C3 USB-Serial/JTAG reader task and the S3 USB-CDC RX callback.
static void feed_console_char(int ch) {
  static char line[48];
  static size_t len = 0;
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

#if !defined(CONFIG_IDF_TARGET_ESP32S3)
static void serial_task(void*) {
  // Console over the C3's USB-Serial/JTAG peripheral.
  usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
  usb_serial_jtag_driver_install(&cfg);
  usb_serial_jtag_vfs_use_driver();
  usb_serial_jtag_vfs_set_rx_line_endings(ESP_LINE_ENDINGS_CR);
  usb_serial_jtag_vfs_set_tx_line_endings(ESP_LINE_ENDINGS_CRLF);

  offsets_help();
  while (true) {
    const int ch = fgetc(stdin);
    if (ch == EOF) {
      vTaskDelay(pdMS_TO_TICKS(20));
      continue;
    }
    feed_console_char(ch);
  }
}
#endif

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

// ---------------------------------------------------------------------------
// USB-MIDI clock source (ESP32-S3 only). MIDI clock is 24 PPQN -- identical to
// the SMS sync rate -- so each Timing Clock byte (0xF8) advances the counter by
// exactly one tick; Start/Continue/Stop drive the transport. The board is a
// composite USB device: a MIDI port plus a CDC serial console on one USB-C.
// ---------------------------------------------------------------------------
#if defined(CONFIG_IDF_TARGET_ESP32S3)
static std::atomic<int32_t> g_midiTick{0};
static std::atomic<bool> g_midiPlaying{false};
static std::atomic<int64_t> g_midiLastClockUs{0};

static long long computeMidiTarget() {
  if (!g_midiPlaying.load()) return -1;  // stopped -> freeze (tracker holds)
  long long t = (long long)g_midiTick.load() + g_offsetTicks.load();
  return t < 0 ? 0 : t;
}

// MIDI clock counts as "active" if the device is mounted and a clock byte
// arrived recently -- used by AUTO mode to prefer MIDI over Link when playing.
static bool midi_active() {
  if (!tud_midi_mounted()) return false;
  return (esp_timer_get_time() - g_midiLastClockUs.load()) < MIDI_ACTIVE_TIMEOUT_US;
}

// React to the System Real-Time bytes that make up MIDI clock/transport.
static void midi_handle_status(uint8_t status) {
  const int64_t now = esp_timer_get_time();
  switch (status) {
    case 0xF8:  // Timing Clock (24 per quarter)
      if (g_midiPlaying.load()) g_midiTick.fetch_add(1);
      g_midiLastClockUs = now;
      break;
    case 0xFA:  // Start: from the top (row 0)
      g_midiTick = 0;
      g_midiPlaying = true;
      g_midiLastClockUs = now;
      break;
    case 0xFB:  // Continue: resume from current position
      g_midiPlaying = true;
      g_midiLastClockUs = now;
      break;
    case 0xFC:  // Stop: freeze
      g_midiPlaying = false;
      break;
    default: break;  // ignore Active Sensing (0xFE), Reset (0xFF), etc.
  }
}

// ---------------------------------------------------------------------------
// MIDI takeover: stream MIDI note events to the console over the two port wires,
// console-clocked, so the tracker plays them live on its own voices with its
// sequencer stopped. Protocol = genmddj's MIDI.md 3 (the console side is built to
// it). Mutually exclusive with the sync counter.
//
// The console is the clock master (idle CLK low; pulses low->high->low, samples
// DAT on the RISING edge). The bridge drives DAT open-drain and changes it on the
// FALLING edge, MSB-first. After an idle gap the bridge presents a leading FLAG
// bit while idle; 1 => a fixed 3-byte frame (status,d1,d2) follows, 0 => queue
// empty. status = type<<4 | channel. Raw MIDI is normalised here into these
// compact frames (running status already expanded by USB-MIDI; NoteOn vel0 ->
// NoteOff; CC 120/123 -> Panic) and CC is coalesced, so the console parser is a
// bounded type switch. Each event on the wire = 1 flag + 24 bits = 25 bits.
// ---------------------------------------------------------------------------

// A ring of normalised events, produced by the USB task and consumed (bit by bit)
// by the CLK ISR + the idle watchdog. One spinlock guards both the queue and the
// shift register (the overflow policy evicts from the tail, and two ISRs plus the
// task all touch the shift state, so SPSC indices aren't enough).
struct midi_evt { uint8_t status, d1, d2; };
static portMUX_TYPE g_fifoMux = portMUX_INITIALIZER_UNLOCKED;
static midi_evt g_evtq[MIDI_EVTQ_SIZE];
static uint16_t g_eHead = 0, g_eTail = 0;   // free-running; index = x & (SIZE-1)
static uint32_t g_sr = 0;                   // shift register, next bit left-justified in bit 31
static int      g_srBits = 0;               // bits left in g_sr (0 => reload on next present)
static int64_t  g_lastClkUs = 0;            // last CLK edge time (for idle-gap detection)
static bool     g_idleArmed = false;        // flag already presented for the current idle gap
static uint32_t g_clkEdges = 0;             // HUD: CLK edges served
static uint32_t g_dropped  = 0;             // HUD: events lost to overflow

static inline uint16_t evtq_count() { return (uint16_t)(g_eHead - g_eTail); }

// Load the next wire frame into the shift register (caller holds g_fifoMux):
// a queued event -> flag 1 + status:d1:d2 (25 bits), else flag 0 (1 bit).
static void IRAM_ATTR load_frame_locked() {
  if (evtq_count() > 0) {
    const midi_evt e = g_evtq[g_eTail++ & (MIDI_EVTQ_SIZE - 1)];
    const uint32_t f = (1u << 24) | ((uint32_t)e.status << 16) |
                       ((uint32_t)e.d1 << 8) | e.d2;
    g_sr = f << (32 - 25);   // MSB (the flag) now sits in bit 31
    g_srBits = 25;
  } else {
    g_sr = 0;                // flag = 0 (empty)
    g_srBits = 1;
  }
}

// Present the next bit on DAT and advance (caller holds g_fifoMux). Reloads a
// fresh frame when the current one is exhausted -- within a burst events run
// back-to-back (…1·s·d1·d2·1·s·d1·d2·…·0), and a fresh burst resyncs via the
// idle watchdog forcing g_srBits = 0 first.
static void IRAM_ATTR present_bit_locked() {
  if (g_srBits == 0) load_frame_locked();
  const int bit = (int)((g_sr >> 31) & 1u);
  g_sr <<= 1;
  g_srBits--;
  gpio_set_level(PIN_MIDI_DAT, bit);   // OD: 1 = release (console pull-up), 0 = low
}

// CLK falling edge: the console has sampled the current bit -> present the next.
static void IRAM_ATTR clk_isr(void*) {
  portENTER_CRITICAL_ISR(&g_fifoMux);
  g_lastClkUs = esp_timer_get_time();
  g_idleArmed = false;
  present_bit_locked();
  g_clkEdges++;
  portEXIT_CRITICAL_ISR(&g_fifoMux);
}

// Called from the presenter timer ISR while in takeover: once CLK has been quiet
// for TAKEOVER_IDLE_US we're between bursts -> resync and present a fresh flag bit
// so it's stable before the console's next rising edge (idle-gap resync).
static void IRAM_ATTR takeover_idle_check() {
  const int64_t now = esp_timer_get_time();
  portENTER_CRITICAL_ISR(&g_fifoMux);
  if (!g_idleArmed && (now - g_lastClkUs) > TAKEOVER_IDLE_US) {
    g_idleArmed = true;
    g_srBits = 0;              // drop any partial frame; reload from the flag
    present_bit_locked();
  }
  portEXIT_CRITICAL_ISR(&g_fifoMux);
}

// Enqueue a normalised event. CC (type 3) coalesces: an unread same-channel,
// same-controller CC has its value replaced in place (a knob sweep collapses to
// one queue slot). A NoteOff is `critical` -> never dropped (evicts the oldest to
// fit, so a release can't be lost); other events drop when the queue is full.
static void evtq_push(uint8_t status, uint8_t d1, uint8_t d2, bool critical) {
  portENTER_CRITICAL(&g_fifoMux);
  if ((status & 0xF0) == (3u << 4)) {
    for (uint16_t i = g_eTail; i != g_eHead; i++) {
      midi_evt& e = g_evtq[i & (MIDI_EVTQ_SIZE - 1)];
      if (e.status == status && e.d1 == d1) { e.d2 = d2; portEXIT_CRITICAL(&g_fifoMux); return; }
    }
  }
  if (evtq_count() >= MIDI_EVTQ_SIZE) {
    if (!critical) { g_dropped++; portEXIT_CRITICAL(&g_fifoMux); return; }
    g_eTail++; g_dropped++;   // evict the oldest so the note-off fits
  }
  midi_evt& e = g_evtq[g_eHead++ & (MIDI_EVTQ_SIZE - 1)];
  e.status = status; e.d1 = d1; e.d2 = d2;
  portEXIT_CRITICAL(&g_fifoMux);
}

// Takeover engagement: AUTO follows recent channel-voice traffic; ON/OFF force it.
enum TakeoverMode { TK_AUTO = 0, TK_ON = 1, TK_OFF = 2 };
static std::atomic<int> g_takeoverMode{TK_AUTO};
static std::atomic<int64_t> g_takeoverLastUs{0};

static bool takeover_active() {
  const int m = g_takeoverMode.load();
  if (m == TK_OFF) return false;
  if (!tud_midi_mounted()) return false;
  if (m == TK_ON) return true;
  return (esp_timer_get_time() - g_takeoverLastUs.load()) < TAKEOVER_TIMEOUT_US;
}

// Reconfigure the two wires for the requested role. Only touches hardware on a
// real transition. Called from the loop task (does gpio_config, not ISR-safe).
static void wire_set_mode(WireMode m) {
  if (m == g_wireMode) return;
  if (m == WIRE_TAKEOVER) {
    gpio_set_level(PIN_MIDI_DAT, 1);           // DAT idle = released high
    portENTER_CRITICAL(&g_fifoMux);
    g_srBits = 0; g_idleArmed = false; g_lastClkUs = esp_timer_get_time();
    present_bit_locked();                      // put a defined (flag) bit on DAT now
    portEXIT_CRITICAL(&g_fifoMux);
    gpio_config_t in = {};
    in.pin_bit_mask = 1ULL << PIN_MIDI_CLK;
    in.mode = GPIO_MODE_INPUT;
    in.pull_up_en = GPIO_PULLUP_DISABLE;
    in.pull_down_en = GPIO_PULLDOWN_ENABLE;    // idle-low CLK (console drives it), defined when unplugged
    in.intr_type = GPIO_INTR_NEGEDGE;          // falling edge = the console has sampled; present the next bit
    ESP_ERROR_CHECK(gpio_config(&in));
    ESP_ERROR_CHECK(gpio_isr_handler_add(PIN_MIDI_CLK, clk_isr, nullptr));
  } else {
    gpio_isr_handler_remove(PIN_MIDI_CLK);
    gpio_config_t od = {};
    od.pin_bit_mask = (1ULL << PIN_TR) | (1ULL << PIN_TH);
    od.mode = GPIO_MODE_OUTPUT_OD;
    od.pull_up_en = GPIO_PULLUP_DISABLE;
    od.pull_down_en = GPIO_PULLDOWN_DISABLE;
    od.intr_type = GPIO_INTR_DISABLE;
    ESP_ERROR_CHECK(gpio_config(&od));
    writeCounter(0);                            // release both lines
    portENTER_CRITICAL(&g_mux);
    g_target = -1;                              // force the presenter to re-snap
    portEXIT_CRITICAL(&g_mux);
  }
  g_wireMode = m;
}

// Normalise a channel-voice USB-MIDI packet into a compact event and enqueue it.
// type<<4|channel per MIDI.md 3.4: 1=NoteOff 2=NoteOn 3=CC 4=PgmChange 5=Bend
// 7=Panic. Drops poly-aftertouch (0xA0) and channel pressure (0xD0).
static void forward_channel_voice(const uint8_t* pkt) {
  const uint8_t s = pkt[1], hi = s & 0xF0, ch = s & 0x0F;
  uint8_t type, d1 = pkt[2], d2 = pkt[3];
  bool critical = false;
  switch (hi) {
    case 0x80:                                                   // NoteOff
      type = 1; critical = true;
      break;
    case 0x90:                                                   // NoteOn (vel 0 -> NoteOff)
      if (d2 == 0) { type = 1; critical = true; }
      else { type = 2; }
      break;
    case 0xB0:                                                   // CC (120/123 -> Panic)
      if (d1 == 120 || d1 == 123) { type = 7; d1 = 0; d2 = 0; }
      else { type = 3; }
      break;
    case 0xC0:                                                   // Program Change
      type = 4; d2 = 0;
      break;
    case 0xE0:                                                   // Pitch Bend (d1=LSB, d2=MSB)
      type = 5;
      break;
    default:                                                     // drop 0xA0 / 0xD0
      return;
  }
  evtq_push((uint8_t)((type << 4) | ch), d1, d2, critical);
  g_takeoverLastUs = esp_timer_get_time();
}

// Parse the `k` takeover-mode command argument.
static void set_takeover(const char* arg) {
  while (*arg == ' ') arg++;
  int m = TK_AUTO;
  if (!std::strncmp(arg, "on", 2)) m = TK_ON;
  else if (!std::strncmp(arg, "off", 3)) m = TK_OFF;
  g_takeoverMode = m;
  printf(">> MIDI takeover: %s\n", m == TK_ON ? "on" : m == TK_OFF ? "off" : "auto");
}

// Drain inbound USB-MIDI packets, pulling out the real-time status bytes.
static void midi_poll_task(void*) {
  uint8_t pkt[4];
  while (true) {
    bool got = false;
    while (tud_midi_available() && tud_midi_packet_read(pkt)) {
      got = true;
      // Real-time messages are single-byte; TinyUSB delivers the status in
      // byte 1 (CIN 0xF). Scan bytes 1..3 defensively for >=0xF8.
      for (int i = 1; i < 4; i++)
        if (pkt[i] >= 0xF8) midi_handle_status(pkt[i]);
      // Channel-voice: a pitch bend on ch16 (byte 1 is the channel status byte)
      // opens the WiFi setup portal -- a no-serial escape hatch; every other
      // channel-voice message is forwarded to the takeover FIFO.
      if (pkt[1] == MIDI_PORTAL_TRIGGER_STATUS) open_portal_once();
      else forward_channel_voice(pkt);
    }
    if (!got) vTaskDelay(1);  // ~1 ms when idle
  }
}

// --- Composite USB descriptor: CDC (console) + MIDI (clock in) --------------
enum {
  ITF_NUM_CDC = 0, ITF_NUM_CDC_DATA,
  ITF_NUM_MIDI, ITF_NUM_MIDI_STREAMING,
  ITF_NUM_TOTAL
};
#define EPNUM_CDC_NOTIF 0x81
#define EPNUM_CDC_OUT   0x02
#define EPNUM_CDC_IN    0x82
#define EPNUM_MIDI_OUT  0x03
#define EPNUM_MIDI_IN   0x83
#define USB_CFG_TOTAL_LEN (TUD_CONFIG_DESC_LEN + TUD_CDC_DESC_LEN + TUD_MIDI_DESC_LEN)

static const tusb_desc_device_t s_dev_desc = {
  .bLength = sizeof(tusb_desc_device_t),
  .bDescriptorType = TUSB_DESC_DEVICE,
  .bcdUSB = 0x0200,
  .bDeviceClass = TUSB_CLASS_MISC,          // IAD -> composite device
  .bDeviceSubClass = MISC_SUBCLASS_COMMON,
  .bDeviceProtocol = MISC_PROTOCOL_IAD,
  .bMaxPacketSize0 = CFG_TUD_ENDPOINT0_SIZE,
  .idVendor = 0x303A,                       // Espressif
  .idProduct = 0x4007,
  .bcdDevice = 0x0100,
  .iManufacturer = 0x01,
  .iProduct = 0x02,
  .iSerialNumber = 0x03,
  .bNumConfigurations = 0x01,
};

static const uint8_t s_cfg_desc[] = {
  TUD_CONFIG_DESCRIPTOR(1, ITF_NUM_TOTAL, 0, USB_CFG_TOTAL_LEN, 0x00, 100),
  TUD_CDC_DESCRIPTOR(ITF_NUM_CDC, 4, EPNUM_CDC_NOTIF, 8, EPNUM_CDC_OUT, EPNUM_CDC_IN, 64),
  TUD_MIDI_DESCRIPTOR(ITF_NUM_MIDI, 5, EPNUM_MIDI_OUT, EPNUM_MIDI_IN, 64),
};

static const char s_lang_id[] = {0x09, 0x04};  // English (0x0409), as raw bytes
static const char* s_str_desc[] = {
  s_lang_id,               // 0: supported language
  "smsggdj",               // 1: Manufacturer
  "SMSGGDJ Link Bridge",   // 2: Product
  "0001",                  // 3: Serial
  "Bridge Console",        // 4: CDC interface
  "Bridge MIDI Clock",     // 5: MIDI interface
};

static void cdc_rx_cb(int itf, cdcacm_event_t*) {
  uint8_t buf[64];
  size_t n = 0;
  if (tinyusb_cdcacm_read((tinyusb_cdcacm_itf_t)itf, buf, sizeof(buf), &n) == ESP_OK)
    for (size_t i = 0; i < n; i++) feed_console_char(buf[i]);
}

static void usb_init() {
  tinyusb_config_t tcfg = {};
  tcfg.device_descriptor = &s_dev_desc;
  tcfg.string_descriptor = s_str_desc;
  tcfg.string_descriptor_count = sizeof(s_str_desc) / sizeof(s_str_desc[0]);
  tcfg.external_phy = false;
  tcfg.configuration_descriptor = s_cfg_desc;
  ESP_ERROR_CHECK(tinyusb_driver_install(&tcfg));

  tinyusb_config_cdcacm_t acm = {};
  acm.usb_dev = TINYUSB_USBDEV_0;
  acm.cdc_port = TINYUSB_CDC_ACM_0;
  acm.callback_rx = &cdc_rx_cb;
  ESP_ERROR_CHECK(tusb_cdc_acm_init(&acm));
  esp_tusb_init_console(TINYUSB_CDC_ACM_0);  // route printf/logs to USB-CDC
}
#endif  // CONFIG_IDF_TARGET_ESP32S3

// Pick the active source's target and report which source it was (so the loop
// can resync the presenter on a switch). On the C3 it is always Link.
static long long computeActiveTarget(bool& usedMidi) {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
  const int mode = g_sourceMode.load();
  usedMidi = (mode == SRC_MIDI) || (mode == SRC_AUTO && midi_active());
  if (usedMidi) return computeMidiTarget();
#else
  usedMidi = false;
#endif
  return computeTarget();
}

extern "C" void app_main() {
  ESP_ERROR_CHECK(nvs_flash_init());
  offsets_load();  // override compiled defaults with any saved offsets/source

#if defined(CONFIG_IDF_TARGET_ESP32S3)
  usb_init();            // composite USB: MIDI clock in + CDC console
  offsets_help();        // print the command list once over the CDC console
#else
  xTaskCreate(serial_task, "serial", 4096, nullptr, 5, nullptr);
#endif

  gpio_setup();

#if defined(CONFIG_IDF_TARGET_ESP32S3)
  wifi_start_background();  // Link optional; non-blocking, no auto-portal
  ESP_ERROR_CHECK(gpio_install_isr_service(0));  // for the takeover CLK edge ISR
  xTaskCreate(midi_poll_task, "midi", 4096, nullptr, 6, nullptr);
#else
  wifi_connect();           // Link mandatory; blocks / hosts portal
#endif

  static ableton::Link link(120.0);
  link.enableStartStopSync(true);
  link.enable(true);
  g_link = &link;

  presenter_timer_start();
  ESP_LOGI(TAG, "bridge: ready");

  int64_t lastHud = 0;
  int lastSrc = -1;  // 0=Link, 1=MIDI; for presenter resync on a source switch
  while (true) {
#if defined(CONFIG_IDF_TARGET_ESP32S3)
    // Arbitrate the two wires: channel-voice MIDI -> takeover responder, else
    // the Link/MIDI-clock counter. No-ops unless the role actually changes.
    wire_set_mode(takeover_active() ? WIRE_TAKEOVER : WIRE_COUNTER);
#endif
    bool usedMidi = false;
    long long t = computeActiveTarget(usedMidi);

    // Link and MIDI count on unrelated timelines, so on a switch the target can
    // jump backward -- which the monotonic presenter would otherwise read as a
    // stall (it never steps the wire down). Inject one freeze cycle so the
    // presenter re-snaps to the new source instead of waiting for it to catch up.
    const int src = usedMidi ? 1 : 0;
    if (src != lastSrc) {
      lastSrc = src;
      t = -1;
    }

    portENTER_CRITICAL(&g_mux);
    g_target = t;
    portEXIT_CRITICAL(&g_mux);

    const int64_t now = esp_timer_get_time();
    if (now - lastHud > 1000000) {
      lastHud = now;
      auto s = link.captureAppSessionState();
      ESP_LOGI(TAG, "src=%s peers=%u tempo=%.1f playing=%d target=%lld off=%dms/%dt",
               usedMidi ? "midi" : "link", (unsigned)link.numPeers(), s.tempo(),
               (int)s.isPlaying(), t, g_offsetMs.load(), g_offsetTicks.load());
#if defined(CONFIG_IDF_TARGET_ESP32S3)
      if (g_wireMode == WIRE_TAKEOVER || g_takeoverMode.load() != TK_AUTO)
        ESP_LOGI(TAG, "  takeover=%s evtq=%u edges=%lu drop=%lu",
                 (g_wireMode == WIRE_TAKEOVER) ? "ON" : "off", (unsigned)evtq_count(),
                 (unsigned long)g_clkEdges, (unsigned long)g_dropped);
#endif
    }
    vTaskDelay(1);  // ~1 ms
  }
}
