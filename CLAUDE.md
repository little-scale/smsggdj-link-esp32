# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

ESP32 firmware: an **Ableton Link → SMS hardware-sync bridge**. The board joins
an Ableton Link session over WiFi, derives a tick clock from the shared beat
timeline, and drives a **2-bit tick counter** onto two GPIOs into a Sega Master
System controller port, so the **SMSGGDJ** tracker (`~/Documents/sms_tracker`,
its `SYNC: IN` mode) follows Ableton Live's tempo and transport.

**Two board targets, one source tree:** the **XIAO ESP32-C3** is Link-only. The
**XIAO ESP32-S3** adds a **USB-MIDI clock** source (it has USB-OTG; the C3's USB
is fixed-function USB-Serial/JTAG and can't do USB-MIDI). MIDI clock is 24 PPQN —
identical to the SMS sync rate — so it feeds the exact same presenter/counter as
Link. See "Clock sources" below.

Status: **C3 working on hardware.** The ESP-IDF firmware in `main/` joins Link,
follows tempo/transport, launches on the next bar after start, and drives a real
SMS via `SYNC: IN` in time (latency tuned out with a +75 ms offset). The **S3
USB-MIDI path is verified on hardware**: composite USB (MIDI + CDC console)
enumerates, MIDI clock drives the counter, Link/MIDI auto-arbitration works, and
the WiFi portal opens from a ch16 pitch bend. Only the S3's **GPIO output into a
real SMS** is still unverified (same wiring as the C3). This file is the spec +
decisions + integration notes.

Sibling reference project: `~/Documents/gba-link-sync` (same idea for a GBA).

## The wire contract (this is the whole protocol — get it exact)

SMSGGDJ's `SYNC: IN` reads controller **port 2** once per video frame and
advances its engine by `(counter − last) & 3` ticks. So the bridge presents a
**rolling 2-bit counter** (0→1→2→3→0…), incremented once per tick:

- **TR = port-2 pin 9 = counter bit 0** ← C3 **D1/GPIO3**, S3 **D3/GPIO4**
- **TH = port-2 pin 7 = counter bit 1** ← C3 **D2/GPIO4**, S3 **D4/GPIO5**
- **GND = port-2 pin 8** (shared) ← XIAO **GND**

(The S3 shifts the lines to D3/D4 because its pad D2 = GPIO3 is a strapping pin;
`config.h` picks the pins by `CONFIG_IDF_TARGET_*`. Full tables in `WIRING.md`.)

Rules:
- **1 tick = 1/24 beat (24 PPQN).** 24 ticks = one Link beat = one bar-quarter.
  (Tracker side: 24 ticks = 4 rows at groove 6; as of tracker v0.24, `SYNC IN`
  forces groove 6 so the song stays aligned regardless of its stored groove.)
- **The counter must never run backward** — the SMS decodes deltas mod 4, so a
  backward step reads as +3. A monotonic, rate-limited presenter enforces this.
- **≤ 3 ticks may be applied per SMS frame read** (the 2-bit delta saturates at
  3). At 24 PPQN this is only a limit above ~450 BPM (NTSC), so it's not a
  practical ceiling — but the presenter must respect it.
- **Bar-aligned launch:** start emitting ticks at the first Link bar line (phase
  0 of the 4-beat quantum) at/after transport start, so the tracker's row 0
  lands on Live's beat 1. Counter freezes while Link transport is stopped
  (tracker holds); resumes on the next count change (the tracker arms on WAIT
  and starts on the first change).
- **Jitter-immune by construction** — it's *counted*, not edge-timed. WiFi/host
  jitter just delays the next count; nothing glitches.

## Hardware (decided)

- **Board: Seeed XIAO ESP32-C3** (Link-only target) — single-core RISC-V,
  ~400 KB RAM, 2.4 GHz WiFi; good fit because the sync is jitter-tolerant. The
  **XIAO ESP32-S3** is the second target (dual-core Xtensa, USB-OTG) and the only
  way to get USB-MIDI — see "Clock sources". Dual-core
  **WROOM-32** is the proven-safe fallback (isolate Link's networking from the
  tick timer). WiFi is mandatory (Link = LAN UDP multicast).
- **Pin map (chosen):** TR/bit0 = **D1 (GPIO3)**, TH/bit1 = **D2 (GPIO4)**,
  shared ground = the XIAO **GND** pad. D1/D2 are adjacent non-strapping GPIOs on
  the left rail; GND is on the right rail (ground wire crosses over). **Avoid**
  the C3 strapping pins for these outputs: D0/GPIO2, D8/GPIO8, D9/GPIO9 (GPIO9 =
  boot-mode) — driving them at boot can brick the flash sequence. Full pin tables
  and the DE-9 pinout are in **`WIRING.md`**.
- **Output: open-drain** on the two signal GPIOs (`GPIO_MODE_OUTPUT_OD`;
  `gpio_set_level` 1 = release/high-Z, 0 = drive low). The ESP32 (3.3 V) only
  pulls the lines **low**; the SMS's internal pull-ups supply the 5 V high. No
  level shifter needed.
- **Power: USB (own supply); share only GND with port 2.** Do **not** tap the
  port's +5 V (pin 5) — WiFi current peaks risk sagging the console's 5 V rail.
- Tick edges should come from a **hardware timer ISR** (toggling the two GPIOs),
  independent of the WiFi/main loop.

## Architecture (implemented in `main/main.cpp`)

Ported from the ares emulator fork (see References):
- Join Link (`ableton::Link`), `enableStartStopSync(true)`, `enable(true)`.
- Per scheduling instant, `target = floor(beatsSinceLaunch × 24)` (see
  `computeTarget()`). No frame-PLL — that was emulator-only (vsync pacing).
- A **monotonic presenter** in the `gptimer` ISR (`onPresenterTimer`): advance
  `presented` toward `target`, never backward, ≤ 3 per SMS frame; output
  `presented & 3` on TR/TH (open-drain `gpio_set_level`).
- `beatsSinceLaunch` uses the captured session state + `phaseAtTime` to find the
  bar line; negative = armed (pre-bar) and freezes the counter.
- The 64-bit `target` is shared loop→ISR via a `portMUX` spinlock (a 64-bit
  `std::atomic` is not lock-free / ISR-safe on the single-core rv32imc C3).
- **User offsets** (signed ms + signed ticks) for by-ear latency alignment:
  `+ms` samples Link time later → ticks emitted earlier. Live-tunable over the
  USB serial console (`serial_task`/`handle_command`), persisted in NVS
  (namespace `bridge`), with compiled fallbacks `DEFAULT_OFFSET_*` in config.h
  (currently +75 ms, tuned by ear against a real SMS). Changing an offset stays
  monotonic for free — the presenter only ever steps the wire forward, so
  lowering an offset just freezes until time catches up.

## WiFi provisioning & shipping a binary

The firmware is built to be **distributed as a prebuilt binary** — a user flashes
it and sets WiFi themselves, no toolchain, no compile-time creds. All in the WiFi
section of `main/main.cpp`:

- **Credential precedence** (`wifi_connect()`): NVS namespace `wifi`
  (`wifi_creds_load`/`save`/`clear`, keys `ssid`/`pass`) → *dev only* compile-time
  `secrets.h` → none. With creds it joins as a station with a timeout
  (`STA_CONNECT_TIMEOUT_MS`); on success it returns, on timeout (wrong pass / AP
  gone) it falls through to the portal.
- **SoftAP captive portal** (`start_captive_portal()`): brings up an open AP
  `SMSGGDJ-setup-XXXX` (MAC suffix), a wildcard `esp_http_server` serving a setup
  form (`/` lists scanned SSIDs, `POST /save`), and a tiny **DNS-hijack task**
  (`dns_hijack_task`, UDP :53 → 192.168.4.1) so phones auto-pop the portal.
  `POST /save` writes creds to NVS and **reboots** — so the single STA path
  handles the connect on next boot (no live error branch).
- **`secrets.h` is now optional** — included via `__has_include`, gated by
  `HAVE_SECRETS`. It's a developer fast-path only; without it (or in a release
  build) the board provisions over the portal.
