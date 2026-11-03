/**
 * @file obd_bluetooth.cpp
 * ELM327 over Bluetooth Classic SPP (BluetoothSerial, master role).
 *
 * ELM327 clones come from many vendors under many names ("OBDII", "OBD2",
 * "V-LINK", "VEEPEAK", ...) and PINs, so nothing has to be configured:
 *  - without a selected adapter, a scan picks the strongest device whose name
 *    looks like an OBD adapter; once connected it is remembered in the settings
 *  - legacy PINs 1234, 0000, 6789, 1111 are tried in turn; Secure Simple
 *    Pairing requests are accepted (we chose the device ourselves)
 *  - when every PIN fails, the old pairing is deleted and, after another full
 *    round, a new scan looks for an adapter again (adapter swapped)
 */
#include <BluetoothSerial.h>
#include <stdlib.h>
#include <string.h>

#include "../core/adapter_list.h"
#include "../core/log.h"
#include "../core/settings.h"
#include "obd_adapter.h"
#include "obd_internal.h"

#if !defined(CONFIG_BT_ENABLED) || !defined(CONFIG_BT_SPP_ENABLED)
#error "Bluetooth Classic (SPP) is not enabled in the ESP32 core. Select the board 'ESP32 Dev Module'."
#endif

// BluetoothSerial is marked deprecated for core 4.x, but it is the only SPP
// client in the 3.x core (ELM327 clones speak Bluetooth Classic only).
#pragma GCC diagnostic ignored "-Wdeprecated-declarations"

