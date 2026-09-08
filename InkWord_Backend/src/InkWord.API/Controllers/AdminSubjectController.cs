using InkWord.Core.Common;
using InkWord.Infrastructure.Repositories;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;
using Microsoft.EntityFrameworkCore;

namespace InkWord.API.Controllers;

/// <summary>
/// 管理端科目清单（P3 卡组管理页 2026-09：科目侧栏/chip 过滤数据源，
/// 含每科目卡组计数）。
/// </summary>
[ApiController]
[Route("api/admin/subjects")]
[Authorize(Roles = "Admin,Operator")]
public class AdminSubjectController : ControllerBase
{
    private readonly IUnitOfWork _uow;
    public AdminSubjectController(IUnitOfWork uow) => _uow = uow;

    /// <summary>科目列表 + 每科目卡组数（未软删卡组，含官方与学习者建组）</summary>
    [HttpGet]
    public async Task<IActionResult> List(CancellationToken ct)
    {
        var items = await _uow.Db.Subjects.AsNoTracking()
            .OrderBy(s => s.SortOrder)
            .Select(s => new
            {
                s.Id, s.Code, s.Name, s.SortOrder,
                DeckCount = _uow.Db.Decks.Count(d => d.SubjectId == s.Id && !d.IsDeleted),
            })
            .ToListAsync(ct);
        return Ok(ApiResponse<object>.Ok(items));
    }
}
