using System.Text.Json;
using Microsoft.Extensions.Caching.Distributed;
using StackExchange.Redis;

namespace InkWord.Infrastructure.Cache;

/// <summary>
/// Redis 缓存封装，支持 GetOrSetAsync 与 RemoveAsync。
/// 基于 IDistributedCache（已包含连接池管理）。
/// </summary>
public interface IRedisCache
{
    Task<T?> GetAsync<T>(string key, CancellationToken ct = default);
    Task<T> GetOrSetAsync<T>(string key, Func<Task<T>> factory, TimeSpan? expiry = null, CancellationToken ct = default);
    Task SetAsync<T>(string key, T value, TimeSpan? expiry = null, CancellationToken ct = default);
    Task RemoveAsync(string key, CancellationToken ct = default);
}

public class RedisCache : IRedisCache
{
    private readonly IDistributedCache _cache;
    private static readonly JsonSerializerOptions JsonOpts = new() { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };

    public RedisCache(IDistributedCache cache) => _cache = cache;

    public async Task<T?> GetAsync<T>(string key, CancellationToken ct = default)
    {
        var raw = await _cache.GetStringAsync(key, ct);
        return raw is null ? default : JsonSerializer.Deserialize<T>(raw, JsonOpts);
    }

    public async Task<T> GetOrSetAsync<T>(string key, Func<Task<T>> factory, TimeSpan? expiry = null, CancellationToken ct = default)
    {
        var cached = await GetAsync<T>(key, ct);
        if (cached is not null) return cached;

        var value = await factory();
        await SetAsync(key, value, expiry, ct);
        return value;
    }

    public Task SetAsync<T>(string key, T value, TimeSpan? expiry = null, CancellationToken ct = default)
    {
        var raw = JsonSerializer.Serialize(value, JsonOpts);
        var options = new DistributedCacheEntryOptions();
        if (expiry.HasValue) options.AbsoluteExpirationRelativeToNow = expiry;
        return _cache.SetStringAsync(key, raw, options, ct);
    }

    public Task RemoveAsync(string key, CancellationToken ct = default) => _cache.RemoveAsync(key, ct);
}
