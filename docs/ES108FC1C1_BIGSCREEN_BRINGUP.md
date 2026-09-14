# ES108FC1C1 10.8" 墨水屏全屏管线定稿方案（run87 / LAN M1 run99）

> 状态：**管线定稿 + LAN 图片上传显示已真机验收**（2026-09-14 标定帧屏显验收全要素正确、无任何偏移；2026-09-15 run99 七次 4bpp 灰阶上传全部成功、769s 运行 0 异常）
> 适用：InkWord_Firmware_BigScreen 工程，ESP-IDF 5.5.3，epdiy V7 管线
> 目的：沉淀 10.8" 大屏 bring-up 的终局配置、实证依据与被否方案，供后续开发（bus_speed 爬升、产品化、其他多批面板接入）直接引用，避免重走弯路。
> 章节导航：§1-§9 全屏管线定稿 → §10-§11 局部刷新 → §12-§14 LAN 图片上传显示（里程碑 M1）

---

## 0. 结论速览

| 项 | 值 |
|---|---|
| 面板 | ES108FC1C1，1920×1080，16bit 并行总线（epdiy V7 直驱，无控制器寄存器） |
| 硬件 | ESP32-S3（卖家开发板）+ 卖家 V7 克隆驱动板（16bit 并行直驱、板载固定升压、电源使能 GPIO46；**与主项目 EVK011 SPI 升压转接板无关**，见 §10.5） |
| 平台 | ESP-IDF 5.5.3（PlatformIO framework-espidf），env `bigscreen-demo` |
| 批结构 | LINE_BATCH=1000（`lcd_va_height` 寄存器仅 10 位 ≤1024，单批 1080 硬件不可能）→ 2 批 = 1000 + 80 |
| 健康串口签名 | 每相位 `prep=1084 cons=1080 vsync=+2 eof=+500 batches=2 va=1000/999/999/999/79`；30 相位/次更新；帧节奏 ~110ms |
| 定稿 run | run87（run79→run87 共 9 轮单变量实验收敛） |
| LAN 里程碑 | **M1 完成 = run99**：WiFi SoftAP + captive portal 自动弹窗 + 浏览器侧 16 级灰阶 4bpp 直传（§12-§14） |
| 基线白驱 | `fill_solid(1)` + `epd_hl_update_screen(MODE_GC16)`；**禁用 `epd_clear()`**（帧未等完 → 屏停在驱黑阶段，§13.2） |
| 灰阶能力 | fb 为 4bpp = **16 级灰**，页面打包顺序与 fb 布局逐位吻合 → 固件端 `memcpy` 直传（§14.2） |

**定稿五件套**（缺一不可，每项均有实证 run 编号）：

1. **run79** 上游 vroland/epdiy main 的 IDF 5.x 初始化序列（stride / reset 组 / 新启动序 + 自旋锁 / 边界 ISR 直启）
2. **run83** 边界末批 `vfp=1`、`ckv=余数+1`（与中间批公式对齐）
3. **run84** `auto_next_frame=false`（引擎批间停住，边界 ISR 硬同步垂直相位）
4. **run85** `dummy_bytes=0`（IDF 5.5.3 外围已在行首插空字，dummy 造成双插整帧右移）
5. **run87** 像素域字相位补偿：末批行前补 `z=2` 字节黑垫（吸收边界重启早锁存）

---

## 1. 管线背景

```
feed 线程×4 → line queue(31槽/线程) → retrieve_line_isr(bounce 填充)
   → GDMA bounce buffer(2×4行) → LCD peripheral FIFO → 16bit 并行总线
   → 面板源极锁存(LE)；行推进 = RMT CKV 脉冲；帧首 = STV
```

- 触发线 `l=63`：line queue 填够双 bounce 即 `epd_lcd_start_frame()`。
- **eof 换算**：`eof×4 = DMA 行数`；正常全帧活跃行 = 1080 → 270 eof；本管线实测 500 eof/帧（见 §4 说明，**无害，勿再修**）。
- **cons 上限** = `lines_total`（retrieve_line_isr 超限返回 false 不递增）。
- 批边界：batch0 输出 1000 行后引擎 vsync 停止（auto_next=false），vsync ISR 设置 batch1 时序并重启引擎 + 续发 CKV。

---

## 2. 定稿配置详解（含代码位置）

### 2.1 [run79] 上游 main 的 IDF 5.x 初始化序列

文件：`components/epdiy/src/output_lcd/lcd_driver.c`

- init：`#if IDF>=5.3` 调 `lcd_ll_set_dma_read_stride(lcd.hal.dev, lcd.config.bus_width)`（=16，16bit 总线镜像线宽，**保留**）。
- `epd_lcd_start_frame()`（L200-254）：
  - 水平时序：`lcd_ll_set_horizontal_timing(dev, le_high-(dummy>0), line_front_porch, lcd_res_h+(dummy>0), end_line)`（dummy=0 时自动退化为纯值）。
  - 垂直时序：`lcd_ll_set_vertical_timing(dev, 1, 0, initial_lines, 1)`（签名 = vsw, vbp, vact, vfp；**初批 vbp=0**，卖家旧值 1 在 IDF 5.5 上相位漂移）。
  - reset 组：`gdma_reset → lcd_ll_stop → lcd_ll_reset → lcd_ll_fifo_reset`。
  - 双 bounce 预填 → `gdma_start`。
  - **启动序**（`frame_start_spinlock` 自旋锁内）：`STV=0 → start_ckv_cycles(initial_lines+5) → delay(1行) → STV=1 → delay(1行) → delay(ckv_high/10) → lcd_ll_start`。CKV 先行、引擎最后，相位确定。
- 边界 = **vsync ISR 直启**（L314-335）：设时序 → `lcd_ll_start` → `delay(line_length_us)` → `start_ckv_cycles(ckv)`。

### 2.2 [run83] 边界末批 vfp=1

```c
lcd_ll_set_vertical_timing(lcd.hal.dev, 1, 0, vertical_lines % LINE_BATCH, 1);
ckv_cycles = vertical_lines % LINE_BATCH + 1;
```
上游原值 vfp=10/ckv=+10 与中间批（vfp=1/+1）不一致；屏显实证末批物理位置偏移与 vfp 差值相关，对齐后消除该分量。

