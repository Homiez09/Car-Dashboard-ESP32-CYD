#include "obd_adapter.h"

#include "../core/adapter_list.h"
#include <esp_attr.h>
#include <esp_system.h>

#include "../core/log.h"
#include "obd_internal.h"

namespace hal::obd {
namespace detail {
namespace {

constexpr uint8_t kMaxResults = 16;
// Bluetooth stack calls, scanning and logging (large buffers are static, not on this stack).
constexpr uint32_t kWorkerStack = 8192;
constexpr UBaseType_t kWorkerPriority = 1;
constexpr BaseType_t kWorkerCore = 0;  // next to the radio stacks; loop() runs on core 1

// Set while a worker runs a radio operation; RTC memory survives crash, watchdog and
// brownout resets (not power-on), so the next boot can tell the radio reset the device.
constexpr uint32_t kRadioBusy = 0x0BDB7A11;
RTC_NOINIT_ATTR uint32_t g_radio_busy;
RTC_NOINIT_ATTR uint8_t g_radio_step;        // detail::Step at the time of the reset
RTC_NOINIT_ATTR uint32_t g_radio_heap_free;  // heap when that step started
char g_crash_report[48] = "";                // filled by crashed_last_boot()

const char* step_name(uint8_t step) {
  switch (static_cast<Step>(step)) {
    case Step::BtStart: return "BT start";
    case Step::BtScan: return "BT scan";
    case Step::BtConnect: return "BT connect";
    case Step::WifiJoin: return "Wi-Fi join";
    case Step::WifiScan: return "Wi-Fi scan";
    case Step::WifiConnect: return "Wi-Fi connect";
    default: return "radio";
  }
}

SemaphoreHandle_t g_results_lock = nullptr;
platform::AdapterInfo g_results[kMaxResults];
uint8_t g_result_count = 0;
std::atomic<bool> g_scanning{false};

void lock() {
  if (g_results_lock == nullptr) g_results_lock = xSemaphoreCreateMutex();
  xSemaphoreTake(g_results_lock, portMAX_DELAY);
}

void unlock() { xSemaphoreGive(g_results_lock); }

}  // namespace

void Worker::post(const char* name, Handler handler, Op op, uint32_t generation) {
  if (queue_ == nullptr) {
    queue_ = xQueueCreate(8, sizeof(Command));
    handler_ = handler;
    xTaskCreatePinnedToCore(task, name, kWorkerStack, this, kWorkerPriority, nullptr, kWorkerCore);
  }
  const Command cmd{op, generation};
  if (xQueueSend(queue_, &cmd, 0) != pdTRUE) LOG_W("OBD", "%s: command queue full", name);
}

void Worker::task(void* arg) {
  auto* self = static_cast<Worker*>(arg);
  Command cmd;
  while (true) {
    if (xQueueReceive(self->queue_, &cmd, portMAX_DELAY) != pdTRUE) continue;
    g_radio_step = static_cast<uint8_t>(Step::None);
    g_radio_busy = kRadioBusy;  // a reset now is blamed on the radio (see crashed_last_boot())
    self->handler_(cmd);
    g_radio_busy = 0;
  }
}

void radio_guard(bool busy) {
  if (busy) g_radio_step = static_cast<uint8_t>(Step::None);
  g_radio_busy = busy ? kRadioBusy : 0;
}

void set_step(Step step) {
  g_radio_heap_free = ESP.getFreeHeap();
  g_radio_step = static_cast<uint8_t>(step);
}

void results_clear() {
  lock();
  g_result_count = 0;
  unlock();
}

void results_add(const platform::AdapterInfo& item, bool match_address) {
  lock();
  g_result_count = core::adapters::add(g_results, g_result_count, kMaxResults, item, match_address);
  unlock();
}

void set_scanning(bool scanning) { g_scanning.store(scanning); }

}  // namespace detail

bool crashed_last_boot() {
  const bool busy = detail::g_radio_busy == detail::kRadioBusy;
  detail::g_radio_busy = 0;
  switch (esp_reset_reason()) {
    case ESP_RST_PANIC:
    case ESP_RST_INT_WDT:
    case ESP_RST_TASK_WDT:
    case ESP_RST_WDT:
    case ESP_RST_BROWNOUT:
      break;
    default:
      return false;
  }
  if (!busy) return false;
  snprintf(detail::g_crash_report, sizeof(detail::g_crash_report), "%s, %lu KB free",
           detail::step_name(detail::g_radio_step), static_cast<unsigned long>(detail::g_radio_heap_free / 1024));
  LOG_W("OBD", "last reset during %s", detail::g_crash_report);
  return true;
}

const char* crash_report() { return detail::g_crash_report; }

void use(core::AdapterTransport transport) {
  if (transport == core::AdapterTransport::Bluetooth) {
    detail::wifi_power_down();
  } else {
    detail::bt_power_down();
  }
}

void power_down() {
  detail::bt_power_down();
  detail::wifi_power_down();
}

bool scan_start(core::AdapterTransport transport) {
  if (detail::g_scanning.load()) return false;
  detail::results_clear();
  detail::set_scanning(true);
  if (transport == core::AdapterTransport::Bluetooth) {
    detail::bt_scan_start();
  } else {
    detail::wifi_scan_start();
  }
  return true;
}

bool scan_running() { return detail::g_scanning.load(); }

uint8_t scan_results(platform::AdapterInfo* out, uint8_t max) {
  detail::lock();
  core::adapters::sort(detail::g_results, detail::g_result_count);
  const uint8_t n = detail::g_result_count < max ? detail::g_result_count : max;
  for (uint8_t i = 0; i < n; ++i) out[i] = detail::g_results[i];
  detail::unlock();
  return n;
}

}  // namespace hal::obd