namespace hal::obd {
namespace {

using State = core::ObdTransport::State;
using detail::Op;

constexpr const char* kDeviceName = "CarCYD";
constexpr const char* kWorkerName = "obd_bt";
constexpr int kScanMs = 10240;            // 8 inquiry units of 1.28 s
constexpr uint8_t kMaxDevices = 16;
// Scans are paced: the first one waits until the stack has settled, a failed one is not
// repeated right away (a missing adapter must not keep the radio busy).
constexpr uint32_t kFirstScanDelayMs = 3000;
constexpr uint32_t kScanIntervalMs = 30000;
// Older clones (Bluetooth 2.0) do not send their name in the inquiry reply: ask for it.
constexpr uint8_t kMaxNameRequests = 4;
constexpr uint32_t kNameTimeoutMs = 3000;
const char* const kPins[] = {"1234", "0000", "6789", "1111"};
constexpr uint8_t kPinCount = sizeof(kPins) / sizeof(kPins[0]);

BluetoothSerial g_bt;
detail::Worker g_worker;
detail::Link g_link;
std::atomic<bool> g_used{false};  // the worker task exists

// Selected adapter, copied from the settings by open() before the connect command is queued.
uint8_t g_saved[6] = {};
bool g_saved_valid = false;

// Adapter found by the automatic search, handed to the main loop (state()) to be stored.
std::atomic<bool> g_learned{false};
uint8_t g_learned_address[6] = {};
char g_learned_name[core::kAdapterNameLen] = {};

std::atomic<uint32_t> g_passkey{1234};  // SSP passkey answer = the PIN being tried

// Worker-only state (large buffers are static to keep the worker stack small).
platform::AdapterInfo g_devices[kMaxDevices];
char g_remote_name[ESP_BT_GAP_MAX_BDNAME_LEN + 1];
std::atomic<bool> g_started{false};
std::atomic<bool> g_ui_scan{false};  // a scan from the adapter list is waiting  // written by the worker (or setup() before it exists)
uint32_t g_next_scan_ms = 0;
uint8_t g_pin = 0;            // index into kPins; kept while it works
uint8_t g_target[6] = {};
uint8_t g_failures = 0;       // failed connects to g_target

bool ensure_started() {
  if (g_started) return true;
  LOG_I("BT", "starting Bluetooth, heap free %lu B, largest block %lu B",
        static_cast<unsigned long>(ESP.getFreeHeap()), static_cast<unsigned long>(ESP.getMaxAllocHeap()));
  // Newer adapters use Secure Simple Pairing: confirm, and answer a passkey with the current PIN.
  g_bt.onConfirmRequest([](uint32_t) { g_bt.confirmReply(true); });
  g_bt.onKeyRequest([]() { g_bt.respondPasskey(g_passkey.load()); });
  // Master role; BLE memory is released (CarCYD only uses Classic).
  detail::set_step(detail::Step::BtStart);
  if (!g_bt.begin(kDeviceName, true, true)) {
    LOG_E("BT", "Bluetooth start failed");
    return false;
  }
  g_started = true;
  g_next_scan_ms = millis() + kFirstScanDelayMs;
  LOG_I("BT", "Bluetooth started, heap free %lu B", static_cast<unsigned long>(ESP.getFreeHeap()));
  return true;
}

void stop_stack() {
  if (!g_started) return;
  g_bt.disconnect();
  g_bt.end();
  g_started = false;
}

/** Inquiry scan (blocking, ~10 s). Returns the number of devices. */
uint8_t scan_devices(platform::AdapterInfo* out, uint8_t max) {
  LOG_I("BT", "scanning");
  detail::set_step(detail::Step::BtScan);
  BTScanResults* results = g_bt.discover(kScanMs);
  if (results == nullptr) return 0;
  uint8_t count = 0;
  for (int i = 0; i < results->getCount() && count < max; ++i) {
    BTAdvertisedDevice* device = results->getDevice(i);
    platform::AdapterInfo& info = out[count++];
    info = platform::AdapterInfo{};
    if (device->haveName()) strncpy(info.name, device->getName().c_str(), sizeof(info.name) - 1);
    memcpy(info.address, *device->getAddress().getNative(), sizeof(info.address));
    info.rssi = device->haveRSSI() ? device->getRSSI() : -100;
  }
  g_bt.discoverClear();

  uint8_t asked = 0;
  for (uint8_t i = 0; i < count && asked < kMaxNameRequests; ++i) {
    if (out[i].name[0] != '\0') continue;
    ++asked;
    g_bt.invalidateRemoteName();
    g_bt.requestRemoteName(out[i].address);
    for (uint32_t start = millis(); millis() - start < kNameTimeoutMs; vTaskDelay(pdMS_TO_TICKS(50))) {
      if (g_bt.readRemoteName(g_remote_name)) {
        strncpy(out[i].name, g_remote_name, sizeof(out[i].name) - 1);
        break;
      }
    }
    g_bt.invalidateRemoteName();
  }

  for (uint8_t i = 0; i < count; ++i) {
    out[i].likely_obd = core::adapters::looks_like_obd(out[i].name);
    LOG_I("BT", "  %02X:%02X:%02X:%02X:%02X:%02X %-20s %d dBm%s", out[i].address[0], out[i].address[1],
          out[i].address[2], out[i].address[3], out[i].address[4], out[i].address[5],
          out[i].name[0] != '\0' ? out[i].name : "(no name)", out[i].rssi, out[i].likely_obd ? "  <- OBD" : "");
  }
  return count;
}

/** Waits (interruptibly) until a scan is allowed. False if the connect was cancelled. */
bool wait_for_scan_slot(uint32_t generation) {
  while (static_cast<int32_t>(millis() - g_next_scan_ms) < 0) {
    if (generation != g_link.generation.load() || g_ui_scan.load()) return false;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return true;
}

/** Automatic search: the strongest device that looks like an OBD adapter. */
bool find_adapter(uint8_t* address, char* name) {
  platform::AdapterInfo* devices = g_devices;
  const uint8_t count = scan_devices(devices, kMaxDevices);
  int best = -1;
  for (uint8_t i = 0; i < count; ++i) {
    if (devices[i].likely_obd && (best < 0 || devices[i].rssi > devices[best].rssi)) best = i;
  }
  if (best < 0) {
    LOG_W("BT", "%u device(s), none named like an OBD adapter; select it in Settings > Connection", count);
    return false;
  }
  memcpy(address, devices[best].address, 6);
  snprintf(name, core::kAdapterNameLen, "%s", devices[best].name);
  return true;
}

void connect(uint32_t generation) {
  if (generation != g_link.generation.load()) return;  // closed again meanwhile
  if (!ensure_started()) {
    g_link.finish(generation, false);
    return;
  }

  // The selected adapter gets two rounds of PINs (with a fresh pairing in between),
  // then a scan may find another one.
  uint8_t target[6];
  char name[core::kAdapterNameLen] = {};
  bool found = false;
  if (g_saved_valid && g_failures < 2 * kPinCount) {
    memcpy(target, g_saved, sizeof(target));  // known adapter: connect directly, no scan
  } else if (!wait_for_scan_slot(generation)) {
    g_link.finish(generation, false);  // cancelled, or the adapter list wants the radio now
    return;
  } else if (find_adapter(target, name)) {
    found = true;
  } else if (g_saved_valid) {
    memcpy(target, g_saved, sizeof(target));  // nothing found: keep trying the selected one
    g_failures = 0;
  } else {
    g_next_scan_ms = millis() + kScanIntervalMs;
    g_link.finish(generation, false);
    return;
  }
  if (!g_saved_valid || found) g_next_scan_ms = millis() + kScanIntervalMs;
  if (generation != g_link.generation.load()) return;

  if (memcmp(target, g_target, sizeof(target)) != 0) {
    memcpy(g_target, target, sizeof(target));
    g_failures = 0;
  }
  const char* pin = kPins[g_pin];
  g_bt.setPin(pin, static_cast<uint8_t>(strlen(pin)));
  g_passkey.store(static_cast<uint32_t>(strtoul(pin, nullptr, 10)));
  LOG_I("BT", "connecting to %02X:%02X:%02X:%02X:%02X:%02X (PIN %s)", target[0], target[1], target[2], target[3],
        target[4], target[5], pin);
  detail::set_step(detail::Step::BtConnect);
  const bool ok = g_bt.connect(target);  // blocks up to ~10 s
  LOG_I("BT", "connect %s", ok ? "ok" : "failed");

  if (ok) {
    g_failures = 0;
    if (found || !g_saved_valid) {
      memcpy(g_learned_address, target, sizeof(g_learned_address));
      memcpy(g_learned_name, name, sizeof(g_learned_name));
      g_learned.store(true);
    }
  } else {
    g_pin = static_cast<uint8_t>((g_pin + 1) % kPinCount);
    ++g_failures;
    // Every PIN failed: a stale pairing key (adapter re-paired with a phone) blocks all of
    // them, so drop it and pair from scratch.
    if (g_failures % kPinCount == 0 && g_bt.deleteBondedDevice(target)) LOG_I("BT", "old pairing removed");
  }
  if (!g_link.finish(generation, ok) && ok) g_bt.disconnect();
}

void scan_for_ui() {
  g_ui_scan.store(false);
  if (!ensure_started()) {
    detail::set_scanning(false);
    return;
  }
  const uint8_t count = scan_devices(g_devices, kMaxDevices);
  for (uint8_t i = 0; i < count; ++i) detail::results_add(g_devices[i], true);
  detail::set_scanning(false);
}

void handle(const detail::Command& cmd) {
  switch (cmd.op) {
    case Op::Connect: connect(cmd.generation); break;
    case Op::Disconnect:
      if (g_started && g_bt.connected(0)) g_bt.disconnect();
      break;
    case Op::Scan: scan_for_ui(); break;
    case Op::PowerDown: stop_stack(); break;
  }
}

void post(Op op, uint32_t generation) {
  g_used.store(true);
  g_worker.post(kWorkerName, handle, op, generation);
}

class BluetoothTransport final : public core::ObdTransport {
 public:
  core::AdapterTransport kind() const override { return core::AdapterTransport::Bluetooth; }

  // Without a selected adapter the transport searches for one itself.
  bool configured() const override { return true; }

  void open() override {
    const core::Settings& s = core::settings::get();
    memcpy(g_saved, s.bt_addr, sizeof(g_saved));
    g_saved_valid = s.bt_addr_valid;
    const uint32_t generation = ++g_link.generation;
    g_link.set(State::Connecting);
    post(Op::Connect, generation);
  }

  void close() override {
    const uint32_t generation = ++g_link.generation;
    const State previous = g_link.get();
    g_link.set(State::Closed);
    if (previous == State::Connected || previous == State::Connecting) post(Op::Disconnect, generation);
  }

  State state() override {
    if (g_learned.exchange(false)) remember_adapter();
    if (g_link.get() == State::Connected && !g_bt.connected(0)) g_link.set(State::Failed);
    return g_link.get();
  }

  size_t read(uint8_t* buf, size_t len) override {
    if (g_link.get() != State::Connected) return 0;
    size_t n = 0;
    while (n < len && g_bt.available() > 0) {
      const int c = g_bt.read();
      if (c < 0) break;
      buf[n++] = static_cast<uint8_t>(c);
    }
    return n;
  }

  void write(const char* text) override {
    if (g_link.get() == State::Connected) g_bt.print(text);
  }

 private:
  /** Stores the adapter found by the automatic search (main loop only: settings are not thread safe). */
  static void remember_adapter() {
    core::Settings& s = core::settings::edit();
    memcpy(s.bt_addr, g_learned_address, sizeof(s.bt_addr));
    s.bt_addr_valid = true;
    if (g_learned_name[0] != '\0') {
      snprintf(s.bt_name, sizeof(s.bt_name), "%s", g_learned_name);
    } else {
      const uint8_t* a = g_learned_address;
      snprintf(s.bt_name, sizeof(s.bt_name), "%02X:%02X:%02X:%02X:%02X:%02X", a[0], a[1], a[2], a[3], a[4], a[5]);
    }
    LOG_I("BT", "adapter %s remembered", s.bt_name);
    core::settings::commit(core::settings::kChangeAdapter);
  }
};

BluetoothTransport g_transport;

}  // namespace

core::ObdTransport& bluetooth() { return g_transport; }

bool bluetooth_started() { return g_started.load(); }

bool start_bluetooth() {
  // Runs in setup() before the worker task exists, so the worker-only state is safe to touch.
  detail::radio_guard(true);
  const bool ok = ensure_started();
  detail::radio_guard(false);
  return ok;
}

namespace detail {

void bt_scan_start() {
  g_ui_scan.store(true);  // a connect waiting for its scan slot gives way
  post(Op::Scan, g_link.generation.load());
}

void bt_power_down() {
  if (!g_used.load()) return;
  g_transport.close();
  post(Op::PowerDown, g_link.generation.load());
}

}  // namespace detail
}  // namespace hal::obd
