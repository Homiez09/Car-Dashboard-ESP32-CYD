# Changelog

All notable changes to this project are documented here. The format follows
[Keep a Changelog](https://keepachangelog.com/) and the project uses [Semantic Versioning](https://semver.org/).

## [Unreleased]

### Added
- Real vehicle data through an **ELM327 OBD-II adapter** over **Bluetooth Classic** (SPP, PIN 1234 / 0000)
  or **Wi-Fi** (open network, TCP port 35000 on the gateway / 192.168.0.10): live mode 01 PIDs, battery
  voltage (ATRV), check engine status, reading (modes 03 / 07 / 0A) and clearing (mode 04) of fault codes.
  Unsupported PIDs are detected (0100 / 0120 / 0140); without PID 5E the fuel rate is estimated from MAF.
- *Settings → Connection*: data source Demo / Bluetooth / Wi-Fi, live adapter, vehicle (ECU) and OBD protocol
  status. The adapter is **found automatically** (strongest device / open network named like an OBD adapter:
  OBD, ELM, V-LINK, Vgate, Veepeak, ...) and remembered once connected; it can also be picked from a scan.
- Bluetooth pairing tries PINs 1234, 0000, 6789, 1111, accepts Secure Simple Pairing, removes a stale pairing
  after a failed round and asks unnamed (BT 2.0) devices for their name.
- *Settings → About*: reason of the last restart (power on, brownout, crash, watchdog, ...).
- Bluetooth is started at boot, before the UI takes its memory; selecting Bluetooth while running saves the
  choice and restarts the device once. A remembered adapter is connected directly (no scan); scans for a new
  one start 3 s after boot and are repeated at most every 30 s.
- If Bluetooth / Wi-Fi resets the device (crash, watchdog, brownout), the next start falls back to demo mode
  and reports the step (BT start / scan / connect, Wi-Fi join / scan / connect) and free memory (*About*).
- Status bar: Bluetooth / Wi-Fi icon for the adapter link and a car icon for the vehicle link — blinking
  while connecting, green when connected, red on an error.
- Simulator: emulated ELM327 adapter and protocol checks (`[sim] PASS/FAIL`, non-zero exit on failure),
  six new screenshots (30-36).

### Removed
- Demo mode on the device: the firmware always reads the car through the ELM327 adapter (Bluetooth or
  Wi-Fi). The simulated car remains in the PC simulator. Settings stored with demo mode switch to Bluetooth.

### Changed
- After an error reset the device keeps its data source and reconnects; only after 3 failed boots in a row
  is the radio paused for that run (tap Bluetooth / Wi-Fi in *Settings → Connection* to retry).
- With Bluetooth / Wi-Fi running only the visible page is kept in memory (pages are rebuilt on each visit);
  Bluetooth uses smaller draw buffers (2 × 10 KB).
- **Partition scheme is now "Huge APP (3MB No OTA/1MB SPIFFS)"** (`PartitionScheme=huge_app`): the
  Bluetooth and Wi-Fi stacks grow the firmware to ≈ 2.1 MB. Stored settings, calibration and trip data
  are kept (same NVS location). OTA updates are not possible with both radios on 4 MB flash.
- Settings format version 2 (adapter selection appended; older settings load unchanged).
- The "ESP32 link" data source was replaced by Bluetooth and Wi-Fi.
- The dashboard builds only its visible view (cluster or engine gauges); a swipe deletes the other view
  before building the next one (about 40 % less memory for the dashboard).

### Fixed
- With Bluetooth running, changing pages restarted the device (out of memory): the next page was built
  while the previous one was still in memory. The old page is now freed first, then the new one is built.
- With Bluetooth running, opening *Live Data* restarted the device (and after 3 times paused the radio): it is
  the largest page (a row per signal). Its rows now share styles instead of per-widget local styles, about
  a quarter less memory. The serial log reports the free heap whenever a page is built.

## [1.1.0] - 2026-10-01

### Changed
- English is the default UI language on a fresh device (Turkish stays available in
  *Settings → System → Language*; stored settings are kept).
- All project documentation is in English.

### Added
- Animated demo (`docs/media/demo.gif`) and hero / social preview image (`docs/media/banner.png`),
  both rendered from the real UI by the simulator (`build.py --demo --banner`).
- GitHub Actions workflow compiling the firmware for all three panel variants.
- Ready-to-flash firmware images for all three board variants attached to each GitHub release.
- Contribution guide, code of conduct, security policy, issue and pull request templates, MIT license.

## [1.0.1] - 2026-09-30

### Fixed
- Mirrored picture with `CYD_PANEL_ILI9341_2` ("CYD2USB" boards): its init sequence clears the GS bit
  (B6h), which is now compensated by the panel rotation offset (landscape MADCTL = MV). The touch
  rotation offset is derived from the panel offset, so touch input keeps working for any orientation.

### Added
- `CYD_PANEL_ROTATION_OFFSET` in `board_config.h` to override the panel orientation (4-7 = mirrored).
- The stored touch calibration is bound to the panel type / orientation; changing them in
  `board_config.h` requests a new calibration automatically at the next start.

## [1.0.0] - 2026-09-30

### Added
- Dashboard with 270° tachometer ring, digital speed, gear / up-shift indicator, red zone and start-up
  needle sweep; four configurable corner values and a second view with six mini gauges.
- Live data list of 18 signals with severity colouring and a detail view (chart, min / avg / max).
- Diagnostics: check engine light status, reading and clearing of DTCs (SAE J2012), descriptions for
  92 common codes in Turkish and English, severity and advice.
- Alert manager (coolant, oil, battery low/high, fuel, speed limit, check engine, link lost) with
  debounce, hysteresis, banner, sound and status LED.
- Trip computer with persistent distance, drive time, speeds, fuel, consumption and 0-100 timer.
- Settings: display (auto brightness, rotation, accent colour), units, gauges, warnings, sound & LED,
  data source, language (TR/EN), touch calibration, restart, factory reset, about.
- Hardware support for the ESP32-2432S028R: DMA display driver, software-SPI touch with calibration,
  PWM backlight with ambient light sensor, speaker tones, RGB LED, BOOT button, NVS storage.
- Demo data source simulating a driving car.
- Desktop simulator producing screenshots of every screen; font/icon generator.

[Unreleased]: https://github.com/muki01/esp32-cyd-car-dashboard/compare/v1.1.0...HEAD
[1.1.0]: https://github.com/muki01/esp32-cyd-car-dashboard/releases/tag/v1.1.0
