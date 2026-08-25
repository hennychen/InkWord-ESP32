using System.Security.Claims;
using InkWord.Core.Common;
using InkWord.Core.Repositories;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;

namespace InkWord.API.Controllers;

/// <summary>
/// 学习者账户（v1.5 T5.3 轻账户，ACCOUNT_MODEL_DECISION §四）。
///
/// 只管内容归属（Deck.OwnerId 云备份），学习记录仍挂 DeviceId，
/// 设备固件零感知。JWT 复用既有 Jwt 配置（同 Secret/Issuer/Audience），
/// role=learner 与管理端 role（Admin/Operator）区分——Admin 控制器已
/// 加 Roles 白名单防越权。离线优先红线（§三.1）：无账户不影响任何
/// 学习功能，App 编辑器未登录仍可 LAN 直传推卡组。
/// </summary>
[ApiController]
[Route("api/account")]
public class AccountController : ControllerBase
{
    private readonly IAccountRepository _accountRepo;
    private readonly IConfiguration _cfg;

    public AccountController(IAccountRepository accountRepo, IConfiguration cfg)
    {
        _accountRepo = accountRepo; _cfg = cfg;
    }

    public record RegisterReq(string Username, string Password, string? DisplayName);
    public record AuthResp(string Username, string DisplayName, string Token, int ExpiresIn);

    /// <summary>注册并登录（用户名唯一；PBKDF2 与管理端同源）</summary>
    [HttpPost("register")]
    [AllowAnonymous]
    public async Task<IActionResult> Register([FromBody] RegisterReq req, CancellationToken ct)
    {
        var username = req.Username?.Trim() ?? "";
        if (username.Length is < 2 or > 64)
            return BadRequest(ApiResponse.Fail(400, "用户名须 2~64 字符"));
        if (string.IsNullOrEmpty(req.Password) || req.Password.Length < 6)
            return BadRequest(ApiResponse.Fail(400, "密码至少 6 位"));

        if (await _accountRepo.GetByUsernameAsync(username, ct) != null)
            return Conflict(ApiResponse.Fail(409, "用户名已被占用"));

        var account = new InkWord.Core.Entities.Account
        {
            Username = username,
            PasswordHash = AuthController.HashPassword(req.Password),
            DisplayName = string.IsNullOrWhiteSpace(req.DisplayName)
                ? username : req.DisplayName.Trim(),
        };
        await _accountRepo.AddAsync(account, ct);
        await _accountRepo.SaveChangesAsync(ct);

        var token = AuthController.IssueToken(_cfg, account.Username, "learner", account.Id);
        return Ok(ApiResponse<AuthResp>.Ok(new AuthResp(
            account.Username, account.DisplayName, token, 3600)));
    }

    /// <summary>登录（学习者；与管理端 /api/auth/login 分路径）</summary>
    [HttpPost("login")]
    [AllowAnonymous]
    public async Task<IActionResult> Login([FromBody] RegisterReq req, CancellationToken ct)
    {
        var account = await _accountRepo.GetByUsernameAsync(req.Username?.Trim() ?? "", ct);
        if (account == null || !AuthController.VerifyPassword(req.Password ?? "", account.PasswordHash))
            return Unauthorized(ApiResponse.Fail(401, "用户名或密码错误"));

        var token = AuthController.IssueToken(_cfg, account.Username, "learner", account.Id);
        return Ok(ApiResponse<AuthResp>.Ok(new AuthResp(
            account.Username, account.DisplayName, token, 3600)));
    }

    /// <summary>当前账户信息（App 启动时校验本地 token 是否仍有效）</summary>
    [HttpGet("me")]
    [Authorize(Roles = "learner")]
    public async Task<IActionResult> Me(CancellationToken ct)
    {
        var id = AccountId;
        if (id == null) return Unauthorized(ApiResponse.Fail(401, "token 缺少账户标识"));

        var account = await _accountRepo.GetByIdAsync(id.Value, ct);
        if (account == null) return Unauthorized(ApiResponse.Fail(401, "账户不存在"));

        return Ok(ApiResponse<AuthResp>.Ok(new AuthResp(
            account.Username, account.DisplayName, "", 0)));
    }

    /// <summary>learner token 的账户 Id（IssueToken 附带 NameIdentifier）</summary>
    internal Guid? AccountId =>
        Guid.TryParse(User.FindFirstValue(ClaimTypes.NameIdentifier), out var g) ? g : null;
}
