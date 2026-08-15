using InkWord.Core.Common;

namespace InkWord.Core.Entities;

/// <summary>
/// OTA 固件升级包。
/// </summary>
public class OtaPackage : BaseEntity
{
    /// <summary>语义化版本号，如 1.2.0</summary>
    public string Version { get; set; } = string.Empty;

    /// <summary>固件二进制下载 URL（OSS/MinIO）</summary>
    public string Url { get; set; } = string.Empty;

    /// <summary>MD5 校验值（十六进制）</summary>
    public string Md5 { get; set; } = string.Empty;

    /// <summary>固件字节数</summary>
    public long Size { get; set; }

    /// <summary>发布说明</summary>
    public string ReleaseNotes { get; set; } = string.Empty;

    /// <summary>是否已发布（设备可见）</summary>
    public bool Published { get; set; }

    /// <summary>目标硬件型号</summary>
    public string TargetBoard { get; set; } = "esp32-s3";
}
