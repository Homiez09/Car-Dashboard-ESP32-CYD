# Architecture

## Layers

```
┌─────────────────────────────────────────────────────────────────┐
│ CarCYD.ino  →  app::setup() / app::loop()                        │
├─────────────────────────────────────────────────────────────────┤
│ app/        Composition root: creates the drivers, data source   │
│             and storage and wires them to core and ui            │
├───────────────────────────────┬─────────────────────────────────┤
│ ui/   LVGL user interface     │ hal/  Hardware drivers            │
│  (depends only on core +      │  (Arduino, LovyanGFX, NVS)        │
│   LVGL)                       │                                   │
├───────────────────────────────┴─────────────────────────────────┤
│ core/  Platform independent business logic (no Arduino.h)        │
│        platform.h: everything the code expects from the hardware  │
└─────────────────────────────────────────────────────────────────┘
```

**Rules**
- `core/` includes no hardware or LVGL headers → unit testing and simulation on a PC are possible.
- `ui/` reaches the hardware only through `core/platform.h` (sounds, brightness information, raw touch
  samples, system information, restart). This is why the same UI code runs in `tools/simulator`.
- `hal/` only drives hardware; it contains no business rules.
- `app/` wires everything together; no other layer depends on `app/`.

## Execution model

Everything runs cooperatively in the Arduino `loop()` task. LVGL is not thread safe, so nothing else touches
the UI or the core state. The only other tasks are the radio workers of the OBD adapter transports
(`hal/obd_*.cpp`): Bluetooth / Wi-Fi connects and scans block inside the ESP32 libraries for seconds, so
they run there and hand results back through atomics; the main loop only polls `state()` and moves bytes.
The main loop:

```
link.poll()            data source (ELM327: one OBD request in flight; simulator: simulated car at 20 Hz)
alerts.update()        limit evaluation, debouncing, hysteresis
trip.update()          distance / time / fuel integration, 0-100 timer
alert sounds, LED      new or escalated alerts, shift light
backlight (20 Hz)      LDR filter → target brightness → smooth fade
BOOT button, buzzer and LED sequencers
settings.service()     NVS save 1.5 s after the last change
trip save (60 s)
lv_timer_handler()     UI timer 40 ms (25 Hz) + LVGL refresh (max. 50 Hz)
```

No step blocks: speaker melodies, LED patterns and backlight fades all advance with timestamps.

## Data flow

```
DataLink (SimLink / Elm327Link over Bluetooth or Wi-Fi / NullLink)
   │ set(SignalId, value, now)      DTC read/clear results, link state, clock
   ▼
VehicleState ──► AlertManager ──► banner / sound / LED
   │        └──► TripComputer ──► persistent storage
   ▼
UI pages (only the visible page is updated)
```

- Every signal value carries a timestamp; a value not refreshed for 2.5 s (8 s for the ELM327 link, which
  polls slow-changing PIDs every 3-5 s) is **stale** and shown as "--" (`VehicleState::stale_after_ms`).
- Limits are defined in one place (`core/signals.cpp` → `limits()`): value colouring and alerts use the same
  function.
- DTC operations are asynchronous: `link::request_dtc_read()` → `DtcState.operation` → result in
  `DtcState.last_result` plus an incremented `revision`. The UI watches the revision number.

## The ELM327 link

```
core/elm327_link.cpp   ELM327 protocol (portable, also runs in the simulator)
   │ ObdTransport (core/obd_transport.h): open / close / state / read / write
   ├── hal/obd_bluetooth.cpp   Bluetooth Classic SPP (BluetoothSerial, PIN 1234 then 0000)
   └── hal/obd_wifi.cpp        Wi-Fi station → TCP gateway:35000 (fallback 192.168.0.10)
```

