/**
 * @file quotes_app.h
 * @brief 待机页《传习录》引文表接口（App 层内容；生成文件勿手改）
 *
 * 开源通用化 Phase 2（2026-10-24）：App 内容自 Core 渲染层（cjk_font.h/c）
 * 迁出——字库（Core）只管字形渲染，学科内容归 App。引文字符仍收录进
 * 字库 bin（gen_cjk_font.swift 字符集输入不变）。
 * 由 tools/gen_cjk_font.swift 生成。
 */
#ifndef INKWORD_QUOTES_APP_H
#define INKWORD_QUOTES_APP_H

#define CHUANXILU_QUOTE_N 24                /**< 引文条数（=小时数） */
/** 待机页逐时轮换引文（UTF-8，\n 分行，每行 <=8字） */
extern const char *const k_chuanxilu_quotes[CHUANXILU_QUOTE_N];

/** 引文出处（右下角署名，UTF-8 单行） */
extern const char k_chuanxilu_attrib[];

#endif /* INKWORD_QUOTES_APP_H */