- **`-DSHIP=1`** (`main/CMakeLists.txt`) defines `BRIDGE_PROVISIONING`, which
  forces the portal even when `secrets.h` is present — so a maintainer's creds
  never leak into a distributed binary.
- **Serial `w` command** (`handle_command`): `wifi_creds_clear()` + reboot →
  back into the portal, to change networks without reflashing.
- **No Bluetooth** — SoftAP avoids the BT stack. The only new dependency is
  `esp_http_server`; the DNS responder is raw lwip sockets.

**Releasing:** `tools/make-release.sh` clean-builds with `-DSHIP=1`, runs
`idf.py merge-bin -f raw` into one image flashable at `0x0`
(`dist/smsggdj-bridge-esp32c3.bin`), copies it to `docs/firmware/`, and stamps
`docs/manifest.json`. The web flasher lives in `docs/` (esp-web-tools, served via
GitHub Pages from `/docs`); the merged `.bin` under `docs/firmware/` is a
deliberate `.gitignore` exception so Pages can serve it. **License:** Ableton
Link is GPL v2-or-later, so the binary is too — keep the repo public and linked
from any release to satisfy the source-offer obligation.

## Clock sources (C3 = Link; S3 = Link + USB-MIDI)

The presenter is source-agnostic — it only chases `g_target`. What computes that
target is selectable on the S3 (all in `main/main.cpp`):

