/**
 * @file GxEPD2_374_DEPG0370.cpp
 * @brief DKE DEPG0370BBU253F33HP-M7 驱动实现（基于 GxEPD2 骨架）
 *
 * 与 GxEPD2_370_GDEY037T03 的差异（对照 DEPG0370 官方 demo code）：
 *   _InitDisplay(): PSR = 0xD3, 0x0D（扫描方向 x- y-：横屏最终方向修正）
 *   _Update_Full(): CDI = 0x97，无温度强制（demo 全刷序列）
 *   _Update_Part(): 0xE0/0x02 + 0xE5/100 + CDI = 0x17（demo 局刷序列）
 * 其余缓冲/窗口/刷新管理与 GxEPD2 原版一致。
 */

#include "GxEPD2_374_DEPG0370.h"

/* 构造参数：busy_level=LOW（忙电平），busy_timeout=10s。
 * SPI 频率 20MHz（2026-08-21 实验 A：10→20MHz 省一半帧传输 ≈5ms/帧；
 * 杜邦线+EVK011 转接实测稳定则保留，若花屏/错帧回退 10MHz）；
 * 如需再调整用 selectSPI(SPI, SPISettings(...))，不要改这里 */
GxEPD2_374_DEPG0370::GxEPD2_374_DEPG0370(int16_t cs, int16_t dc, int16_t rst, int16_t busy) :
  GxEPD2_EPD(cs, dc, rst, busy, LOW, 20000000, WIDTH, HEIGHT, panel, hasColor, hasPartialUpdate, hasFastPartialUpdate)
{
}

void GxEPD2_374_DEPG0370::clearScreen(uint8_t value)
{
  // full refresh needed for all cases (previous != screen)
  _writeScreenBuffer(0x10, value); // set previous
  _writeScreenBuffer(0x13, value); // set current
  refresh(false); // full refresh
  _initial_write = false;
}

void GxEPD2_374_DEPG0370::writeScreenBuffer(uint8_t value)
{
  if (_initial_write) return clearScreen(value);
  _writeScreenBuffer(0x13, value); // set current
}

void GxEPD2_374_DEPG0370::writeScreenBufferAgain(uint8_t value)
{
  _writeScreenBuffer(0x10, value); // set previous
}

void GxEPD2_374_DEPG0370::_writeScreenBuffer(uint8_t command, uint8_t value)
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

