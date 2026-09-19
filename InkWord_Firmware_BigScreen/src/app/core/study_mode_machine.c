/**
 * @file study_mode_machine.c
 * @brief 学习模式状态机实现 —— 大屏核心闭环精简版
 *
 * 小屏版 682 行耦合音频/ WiFi/ chat/ reader/ 跟读评测十余模块；本版
 * 保 API 签名重写（见 .h 精简范围），序列逻辑（过滤视图/游标钳位/
 * after_* 家族/临时视图纪律/last_mode NVS）与小屏逐条对应：
 *   - seq_total/seq_word_index：FLASH/REVIEW=active/due 过滤视图，
 *     WRONGBOOK/COLLECTION/MASTERED=对应计数字段（learning_state 直查）
 *   - 边界钳制不回绕（小屏 2026-09-03 定稿）：首条上翻/末条下翻拒绝，
 *     无震动马达（大屏板未布）→ 边界反馈仅日志
 *   - speak(3)：settings_audio_enabled 桩恒 false 整段跳过（小屏
 *     v1.2 T2.5 门控语义保留，音频阶段接真实设置）
 */
#include "study_mode_machine.h"
#include "debug_log.h"
#include "learning_state.h"
#include "page_router.h"
#include "settings_keys.h"
#include "settings_ui.h"   /* speak 门控（桩恒 false，见文件头） */
#include "word_parser.h"   /* seek：词库总数钳位 + 墨封反查 */

#include "nvs.h"

static const char *TAG = "MODE";

static study_mode_t s_current = MODE_FLASH;

/* 当前词游标（各模式共用单游标，切模式归零） */
static int s_cursor = 0;

/* 释义显示状态：true=显示（默认），false=遮蔽（自测）；SET 翻义切换，
 * 翻页/切模式自动回显示（新词全显，按 SET 开始遮蔽自测） */
static bool s_reveal = true;

static const char *s_names[MODE_COUNT] =
    { "Flash", "Dictation", "Review", "Reader", "WrongBook", "收藏",
      "AI Chat", "测验", "目录", "语音", "墨封录" };

/* ---- 序列抽象：闪卡/复习=过滤视图，临时视图=对应计数 ---- */

static int seq_total(void)
{
    if (s_current == MODE_WRONGBOOK)  return learning_state_wrong_count();
    if (s_current == MODE_COLLECTION) return learning_state_collected_count();
    if (s_current == MODE_MASTERED)   return learning_state_mastered_count();
    if (s_current == MODE_REVIEW)     return learning_state_due_count();
    /* 未接线模式（DICTATION/READER/CHAT/...）不可达，0 兜底 */
    if (s_current != MODE_FLASH)      return 0;
    /* 闪卡：未墨封视图（2026-09-04 墨封——学习主链路过滤，无墨封词
     * 时与全库直映射等价） */
    return learning_state_active_count();
}

/* 游标 -> 词库索引（过滤序列内第 cursor 个词；越界 -1 交渲染层空态兑底） */
static int seq_word_index(int cursor)
{
    if (s_current == MODE_WRONGBOOK)  return learning_state_wrong_at(cursor);
    if (s_current == MODE_COLLECTION) return learning_state_collected_at(cursor);
    if (s_current == MODE_MASTERED)   return learning_state_mastered_at(cursor);
    if (s_current == MODE_REVIEW)     return learning_state_due_at(cursor);
    if (s_current != MODE_FLASH)      return -1;
    return learning_state_active_at(cursor);
}

/* 模式是否可经 NVS 恢复 / apply_mode 持久化（临时视图 + 未接线模式除外） */
static bool mode_persistable(study_mode_t m)
{
    return m == MODE_FLASH || m == MODE_REVIEW;
}

void study_mode_init(void)
{
    /* 从 NVS 恢复上次模式（白名单 = 可持久化模式；键缺省=首次启动闪卡） */
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        uint8_t m = 0;
        if (nvs_get_u8(h, NVS_KEY_LAST_MODE, &m) == ESP_OK &&
            m < MODE_COUNT && mode_persistable((study_mode_t)m)) {
            s_current = (study_mode_t)m;
        }
        nvs_close(h);
    }
    s_cursor = 0;
    LOG_I("study mode initialized: %s", s_names[s_current]);
}

study_mode_t study_mode_current(void)
{
    return s_current;
}

