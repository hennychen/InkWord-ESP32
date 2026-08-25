using System.Text.Json;
using InkWord.Core.Entities;
using InkWord.Services;
using Xunit;

namespace InkWord.Tests;

/// <summary>T5.4 AI 卡组生成纯逻辑用例：三版式解析归一、版式映射写入、
/// 长度截断、旧载荷兼容（AccountTests 同范式——纯服务直测无 DB/LLM 依赖）。</summary>
public class DeckGenTests
{
    // ---- ParseDeckItems ----

    [Fact]
    public void ParseDeckItems_Poem_FieldsNormalized()
    {
        const string raw = """
            ```json
            [{"front":"床前明月光","back":"疑是地上霜","phonetic":"yí shì dì shàng shuāng","meaning":"好像是地上的霜"},{"front":"","back":"空正面跳过"}]
            ```
            """;
        var items = AiContentService.ParseDeckItems(raw, "poem-card");

        var item = Assert.Single(items);
        Assert.Equal((int)AiContentKind.DeckItems, item.Kind);
        Assert.Equal("床前明月光", item.Front);
        Assert.Equal("疑是地上霜", item.Back);
        Assert.Equal("yí shì dì shàng shuāng", item.Phonetic);
        Assert.Equal("好像是地上的霜", item.Meaning);
        Assert.Null(item.Example); // poem 不产例句
    }

    [Fact]
    public void ParseDeckItems_Qa_MinimalContract()
    {
        var items = AiContentService.ParseDeckItems(
            """[{"front":"光的速度是多少？","back":"约 30 万公里/秒"}]""", "qa-card");

        var item = Assert.Single(items);
        Assert.Equal("光的速度是多少？", item.Front);
        Assert.Equal("约 30 万公里/秒", item.Back);
        Assert.Null(item.Phonetic); // qa 无拼音/音标
        Assert.Null(item.Meaning);
        Assert.Null(item.Example);
    }

    [Fact]
    public void ParseDeckItems_WordCard_ExampleKept_AndFallback()
    {
        var raw = """[{"front":"apple","back":"苹果","phonetic":"/ˈæpl/","example":"an apple a day（每天一苹果）"}]""";
        // null/空版式回退 word-card（与固件 card_layout 同口径）
        var items = AiContentService.ParseDeckItems(raw, null);
        var item = Assert.Single(items);
        Assert.Equal("apple", item.Front);
        Assert.Equal("/ˈæpl/", item.Phonetic);
        Assert.Equal("an apple a day（每天一苹果）", item.Example);
        Assert.Null(item.Meaning); // word-card 无译文
    }

    [Fact]
    public void ParseDeckItems_DedupAndBlankSkip()
    {
        var raw = """
            [{"front":"同题","back":"甲"},{"front":"同题","back":"乙"},
             {"back":"无正面"},{"front":"无背面"}]
            """;
        var items = AiContentService.ParseDeckItems(raw, "qa-card");
        var item = Assert.Single(items);
        Assert.Equal("甲", item.Back); // 首见保留，重复/缺面跳过
    }

    [Fact]
    public void ParseDeckItems_TruncatesOverlongBack()
    {
        var longBack = new string('长', 200); // 200 汉字 = 600B > BackMaxBytes 250B
        var items = AiContentService.ParseDeckItems(
            $$"""[{"front":"题","back":"{{longBack}}"}]""", "qa-card");
        var item = Assert.Single(items);
        Assert.True(System.Text.Encoding.UTF8.GetByteCount(item.Back!) <= AiContentService.BackMaxBytes);
    }

    [Fact]
    public void ParseDeckItems_NonArrayOrBadJson_ReturnsEmpty()
    {
        Assert.Empty(AiContentService.ParseDeckItems("不是数组", "qa-card"));
        Assert.Empty(AiContentService.ParseDeckItems("[{坏 JSON", "qa-card"));
        Assert.Empty(AiContentService.ParseDeckItems("", "qa-card"));
    }

    // ---- ApplyDeckSuggestion ----

