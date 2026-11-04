/**
 * @file sim_platform.cpp
 * core/platform.h implemented for the desktop simulator.
 */
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "../../CarCYD/src/core/adapter_list.h"
#include "../../CarCYD/src/core/platform.h"
#include "sim.h"

/** LVGL assertion handler (lv_conf.h): fail loudly in the simulator. */
extern "C" void carcyd_lv_assert_failed(void) {
  printf("[sim] FAIL  LVGL assertion\n");
  abort();
}

namespace platform {

uint32_t millis() { return sim::now(); }

void system_info(SystemInfo& out) {
  out.chip_model = "ESP32-D0WD-V3";
  out.chip_revision = 3;
  out.cores = 2;
  out.cpu_mhz = 240;
  out.flash_bytes = 4 * 1024 * 1024;
  out.heap_free = 187 * 1024;
  out.heap_min_free = 171 * 1024;
  out.heap_total = 320 * 1024;
  out.sdk_version = "sim";
  out.board_name = "ESP32-2432S028R";
  out.panel_name = "ILI9341 320x240";
  out.reset_reason = "Power on";
  out.radio_report = "";
}

void restart() {
  printf("[sim] restart requested\n");
  exit(0);
}

void factory_reset() {
  printf("[sim] factory reset requested\n");
  exit(0);
}

uint16_t light_sensor_raw() { return 412; }
uint8_t ambient_level() { return 64; }
uint8_t backlight_level() { return 85; }

void play(Sound) {}

bool touch_read_raw(TouchSample& out) {
  out.pressed = sim::touch_pressed();
  out.raw_x = static_cast<int16_t>(sim::touch_x());
  out.raw_y = static_cast<int16_t>(sim::touch_y());
  out.pressure = out.pressed ? 1 : 0;
  return true;
}

void touch_apply_calibration(const core::TouchCalibration& cal) {
  printf("[sim] calibration a=%.3f b=%.3f c=%.1f d=%.3f e=%.3f f=%.1f\n", cal.a, cal.b, cal.c, cal.d, cal.e, cal.f);
}

void touch_input_enable(bool enable) { sim::set_touch_input(enable); }

// ---- OBD adapter discovery: a fixed set of nearby devices / networks ---------------------------

namespace {

constexpr uint32_t kScanMs = 1500;
core::AdapterTransport g_scan_transport = core::AdapterTransport::Bluetooth;
uint32_t g_scan_until = 0;
bool g_scanned = false;

AdapterInfo make(const char* name, int8_t rssi, bool secured, uint8_t last) {
  AdapterInfo a{};
  snprintf(a.name, sizeof(a.name), "%s", name);
  const uint8_t address[6] = {0x00, 0x1D, 0xA5, 0x68, 0x98, last};
  memcpy(a.address, address, sizeof(address));
  a.rssi = rssi;
  a.secured = secured;
  a.likely_obd = core::adapters::looks_like_obd(name);
  return a;
}

}  // namespace

bool adapter_scan_start(core::AdapterTransport transport) {
  g_scan_transport = transport;
  g_scan_until = sim::now() + kScanMs;
  g_scanned = true;
  return true;
}

bool adapter_scan_running() { return g_scanned && static_cast<int32_t>(sim::now() - g_scan_until) < 0; }

uint8_t adapter_scan_results(AdapterInfo* out, uint8_t max) {
  if (!g_scanned) return 0;
  AdapterInfo found[4];
  uint8_t n = 0;
  // Devices "appear" during the scan.
  const bool late = !adapter_scan_running();
  if (g_scan_transport == core::AdapterTransport::Bluetooth) {
    found[n++] = make("Galaxy Buds2", -58, false, 0x01);
    found[n++] = make("OBDII", -64, false, 0x02);
    if (late) found[n++] = make("JBL Flip 5", -81, false, 0x03);
  } else {
    found[n++] = make("Home-5G", -52, true, 0x11);
    found[n++] = make("WiFi_OBDII", -61, false, 0x12);
    if (late) found[n++] = make("V-LINK", -77, false, 0x13);
  }
  core::adapters::sort(found, n);
  if (n > max) n = max;
  memcpy(out, found, sizeof(found[0]) * n);
  return n;
}

}  // namespace platform