/* 模式切换公共副作用（switch_next / study_mode_set 共用）：游标归零 /
 * 遮蔽复位 / NVS 持久化。临时视图由 enter_* 显式进入，不经 apply_mode。 */
static void apply_mode(study_mode_t mode)
{
    s_current = mode;
    s_cursor = 0;
    s_reveal = true;

    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u8(h, NVS_KEY_LAST_MODE, (uint8_t)s_current);
        nvs_commit(h);
        nvs_close(h);
    }

    LOG_I("mode switched -> %s", s_names[s_current]);
}

study_mode_t study_mode_switch_next(void)
{
    /* 跳过临时视图与未接线模式（do-while 保证至少前进一步，不会死循环；
     * 可用集 = FLASH/REVIEW，音频/阅读器阶段放开 DICTATION/READER） */
    do {
        s_current = (study_mode_t)((s_current + 1) % MODE_COUNT);
    } while (!mode_persistable(s_current));
    apply_mode(s_current);
    return s_current;
}

void study_mode_set(study_mode_t mode)
{
    if (mode < 0 || mode >= MODE_COUNT) return;
    if (!mode_persistable(mode)) return;
    apply_mode(mode);   /* 同模式重入也归零游标，与 switch_next 语义一致 */
}

const char *study_mode_name(study_mode_t mode)
{
    return (mode >= 0 && mode < MODE_COUNT) ? s_names[mode] : "Unknown";
}

/* ---- 语义动作处理 ---- */
void study_mode_handle_action(int action)
{
    int total = seq_total();

    LOG_I("按键 action=%d, 当前模式=%d, 序列总数=%d", action, s_current, total);

    if (total <= 0) {
        LOG_I("序列为空（词库空或当前视图无词条），按键无响应");
        return;
    }

    switch (action) {
    case 0: /* prev：边界钳制不回绕（小屏 2026-09-03 定稿；大屏无震动，
     * 边界拒绝仅日志） */
        if (s_cursor <= 0) {
            LOG_D("prev rejected at #0 (seq boundary)");
            return;
        }
        s_cursor--;
        s_reveal = true;
        break;
    case 1: /* next：末条下翻同钳制（对称） */
        if (s_cursor >= total - 1) {
            LOG_D("next rejected at #%d (seq boundary)", total - 1);
            return;
        }
        s_cursor++;
        s_reveal = true;
        break;
    case 2: /* confirm：翻转释义遮蔽/揭晓 */
        s_reveal = !s_reveal;
        LOG_D("confirm action in %s mode (reveal=%d)",
              s_names[s_current], s_reveal);
        break;
    case 3: /* speak：音频门控（桩恒 false 整段跳过；音频阶段接真实
             * 设置后恢复发音+听-跟一体流，见小屏版 P0C/P1 编排） */
        if (!settings_audio_enabled()) break;
        break;
    default:
        break;
    }

    /* 仅画面变化的动作触发重绘：翻页(prev/next)与翻义(confirm)；
     * speak(3) 只播放音频不重绘（大屏桩路径零动作） */
    if (action == 0 || action == 1 || action == 2) {
        page_router_render_top();
    }
}

bool study_mode_is_revealed(void)
{
    return s_reveal;
}

void study_mode_reset_cursor(void)
{
    s_cursor = 0;
    s_reveal = true;
    LOG_D("cursor reset to #0");
    page_router_render_top();
}

/* ---- 错词本临时视图 ---- */

bool study_mode_enter_wrongbook(void)
{
    if (learning_state_wrong_count() == 0) {
        LOG_W("wrong book empty, nothing to enter");
        return false;
    }
    s_current = MODE_WRONGBOOK;
    s_cursor = 0;
    s_reveal = true;
    LOG_I("entered wrong book (%d words)", learning_state_wrong_count());
    return true;
}

void study_mode_exit_wrongbook(void)
{
    /* 临时视图：不写 last_mode，重启后自然回学习模式 */
    s_current = MODE_FLASH;
    s_cursor = 0;
    s_reveal = true;
    LOG_I("left wrong book");
}

/* ---- 收藏浏览临时视图 ---- */

bool study_mode_enter_collection(void)
{
    if (learning_state_collected_count() == 0) {
        LOG_W("collection empty, nothing to enter");
        return false;
    }
    s_current = MODE_COLLECTION;
    s_cursor = 0;
    s_reveal = true;
    LOG_I("entered collection (%d words)", learning_state_collected_count());
    return true;
}

