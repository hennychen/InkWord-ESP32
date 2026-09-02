/**
 * @file selftest_golden.h
 * @brief 黄金帧基准表（T2.2）——tools/gen_golden.py 自动生成，勿手改
 *
 * 生成流程（首次基线 / 布局或字库变更后更新）：
 *   1. 烧 demo 自检固件：pio run -e inkword-s3-demo -t upload
 *   2. 串口捕获自检 dump：
 *      pio device monitor | tee /tmp/golden_dump.txt
 *      （开机自动跑自检序列，每页输出 [GOLDEN] BEGIN/END base64 段）
 *   3. 生成基准：python3 tools/gen_golden.py /tmp/golden_dump.txt \
 *        -o src/selftest_golden.h
 *   4. 人工目视确认 demo 屏末帧与预期一致后重新编译烧录，自检从
 *      NO BASELINE 转为 PASS/FAIL 判定模式
 *
 * 帧格式：epd_gfx_read_window(0,0,W,H) 整帧——行主序 MSB-first，
 * 每行 (W+7)/8 字节，bit=1=黑（与 §9.4 差分影子同格式）。
 * 风险联动：字库 bin / 词库 / NVS 设置状态变化会合法改变帧——更新
 * 基准时核对 gen_golden.py 写入的 META（日期/源文件/尺寸），布局
 * 参数无关的字库升级须同步重 dump（PANEL_COMPAT_DESIGN D2 风险项）。
 *
 * 占位空表（首版机制入库，基线待 3.7" 真机 dump 生成）：GOLDEN_COUNT=0
 * 时消费侧零遍历，自检输出 NO BASELINE 并附 dump 段供生成。
 */
#ifndef INKWORD_SELFTEST_GOLDEN_H
#define INKWORD_SELFTEST_GOLDEN_H

#define GOLDEN_COUNT 0

#endif /* INKWORD_SELFTEST_GOLDEN_H */
