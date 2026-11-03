/**
 * @file adapter_picker.h
 * Sheet that scans for OBD adapters (Bluetooth devices or Wi-Fi networks)
 * and stores the one the user taps as the data source.
 */
#pragma once

#include "../../core/types.h"

namespace ui::adapter_picker {

/** Opens the sheet and starts a scan. */
void open(core::AdapterTransport transport);

}  // namespace ui::adapter_picker
