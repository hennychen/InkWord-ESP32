/**
 * @file GxEPD2_gdeq031t10.cpp
 * @brief 3.1" 240x320 BW 墨水屏驱动实现（UC8253 COG，GDEQ031T10）
 *
 * 骨架取自 GxEPD2_374_DEPG0370.cpp（同 UC8253 控制器族），按 GDEQ031T10
 * demo 实证序列修正（2026-09-05）。
 *
 * ⚠ 与 DEPG0370 的关键差异：
 *   - PSR(0x00) 仅 1 字节 0x1F（LUT from register），非 DEPG0370 的 2 字节
 *   - SPI 10MHz（DEPG0370 用 20MHz）
 *   - 局刷 E5=0x79（DEPG0370 用 100/0x64）
 *   - 快刷 E5=0x5A（DEPG0370 无此模式）
 */

#include "GxEPD2_gdeq031t10.h"

/* 构造参数：busy_level=LOW（UC8253 系忙电平），busy_timeout=5s。
 * SPI 频率 10MHz（demo 标定值，DEPG0370 用 20MHz 但本屏规格不同） */
GxEPD2_gdeq031t10::GxEPD2_gdeq031t10(int16_t cs, int16_t dc, int16_t rst, int16_t busy) :
  GxEPD2_EPD(cs, dc, rst, busy, LOW, 10000000, WIDTH, HEIGHT, panel, hasColor, hasPartialUpdate, hasFastPartialUpdate)
{
}

void GxEPD2_gdeq031t10::clearScreen(uint8_t value)
{
  _writeScreenBuffer(0x10, value); // set previous
  _writeScreenBuffer(0x13, value); // set current
  refresh(false); // full refresh
  _initial_write = false;
}

void GxEPD2_gdeq031t10::writeScreenBuffer(uint8_t value)
{
  if (_initial_write) return clearScreen(value);
  _writeScreenBuffer(0x13, value); // set current
}

void GxEPD2_gdeq031t10::writeScreenBufferAgain(uint8_t value)
{
  _writeScreenBuffer(0x10, value); // set previous
}

void GxEPD2_gdeq031t10::_writeScreenBuffer(uint8_t command, uint8_t value)
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

