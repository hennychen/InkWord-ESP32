/**
 * @file GxEPD2_gdeq031t10.h
 * @brief 3.1" 240x320 BW 墨水屏驱动类（UC8253 COG，GDEQ031T10）
 *
 * 面板规格：
 *   分辨率 240x320（竖屏原生），SPI 接口，24P FPC 0.5mm
 *   视域 62.72x47.04mm，点间距 0.196x0.196mm
 *   黑白双色，对角 ≈129 PPI
 *   全刷 3s / 快刷 1s / 局刷 0.5s
 *
 * ⚠ 与 DEPG0370 的关键差异（demo 实证 2026-09-05）：
 *   - PSR(0x00) 仅 1 字节（0x1F = LUT from register），非 DEPG0370 的 2 字节
 *   - 180°旋转：PSR=0x13（翻转 UD+SHL），非 0xD3→0xDB
 *   - 局刷 E5=0x79（DEPG0370 用 100/0x64）
 *   - 快刷 E5=0x5A（DEPG0370 无此模式）
 *   - SPI 10MHz（DEPG0370 用 20MHz）
 */
#ifndef _GXEPD2_GDEQ031T10_H_
#define _GXEPD2_GDEQ031T10_H_

#include <GxEPD2_EPD.h>

class GxEPD2_gdeq031t10 : public GxEPD2_EPD
{
  public:
    static const uint16_t WIDTH = 240;
    static const uint16_t WIDTH_VISIBLE = WIDTH;
    static const uint16_t HEIGHT = 320;
    /* 库内官方同类 gdeq/GxEPD2_310_GDEQ031T10 即此条目（240x320）。
     * 曾误留骨架来源的 GDEY037T03（3.7" 416x240）：本类从不读该字段，
     * 只随构造透传给 GxEPD2_EPD::panel 存档，故无行为影响，但值本身错 */
    static const GxEPD2::Panel panel = GxEPD2::GDEQ031T10;
    static const bool hasColor = false;
    static const bool hasPartialUpdate = true;
    static const bool usePartialUpdateWindow = true;
    static const bool hasFastPartialUpdate = true;
    static const uint16_t power_on_time = 50;
    static const uint16_t power_off_time = 50;
    static const uint16_t full_refresh_time = 3000;
    static const uint16_t partial_refresh_time = 500;

    GxEPD2_gdeq031t10(int16_t cs, int16_t dc, int16_t rst, int16_t busy);

    void clearScreen(uint8_t value = 0xFF);
    void writeScreenBuffer(uint8_t value = 0xFF);
    void writeScreenBufferAgain(uint8_t value = 0xFF);
    void writeImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImageForFullRefresh(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImageToPrevious(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                        int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImagePartToPrevious(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                                  int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                        int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImageAgain(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeImagePartAgain(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                             int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void writeNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                       int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                       int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void drawNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert = false, bool mirror_y = false, bool pgm = false);
    void refresh(bool partial_update_mode = false);
    void refresh(int16_t x, int16_t y, int16_t w, int16_t h);
    void powerOff();
    void hibernate();

    void hwReset();
    void initFullDemo();
    void initFastDemo();
    void initPartialDemo();
    void demoWriteDual(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       const uint8_t* prev_fb, const uint8_t* new_fb);
    void demoWriteDualNoWindow(const uint8_t* prev_fb, const uint8_t* new_fb);
    void demoWriteFull(const uint8_t* new_fb);
    void updateDemoPartial(uint8_t passes = 1);

  private:
    void _writeScreenBuffer(uint8_t command, uint8_t value);
    void _writeImage(uint8_t command, const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm);
    void _writeImagePart(uint8_t command, const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
                         int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm);
    void _setPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h);
    void _PowerOn();
    void _PowerOff();
    void _InitDisplay();
    void _Update_Full();
    void _Update_Part();
};

#endif
