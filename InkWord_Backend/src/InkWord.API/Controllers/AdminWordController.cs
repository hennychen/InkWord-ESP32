using System.Globalization;
using System.Text;
using InkWord.API.DTOs;
using InkWord.Core.Common;
using InkWord.Core.Entities;
using InkWord.Core.Repositories;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;
using Microsoft.EntityFrameworkCore;

namespace InkWord.API.Controllers;

/// <summary>
/// 管理端词库接口（供 Angular 调用）。
/// </summary>
[ApiController]
[Route("api/admin/words")]
[Authorize]
public class AdminWordController : ControllerBase
{
    private readonly IWordRepository _wordRepo;

    public AdminWordController(IWordRepository wordRepo) => _wordRepo = wordRepo;

    /// <summary>B-13 单条新增（含去重校验）</summary>
    [HttpPost]
    public async Task<IActionResult> Create([FromBody] WordCreateDto dto, CancellationToken ct)
    {
        if (await _wordRepo.ExistsByTextAsync(dto.Text, dto.Tag, ct))
            return Conflict(ApiResponse.Fail(409, $"word '{dto.Text}' already exists in tag '{dto.Tag}'"));

        var maxVer = await _wordRepo.GetMaxVersionAsync(ct);
        var word = new Word
        {
            Text = dto.Text,
            Phonetic = dto.Phonetic ?? "",
            Meaning = dto.Meaning ?? "",
            Example = dto.Example ?? "",
            Audio = dto.Audio ?? "",
            Tag = dto.Tag ?? "",
            Difficulty = dto.Difficulty,
            Version = maxVer + 1,
            ChangeType = 0
        };
        await _wordRepo.AddAsync(word, ct);
        await _wordRepo.SaveChangesAsync(ct);
        return Ok(ApiResponse<Word>.Ok(word));
    }

    /// <summary>B-13 单条修改</summary>
    [HttpPut("{id:guid}")]
    public async Task<IActionResult> Update(Guid id, [FromBody] WordUpdateDto dto, CancellationToken ct)
    {
        var word = await _wordRepo.GetByIdAsync(id, ct);
        if (word == null) return NotFound(ApiResponse.Fail(404, "not found"));

        word.Text = dto.Text;
        word.Phonetic = dto.Phonetic ?? "";
        word.Meaning = dto.Meaning ?? "";
        word.Example = dto.Example ?? "";
        word.Audio = dto.Audio ?? "";
        word.Tag = dto.Tag ?? "";
        word.Difficulty = dto.Difficulty;
        word.ChangeType = 1;
        word.Version = Math.Max(word.Version, await _wordRepo.GetMaxVersionAsync(ct)) + 1;

        await _wordRepo.UpdateAsync(word, ct);
        await _wordRepo.SaveChangesAsync(ct);
        return Ok(ApiResponse<Word>.Ok(word));
    }

    /// <summary>B-13 软删除</summary>
    [HttpDelete("{id:guid}")]
    public async Task<IActionResult> Delete(Guid id, CancellationToken ct)
    {
        var word = await _wordRepo.GetByIdAsync(id, ct);
        if (word == null) return NotFound(ApiResponse.Fail(404, "not found"));

        await _wordRepo.DeleteAsync(word, ct);
        await _wordRepo.SaveChangesAsync(ct);
        return Ok(ApiResponse.Ok());
    }

    /// <summary>B-15 分页 + 多条件筛选</summary>
    [HttpGet]
    public async Task<IActionResult> List([FromQuery] WordQueryDto q, CancellationToken ct)
    {
        var (items, total) = await _wordRepo.GetPagedAsync(q.Page, q.Size, w =>
            (!q.Difficulty.HasValue || w.Difficulty == q.Difficulty.Value) &&
            (string.IsNullOrEmpty(q.Tag) || w.Tag == q.Tag) &&
            (string.IsNullOrEmpty(q.Keyword) ||
             EF.Functions.ILike(w.Text, $"%{q.Keyword}%") ||
             EF.Functions.ILike(w.Meaning, $"%{q.Keyword}%")), ct);

        return Ok(ApiResponse<PagedResult<Word>>.Ok(new PagedResult<Word>(items, total, q.Page, q.Size)));
    }

    /// <summary>B-14 批量导入 CSV（首行表头：text,phonetic,meaning,example,audio,tag,difficulty）</summary>
    [HttpPost("import")]
    public async Task<IActionResult> ImportCsv(IFormFile file, [FromQuery] string tag, CancellationToken ct)
    {
        if (file == null || file.Length == 0)
            return BadRequest(ApiResponse.Fail(400, "empty file"));

        var ok = 0; var fail = 0; var errors = new List<string>();
        var maxVer = await _wordRepo.GetMaxVersionAsync(ct);

        using var stream = file.OpenReadStream();
        using var reader = new StreamReader(stream, Encoding.UTF8);
        // 跳过表头
        await reader.ReadLineAsync(ct);

        string? line;
        while ((line = await reader.ReadLineAsync(ct)) != null)
        {
            if (string.IsNullOrWhiteSpace(line)) continue;
            var f = line.Split(',');
            if (f.Length < 3) { fail++; errors.Add($"字段不足: {line}"); continue; }

            var text = f[0].Trim();
            if (await _wordRepo.ExistsByTextAsync(text, tag, ct))
            { fail++; errors.Add($"重复: {text}"); continue; }

            var word = new Word
            {
                Text = text,
                Phonetic = f.Length > 1 ? f[1].Trim() : "",
                Meaning = f.Length > 2 ? f[2].Trim() : "",
                Example = f.Length > 3 ? f[3].Trim() : "",
                Audio = f.Length > 4 ? f[4].Trim() : "",
                Tag = !string.IsNullOrEmpty(tag) ? tag : (f.Length > 5 ? f[5].Trim() : ""),
                Difficulty = f.Length > 6 && int.TryParse(f[6], out var d) ? d : 1,
                Version = ++maxVer,
                ChangeType = 0
            };
            await _wordRepo.AddAsync(word, ct);
            ok++;
        }
        await _wordRepo.SaveChangesAsync(ct);

        return Ok(ApiResponse<object>.Ok(new { success = ok, failed = fail, errors }, $"imported {ok} words"));
    }
}