### 2.3 [run84] auto_next_frame=false

`epd_lcd_start_frame()` 内 `lcd_ll_enable_auto_next_frame(dev, false)`（L232）。
机理：true 时 batch0 结束瞬间引擎**自动**开跑 batch1 行周期，而 vsync ISR 有 1-3 行周期服务延迟 → 早跑的行落在错误物理行（屏显：末批整体下移 2-5 行，"1000" 标签跨批剪切）。false 后引擎批间停住，相位完全由边界 ISR 的 start+delay+CKV 决定。

### 2.4 [run85] dummy_bytes=0

init（L514-519）：`dummy_bytes = 0;`（上游 main 为 `bus_width/8`=2）。
机理：IDF 5.5.3 的 LCD 外围**本身**已在每行首插入 1 空字；dummy 字节 + 水平时序额外 LE（`le_high-1 / res_h+1`）造成**双插** → 整帧右移 8px：左缘白隙、右边框（x=1912-1919 = 行末字节）被裁掉看不见、十字/CENTER 偏右。回退 dummy=0 后三症全消，且批边界闭环不受影响（**证明边界修复真源是 reset 组/stride/启动序，而非 dummy**）。
注意：`bb_size`、水平时序公式、`fill_bounce_buffer` 偏移均引用 `dummy_bytes`，改 0 全自动退化，无需他改。

### 2.5 [run87] 末批字相位像素补偿

文件：`components/epdiy/src/output_lcd/render_lcd.c` `retrieve_line_isr()`（L71-84）：

```c
if (ctx->lines_consumed >= 1000) {
    const int z = 2;
    int n = ctx->display_width / 4;
    memmove(buf + z, buf, n - z);
    memset(buf, 0x00, z);
}
```
机理：边界重启后引擎行窗从流内**早 1 字**（16bit 总线 1 字 = 2B = 16px）开始锁存 → 末批整体左移 16px（行尾 2B 撕裂缝落入右边框黑区不可见）。末批每行前补 z=2 黑垫，垫字被早锁存吸收，显示窗恰好落回真实行数据；垫字位置对应左边框黑区，不可见。
量级实证链：run85 左移 16px → run86 z=1 补 8px 剩 8px（"好了一些"）→ run87 z=2 归零。

---

## 3. 被否方案清单（实证负结果，禁止回走）

| run | 方案 | 结果 |
|---|---|---|
| run76 | 卖家 epdiy2 参数原样（vbp=0、无 fifo_reset、旧启动序） | IDF 5.5.3 批边界停摆：cons=1008、eof 冻 250/帧、尾 72-80 行陈数据（run75 及以前一切伪影真源） |
| run77 | 边界 ISR 内 gdma_reset+gdma_start 重武装 | 不生效：cons=1016、eof 仍 250 |
| run78 | 边界专用任务重武装（ISR 给信号量） | 与 eof ISR 竞态：eof 随机冻 540/80 行 |
| run80/81 | `output_always_on` true/false 对照 | 边界恢复不依赖该位；终局 false |
| run82 | 边界 stop+fifo_reset | eof 修到精确 270 但破坏 eof/bounce 轮转同步：k≥1 相位仅消费 168 行（灾难性） |
| — | 单批 1080 行 | `lcd_va_height` 10 位寄存器硬件不可能 |
| — | RMT 单发 1080+ CKV | `tx_loop_num_chn` 10 位 ≤1023；现方案 1005/81 两发安全 |

---

## 4. 健康签名与诊断体系

### 4.1 串口健康签名（每相位一行）

```
I (2395) epd_lcd: frame k=0 done: prep=1084 cons=1080 vsync=2 eof=270 batches=2 va=1000/999/999/999/79
```

- `prep=1084`：feed 准备行 = 1080 + 4 管线 slack；`cons=1080` = 全行消费闭环。
- `vsync=+2/帧` = 2 批各 1 次；`batches=2`。
- `eof=+500/帧`（=2000 行槽）：blanking/空闲行消费，**屏显完美实证无害**；run82 曾"修"到 270 但毁轮转——**不要再动**。
- `va=写/回读×4`：1000（写值）/999（写后回读=存值-1）/999（reset 后）/999（start 后）/79（边界回读）；任一变异 = 时序寄存器未生效。
- 异常判读：cons 冻 <1080 = 边界停摆；eof 冻 = FIFO 不排空；vsync≠+2 = 批数异常。

### 4.2 诊断代码 inventory（稳定后裁剪清单）

- `lcd_driver.c`：`g_lcd_dbg_vsync/eof`、`g_dbg_va_*` 五元 + `epd_lcd_dbg_va/va2` getter、start_frame/ISR 内回读点。
- `render_lcd.c`：每帧 done 日志（feed_done 后）、`start_frame @ l=` ROM printf、lq 填充诊断。
- 裁剪时机：bus_speed 爬升试验完成后；保留每帧 done 日志降级为 DEBUG 级即可。

### 4.3 工具链

```bash
# 串口捕获（pyserial 自动重连；reset 参数=RTS 脉冲，实测对本板无效，靠烧录复位）
nohup ~/.platformio/penv/bin/python /tmp/serread.py /tmp/runNN.log &
# 构建+烧录（烧录前必须 pkill 读取器释放端口）
pkill -9 -f serread.py; pio run -e bigscreen-demo -t upload --upload-port /dev/cu.wchusbserial110
```
- 同内容重画 diff 为空不更新：**只有每次烧录后的首帧才上屏**；串口验证必须紧跟烧录启动捕获。
- run86 曾出现一次烧录后硬挂（日志停 "starting update"）：同固件重烧即愈，判定偶发坏烧录；若再现先重烧再排障。

---

## 5. 屏显缺陷 → 根因映射表（照片判读方法论）

屏竖放/横放判读：帧缓冲水平线(y=const) 在竖放照片中呈垂直带，反之亦然。

