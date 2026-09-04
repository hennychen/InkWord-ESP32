using InkWord.API.DTOs;
using InkWord.Core.Common;
using InkWord.Core.Entities;
using InkWord.Core.Repositories;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;
using Microsoft.EntityFrameworkCore;
using InkWord.Infrastructure.DbContext;

namespace InkWord.API.Controllers;

/// <summary>
/// 管理端书籍接口（阅读器后端，2026-09-05）。
/// </summary>
[ApiController]
[Route("api/admin/books")]
[Authorize(Roles = "Admin,Operator")]
public class AdminBookController : ControllerBase
{
    private readonly IBookRepository _bookRepo;
    private readonly AppDbContext _db;
    private readonly string _bookDir;

    public AdminBookController(IBookRepository bookRepo, AppDbContext db, IConfiguration config)
    {
        _bookRepo = bookRepo;
        _db = db;
        _bookDir = config["Books:Dir"] ?? "data/books";
    }

    /// <summary>书籍列表（分页 + 过滤）</summary>
    [HttpGet]
    public async Task<IActionResult> List([FromQuery] BookQueryDto query, CancellationToken ct)
    {
        var (items, total) = await _bookRepo.QueryAsync(
            query.Keyword, query.Language, query.Published, query.Page, query.Size, ct);
        return Ok(ApiResponse<PagedResult<object>>.Ok(new PagedResult<object>(
            items.Select(b => new
            {
                b.Id, b.BookKey, b.Title, b.Author, b.Language, b.Tags,
                b.FileSize, b.Format, b.Published, b.DownloadCount, b.CreatedAt,
            }).ToList(), total, query.Page, query.Size)));
    }

    /// <summary>上传书籍文件（multipart：file + metadata）</summary>
    [HttpPost("upload")]
    public async Task<IActionResult> Upload([FromForm] BookCreateDto meta,
        IFormFile file, CancellationToken ct)
    {
        if (file == null || file.Length == 0)
            return BadRequest(ApiResponse.Fail(400, "文件不能为空"));
        if (file.Length > 10 * 1024 * 1024) // 10MB 上限
            return BadRequest(ApiResponse.Fail(400, "文件不能超过 10MB"));

        // 确定格式
        var ext = Path.GetExtension(file.FileName).ToLowerInvariant() switch
        {
            ".md" => "md",
            ".htm" or ".html" => "html",
            _ => "txt"
        };

        // 生成 BookKey（文件名去扩展名，转小写，替换非字母数字为 -）
        var baseName = Path.GetFileNameWithoutExtension(file.FileName);
        var bookKey = System.Text.RegularExpressions.Regex
            .Replace(baseName, @"[^a-zA-Z0-9\u4e00-\u9fff]+", "-").Trim('-').ToLowerInvariant();
        if (string.IsNullOrEmpty(bookKey)) bookKey = $"book-{Guid.NewGuid():N}"[..16];

        // 检查重复
        var existing = await _bookRepo.GetByBookKeyAsync(bookKey, ct);
        if (existing != null)
            return BadRequest(ApiResponse.Fail(400, $"BookKey '{bookKey}' 已存在"));

        // 保存文件
        Directory.CreateDirectory(_bookDir);
        var filePath = Path.Combine(_bookDir, $"{bookKey}.{ext}");
        using (var fs = System.IO.File.Create(filePath))
            await file.CopyToAsync(fs, ct);

        // 创建书记录
        var book = new Book
        {
            BookKey = bookKey,
            Title = meta.Title.Trim(),
            Author = meta.Author?.Trim() ?? "",
            Language = meta.Language ?? "zh",
            Tags = meta.Tags?.Trim() ?? "",
            Description = meta.Description?.Trim() ?? "",
            FileSize = file.Length,
            Format = ext,
            Published = false, // 默认草稿，需手动发布
        };
        await _bookRepo.AddAsync(book, ct);
        await _bookRepo.SaveChangesAsync(ct);

        return Ok(ApiResponse<object>.Ok(new { book.Id, book.BookKey, book.Title }));
    }

    /// <summary>更新书籍元数据</summary>
    [HttpPut("{id:guid}")]
    public async Task<IActionResult> Update(Guid id, [FromBody] BookUpdateDto dto, CancellationToken ct)
    {
        var book = await _bookRepo.GetByIdAsync(id, ct);
        if (book == null) return NotFound(ApiResponse.Fail(404, "book not found"));

        if (dto.Title != null) book.Title = dto.Title.Trim();
        if (dto.Author != null) book.Author = dto.Author.Trim();
        if (dto.Language != null) book.Language = dto.Language;
        if (dto.Tags != null) book.Tags = dto.Tags.Trim();
        if (dto.Description != null) book.Description = dto.Description.Trim();
        if (dto.Published.HasValue) book.Published = dto.Published.Value;
        book.UpdatedAt = DateTime.UtcNow;

        await _bookRepo.SaveChangesAsync(ct);
        return Ok(ApiResponse.Ok());
    }

    /// <summary>删除书籍（软删除 + 移除文件）</summary>
    [HttpDelete("{id:guid}")]
    public async Task<IActionResult> Delete(Guid id, CancellationToken ct)
    {
        var book = await _bookRepo.GetByIdAsync(id, ct);
        if (book == null) return NotFound(ApiResponse.Fail(404, "book not found"));

        // 移除文件
        var filePath = Path.Combine(_bookDir, $"{book.BookKey}.{book.Format}");
        if (System.IO.File.Exists(filePath))
            System.IO.File.Delete(filePath);

        await _bookRepo.DeleteAsync(book, ct);
        await _bookRepo.SaveChangesAsync(ct);
        return Ok(ApiResponse.Ok());
    }

    /// <summary>发布/取消发布</summary>
    [HttpPost("{id:guid}/publish")]
    public async Task<IActionResult> Publish(Guid id, [FromQuery] bool publish, CancellationToken ct)
    {
        var book = await _bookRepo.GetByIdAsync(id, ct);
        if (book == null) return NotFound(ApiResponse.Fail(404, "book not found"));

        book.Published = publish;
        book.UpdatedAt = DateTime.UtcNow;
        await _bookRepo.SaveChangesAsync(ct);
        return Ok(ApiResponse.Ok());
    }

    /// <summary>书籍详情（含阅读统计）</summary>
    [HttpGet("{id:guid}")]
    public async Task<IActionResult> Detail(Guid id, CancellationToken ct)
    {
        var book = await _bookRepo.GetByIdAsync(id, ct);
        if (book == null) return NotFound(ApiResponse.Fail(404, "book not found"));

        var readerCount = await _db.ReadingProgresses.AsNoTracking()
            .CountAsync(p => p.BookId == id, ct);
        var avgPage = await _db.ReadingProgresses.AsNoTracking()
            .Where(p => p.BookId == id && p.TotalPages > 0)
            .AverageAsync(p => (double?)p.CurrentPage / p.TotalPages * 100, ct) ?? 0;

        return Ok(ApiResponse<object>.Ok(new
        {
            book.Id, book.BookKey, book.Title, book.Author, book.Language,
            book.Tags, book.FileSize, book.Format, book.Published,
            book.DownloadCount, book.Description, book.CreatedAt,
            readerCount,
            avgProgressPct = Math.Round(avgPage, 1),
        }));
    }
}