void GxEPD2_374_DEPG0370::writeImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImage(0x13, bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_374_DEPG0370::writeImageForFullRefresh(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImage(0x10, bitmap, x, y, w, h, invert, mirror_y, pgm); // set previous
  _writeImage(0x13, bitmap, x, y, w, h, invert, mirror_y, pgm); // set current
}

void GxEPD2_374_DEPG0370::writeImageAgain(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImage(0x10, bitmap, x, y, w, h, invert, mirror_y, pgm); // set previous
}

void GxEPD2_374_DEPG0370::writeImageToPrevious(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImage(0x10, bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_374_DEPG0370::_writeImage(uint8_t command, const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  delay(1); // yield() to avoid WDT
  uint16_t wb = (w + 7) / 8; // width bytes, bitmaps are padded
  x -= x % 8; // byte boundary
  w = wb * 8; // byte boundary
  int16_t x1 = x < 0 ? 0 : x; // limit
  int16_t y1 = y < 0 ? 0 : y; // limit
  int16_t w1 = x + w < int16_t(WIDTH) ? w : int16_t(WIDTH) - x; // limit
  int16_t h1 = y + h < int16_t(HEIGHT) ? h : int16_t(HEIGHT) - y; // limit
  int16_t dx = x1 - x;
  int16_t dy = y1 - y;
  w1 -= dx;
  h1 -= dy;
  if ((w1 <= 0) || (h1 <= 0)) return;
  if (!_init_display_done) _InitDisplay();
  if (_initial_write) writeScreenBuffer(); // initial full screen buffer clean
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

void GxEPD2_374_DEPG0370::writeImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImagePart(0x13, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_374_DEPG0370::writeImagePartAgain(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImagePart(0x10, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm); // set previous
}

void GxEPD2_374_DEPG0370::writeImagePartToPrevious(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  _writeImagePart(0x10, bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_374_DEPG0370::_writeImagePart(uint8_t command, const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  delay(1); // yield() to avoid WDT
  if ((w_bitmap < 0) || (h_bitmap < 0) || (w < 0) || (h < 0)) return;
  if ((x_part < 0) || (x_part >= w_bitmap)) return;
  if ((y_part < 0) || (y_part >= h_bitmap)) return;
  uint16_t wb_bitmap = (w_bitmap + 7) / 8; // width bytes, bitmaps are padded
  x_part -= x_part % 8; // byte boundary
  w = w_bitmap - x_part < w ? w_bitmap - x_part : w; // limit
  h = h_bitmap - y_part < h ? h_bitmap - y_part : h; // limit
  x -= x % 8; // byte boundary
  w = 8 * ((w + 7) / 8); // byte boundary, bitmaps are padded
  int16_t x1 = x < 0 ? 0 : x; // limit
  int16_t y1 = y < 0 ? 0 : y; // limit
  int16_t w1 = x + w < int16_t(WIDTH) ? w : int16_t(WIDTH) - x; // limit
  int16_t h1 = y + h < int16_t(HEIGHT) ? h : int16_t(HEIGHT) - y; // limit
  int16_t dx = x1 - x;
  int16_t dy = y1 - y;
  w1 -= dx;
  h1 -= dy;
  if ((w1 <= 0) || (h1 <= 0)) return;
  if (!_init_display_done) _InitDisplay();
  if (_initial_write) writeScreenBuffer(); // initial full screen buffer clean
  _writeCommand(0x91); // partial in
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
  _writeCommand(0x92); // partial out
  delay(1); // yield() to avoid WDT
}

void GxEPD2_374_DEPG0370::writeImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (black)
  {
    writeImage(black, x, y, w, h, invert, mirror_y, pgm);
  }
}

void GxEPD2_374_DEPG0370::writeImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (black)
  {
    writeImagePart(black, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  }
}

void GxEPD2_374_DEPG0370::writeNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (data1)
  {
    writeImage(data1, x, y, w, h, invert, mirror_y, pgm);
  }
}

void GxEPD2_374_DEPG0370::drawImage(const uint8_t bitmap[], int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImage(bitmap, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
  writeImageAgain(bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_374_DEPG0370::drawImagePart(const uint8_t bitmap[], int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  writeImagePart(bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  refresh(x, y, w, h);
  writeImagePartAgain(bitmap, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
}

void GxEPD2_374_DEPG0370::drawImage(const uint8_t* black, const uint8_t* color, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (black)
  {
    drawImage(black, x, y, w, h, invert, mirror_y, pgm);
  }
}

void GxEPD2_374_DEPG0370::drawImagePart(const uint8_t* black, const uint8_t* color, int16_t x_part, int16_t y_part, int16_t w_bitmap, int16_t h_bitmap,
    int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (black)
  {
    drawImagePart(black, x_part, y_part, w_bitmap, h_bitmap, x, y, w, h, invert, mirror_y, pgm);
  }
}

void GxEPD2_374_DEPG0370::drawNative(const uint8_t* data1, const uint8_t* data2, int16_t x, int16_t y, int16_t w, int16_t h, bool invert, bool mirror_y, bool pgm)
{
  if (data1)
  {
    drawImage(data1, x, y, w, h, invert, mirror_y, pgm);
  }
}

void GxEPD2_374_DEPG0370::refresh(bool partial_update_mode)
{
  if (partial_update_mode) refresh(0, 0, WIDTH, HEIGHT);
  else
  {
    _Update_Full();
    _initial_refresh = false; // initial full update done
  }
}

void GxEPD2_374_DEPG0370::refresh(int16_t x, int16_t y, int16_t w, int16_t h)
{
  if (_initial_refresh) return refresh(false); // initial update needs be full update
  // intersection with screen
  int16_t w1 = x < 0 ? w + x : w; // reduce
  int16_t h1 = y < 0 ? h + y : h; // reduce
  int16_t x1 = x < 0 ? 0 : x; // limit
  int16_t y1 = y < 0 ? 0 : y; // limit
  w1 = x1 + w1 < int16_t(WIDTH) ? w1 : int16_t(WIDTH) - x1; // limit
  h1 = y1 + h1 < int16_t(HEIGHT) ? h1 : int16_t(HEIGHT) - y1; // limit
  if ((w1 <= 0) || (h1 <= 0)) return;
  // make x1, w1 multiple of 8
  w1 += x1 % 8;
  if (w1 % 8 > 0) w1 += 8 - w1 % 8;
  x1 -= x1 % 8;
  if (usePartialUpdateWindow) _writeCommand(0x91); // partial in
  _setPartialRamArea(x1, y1, w1, h1);
  _Update_Part();
  if (usePartialUpdateWindow) _writeCommand(0x92); // partial out
}

void GxEPD2_374_DEPG0370::powerOff(void)
{
  _PowerOff();
}

void GxEPD2_374_DEPG0370::hibernate()
{
  _PowerOff();
  if (_rst >= 0)
  {
    _writeCommand(0x07); // deep sleep
    _writeData(0xA5);    // check code
    _hibernating = true;
    _init_display_done = false;
  }
}

void GxEPD2_374_DEPG0370::_setPartialRamArea(uint16_t x, uint16_t y, uint16_t w, uint16_t h)
{
  uint16_t xe = (x + w - 1) | 0x0007; // byte boundary inclusive (last byte)
  uint16_t ye = y + h - 1;
  x &= 0xFFF8; // byte boundary
  _writeCommand(0x90); // partial window
  _writeData(x);
  _writeData(xe);
  _writeData(y / 256);
  _writeData(y % 256);
  _writeData(ye / 256);
  _writeData(ye % 256);
  _writeData(0x01);
}

void GxEPD2_374_DEPG0370::_PowerOn()
{
  if (!_power_is_on)
  {
    _writeCommand(0x04);
    _waitWhileBusy("_PowerOn", power_on_time);
  }
  _power_is_on = true;
}

void GxEPD2_374_DEPG0370::_PowerOff()
{
  if (_power_is_on)
  {
    _writeCommand(0x02); // power off
    _waitWhileBusy("_PowerOff", power_off_time);
  }
  _power_is_on = false;
}

void GxEPD2_374_DEPG0370::_InitDisplay()
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
  /* 扫描方向位（bit3=y/gate 轴, bit2=x/source 轴；demo 原注 "bit2=x,bit3=y"）：
   *   0xDF = x+ y+（demo 默认）    0xD7 = x+ y-（bit3=0，翻转 416 轴）
   *   0xDB = x- y+（bit2=0，翻转 240 轴）  0xD3 = x- y-（双翻=180°）
   * （demo 注释中的 0xDD "x-y+" 与其自身 bit 定义矛盾，疑为笔误，未采用）
   * 方向标定过程（2026-08，F 自检图案实测）：0xDF 双轴镜像 → 0xD7 修横轴后
   * 实测 F 横笔朝下、TL 在左下（上下镜像，竖轴仍反）→ 再翻 x 轴（bit2）→
   * 0xD3 为最终值。标定口诀：看哪轴镜像就翻对应 bit（横屏竖轴=x/bit2） */
  _writeData(0xD3);    // 横屏方向最终修正：x- y-（0xDF→0xD7→0xD3 实测标定）
  _writeData(0x0d);    // demo LUT bank（2026-08-18 回退：全刷路径 demo 本就 0x0d，
                       // 与 0x97 CDI 配套；局刷已改走 demo 忠实序列不再用本函数的局刷分支）
  _init_display_done = true;
}

void GxEPD2_374_DEPG0370::_Update_Full()
{
  _writeCommand(0x50); // border setting with white waveform shaking (demo)
  _writeData(0x97);
  _PowerOn();
  _writeCommand(0x12); // display refresh
  _waitWhileBusy("_Update_Full", full_refresh_time);
  _PowerOff();
}

void GxEPD2_374_DEPG0370::_Update_Part()
{
  /* 注意：UI 局刷主路径已改走 demo 忠实序列（hwReset/initPartialDemo/
   * updateDemoPartial，由 epd_driver.cpp 驱动，无窗口整屏双 RAM 差分）；
   * 本函数（partial window 路径）现无调用方 —— 遗留直通 API
   * epd_partial_refresh 已删（2026-08-20），保留本函数仅备查，
   * 仍保留此前对齐官方的参数 */
  /* 2026-08-18 残影重叠修复：局刷序列对齐 GxEPD2 官方 GDEY037T03（同 UC8253），
   * 替换 demo 参数 ——
   *   a) E5=0x6E（demo 为 100）：官方快速局刷调定的温度补偿，驱动力足；
   *   b) CDI=0xD7（demo 为 0x17 VBD floating）：VBD 内部生成，边框电压受控，
   *      floating 会往局刷窗口边界残留电荷；
   *   c) 刷后 _InitDisplay() 撤销 TSFIX（关键）：demo 序列缺这一步，
   *      E0/E5 强制温度状态泄漏到后续全刷 → 全刷 LUT 波形被污染，
   *      阈值自动全刷/深清也洗不掉残影（真机：翻词 20+ 次后字迹叠加） */
  if (hasFastPartialUpdate)
  {
    _writeCommand(0xE0); // Cascade Setting (CCSET)
    _writeData(0x02);    // TSFIX
    _writeCommand(0xE5); // Force Temperature (TSSET)
    _writeData(0x6E);
  }
  _writeCommand(0x50);
  _writeData(0xD7);
  _PowerOn();
  _writeCommand(0x12); // display refresh
  _waitWhileBusy("_Update_Part", partial_refresh_time);
  _PowerOff();
  if (hasFastPartialUpdate) _InitDisplay(); // undo TSFIX
}

/* ---- demo 忠实版局刷（2026-08-18 残影修复）----
 * 对照 Info/ 官方 demo code：Display_windows_image_partial_update =
 * Initial_partial_mode（硬复位+PSR+CDI/E0/E5）+ EPD_Dis_Part_RAM（双 RAM）
 * + Update（0x04/0x12/0x02）。完全无状态，不依赖 COG 跨刷新存活 */
void GxEPD2_374_DEPG0370::hwReset()
{
  _reset();
}

void GxEPD2_374_DEPG0370::initFullDemo()
{
  /* demo Epaper_Initial_full_mode：PSR + CDI=0x97；方向位保留 0xD3 实测标定值。
   * 必须在 hwReset() 后调用（demo 每次全刷前硬复位，清掉局刷残留的
   * E0=TSFIX/E5=强制温度/PSR2=LUT bank，否则全刷 LUT 在污染状态下选波形） */
  _power_is_on = false;
  _writeCommand(0x00);
  _writeData(0xD3);
  _writeData(0x0d);
  _writeCommand(0x50); // border setting with white waveform shaking
  _writeData(0x97);
  _initial_write = false;
  _initial_refresh = false;
  _init_display_done = true;
}

void GxEPD2_374_DEPG0370::initPartialDemo()
{
  /* 局刷初始化（2026-08-20 回归本面板 demo 原始三件套：
   *   PSR2=0x0d / CDI=0x17(VBD floating) / E5=100(0x64)。
   * 此前用 GDEY037T03 官方值（0x1f/0xD7/0x6E）属张冠李戴 —— GDEY037T03
   * 是 SSD1680 体系面板，PSR 第二字节位定义与 UC8253 完全不同；
   * GxEPD2 对 UC8253 家族（GxEPD2_213_flex）注释 0x0d = "VCOM to 0V
   * fast"，直接影响局刷驱动力。8-18 那次"demo 参数仍残影"的测试
   * PSR2 用的也是 0x1f（未入 git，demo 三件套从未被忠实测过）。
   * PSR 方向位保留 0xD3 实测标定值（与 demo 0xDF 仅差镜像位 bit2） */
  _power_is_on = false;
  _writeCommand(0x00);
  _writeData(0xD3);
  _writeData(0x0d);
  _writeCommand(0x50); // border setting
  _writeData(0x17);    // VBD[1:0]=00 floating（demo 原值）
  _writeCommand(0xE0); // force temp to get the lut waveform
  _writeData(0x02);
  _writeCommand(0xE5);
  _writeData(100);     // demo 原值（门电压选择）
  _initial_write = false;    /* 局刷路径不触发库的首次清屏逻辑 */
  _initial_refresh = false;
  _init_display_done = true;
}

void GxEPD2_374_DEPG0370::demoWriteDual(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                                        const uint8_t* prev_fb, const uint8_t* new_fb)
{
  /* demo EPD_Dis_Part_RAM 忠实实现：单次 0x91 partial-in 会话内连续写双平面，
   * 无 0x92（demo 不退出 partial 窗口，直接 0x04/0x12/0x02）。
   * prev/new 为竖屏整帧（240x416 行主序，行宽 30 字节，bit=1 白）。
   * x/w 需 8 像素对齐（UI 局刷窗口 y/h 8 对齐约束转置后自然满足） */
  uint16_t wb = w / 8;
  uint16_t xb = x / 8;
  _writeCommand(0x91); // partial in
  _setPartialRamArea(x, y, w, h);
  _writeCommand(0x10); // previous plane
  _startTransfer();
  for (uint16_t i = 0; i < h; i++)
  {
    const uint8_t* row = prev_fb + (uint32_t)(y + i) * (WIDTH / 8) + xb;
    for (uint16_t j = 0; j < wb; j++) _transfer(row[j]);
  }
  _endTransfer();
  _writeCommand(0x13); // current plane
  _startTransfer();
  for (uint16_t i = 0; i < h; i++)
  {
    const uint8_t* row = new_fb + (uint32_t)(y + i) * (WIDTH / 8) + xb;
    for (uint16_t j = 0; j < wb; j++) _transfer(row[j]);
  }
  _endTransfer();
}

void GxEPD2_374_DEPG0370::demoWriteFull(const uint8_t* new_fb)
{
  /* demo Epaper_Load_image 忠实实现：真全刷写入 —— 无 0x91/0x90 窗口指令，
   * 直接整屏写 0x13（demo Display_image_full_update 调用链）。
   * 与窗口化全屏刷（demoWriteDual 全屏参数）的区别：UC8253 在 partial window
   * 模式下的刷新 LUT/驱动行为与全屏模式不同，窗口包裹的全刷驱动力不足，
   * 真机实测留残影（2026-08-18，1796ms 完整执行仍洗不净） */
  _writeCommand(0x13);
  _startTransfer();
  for (uint32_t i = 0; i < uint32_t(WIDTH) * uint32_t(HEIGHT) / 8; i++)
  {
    _transfer(new_fb[i]);
  }
  _endTransfer();
}

void GxEPD2_374_DEPG0370::demoWriteDualNoWindow(const uint8_t* prev_fb, const uint8_t* new_fb)
{
  /* Plan B（2026-08-20）：无窗口整屏双 RAM 写入 —— 不发 0x91/0x90，
   * 直接整屏写 0x10 旧帧 + 0x13 新帧，COG 按双平面内存差分驱动变化
   * 像素、跳过不变像素（含窗口外的出处/状态栏）。规避 partial window
   * 模式本身（窗口模式三组参数实测均不能干净刷白，见 .h 注释）。
   * 与 demoWriteDual 的关键差异：无 0x91 partial-in / 0x90 窗口指令，
   * COG 保持全屏扫描模式 —— 与 demoWriteFull 同路径，仅多写旧帧平面 */
  _writeCommand(0x10); // previous plane（整屏）
  _startTransfer();
  for (uint32_t i = 0; i < uint32_t(WIDTH) * uint32_t(HEIGHT) / 8; i++)
  {
    _transfer(prev_fb[i]);
  }
  _endTransfer();
  _writeCommand(0x13); // current plane（整屏）
  _startTransfer();
  for (uint32_t i = 0; i < uint32_t(WIDTH) * uint32_t(HEIGHT) / 8; i++)
  {
    _transfer(new_fb[i]);
  }
  _endTransfer();
}

/* demoWriteSingleNoWindow（单平面写）已删（2026-08-21 实验 B 证伪）：
 * 真机实测 0x12 后 COG 不自动 new→old 转移 —— 只写 0x13 时差分基准
 * 落后一帧，连续局刷出现上上帧陈旧像素（残迹）。0x10 必须每次显式
 * 重写，勿再试 */

void GxEPD2_374_DEPG0370::updateDemoPartial(uint8_t passes)
{
  /* 双刷（passes≥2）：同一上电会话内连发两次 0x12。COG 差分按双 RAM
   * 内存内容而非实际光学状态，第一次未翻转彻底的像素会被第二次
   * 再次驱动（单次翻转补强手段）。
   *
   * 电源终态：0x02 Power Off（关高压 rail、保 VCI 逻辑供电），不深睡。
   * 8-20 曾为救窗口模式加刷新后深睡（0x07/0xA5），窗口模式已弃用
   * （改无窗口整屏双 RAM，见 demoWriteDualNoWindow），深睡每次多耗
   * 400ms 拖慢切换，使命结束移除；下次刷新前 hwReset 自动重新初始化 */
  if (passes < 1) passes = 1;
  _writeCommand(0x04); // power on
  _waitWhileBusy("DemoPartPowOn", power_on_time);
  uint32_t t0 = millis();
  for (uint8_t p = 0; p < passes; p++) {
    _writeCommand(0x12); // update
    _writeData(0x00);   /* demo 忠实哑字节（Epaper_Update_and_Deepsleep） */
    _waitWhileBusy("DemoPart", full_refresh_time);
  }
  Serial.printf("[EPD] demo partial refresh busy: %ums (%u %s)",
                (unsigned int)(millis() - t0), passes, passes > 1 ? "passes" : "pass");
  Serial.println(); /* 耗时异常→波形/供电问题信号 */
  _writeCommand(0x02); // power off
  _writeData(0x00);   /* demo 忠实哑字节 */
  _waitWhileBusy("DemoPartPowOff", power_off_time);
  _power_is_on = false;
}