| 屏显特征 | 根因 | 定稿处置 |
|---|---|---|
| 尾 72-80 行陈数据/粗带/糊块（run75 及以前全部伪影） | 批边界停摆，eof 冻 250 | run79 五件套之 1 |
| 整帧右移 8px：左缘白隙 + 右边框被裁 + 十字偏右 | dummy 双插（外围自插空字 + dummy/额外 LE） | run85 dummy=0 |
| 末批整体下移 2-5 行（"1000" 标签跨批剪切、垂直线段 gap） | auto_next=true 时 ISR 延迟致引擎早跑 | run84 false |
| 末批整体左移 16px（标签下半剪切、底边框错位） | 边界重启早锁存 1 字 | run87 z=2 补偿 |
| 跨批边界要素剪切/台阶 | 通用特征：任何跨 y=1000 的要素都是批相位试金石 | 标定帧 "1000" 标签即为此设计 |

---

## 6. 文件地图

| 文件 | 定稿相关段 |
|---|---|
| `components/epdiy/src/output_lcd/lcd_driver.c` | `epd_lcd_start_frame()` L200-254；vsync ISR 边界分支 L314-335；init `dummy_bytes=0` L514-519、stride、always_on(false)；诊断变量 L123-140 |
| `components/epdiy/src/output_lcd/render_lcd.c` | `retrieve_line_isr()` 补偿块 L71-84；每帧 done 日志 |
| `components/epdiy/src/output_lcd/lcd_driver.h` | `epd_lcd_dbg_va/va2` 声明 |
| `src/test/demo/demo_seller.c` | `draw_line_map_frame()` 标定帧（白底/黑边框/十字/每100px刻度+数字/CENTER）+ 常驻重画循环；run91 局刷重置对策（`RUN91_RESET_K`） |
| `src/lan/lan_image.c`（613 行） | 作业投递架构（5 作业枚举 + 队列，规避 httpd 低优先级驱屏饿死）；GC16 基线白驱；4bpp/1bit 双格式解包；captive portal（DHCP DNS 下发 + UDP/53 劫持 + 404→302）；诊断端点 `/white /black /pattern /pol /clear /status` |
| `src/lan/lan_page.h`（270 行） | 上传页面全部前端逻辑：旋转/镜像/适配三模式/缩放偏移/亮度对比度 Gamma 反色/三种输出模式/预览即所得/下载 PNG |
| `tools/decode_waveform.py` | 离线解码 epdiy 波形头（`code(phase,to,frm) = (flat[phase*64+to*4+(frm>>2)] >> (6-2*(frm&3))) & 3`），§13.3 波形方向结论来源，无需真机即可判读驱黑/驱白/nop 序列 |

---

## 7. 遗留与后续开发指引

1. **bus_speed 爬升**：现 5（卖家 demo 17）。每档单变量烧录 + 串口签名 + 屏显三验；签名变异即回退。
2. **VCOM 校准**：本克隆板**软件 VCOM 不存在**（I2C 控制链不可用，见 §10.4）；硬件路径 = 板载 VCOM 电位器手调（早期 bring-up 记录标签 -2.45V，位置待卖家 V7 板复核；曾误记的 "EVK011 J1-24 测试点" 系主项目 SPI 升压板原理图误植，已撤销，见 §10.5）；灰阶产品化前必做（未更新区灰染根因，见 §10.3）。
3. **波形**：现用 `epdiy_ED047TC1` 非本屏专用；GC16 30 相位 ~3.3s/次更新（含 diff 计算的全流程 ~4.35s）；专用波形可提质提速。GC16 四组合的驱动方向已离线解码 + 真机双向验证（§13.3），换波形时须重跑 `tools/decode_waveform.py` 复核。
4. **补偿参数表化**：`z=2` 与边界行 `1000` 现硬编码于 render_lcd.c；接入其他多批面板时移入面板描述符（boundary_line、word_phase_pad）。
5. **CENTER 文字锚点**：demo 内左锚 `W/2-60`（视觉偏左 ~20px），纯标定帧美观问题，可改 `EPD_DRAW_ALIGN_CENTER` + `W/2`。
6. **eof=500 机理**（blanking 消费路径）未完全闭环解释；屏显无害，留作学术遗留，**禁止**以"修 eof"为目标的改动（run82 教训）。
7. 诊断裁剪见 §4.2；裁剪后重验串口签名基线并更新本文 §4.1。
8. **captive portal 弹窗未取证**：`dns_hijack_task` 无查询计数日志，无法从串口判断 iOS/Android 探测是否真被劫持（run99 上传成功仅证明 HTTP 链路通，可能系手输 IP）。补一行 DNS 查询计数日志即可闭环。
9. **16 级灰阶的视觉质量未量化**：run99 已证实 4bpp 数据链路端到端打通（§14.5），但灰阶层次是否受 VCOM 固定偏移压缩（同 §10.3 灰染机理）需灰阶标板照片判读；若层次不足，先做 §7.2 VCOM 校准而非改波形。
10. **局刷空 diff 的 789ms 固定开销**：重复内容 no-op 仍耗 789ms（diff 计算 + 全屏比对），可在应用层加内容指纹短路（§14.5）。

---

## 8. 实验史速览（run76→run99）

