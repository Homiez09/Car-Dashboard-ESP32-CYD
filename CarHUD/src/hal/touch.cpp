#include "touch.h"
#include <Arduino.h>
#include <Wire.h>
#include "../config/board_config.h"
#include "../core/log.h"
#include "../core/persist.h"
#include "../core/util.h"
#include "display.h"
#include "lcd.h"
namespace hal::touch {
namespace {
constexpr uint8_t kPressDebounce=2;
struct StoredCalibration { core::TouchCalibration cal; uint32_t orientation; };
core::Storage* g_storage=nullptr; lv_indev_t* g_indev=nullptr;
core::TouchCalibration g_cal=core::TouchCalibration::identity();
bool g_calibrated=false, g_enabled=true, g_i2c_ready=false;
uint8_t g_press_count=0; lv_point_t g_last{0,0};
bool read_ft6336(int32_t& x,int32_t& y){
 if(!g_i2c_ready)return false;
 Wire.beginTransmission(board::kTouchAddress); Wire.write(0x02);
 if(Wire.endTransmission(false)!=0)return false;
 if(Wire.requestFrom(board::kTouchAddress,static_cast<uint8_t>(5))!=5)return false;
 uint8_t count=Wire.read()&0x0F, xh=Wire.read(), xl=Wire.read(), yh=Wire.read(), yl=Wire.read();
 if(count==0||count>2)return false;
 int32_t rx=((xh&0x0F)<<8)|xl, ry=((yh&0x0F)<<8)|yl;
 if(rx>=board::kLcdWidth||ry>=board::kLcdHeight)return false;
 if(display::flipped()){x=board::kLcdHeight-1-ry;y=rx;}
 else{x=ry;y=board::kLcdWidth-1-rx;}
 return true;
}
core::TouchCalibration active_calibration(){
 return display::flipped()?g_cal.rotated_180(lcd().width(),lcd().height()):g_cal;
}
void read_cb(lv_indev_t*,lv_indev_data_t* data){
 int32_t x=0,y=0; bool pressed=g_enabled&&read_ft6336(x,y);
 if(!pressed){g_press_count=0;data->state=LV_INDEV_STATE_RELEASED;data->point=g_last;return;}
 if(g_press_count<kPressDebounce)++g_press_count;
 if(g_press_count<kPressDebounce){data->state=LV_INDEV_STATE_RELEASED;data->point=g_last;return;}
 auto p=active_calibration().map(x,y);
 g_last.x=core::clamp<int32_t>(p.x,0,lcd().width()-1);
 g_last.y=core::clamp<int32_t>(p.y,0,lcd().height()-1);
 data->point=g_last;data->state=LV_INDEV_STATE_PRESSED;
}
}
void init(core::Storage& storage){
 g_storage=&storage;pinMode(board::kTouchReset,OUTPUT);digitalWrite(board::kTouchReset,LOW);delay(10);
 digitalWrite(board::kTouchReset,HIGH);delay(120);pinMode(board::kTouchIrq,INPUT_PULLUP);
 g_i2c_ready=Wire.begin(board::kTouchSda,board::kTouchScl,board::kTouchI2cHz);
 if(!g_i2c_ready)LOG_E("TOUCH","FT6336G I2C initialization failed");
 StoredCalibration stored{core::TouchCalibration::identity(),0};
 auto result=core::persist::load(storage,core::storage_key::kTouchCal,&stored,sizeof(stored));
 if(result==core::persist::LoadResult::Missing)LOG_I("TOUCH","no calibration stored; using nominal mapping");
 else if(result!=core::persist::LoadResult::Ok)LOG_W("TOUCH","stored calibration invalid; recalibration required");
 else if(stored.orientation!=lcd_orientation_id())LOG_W("TOUCH","calibration belongs to another panel");
 else if(!stored.cal.plausible())LOG_W("TOUCH","stored calibration implausible");
 else{g_cal=stored.cal;g_calibrated=true;LOG_I("TOUCH","calibration loaded");}
 g_indev=lv_indev_create();lv_indev_set_type(g_indev,LV_INDEV_TYPE_POINTER);lv_indev_set_read_cb(g_indev,read_cb);
}
bool calibrated(){return g_calibrated;}
bool read_raw(platform::TouchSample& out){
 int32_t x=0,y=0;out.pressed=read_ft6336(x,y);out.raw_x=static_cast<int16_t>(x);
 out.raw_y=static_cast<int16_t>(y);out.pressure=out.pressed?1:0;return true;
}
void apply_calibration(const core::TouchCalibration& cal){
 auto base=display::flipped()?cal.rotated_180(lcd().width(),lcd().height()):cal;
 if(!base.plausible()){LOG_W("TOUCH","rejected implausible calibration");return;}
 g_cal=base;g_calibrated=true;
 if(g_storage){StoredCalibration stored{g_cal,lcd_orientation_id()};
 core::persist::save(*g_storage,core::storage_key::kTouchCal,core::kTouchCalibrationVersion,&stored,sizeof(stored));}
 LOG_I("TOUCH","calibration saved");
}
void set_enabled(bool enabled){g_enabled=enabled;g_press_count=0;if(g_indev)lv_indev_reset(g_indev,nullptr);}
}
