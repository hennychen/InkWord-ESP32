/**
 * @file GxEPD2_310_320x240.cpp
 * @brief 3.1" 320x240 BW 墨水屏驱动实现（UC8253 COG）
 *
 * 骨架取自 GxEPD2_374_DEPG0370.cpp（同 UC8253 控制器族），按 3.1" 屏
 * 规格调整分辨率（320x240）与时序（全刷 3s / 局刷 0.5s）。
 *
 * ⚠ PSR(0x00) 方向字节 0xD3 为 DEPG0370 标定值，本屏 FPC 排线不同，
 *   真机 bring-up 需标定扫描方向（看哪轴镜像翻对应 bit，见 _InitDisplay）。
 */

#include "GxEPD2_310_320x240.h"

/* 构造参数：busy_level=LOW（UC8253 系忙电平），busy_timeout=2s。
 * SPI 频率 20MHz（与 DEPG0370 同口径，杜邦线+转接板实测稳定） */
GxEPD2_310_320x240::GxEPD2_310_320x240(int16_t cs, int16_t dc, int16_t rst, int16_t busy) :
  GxEPD2_EPD(cs, dc, rst, busy, LOW, 20000000, WIDTH, HEIGHT, panel, hasColor, hasPartialUpdate, hasFastPartialUpdate)
{
}

void GxEPD2_310_320x240::clearScreen(uint8_t value)
{
  _writeScreenBuffer(0x10, value); // set previous
  _writeScreenBuffer(0x13, value); // set current
  refresh(false); // full refresh
  _initial_write = false;
}

void GxEPD2_310_320x240::writeScreenBuffer(uint8_t value)
{
  if (_initial_write) return clearScreen(value);
  _writeScreenBuffer(0x13, value); // set current
}

void GxEPD2_310_320x240::writeScreenBufferAgain(uint8_t value)
{
  _writeScreenBuffer(0x10, value); // set previous
}

void GxEPD2_310_320x240::_writeScreenBuffer(uint8_t command, uint8_t value)
{
  if (!_init_display_done) _InitDisplay();
  _writeCommand(command);
  _startTransfer();
  for (uint32_t i = 0; i < uint32_t(WIDTH) * uint32_t(HEIGHT) / 8; i++)
  {
    _transfer(value);
  }
  _endTransfer();
}

