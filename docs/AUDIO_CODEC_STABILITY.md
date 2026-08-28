# 音频杂音修复与 codec 稳定性加固（2026-08-28）

> 范围：InkWord_Firmware 音频播放链（I2S + ES8311 + NS4150B）。
> 三项主题：首播杂音根除、ES8311 I2C 挂死防御、采样率全链统一 48kHz。
> 全部经真机验证闭环（杂音三轮听感、深睡唤醒两轮）。

---

## 0. 背景与现象

- **现象**：部分单词首次播放有「噗/咔」杂音，同曲重播正常；多数杂音集中在第一次。
- **硬件拓扑**：ESP32-S3 I2S（BCLK=4 / LRCK=5 / DOUT=6）→ ES8311 codec（I2C 地址 0x18，SDA=38 / SCL=39，MCLK=GPIO0 输出 256×fs 实线）→ NS4150B 功放（板载 R10 上拉常开，无 MCU 控制线，**解 mute 即可闻**）。
- **采样率事实（修复前）**：开机默认 44100（I2S 初始装载）／UI 提示音 16000（gen_ui_sounds.py）／单词 MP3 48000（后端 Piper TTS 实测）——三档并存。

## 1. 首播杂音：根因与修复

### 1.1 根因链

```
play_by_ext()
  ├─ es8311_dac_start()          ← 先上电解 mute（NS4150B 常开，喇叭已通）
  └─ play_mp3() 首帧解码
       └─ audio_set_sample_rate(48000)
            ├─ I2S 驱动卸载/重装  → MCLK 失钟窗口 + 跳频（44.1k→48k）
            └─ es8311_set_sample_rate → I2C 重写 REG02-08 分频
```

解 mute 状态下经历失钟/跳频/分频重写瞬态，被常开功放放大为可闻杂音。
**第一次播放有频率跳变、第二次同频所以正常**——与现象完全吻合。

关键佐证（codec I2C 失联 no-op 态反证）：codec 挂死（I2C NACK、寄存器残留）时
首播/重播均无杂音 → **纯 I2S 重装（失钟/跳频）本身不可闻；杂音主源是 mute
缺失窗口内 codec 分频寄存器的真实重写瞬态**。修复设计据此聚焦。

### 1.2 修复（audio_player.c）

| 修改点 | 内容 |
|---|---|
| `play_wav` | `audio_set_sample_rate()` 先行，`es8311_dac_start()` 后置——切换时 codec 处于 mute 态，瞬态不可闻 |
| `play_mp3` | `dac_on` 局部标志：首帧 set_rate 完成后才首次 `dac_start`；中途变率（拼接文件罕见）直接切，杂音容忍 |
| `play_by_ext` | 移除先行 `dac_start`（已下放） |
| `audio_set_sample_rate` | 新增 `s_cur_rate` 同频短路——同频直接返回，连重播的残余失钟窗口一并消除（零开销） |
| `audio_deinit` | `s_cur_rate = 0`（驱动已卸，短路判据失效） |
| `audio_play_test_tone` | 改走 `audio_set_sample_rate()`（曾只同步 codec 分频不重装 I2S，异频残留变调） |
| `audio_task` | 播放前 `!es8311_present()` 自愈重试 `es8311_init()`（见 §2.3） |

### 1.3 验证

三轮真机听感（用户确认）：首播干净、重播正常；重播无 `sample rate` 日志
证明同频短路生效。48k 统一（§4）后全链同频，会话内短路全程命中。

## 2. ES8311 I2C 挂死：发现与三级防御

### 2.1 现象与根因

验证期间发现：MCU 复位后 `es8311_probe` 持续 NACK，但音频仍能播放
（I2S 数据通路与 DAC 模拟通路独立存活，靠复位前寄存器残留出声）。

- **根因**：codec 处于时钟使能态（REG01=0x3F）+ 已配置运行态时，MCLK
  （GPIO0）突然停止 → 内部时钟域挂死 → I2C 地址检测器不再响应。
- **诊断特征**：总线空闲电平健康（SDA=1 / SCL=1，非总线死锁），仅芯片
  不认地址。退避重试（900ms 跨度）无效——非延迟问题。
- **触发器**：macOS pyserial 打开串口的 DTR 毛刺（CP2102 RTS 接 EN，
  rts/dtr 预置 False 也无法避免）；**产品级同款场景：深睡唤醒**
  （codec 供电保持 + MCU 复位）。
- **影响**：出声正常但 I2C 全失——音量调节失效、静默收口失效（底噪回归）。
- **确定性恢复**：断电冷启动，或跑一次 esptool 完整序列（经验可救回）。

### 2.2 总线恢复（i2c_bus.c / i2c_bus.h）

新增 `i2c_bus_recover()`：卸载 I2C 驱动 → GPIO 开漏接管 → 空闲电平诊断
日志 → 9×SCL 脉冲（每拍检查 SDA 释放）→ 地址事务重同步（START + 0x30 +
ACK 槽）→ STOP → 重装驱动。`bus_install_driver()` 提取为 init/recover 共用。
对「时钟域挂死」无效（芯片问题非总线问题）但无害，是标准 I2C 疑难杂症工具。

### 2.3 探测三级链与播放自愈（es8311.c / audio_player.c）

