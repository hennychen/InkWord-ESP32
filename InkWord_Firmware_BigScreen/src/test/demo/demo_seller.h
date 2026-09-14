// 卖家 demo 复刻入口声明（BIGSCREEN_SELLER_DEMO 构建使用）。
// 作用：以卖家 ED060KD1-EpdiyV7 示例为"已知良好基准"在本板跑完整
// 演示流程（文字 → DU 进度条动画 → 三张图片 GC16），用于与
// epdiy_inkword_fixed（含本项目管线修复）的输出做 A/B 对照。
#pragma once

// 独立任务跑卖家 demo 流程（app_main 主任务栈不够 printf/heap 打印）。
void demo_seller_task(void* arg);