- **`computeTarget()`** — Ableton Link (unchanged; both targets).
- **`computeMidiTarget()`** (S3) — USB-MIDI. `midi_poll_task` drains TinyUSB
  packets and `midi_handle_status()` acts on System Real-Time bytes: `0xF8`
  clock → `g_midiTick++` (1 tick = 1/24 beat, a 1:1 map), `0xFA` start → reset to
  0 + play, `0xFB` continue, `0xFC` stop → freeze. Target = `g_midiTick +
  offsetTicks`.
- **Arbiter** `computeActiveTarget()`: mode `SRC_AUTO` uses MIDI when
  `midi_active()` (mounted + a clock byte within `MIDI_ACTIVE_TIMEOUT_US`), else
  Link; `SRC_LINK`/`SRC_MIDI` force one. Set with the serial **`c`** command,
  persisted in NVS (`bridge/src`).
- **Offsets:** the tick offset (`t`) applies to both sources. The **ms offset
  (`m`) is Link-only** — MIDI clock is reactive (can't be nudged *earlier* in
  time without tempo prediction); documented, not a bug.

**S3 USB is a composite device** (`usb_init()`): a hand-rolled descriptor with
**CDC (console) + MIDI (clock in)** on one USB-C — esp_tinyusb's auto-descriptor
covers CDC/MSC/NET but *not* MIDI, hence the custom `s_cfg_desc`/`s_dev_desc`
(IAD device class). The C3 keeps its USB-Serial/JTAG console; the S3 routes the
console to USB-CDC via `esp_tusb_init_console` (CDC writes are non-blocking, so a
disconnected host never stalls the loop) with input through the `cdc_rx_cb`. On
the S3, WiFi is **optional/non-blocking** (`wifi_start_background`, no auto-portal
— a MIDI-only user need never set WiFi); the portal is opened on demand by either
the serial `w` command **or a USB-MIDI pitch bend on channel 16**
(`MIDI_PORTAL_TRIGGER_STATUS`, handled in `midi_poll_task` → `open_portal_once()`,
one-shot) — so a user with no serial console can still set up WiFi.

**Build:** `idf.py set-target esp32s3` then build; `espressif/esp_tinyusb` is
fetched only for the S3 (target rule in `main/idf_component.yml`), and TinyUSB
MIDI+CDC are enabled in `sdkconfig.defaults.esp32s3`. C3 code excludes all of
this via `#if defined(CONFIG_IDF_TARGET_ESP32S3)`.

## Gotchas (learned the hard way)

- **Ableton "Start Stop Sync"** is a separate toggle from "Link". If it's off,
  `isPlaying()` is always false → the bridge sees `playing=0` and never launches.
- Opening the USB-Serial/JTAG port pulses DTR/RTS and **resets the C3**. Use one
  persistent `idf.py monitor` session for live tuning; one-shot opens reboot the
  board (which then reloads the NVS/compiled default).
