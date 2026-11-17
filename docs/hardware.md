# CarHUD hardware: LCDWiki ESP32-S3 ES3C28P

CarHUD targets the LCDWiki ES3C28P 2.8-inch ESP32-S3 board.

## Main hardware

- MCU: ESP32-S3, dual-core Xtensa LX7
- Flash: 16 MB
- PSRAM: 8 MB OPI
- LCD: 2.8-inch IPS ILI9341V, native 240 x 320, SPI
- Touch: FT6336G capacitive controller, I2C address 0x38
- Audio: ES8311 codec / I2S path (not a GPIO buzzer)
- Status LED: single WS2812-compatible RGB LED on GPIO42
- microSD: SDIO interface

## Display pin map

| Signal | GPIO |
|---|---:|
| LCD CS | 10 |
| LCD DC | 46 |
| LCD SCLK | 12 |
| LCD MOSI | 11 |
| LCD MISO | 13 |
| LCD backlight | 45 |
| LCD reset | Shared with CHIP_PU |

The panel is configured at its native portrait geometry (240 x 320). The application rotates it to landscape (320 x 240).

## FT6336G touch pin map

| Signal | GPIO |
|---|---:|
| I2C SDA | 16 |
| I2C SCL | 15 |
| Reset (active low) | 18 |
| Interrupt (active low) | 17 |
| I2C address | 0x38 |

The FT6336G reports native portrait coordinates. touch.cpp maps these into the application's landscape coordinate space, then applies the persisted affine calibration. A new orientation identifier invalidates calibration saved for the former XPT2046-based board.

## Other interfaces

| Function | GPIO / interface |
|---|---|
| WS2812 RGB LED | GPIO42 (addressable LED; driver not yet enabled) |
| Battery measurement | GPIO9 ADC |
| microSD | SDIO: CLK 38, CMD 40, D0 39, D1 41, D2 48, D3 47 |
| Audio | ES8311 codec / I2S |
| OBD-II Wi-Fi | Wi-Fi STA + TCP, adapter default port 35000 |
| OBD-II Bluetooth | ESP32-S3 BLE GATT requires adapter service/characteristic profile; Bluetooth Classic SPP is not supported |

The board does not provide the CYD XPT2046 resistive touch controller, LDR, three discrete RGB LED pins, or GPIO speaker amplifier input. Those legacy functions are disabled in this board configuration until their ES3C28P-specific drivers are implemented.