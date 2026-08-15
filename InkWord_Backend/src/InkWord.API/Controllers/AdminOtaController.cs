using System.Security.Cryptography;
using InkWord.API.DTOs;
using InkWord.Core.Common;
using InkWord.Core.Entities;
using InkWord.Core.Repositories;
using Microsoft.AspNetCore.Authorization;
using Microsoft.AspNetCore.Mvc;

namespace InkWord.API.Controllers;

/// <summary>OTA 升级包管理（B-17）。</summary>
[ApiController]
[Route("api/admin/ota")]
[Authorize]
public class AdminOtaController : ControllerBase
{
    private readonly IOtaPackageRepository _otaRepo;
    private readonly IWebHostEnvironment _env;

    public AdminOtaController(IOtaPackageRepository otaRepo, IWebHostEnvironment env)
    {
        _otaRepo = otaRepo; _env = env;
    }

    [HttpGet]
    public async Task<IActionResult> List(CancellationToken ct)
    {
        var items = await _otaRepo.ListAllAsync(ct);
        return Ok(ApiResponse<object>.Ok(items));
    }

    /// <summary>上传 .bin 固件，写入本地/OSS，计算 MD5，入库。</summary>
    [HttpPost("upload")]
    public async Task<IActionResult> Upload([FromForm] OtaPkgUploadDto meta, IFormFile file, CancellationToken ct)
    {
        if (file == null || file.Length == 0)
            return BadRequest(ApiResponse.Fail(400, "empty firmware file"));

        string md5;
        using (var md5c = MD5.Create())
        using (var stream = file.OpenReadStream())
        {
            md5 = BitConverter.ToString(await md5c.ComputeHashAsync(stream, ct)).Replace("-", "").ToLowerInvariant();
        }

        // 持久化到 wwwroot/firmware（生产应替换为 OSS/MinIO）
        var dir = Path.Combine(_env.ContentRootPath, "wwwroot", "firmware");
        Directory.CreateDirectory(dir);
        var fileName = $"firmware-{meta.Version}.bin";
        var path = Path.Combine(dir, fileName);
        await using (var fs = System.IO.File.Create(path))
        {
            await file.CopyToAsync(fs, ct);
        }

        var pkg = new OtaPackage
        {
            Version = meta.Version,
            Url = $"/firmware/{fileName}",
            Md5 = md5,
            Size = file.Length,
            ReleaseNotes = meta.ReleaseNotes ?? "",
            Published = meta.Published,
            TargetBoard = "esp32-s3"
        };
        await _otaRepo.AddAsync(pkg, ct);
        await _otaRepo.SaveChangesAsync(ct);

        return Ok(ApiResponse<OtaPackage>.Ok(pkg));
    }
}
