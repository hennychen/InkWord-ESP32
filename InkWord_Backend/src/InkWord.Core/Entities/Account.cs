using InkWord.Core.Common;

namespace InkWord.Core.Entities;

/// <summary>
/// 学习者账户（v1.5 T5.3 轻账户，ACCOUNT_MODEL_DECISION §四）。
///
/// 与管理后台 User 分表（红线 §三.5：操作员与学习者安全等级和生命周期
/// 不同，混表互相污染）；只管内容归属（Deck.OwnerId），学习记录仍挂
/// DeviceId，设备侧零感知。PasswordHash 与 User 同源 PBKDF2
/// （AuthController.HashPassword，salt:hash 十六进制对）。
/// </summary>
public class Account : BaseEntity
{
    /// <summary>登录名（唯一，App 注册入口）</summary>
    public string Username { get; set; } = string.Empty;

    /// <summary>PBKDF2 哈希（复用 AuthController 既有实现）</summary>
    public string PasswordHash { get; set; } = string.Empty;

    /// <summary>显示名（昵称，可选）</summary>
    public string DisplayName { get; set; } = string.Empty;

    // 不建 Device 关联：v2.0 经 Device.UserId 兑现绑定（决策 §四）
}
