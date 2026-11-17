#include "lcd.h"
#include "../config/board_config.h"
namespace hal {
namespace {
constexpr const char* kPanelName="ILI9341V 240x320";
class Es3c28pDevice final : public lgfx::LGFX_Device {
 public:
  Es3c28pDevice() {
    auto b=bus_.config(); b.spi_host=SPI2_HOST; b.spi_mode=0; b.freq_write=ES3C28P_LCD_SPI_HZ;
    b.freq_read=16000000; b.spi_3wire=false; b.use_lock=true;
    b.pin_sclk=board::kLcdSclk; b.pin_mosi=board::kLcdMosi; b.pin_miso=board::kLcdMiso; b.pin_dc=board::kLcdDc;
    bus_.config(b); panel_.setBus(&bus_);
    auto p=panel_.config(); p.pin_cs=board::kLcdCs; p.pin_rst=board::kLcdRst; p.pin_busy=-1;
    p.panel_width=board::kLcdWidth; p.panel_height=board::kLcdHeight;
    p.memory_width=board::kLcdWidth; p.memory_height=board::kLcdHeight;
    p.offset_x=0; p.offset_y=0; p.offset_rotation=0; p.readable=true; p.invert=true;
    p.rgb_order=false; p.dlen_16bit=false; p.bus_shared=false; panel_.config(p); setPanel(&panel_);
  }
 private: lgfx::Bus_SPI bus_; lgfx::Panel_ILI9341 panel_;
};
Es3c28pDevice g_device;
}
lgfx::LGFX_Device& lcd(){return g_device;}
const char* lcd_panel_name(){return kPanelName;}
uint32_t lcd_orientation_id(){return 0x45533301u;}
}
