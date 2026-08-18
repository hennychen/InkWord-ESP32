/// EPD 帧格式与设备通信协议常量（单点定义）
///
/// 语义与固件侧对齐：
/// - 帧魔数：InkWord_Firmware/src/epd_driver.h（EPD_WIDTH/EPD_HEIGHT/EPD_FB_SIZE，
///   竖屏 240x416，1bpp，行宽 30 字节，MSB first，bit=1 白）
/// - HTTP 端点：InkWord_Firmware/src/lan_display_server.cpp
///   （POST /api/display 要求 body 恰好 12480 字节裸帧，无协议头）
/// - BLE UUID 与广播布局：InkWord_Firmware/src/ble_provision.cpp
///   （两侧常量必须同步修改）
library;

class EpdProtocol {
  EpdProtocol._();

  /* ---------------- 帧格式（与 epd_driver.h 对齐） ---------------- */
  /// 面板竖屏宽（像素）
  static const int frameWidth = 240;

  /// 面板竖屏高（像素）
  static const int frameHeight = 416;

  /// 每行字节数 = 240 / 8
  static const int bytesPerRow = frameWidth ~/ 8;

  /// 整帧字节数 = 30 x 416（POST /api/display 的 body 长度）
  static const int frameBytes = bytesPerRow * frameHeight; // 12480

  /* ---------------- HTTP API（lan_display_server.cpp） ---------------- */
  static const String pathDisplay = '/api/display';
  static const String pathWifiScan = '/api/wifi/scan';
  static const String pathWifiStatus = '/api/wifi/status';
  static const String pathWifiConnect = '/api/wifi/connect';

  /// mDNS 主机名（STA 在线模式注册 _http._tcp）
  static const String mdnsHost = 'inkword.local';

  /* ---------------- Captive Portal 兜底 ---------------- */
  static const String portalSsid = 'InkWord-Setup';
  static const String portalUrl = 'http://192.168.4.1';

  /* ---------------- BLE 配网服务（ble_provision.cpp） ---------------- */
  /// GATT 服务 UUID
  static const String bleServiceUuid = 'cc5a0001-7e8b-4c3a-9d2e-5f6a7b8c9d0e';

  /// 凭据写入特征（write）：JSON {"ssid":"...","pass":"..."}
  static const String bleCharCredsUuid = 'cc5a0002-7e8b-4c3a-9d2e-5f6a7b8c9d0e';

  /// 状态特征（read + notify）：JSON {"state":"idle|connecting|ok|fail","ip":"..."}
  /// 拒绝配网时 notify {"state":"error","reason":"..."}
  static const String bleCharStatusUuid =
      'cc5a0003-7e8b-4c3a-9d2e-5f6a7b8c9d0e';

  /// Wi-Fi 扫描特征（write 触发 + notify 推送）：
  /// 写任意 1 字节触发设备侧扫描；每 AP notify
  /// {"i":idx,"n":total,"ssid":"...","rssi":-60,"auth":true}，
  /// 结束 notify {"end":true,"n":total}，失败 {"end":true,"err":"busy|scan_failed"}
  static const String bleCharScanUuid = 'cc5a0004-7e8b-4c3a-9d2e-5f6a7b8c9d0e';

  /// BLE 广播 manufacturer data 的厂商标识（Espressif 0x02E5，小端两字节 E5 02）
  static const int bleMsdCompanyId = 0x02E5;

  /// manufacturer data 载荷布局（不含 2 字节 companyId）：
  /// [0]=协议版本(1)  [1]=Wi-Fi 状态(0 idle/1 connecting/2 ok/3 fail)
  /// [2..5]=设备 IP 四字节（0.0.0.0 = 未联网）
  static const int bleMsdPayloadLen = 6;
  static const int bleProtoVersion = 1;
}
