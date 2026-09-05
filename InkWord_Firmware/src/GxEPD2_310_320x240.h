/**
 * @file GxEPD2_310_320x240.h
 * @brief 3.1" 320x240 BW 墨水屏驱动类（UC8253 COG）
 *
 * 骨架取自 GxEPD2_374_DEPG0370（同为 UC8253 类 COG），按 3.1" 屏规格
 * 调整分辨率（320x240）与时序参数（全刷 3s / 快刷 1s / 局刷 0.5s）。
 *
 * 面板规格：
 *   分辨率 320x240，SPI 接口，24P FPC 0.5mm 间距
 *   视域 62.72x47.04mm，点间距 0.196x0.196mm
 *   黑白双色，对角 ≈129 PPI
 *
 * ⚠ bring-up 待标定项：
 *   - PSR(0x00) 方向字节（0xD3 为 DEPG0370 标定值，本屏 FPC 不同，
 *     需真机 F 自检图案标定扫描方向，见 _InitDisplay 注释）
 *   - BUSY 极性（UC8253 系默认 LOW 忙，待确认）
 *   - 局刷 CDI/E5 参数（暂沿用 DEPG0370 demo 值，真机验证残影表现）
 */
#ifndef _GXEPD2_310_320x240_H_
#define _GXEPD2_310_320x240_H_

#include <GxEPD2_EPD.h>

class GxEPD2_310_320x240 : public GxEPD2_EPD
{
  public:
    // attributes
    static const uint16_t WIDTH = 320;
    static const uint16_t WIDTH_VISIBLE = WIDTH;
    static const uint16_t HEIGHT = 240;
    static const GxEPD2::Panel panel = GxEPD2::GDEY037T03;  // UC8253 族
    static const bool hasColor = false;
    static const bool hasPartialUpdate = true;
    static const bool usePartialUpdateWindow = true;
    static const bool hasFastPartialUpdate = true;
    static const uint16_t power_on_time = 50;   // ms
    static const uint16_t power_off_time = 50;  // ms
    static const uint16_t full_refresh_time = 3000;  // ms（规格 3s）
    static const uint16_t partial_refresh_time = 500; // ms（规格 0.5s）
    // constructor
    GxEPD2_310_320x240(int16_t cs, int16_t dc, int16_t rst, int16_t busy);
    // methods (virtual)
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
    /* ---- demo 忠实版局刷支持（同 DEPG0370 模式：完全无状态）---- */
    void hwReset();
    void initFullDemo();
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
