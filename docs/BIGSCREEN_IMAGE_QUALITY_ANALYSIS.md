# BigScreen 上传图片不清晰问题分析与改进记录（run111）

> 日期：2026-09-17 · 涉及工程：`InkWord_Firmware_BigScreen`
> 状态：客户端缩放已修复（run111 上板验证）；专用波形待外部输入

## 1. 问题

LAN 发送页面上传图片到 ES108FC1C1 10.8" 屏后显示不清晰。

## 2. 显示管线回顾

```
浏览器 canvas：变换（旋转/镜像/适配/缩放/偏移）→ 灰度/亮度/对比度/Gamma/USM
  → 量化（1bit FS 蛇形 / Bayer 8x8 / 阈值 / 2bit FS / 16 级灰直出）
  → POST /upload（1bit=259200B 或 4bpp=1036800B）
设备端：解包入 epdiy 4bpp fb →（4bpp 且非 2bit 白名单时设备侧 FS 二值化兜底）
  → 全屏 GC16 diff 更新（波形三态 builtin/scanq/binfast 可切换）
```

## 3. 根因分层结论（按影响排序）

| # | 层 | 根因 | 状态 |
|---|---|---|---|
| 1 | 波形 | 借用 ED047TC1（9.7" 通用）波形驱 ES108FC1C1；LCD 并行路径不执行 phase_times → §15 灰阶塌缩、边界驱不满 | **待专用波形**（display_config.h TODO(A)） |
| 2 | 客户端缩放 | drawImage 一步缩到 1920x1080，默认低质插值 + 跳采样丢细节/生摩尔纹 | **run111 已修** |
| 3 | 量化 | 1bit 抖动物理极限（~204ppi 颗粒感固有）；USM 50 对文字图偏多 | 参数调整即可（页面滑块/模式） |
| 4 | diff 刷新 | 连续上传时仅变化像素被驱，波形不匹配时小幅跃迁驱不干净 → 残影累积 | 缓解：传图前先点"全白" |

## 4. 硬件核对结论（2026-09-17，对照卖家 ESP32-S3 引脚图 + FPC_OUT1 图）

- **D7 引脚疑点已排除**：引脚图 pin12 标注 "IO19" 为笔误（GPIO18 实际在 pin26），
  卖家确认 FPC D7 接 GPIO8，GPIO19 为按键 ADC 专用。`board_config.h` 的
  `EPD_PIN_D7=8` 正确。
- D0-D15/CKH/CKV/STH/LEH/STV/I2C 全部核对一致。
- 卖家 FPC 为 **34pin 自研方案**（含 ±15V/+22V/-20V/VCOM/16bit 数据/OE/MODE 硬拉
  EPD_VDD、EPD_DIR 10K 上拉 3V3），与 NekoInk 50pin 参考引脚号完全不同 →
  **NekoInk 的 pin31 上拉 / pin43 GDSP / pin36 SDCE0 三条注意事项不适用本板**。
- 8080 并口时序为标准（NekoInk 作者确认），命令链路无问题。

## 5. 专用波形获取与接入工具链

### 5.1 现状

NekoInk 仓库（zephray/NekoInk，已停止维护，后继为 GitLab Glider）：
- 有 ES108FC1 的 39-50pin 适配板 KiCAD 文件；
- `utils/wbf_waveform_dump` 模式表中含 ES108FC1 条目
  （wbf 版本 0x18/0x20，模式 INIT/DU/GC16/GL16×3/A2），但**波形数据文件未入库**。

### 5.2 获取路径（二选一）

1. 联系 NekoInk 作者（Wenting Zhang）索取 ES108FC1 wbf dump；
2. 从联想 Yoga Book C930（该屏来源机型）固件提取 wbf，
   经 `wbf_waveform_dump` 转 iwf（descriptor .iwf + PREFIX_M*_T*.csv）。

### 5.3 转换工具（已就绪，实测通过）

`tools/iwf2epdiy.py`：iwf → epdiy 波形 C 头。已用 NekoInk 官方样例
（gdew101_gd，5 模式）验证转换正确，产出结构与 `epdiy_ED047TC1.h` 同构。

```bash
python3 tools/iwf2epdiy.py es108fc1.iwf src/config/waveform_ES108FC1.h --name ES108FC1
```

然后 `display_config.h` 的 `PANEL_WAVEFORM` 改为 `ES108FC1`。

关键映射（勿凭直觉改）：
- iwf 电压码：0=GND、1=VNEG(驱黑)、2=VPOS(驱白)、3=Keep；
- epdiy 电压码：1=驱白、2=驱黑（ED047TC1 GC16 白基线实证口径）；
- 即 iwf 1→2、2→1；
- epdiy lut 布局：`data[phase][from][4]`，byte=`to>>2`、shift=`6-2*(to&3)`
  （与 `tools/decode_waveform.py` / `waveform_scanq.c` 逐位一致）。