Phases: adapter connect → `ATZ ATE0 ATL0 ATS0 ATH0 ATAT1 ATSP0` → `0100` (protocol search; no answer =
ignition off, retried every 4 s) → `ATDPN`, `0120`, `0140` (supported PIDs) → polling. The scheduler always
sends the most overdue supported PID (rpm / speed every 250 ms, temperatures every 3 s, ...); a PID answering
`NO DATA` three times is dropped until the next connection. `ATRV` supplies the battery voltage, `0101` the
check engine light. Without PID 5E the fuel rate is estimated from MAF. DTC requests run `0101 03 07 0A`
(preceded by `04` to clear) between polls; ISO-TP multi-frame replies are reassembled.

`VehicleState` reports the two connections separately: `adapter_state` (radio link to the adapter, shown by
the Bluetooth / Wi-Fi status icon) and `link_state` (ECU answers, shown by the car icon). Only one radio is
powered at a time (`hal::obd::use()`). The adapter is chosen in *Settings → Connection* from a scan
(`platform::adapter_scan_*()`) and stored in `Settings` (`bt_addr` / `wifi_ssid`).

## Adding a data source

1. Write a class derived from `core::DataLink`. Keep protocol logic in `core/` and put hardware access behind
   an interface implemented in `hal/` (like `ObdTransport`), so the simulator can run it.
2. In `poll()`, decode incoming data and call `state.set(SignalId::Rpm, value, now)`; keep
   `state.link_state` up to date (including a connection timeout).
3. Execute `request_dtc_read/clear()` and write the result into `state.dtc`
   (`operation` → `Idle`, `last_result`, `revision++`).
4. Select it in `app.cpp` → `select_data_source()`.

The UI, the alerts and the trip computer need no changes. Connector CN1 (IO22 / IO27) is free for a UART.

## Adding a signal

1. `core/types.h` → add it to `SignalId` (the order is the order of the Live Data list).
2. `core/signals.cpp` → metadata row (and a `limits()` entry if needed).
3. `core/i18n.h` → full and short name in every language.
4. `ui/signal_format.cpp` → icon.
The signal then automatically appears in Live Data, the value picker and the chart view.

## Adding a page

Derive from `ui::Page` (`create`, `title`, `update`, optionally `on_back`, `save_state/restore_state`), add it to
`make_page()` and `PageId` in `ui.cpp` and give it an icon on the navigation rail.

## Persistent data

| Key | Content | Written |
|---|---|---|
| `settings` | `core::Settings` | 1.5 s after the last change and before a restart |
| `touch_cal` | affine correction + display orientation id | at the end of a calibration |
| `trip` | `core::TripData` | every 60 s while driving, on reset and before a restart |

Format: `[magic][version][size][CRC32][payload]` (`core/persist.h`). Structures are extended **by appending
fields only**; a shorter blob written by older firmware is loaded as a prefix and new fields keep their
defaults. Corrupt data (CRC) falls back to the defaults. Every settings field is range checked after loading.

## Memory and performance

| Resource | Value |
|---|---|
| Flash (firmware) | ≈ 2.1 MB of 3 MB ("Huge APP" partition scheme; Bluetooth + Wi-Fi stacks ≈ 1.35 MB, no OTA) |
| Static RAM | ≈ 69 KB |
| LVGL draw buffers | 2 × 320×40×2 B = 50 KB, DMA capable internal RAM |
| LVGL objects | pages are created on first visit, sub-pages are deleted when closed |
| Display transfer | `RGB565_SWAPPED` → DMA without conversion; LVGL renders into one buffer while the other is sent |

Techniques: only the visible page is updated; labels are redrawn only when their text changes
(`ui::set_text`); the ring gauge invalidates only the changed arc segment; no shadows or complex gradients.

## Localization

The `I18N_STRINGS` X-macro in `core/i18n.h` defines every string once, in all languages, so the tables stay in
sync at compile time. The fonts contain the required glyphs (see `tools/fonts`). Adding a language means adding a
column to the macro and a `Language` value.

## Rebuilding the UI

When the language or the accent colour changes, the whole UI is rebuilt at a safe point (`lv_async_call`); the
open page and sub-page are preserved (`save_state/restore_state`).