| run | 单变量 | 串口签名 | 屏显 |
|---|---|---|---|
| 76 | 卖家参数原样 | cons=1008 eof=250 冻 | 停摆伪影 |
| 77 | ISR 内 gdma 重武装 | cons=1016 eof=250 | 无效 |
| 78 | 边界任务重武装 | eof 随机冻 | 更乱 |
| 79 | 上游 main 初始化序列整体移植 | k=0 完美 / k≥1 退化 | 部分 |
| 80 | +va 回读诊断 | 全 30 相位 cons=1080 eof=500 | 首幅完整正确帧（余 1000 错位） |
| 81 | always_on=false | 同 80 | 同 80 |
| 82 | 边界 stop+fifo_reset | eof=270 但 k≥1 cons=168 | 灾难，回退 |
| 83 | 边界 vfp 10→1 | 同 81 | 错位减量（余 2-5 行垂直） |
| 84 | auto_next=false | 同 81 | 垂直对齐（余整帧右移被注意） |
| 85 | dummy=0 | 同 81 | 水平全对齐（余末批左移 16px） |
| 86 | 补偿 z=1 | 同 81（首烧偶发硬挂，重烧愈） | 左移剩 8px |
| 87 | 补偿 z=2 | 同 81 | **完全正常，无偏移，定稿** |
| 88 | 局部刷新六步循环（update_area） | 每步全扫签名同 81，273s 0 错 | 分区位置/文字/跨边界全对；**未更新区 ~45 循环后灰染** |
| 89 | 软件 VCOM（tps_set_vcom+epd_poweron） | I2C 未安装 → abort 无限重启 | 无（回退） |
| 90 | 回退 run88 固件 | 同 81 | 恢复 run88 行为 |
| 91 | 局刷产品对策（每 8 局刷插全屏 GC16 重置） | 322s 5 重置 0 错，签名同 81 | **对策闭环，局刷产品层可行** |
| 92 | LAN M1 首烧（WiFi AP + httpd） | AP/httpd 就绪 | set_all_white 基线 no-op（diff 空无扫描） |
| 93 | LAN 基线改显式 diff | diff 仅 13 行 | 8bpp 尺寸误：fb 实为 4bpp，1MB 堆溢出 |
| 94 | LAN 4bpp 解包修正 | diff=0,0,1920,1080，签名同 81 | **整屏驱黑**（front/back 语义反） |
| 95 | LAN 缓冲语义修正（front=目标图） | **实为 run94 同一固件**（新码未烧录，§13.4） | 纯黑屏（原"待上传复验"结论**作废**） |
| 96 | 基线改 `epd_clear()` | 6623ms，**无 `frame k=` 日志**（该路径不可观测） | 未记录 |
| 97 | 作业投递架构 + `epd_clear_area_cycles` | **同一行 epd_clear 仅 643ms**（vs run96 6623ms，10 倍漂移） | **纯黑屏**（66 帧中前 10 帧驱黑，帧未等完即返回） |
| 98 | 基线改 GC16 白驱 `fill_solid(1)` + 5 作业诊断端点 | `baseline err=0 ms=4240`、30 帧全扫、签名同 81 | **全白 ✓ 纯黑屏三轮误判闭环**；`to=15,from=0` 驱白首次真机验证 |
| 99 | captive portal + 4bpp 灰阶直传 + 页面拆 `lan_page.h` | 7 次 `recv ok: 1036800 B (4bpp gray16)`、`job 0/1/2/3` 全 `err=0`、660 帧签名健康、769s **0 异常** | **图片显示验收 ✓**；局刷裁剪 `361,0,1198×1080` 生效；空 diff no-op **789ms**（vs 全屏 4354ms） |

---

## 9. 卖家示例（ED060KD1-EpdiyV7）与定稿实现逐层对照

> 对照源：`Info/ED060KD1-EpdiyV7示例和操作说明/`（示例 main.c + 操作说明/epdiy2 库）
> 结论先行：卖家示例是 **IDF 4.x + 中小屏单跑 demo**；定稿实现 = 卖家基线 + **IDF 5.x HAL 适配** + **批边界相位硬同步** + **像素域字相位补偿** 三层改造，另加管线健壮性修复。board 层参数两版完全一致。

### 9.1 示例应用层（main.c vs src/test/demo/demo_seller.c）

| 项 | 卖家示例 | 定稿 demo | 备注 |
|---|---|---|---|
| init 目标屏 | **ES120**（2560×1600, 16bit, speed 12） | ES108FC（1920×1080, 16bit） | 示例内 ES108FC/ED060KD1 定义存在但**未被 init 使用** |
| ES108FC bus_speed | 17（定义未用） | **5** | 爬升试验待做（§7.1） |
| 波形 | ES108FC→epdiy_ED047TC1 | 同 | ✓ 一致 |
| VCOM | `epd_set_vcom(1560)` | 同调用保留 | 两版均**无效**：v7 板 `vcom_ctrl=false` 且 `tps_set_vcom` 被注释（卖家库 epd_board_v7.c L130/L213）；实靠硬件电位器 |
| 电源时序 | 每次 update 前后 poweron/off + 结尾 epd_deinit+deep sleep | 常驻上电循环 | run48/49 实证频繁开关机黑屏 |
| 更新内容 | clear + DU/GL16/GC16 多段动画 | 常驻标定帧 GC16 | run45 起 |
| LUT | EPD_LUT_64K | 同 | ✓ 一致 |

### 9.2 驱动库层（epdiy2 vs components/epdiy，16bit 总线口径）

| # | 位置 | 卖家 epdiy2（IDF 4.x） | 定稿（IDF 5.5.3） | 实证 |
|---|---|---|---|---|
| 1 | init | 无 stride | `lcd_ll_set_dma_read_stride(16)`（IDF≥5.3 新寄存器） | run79 包 |
| 2 | start_frame 垂直时序 | `(1,1,N,1)` vbp=**1** | `(1,0,N,1)` vbp=**0** | run79 |
| 3 | start_frame reset 组 | gdma_reset+stop+fifo_reset | 增 **lcd_ll_reset** | run79 |
| 4 | start_frame auto_next | **true** | **false** | run84（末批下移 2-5 行） |
| 5 | 启动序 | STV0→1µs→CKV(N+**6**)→1µs→STV1→(line−ckv/10−1)→引擎，无锁 | STV0→CKV(N+**5**)→1行→STV1→1行→ckv/10→引擎 + **frame_start_spinlock** | run79 |
| 6 | 边界末批 | vfp=**10** / ckv=余数+**10** | vfp=**1** / ckv=余数+**1** | run83 |
| 7 | dummy 字节 | `(bus_width==8)` → 16bit 下 =0 | 常量 **0** | 终值相同但路径不同：run79 曾引上游 main 的 dummy=2 → 整帧右移 8px，run85 回退（**上游 main 在 16bit+IDF5.5 上是弯路**） |
| 8 | phase_cycles / always_on / hsync_position | `(0,(dummy>0),1)` / false / 1 | 同 | ✓ 一致 |
| 9 | 边界批数 | `vertical_lines / LINE_BATCH` 整除 | 同 | **非差异**：语义=边界转换次数，1080→1000+80 两版均成立 |
| 10 | 像素补偿 | 无 | `retrieve_line_isr` y≥1000 行前补 z=2 黑垫 | run87（边界早锁存 1 字） |