**首验注意**：极性映射为推定，上屏若黑白互换，对调脚本 `IWF_TO_EPDIY`
表中的 1/2 重跑即可。

### 5.4 波形到位后的验证

烧录后用页面"16 级灰阶"按钮（/grayramp 标板，run106/107）拍照判读：
带 0 黑、带 15 白、中间带 1-14 应呈渐变而非塌白（§15 签名消失即根治）。

## 6. run111：客户端缩放质量修复（已上板）

改动 `lan_page.h`：
- `drawOnly()` 前置 `makePrep(sx,sy)`：金字塔逐级减半降采样到目标分辨率，
  全程 `imageSmoothingQuality='high'`；
- prep 缓存键 = 图片序号 + 缩放量化步长（5%），滑块拖动复用不卡顿；
- prep 以原图尺寸为目的地绘制（`drawImage(prep,-w/2,-h/2,w,h)`），
  缩放补偿取整误差，旋转/镜像/偏移几何路径不变；
- 图片载入 `imgSeq++` 失效缓存。

`lan_image.c` `LAN_BUILD_TAG` → "run111"（含注释）。烧录验证：
串口心跳 `alive [run111]`，`/status` 确认，页面含新代码，
NVS WiFi 自动重连正常（STA IP 直连可达）。

## 7. 测试图

`InkWord-ESP32/inkword_test_1920x1080.png`（Pillow 生成，1920×1080 灰度，
与屏物理分辨率 1:1，用「完整显示」上传即无缩放干扰）。

| 区域 | 判读目标 |
|---|---|
| 48→12px 分级中文小字 | 文字可读下限、笔画边缘是否发虚 |
| 16 级灰阶条（带号） | 灰阶塌缩复证 / FS 抖动纹理均匀性 |
| 1/2/4/8px 横竖细线 | 细线缺失/糊灰（binfast vs builtin A/B 关键指标） |
| 16px 棋盘 | 高频翻转稳定性、鬼影 |
| 同心圆（8px 间距） | 摩尔纹、圆弧锯齿 |
| 斜线楔形 + 纯黑/纯白饱和块 | 饱和度（波形驱不满的直接证据） |

建议流程：1bit FS + builtin 刷一次 → 切 binfast 再刷对比细线/文字 →
（灰阶验证）scanq + /dither=off + 16 级灰模式。
注：1px 细线融入 FS 噪点属正常，重点看 2px 以上。

## 8. 烧录/验证环境备忘

- 环境：`pio run -e bigscreen-lan`（pio 路径
  `/Users/pm/Library/Python/3.9/bin/pio`），串口 `/dev/cu.wchusbserial10`；
- 烧录失败 "device reports readiness but returned no data" = 串口被残留
  进程占用（本次为 `/tmp/serread.py`），`lsof` 找到 kill 后重烧；
- 串口非交互抓取：python3.11 pyserial timeout 轮询，心跳每 10s 带 build 标识。

## 9. 待办

- [x] 获取 ES108FC1 面板规格书（存档 `Info/ES108FC1/ES108FC1_Ver0.1.pdf`，见 §10）
- [ ] 获取 ES108FC1 专用 wbf/iwf（卖家渠道优先；C930 固件提取备选；~~NekoInk issue~~ 用户决定不提）
- [ ] 转换接入 + /grayramp 验证灰阶塌缩根治
- [x] 真机对比 run110/run111 上传照片清晰度（整体显示较好，小字/细线/饱和度达标）
- [ ] binfast 残留调参（bn1/bn2 远程已可调，7+7/10+10 结果待记录；固件已加自动 GC16 清屏兜底）
- [ ] scanq map 标定迭代（免重烧，/wf 端点，真机拍照收敛）

## 10. ES108FC1 面板规格书分析（Tentative 0.10，2017-07-03，E Ink）

来源：GitHub `richild999/ES108FC1`，存档 `Info/ES108FC1/ES108FC1_Ver0.1.pdf`。

### 10.1 关键参数 vs 固件现状

