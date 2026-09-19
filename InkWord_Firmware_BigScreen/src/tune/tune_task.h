#pragma once
// 波形调参测试任务（bigscreen-tune 环境）
//
// 功能：标准测试图 + 运行时参数切换，用于系统化扫描 binfast/scanq 最优参数
//
// 测试图内容（2x2 布局 + 顶部参数标签）：
//   - 左上：灰阶渐变（16 级 Bayer 抖动，验证灰阶还原）
//   - 右上：棋盘格（40px 格子，验证边界锐度）
//   - 左下：细线测试（1/2/3/5px 水平+垂直线，验证分辨率极限）
//   - 右下：文本测试（多字号英文，验证字形清晰度）
//
// 串口命令（115200 baud）：
//   n/N - 下一组参数
//   p/P - 上一组参数
//   r/R - 重刷当前（先深清再刷新）
//   i/I - 显示当前参数信息
//   w/W - 切换波形（binfast <-> scanq）
//   h/H/? - 显示帮助
//
// 预设参数组合（10 组）：
//   BF 2+2 ~ BF 5+5：binfast n1+n2 对称扫描（0.44s ~ 1.1s）
//   BF 3+5 / BF 5+3：非对称扫描（测试黑白驱动差异）
//   SQ 8+4 ~ SQ 15+5：scanq 黑饱和 + 白修复（1.32s ~ 2.2s）
//
// 用法：
//   pio run -e bigscreen-tune -t upload && pio device monitor -b 115200
//   拍照记录每组参数效果，对比找最优
#ifdef __cplusplus
extern "C" {
#endif

void tune_task(void* arg);
void tune_process_command(char cmd);

#ifdef __cplusplus
}
#endif
