using System.IdentityModel.Tokens.Jwt;
using System.Security.Claims;
using System.Security.Cryptography;
using System.Text;
using InkWord.Core.Common;
using InkWord.Core.Repositories;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;
using Microsoft.IdentityModel.Tokens;

namespace InkWord.API.Controllers;

/// <summary>管理后台认证（登录颁发 JWT）。</summary>
[ApiController]
[Route("api/auth")]
public class AuthController : ControllerBase
{
    private readonly IUserRepository _userRepo;
    private readonly IConfiguration _cfg;

    public AuthController(IUserRepository userRepo, IConfiguration cfg)
    {
        _userRepo = userRepo; _cfg = cfg;
    }

    public record LoginReq(string Username, string Password);
    public record LoginResp(string Username, string Token, int ExpiresIn);

    [HttpPost("login")]
    [AllowAnonymous]
    public async Task<IActionResult> Login([FromBody] LoginReq req, CancellationToken ct)
    {
        var user = await _userRepo.GetByUsernameAsync(req.Username, ct);
        if (user == null || !VerifyPassword(req.Password, user.PasswordHash))
            return Unauthorized(ApiResponse.Fail(401, "用户名或密码错误"));

        var token = IssueToken(_cfg, user.Username, user.Role);
        return Ok(ApiResponse<LoginResp>.Ok(new LoginResp(user.Username, token, 3600)));
    }

    /// <summary>
    /// JWT 签发（public static 供 AccountController learner 路径复用，
    /// v1.5 T5.3；accountId 非空时附带 NameIdentifier claim 供卡组归属）。
    /// </summary>
    public static string IssueToken(
        IConfiguration cfg, string username, string role, Guid? accountId = null)
    {
        var key = new SymmetricSecurityKey(Encoding.UTF8.GetBytes(cfg["Jwt:Secret"]!));
        var creds = new SigningCredentials(key, SecurityAlgorithms.HmacSha256);

        var claims = new List<Claim>
        {
            new(ClaimTypes.Name, username),
            new(ClaimTypes.Role, role)
        };
        if (accountId.HasValue)
            claims.Add(new Claim(ClaimTypes.NameIdentifier, accountId.Value.ToString()));

        var token = new JwtSecurityToken(
            issuer: cfg["Jwt:Issuer"],
            audience: cfg["Jwt:Audience"],
            claims: claims,
            expires: DateTime.UtcNow.AddHours(1),
            signingCredentials: creds);

        return new JwtSecurityTokenHandler().WriteToken(token);
    }

    /// <summary>哈希校验（salt:hash PBKDF2；public 供 AccountController/测试复用）。</summary>
    public static bool VerifyPassword(string input, string storedHash)
    {
        // storedHash 形如 "salt:hash"
        var parts = storedHash.Split(':');
        if (parts.Length != 2) return false;
        var salt = Convert.FromHexString(parts[0]);
        using var pbkdf2 = new Rfc2898DeriveBytes(input, salt, 10000, HashAlgorithmName.SHA256);
        var hash = pbkdf2.GetBytes(32);
        return Convert.ToHexString(hash).Equals(parts[1], StringComparison.OrdinalIgnoreCase);
    }

    /// <summary>生成密码哈希（供初始化种子账号使用）。</summary>
    public static string HashPassword(string password)
    {
        var salt = RandomNumberGenerator.GetBytes(16);
        using var pbkdf2 = new Rfc2898DeriveBytes(password, salt, 10000, HashAlgorithmName.SHA256);
        var hash = pbkdf2.GetBytes(32);
        return $"{Convert.ToHexString(salt)}:{Convert.ToHexString(hash)}";
    }
}
