/**
 * @file obd_internal.h
 * Shared plumbing of the OBD adapter transports (not part of the HAL API).
 */
#pragma once

#include <Arduino.h>

#include <atomic>

#include "../core/obd_transport.h"
#include "../core/platform.h"

namespace hal::obd::detail {

enum class Op : uint8_t { Connect, Disconnect, Scan, PowerDown };

struct Command {
  Op op;
  uint32_t generation;  ///< connect commands older than the current generation are stale
};

using Handler = void (*)(const Command& cmd);

/** Background task that executes blocking radio operations in order. */
class Worker {
 public:
  void post(const char* name, Handler handler, Op op, uint32_t generation);

 private:
  static void task(void* arg);
  QueueHandle_t queue_ = nullptr;
  Handler handler_ = nullptr;
};

/**
 * Connection state shared between the main loop and a worker. open()/close()
 * bump the generation, so a connect that finishes after close() is discarded.
 */
struct Link {
  std::atomic<uint8_t> state{static_cast<uint8_t>(core::ObdTransport::State::Closed)};
  std::atomic<uint32_t> generation{0};

  core::ObdTransport::State get() const { return static_cast<core::ObdTransport::State>(state.load()); }
  void set(core::ObdTransport::State s) { state.store(static_cast<uint8_t>(s)); }
  /** Applies the result of a connect only if no open()/close() happened since. */
  bool finish(uint32_t gen, bool ok) {
    if (gen != generation.load()) return false;
    set(ok ? core::ObdTransport::State::Connected : core::ObdTransport::State::Failed);
    return true;
  }
};

/** Radio operation in progress; kept across a reset to report where it happened. */
enum class Step : uint8_t { None = 0, BtStart, BtScan, BtConnect, WifiJoin, WifiScan, WifiConnect };
void set_step(Step step);

/** Marks a radio operation outside a worker (start-up) for crashed_last_boot(). */
void radio_guard(bool busy);

// Scan results (filled by the worker tasks, read by the UI).
void results_clear();
void results_add(const platform::AdapterInfo& item, bool match_address);
void set_scanning(bool scanning);

// Implemented per radio (obd_bluetooth.cpp / obd_wifi.cpp); executed by its worker.
void bt_scan_start();
void wifi_scan_start();
void bt_power_down();
void wifi_power_down();

}  // namespace hal::obd::detail
