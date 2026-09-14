#pragma once
// ============================================================
// display_config.h — ES108FC1C1-RHY 屏幕/总线/电源参数（集中管理）
// 方案 §3.2「风险控制点」：保守起步值，实测后逐项校准。
//
// 波形勘误记录（官方源码对齐纪律）：
//   方案原文写 epdiy_ED097TC2；卖家示例（Info/ED060KD1-EpdiyV7示例和
//   操作说明/ED060KD1-EpdiyV7/main.c）对同分辨率 ES108FC 使用
//   epdiy_ED047TC1 —— 以卖家示例为准。专用波形到位后替换（遗留 TODO-A）。
// ============================================================

// ES108FC1C1-RHY 几何（1920x1080）
#define PANEL_WIDTH  1920
#define PANEL_HEIGHT 1080

// 并行总线位宽（V7 板 16bit）
#define PANEL_BUS_WIDTH 16

// 总线速率：10MHz 下 ISR 消费速率（6.25us/物理行）远超双线程 LUT
// 喂线能力，首帧 64 行后即饿死（EPD_DRAW_EMPTY_LINE_QUEUE，真机实证）。
// 降至 5MHz：行周期翻倍，喂线需求减半，帧时间 27→54ms（GC16 全刷秒级
// 本就如此，无副作用）。链路稳定后可结合喂线优化回调升。
// 遗留 TODO-C：升速前需量化生产余量（bring-up 步骤 8）。
#define PANEL_BUS_SPEED_MHZ 3

// VCOM：2450mV（面板排线二维码标签 -2.45V，2026-09 实测确认）
// 卖家板无 PMIC，epd_set_vcom 为空壳——实际 VCOM 由板载电位器决定；
// 此值仅作记录与安全范围检查（1000~2500mV，panel_es108fc.c）。
// 硬件校准：万用表测电位器输出对照 -2.45V（遗留 TODO-B）。
#define PANEL_VCOM_MV 2450

// 波形起点（非专用）：ED047TC1 为 9.7" 通用波形
// TODO(A): 获取 ES108FC1C1-RHY 专用波形后替换此宏
#define PANEL_WAVEFORM epdiy_ED047TC1
