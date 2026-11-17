/**
 * @file obd_adapter.h
 * Radio transports to ELM327 adapters: Bluetooth Classic (SPP) and Wi-Fi (TCP).
 *
 * Connecting and scanning block inside the ESP32 libraries for seconds, so
 * each transport runs them in its own worker task; the main loop only polls
 * state() and moves bytes. Only one radio is powered at a time (use()).
 *
 * Bluetooth: connects by device address, legacy PIN 1234 (then 0000).
 * Wi-Fi: joins the adapter's open network, then TCP to the gateway address
 *        (fallback 192.168.0.10) on port 35000.
 */
#pragma once

#include <stdint.h>

#include "../core/obd_transport.h"
#include "../core/platform.h"

namespace hal::obd {

core::ObdTransport& bluetooth();

/**
 * Starts the Bluetooth stack synchronously. Called in setup() before the UI
 * allocates its memory: the stack needs a large share of the heap.
 */
bool start_bluetooth();
bool bluetooth_started();
core::ObdTransport& wifi();

/**
 * True if the previous run ended in a crash, watchdog or brownout reset while a
 * Bluetooth / Wi-Fi operation was in progress. Call once at start-up.
 */
bool crashed_last_boot();

/** "<step>, <heap> KB free" of that reset, or "" (valid after crashed_last_boot()). */
const char* crash_report();

/** Selects the transport in use and powers the other radio down. */
void use(core::AdapterTransport transport);

/** Powers both radios down (demo mode). */
void power_down();

bool scan_start(core::AdapterTransport transport);
bool scan_running();
uint8_t scan_results(platform::AdapterInfo* out, uint8_t max);

}  // namespace hal::obd
