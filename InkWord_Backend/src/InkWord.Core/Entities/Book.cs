using InkWord.Core.Common;

namespace InkWord.Core.Entities;

/// <summary>
/// 书籍元数据（管理端维护，设备端拉取）。
/// 书籍文件存储于后端 data/books/ 目录（Docker volume 挂载）。
/// </summary>
public class Book : BaseEntity
{
    /// <summary>书籍唯一标识键（文件名去扩展名，如 "tangshi300"）</summary>
    public string BookKey { get; set; } = string.Empty;

    /// <summary>书名</summary>
    public string Title { get; set; } = string.Empty;

    /// <summary>作者（可空）</summary>
    public string Author { get; set; } = string.Empty;

    /// <summary>语言：zh/en</summary>
    public string Language { get; set; } = "zh";

    /// <summary>分类/标签（逗号分隔，如 "古诗,唐诗,小学"）</summary>
    public string Tags { get; set; } = string.Empty;

    /// <summary>文件大小（bytes）</summary>
    public long FileSize { get; set; }

    /// <summary>文件格式：txt/md/html</summary>
    public string Format { get; set; } = "txt";

    /// <summary>封面图 URL 或路径（可空，一期留空）</summary>
    public string CoverUrl { get; set; } = string.Empty;

    /// <summary>简介（可空）</summary>
    public string Description { get; set; } = string.Empty;

    /// <summary>是否发布（false=草稿，设备端不可见）</summary>
    public bool Published { get; set; }

    /// <summary>下载次数（设备拉取文件时 +1）</summary>
    public int DownloadCount { get; set; }
}