void GxEPD2_gdeq031t10::writeImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImage(0x13, bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_gdeq031t10::writeImageForFullRefresh(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImage(0x10, bitmap, x, y, w, h, invert, mirror_y, pgm); // set previous
  _writeImage(0x13, bitmap, x, y, w, h, invert, mirror_y, pgm); // set current
}

void GxEPD2_gdeq031t10::writeImageAgain(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImage(0x10, bitmap, x, y, w, h, invert, mirror_y, pgm); // set previous
}

void GxEPD2_gdeq031t10::writeImageToPrevious(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImage(0x10, bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_gdeq031t10::_writeImage(uint8_t command, const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
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

void GxEPD2_gdeq031t10::writeImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImagePart(0x13, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_gdeq031t10::writeImagePartAgain(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImagePart(0x10, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_gdeq031t10::writeImagePartToPrevious(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImagePart(0x10, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_gdeq031t10::_writeImagePart(uint8_t command, const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
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

void GxEPD2_gdeq031t10::writeImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (black) writeImage(black, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_gdeq031t10::writeImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (black) writeImagePart(black, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_gdeq031t10::writeNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (data1) writeImage(data1, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_gdeq031t10::drawImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImage(bitmap, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
  writeImageAgain(bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_gdeq031t10::drawImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImagePart(bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
  writeImagePartAgain(bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_gdeq031t10::drawImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (black) drawImage(black, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_gdeq031t10::drawImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (black) drawImagePart(black, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_gdeq031t10::drawNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (data1) drawImage(data1, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_gdeq031t10::refresh(bool partial_update_mode)
{
  if (partial_update_mode) refresh(0, 0, WIDTH, HEIGHT);
  else
  {
    _Update_Full();
    _initial_refresh = false;
  }
}

void GxEPD2_gdeq031t10::refresh(int16_t x, int16_t y, int16_t w, int16_t h)
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

void GxEPD2_gdeq031t10::powerOff(void)
{
  _PowerOff();
}

void GxEPD2_gdeq031t10::hibernate()
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

void GxEPD2_gdeq031t10::_setPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
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

void GxEPD2_gdeq031t10::_PowerOn()
{
  if (!_power_is_on)
  {
    _writeCommand(0x04);
    _waitWhileBusy("_PowerOn", power_on_time);
  }
  _power_is_on = true;
}

void GxEPD2_gdeq031t10::_PowerOff()
{
  if (_power_is_on)
  {
    _writeCommand(0x02);
    _waitWhileBusy("_PowerOff", power_off_time);
  }
  _power_is_on = false;
}

/* ⚠ _InitDisplay / initXxxDemo 序列待 demo 忠实重写（P1 阶段）：
 *   PSR(0x00) 改为单字节 0x1F（非 DEPG0370 的 2 字节 0xD3,0x0D）
 *   SPI 已在构造函数改为 10MHz
 * 当前暂保留 DEPG0370 骨架序列占位，bring-up 时替换 */
void GxEPD2_gdeq031t10::_InitDisplay()
{
  if (_hibernating) _reset();
  else
  {
    _writeCommand(0x00); // PANEL SETTING
    _writeData(0x1F);    // demo: PSR 单字节 0x1F（LUT from register）
    delay(1);
  }
  _power_is_on = false;
  _writeCommand(0x00); // PANEL SETTING
  _writeData(0x1F);    // demo: PSR 单字节 0x1F
  _init_display_done = true;
}

void GxEPD2_gdeq031t10::_Update_Full()
{
  _writeCommand(0x50);
  _writeData(0x97);
  _PowerOn();
  _writeCommand(0x12); // display refresh
  _waitWhileBusy("_Update_Full", full_refresh_time);
  _PowerOff();
}

void GxEPD2_gdeq031t10::_Update_Part()
{
  /* demo 局刷参数：E0=0x02, E5=0x79, CDI=0x50/0xD7 */
  if (hasFastPartialUpdate)
  {
    _writeCommand(0xE0); // CCSET
    _writeData(0x02);    // TSFIX
    _writeCommand(0xE5); // Force Temperature
    _writeData(0x79);    // demo 局刷值（DEPG0370 用 0x6E/100）
  }
  _writeCommand(0x50);
  _writeData(0xD7);
  _PowerOn();
  _writeCommand(0x12);
  _waitWhileBusy("_Update_Part", partial_refresh_time);
  _PowerOff();
  if (hasFastPartialUpdate) _InitDisplay();
}

/* ---- demo 忠实版（GDEQ031T10 官方 demo 序列，2026-09-05）---- */
void GxEPD2_gdeq031t10::hwReset()
{
  _reset();
}

void GxEPD2_gdeq031t10::initFullDemo()
{
  /* demo 全刷初始化：PSR=0x1F（单字节）+ power on + CDI=0x97 */
  _power_is_on = false;
  _writeCommand(0x00);
  _writeData(0x1F);    // PSR: LUT from register（单字节）
  _writeCommand(0x04); // Power On
  _waitWhileBusy("InitFullPowOn", power_on_time);
  _writeCommand(0x50);
  _writeData(0x97);    // CDI
  _initial_write = false;
  _initial_refresh = false;
  _init_display_done = true;
}

void GxEPD2_gdeq031t10::initFastDemo()
{
  /* demo 快刷初始化：PSR=0x1F + power on + E0=0x02 + E5=0x5A */
  _power_is_on = false;
  _writeCommand(0x00);
  _writeData(0x1F);    // PSR: LUT from register
  _writeCommand(0x04); // Power On
  _waitWhileBusy("InitFastPowOn", power_on_time);
  _writeCommand(0xE0);
  _writeData(0x02);    // CCSET: TSFIX
  _writeCommand(0xE5);
  _writeData(0x5A);    // Force Temperature（快刷专用值）
  _initial_write = false;
  _initial_refresh = false;
  _init_display_done = true;
}

void GxEPD2_gdeq031t10::initPartialDemo()
{
  /* demo 局刷初始化：PSR=0x1F + power on + E0=0x02 + E5=0x79 + CDI=0x50/0xD7 */
  _power_is_on = false;
  _writeCommand(0x00);
  _writeData(0x1F);    // PSR: LUT from register
  _writeCommand(0x04); // Power On
  _waitWhileBusy("InitPartPowOn", power_on_time);
  _writeCommand(0xE0);
  _writeData(0x02);    // CCSET: TSFIX
  _writeCommand(0xE5);
  _writeData(0x79);    // Force Temperature（局刷专用值）
  _writeCommand(0x50);
  _writeData(0xD7);    // CDI
  _initial_write = false;
  _initial_refresh = false;
  _init_display_done = true;
}

void GxEPD2_gdeq031t10::demoWriteDual(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                        const uint8_t* prev_fb, const uint8_t* new_fb)
{
  /* demo EPD_Dis_Part_RAM 忠实：0x91 partial-in → 写双 RAM → 不 0x92。
   * prev/new 为竖屏整帧（240x320 行主序，行宽 30 字节，bit=1 白） */
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

void GxEPD2_gdeq031t10::demoWriteFull(const uint8_t* new_fb)
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

void GxEPD2_gdeq031t10::demoWriteDualNoWindow(const uint8_t* prev_fb, const uint8_t* new_fb)
{
  /* 无窗口整屏双 RAM 写入：不发 0x91/0x90，直接 0x10 旧帧 + 0x13 新帧，
   * COG 全屏差分驱动变化像素。帧大小 240×320/8 = 9600 字节 */
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

void GxEPD2_gdeq031t10::updateDemoPartial(uint8_t passes)
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
