#pragma once
#include <stdint.h>
#define ES3C28P_BOARD_NAME "ESP32-S3 (ES3C28P)"
#ifndef ES3C28P_LCD_SPI_HZ
#define ES3C28P_LCD_SPI_HZ 40000000
#endif
namespace board {
constexpr int kLcdSclk=12, kLcdMosi=11, kLcdMiso=13, kLcdCs=10, kLcdDc=46, kLcdRst=-1, kLcdBacklight=45;
constexpr int kLcdWidth=240, kLcdHeight=320;
constexpr int kTouchSda=16, kTouchScl=15, kTouchReset=18, kTouchIrq=17;
constexpr uint8_t kTouchAddress=0x38;
constexpr uint32_t kTouchI2cHz=400000;
constexpr int kLedRed=-1, kLedGreen=-1, kLedBlue=-1, kLightSensor=-1, kSpeaker=-1;
constexpr uint16_t kLightRawBright=0, kLightRawDark=0;
constexpr int kSdClk=38, kSdCmd=40, kSdD0=39, kSdD1=41, kSdD2=48, kSdD3=47;
constexpr int kBootButton=0, kLinkRx=-1, kLinkTx=-1;
constexpr uint8_t kLedcBacklight=0, kLedcSpeaker=2, kLedcRed=4, kLedcGreen=5, kLedcBlue=6;
}
