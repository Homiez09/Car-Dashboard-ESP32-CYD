/**
 * @file adapter_list.h
 * Helpers for the list of OBD adapters found by a Bluetooth / Wi-Fi scan.
 */
#pragma once

#include <stdint.h>

#include "platform.h"

namespace core::adapters {

/** True if a device name / SSID looks like an OBD adapter (OBD, ELM, V-LINK, ...). */
bool looks_like_obd(const char* name);

/**
 * Adds a scan result or updates the entry with the same address (Bluetooth)
 * or name (Wi-Fi). Returns the new count.
 */
uint8_t add(platform::AdapterInfo* list, uint8_t count, uint8_t max, const platform::AdapterInfo& item,
            bool match_address);

/** Likely OBD adapters first, then by signal strength. */
void sort(platform::AdapterInfo* list, uint8_t count);

}  // namespace core::adapters
