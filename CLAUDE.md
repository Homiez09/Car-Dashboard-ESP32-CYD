# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## Project

CarCYD is Arduino/C++17 firmware for the ESP32-2432S028R "Cheap Yellow Display" (320×240 touch): an LVGL 9 car dashboard (gauge cluster, live OBD-II data, DTC reader, trip computer, alerts, settings). The sketch is `CarCYD/CarCYD.ino`, which only calls `app::setup()` / `app::loop()`. Dependencies: esp32 Arduino core 3.x, lvgl 9.6+, LovyanGFX 1.2+. The firmware reads the car only through an ELM327 OBD-II adapter over Bluetooth Classic / Wi-Fi (`core/elm327_link.cpp` + `hal/obd_*.cpp`); there is no demo mode on the device. The simulated car (`core/sim_link.cpp`, `Settings::demo_mode`) is used by the PC simulator only.

## Commands

```bash
# Firmware build (must stay warning-free; CI builds all three panel variants).
# PartitionScheme=huge_app is required: BT + Wi-Fi stacks make the image ~2.1 MB ("Sketch too big" otherwise).
FQBN=esp32:esp32:esp32:PartitionScheme=huge_app
arduino-cli compile --fqbn $FQBN --warnings all CarCYD
arduino-cli compile --fqbn $FQBN --warnings all \
  --build-property compiler.cpp.extra_flags=-DCYD_PANEL=3 CarCYD   # 1=ILI9341, 2=ILI9341_2 (default), 3=ST7789
arduino-cli upload --fqbn $FQBN -p <port> CarCYD

# Flashable images go to ./build (the user flashes from there, not from the arduino-cli cache).
# The merged image (bootloader + partitions + app, flash at 0x0) is renamed per panel variant:
arduino-cli compile --fqbn $FQBN --warnings all --output-dir build CarCYD
mv build/CarCYD.ino.merged.bin build/CarCYD-<version>-CYD2USB-ILI9341.bin   # CYD_PANEL=2 (default)
rm -f build/CarCYD.ino.elf build/CarCYD.ino.map

# Headless PC simulator (runs core/ + ui/ with the same lv_conf.h and fonts). Doubles as the test suite:
# it drives an emulated ELM327 (tools/simulator/sim_elm.cpp) and prints [sim] PASS/FAIL; any FAIL -> exit 1
python tools/simulator/build.py --zig /path/to/zig            # screenshots -> docs/screenshots/en
python tools/simulator/build.py --zig /path/to/zig --lang tr  # Turkish UI
python tools/simulator/build.py --zig /path/to/zig --demo --banner   # docs/media/demo.gif + banner.png
python tools/simulator/build.py --cc gcc --cxx g++            # without zig
#   needs Pillow; LVGL source from --lvgl / LVGL_DIR (default ~/Documents/Arduino/libraries/lvgl)

# Regenerate fonts/icons (needs Node/npx; fetches lv_font_conv 1.5.3)
python tools/fonts/generate_fonts.py
```

There is no separate unit-test framework; verification = warning-free compile + simulator run (`parser_checks()` / `obd_scenario()` in `sim_main.cpp` hold the protocol checks). To extend the simulator tour, add `tap()/swipe()/long_press()/run_until()/snap()` steps to `scenario()` (or `demo_tour()`) in `tools/simulator/sim_main.cpp`.

## Architecture

Layers under `CarCYD/src/` (see `docs/architecture.md` for the full picture):

- `core/` — platform-independent logic: `VehicleState`, signal metadata/limits, `AlertManager`, `TripComputer`, DTC database, settings, units, i18n, persistence format. **Must not include `Arduino.h`, LVGL, or hardware headers.** Hardware expectations are declared in `core/platform.h`.
- `ui/` — LVGL pages (`ui/pages/`, derived from `ui::Page`), reusable components (`ui/components/`), `theme.h` design tokens, generated fonts. Reaches hardware **only** via `core/platform.h` — that is what lets the same UI code compile in the simulator (`tools/simulator/sim_platform.cpp` is the PC implementation of `platform.h`; `hal/platform_esp32.cpp` is the device one).
- `hal/` — drivers only (LovyanGFX display + XPT2046 touch on software SPI, backlight PWM, LDR, buzzer, RGB LED, BOOT button, NVS, OBD adapter radios). No business rules.
- `app/` — composition root and cooperative main loop; nothing depends on it. `select_data_source()` in `app.cpp` picks `Elm327Link` with the Bluetooth / Wi-Fi transport (or `NullLink` when the radio is paused after 3 failed boots in a row), and powers the unused radio down. Bluetooth is started in `setup()` between the display buffers and the UI (memory); switching to it at runtime restarts the device. With a radio running, `ui::set_keep_pages(false)` keeps only the visible page in memory.
- `config/` — `board_config.h` (pins, `CYD_PANEL`/`CYD_PANEL_INVERT` variant selection), `app_config.h` (version, timing tunables).

Key cross-cutting behaviour:

- **Cooperative main loop.** LVGL isn't thread-safe; only `loop()` touches UI and core state. The sole extra tasks are the radio workers in `hal/obd_*.cpp` (BT/Wi-Fi connect and scan block for seconds) — they communicate via atomics (`detail::Link`, generation counter so a late connect after `close()` is discarded). Nothing in the loop may block: melodies, LED patterns, fades advance via timestamps. Loop order: `link.poll()` → alerts → trip → sounds/LED → backlight → button/buzzer/LED sequencers → `settings.service()` (NVS save 1.5 s after last change) → trip save (60 s) → `lv_timer_handler()`.
- **Data flow:** `DataLink` → `state.set(SignalId, value, now)` → `VehicleState` (timestamped; older than `stale_after_ms` = stale, shown as "--": 2.5 s, 8 s for the ELM327 link) → `AlertManager` / `TripComputer` / visible UI page only. Value colouring and alerts share one `limits()` function in `core/signals.cpp`.
- **DTC ops are async:** request → `DtcState.operation` → result in `last_result` + `revision++`; UI polls `revision`.
- **ELM327 link** (`core/elm327_link.cpp`, portable): talks to an `ObdTransport` (`core/obd_transport.h`), one command in flight, phases adapter → AT init → `0100` probe (retried while ignition off) → supported-PID discovery → priority polling. `VehicleState.adapter_state` (BT/Wi-Fi icon) and `link_state` (car icon) are separate. Response parsers are free functions in `core::elm327` so they can be checked against recorded adapter output. Adapter scans go UI → `platform::adapter_scan_*()` → `hal::obd`.
- **Persistence** (`core/persist.h`): `[magic][version][size][CRC32][payload]`. `Settings` and `TripData` may only be extended by **appending fields** (older shorter blobs load as a prefix); fields are range-checked after load.
- **Language / accent colour change rebuilds the whole UI** via `lv_async_call`, preserving the open page through `save_state/restore_state`.
- **Low-memory page switching** (radio running, `keep_pages == false`): `navigate()` parks the left page in `g.released`; one `lv_async_call(released_async)` deletes its widgets, then the `Page`, and only then builds the new page — two pages in memory at once ran out of heap with Bluetooth and reset the device. Don't build the next page before the old one is freed, and don't split widget/object deletion into separate `lv_async_call`s (LVGL runs the newest timer first). `g.pages[g.current]` can be `nullptr` until then — null-check it.
- **Page memory with a radio running:** a page must fit next to the BT stack on its own. Repeated rows (Live Data: one per signal) use shared static `lv_style_t` instead of `lv_obj_set_style_*` per widget (each local style is a heap allocation per object); measure with the simulator before adding many widgets. The log line `page N built, heap free ...` shows the real number on the device. Labels with `LV_LABEL_LONG_MODE_DOTS` need `lv_label_set_text` (not `_static`). Nothing invisible is built: pages on first visit, sub-views (Live detail, Settings sections, Dashboard views) only while shown — the Dashboard deletes the hidden view first, then builds the next one in an `lv_async_call` (never delete the object an input event is running on).
- **Performance conventions:** only the visible page updates; use `ui::set_text` (redraws only on change); pages are created on first visit, sub-pages deleted on close; no shadows/complex gradients.

## Extension recipes

- **Signal:** `SignalId` in `core/types.h` (enum order = Live Data order) → metadata row (+ `limits()`) in `core/signals.cpp` → names in every language in `core/i18n.h` → icon in `ui/signal_format.cpp`.
- **Page:** derive from `ui::Page`, register in `make_page()` and `PageId` in `ui/ui.cpp`, add a nav-rail icon.
- **Data source:** subclass `core::DataLink` (protocol in `core/`, hardware behind an interface implemented in `hal/` and faked in the simulator), call `state.set(...)` in `poll()`, maintain `state.link_state` (with timeout), handle DTC requests, wire it in `select_data_source()` (and in `sim::on_settings_changed()`).
- **OBD PID:** add `{pid, period_ms}` to `kPoll` in `core/elm327_link.cpp` (bump `Elm327Link::kPollCount`), decode it in `elm327::decode_pid()`, and answer it in `tools/simulator/sim_elm.cpp`.
- **Language:** add `core::Language` value + a column in the `I18N_STRINGS` X-macro (`core/i18n.h`), `i18n::language_name()`, language picker; extend `TEXT_RANGES` in `generate_fonts.py` if new glyphs are needed.
- **Icon:** append to the **end** of `ICONS` in `tools/fonts/generate_fonts.py` (existing code points must not change) and regenerate.

## Rules and conventions

- Never hand-edit `CarCYD/src/ui/fonts/*` — they are generated by `tools/fonts/generate_fonts.py`.
- `CarCYD/lv_conf.h` lives in the sketch folder and is found via `__has_include`; don't move it into the libraries folder.
- No hard-coded colours/sizes in UI — use `ui/theme.h` tokens (see `docs/ui-guidelines.md`: colour signals state only, 40 px min touch targets, steppers over sliders, destructive actions need a confirm dialog).
- Style: `.clang-format` (Google-based, 120 cols). Types `PascalCase`, functions/vars `snake_case`, constants `kPascalCase`, members end with `_`. Public headers start with a `@file` block.
- Commits use Conventional Commits (`feat(ui): ...`, `fix(touch): ...`). Update `CHANGELOG.md` (*Unreleased*) and docs when behaviour changes.
- Firmware needs the **Huge APP** partition scheme (no OTA) — keep the FQBN option in CI, README, CONTRIBUTING in sync.
- Version lives in `config/app_config.h` (`APP_VERSION` string and `APP_VERSION_MAJOR/MINOR/PATCH` — keep in sync).
