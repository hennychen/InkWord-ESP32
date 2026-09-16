// [run101] 扫描量子化 GC16 专用波形（bringup 文档 §16）
//
// 背景：LCD 16bit 并行路径不执行波形 phase_times（每相位=一次 1080 行全扫
//   ≈110ms），ED047TC1 GC16 的 8~20ms 白驱回修相位被过驱 ~10 倍 → gray16
//   中间灰阶塌缩为白（§15）。本模块以「1 扫描 = 最小时间量子」重排 GC16：
//     A 段（nsat 扫）：全部驱黑饱和（from=0 已黑 → nop）；
//     B 段（mmax 扫）：目标灰阶 to 的前 map[to] 扫驱白回修，其余 nop。
//   灰阶由白回修扫描数 M=map[to] 唯一决定（饱和后状态与 from 无关）。
//
// 结构体直接组装 epdiy 内部 EpdWaveform（type=2 命中 MODE_GC16 的
//   get_waveform_index 匹配），LUT 打包格式与原波形逐位一致：
//   data[phase][to][4]，byte=from>>2，shift=6-2*(from&3)，码值 0=nop/1=黑/2=白。
//
// 参数可经 /wf 端点运行时重排（免重烧迭代标定），见 lan_image.c wf_handler。
//
// [run109] binfast 二值快速波形：黑白跃迁各只驱 n1/n2 扫（默认 3+3），
//   同色/灰阶目标均 nop。服务 1bit 内容（更快更锐）；2bit 中间级会被
//   饱和翻转为纯黑白，灰阶标定仍走 scanq。参数经 /wf?wf=binfast&bn1=..
//   &bn2=.. 真机迭代（不足签名：黑不黑/白不白）。
#pragma once

#include <stdbool.h>
#include <stdint.h>
#include <epd_internals.h>

#ifdef __cplusplus
extern "C" {
#endif

/// A+B 段总扫描数上限（32 扫 ≈ 3.5s，超过原 GC16 的 3.3s 无收益）
#define SCANQ_MAX_PHASES 32

/// 按默认参数构建波形（幂等；开机初始化时调用一次）
void scanq_init(void);

/// 波形对象指针（scanq_init 之后有效；赋给 hl.waveform 即生效于下一次更新）
const EpdWaveform* scanq_waveform(void);

/// 参数重排：nsat>=1, mmax>=1, nsat+mmax<=SCANQ_MAX_PHASES,
///   0<=map16[t]<=mmax。非法参数返回 false 且保持原状态不变。
bool scanq_rebuild(int nsat, int mmax, const int map16[16]);

/// 读取当前参数（map16 可为 NULL）
void scanq_get_params(int* nsat, int* mmax, int map16[16]);

/// binfast 波形对象（首次调用按默认 n1/n2 懒构建；赋给 hl.waveform 生效）
const EpdWaveform* binfast_waveform(void);

/// 参数重排：n1>=1, n2>=1, n1+n2<=SCANQ_MAX_PHASES。非法返回 false 且不变。
bool binfast_rebuild(int n1, int n2);

/// 读取当前 binfast 参数（可为 NULL）
void binfast_get_params(int* n1, int* n2);

#ifdef __cplusplus
}
#endif
