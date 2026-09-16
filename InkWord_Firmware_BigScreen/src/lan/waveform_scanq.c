// [run101] 扫描量子化 GC16 专用波形实现（设计依据见头文件与 bringup 文档 §16）
#include "waveform_scanq.h"

#include <string.h>
#include <esp_log.h>

#define TAG "scanq"

// 默认参数依据（§15.2 剂量分析 + run100 真机塌缩签名）：
//   nsat=10：黑饱和 ≈10×110ms=1.1s，覆盖原波形黑段意图总量 1020ms；
//   mmax=5 ：白满剂量 ≈5×110ms=550ms = 原波形白段意图总量；run100 实测
//            to>=5（白驱 >=550ms）已塌到全白 → 5 扫即白，量子上限 ~6-7 级；
//   map[t]=round(t*mmax/15)：16 级均摊到 0..5 白修扫描（初始线性，标定重排）。
#define SCANQ_DEF_NSAT 10
#define SCANQ_DEF_MMAX 5

// LUT 打包缓冲：data[phase][to][4]（与原波形同构），32×16×4 = 2KB 静态
static uint8_t s_lut[SCANQ_MAX_PHASES][16][4];
static int s_times[SCANQ_MAX_PHASES];

static EpdWaveformPhases s_phases;
static const EpdWaveformPhases* s_ranges[1];
static EpdWaveformMode s_mode;
static const EpdWaveformMode* s_modes[1];
static EpdWaveformTempInterval s_intervals[1];
static EpdWaveform s_wf;

static int s_nsat = SCANQ_DEF_NSAT;
static int s_mmax = SCANQ_DEF_MMAX;
static int s_map[16];
static bool s_built = false;

// byte=from>>2，shift=6-2*(from&3)：与 decode_waveform.py / 原波形逐位一致
//   [run109] 加 lut 参数：scanq 与 binfast 各持独立 LUT 缓冲，打包位法共用
static void set_code(uint8_t lut[][16][4], int p, int to, int from, int code) {
    lut[p][to][from >> 2] |= (uint8_t)(code << (6 - 2 * (from & 3)));
}

static void build_lut(void) {
    const int k = s_nsat + s_mmax;
    memset(s_lut, 0, sizeof(s_lut));
    for (int p = 0; p < k; p++) {
        // LCD 路径不消费 phase_times，仍填实测扫描时长便于文档/离线核对
        s_times[p] = 110;
        for (int to = 0; to < 16; to++) {
            for (int from = 0; from < 16; from++) {
                int code;
                if (p < s_nsat) {
                    // A 段：黑饱和；from=0 已黑 → nop（沿用原波形对已黑像素
                    // 的排除思路，减少无谓电荷注入，§10.3 nop 泵电荷同理）
                    code = (from == 0) ? 0 : 1;
                } else {
                    // B 段：前 map[to] 扫白回修定级，其余 nop
                    code = ((p - s_nsat) < s_map[to]) ? 2 : 0;
                }
                set_code(s_lut, p, to, from, code);
            }
        }
    }
    s_phases.phases = k;
    s_phases.luts = (const uint8_t*)s_lut;
    s_phases.phase_times = s_times;
}

void scanq_init(void) {
    if (s_built) {
        return;
    }
    for (int t = 0; t < 16; t++) {
        // round(t * mmax / 15)，纯整数
        s_map[t] = (t * SCANQ_DEF_MMAX * 2 + 15) / 30;
    }
    s_ranges[0] = &s_phases;
    s_mode.type = 2;            // MODE_GC16：get_waveform_index 按 type 匹配
    s_mode.temp_ranges = 1;
    s_mode.range_data = s_ranges;
    s_modes[0] = &s_mode;
    // 单温度区间全量程兜底（ED047TC1 原值 20-30；本板无温度传感，
    // waveform_temp_range_index 对 num_temp_ranges=1 恒返回 0）
    s_intervals[0].min = 0;
    s_intervals[0].max = 60;
    s_wf.num_modes = 1;
    s_wf.num_temp_ranges = 1;
    s_wf.mode_data = s_modes;
    s_wf.temp_intervals = s_intervals;
    build_lut();
    s_built = true;
    ESP_LOGW(TAG, "scanq built: nsat=%d mmax=%d phases=%d (110ms/scan)",
             s_nsat, s_mmax, s_nsat + s_mmax);
}

const EpdWaveform* scanq_waveform(void) {
    scanq_init();
    return &s_wf;
}