| 规格书参数 | 值 | 固件对照 |
|---|---|---|
| 分辨率/像素间距 | 1920×1080，0.1245mm 方形像素（≈204ppi），塑料基板 | ✓ |
| 扫描方向 | Landscape pin out, **Portrait mode scan** | 印证 EPD_ROT_LANDSCAPE 必要性（竖扫横放） |
| 电源轨 | VGL -20V / VGH +28V（栅）、VNEG -15V / VPOS +15V（源）、VDD 3.3V | 与 FPC_OUT1 电压轨吻合（+22V 为 VGH 转接实测裕量） |
| **VCOM** | **-4 ~ -1V，逐屏标定值（"Adjusted"），须设在标定值 ±0.1V** | PANEL_VCOM_MV=2450（屏标签 -2.45V）在范围内；卖家板为电位器手调 |
| CKH 周期 | typ 50ns（数据链路 ~20MHz 能力） | 总线 3MHz 远低于上限（但见 10.2：非瓶颈） |
| D0-D15 setup/hold | 8ns | 3MHz 下轻松满足 |
| CKV / 刷新率 | CKV ≤200kHz；帧率上限 85Hz | scanq 110ms/扫描量子（≈9Hz）安全 |
| 上掉电时序 | 源极 VNEG→VPOS ≥1ms、栅极 VGL→VGH ≥1ms；掉电 VGL 须保持最负 | 电源常驻策略（run48/49 教训）与规格一致 |
| 光学 | 白反射率 46% typ，**CR 仅 16 typ / 12 min** | 纯黑"不够黑"部分为面板物理上限 |
| 灰阶定义 | Gn = DS+(WS-DS)×n/(m-1)（L* 线性） | scanq map 标定时应按 L* 分档 |

### 10.2 对遗留问题的归因与对策

1. **binfast 残留多两个贡献因素**：CR 上限 16 + VCOM 电位器手调偏差。
   调参顺序：bn1/bn2 加扫描 → 无改善微调 VCOM 电位器（±0.1V 粒度）→
   仍不行归为 binfast 波形能力边界（固件已加每 N 次自动 GC16 清屏兜底）。
2. **总线提速对刷新时长无帮助**：GC16 ~110ms/扫描 由波形相位与行建立时间
   决定，非 16bit 总线吞吐瓶颈（3MHz 下每行数据仅 ~3.2us），不值得冒险提速。
3. **波形不随屏出厂**：规格书明确灰阶表现"取决于控制器及配套波形文件"，
   波形由整机制造商持有——确认只能走外部获取路径。

## 11. binfast 波形能力边界与深清迭代（run112→run114 真机实证）

### 11.1 binfast 限制（定稿认知）

| 限制 | 机理 | 对策 |
|---|---|---|
| 跨帧残影累积 | 等幅方波快速波形，diff 更新小幅跃迁驱不净 | 每 N 次自动深清（`/wf?bfn=N`，默认 2） |
| 纯色黑帧/大面积实心黑 → 黑粗条纹 | LCD 并行路径每帧"产大于消"排水问题（run72 已知），纯色满屏帧最显形 | 深清弃用纯色黑帧，改反相图驱动 |
| **灰阶标板不可用** | 纯二值 LUT 无中间灰：带 0~8 全黑、9~15 全白；且左半实心黑块触发条纹 | run114 固件防护：JOB_GRAYRAMP 非 builtin 自动临时切 builtin 执行后恢复 |

**定位结论**：binfast = 快速预览模式（~2s/张 + 周期性深清）；正式显示用
builtin GC16（~4.4s/张，测试图全项达标零残影）。

### 11.2 深清序列迭代记录（真机 A/B 终审，2026-09-17）

| 版本 | 序列 | 真机结果 |
|---|---|---|
| run112 | 目标图单次 GC16 过驱（back 取反） | 残影仍在（面板物理滞留，单遍不够） |
| run113a | 白 → 黑 → 图（GC16 段） | 残影清除，但**黑粗条纹**重 |
| run113b | 白 → 黑 → 白 → 图（GC16 段） | 残影清除，仅轻微灰纹 |
| run113c | 白 → 反相图 → 图（GC16 段） | 条纹源头消除但净残影弱于 113b，弃用 |
| **run115（定稿）** | **白 → 黑 → 白 → 图（binfast 自驱段）** | **残影与灰纹均消除（用户确认）** |

**灰纹根因（run115 定论）**：GC16 纯色段的黑→白满摆幅跃迁依赖短相位
（20~30ms/相位），LCD 路径每帧"产大于消"使部分扫描行错过短相位 → 这些行
黑白转换未完成，形成横向灰带（与 §15 灰阶塌缩同源机理，用户判读"黑到白
未完成"正确）。binfast 等幅方波每次扫描均为完整满摆幅、无相位依赖 →
纯色深清段改用 binfast 自驱，从根上规避。

**定稿成本**：深清 ~9s（3 个 binfast 纯色段 + 1 次回图，含每次 ~2s 的
解包/diff 固定开销），仅 binfast 模式按周期触发（默认每 2 次，
`/wf?bfn=N` 可调）；binfast 平均 ~6s/张。结束后从 s_raw 重解包恢复上传图。

### 11.3 波形切换注意

- binfast / scanq 的远程调参（bn1/bn2、nsat/mmax/map）不持久化，重启回默认；
- 灰阶判读一律在 builtin / scanq 下进行（run114 起固件自动兜底）；
- 换波形须在非 busy 时（LUT 逐相位解包，busy 中途换 = 撕裂）。