### 9.3 InkWord 管线健壮性修复（卖家库无，早期 run 沉淀）

- run34：retrieve 空队列 fallback（连空 64 次才置 error，垫白线）——1920 宽产速不均防死锁。
- run42：feed 线程 2→4 + 绑核 + notify 全量分发（原实现对核二连发致线程 2/3 永闲）。
- EOF 双填修复、25s 超时兜底、批间无缝续批、IDF 5.5 兼容宏。
- run48/49：常驻上电取代逐次 poweron/off。

### 9.4 board 层

- epd_board_v7 配置两版一致：`ckv_high_time=60 / line_front_porch=4 / le_high_time=4`；`vcom_ctrl=false` 一致。

### 9.5 一句话总结

卖家示例在 IDF 4.x 上靠旧 HAL 的隐式行为（FIFO/相位自恢复）掩盖了批边界相位问题 ；迁到 IDF 5.5.3 后这些隐式行为消失，必须显式硬同步（§2 五件套）。卖家 16bit dummy=0 与定稿终值吻合，证明 dummy 从来不是 16bit 总线的必需品；真正必需的是 stride/reset 组/启动序（边界修复真源）+ auto_next/vfp（垂直相位）+ z=2（水平字相位）。

---

## 10. 局部刷新测试（run88→run90）

### 10.1 机制：全屏扫描 + diff 掩码（非裁剪扫描）

- `highlevel.c` 算完 `diff_area` 后**故意扩张为全屏**再调 `epd_draw_base`（上游留有 FIXME），仅 `dirty_lines` 传入渲染器。
- `render_lcd.c` 非脏行填 `0x00`；`mask_line_buffer` 注释证实 **nop 编码 = 0x00**（上游约定）。
- 推论：STV 每帧仍扫满 1080 行，批边界**永远在帧行 1000** → `z=2` 补偿门限（帧相对行）在局部刷新下天然正确，无需改动。
- “局部”的收益是**未变区不被重新驱动**（无闪烁），**不省扫描时间**：GL16 局部 ≈ 3284ms vs 全屏同值；DU 5 相位 ≈ 554ms。

### 10.2 run88 六步循环结果（273s / ~45 循环 / 0 错误）

| 步 | 区域 (x,y,w×h) | 意图 | 串口/屏显 |
|---|---|---|---|
| P0 | 300,200,700×300 | batch0 内 GL16 | diff 700×299 ✓ |
| P1 | 300,850,700×300 | 跨 y=1000（底裁 230）单批 | ✓ 无垂直错位 |
| P2 | 1100,1000,700×70 | batch1 区内 | diff 700×68 ✓ |
| P3 | 1400,20,300×1040 | 竖条跨批边界（高>1000→2批） | ✓ **无 16px 水平阶**（z=2 门限实证） |
| P4 | 同 P0 区 MODE_DU | 快刷（diff 仅 96×30 文字） | 554ms ✓ |
| P5 | 300,600,400×40 | 小文本区 | ✓ |

屏显照片判读：五区位置/边框/计数文字/奇偶块全部正确；跨边界连续；背景标定帧内容保持。**位置层闭环。**

### 10.3 发现的硬件限制：未更新区灰染（wash）

- 现象：~45 次全扫循环后，**从未被 diff 触及的区域**（左带 x<300、右带 1000<x<1400）退为中灰；反复白填区保持亮白；色带边界与更新区 x 范围精确对齐。
- 根因链：nop 扫描槽像素电极 0V，而**硬件固定 VCOM 存在偏移** → 每个 nop 槽净电场泵电荷；累积电荷 ∝ 扫描次数；强驱动（GC16/GL16 活动行）可覆写 → 近期更新区清晰。
- 佐证：卖家单次更新 demo 清晰；run87 单帧清晰；重启后 frame 0 GC16 恢复清晰（双稳不被破坏，可逆）。
- 产品对策：① 每 K 次局部更新插入一次全屏 GC16 重置累积；② 小改动优先 MODE_DU（5 相位泵电荷少）；③ 硬件调 VCOM（§10.5）可根治。

### 10.4 run89 被否：软件 VCOM 不存在（禁止回走）

- 实验：恢复 `tps_set_vcom` DAC 写入 + demo 调 `epd_set_vcom(1560)+epd_poweron()`。
- 结果：`i2c driver not installed` → ESP_ERR_INVALID_STATE → **abort 无限重启**（26 次/40s）。
- 根因：本克隆板 board v7 的 I2C 控制链**不可用**——卖家原库同样注释掉 `i2c_param_config/i2c_driver_install/pca9555_set_value/tps_set_vcom`；电源/VCOM 由卖家 V7 克隆板板载升压硬件固定生成（GPIO46 使能，见 board_config.h `BS_EPD_POWER_EN`）。
- 纪律：**禁止**在本板调用 `epd_poweron/epd_set_vcom/tps_*`（set_vcom 仅存值无害但无意义）；电源路径维持 raw GPIO `BS_EPD_POWER_EN`。

### 10.5 硬件归属澄清与调校入口（EVK011 误植撤销）

- **EVK011 与本屏无关**：EVK011/EVK011-C 是主项目 InkWord_Firmware 的 COG 屏（DEPG0370 等）**SPI 升压转接板**（24pin FPC J1、4 线 SPI、GDR/RESE 分立升压）；本屏为 16bit 并行直驱、仅适配 InkWord_Firmware_BigScreen，两者硬件家族完全不同。前版引用 "EVK011 原理图 J1-24 VCOM 测试点" 系误植，**撤销**。
- 本屏硬件链：卖家 ESP32-S3 板 + 卖家 V7 克隆驱动板（板载固定升压、GPIO46 电源使能）；VCOM 硬件调校入口 = 板载电位器（早期 bring-up 记录标签 -2.45V，位置待卖家 V7 板复核）。
- 调校方法：以“N 次局部循环后未更新区保持白”为判据收敛调节电位器；若电位器不在本板，以卖家 V7 板原理图或板上实测定位 VCOM 测试点为准。

### 10.6 run90：回退复验

