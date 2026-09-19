/**
 * @file reader_engine.h
 * @brief 大屏阅读引擎（小屏 reader_engine 移植 + 大屏排版适配）
 *
 * 职责：书加载（SPIFFS 书库 → 内嵌演示书回退，整本入 PSRAM）→
 * UTF-8 逐字解码 → 墨迹盒变宽排版 → 按字号/行距建页表 → 正文区
 * 渲染 → NVS 进度。页游标由 reader_page 持有（覆盖层页方案，
 * 模式机零改动），本模块只提供页数查询 / 正文渲染 / 字号行距
 * 切换（保位）/ 进度恢复。
 *
 * 排版（大屏方案定稿 2026-09-16）：
 *   字号三档 1=20px / 2=24px / 3=32px（16px 级不用于大屏正文），
 *     默认 3（=XLARGE reader_level 表值）；
 *   行距三档 15/16/18（×0.1 倍字号，默认 16=1.6 倍，W3C 1.5-1.8）；
 *   段距 = 0.8 倍行高（连续空行折叠为一次段距）；
 *   分页 = 行级切分 + 段首孤行保护（段的第一行放不下页尾时整段
 *     挪新页；完整"段内不分页"不做——大段会导致页尾大片空白）。
 * 切级重建页表，按"当前页首字符字节偏移"重新定位页码（小屏先例）。
 *
 * 书来源：/storage/books/ 首个 .txt/.md（SPIFFS，storage_init 后）；
 * 无书回退内嵌演示书（demo_book.txt，embed_data.S .incbin）。
 * 惰性初始化：reader_page 首次进入时调 reader_engine_init（建页表
 * 耗时与书长成正比，不进启动链保首帧速度）。
 */
#ifndef BIGSCREEN_READER_ENGINE_H
#define BIGSCREEN_READER_ENGINE_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ---- 阅读页几何（engine 正文区 + reader_page 状态栏/提示栏共用基准；
 *      XLARGE 1920x1080 首帧上机后校准） ---- */
#define RD_STATUS_H     120   /* 状态栏高（reader_page 绘制；正文区顶基准） */
#define RD_MARGIN_X     80    /* 左右页边距（=layout margin_x 表值） */
#define RD_BOTTOM_BAR   80    /* 底部按键提示栏高（reader_page 绘制；
                                * 2026-09-17 UI 重设计 60→80 对齐三区范式） */

/**
 * @brief 初始化：自动找书（SPIFFS 首书 → 演示书回退）+ 整本入 PSRAM
 *        + 按恢复的字号/行距建页表 + 章节索引 + 书签恢复。
 *        失败时引擎保持未就绪（渲染走占位页），不阻塞其他功能。
 * @return 0 就绪；-1 无书/加载失败；-2 内存不足
 */
int reader_engine_init(void);

/** 是否有可读书（未就绪时阅读页显示占位提示） */
bool reader_ready(void);

/** 当前字号级总页数（未就绪返回 0） */
int reader_page_count(void);

/** 当前字号级（大屏三档 1/2/3 = 20/24/32px；未就绪返回默认 3） */
int reader_font_level(void);

/** 当前行距档（15/16/18 = 1.5/1.6/1.8 倍；未就绪返回默认 16） */
int reader_line_spacing(void);

/**
 * @brief 切换字号级（dir=±1 循环 1→2→3→1），重建页表。
 * @param cur_page 当前页码（reader_page 游标）
 * @return 新页码（按"cur_page 页首字符偏移"在新页表中就近定位）
 */
int reader_font_step(int dir, int cur_page);

/** 行距档步进（dir=±1 循环 15/16/18），重建页表保位（同上） */
int reader_spacing_step(int dir, int cur_page);

/**
 * @brief NVS 恢复上次阅读页（书签名匹配才有效）。
 * @return 页码；-1 无有效记录
 */
int reader_progress_page(void);

/** 进度持久化（页码 + 字号级 + 行距档，NVS "inkword"/rd_*） */
void reader_engine_save_progress(int page);

/** 渲染第 page 页正文到内容区（状态栏/提示栏归 reader_page） */
void reader_render_body(int page);

/** 未就绪占位页（engine 自绘整页；书籍加载失败提示） */
void reader_render_placeholder(void);

/**
 * @brief 加载指定书籍（释放旧书 + 重建页表 + 章节索引 + 书签恢复
 *        + 恢复该书进度/字号/行距）。书架选书后经此接口加载。
 * @param path 书籍绝对路径（/storage/books/...）。
 * @return 0 成功；-1 加载失败；-2 内存不足
 */
int reader_engine_load_book(const char *path);

/** 加载内置演示书（书架首项；语义同 load_book） */
int reader_engine_load_demo(void);

/** 当前书名（文件名去扩展名；演示书固定"内置演示书"；未加载 ""） */
const char *reader_engine_book_title(void);

/** 当前已加载书的签名（FNV-1a，进度恢复/书签键区分用；未加载 0） */
uint32_t reader_engine_get_signature(void);

/** 当前书全文指针（只读，章节检测消费；未加载返回 NULL） */
const char *reader_engine_get_text(void);

/** 当前书字节长度（与 get_text 配对；未加载返回 0） */
uint32_t reader_engine_get_text_len(void);

/** 当前页首字节偏移（书签定位用；page 越界返回 0） */
uint32_t reader_engine_page_offset(int page);

/** 页偏移数组指针（只读，章节索引 map_pages 用；未就绪返回 NULL） */
const uint32_t *reader_engine_page_offsets(void);

/**
 * @brief 进度键卡组隔离（小屏 v1.3 T3.1 同款）：非空 scope 时 rd_*
 *        键加后缀（rd_page_<scope>）。大屏用法：post_load 按书签名
 *        hex 低 7 位设置 scope——书架换书互不覆盖进度（键长 15 上限内）。
 *        ≤7 字符；NULL/"" 用原键。
 */
void reader_set_progress_scope(const char *scope);

#ifdef __cplusplus
}
#endif
#endif /* BIGSCREEN_READER_ENGINE_H */
