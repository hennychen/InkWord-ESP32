# libhelix-mp3（精简解码器子集）

来源：[ESP8266Audio](https://github.com/earlephilhower/ESP8266Audio) `src/libhelix-mp3`
（RealNetworks Helix MP3 解码器提取版，RCSL 1.0/RPSL 1.0 双许可，见 LICENSE.txt）。
仅裁掉 `mpadecobjfixpt.h`/`player.h`（完整播放器示例外壳），其余原样内嵌，
**未改动解码内部实现**。

- 公开 API：`mp3dec.h`（MP3InitDecoder/MP3Decode/MP3FindSyncWord/...）
- 消费方：`src/audio_player.c`（编译开关 `HAVE_LIBHELIX_MP3`，platformio.ini 全 env 生效）
- 双轨同步：`src/CMakeLists.txt` SRCS 已登记（ESP-IDF CMake 迁移线，与 panels/ 同纪律）
- PROFILE 宏保持未定义（mp3dec.c 内 systime.h/timing 代码不参与编译）

更新方式：从上游重新拷贝同名文件覆盖即可。