void study_mode_exit_collection(void)
{
    s_current = MODE_FLASH;
    s_cursor = 0;
    s_reveal = true;
    LOG_I("left collection");
}

/* ---- 墨封录临时视图（收藏浏览同构镜像） ---- */

bool study_mode_enter_mastered(void)
{
    if (learning_state_mastered_count() == 0) {
        LOG_W("mastered list empty, nothing to enter");
        return false;
    }
    s_current = MODE_MASTERED;
    s_cursor = 0;
    s_reveal = true;
    LOG_I("entered mastered list (%d words)", learning_state_mastered_count());
    return true;
}

void study_mode_exit_mastered(void)
{
    s_current = MODE_FLASH;
    s_cursor = 0;
    s_reveal = true;
    LOG_I("left mastered list");
}

void study_mode_seek(int word_index)
{
    /* 词库索引定位：切 FLASH + 游标=index 钳位 + 渲染；不写 NVS
     * （FLASH 本就可恢复）。目标词已墨封时先启封——选词=要学它
     * （跳转即启封语义；不启封则 active 视图反查不到该词） */
    int total = word_parser_get_count();
    if (total <= 0) return;
    if (word_index < 0) word_index = 0;
    if (word_index >= total) word_index = total - 1;
    if (learning_state_is_mastered(word_index)) {
        learning_state_toggle_master(word_index);
        LOG_I("seek unmastered word #%d", word_index);
    }
    s_current = MODE_FLASH;
    /* 词库索引→active 序列位置反查（O(N) 单次，翻词路径同量级） */
    s_cursor = 0;
    int pos = 0;
    for (int i = 0; i <= word_index; i++)
        if (!learning_state_is_mastered(i)) pos++;
    s_cursor = pos - 1;
    s_reveal = true;
    LOG_I("seek to word #%d (active pos %d)", word_index, s_cursor);
    page_router_render_top();
}

bool study_mode_after_uncollect(void)
{
    if (s_current != MODE_COLLECTION) return false;

    /* 当前词已移出收藏序列：后词前移；清空自动退回闪卡 */
    int total = learning_state_collected_count();
    if (total == 0) {
        study_mode_exit_collection();
        return true;
    }
    /* 越界钳到 n-1：取消末词时显示前一词，不回绕跳跃
     * （与 after_quality 的回绕 0 策略略异，体验优先） */
    if (s_cursor >= total) s_cursor = total - 1;
    return true;
}

bool study_mode_after_quality(int quality)
{
    if (s_current != MODE_WRONGBOOK || quality < 3) return false;

    /* 当前词连错清零已移出错词序列：后词前移，游标原位停留 */
    if (learning_state_wrong_count() == 0) {
        study_mode_exit_wrongbook();
        return true;
    }
    if (s_cursor >= learning_state_wrong_count()) s_cursor = 0;
    return true;
}

bool study_mode_after_master(void)
{
    if (s_current == MODE_FLASH) {
        int total = learning_state_active_count();
        if (total == 0) { s_cursor = 0; return true; }
        if (s_cursor >= total) s_cursor = total - 1;
        return true;
    }
    if (s_current == MODE_WRONGBOOK) {
        if (learning_state_wrong_count() == 0) {
            study_mode_exit_wrongbook();
            return true;
        }
        if (s_cursor >= learning_state_wrong_count()) s_cursor = 0;
        return true;
    }
    if (s_current == MODE_REVIEW) {
        int total = learning_state_due_count();
        if (s_cursor >= total) s_cursor = total > 0 ? total - 1 : 0;
        return true;
    }
    if (s_current == MODE_MASTERED) {
        int total = learning_state_mastered_count();
        if (total == 0) {
            study_mode_exit_mastered();
            return true;
        }
        if (s_cursor >= total) s_cursor = total - 1;
        return true;
    }
    return false;
}

bool study_mode_after_due_review(void)
{
    if (s_current != MODE_REVIEW) return false;

    int total = learning_state_due_count();
    if (s_cursor >= total) s_cursor = total > 0 ? total - 1 : 0;
    return true;
}

int study_mode_current_word_index(void)
{
    return seq_word_index(s_cursor);
}

int study_mode_seq_pos(void)
{
    return s_cursor;
}

int study_mode_seq_total(void)
{
    return seq_total();
}
