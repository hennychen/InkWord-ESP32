namespace InkWord.Services;

/// <summary>
/// 场景剧本（A1 场景化对话，2026-08-28）：对话模式 scenario 的剧本源。
///
/// 一期静态内嵌 6 场景（C# 数组字面量而非 JSON 资源：编译期检查、
/// 零解析零加载失败路径）；剧本内容打磨成熟后再 CRUD 化（A3 后，
/// 对齐「内容先于平台」决策——见路线图被拒方案 §2）。
/// Id 用 ASCII 短码：URL query 与固件 scenario[16] 缓冲友好。
/// WarmupIntro ≤180 UTF-8 字节（设备 s_reply 同级缓冲语义）。
/// </summary>
public record ScenarioScript(
    string Id,
    string TitleZh,        // 设备二级页标签（纯 CJK，TINY 档可显）
    string TitleEn,        // App/管理端展示
    string Persona,        // LLM 扮演人设（英文短语，拼入 system prompt）
    string SceneSetup,     // 场景设定（英文，拼入 system prompt）
    string[] TargetWords,  // 引导学生使用的目标词（≤5）
    string OpeningLine,    // 英文开场白（首轮 assistant 注入，不走 LLM）
    string WarmupIntro);   // 中文预热文案（首轮 Warmup 字段下发设备屏显）

public static class ScenarioLibrary
{
    /// <summary>全部场景（设备二级页列举，顺序即展示序）</summary>
    public static readonly IReadOnlyList<ScenarioScript> All =
    [
        new("food", "餐厅点餐", "Ordering Food",
            "a friendly waiter at a cozy restaurant",
            "The student is ordering lunch. Guide them to order a main dish and a drink, and to ask about the price politely.",
            ["order", "menu", "delicious", "bill"],
            "Welcome! Here is our menu. What would you like to eat today?",
            "今天练习餐厅点餐。关键词：order 点餐 / menu 菜单 / bill 账单。试着点一份午餐吧！"),
        new("directions", "问路指路", "Asking Directions",
            "a kind local person who knows the town well",
            "The student is a visitor asking the way to the library. Guide them to ask politely and understand the route.",
            ["turn", "left", "right", "straight", "corner"],
            "Hi! You look a little lost. Where would you like to go?",
            "今天练习问路。关键词：turn 转 / left 左 / right 右 / straight 直走。你要去图书馆，试着问问路！"),
        new("school", "校园聊天", "School Chat",
            "a friendly classmate at school",
            "The student is chatting with a classmate between classes about favorite subjects, homework and after-school clubs.",
            ["subject", "favorite", "homework", "borrow"],
            "Hey! Our math class was fun today. What subject do you like best?",
            "今天练习校园聊天。关键词：subject 科目 / homework 作业 / borrow 借。和同学聊聊喜欢的课吧！"),
        new("shopping", "商场购物", "Shopping",
            "a helpful shop assistant in a clothes shop",
            "The student is buying a T-shirt. Guide them to ask about size and color, try it on, and check the price.",
            ["size", "try on", "expensive", "discount"],
            "Welcome to our shop! Can I help you find something?",
            "今天练习购物。关键词：size 尺码 / try on 试穿 / discount 折扣。买一件 T 恤试试！"),
        new("travel", "旅行住宿", "Travel & Hotel",
            "a warm hotel receptionist in a tourist town",
            "The student is checking in at a hotel and asking about fun places to visit nearby.",
            ["book", "visit", "map", "weather"],
            "Good afternoon! Welcome to Sunshine Hotel. Do you have a booking?",
            "今天练习旅行英语。关键词：book 预订 / visit 参观 / map 地图。入住酒店并问问哪里好玩！"),
        new("doctor", "看医生", "Seeing the Doctor",
            "a gentle school doctor",
            "The student has a cold and is seeing the doctor. Guide them to describe how they feel in simple words.",
            ["headache", "fever", "rest", "medicine"],
            "Hello, come in. You don't look well. How do you feel today?",
            "今天练习看医生。关键词：headache 头痛 / fever 发烧 / medicine 药。告诉医生你哪里不舒服！"),
    ];

    /// <summary>按 Id 查场景（忽略大小写；未命中 null）</summary>
    public static ScenarioScript? Get(string id) =>
        All.FirstOrDefault(s => string.Equals(s.Id, id, StringComparison.OrdinalIgnoreCase));
}
