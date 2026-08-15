# =========================================================
# Dockerfile — InkWord Admin (A-12)
# 多阶段构建：Node 构建 Angular SPA → Nginx 部署 + 反向代理
# =========================================================

# ---- Stage 1: Build Angular SPA ----
FROM node:20-alpine AS builder
LABEL stage="builder"

WORKDIR /app

# 先复制 package 文件，利用 Docker 缓存层
COPY package.json package-lock.json ./
RUN npm ci --no-audit --no-fund

# 复制源码并构建
COPY . .
RUN npm run build -- --configuration production

# ---- Stage 2: Nginx 部署 ----
FROM nginx:1.25-alpine

# 移除默认配置
RUN rm /etc/nginx/conf.d/default.conf

# 复制自定义 Nginx 配置
COPY nginx.conf /etc/nginx/conf.d/inkword.conf

# 复制构建产物
COPY --from=builder /app/dist/ink-word-admin/browser /usr/share/nginx/html

EXPOSE 80

CMD ["nginx", "-g", "daemon off;"]