```
es8311_init 探测：NACK → 300ms 退避重试 → i2c_bus_recover() → 再退避
audio_task 播放前：!es8311_present() → es8311_init() 自愈
```

## 3. 深睡唤醒挂死防御（REG01 静止化）与实测

### 3.1 防御设计（es8311.c）

`es8311_deinit()`（suspend 序列）尾部新增：

```c
i2c_write(REG_CLK_MGR01, 0x00);   /* 失 MCLK 前关时钟使能 */
```

让芯片以**静止态**渡过无钟窗口（深睡期间数字域断电、MCLK 停止）；
唤醒后 `es8311_init` 在 MCLK 重建后重写 0x3F，顺序正确。

调用链审计（无缺口）：

```
power_enter_sleep() → audio_deinit() → es8311_deinit() → REG01=0x00
                   → esp_deep_sleep_start()（MCLK 停）
唤醒 → power_init() → setup → audio_init() → es8311_init()（MCLK 重建后 probe）
```

### 3.2 实测验证（2026-08-28，PM_SLEEP_TIMEOUT_MIN=1 加速）

两轮深睡 → 中键唤醒（EXT1），**codec 全部存活，零失败**：

```
codec found at 0x18 (id=0x83)
deep sleep in: timer=7200s idle=60s periph=1
codec deinit (suspend)                ← REG01=0x00 写入
deep sleep now
wakeup: button (CENTER key)
codec found at 0x18 (id=0x83)         ★ 唤醒后存活
```

对比：同日运行态设备（codec 时钟使能态）被串口毛刺复位 → 三连败
（probe 持续 NACK）。防御完全生效，产品级深睡唤醒挂死隐患根除。

> 验证方法沉淀：`-DPM_SLEEP_TIMEOUT_MIN=1` build flag（power_manager.h
> 注释背书的正规途径）把入睡缩至 1 分钟；静默期内任何按键都会刷新
> idle 推迟入睡；验证后 build flag 已撤（platformio.ini 与备份逐字节一致）。

## 4. 采样率全链统一 48000

### 4.1 决策

| 方案 | 评估 |
|---|---|
| 提示音 22050 | es8311 系数表有条目（5644800=22050×256），与 44.1k 同族；但与单词 48k 之间仍双向跳变——**否** |
| **全链 48000** | 与单词 MP3（Piper 实测 48k）同频；会话内提示音↔单词零跳变，`s_cur_rate` 短路全程命中——**采用** |

### 4.2 修改

- `gpio_config.h`：`I2S_SAMPLE_RATE` 44100 → 48000（开机默认即 48k）。
- `es8311.c`：init 默认系数硬编码 44100 → 引用 `I2S_SAMPLE_RATE` 宏
  （消除双源失配隐患——若只改宏不改硬编码，I2S 48k / codec 44.1k 系数
  在 dac_start 先于 set_rate 的窗口变调）。
- `gen_ui_sounds.py`：`SR` 16000 → 48000；4 个提示音重新生成至
  `tools/ui_sounds/`（48kHz/mono/PCM16/<0.3s）。
- es8311 系数表按 `rate × 256` 匹配 MCLK：48000 → 12288000 明确命中。

### 4.3 效果与边界

开机 → 提示音 → 单词 → 重播全链 48k，会话内**零 I2S 重装、零 codec
分频重写**。真机日志 `I2S audio initialized @ 48000Hz` 确认。

边界：嵌入人声 WAV（bring-up 判据资产）仍为 44.1k，**保留不迁移**——
一次跳变顺带验证 set_rate 路径，且时序修复后跳变无杂音。

## 5. 修改文件清单

| 文件 | 修改内容 | 备份 |
|---|---|---|
| `src/audio_player.c` | 起播时序重排 + `s_cur_rate` 同频短路 + 播放自愈 | .bak5 / .bak6 |
| `src/es8311.c` | 探测三级递进链 + deinit REG01 静止化 + 默认系数宏同源 | .bak2 / .bak3 |
| `src/i2c_bus.c` / `.h` | `i2c_bus_recover()`（9×SCL + 地址事务 + STOP）+ `bus_install_driver()` 共用提取 | .bak1 |
| `src/gpio_config.h` | `I2S_SAMPLE_RATE` 48000 | .bak1 |
| `tools/gen_ui_sounds.py` | `SR` 48000 | .bak1 |
| `tools/ui_sounds/*.wav` | 4 个 48k 提示音重生成 | — |
| `platformio.ini` / `src/power_manager.c` | 验证期临时项（1 分钟入睡 / 诊断日志），验证后撤净，无净变化 | .bak9 / .bak2 |

## 6. 遗留事项

1. **SD 卡提示音拷贝**：`tools/ui_sounds/` 下 4 个 wav 需人工拷入 SD 卡
   `/sdcard/audio/ui/`（当前缺失，`ui_sfx` 静默跳过并打
   `ui sfx missing` 警告）。
2. **codec 挂死硬件根治**：供电受控（load switch / EN），复位路径同步
   断电——超出软件范围；软件防御已覆盖深睡场景，串口毛刺属开发期问题。
3. **串口监听纪律**（开发期）：macOS pyserial open 必毛刺复位设备并可能
   打挂时钟使能态 codec；监听须在设备进入被测状态前打开并保持，避免
   中途重开串口。