    [Fact]
    public void ApplyDeck_Poem_MapsT44CloudContract()
    {
        var word = new Word { Text = "床前明月光", Front = "床前明月光" };
        var sug = new WordAiSuggestion(3,
            Front: "床前明月光", Back: "疑是地上霜",
            Phonetic: "yí shì dì shàng shuāng", Meaning: "好像是地上的霜");

        AiContentService.ApplyDeckSuggestion(word, "poem-card", sug,
            null, null, null, null, null);

        // T4.4 云通道契约：text=下句 / root=上句 / phonetic=下句拼音 / meaning=译文
        Assert.Equal("疑是地上霜", word.Text);
        Assert.Equal("床前明月光", word.Root);
        Assert.Equal("yí shì dì shàng shuāng", word.Phonetic);
        Assert.Equal("好像是地上的霜", word.Meaning);
        Assert.Equal("", word.Example);
        Assert.Equal("床前明月光", word.Front);
        Assert.Equal("疑是地上霜", word.Back);

        // PayloadJson 结构化载荷 {"prev":上句}（App/云端消费，设备零解析）
        using var doc = JsonDocument.Parse(word.PayloadJson!);
        Assert.Equal("床前明月光", doc.RootElement.GetProperty("prev").GetString());
    }

    [Fact]
    public void ApplyDeck_Qa_MapsT43Contract()
    {
        var word = new Word { Text = "光的速度是多少？" };
        var sug = new WordAiSuggestion(3, Front: "光的速度是多少？", Back: "约 30 万公里/秒");

        AiContentService.ApplyDeckSuggestion(word, "qa-card", sug,
            null, null, null, null, null);

        // T4.3 契约：text=题面 / meaning=答案
        Assert.Equal("光的速度是多少？", word.Text);
        Assert.Equal("约 30 万公里/秒", word.Meaning);
        Assert.Equal(word.Text, word.Front);
        Assert.Equal(word.Meaning, word.Back);
        Assert.Null(word.PayloadJson); // qa 无结构化载荷
    }

    [Fact]
    public void ApplyDeck_WordCard_ExampleAndReqPriority()
    {
        var word = new Word { Text = "apple" };
        var sug = new WordAiSuggestion(3,
            Front: "apple", Back: "苹果", Phonetic: "/ˈæpl/", Example: "old");

        // req 终值优先于建议原值（审校可编辑）
        AiContentService.ApplyDeckSuggestion(word, "word-card", sug,
            reqFront: null, reqBack: "苹果（水果）", reqPhonetic: null, reqMeaning: null, reqExample: "new example");

        Assert.Equal("apple", word.Text);
        Assert.Equal("苹果（水果）", word.Meaning); // req.Back 优先
        Assert.Equal("new example", word.Example); // req.Example 优先
        Assert.Equal("/ˈæpl/", word.Phonetic); // req 缺省取建议
    }

    [Fact]
    public void ApplyDeck_TruncatesOverlongReqFront()
    {
        var longFront = new string('题', 40); // 120B > FrontMaxBytes 60B
        var word = new Word();
        AiContentService.ApplyDeckSuggestion(word, "qa-card",
            new WordAiSuggestion(3, Front: "x", Back: "y"),
            reqFront: longFront, reqBack: null, reqPhonetic: null,
            reqMeaning: null, reqExample: null);

        Assert.True(System.Text.Encoding.UTF8.GetByteCount(word.Text) <= AiContentService.FrontMaxBytes);
    }

    // ---- 旧载荷兼容 ----

    [Fact]
    public void LegacyFourFieldJson_DeserializesWithNullDeckFields()
    {
        // M1 既有 kind=0 载荷（无 T5.4 字段）：反序列化后新字段须为 null
        const string legacy = """{"kind":0,"example":"an apple（苹果）","root":null,"confusionNote":null}""";
        var opts = new JsonSerializerOptions { PropertyNamingPolicy = JsonNamingPolicy.CamelCase };
        var sug = JsonSerializer.Deserialize<WordAiSuggestion>(legacy, opts)!;

        Assert.Equal(0, sug.Kind);
        Assert.NotNull(sug.Example);
        Assert.Null(sug.Front);
        Assert.Null(sug.Back);
        Assert.Null(sug.Phonetic);
        Assert.Null(sug.Meaning);
    }

    [Fact]
    public void ExtractJsonArray_MarkdownFenceAndObjectRejected()
    {
        Assert.NotNull(AiContentService.ExtractJsonArray("```json\n[{\"a\":1}]\n```"));
        Assert.Equal("[{\"a\":1}]", AiContentService.ExtractJsonArray("前缀[{\"a\":1}]尾缀"));
        Assert.Null(AiContentService.ExtractJsonArray("只有对象 {\"a\":1}"));
        Assert.Null(AiContentService.ExtractJsonArray(""));
    }
}