回退 run89 三处改动后重烧：0 abort，签名 `prep=1084 cons=1080 vsync=+2 eof=+500 batches=2 va=1000/999/999/999/79`，六步循环恢复运行。**当前在机固件 = run88 序列 + 回退注释。**

---

## 11. 局部刷新收尾：产品对策验证（run91）

- 对策：每 8 次局刷插一次全屏 GC16 重置（`RUN91_RESET_K`，demo_seller.c）；重置帧不推进六步序列，fb 累积全图直取 update_screen。
- 结果：322s 运行（超 run88 灰染暴露窗 ~270s），5 次重置（Frame 9/18/27/…），0 错误；局刷帧与重置帧签名均同定稿。
- 结论：局部刷新**产品层可行**——K=8 窗内未更新区 nop 暴露为 run88 灰染阈值的 1/6，重置强驱动覆写泵电荷；小改动场景叠加 MODE_DU 优先（5 相位泵电荷少）。
- 纪律：灰染是硬件特性（VCOM 固定偏移），对策仅“重置累积”非“消除泵电荷”；K 放大须有实测照片证据，不得纸面推断。

---

## 12. LAN 图片上传显示（适配里程碑 M1，run92→run94）

> 本节为 M1 首版链路（仅 1bit 抖动）。run99 已扩展为 16 级灰阶 4bpp 直传 + 旋转/适配/灰阶控件 + captive portal，见 §14；基线白驱已从 `epd_clear()` 改为 GC16 diff，见 §13.5。

### 12.1 链路与用法

- env `bigscreen-lan`（`-D BIGSCREEN_LAN`）：src/lan/lan_image.c——WiFi SoftAP（SSID `InkWord-BigScreen` / `inkword123`，channel 6）+ esp_http_server（`http://192.168.4.1`）。
- `GET /` 上传页：浏览器 canvas 等比 contain（白底）→ 灰度 → Floyd–Steinberg 抖动 → 1920×1080 1bit 打包（MSB first，bit1=白，259200B）POST `/upload`；`GET /status` 状态查询。
- 设备侧：PSRAM 接收 → 解包入 epdiy 4bpp fb → 全屏 MODE_GC16（~3.3s）；`s_busy` 以 503 拒并发更新。
- 设计：解码/缩放/抖动全在浏览器侧，设备零图像解码依赖；管线复用 run87 定稿配置（LANDSCAPE 原生 1920×1080，不走旋转路径）；电源仍 raw GPIO46（禁 epd_poweron，§10.4）。
- 裸流联调：`curl -X POST --data-binary @/tmp/test_pattern.raw http://192.168.4.1/upload`（四象限图案：全黑/棋盘/竖条/斜条 + 黑框十字）。

### 12.2 踩坑（真机实证）

1. **set_all_white 基线 no-op**（run92）：双缓冲同白 → diff 空 → 无扫描无签名，屏留上一固件残影。正解：front=黑/back=白 显式构造 diff。
2. **fb 为 4bpp 非 8bpp**（run93）：`fb_size = width/2*height`（epdiy.c `epd_hl_init`）；按 8bpp 尺寸 memset/解包致 1MB 堆溢出且 diff 仅 13 行。半字节约定：偶 x=低半字节、奇 x=高半字节，白=15/黑=0（`epd_draw_pixel`）。
3. IDF http_server 枚举无 `HTTPD_503_*` 常量 → `httpd_resp_set_status` 自定义状态行。
4. **hl 缓冲语义与上游相反**（run94→run95）：本 patched 库 `epd_hl_get_framebuffer` 返回 **front_fb**（画缓冲），`epd_difference_image_cropped(to=front, from=back)`，更新驱 front 并按脏行把 back 同步为 front；按上游语义写（front=旧态）会整屏驱反（run94 front=黑/back=白 → 纯黑屏）。铁律：目标图永远写 `get_framebuffer` 返回的缓冲；强制实画时向 back 构造旧态。

### 12.3 状态（已被 §13/§14 取代）

> **本节 run95 结论作废**。复核证实 run94.log 与 run95.log 系**同一份 run94 固件**的两次运行（新码未烧录），"基线黑→白全屏实画"从未发生，真机表现为纯黑屏；`to=15,from=0`（白驱）直到 run98 才首次获得真机验证。取证方法见 §13.4，定案见 §13.5，最终验收状态见 §14.5。

---

## 13. 纯黑屏定案：`epd_clear()` 弃用与 GC16 白驱基线（run96→run98）

### 13.1 三轮误判时间线（同一现象、三个不同根因）

| 轮 | 固件 | 真实根因 | 当时误判 |
|---|---|---|---|
| 1 | run94 | front/back 缓冲语义写反 → 整屏驱黑 | 归因正确 |
| 2 | run95 | **新码未烧录**，跑的仍是 run94 | 误以为语义已修、以为剩上传链路问题 |
| 3 | run97 | `epd_clear()` 帧未等完，屏停在驱黑阶段 | 一度怀疑波形方向反了 |

前两轮损失源于**取证不足**（未验证固件是否真被烧录），第三轮才是真根因。

### 13.2 根因：`epd_clear()` 缺少 run72 的每帧残留排空

调用链：`epd_clear()` → `epd_clear_area(full)` → `epd_clear_area_cycles(area, 3, 12)`
= 3 轮 ×（10 帧驱黑 color0 + 10 帧驱白 color1 + 2 帧浮空 color2）= **66 帧**。

该路径内部走 `epd_push_pixels_lcd`，**缺少 `lcd_do_update` 中 run72 加入的每帧残留排空**（`lq_reset` + drain `frame_done`）。run72 注释原文（`render_lcd.c` L124-130）：

> 排空上帧残留行：LCD 每帧实际消费（eof×BOUNCE_BUF_LINES）少于 lines_total（1080 产 vs ~1000 消），尾差残留队列；不清则下帧起步空间 52 < 触发线 64，feed 满队列忙等、start_frame 永不触发

三重后果：

1. **时长不可预测**：同一行 `epd_clear()`，run96 耗 6623ms、run97 仅 643ms（**10 倍漂移**）——`frame_done` 提前返回。
2. **停在驱黑阶段**：66 帧中**前 10 帧是驱黑**，中途中断 → 屏停在驱黑 → 纯黑屏。
3. **不可观测**：该路径无 `frame k=` 日志，无从定位。