void GxEPD2_310_320x240::writeImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImage(0x13, bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_310_320x240::writeImageForFullRefresh(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImage(0x10, bitmap, x, y, w, h, invert, mirror_y, pgm); // set previous
  _writeImage(0x13, bitmap, x, y, w, h, invert, mirror_y, pgm); // set current
}

void GxEPD2_310_320x240::writeImageAgain(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImage(0x10, bitmap, x, y, w, h, invert, mirror_y, pgm); // set previous
}

void GxEPD2_310_320x240::writeImageToPrevious(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImage(0x10, bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_310_320x240::_writeImage(uint8_t command, const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  delay(1); // yield() to avoid WDT
  uint16_t wb = (w + 7) / 8;
  x -= x % 8;
  w = wb * 8;
  int16_t x1 = x < 0 ? 0 : x;
  int16_t y1 = y < 0 ? 0 : y;
  int16_t w1 = x + w < int16_t(WIDTH) ? w : int16_t(WIDTH) - x;
  int16_t h1 = y + h < int16_t(HEIGHT) ? h : int16_t(HEIGHT) - y;
  int16_t dx = x1 - x;
  int16_t dy = y1 - y;
  w1 -= dx;
  h1 -= dy;
  if ((w1 <= 0) || (h1 <= 0)) return;
  if (!_init_display_done) _InitDisplay();
  if (_initial_write) writeScreenBuffer();
  _writeCommand(0x91); // partial in
  _setPartialRamArea(x1, y1, w1, h1);
  _writeCommand(command);
  _startTransfer();
  for (int16_t i = 0; i < h1; i++)
  {
    for (int16_t j = 0; j < w1 / 8; j++)
    {
      uint8_t data;
      uint16_t idx = mirror_y ? j + dx / 8 + uint16_t((h - 1 - (i + dy))) * wb : j + dx / 8 + uint16_t(i + dy) * wb;
      if (pgm)
      {
#if defined(__AVR) || defined(ESP8266) || defined(ESP32)
        data = pgm_read_byte(&bitmap[idx]);
#else
        data = bitmap[idx];
#endif
      }
      else
      {
        data = bitmap[idx];
      }
      if (invert) data = ~data;
      _transfer(data);
    }
  }
  _endTransfer();
  _writeCommand(0x92); // partial out
  delay(1); // yield() to avoid WDT
}

void GxEPD2_310_320x240::writeImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImagePart(0x13, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_310_320x240::writeImagePartAgain(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImagePart(0x10, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_310_320x240::writeImagePartToPrevious(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImagePart(0x10, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_310_320x240::_writeImagePart(uint8_t command, const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  delay(1);
  if ((w_bitmap < 0) || (h_bitmap < 0) || (w < 0) || (h < 0)) return;
  if ((x_part < 0) || (x_part >= w_bitmap)) return;
  if ((y_part < 0) || (y_part >= h_bitmap)) return;
  uint16_t wb_bitmap = (w_bitmap + 7) / 8;
  x_part -= x_part % 8;
  w = w_bitmap - x_part < w ? w_bitmap - x_part : w;
  h = h_bitmap - y_part < h ? h_bitmap - y_part : h;
  x -= x % 8;
  w = 8 * ((w + 7) / 8);
  int16_t x1 = x < 0 ? 0 : x;
  int16_t y1 = y < 0 ? 0 : y;
  int16_t w1 = x + w < int16_t(WIDTH) ? w : int16_t(WIDTH) - x;
  int16_t h1 = y + h < int16_t(HEIGHT) ? h : int16_t(HEIGHT) - y;
  int16_t dx = x1 - x;
  int16_t dy = y1 - y;
  w1 -= dx;
  h1 -= dy;
  if ((w1 <= 0) || (h1 <= 0)) return;
  if (!_init_display_done) _InitDisplay();
  if (_initial_write) writeScreenBuffer();
  _writeCommand(0x91);
  _setPartialRamArea(x1, y1, w1, h1);
  _writeCommand(command);
  _startTransfer();
  for (int16_t i = 0; i < h1; i++)
  {
    for (int16_t j = 0; j < w1 / 8; j++)
    {
      uint8_t data;
      uint16_t idx = mirror_y ? x_part / 8 + j + dx / 8 + uint16_t((h_bitmap - 1 - (y_part + i + dy))) * wb_bitmap : x_part / 8 + j + dx / 8 + uint16_t(y_part + i + dy) * wb_bitmap;
      if (pgm)
      {
#if defined(__AVR) || defined(ESP8266) || defined(ESP32)
        data = pgm_read_byte(&bitmap[idx]);
#else
        data = bitmap[idx];
#endif
      }
      else
      {
        data = bitmap[idx];
      }
      if (invert) data = ~data;
      _transfer(data);
    }
  }
  _endTransfer();
  _writeCommand(0x92);
  delay(1);
}

void GxEPD2_310_320x240::writeImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (black) writeImage(black, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_310_320x240::writeImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (black) writeImagePart(black, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_310_320x240::writeNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (data1) writeImage(data1, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_310_320x240::drawImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImage(bitmap, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
  writeImageAgain(bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_310_320x240::drawImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImagePart(bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
  writeImagePartAgain(bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_310_320x240::drawImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (black) drawImage(black, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_310_320x240::drawImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (black) drawImagePart(black, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_310_320x240::drawNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (data1) drawImage(data1, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_310_320x240::refresh(bool partial_update_mode)
{
  if (partial_update_mode) refresh(0, 0, WIDTH, HEIGHT);
  else
  {
    _Update_Full();
    _initial_refresh = false;
  }
}

void GxEPD2_310_320x240::refresh(int16_t x, int16_t y, int16_t w, int16_t h)
{
  if (_initial_refresh) return refresh(false);
  int16_t w1 = x < 0 ? w + x : w;
  int16_t h1 = y < 0 ? h + y : h;
  int16_t x1 = x < 0 ? 0 : x;
  int16_t y1 = y < 0 ? 0 : y;
  w1 = x1 + w1 < int16_t(WIDTH) ? w1 : int16_t(WIDTH) - x1;
  h1 = y1 + h1 < int16_t(HEIGHT) ? h1 : int16_t(HEIGHT) - y1;
  if ((w1 <= 0) || (h1 <= 0)) return;
  w1 += x1 % 8;
  if (w1 % 8 > 0) w1 += 8 - w1 % 8;
  x1 -= x1 % 8;
  if (usePartialUpdateWindow) _writeCommand(0x91);
  _setPartialRamArea(x1, y1, w1, h1);
  _Update_Part();
  if (usePartialUpdateWindow) _writeCommand(0x92);
}

void GxEPD2_310_320x240::powerOff(void)
{
  _PowerOff();
}

void GxEPD2_310_320x240::hibernate()
{
  _PowerOff();
  if (_rst >= 0)
  {
    _writeCommand(0x07); // deep sleep
    _writeData(0xA5);
    _hibernating = true;
    _init_display_done = false;
  }
}

void GxEPD2_310_320x240::_setPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
  uint16_t xe = (x + w - 1) | 0x0007;
  uint16_t ye = y + h - 1;
  x &= 0xFFF8;
  _writeCommand(0x90);
  _writeData(x);
  _writeData(xe);
  _writeData(y / 256);
  _writeData(y % 256);
  _writeData(ye / 256);
  _writeData(ye % 256);
  _writeData(0x01);
}

void GxEPD2_310_320x240::_PowerOn()
{
  if (!_power_is_on)
  {
    _writeCommand(0x04);
    _waitWhileBusy("_PowerOn", power_on_time);
  }
  _power_is_on = true;
}

void GxEPD2_310_320x240::_PowerOff()
{
  if (_power_is_on)
  {
    _writeCommand(0x02);
    _waitWhileBusy("_PowerOff", power_off_time);
  }
  _power_is_on = false;
}

void GxEPD2_310_320x240::_InitDisplay()
{
  if (_hibernating) _reset();
  else
  {
    _writeCommand(0x00); // PANEL SETTING
    _writeData(0x1e);    // soft reset
    _writeData(0x0d);
    delay(1);
  }
  _power_is_on = false;
  _writeCommand(0x00); // PANEL SETTING
  /* ⚠ bring-up 标定项：方向字节决定 X/Y 扫描方向。
   * bit3=y/gate 轴, bit2=x/source 轴（UC8253）：
   *   0xDF = x+ y+   0xD7 = x+ y-
   *   0xDB = x- y+   0xD3 = x- y-（双翻=180°）
   * DEPG0370 标定值 0xD3（横屏最终方向修正），本屏 FPC 不同，
   * 需真机 F 自检图案标定：看哪轴镜像翻对应 bit。
   * 标定口诀：看哪轴镜像就翻对应 bit（横屏竖轴=x/bit2） */
  _writeData(0xD3);    // ⚠ 暂定 DEPG0370 标定值，bring-up 需验证
  _writeData(0x0d);    // LUT bank
  _init_display_done = true;
}

void GxEPD2_310_320x240::_Update_Full()
{
  _writeCommand(0x50);
  _writeData(0x97);
  _PowerOn();
  _writeCommand(0x12); // display refresh
  _waitWhileBusy("_Update_Full", full_refresh_time);
  _PowerOff();
}

void GxEPD2_310_320x240::_Update_Part()
{
  /* 暂沿用 DEPG0370 对齐官方的局刷参数（E0/E5/CDI），真机 bring-up
   * 需验证残影表现；若残影严重，按 DEPG0370 先例调 E5 温度补偿 */
  if (hasFastPartialUpdate)
  {
    _writeCommand(0xE0); // CCSET
    _writeData(0x02);    // TSFIX
    _writeCommand(0xE5); // Force Temperature
    _writeData(0x6E);
  }
  _writeCommand(0x50);
  _writeData(0xD7);
  _PowerOn();
  _writeCommand(0x12);
  _waitWhileBusy("_Update_Part", partial_refresh_time);
  _PowerOff();
  if (hasFastPartialUpdate) _InitDisplay();
}

/* ---- demo 忠实版局刷（完全无状态，同 DEPG0370 模式）---- */
void GxEPD2_310_320x240::hwReset()
{
  _reset();
}

void GxEPD2_310_320x240::initFullDemo()
{
  /* demo 全刷初始化：PSR + CDI=0x97；方向位暂取 0xD3（⚠ bring-up 标定） */
  _power_is_on = false;
  _writeCommand(0x00);
  _writeData(0xD3);    // ⚠ bring-up 需验证
  _writeData(0x0d);
  _writeCommand(0x50);
  _writeData(0x97);
  _initial_write = false;
  _initial_refresh = false;
  _init_display_done = true;
}

void GxEPD2_310_320x240::initPartialDemo()
{
  /* demo 局刷初始化三件套：PSR2=0x0d / CDI=0x17(VBD floating) / E5=100。
   * 暂沿用 DEPG0370 回归的 demo 原始值，真机验证残影表现 */
  _power_is_on = false;
  _writeCommand(0x00);
  _writeData(0xD3);    // ⚠ bring-up 需验证
  _writeData(0x0d);
  _writeCommand(0x50);
  _writeData(0x17);    // VBD floating
  _writeCommand(0xE0);
  _writeData(0x02);    // TSFIX
  _writeCommand(0xE5);
  _writeData(100);
  _initial_write = false;
  _initial_refresh = false;
  _init_display_done = true;
}

void GxEPD2_310_320x240::demoWriteDual(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                        const uint8_t* prev_fb, const uint8_t* new_fb)
{
  /* demo EPD_Dis_Part_RAM 忠实：0x91 partial-in → 写双 RAM → 不 0x92。
   * prev/new 为竖屏整帧（320x240 行主序，行宽 40 字节，bit=1 白） */
  uint16_t wb = w / 8;
  uint16_t xb = x / 8;
  _writeCommand(0x91);
  _setPartialRamArea(x, y, w, h);
  _writeCommand(0x10);
  _startTransfer();
  for (uint16_t i = 0; i < h; i++)
  {
    const uint8_t* row = prev_fb + (uint32_t)(y + i) * (WIDTH / 8) + xb;
    for (uint16_t j = 0; j < wb; j++) _transfer(row[j]);
  }
  _endTransfer();
  _writeCommand(0x13);
  _startTransfer();
  for (uint16_t i = 0; i < h; i++)
  {
    const uint8_t* row = new_fb + (uint32_t)(y + i) * (WIDTH / 8) + xb;
    for (uint16_t j = 0; j < wb; j++) _transfer(row[j]);
  }
  _endTransfer();
}

void GxEPD2_310_320x240::demoWriteFull(const uint8_t* new_fb)
{
  /* demo 真全刷写入：无窗口，直接整屏写 0x13 */
  _writeCommand(0x13);
  _startTransfer();
  for (uint32_t i = 0; i < uint32_t(WIDTH) * uint32_t(HEIGHT) / 8; i++)
  {
    _transfer(new_fb[i]);
  }
  _endTransfer();
}

void GxEPD2_310_320x240::demoWriteDualNoWindow(const uint8_t* prev_fb, const uint8_t* new_fb)
{
  /* 无窗口整屏双 RAM 写入：不发 0x91/0x90，直接 0x10 旧帧 + 0x13 新帧，
   * COG 全屏差分驱动变化像素。帧大小 320×240/8 = 9600 字节 */
  _writeCommand(0x10);
  _startTransfer();
  for (uint32_t i = 0; i < uint32_t(WIDTH) * uint32_t(HEIGHT) / 8; i++)
  {
    _transfer(prev_fb[i]);
  }
  _endTransfer();
  _writeCommand(0x13);
  _startTransfer();
  for (uint32_t i = 0; i < uint32_t(WIDTH) * uint32_t(HEIGHT) / 8; i++)
  {
    _transfer(new_fb[i]);
  }
  _endTransfer();
}

void GxEPD2_310_320x240::updateDemoPartial(uint8_t passes)
{
  /* demo 更新序列：0x04→0x12×passes→0x02。
   * 注意：规格建议每 5 次快刷/局刷后加一次全屏刷新以减少残影 */
  if (passes < 1) passes = 1;
  _writeCommand(0x04);
  _waitWhileBusy("DemoPartPowOn", power_on_time);
  uint32_t t0 = millis();
  for (uint8_t p = 0; p < passes; p++) {
    _writeCommand(0x12);
    _writeData(0x00);
    _waitWhileBusy("DemoPart", full_refresh_time);
  }
  Serial.printf("[EPD] demo partial refresh busy: %ums (%u %s)",
                (unsigned int)(millis() - t0), passes, passes > 1 ? "passes" : "pass");
  Serial.println();
  _writeCommand(0x02);
  _writeData(0x00);
  _waitWhileBusy("DemoPartPowOff", power_off_time);
  _power_is_on = false;
}
