/**
 * @file card_layout.h
 * @brief 卡片版式分派（v1.4 T4.3，全科地基渲染层）
 *
 * 按 Deck manifest 的 payloadType 选择词条卡渲染版式；一期 switch
 * 三版式（计划冻结：>5 种再升注册表，见 ROADMAP v1.4 表 3）。
 * 字段→版式语义约定（WordEntry 复用不扩结构——内存红线 1096B/条）：
 *   word-card：text 单词 / phonetic IPA / meaning 释义（现版式）
 *   qa-card  ：text 题干（多行 CJK）/ meaning 答案（遮蔽→揭晓）
 *   poem-card：text 诗行 / phonetic 拼音标注行（IPA 点阵渲染链复用）
 *              / meaning 译文（遮蔽→揭晓）
 * payloadJson 结构化载荷（上下句默写等）T4.4 消费，本期不解析；
 * 渲染实现留 main.cpp（QUIZ_DESIGN「渲染留 main.cpp」先例）。
 */
#ifndef INKWORD_CARD_LAYOUT_H
#define INKWORD_CARD_LAYOUT_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    CARD_LAYOUT_WORD = 0,  /**< word-card：单词卡（现版式，缺省回退） */
    CARD_LAYOUT_QA,        /**< qa-card：问答卡（题干/答案多行 CJK） */
    CARD_LAYOUT_POEM,      /**< poem-card：古诗卡（拼音行+诗行+译文） */
} card_layout_t;

/**
 * @brief payloadType → 版式枚举。
 *        NULL/空/未知串保守回退 CARD_LAYOUT_WORD（英语开箱行为不变）。
 */
card_layout_t card_layout_from_payload(const char *payload_type);

#ifdef __cplusplus
}
#endif
#endif /* INKWORD_CARD_LAYOUT_H */