bool scanq_rebuild(int nsat, int mmax, const int map16[16]) {
    if (nsat < 1 || mmax < 1 || nsat + mmax > SCANQ_MAX_PHASES) {
        return false;
    }
    for (int t = 0; t < 16; t++) {
        if (map16[t] < 0 || map16[t] > mmax) {
            return false;
        }
    }
    // 非单调映射仅告警不拒绝（标定实验允许探索非单调响应）
    for (int t = 1; t < 16; t++) {
        if (map16[t] < map16[t - 1]) {
            ESP_LOGW(TAG, "map non-monotonic at to=%d (%d < %d)",
                     t, map16[t], map16[t - 1]);
            break;
        }
    }
    s_nsat = nsat;
    s_mmax = mmax;
    memcpy(s_map, map16, sizeof(s_map));
    build_lut();
    ESP_LOGW(TAG, "scanq rebuilt: nsat=%d mmax=%d phases=%d",
             s_nsat, s_mmax, s_nsat + s_mmax);
    return true;
}

void scanq_get_params(int* nsat, int* mmax, int map16[16]) {
    if (nsat != NULL) {
        *nsat = s_nsat;
    }
    if (mmax != NULL) {
        *mmax = s_mmax;
    }
    if (map16 != NULL) {
        memcpy(map16, s_map, sizeof(s_map));
    }
}

// ===================== binfast：二值快速波形（run109） =====================
// 1bit 内容专用：黑白跃迁各只驱 n1/n2 扫（默认 3+3 ≈ 0.66s，vs GC16 30
//   相位 3.3s、scanq 对二值 15 相位 1.65s）。翻转次数少 → 边界扩散小 →
//   更锐；同色组合 nop 零注入。扫描数经 /wf?wf=binfast&bn1=..&bn2=..
//   真机迭代（不足签名：黑不黑/白不白；过量则无谓变慢）。灰阶目标与
//   同色组合均 nop——2bit 中间级会被饱和翻转为纯黑白，故 binfast 只配
//   1bit 模式，2bit 标定仍走 scanq。
#define BF_DEF_N1 3
#define BF_DEF_N2 3

static uint8_t s_bf_lut[SCANQ_MAX_PHASES][16][4];
static int s_bf_times[SCANQ_MAX_PHASES];
static EpdWaveformPhases s_bf_phases;
static const EpdWaveformPhases* s_bf_ranges[1];
static EpdWaveformMode s_bf_mode;
static const EpdWaveformMode* s_bf_modes[1];
static EpdWaveformTempInterval s_bf_intervals[1];
static EpdWaveform s_bf_wf;
static int s_bf_n1 = BF_DEF_N1, s_bf_n2 = BF_DEF_N2;
static bool s_bf_built = false;

static void build_bf_lut(void) {
    memset(s_bf_lut, 0, sizeof(s_bf_lut));
    for (int p = 0; p < s_bf_n1 + s_bf_n2; p++) {
        s_bf_times[p] = 110;
        for (int to = 0; to < 16; to++) {
            for (int from = 0; from < 16; from++) {
                // A 段：白→黑驱黑；B 段：黑→白驱白（15=白/0=黑，阈值取 8）
                int code = (p < s_bf_n1)
                               ? ((to < 8 && from >= 8) ? 1 : 0)
                               : ((to >= 8 && from < 8) ? 2 : 0);
                set_code(s_bf_lut, p, to, from, code);
            }
        }
    }
    s_bf_phases.phases = s_bf_n1 + s_bf_n2;
    s_bf_phases.luts = (const uint8_t*)s_bf_lut;
    s_bf_phases.phase_times = s_bf_times;
}

const EpdWaveform* binfast_waveform(void) {
    if (!s_bf_built) {
        binfast_rebuild(BF_DEF_N1, BF_DEF_N2);
    }
    return &s_bf_wf;
}

bool binfast_rebuild(int n1, int n2) {
    if (n1 < 1 || n2 < 1 || n1 + n2 > SCANQ_MAX_PHASES) {
        return false;
    }
    s_bf_n1 = n1;
    s_bf_n2 = n2;
    build_bf_lut();
    if (!s_bf_built) {
        // 描述结构一次性组装（同 scanq：type=2 命中 MODE_GC16，单温度
        // 区间全量程兜底，本板无温度传感）
        s_bf_ranges[0] = &s_bf_phases;
        s_bf_mode.type = 2;
        s_bf_mode.temp_ranges = 1;
        s_bf_mode.range_data = s_bf_ranges;
        s_bf_modes[0] = &s_bf_mode;
        s_bf_intervals[0].min = 0;
        s_bf_intervals[0].max = 60;
        s_bf_wf.num_modes = 1;
        s_bf_wf.num_temp_ranges = 1;
        s_bf_wf.mode_data = s_bf_modes;
        s_bf_wf.temp_intervals = s_bf_intervals;
        s_bf_built = true;
    }
    ESP_LOGW(TAG, "binfast built: n1=%d n2=%d phases=%d (110ms/scan)",
             s_bf_n1, s_bf_n2, s_bf_n1 + s_bf_n2);
    return true;
}

void binfast_get_params(int* n1, int* n2) {
    if (n1 != NULL) {
        *n1 = s_bf_n1;
    }
    if (n2 != NULL) {
        *n2 = s_bf_n2;
    }
}
