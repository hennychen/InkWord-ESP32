/**
 * @file GxEPD2_374_DEPG0370.h
 * @brief DKE DEPG0370BBU253F33HP-M7 3.7" 240x416 BW 墨水屏驱动类
 *
 * 骨架取自 GxEPD2 的 GxEPD2_370_GDEY037T03（同为 UC8253 类 COG、240x416、
 * OTP 波形），按 DEPG0370 官方 demo code 修正初始化差异：
 *   - PSR(0x00) 第一字节 0xDF（demo：x+ y+ 扫描方向）
 *   - 全刷 CDI(0x50) = 0x97（与 GxEPD2 一致）
 *   - 局刷 CDI(0x50) = 0xD7、温度 0xE5 = 0x6E、刷后 _InitDisplay() 撤销 TSFIX
 *     （2026-08-18 对齐官方 GDEY037T03，修 demo 参数导致的残影叠加）
 *
 * 硬件架构（EVK011 转接板）：
 *   升压电路在转接板上（Q1 SI1308EDL + L1 47uH + MBR0503），由屏幕 COG
 *   从 FPC pin2(GDR) 自主输出 PWM 驱动，MCU 仅提供 VCI 3.3V（J2-16），
 *   不输出任何 GDR/RESE 信号。
 */
#ifndef _GXEPD2_374_DEPG0370_H_
#define _GXEPD2_374_DEPG0370_H_

#include <GxEPD2_EPD.h>

class GxEPD2_374_DEPG0370 : public GxEPD2_EPD
{
  public:
    // attributes
    static const uint16_t WIDTH = 240;
    static const uint16_t WIDTH_VISIBLE = WIDTH;
    static const uint16_t HEIGHT = 416;
    static const GxEPD2::Panel panel = GxEPD2::GDEY037T03;
    static const bool hasColor = false;
    static const bool hasPartialUpdate = true;
    static const bool usePartialUpdateWindow = true;
    static const bool hasFastPartialUpdate = true;
    static const uint16_t power_on_time = 50; // ms
    static const uint16_t power_off_time = 50; // ms
    static const uint16_t full_refresh_time = 1500; // ms
    static const uint16_t partial_refresh_time = 350; // ms
    // constructor
    GxEPD2_374_DEPG0370(int16_t cs, int16_t dc, int16_t rst, int16_t busy);
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
    /* ---- demo 忠实版局刷支持（2026-08-18 残影修复）----
     * demo 每次局刷：硬复位→partial 初始化→写双 RAM→0x04/0x12/0x02，
     * 完全无状态；GxEPD2 增量路径（仅写 0x13、依赖 COG 0x10 跨刷新存活）
     * 在本面板上旧帧不可靠，是残影叠加根因 */
    void hwReset();          /**< 公开硬件复位（demo 每次刷新前必做） */
    void initFullDemo();     /**< demo Epaper_Initial_full_mode：PSR+CDI=0x97（每次全刷
                                 前必调，硬复位后重写，清局刷残留 E0/E5/PSR2） */
    void initPartialDemo(); /**< demo partial 初始化（本面板 demo 原始参数：
                                 PSR2=0x0d/CDI=0x17/E5=100；GDEY037T03 值
                                 （0x1f/0xD7/0x6E）属 SSD1680 体系张冠李戴，
                                 PSR2 位定义不同导致驱动力错乱，已弃用） */
    void demoWriteDual(uint16_t x, uint16_t y, uint16_t w, uint16_t h,
                       const uint8_t* prev_fb, const uint8_t* new_fb); /**< demo
                                 EPD_Dis_Part_RAM 忠实版：单次 partial-in 会话内
                                 连续写双平面（旧帧→0x10，新帧→0x13），无 0x92 */
    void demoWriteDualNoWindow(const uint8_t* prev_fb, const uint8_t* new_fb); /**< Plan B
                                 无窗口整屏双 RAM 写入（2026-08-20）：不发 0x91/0x90，
                                 直接整屏写 0x10 旧帧 + 0x13 新帧，靠 COG 全屏
                                 差分驱动变化像素 —— 规避 partial window 模式
                                 （三组参数实测均不能干净刷白：0x1f 体系值留浅影、
                                 0x0d demo 值无深睡不消失/有深睡仍遮盖，
                                 与 GxEPD2 "多数 UC 面板禁用 partial window"
                                 结论一致）；代价：每次传整屏 12KB，耗时略增 */
    void demoWriteFull(const uint8_t* new_fb); /**< demo Epaper_Load_image 忠实版：
                                 真全刷写入 —— 不带 0x91/0x90 窗口指令，
                                 直接整屏写 0x13（窗口化全屏刷驱动力不足，
                                 真机验证窗口包裹的全刷仍留残影） */
    void updateDemoPartial(uint8_t passes = 1);/**< demo Epaper_Update_partial：0x04→0x12×passes→0x02。
                                 passes=2 双刷（同会话两次 0x12）：第二次 0x12 把同批
                                 RAM 差分像素再驱动一遍（COG 按内存差分非光学态，
                                 窗口时代补偿黑→白弱方向的手段，无窗口波形下
                                 单刷已洗净，双刷留作浅影回退）；耗时约 2x */
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
