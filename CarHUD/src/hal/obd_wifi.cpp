/**
 * @file obd_wifi.cpp
 * ELM327 over Wi-Fi: the adapter is an access point with a TCP server.
 * Without a selected network, the strongest open network named like an OBD
 * adapter ("WiFi_OBDII", "V-LINK", ...) is joined and remembered.
 */
#include <WiFi.h>
#include <string.h>

#include "../core/adapter_list.h"
#include "../core/log.h"
#include "../core/settings.h"
#include "obd_adapter.h"
#include "obd_internal.h"

namespace hal::obd {
namespace {

using State = core::ObdTransport::State;
using detail::Op;

constexpr const char* kWorkerName = "obd_wifi";
constexpr uint16_t kPort = 35000;
constexpr uint32_t kJoinTimeoutMs = 15000;
constexpr int32_t kTcpTimeoutMs = 3000;
const IPAddress kDefaultAddress(192, 168, 0, 10);  // most ELM327 Wi-Fi clones

NetworkClient g_client;
detail::Worker g_worker;
detail::Link g_link;
char g_ssid[core::kAdapterNameLen] = {};  // written by open() before the connect command is queued
std::atomic<bool> g_used{false};
std::atomic<bool> g_learned{false};        // g_ssid was found by the automatic search and works

void station_mode() {
  if (WiFi.getMode() != WIFI_STA) WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);  // power save adds 100+ ms latency per OBD request
}

bool join(uint32_t generation) {
  detail::set_step(detail::Step::WifiJoin);
  if (WiFi.status() == WL_CONNECTED && WiFi.SSID() == g_ssid) return true;
  LOG_I("WIFI", "joining \"%s\"", g_ssid);
  WiFi.disconnect();
  WiFi.begin(g_ssid);  // ELM327 adapters use open networks
  const uint32_t start = millis();
  while (millis() - start < kJoinTimeoutMs) {
    if (WiFi.status() == WL_CONNECTED) return true;
    if (generation != g_link.generation.load()) return false;
    vTaskDelay(pdMS_TO_TICKS(100));
  }
  return false;
}

/** Automatic search: the strongest open network named like an OBD adapter. */
bool find_network(char* ssid) {
  detail::set_step(detail::Step::WifiScan);
  const int16_t count = WiFi.scanNetworks(false, false);
  int best = -1;
  for (int16_t i = 0; i < count; ++i) {
    if (WiFi.encryptionType(i) != WIFI_AUTH_OPEN || !core::adapters::looks_like_obd(WiFi.SSID(i).c_str())) continue;
    if (best < 0 || WiFi.RSSI(i) > WiFi.RSSI(best)) best = i;
  }
  if (best >= 0) snprintf(ssid, core::kAdapterNameLen, "%s", WiFi.SSID(best).c_str());
  WiFi.scanDelete();
  if (best < 0) LOG_W("WIFI", "no open network named like an OBD adapter; select it in Settings > Connection");
  return best >= 0;
}

void connect(uint32_t generation) {
  if (generation != g_link.generation.load()) return;
  station_mode();
  const bool search = g_ssid[0] == '\0';
  if (search && !find_network(g_ssid)) {
    g_link.finish(generation, false);
    return;
  }
  bool ok = join(generation);
  if (ok) {
    g_client.stop();
    // The adapter is normally the network's gateway (and DHCP server).
    const IPAddress gateway = WiFi.gatewayIP();
    LOG_I("WIFI", "connecting to %s:%u", gateway.toString().c_str(), kPort);
    detail::set_step(detail::Step::WifiConnect);
    ok = g_client.connect(gateway, kPort, kTcpTimeoutMs) != 0;
    if (!ok && gateway != kDefaultAddress) ok = g_client.connect(kDefaultAddress, kPort, kTcpTimeoutMs) != 0;
    if (ok) g_client.setNoDelay(true);
  }
  LOG_I("WIFI", "connect %s", ok ? "ok" : "failed");
  if (search) {
    if (ok) {
      g_learned.store(true);
    } else {
      g_ssid[0] = '\0';  // search again next time
    }
  }
  if (!g_link.finish(generation, ok) && ok) g_client.stop();
}

void scan() {
  detail::set_step(detail::Step::WifiScan);
  station_mode();
  const int16_t count = WiFi.scanNetworks(false, false);  // blocking, ~2 s
  for (int16_t i = 0; i < count; ++i) {
    platform::AdapterInfo info{};
    strncpy(info.name, WiFi.SSID(i).c_str(), sizeof(info.name) - 1);
    if (info.name[0] == '\0') continue;  // hidden network
    info.rssi = static_cast<int8_t>(WiFi.RSSI(i));
    info.secured = WiFi.encryptionType(i) != WIFI_AUTH_OPEN;
    info.likely_obd = core::adapters::looks_like_obd(info.name);
    detail::results_add(info, false);
  }
  WiFi.scanDelete();
  detail::set_scanning(false);
}

void handle(const detail::Command& cmd) {
  switch (cmd.op) {
    case Op::Connect: connect(cmd.generation); break;
    case Op::Disconnect: g_client.stop(); break;
    case Op::Scan: scan(); break;
    case Op::PowerDown:
      g_client.stop();
      WiFi.disconnect(true);
      WiFi.mode(WIFI_OFF);
      break;
  }
}

void post(Op op, uint32_t generation) {
  g_used.store(true);
  g_worker.post(kWorkerName, handle, op, generation);
}

class WifiTransport final : public core::ObdTransport {
 public:
  core::AdapterTransport kind() const override { return core::AdapterTransport::Wifi; }

  // Without a selected network the transport searches for one itself.
  bool configured() const override { return true; }

  void open() override {
    memcpy(g_ssid, core::settings::get().wifi_ssid, sizeof(g_ssid));
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
    if (g_learned.exchange(false)) {
      // Main loop only: settings are not thread safe.
      snprintf(core::settings::edit().wifi_ssid, core::kAdapterNameLen, "%s", g_ssid);
      LOG_I("WIFI", "adapter network %s remembered", g_ssid);
      core::settings::commit(core::settings::kChangeAdapter);
    }
    if (g_link.get() == State::Connected && (WiFi.status() != WL_CONNECTED || !g_client.connected())) {
      g_link.set(State::Failed);
    }
    return g_link.get();
  }

  size_t read(uint8_t* buf, size_t len) override {
    if (g_link.get() != State::Connected) return 0;
    const int available = g_client.available();
    if (available <= 0) return 0;
    const int n = g_client.read(buf, static_cast<size_t>(available) < len ? available : len);
    return n > 0 ? static_cast<size_t>(n) : 0;
  }

  void write(const char* text) override {
    if (g_link.get() == State::Connected) g_client.print(text);
  }
};

WifiTransport g_transport;

}  // namespace

core::ObdTransport& wifi() { return g_transport; }

namespace detail {

void wifi_scan_start() { post(Op::Scan, g_link.generation.load()); }

void wifi_power_down() {
  if (!g_used.load()) return;
  g_transport.close();
  post(Op::PowerDown, g_link.generation.load());
}

}  // namespace detail
}  // namespace hal::obd
