using InkWord.API.Controllers;
using InkWord.Core.Entities;

namespace InkWord.Tests;

/// <summary>
/// v2.0 #3 卡组生态首增量（UGC 分享）纯逻辑测试。
///
/// fork 深拷贝映射契约：内容字段全量拷贝（PayloadJson/Audio 等同源
/// 可用）+ 归属重定向（Tag/SubjectId/DeckId）+ 源卡组私有状态不渗入
/// （AiStatus/AiSuggestion/ChangeType）。发现页查询/分享开关写库语义
/// 沿 T5.3 已知缺口口径留集成/真机验证。
/// </summary>
public class DeckShareTests
{
    private static (Word Src, Deck Dst) Pair()
    {
        var src = new Word
        {
            Text = "会当凌绝顶",
            Phonetic = "huì dāng",
            Meaning = "终当登上泰山最高处",
            Example = "《望岳》",
            Audio = "poem_001.mp3",
            Tag = "uABC123",
            Root = "会当凌绝顶，一览众山小",
            Inflections = "会当,凌绝顶",
            Source = "部编版",
            Grade = "七年级下",
            Difficulty = 3,
            Version = 42,
            ChangeType = 1,
            AiStatus = 2,
            AiSuggestion = """{"kind":1}""",
            SubjectId = Guid.NewGuid(),
            DeckId = Guid.NewGuid(),
            Front = "会当凌绝顶，",
            Back = "一览众山小",
            PayloadJson = """{"prev":"岱宗夫如何？齐鲁青未了。"}""",
        };
        var dst = new Deck
        {
            SubjectId = Guid.NewGuid(),
            Code = "uXYZ9ab",
            Name = "我的古诗副本",
        };
        return (src, dst);
    }

    [Fact]
    public void CopyWord_ContentFieldsAllCopied()
    {
        var (src, dst) = Pair();

        var w = MyDeckController.CopyWord(src, dst, 101);

        Assert.Equal("会当凌绝顶", w.Text);
        Assert.Equal("huì dāng", w.Phonetic);
        Assert.Equal("终当登上泰山最高处", w.Meaning);
        Assert.Equal("《望岳》", w.Example);
        Assert.Equal("poem_001.mp3", w.Audio);            // 音频同源可用
        Assert.Equal(src.Root, w.Root);
        Assert.Equal(src.Inflections, w.Inflections);
        Assert.Equal(src.Source, w.Source);
        Assert.Equal(src.Grade, w.Grade);
        Assert.Equal(3, w.Difficulty);
        Assert.Equal(src.Front, w.Front);                  // T4.1 双写契约
        Assert.Equal(src.Back, w.Back);
        Assert.Equal(src.PayloadJson, w.PayloadJson);      // T4.4 默写载荷
    }

    [Fact]
    public void CopyWord_MetaRedirectedToCopyDeck()
    {
        var (src, dst) = Pair();

        var w = MyDeckController.CopyWord(src, dst, 101);

        Assert.Equal("uXYZ9ab", w.Tag);                    // uq(Text,Tag) 隔离键
        Assert.Equal(dst.SubjectId, w.SubjectId);
        Assert.Equal(dst.Id, w.DeckId);
        Assert.Equal(101, w.Version);                      // 全局递增（调用方保证）
    }

    [Fact]
    public void CopyWord_SourcePrivateStateNotLeaked()
    {
        var (src, dst) = Pair();

        var w = MyDeckController.CopyWord(src, dst, 101);

        Assert.Equal(0, w.AiStatus);                       // 源审校状态不拷
        Assert.Null(w.AiSuggestion);
        Assert.Equal(0, w.ChangeType);                     // 新增语义
        Assert.False(w.Archived);                           // 只拷未归档行，副本天然不归档
        Assert.NotEqual(src.Version, w.Version);
    }

    [Fact]
    public void CopyWord_SourceUntouched()
    {
        var (src, dst) = Pair();
        var srcTagBefore = src.Tag;
        var srcDeckBefore = src.DeckId;

        _ = MyDeckController.CopyWord(src, dst, 101);

        Assert.Equal(srcTagBefore, src.Tag);               // 只读源，无改写
        Assert.Equal(srcDeckBefore, src.DeckId);
        Assert.Equal(42, src.Version);
    }
}