纪律：**禁用 `epd_clear()` / `epd_clear_area_cycles()`**；一切清屏走 GC16 diff（可观测、有 30 帧签名）。

### 13.3 波形方向离线解码（`tools/decode_waveform.py`）

2bit 驱动码（`lut.h` L11/L13）：`0`=nop、`1`=驱黑（`DARK_BYTE 0B01010101`）、`2`=驱白（`CLEAR_BYTE 0B10101010`）。

GC16 30 相位四组合：

| to,from | 语义 | 序列 | 净时长 |
|---|---|---|---|
| 15,0 | 黑→白 | 15 nop + 15 驱白 | +550ms |
| 0,15 | 白→黑 | 15 驱黑 + 15 nop | −1020ms |
| 15,15 | 白→白 | 驱黑+驱白闪烁式 | −470ms |
| 0,0 | 黑→黑 | 全 nop | — |

run98 真机跑 `to=15,from=0` 得**全白** → 离线解码与实物一致，**极性无需反转**（`inv=0` 全程）。

**陷阱警示**：若中途停在驱黑阶段（§13.2），极易误判为“波形方向反了”而去做无谓的极性反转。判据：**必须看到完整 30 帧 `frame k=0…29` 才能对屏显下结论**。

### 13.4 取证方法论：防陈旧固件三重纪律 + 启动日志抓取

**三重纪律**（run94/run95 误判直接产物）：

1. 构建后 `stat` 核对 `firmware.bin` mtime **晚于**源码 mtime；
2. 烧录只认 `Hash of data verified` + `Hard resetting`；
3. 运行期确认 `alive [runNN]` 心跳 + `/status` + 响应体 `build` 字段（`LAN_BUILD_TAG`）。

**同源日志判据**（run94 vs run95）：两文件均 8576 B、md5 **不同**（5 处 thread 编号差异）→ 证明是两次独立运行；但文案签名逐字一致 + run95.log 落盘时刻 21:35 **等于** firmware.bin 构建时刻 + 源码编辑在 21:47:54（晚于）→ **两次跑的是同一份 run94 固件**。教训：**md5 不同 ≠ 固件不同**，须比对构建标识与 mtime 时序。

**启动日志抓取**：本板 WCH 适配器的 RTS 复位脉冲无效（§4），固件启动完即静默。若先烧录、再挂读取器，工具往返延迟会让板子已跑 15s+，错过 baseline 与 30 帧。正解：**把 `pio upload` 与 `nohup serread.py &` 压进同一条 shell 命令**（run98b 据此拿到 110 行完整启动日志）。

### 13.5 run98 定案：GC16 白驱基线 + 诊断端点体系

```c
fill_solid(1);      // front=白 / back=黑，显式构造 diff（避开 §12.2 踩坑 1 的空 diff no-op）
int berr = (int)epd_hl_update_screen(&hl, MODE_GC16, epd_ambient_temperature());
```

签名：`baseline white via GC16: err=0 ms=4240 inv=0`、`actual draw took 3177ms`、`frame k=0…29` 全到位、每帧 `prep=1084 cons=1080 vsync+2 eof+500 batches=2 va=1000/999/999/999/79`。用户目视**全白** → 纯黑屏闭环。

诊断端点（**本轮最大工程收益：判读不再依赖重烧**）：

| 端点 | 作业 | 用途 |
|---|---|---|
| `/white` | JOB_WHITE(0) | 全屏白，验 `to=15,from=0` |
| `/black` | JOB_BLACK(1) | 全屏黑，验 `to=0,from=15` |
| `/pattern` | JOB_PATTERN(2) | 四象限图案（左上全黑/右上 16px 棋盘/左下 8px 竖条/右下 24px 斜条 + 16px 黑框十字），验 x/y 映射与批边界 |
| `/pol` | — | 运行时切换 `s_inv` 极性反转，隔离“映射问题 vs 极性问题” |
| `/clear` | JOB_CLEAR(4) | `epd_clear()` 对照（**不可靠，仅留作对比**） |
| `/status` | — | JSON：busy / inv / build / 尺寸 |

**架构铁律**：httpd 任务默认优先级 `tskIDLE_PRIORITY+5`=5，而 feed 线程 `epd_prep`（`render.c` L265-270）与 `lan_image_task` 均为 `configMAX_PRIORITIES-1` → **绝不可在 httpd 任务内直接驱屏**（低优先级主控会在帧完成后永久饿死：frame_done 就绪也无任务可调度，静默挂死，真机实证，见 `main.c` L50-53）。一切驱屏由 httpd 投递作业、`lan_image_task` 执行。

---

## 14. LAN 页面增强与 M1 验收（run99）

### 14.1 captive portal：连 WiFi 后自动弹浏览器

三步链路（IDF 5.5.0）：

1. **DHCP 下发 DNS**：`esp_netif_set_dns_info(s_ap_netif, ESP_NETIF_DNS_MAIN, &dns)`。该 API 对带 `ESP_NETIF_DHCP_SERVER` 标志的 netif 会转调 `dhcps_dns_setserver_by_type`（`esp_netif_lwip.c:2069`），从而让 DHCP 报文携带 DNS 选项。**必须在 `esp_wifi_start()` 之后**调用；且须留住 `esp_netif_create_default_wifi_ap()` 的返回句柄（run98 前直接丢弃）。注意 IDF **无** `esp_netif_dhcps_set_dns()`。
2. **UDP/53 全域劫持**：自写极简 DNS（`dns_hijack_task`，prio 5，与驱屏无关），对任意 QNAME 一律回 A 记录 = 192.168.4.1；answer 用 `0xC00C` 名字压缩指针指回偏移 12 的 QNAME（省去重抄域名）。解析时须跳过 QNAME labels 并校验 `lbl<=63` 防越界。
3. **未注册路径 302 → `/`**：`httpd_register_err_handler(httpd, HTTPD_404_NOT_FOUND, portal_redirect_handler)`，带 `Cache-Control: no-store`。