- Console output needs `CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG=y` — the C3's only
  USB is the USB-Serial/JTAG peripheral, not a UART bridge. **On the S3 this is
  forced off** (`sdkconfig.defaults.esp32s3`): TinyUSB owns the USB-OTG PHY, so
  the console is USB-CDC (runtime) with early-boot logs on UART0.

## Toolchain (decided: ESP-IDF)

**ESP-IDF v5.5.** Arduino-ESP32 was attempted first but dead-ends: Ableton Link's
networking needs `espressif/asio` (an ESP32-patched asio with lwip shims),
distributed only as an IDF managed component — vanilla asio will not compile for
ESP32. IDF's component manager fetches it automatically. Install IDF once
(`~/esp/esp-idf`, `install.sh esp32c3,esp32s3` — C3 toolchain is RISC-V, S3 is
Xtensa); each shell needs `. ~/esp/esp-idf/export.sh`.

Build/flash:

```
. ~/esp/esp-idf/export.sh            # per shell
git submodule update --init lib/link # Ableton Link (header-only)
# secrets.h is OPTIONAL now (dev fast-path); without it the board provisions
# WiFi over a SoftAP captive portal. See "WiFi provisioning" above.

idf.py set-target esp32c3            # or esp32s3 (adds USB-MIDI). once per checkout.
idf.py build                         # dev build (uses secrets.h if present)
idf.py -DSHIP=1 build                # release build (forces the portal)
idf.py -p /dev/cu.usbmodem* flash monitor
tools/make-release.sh                # both targets -> dist/ + docs/firmware/
```

Switching `set-target` between C3/S3 reconfigures everything; `make-release.sh`
does `rm -rf build` between targets to avoid a stale-cache "refusing to delete".

Integration specifics worth knowing (all in `main/CMakeLists.txt` /
`sdkconfig.defaults` / top of `main.cpp`):
- `espressif/asio` is pulled via `main/idf_component.yml`; we use it instead of
  Link's bundled asio (the submodule's `modules/asio-standalone` is unused —
  that's why `lib/link` is added non-recursively).
- `CONFIG_COMPILER_CXX_EXCEPTIONS=y` (Link needs exceptions) and
  `CONFIG_PARTITION_TABLE_SINGLE_APP_LARGE=y` (Link + asio + WiFi + the captive
  portal are large — the ship build is ~1.12 MB, ~27% free in the partition).
- `esp_http_server` is in `REQUIRES` (captive portal); `if(SHIP)` adds the
  `BRIDGE_PROVISIONING` compile definition. No `sdkconfig` change was needed for
  SoftAP/httpd.
- `-fexceptions` + `LINK_ESP_TASK_CORE_ID` set on the main component.
- We define `__atomic_is_lock_free` ourselves: IDF only provides it for RISC-V
  *with* the atomic extension, but the C3 is rv32imc (none) — Link's TripleBuffer
  references it via an assert.

### History: Arduino-ESP32 (abandoned — do not use)

The project started on **Arduino-ESP32** (faster to first blink) but abandoned
it: see the ESP-IDF section above — Link's networking needs `espressif/asio`,
which only the IDF component manager can provide. There is no Arduino sketch and
`arduino-cli` is not part of this project's workflow. Mentioned only so a future
instance doesn't reconsider the dead end.

## References (read these before writing code)

- **`~/Documents/ares-link-sync/ares/ares/ms/controller/link-sync/link-session.cpp`**
  — the reference Link→counter implementation (target/presenter/offsets/PLL).
  `link-sync.cpp` shows the pin mapping on the emulator side.
- **`~/Documents/sms_tracker/HARDWARE.md`** — SMS controller port 2 pinout +
  register map (the slave side: port `$DD` bits, the counter protocol).
- **`~/Documents/sms_tracker/DESIGN.md` §11** — the sync design (OUT/PULSE/IN).
- **`~/Documents/ares-link-sync/link/`** — the Ableton Link SDK.

## Invariants

- The counter **never runs backward** and applies **≤3 ticks per SMS frame**.
- The bridge only ever **pulls the wire low** (open-drain); never drives 5 V.
- 24 PPQN is fixed — it matches the tracker's groove-6 lock.
