# =========================================================
# Dockerfile — InkWord Backend API
# 多阶段构建：SDK 编译 → Runtime 运行
# =========================================================

# ---- Stage 1: Build ----
FROM mcr.microsoft.com/dotnet/sdk:8.0 AS builder
LABEL stage="builder"

WORKDIR /src

# 先复制 csproj 利用缓存层
COPY src/InkWord.Core/InkWord.Core.csproj src/InkWord.Core/
COPY src/InkWord.Infrastructure/InkWord.Infrastructure.csproj src/InkWord.Infrastructure/
COPY src/InkWord.Services/InkWord.Services.csproj src/InkWord.Services/
COPY src/InkWord.Jobs/InkWord.Jobs.csproj src/InkWord.Jobs/
COPY src/InkWord.API/InkWord.API.csproj src/InkWord.API/
RUN dotnet restore src/InkWord.API/InkWord.API.csproj

# 复制源码并编译
COPY src/ ./src/
RUN dotnet publish src/InkWord.API/InkWord.API.csproj \
    -c Release -o /app/publish --no-restore

# ---- Stage 2: Runtime ----
FROM mcr.microsoft.com/dotnet/aspnet:8.0

WORKDIR /app
COPY --from=builder /app/publish ./

# 容器内监听 8080（.NET 8 默认）
ENV ASPNETCORE_URLS=http://+:8080
ENV ASPNETCORE_ENVIRONMENT=Production

EXPOSE 8080

ENTRYPOINT ["dotnet", "InkWord.API.dll"]