触发原理：iOS `captive.apple.com/hotspot-detect.html`、Android `connectivitycheck.gstatic.com/generate_204`、Windows `msftconnecttest.com/connecttest.txt` 拿不到预期响应 → 判定为门户网络 → 弹窗。
串口签名：`dhcps DNS -> 192.168.4.1 : ESP_OK` + `DNS hijack up on :53 -> 192.168.4.1`。

**双栈陷阱**：`CONFIG_LWIP_IPV6=y` 下 `esp_netif_dns_info_t.ip` 是 `esp_ip_addr_t`（= `ip_addr_t`，含 u_addr union），而 `esp_netif_set_ip4_addr()` 只收 `esp_ip4_addr_t*` → 编译报 incompatible pointer type。正解：用 lwIP `IP_ADDR4(&dns.ip, 192,168,4,1)`（双栈下一并设 `IPADDR_TYPE_V4`，单栈退化为纯 IPv4 赋值），需 `#include <lwip/ip_addr.h>`。

### 14.2 4bpp 16 级灰阶直传（本轮最大能力升级）

洞察：fb 本就是 4bpp = **16 级灰**（§12.2 踩坑 2 已纠正尺寸），但 run92-98 只用 0/15 两级，**浪费了 GC16 的灰阶能力**。

关键：页面打包顺序（`p>>1` 定字节、`p&1` 定高/低半字节、15=白/0=黑）与 epdiy fb 布局**逐位吻合** → 固件端只需 `memcpy`：

```c
if (s_raw_len == FB_BYTES) memcpy(fb, s_raw, FB_BYTES);   // 4bpp：布局一致，零转换
```

错一位即整屏错位，故必须以 `frame k=` 签名 + `/pattern` 交叉验证。
格式判别用 `content_len`：`FB_BYTES`(1,036,800)=4bpp / `RAW_BYTES`(259,200)=1bit；1bit 路径保留（memset 全黑底 + 逐像素置白）。
`/upload` 响应体含 `bytes`/`inv`/`build`；日志 `recv ok: %u B (%s)`。

### 14.3 页面能力清单（`lan_page.h`）

拆分为独立头文件的理由：run98 前 HTML 以 C 字符串内嵌于 lan_image.c，加控件后会突破 300 行字符串拼接、与驱动逻辑混杂不可维护；本项目无文件系统与资源打包机制，故沿用“字符串常量 + include”。

- **旋转/镜像**：0/90/180/270 + 水平/垂直镜像；**竖图自动转 90°**（横屏面板）
- **自适应显示**：contain / cover / stretch 三模式 + 缩放 0.1~3 + XY 偏移；canvas CSS `max-width:100%;max-height:58vh`
- **灰阶处理**：亮度 ±100 / 对比度 0.3~3 / Gamma 0.3~3 / 反色；处理链 = 灰度(.299/.587/.114) → 对比度 → 亮度 → 钳位 → Gamma → 反色
- **三种输出模式**：`gray16`（默认推荐，4bpp 直传）/ `fs`（Floyd–Steinberg 抖动，1bit）/ `thr`（阈值 1~254，1bit）
- **预览即所得**：打包后**反解回 canvas**（`q*17` 即 `q*255/15`），所见即屏显
- **变换统一走 `setTransform` 链**：`translate(W/2+ox, H/2+oy)` → `rotate` → `scale(sx*(mirH?-1:1), sy*(mirV?-1:1))` → `drawImage(-w/2,-h/2)`；旋转 90/270 时 iw/ih 互换
- 下载 PNG；诊断按钮直连 §13.5 端点
- **性能**：滑块拖动只重绘变换（`drawOnly()`），**220ms debounce** 后才做像素级处理（`process()`）——1920×1080 像素循环在手机上不可每帧跑

### 14.4 工程细节

- httpd：`recv_wait_timeout=60` / `send_wait_timeout=30`（默认 5s，1MB 上传必超时）+ `lru_purge_enable=true`；8 个 URI
- 接收缓冲：`heap_caps_malloc(FB_BYTES, MALLOC_CAP_SPIRAM)`（原 RAW_BYTES，扩容 4 倍）
- `EpdDrawError` **无 typedef**（该库只有 `enum EpdDrawError`）→ 用 `int berr = (int)epd_hl_update_screen(...)`
- 基线后 `memset(hl.back_fb, 0xFF, FB_BYTES)`，确保首次上传 diff 为 to=图像/from=白

### 14.5 真机验收数据（`/tmp/run99.log`，1290 行，769s）

| 项 | 实测 |
|---|---|
| 启动 | `baseline white via GC16: err=0 ms=4288 inv=0`、`dhcps DNS -> 192.168.4.1 : ESP_OK`、`DNS hijack up on :53`、`LAN ready [run99]` |
| 4bpp 上传 | **7 次** `recv ok: 1036800 B (4bpp gray16)`，格式判别 **0 误判** |
| 作业全绿 | `job 3`(UPDATE) ms=4351/4354/4355/4359/4364；`job 2`(PATTERN) ms=5486/5491；`job 1`(BLACK) ms=4370；`job 0`(WHITE) ms=4369 —— **全部 err=0** |
| 局刷裁剪生效 | `diff area: x:361, y:0, w:1198, h:1080`（缩放/偏移后图像仅占中段，两侧白边未变 → 自动裁剪） |
| 空 diff no-op | 重复上传同图 → `diff area: x:1920,y:1080,w:0,h:0` → **ms=789**（vs 全屏 4354ms，**5.5× 提速**），印证 §12.2 踩坑 1 机制正向生效 |
| 帧健康 | 660 行 `frame k=`，全部 `prep=1084 cons=1080 batches=2 va=1000/999/999/999/79`；`actual draw took` 3224~3309ms（11 次，抖动 <3%） |
| 异常 | `Guru/abort/assert/Backtrace/E (/failed` 计数 = **0** |
| 心跳 | `alive [run99] hb=65 busy=0 inv=0`（10s 间隔，全程 busy 归零 = 无作业泄漏） |
| 资源 | RAM 11.6%（38092B）、Flash 27.6%（867961B）；零 `lan_` 编译警告 |

**待补取证**：弹窗是否自动出现（§7.8）、灰阶视觉层次（§7.9）。
