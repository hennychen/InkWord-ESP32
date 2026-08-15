namespace InkWord.Core.Common;

/// <summary>
/// 统一 API 响应包装。
/// </summary>
public class ApiResponse<T>
{
    public int Code { get; set; } = 0;          // 0=成功，非 0=业务错误码
    public string Message { get; set; } = "ok";
    public T? Data { get; set; }

    public static ApiResponse<T> Ok(T data, string msg = "ok") => new() { Code = 0, Message = msg, Data = data };
    public static ApiResponse<T> Fail(int code, string msg) => new() { Code = code, Message = msg, Data = default };
}

public class ApiResponse : ApiResponse<object>
{
    public static ApiResponse Ok(string msg = "ok") => new() { Code = 0, Message = msg };
    public static new ApiResponse Fail(int code, string msg) => new() { Code = code, Message = msg };
}
