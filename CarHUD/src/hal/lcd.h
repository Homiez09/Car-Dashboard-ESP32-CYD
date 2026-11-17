/**
 * LovyanGFX device for LCDWiki ESP32-S3 ES3C28P, ILI9341V 240x320 SPI.
 * FT6336G touch is read separately over I2C.
 */
#pragma once
#define LGFX_USE_V1
#include <LovyanGFX.hpp>
namespace hal {
lgfx::LGFX_Device& lcd();
const char* lcd_panel_name();
uint32_t lcd_orientation_id();
}
