/** 环境配置 — 开发环境 */
export const environment = {
  production: false,
  /** 后端 API 基础地址。
   *  5090：避开 macOS AirPlay 接收器默认抢占的 5000/7000 端口
   *  （Chrome 走 IPv6 ::1 会连到 Control Center 致 CORS/连接异常）。 */
  apiUrl: 'http://localhost:5090/api',
};
