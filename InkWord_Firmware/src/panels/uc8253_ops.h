/**
 * @file uc8253_ops.h
 * @brief UC8253 族 GxEPD2 路径通用 ops 模板（P1-b①，2026-09-05）
 *
 * 背景：DEPG0370（3.7"）与 3.1" 320x240 两块 UC8253 屏的 ops 包装
 * 六函数逐字等价（仅 s_epd2 类型/几何不同），每屏各复制 ~150 行。
 * 本宏将其收敛为单点：面板单元 #include 本头后展开
 *   UC8253_DEFINE_OPS(s_epd2, g_panel_xxx);
 * 即得六个 static ops 函数（函数名/行为与合并前逐字一致，序列
 * 字节不动铁律；desc 注册处 .ops.xxx = panel_xxx 引用零改动）。
 *
 * 函数体调用的族方法（GxEPD2_EPD 子类须提供）：
 *   init / hwReset / initFullDemo / demoWriteFull / updateDemoPartial /
 *   initPartialDemo / demoWriteDualNoWindow / writeImageForFullRefresh /
 *   writeScreenBuffer / refresh / powerOff / hibernate
 *
 * —— P1-b② 待办（2026-09-05 预研结论，实施前置：① 3.1" 屏 bring-up
 * 定稿（PSR/序列字节未定，序列含 0x24 余量填充与试参数，与 DEPG0370
 * 存在实质字节差异，非纯几何换皮）；② 黄金帧基线补齐（GOLDEN_COUNT
 * 现为 0，DEPG0370 为现役主力屏无回归网不动序列）——两条件满足后：
 * 将 GxEPD2_374_DEPG0370 / GxEPD2_310_320x240 两类（仅 WIDTH/HEIGHT/
 * full/partial_refresh_time 四常量 + 序列字节不同）合并为数据驱动的
 * 单 UC8253 类（构造参数化几何与时序，序列字节表由面板文件提供），
 * 随后可整体移除 GxEPD2 lib_deps（Adafruit GFX 保留），全工程统一
 * 「desc + 手写 bus 序列」单一风格。
 *
 * —— 序列语义与调优史（原两面板文件注释合并，勿删）——
 * · full_refresh：demo 忠实版真全刷（硬复位清局刷残留 E0/E5/PSR2 →
 *   full 初始化 PSR+CDI → 无窗口整屏写 0x13 → 0x04/0x12/0x02）。
 *   不能用 demoWriteDual 全屏参数代替——窗口包裹的全屏刷驱动力不足，
 *   真机实测留残影（2026-08-18）。
 * · write_full：竖屏原始帧直通全刷（epd_full_refresh / LAN 语义），
 *   frame=NULL 清白；本路径不维护 prev 帧一致性，调用方须强制下一次
 *   全刷（main.cpp ui_force_full_refresh_next() 已保证）。
 * · partial：Plan B 无窗口整屏双 RAM 差分局刷（2026-08-20 取代窗口
 *   路径）：不发 0x91/0x90，整屏写 0x10 旧帧 + 0x13 新帧，COG 全屏
 *   差分驱动变化像素。窗口模式三组参数实测均不能干净刷白，与 GxEPD2
 *   「多数 UC 面板禁用 partial window」结论一致，弃用；代价：每次传
 *   整屏（SPI @20MHz 下 9.6~12.5KB 仅 ~5-6ms，可忽略）。每次局刷前
 *   硬件复位 COG 状态归零；0x10 必须每次显式重写（单平面写已实验
 *   证伪 2026-08-21：0x12 后 COG 不自动 new→old，差分基准落后一帧
 *   → 连续局刷残迹）。passes 双刷：单次翻转不彻底时第二次 0x12 再
 *   驱动一遍（desc.passes 按屏调优：单相 LUT 面板双刷会过驱动伪影）。
 * · init：串口诊断关闭 / initial（复位+上电）/ 复位脉宽 20ms / 常规
 *   复位脚（原 epd_driver_init 内 s_epd2.init(0, true, 20, false)）。
 */
#ifndef INKWORD_UC8253_OPS_H
#define INKWORD_UC8253_OPS_H

#include <stdint.h>

/* 展开六个 static ops 函数（面板文件全局作用域调用一次）：
 *   INST —— 面板文件的 static GxEPD2_EPD 子类实例名（s_epd2）
 *   DESC —— 面板 desc 全局符号（g_panel_xxx，write_full 取几何） */
#define UC8253_DEFINE_OPS(INST, DESC)                                      \
    static int panel_init(void)                                            \
    {                                                                      \
        INST.init(0, true, 20, false);                                     \
        return 0;                                                          \
    }                                                                      \
    static int panel_full_refresh(const uint8_t *frame)                    \
    {                                                                      \
        INST.hwReset();          /* 清局刷残留 E0/E5/PSR2 */               \
        INST.initFullDemo();                                               \
        INST.demoWriteFull(frame);                                         \
        INST.updateDemoPartial(); /* 0x04/0x12/0x02 全局刷/局刷同款 */      \
        return 0;                                                          \
    }                                                                      \
    static int panel_write_full(const uint8_t *frame)                      \
    {                                                                      \
        if (frame) {                                                       \
            INST.writeImageForFullRefresh(frame, 0, 0,                     \
                                          DESC.panel_w, DESC.panel_h);     \
        } else {                                                           \
            INST.writeScreenBuffer(0xFF);                                  \
        }                                                                  \
        INST.refresh(false); /* 全刷 */                                    \
        INST.powerOff();                                                   \
        return 0;                                                          \
    }                                                                      \
    static int panel_partial(const uint8_t *prev, const uint8_t *new_,     \
                             uint8_t passes)                               \
    {                                                                      \
        INST.hwReset();          /* 每次局刷前硬件复位，COG 状态归零 */     \
        INST.initPartialDemo();                                            \
        INST.demoWriteDualNoWindow(prev, new_); /* 0x10+0x13 双 RAM */     \
        INST.updateDemoPartial(passes);                                    \
        return 0;                                                          \
    }                                                                      \
    static void panel_power_off(void)                                      \
    {                                                                      \
        INST.powerOff(); /* 0x02 关高压 rails（VCI 3.3V 保持供电） */       \
    }                                                                      \
    static void panel_deep_sleep(void)                                     \
    {                                                                      \
        INST.hibernate(); /* 0x02 下电 + 0x07/0xA5 深睡，硬复位唤醒 */      \
    }

#endif /* INKWORD_UC8253_OPS_H */
