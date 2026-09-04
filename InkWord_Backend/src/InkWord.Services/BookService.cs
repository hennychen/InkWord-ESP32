namespace InkWord.Services;

/// <summary>
/// 书籍文件管理服务（文件校验/格式检测）。
/// 一期轻量：仅做文件存在性校验，复杂处理（EPUB 解析等）后续扩展。
/// </summary>
public class BookService
{
    private readonly string _bookDir;

    public BookService(IConfiguration config)
    {
        _bookDir = config["Books:Dir"] ?? "data/books";
    }

    /// <summary>确保书籍文件目录存在</summary>
    public void EnsureDir() => Directory.CreateDirectory(_bookDir);

    /// <summary>检查书籍文件是否存在</summary>
    public bool FileExists(string bookKey, string format)
        => File.Exists(Path.Combine(_bookDir, $"{bookKey}.{format}"));

    /// <summary>获取书籍文件路径（安全校验后）</summary>
    public bool TryResolveSafePath(string bookKey, string format, out string fullPath)
    {
        fullPath = string.Empty;
        // 防路径穿越：BookKey 只允许字母数字中文和 -_
        if (bookKey.Any(c => !char.IsLetterOrDigit(c) && c != '-' && c != '_' && c > 127))
            return false;
        fullPath = Path.Combine(_bookDir, $"{bookKey}.{format}");
        return File.Exists(fullPath);
    }
}
