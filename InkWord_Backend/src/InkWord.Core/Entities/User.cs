using InkWord.Core.Common;

namespace InkWord.Core.Entities;

/// <summary>
/// 管理后台用户（非设备终端用户）。
/// </summary>
public class User : BaseEntity
{
    public string Username { get; set; } = string.Empty;
    public string PasswordHash { get; set; } = string.Empty;
    public string DisplayName { get; set; } = string.Empty;
    public string Role { get; set; } = "Admin";   // Admin / Operator

    // 导航
    public ICollection<Device> Devices { get; set; } = new List<Device>();
}
