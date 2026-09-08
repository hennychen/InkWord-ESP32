// gen_cjk_font.swift —— 中文点阵字库生成器（四级 16/20/24/32px，P3 阅读模式；
// 32px 级 2026-09-03 增：LARGE 档（7.5"+ 大屏）正文/引文用，楷体大字链）
//
// 输入：tools/chuanxilu_quotes.txt（待机页引文，字符集一并收录）
// 输出：src/cjk_font_data.bin（二进制字库：头 + 码点表 + 四级位图）
//       src/cjk_font.c / src/cjk_font.h（lookup 实现，小文件）
//       src/quotes_app.c / src/quotes_app.h（引文表，App 层；开源通用化
//       Phase 2 2026-10-24 自 cjk_font 拆出——Core 渲染层不含学科内容）
//
// 字符集（P3，2026-08-20）：引文 ∪ 出处 ∪ GB2312 一级 3755 字 ∪ 常用全角
//       标点 ∪ 全角空格 U+3000 ∪ ASCII 0x20-0x7E。半角字符经墨迹盒变宽
//       渲染（reader_engine 逐字裁剪 blit），点阵本身仍是方格。
//
// 渲染：macOS CoreText（楷体 Kaiti SC 优先，加粗：Bold 变体或 3x3 膨胀），
//       每级独立两遍法定字号/基线（实测墨迹盒，Pass1 测极值 Pass2 居中），
//       灰度抗锯齿阈值 128，行主序 MSB-first，bit=1 着色，
//       与 epd_gfx_draw_bitmap 位图格式一致。
//
// bin 布局（小端，EMBED_FILES 编入固件，避免 ~5MB 的 C 数组源码；
// 头自描述：cp 表起点 = 12 + levels*4，消费端动态计算勿写死）：
//   [0..3]  "CKF1"          [4..5]  u16 version=1
//   [6..7]  u16 levels=4    [8..11] u32 n（字形数）
//   [12..19] u16 cell[4]    [20..27] u16 stride[4]
//   [28..]   u16 cp[n] 升序（4 对齐后） level0 位图 n*32B → level1 n*60B → level2 n*72B → level3 n*128B
//
// 用法：cd InkWord_Firmware && swift tools/gen_cjk_font.swift
//       （改引文/字表后重新运行即可，勿手改生成文件）
// 子集模式（v1.4 T4.5 字库子集下发）：
//       swift tools/gen_cjk_font.swift --subset <charset.txt> <deck_id>
//       读字符集文本（后端 /api/admin/decks/<code>/charset 导出），减去主集
//       src/cjk_font_data.bin 已收录码点，差集空即退出；差集字符渲染三级
//       位图 → ./deck_<id>.bin（CKF1 同格式，固件 cjk_font_sd 级联查找：
//       主集 miss → 子集；拷入 SD /fonts/deck_<id>.bin 生效）。

import Foundation
import CoreText
import CoreGraphics

// ---------- 0. 模式解析（T4.5）：全量（缺省）/ --subset 子集 ----------
let args = CommandLine.arguments
var subsetMode = false
var subsetDeckId = ""
if args.contains("--subset") {
    /* swift JIT：args[0]=脚本路径 → [1]="--subset" [2]=charset [3]=deck_id */
    guard args.count == 4 else {
        FileHandle.standardError.write(
            "usage: swift tools/gen_cjk_font.swift --subset <charset.txt> <deck_id>\n"
                .data(using: .utf8)!); exit(1)
    }
    subsetMode = true
    subsetDeckId = args[3]
}

let LEVELS = [16, 20, 24, 32]   // 像素格边长（level 0/1/2/3；32px 级 LARGE 档大屏）
let FONT_SIZE_HINT: [Int: CGFloat] = [16: 15, 20: 19, 24: 22, 32: 30]
                      // 24 级 23pt 时墨迹盒+Bold 外扩贴底（真机实测），缩 1pt  留裕量；
                      // 其余级按比例给初值，两遍法会自动再缩
let MAX_COLS = 8       // 引文区每行最多字符数
let MAX_LINES = 5      // 引文区每条最多行数

// ---------- 1. 解析引文（全量模式） ----------
var quotes: [[String]] = []
if !subsetMode {
let src = URL(fileURLWithPath: "tools/chuanxilu_quotes.txt")
guard let raw = try? String(contentsOf: src, encoding: .utf8) else {
    FileHandle.standardError.write("cannot read tools/chuanxilu_quotes.txt\n".data(using: .utf8)!); exit(1)
}
var cur: [String] = []
for line in raw.components(separatedBy: "\n") {
    if line.hasPrefix("#") { continue }
    if line.trimmingCharacters(in: .whitespaces).isEmpty {
        if !cur.isEmpty { quotes.append(cur); cur = [] }
        continue
    }
    cur.append(line)
}
if !cur.isEmpty { quotes.append(cur) }

// 布局校验（超标即失败，防止生成跑版数据）
for (qi, q) in quotes.enumerated() {
    precondition(q.count <= MAX_LINES, "quote #\(qi): \(q.count) lines > \(MAX_LINES)")
    for (li, l) in q.enumerated() {
        precondition(l.count <= MAX_COLS, "quote #\(qi) line \(li): '\(l)' \(l.count) chars > \(MAX_COLS)")
    }
}
}

// ---------- 2. 收集字符集 ----------
/* 出处串（右下角署名，与引文同字库渲染；字符集必须一并收录） */
let ATTRIB = "——王阳明《传习录》"

/* 常用全角标点（阅读正文 + 引文） */
let PUNCT = "，。、；：？！“”‘’（）《》〈〉【】「」『』…—·～‰℃°＋－×÷＝／　"
             // 尾字符为 U+3000 全角空格（渲染为空格宽）

/* IPA 音标字符集（词卡音标行 16px 点阵渲染，2026-08-23）：内嵌词库
 * phonetic 实测 19 扩展字符 + 词典备用（ː ɒ）。FreeSans 仅 0x20-0x7E
 * 真机 IPA 全跳过，音标行改点阵整行渲染后由本表兜底覆盖 */
let IPA = "ˈˌəɪɛæʊɑɔʌɡʃʤŋɜʧθðʒːɒ"

var charset = Set<Character>()
if subsetMode {
    /* 子集模式：读字符集文本 − 主集已收录码点 = 差集（生僻字刚需，
     * 后端 GET /api/admin/decks/<code>/charset 导出全文，此处吞
     * 换行/空白后逐字符去重） */
    guard let cs = try? String(contentsOf: URL(fileURLWithPath: args[2]),
                                encoding: .utf8) else {
        FileHandle.standardError.write(
            "cannot read \(args[2])\n".data(using: .utf8)!); exit(1)
    }
    for ch in cs where !ch.isWhitespace {
        charset.insert(ch)
    }
    guard let mainBin = try? Data(contentsOf: URL(fileURLWithPath:
                                "src/cjk_font_data.bin")) else {
        FileHandle.standardError.write(
            "cannot read src/cjk_font_data.bin (run full mode first)\n"
                .data(using: .utf8)!); exit(1)
    }
    func rd32(_ o: Int) -> Int {
        Int(mainBin[o]) | Int(mainBin[o + 1]) << 8 |
        Int(mainBin[o + 2]) << 16 | Int(mainBin[o + 3]) << 24
    }
    let n = rd32(8)
    /* cp 表起点动态：头自描述（12 + levels*4；主集升级四级后旧写死 24 失效，
     * 2026-09-03 32px 级引入） */
    let mainLevels = Int(mainBin[6]) | Int(mainBin[7]) << 8
    let mainCpOff = 12 + mainLevels * 4
    var mainCps = Set<UInt32>()
    for i in 0..<n {
        mainCps.insert(UInt32(mainBin[mainCpOff + 2 * i]) |
                       UInt32(mainBin[mainCpOff + 2 * i + 1]) << 8)
    }
    charset = Set(charset.filter {
        !mainCps.contains($0.unicodeScalars.first!.value)
    })
    if charset.isEmpty {
        FileHandle.standardError.write(
            "subset empty: all chars covered by main font, no deck font needed\n"
                .data(using: .utf8)!)
        exit(0)
    }
} else {
for q in quotes { for line in q { for ch in line { charset.insert(ch) } } }
for ch in ATTRIB { charset.insert(ch) }
for ch in PUNCT { charset.insert(ch) }
for ch in IPA { charset.insert(ch) }
for cp in 0x20...0x7E { charset.insert(Character(UnicodeScalar(cp)!)) }   // ASCII
}

var phonCps = Set<Character>()
if !subsetMode {
/* 词卡音标行全量字符收集（src/default_words.json phonetic 列）：诗词类
 * 词条该列填中文作者名（生成端语义，点阵一并渲染），人名生僻字
 * （翃燮夔等）不在 GB 一级库，不收录则真机画空心框 */
if let jsonData = try? Data(contentsOf: URL(fileURLWithPath: "src/default_words.json")),
   let obj = try? JSONSerialization.jsonObject(with: jsonData),
   let dict = obj as? [String: Any],
   let words = dict["words"] as? [[String: Any]] {
    for w in words {
        guard let ph = w["phonetic"] as? String else { continue }
        for ch in ph { if ch != "/" && ch != "[" && ch != "]" { phonCps.insert(ch) } }
    }
}
for ch in phonCps { charset.insert(ch) }
}

var gbCount = 0   /* 报告代码（L~683）顶层引用：声明留在块外，子集模式恒 0 */
if !subsetMode {
/* GB2312 一级字库 3755 字（区位 16-55），GBK 双字节解码取 Unicode。
 * 注：GB_2312_80(0x0630) 在新 macOS 解码失效（逐字节返回 nil，实测
 * 2026-08-20），改用 GBK_95(0x0631，GBK 对 GB2312 超集，解码 3760 槽
 * 含一级全部 + 少量空位填充字，多收无妨） */
let gbEnc = String.Encoding(
    rawValue: CFStringConvertEncodingToNSStringEncoding(
        CFStringEncoding(0x0631)))
for qu in 16...55 {
    for wei in 1...94 {
        let b0 = UInt8(0xA0 + qu), b1 = UInt8(0xA0 + wei)
        if let s = String(bytes: [b0, b1], encoding: gbEnc),
           let cp = s.unicodeScalars.first?.value, cp != 0xFFFD, cp != 0 {
            charset.insert(Character(UnicodeScalar(cp)!))
            gbCount += 1
        }
    }
}
precondition(gbCount >= 3700, "GB2312 level-1 decode suspiciously small: \(gbCount)")
}

let cps = charset.map { $0.unicodeScalars.first!.value }.sorted()
for cp in cps {
    precondition(cp < 0x10000, "non-BMP codepoint U+\(String(cp, radix: 16)) unsupported")
}

// ---------- 3. 选字体家族（分级链）+ Bold 能力 ----------
/* 字体链分级（2026-08-23 真机「虚感」修正）：小字级 16/20px 用黑体——
 * 笔画均匀、二值点阵最实；楷体 Bold 的横细竖粗在 1bit 小字下横画仍
 * 断续发虚，此为虚感主因。大字级 24px 保持楷体（待机页《传习录》
 * 引文书卷气、阅读器大字号楷韵不受影响）。PingFang 的 traitBold 在
 * macOS 映射 Semibold 半粗字重，正是小字点阵想要的浓度。 */
let SMALL_CHAIN = ["PingFang SC", "Heiti SC", "Hiragino Sans GB",
                   "Songti SC", "Kaiti SC"]   /* "Heiti SC" 方为实际家族名，
                   "STHeiti SC" 解析落 Helvetica（2026-08-23 探测实测） */
let LARGE_CHAIN = ["Kaiti SC", "Kaiti TC", "Songti SC", "STHeiti SC", "Hiragino Sans GB"]

/* 覆盖全部字符的第一个家族 + Bold 能力（原单链逻辑提取，两链各调）。
 * 白名单：纯 CJK 家族不含但无需该家族自带的字符——
 * ÷°℃‰×（全角符号，系统字体级联补）+ U+E810~E81F（GBK 解码一级库时
 * 未定义槽映射出的 PUA 噪音字，设备端永不显示；若不白名单，黑体/楷体
 * 链会被一票否决落到宋体）+ IPA/音标列生僻人名（PingFang 缺 IPA 全套
 * 与部分人名用字，实测 STHeiti SC 全覆盖；渲染走 CTLine 级联补，
 * 2026-08-23 音标行点阵化引入） */
func fallbackOK(_ cp: UInt16) -> Bool {
    [0x00F7, 0x00B0, 0x2103, 0x2030, 0x00D7].contains(cp) ||
    (0xE810...0xE81F).contains(cp) ||
    (0x0250...0x02AF).contains(cp) ||           // IPA 扩展
    (0x02C0...0x02DF).contains(cp) ||           // IPA 修饰符（ˈ ˌ ː）
    phonCps.contains(Character(UnicodeScalar(cp)!))  // 音标列收集集（人名等）
}
func pickFont(_ chain: [String]) -> (name: String, bold: Bool) {
    for name in chain {
        guard let f = CTFontCreateWithName(name as CFString, 16, nil) as CTFont? else { continue }
        let u16 = Array(String(charset).utf16)
        var glyphs = [CGGlyph](repeating: 0, count: u16.count)
        /* Bold 能力检测（全有/白名单两路径共用）：白名单路径原硬编码 false，
         * PingFang(→Semibold)/Kaiti(→Bold) 明明有真字重却被漏检落入 3x3
         * 膨胀——膨胀外扩 1px 与渲染后闭环互搏恶性缩字（16px 级 15pt→10pt，
         * 2026-08-23 实测教训） */
        let bold = CTFontCreateCopyWithSymbolicTraits(
            f, 16, nil,
            CTFontSymbolicTraits.traitBold, CTFontSymbolicTraits.traitBold) != nil
        if u16.isEmpty || CTFontGetGlyphsForCharacters(f, u16, &glyphs, u16.count) {
            return (name, bold)
        }
        /* 缺字诊断：白名单外的缺字才视为真缺口（print+String(radix:) 纯
         * Swift 路径——FileHandle.write+String(format:) 在 swift JIT 模式
         * 下实测 segfault，2026-08-23） */
        var missing = ""
        for (i, g) in glyphs.enumerated() where g == 0 && !fallbackOK(u16[i]) {
            if missing.isEmpty { missing = "... missing: " }
            if missing.count > 40 { break }
            missing += "U+" + String(u16[i], radix: 16, uppercase: true) + " "
        }
        if missing.isEmpty { return (name, bold) }   /* 仅白名单缺口：级联可补 */
        print("font '\(name)' skipped \(missing)")
    }
    preconditionFailure("no covering CJK font found")
}
let smallFont = pickFont(SMALL_CHAIN)
let largeFont = pickFont(LARGE_CHAIN)

/* 加粗（墨水屏笔画细则发虚）：优先真 Bold 变体；无则渲染后 3x3 膨胀 */
let BOLD = true
/* 二值化阈值（measure/render 共用，必须同步否则墨迹盒测量失准贴边）：
 * 100 = 覆盖率 ≥40% 保留（2026-08-23，原 128 丢弃抗锯齿边缘致细笔断续） */
let INK_THRESHOLD = 100

func fontForCell(_ cell: Int) -> (name: String, bold: Bool) {
    let (name, bold) = cell < 24 ? smallFont : largeFont
    return (name, bold)
}   /* 32px 级走 largeFont（楷体链，2026-09-03）：大字级书卷气优先，
    * 黑体浓度优势在 1bit 小字下才成立（见上方字体链分级注释）。
    * 二十四轮（2026-08-31）膨胀重构：dilate 语义从字体选择移出（原
    BOLD && !bold 只在无真 Bold 时膨胀，PingFang/Kaiti 有真字重故全库
    从未膨胀）；改为主循环在闭环外按级统一膨胀（见 dilateBits） */

/* 音标字符专属渲染字体（2026-08-23）：主链 PingFang 缺 IPA 全套，
 * CTLine 级联不可控（ˈ 级联渲染为空白，实测）。显式分派 STHeitiSC-Medium
 * （真字重，PostScript 名验证；单字体实测 21 个 IPA 字符全覆盖，
 * Medium 浓度与主链 Semibold 同级）；名字漂移（创建失败回退系统默认）
 * 则 STHeiti SC Regular 充当。分派范围：IPA 常量 + 音标列收集的
 * Latin/希腊扩展字符（< 0x3000）；人名汉字 PingFang 全覆盖不分派 */
var phonSet = Set<Character>()
for ch in IPA { phonSet.insert(ch) }
for ch in phonCps {
    let cp = ch.unicodeScalars.first!.value
    if cp > 0x7F && cp < 0x3000 { phonSet.insert(ch) }
}
var phonFontCache: [CGFloat: CTFont] = [:]
func phonFont(_ size: CGFloat) -> CTFont {
    if let f = phonFontCache[size] { return f }
    let f = CTFontCreateWithName("STHeitiSC-Medium" as CFString, size, nil)
    let ps = CTFontCopyPostScriptName(f) as String? ?? ""
    let chosen = ps == "STHeitiSC-Medium" ? f
        : CTFontCreateWithName("STHeiti SC" as CFString, size, nil)
    phonFontCache[size] = chosen
    return chosen
}

func fontLabel(_ cell: Int) -> String {
    let (name, bold) = fontForCell(cell)
    return name + (bold ? " Bold" : "")
}
let fontsDesc = "16/20px \(fontLabel(16)) + 24/32px \(fontLabel(24))"

func makeFont(_ size: CGFloat, _ cell: Int) -> CTFont {
    let (name, bold) = fontForCell(cell)
    var f = CTFontCreateWithName(name as CFString, size, nil) as CTFont
    if bold,
       let b = CTFontCreateCopyWithSymbolicTraits(
           f, size, nil,
           CTFontSymbolicTraits.traitBold, CTFontSymbolicTraits.traitBold) {
        f = b
    }
    return f
}

// ---------- 3.5 两遍法定字号/基线（每级独立）：实测墨迹盒，不再猜 em 字面框 ----------
/* Pass1 大画布逐字实测全部字符相对 pen 的墨迹极值 A(基线上)/B(基线下探)/L/R，
 * Pass2 据此在格内居中定基线与 pen x；墨迹盒装不下 CELL-2 时自动缩 1pt 重测
 * （保证上下各留 ≥1px，四边 edge-touch 恒为 0）。见 2026-08-18 真机裁切教训。 */
let sortedChars = charset.sorted(by: { $0.unicodeScalars.first!.value < $1.unicodeScalars.first!.value })

func measureGlyph(_ ch: Character, _ f: CTFont, cell: Int) -> (l: CGFloat, r: CGFloat, t: CGFloat, b: CGFloat)? {
    /* 音标字符分派专属字体（见 phonFont 注释） */
    let f = phonSet.contains(ch) ? phonFont(CTFontGetSize(f)) : f
    let BIGPAD = 16
    let BIG = cell + BIGPAD * 2
    var buf = [UInt8](repeating: 255, count: BIG * BIG)
    let ctx = CGContext(data: &buf, width: BIG, height: BIG,
                        bitsPerComponent: 8, bytesPerRow: BIG,
                        space: CGColorSpaceCreateDeviceGray(),
                        bitmapInfo: CGImageAlphaInfo.none.rawValue)!
    let attr = NSAttributedString(string: String(ch), attributes: [
        NSAttributedString.Key(kCTFontAttributeName as String): f as Any
    ])
    let line = CTLineCreateWithAttributedString(attr)
    let penX = CGFloat(BIGPAD), penY = CGFloat(BIGPAD)
    ctx.textPosition = CGPoint(x: penX, y: penY)
    CTLineDraw(line, ctx)
    var minx = BIG, maxx = -1, miny = BIG, maxy = -1
    for y in 0..<BIG {
        for x in 0..<BIG where buf[y * BIG + x] < INK_THRESHOLD {
            if x < minx { minx = x }; if x > maxx { maxx = x }
            if y < miny { miny = y }; if y > maxy { maxy = y }
        }
    }
    if maxx < 0 { return nil }   // 空字形（空格等全白字符）
    /* 内存第 0 行 = 视觉顶部 = CG y 最大（翻转关系）；x 无翻转 */
    return (CGFloat(minx) - penX, CGFloat(maxx) - penX,
            CGFloat(BIG - 1 - miny) - penY, CGFloat(BIG - 1 - maxy) - penY)
}

func renderGlyph(_ ch: Character, _ f: CTFont, cell: Int, stride: Int,
                 textX: CGFloat, baseline: CGFloat) -> [UInt8] {
    /* 音标字符分派专属字体（STHeitiSC-Medium 真字重）。二十四轮（2026-08-31）
     * 膨胀重构：dilate 移出本函数——闭环内膨胀会被 edgeTouch 检出伪贴边 →
     * 缩字号 → 再膨胀再贴边的恶性互搏（2026-08-23 Bold 漏检事故同机理），
     * 改为主循环在闭环/降级/合成全部结束后按级统一膨胀（dilateBits） */
    let isPhon = phonSet.contains(ch)
    let f = isPhon ? phonFont(CTFontGetSize(f)) : f
    var buf = [UInt8](repeating: 255, count: cell * cell)   // 灰度，白底
    let ctx = CGContext(data: &buf, width: cell, height: cell,
                        bitsPerComponent: 8, bytesPerRow: cell,
                        space: CGColorSpaceCreateDeviceGray(),
                        bitmapInfo: CGImageAlphaInfo.none.rawValue)!
    let attr = NSAttributedString(string: String(ch), attributes: [
        NSAttributedString.Key(kCTFontAttributeName as String): f as Any
    ])
    let line = CTLineCreateWithAttributedString(attr)
    ctx.textPosition = CGPoint(x: textX, y: baseline)
    CTLineDraw(line, ctx)

    let glyphBytes = stride * cell
    var bits = [UInt8](repeating: 0, count: glyphBytes)
    for gy in 0..<cell {
        for gx in 0..<cell {
            /* CGBitmapContext 内存第 0 行即视觉顶部，图像行 gy 直接读内存行 gy */
            /* 墨水屏 1bit 二值化（阈值与 measureGlyph 共用 INK_THRESHOLD） */
            if buf[gy * cell + gx] < INK_THRESHOLD {
                bits[gy * stride + gx / 8] |= UInt8(0x80 >> (gx % 8))
            }
        }
    }

    return bits
}

/* 3x3 膨胀（位图级机械加粗，从原 renderGlyph 提出）：二十四轮（2026-08-31）
 * 曾对 20px 级启用，真机实测晕染回滚——Semibold 基础上再膨胀，密笔画
 * 汉字（量/赢/疆类，笔画间隙 1~2px）间隙被填死糊成墨块，低对比度灰底
 * 上墨块边缘发虚加重晕染感。教训：低对比度屏小字可读性 ≠ 无限加粗，
 * 笔画间隙的存留比墨迹浓度更重要；常规字重 + 足够字号才是正解 */
func dilateBits(_ bits: [UInt8], cell: Int, stride: Int) -> [UInt8] {
    var out = bits
    func bit(_ y: Int, _ x: Int) -> Bool {
        bits[y * stride + x / 8] & UInt8(0x80 >> (x % 8)) != 0
    }
    for gy in 0..<cell {
        for gx in 0..<cell where bit(gy, gx) {
            for dy in -1...1 {
                for dx in -1...1 {
                    let y = gy + dy, x = gx + dx
                    if y >= 0 && y < cell && x >= 0 && x < cell {
                        out[y * stride + x / 8] |= UInt8(0x80 >> (x % 8))
                    }
                }
            }
        }
    }
    return out
}
/* 二十四轮回滚：膨胀不再调用（函数保留供后续温和变体实验，如右下 2x2） */

/* stderr 报告通道（必须先于 level 渲染循环初始化：swift JIT 顶层代码
 * 按序执行，循环内 say() 若早于此定义调用，会捕获尚未初始化的 err
 * → EXC_BAD_ACCESS segfault，2026-08-23 实测教训） */
let err = FileHandle.standardError
func say(_ s: String) { try? err.write(contentsOf: Data((s + "\n").utf8)) }

/* 逐级渲染（两遍法自适应字号 + 位图 + 边界触碰检测） */
struct LevelOut {
    let cell: Int, stride: Int
    var fontSize: CGFloat = 0
    var bits: [[UInt8]] = []
    var touched = 0
    var demoted = 0   /* per-glyph 降级字数 */
}
var levelOuts: [LevelOut] = []

/* 位图四边贴边像素数（整级/单字形共用） */
func edgeTouch(_ b: [UInt8], cell: Int, stride: Int) -> Int {
    var t = 0
    for gx in [0, cell - 1] { for gy in 0..<cell {
        if b[gy * stride + gx / 8] & UInt8(0x80 >> (gx % 8)) != 0 { t += 1 } } }
    for gy in [0, cell - 1] { for gx in 0..<cell {
        if b[gy * stride + gx / 8] & UInt8(0x80 >> (gx % 8)) != 0 { t += 1 } } }
    return t
}

/* 单字形自适应降级：从 baseSize 起独立两遍法居中渲染、贴边则缩 1pt 直至
 * 装下（字号不同基线不同，不能用整级基线）。cell 制点阵按格对齐，
 * 降级字形仅比同级略小、无基线错乱（2026-08-23） */
func renderGlyphFit(_ ch: Character, cell: Int, stride: Int, baseSize: CGFloat) -> [UInt8] {
    var size = baseSize
    var last: [UInt8] = []
    while size >= 8 {
        let f = makeFont(size, cell)
        guard let m = measureGlyph(ch, f, cell: cell) else { break }  /* 空字形无墨不贴边 */
        let baseline = ((CGFloat(cell) + (-m.b) - m.t) / 2).rounded()
        let textX = ((CGFloat(cell) - m.l - m.r) / 2).rounded()
        let b = renderGlyph(ch, f, cell: cell, stride: stride,
                            textX: textX, baseline: baseline)
        if edgeTouch(b, cell: cell, stride: stride) == 0 { return b }
        last = b
        size -= 1
    }
    return last   /* 8pt 仍贴边（理论不至）：接受裁切 */
}

/* IPA 重音/长音/间隔号合成位图（2026-08-24）：字体渲染的 ˈ ˌ 在 16px 级
 * （12pt）1px 细竖笔低于二值化阈值被整体丢弃——真机音标行重音符
 * 渲染成空格（报障 /səˈsaɪəti/ 显为 sə saɪəti），ː 双三角 7 点、
 * · 间隔号 2 点同样细弱。四个记号本质是几何符号，直接按级合成：
 * ˈ 顶部竖笔（IPA 主重音惯例位置）、ˌ 底部竖笔（次重音下半线）、
 * ː 中部双点（三角冒号近似）、· 中心方点（中点全档统一，底部
 * 标签行间隔号同步受益）；笔画宽与主链 Bold 笔宽同源（cell/8 取整） */
func synthModifier(_ cp: UInt32, cell: Int, stride: Int) -> [UInt8]? {
    guard [0x02C8, 0x02CC, 0x02D0, 0x00B7].contains(cp) else { return nil }
    var bits = [UInt8](repeating: 0, count: stride * cell)
    let pen = max(2, (cell + 7) / 8)          // 笔宽：16/20→2px、24→3px
    func fill(_ x0: Int, _ y0: Int, _ w: Int, _ h: Int) {
        for y in y0..<(y0 + h) { for x in x0..<(x0 + w) {
            bits[y * stride + x / 8] |= UInt8(0x80 >> (x % 8)) } }
    }
    switch cp {
    case 0x02C8:                              // ˈ 主重音：顶部竖笔
        fill((cell - pen) / 2, 1, pen, max(4, cell / 3))
    case 0x02CC:                              // ˌ 次重音：底部竖笔
        let h = max(3, cell / 5)
        fill((cell - pen) / 2, cell - 2 - h, pen, h)
    case 0x02D0:                              // ː 长音符：中部双点
        let d = max(2, (cell + 7) / 8)
        fill((cell - d) / 2, cell * 2 / 5, d, d)
        fill((cell - d) / 2, cell * 3 / 5, d, d)
    default:                                  // · 间隔号：中心方点
        let d = max(2, (cell + 7) / 8)
        fill((cell - d) / 2, (cell - d) / 2, d, d)
    }
    return bits
}

for cell in LEVELS {
    let stride = (cell + 7) / 8
    var out = LevelOut(cell: cell, stride: stride)
    var fontSize = FONT_SIZE_HINT[cell] ?? CGFloat(cell - 2)
    /* 二十四轮：膨胀不在此层（闭环内膨胀会被 edgeTouch 检出伪贴边 → 恶性
     * 缩字）；20px 级在下方 synthModifier 替换后统一 dilateBits */
    /* 贴边容忍：整级渲染后残余贴边 ≤ 此像素值不缩整级，改走 per-glyph
     * 降级（renderGlyphFit）。原闭环追求绝对归零，为 1~2 个极端墨迹字
     * 缩整级 1pt，三级各白损 6~8% 字面（2026-08-23 实测 16px 级因此
     * 12pt→11pt） */
    let TOUCH_TOLERANCE = 256

    /* 渲染后实测闭环：measure（大画布）与 render（小画布）光栅化在低阈值
     * 下存在亚像素级差异，开环定位会残留贴边；渲染完直接扫位图四边，
     * 贴边超容忍值则缩 1pt 整级重渲染，收敛后少量残余走 per-glyph 降级
     * （2026-08-23） */
    while true {
        let f = makeFont(fontSize, cell)
        var A: CGFloat = 0, B: CGFloat = 0, L: CGFloat = 0, R: CGFloat = 0
        for ch in sortedChars {
            guard let m = measureGlyph(ch, f, cell: cell) else { continue }
            if m.t > A { A = m.t }
            if m.b < B { B = m.b }
            if m.l < L { L = m.l }
            if m.r > R { R = m.r }
        }
        if A - B <= CGFloat(cell) - 2 && R - L <= CGFloat(cell) - 2 {
            /* 取整对齐光栅化相位（measure 的 pen 为整数） */
            let baseline = ((CGFloat(cell) + (-B) - A) / 2).rounded()
            let textX = ((CGFloat(cell) - L - R) / 2).rounded()
            let font = makeFont(fontSize, cell)
            out.fontSize = fontSize
            for ch in sortedChars {
                let b = renderGlyph(ch, font, cell: cell, stride: stride,
                                    textX: textX, baseline: baseline)
                out.bits.append(b)
            }
            out.touched = out.bits.reduce(0) { $0 + edgeTouch($1, cell: cell, stride: stride) }
            if out.touched <= TOUCH_TOLERANCE { break }   /* 少量贴边走 per-glyph 降级 */
            say("// level cell=\(cell) fontSize=\(Int(fontSize)) edge-touch=\(out.touched) -> shrink & rerender")
            out.bits.removeAll()
            out.touched = 0
        }
        fontSize -= 1
        precondition(fontSize >= 8, "glyph ink box cannot fit even at 8pt (cell=\(cell))")
    }
    /* per-glyph 降级：残余贴边字形单独缩 1pt 独立居中重渲（仅占极端
     * 墨迹的少数字，保住整级字面）；降级后整级贴边归零 */
    for (i, ch) in sortedChars.enumerated()
    where edgeTouch(out.bits[i], cell: cell, stride: stride) > 0 {
        out.bits[i] = renderGlyphFit(ch, cell: cell, stride: stride,
                                     baseSize: fontSize - 1)
        out.demoted += 1
    }
    if out.demoted > 0 {
        say("// level cell=\(cell) demoted \(out.demoted) glyphs to \(Int(fontSize) - 1)pt (extreme ink box)")
    }
    /* IPA 记号合成替换（见 synthModifier 注释）：在降级后、进库前覆盖，
     * 合成图严格避开 cell 四边（ˈ 行 1 起、ˌ 行 cell-2 止）不引入贴边 */
    for (i, ch) in sortedChars.enumerated() {
        if let s = synthModifier(ch.unicodeScalars.first!.value,
                                 cell: cell, stride: stride) {
            out.bits[i] = s
        }
    }
    /* 二十四轮（2026-08-31）回滚：原 20px 级统一膨胀块删除——真机实测
     * 晕染（见 dilateBits 注释），恢复 Semibold/STHeitiSC-Medium 原样位图 */
    levelOuts.append(out)
}

// ---------- 4. 打包 bin ----------
func le16(_ v: Int) -> [UInt8] { [UInt8(v & 0xFF), UInt8((v >> 8) & 0xFF)] }
func le32(_ v: Int) -> [UInt8] { [UInt8(v & 0xFF), UInt8((v >> 8) & 0xFF),
                                 UInt8((v >> 16) & 0xFF), UInt8((v >> 24) & 0xFF)] }
var bin = Data()
bin.append(contentsOf: Array("CKF1".utf8))
bin.append(contentsOf: le16(1))               // version
bin.append(contentsOf: le16(LEVELS.count))    // levels
bin.append(contentsOf: le32(cps.count))       // n
for l in levelOuts { bin.append(contentsOf: le16(l.cell)) }
for l in levelOuts { bin.append(contentsOf: le16(l.stride)) }
for cp in cps { bin.append(contentsOf: le16(Int(cp))) }
while bin.count % 4 != 0 { bin.append(0) }
for l in levelOuts { for b in l.bits { bin.append(contentsOf: b) } }
if subsetMode {
    let outPath = "deck_\(subsetDeckId).bin"
    try! bin.write(to: URL(fileURLWithPath: outPath))
    say("// subset deck=\(subsetDeckId) glyphs=\(cps.count) bin=\(bin.count)B -> \(outPath)")
    say("// copy to SD: /fonts/deck_\(subsetDeckId).bin (cjk_font_sd 级联加载)")
    exit(0)   /* 子集不生成 c/h 与引文表（主集不变） */
}
try! bin.write(to: URL(fileURLWithPath: "src/cjk_font_data.bin"))

// ---------- 5. 输出 C（lookup 实现 + 引文表，数据在 bin） ----------
/* 先算好各级描述（避免多行字符串插值内嵌套引号字面量，Swift 解析器不支持） */
let levelDesc = levelOuts.map { "\($0.cell)px=\($0.stride * $0.cell)B" }.joined(separator: " + ")
let levelCells = LEVELS.map { String($0) }.joined(separator: "/")
let totalGlyphBytes = cps.count * levelOuts.reduce(0) { $0 + $1.stride * $1.cell }
var c = """
/**
 * @file cjk_font.c
 * @brief 中文点阵字库 lookup（生成文件，勿手改；引文表已拆出 quotes_app.c）
 *
 * 字形数据在 cjk_font_data.bin（CMake EMBED_FILES 编入固件）：
 *   \(fontsDesc)，四级 \(levelCells)px，
 *   \(cps.count) 字形 x \(levelDesc)
 *   = \(totalGlyphBytes) 字节。
 * 码点升序二分查找；位图行主序 MSB-first，bit=1 着色（epd_gfx_draw_bitmap 格式）。
 * 由 tools/gen_cjk_font.swift 生成；改字表/引文后重跑 swift tools/gen_cjk_font.swift。
 */
#include "cjk_font.h"
#include <stddef.h>

/* objcopy 嵌入符号（platformio.ini board_build.embed_files=src/cjk_font_data.bin，
 * 路径含 src/ → 符号带 src_ 前缀；ESPIDF CMake 迁移后为无前缀版，届时
 * 须同步，见 src/CMakeLists.txt 头注释） */
extern const uint8_t _binary_src_cjk_font_data_bin_start[];
#define BIN_BASE (_binary_src_cjk_font_data_bin_start)

/* bin 头（小端，自描述）：0..3 magic, 4..5 ver, 6..7 levels, 8..11 n,
 * cell[levels] @12、stride[levels] 紧随，cp 表起点 = 12+levels*4，
 * 4 对齐后按级位图（2026-09-03 四级化，消费端动态计算勿写死偏移） */
static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t glyph_n(void)      { return rd_le32(BIN_BASE + 8); }
static uint16_t bin_levels(void)   { const uint8_t *p = BIN_BASE + 6; return (uint16_t)(p[0] | (p[1] << 8)); }
static uint16_t glyph_cell(int lvl)  { const uint8_t *p = BIN_BASE + 12 + lvl * 2; return (uint16_t)(p[0] | (p[1] << 8)); }
static uint16_t glyph_stride(int lvl){ const uint8_t *p = BIN_BASE + 12 + bin_levels() * 2 + lvl * 2; return (uint16_t)(p[0] | (p[1] << 8)); }

/* cp 表起点 = 12 + levels*4（bin 头自描述，cp_table/level_base 唯一同源。
 * 2026-09-03 四级化漏改事故存档：level_base 曾硬编码旧三级头 24，
 * 四级 bin 下位图基址左移 4B —— 16/32px 级 stride 整除4B 恰整行仅
 * 字形平移（视觉无感），20/24px 级 stride=3 行错乱，真机释义区
 * 每字右侧破碎（2026-09-03 3.7" 真机定位，勿再写死偏移）。
 * 模板同步修复（2026-10-24 开源通用化 Phase 4：事故修复曾只改了
 * src/cjk_font.c 未同步本模板，重生成会复现四级错位事故） */
static uint32_t cp_table_off(void) { return 12 + (uint32_t)bin_levels() * 4; }

static const uint16_t *cp_table(void)
{
    return (const uint16_t *)(BIN_BASE + cp_table_off());
}

static const uint8_t *level_base(int lvl)
{
    uint32_t n = glyph_n();
    uint32_t off = cp_table_off() + 2 * n;
    off = (off + 3) & ~3u;
    for (int i = 0; i < lvl; i++)
        off += n * (uint32_t)glyph_stride(i) * (uint32_t)glyph_cell(i);
    return BIN_BASE + off;
}

const uint8_t *cjk_glyph_lookup_level(uint32_t cp, int level)
{
    if (level < 0 || level >= CJK_FONT_LEVELS) return NULL;
    const uint16_t *tab = cp_table();
    int lo = 0, hi = (int)glyph_n() - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (tab[mid] == (uint16_t)cp)
            return level_base(level) +
                   (uint32_t)mid * glyph_stride(level) * glyph_cell(level);
        if (tab[mid] < cp) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

const uint8_t *cjk_glyph_lookup(uint32_t cp)
{
    return cjk_glyph_lookup_level(cp, CJK_FONT_LEVELS - 1);  /* 最大级兼容（零外部消费方） */
}

int cjk_glyph_cell_size(int level)   { return glyph_cell(level); }
int cjk_glyph_stride_size(int level) { return glyph_stride(level); }

"""
// 开源通用化 Phase 2（2026-10-24）：引文表拆出 cjk_font.c → quotes_app.c
// （App 层内容不驻留 Core 渲染层；数据搬家非渲染变化，字库 bin 字符集不变）
try! c.write(to: URL(fileURLWithPath: "src/cjk_font.c"), atomically: true, encoding: .utf8)

var q = """
/**
 * @file quotes_app.c
 * @brief 《传习录》引文表数据（生成文件，勿手改）
 *
 * 开源通用化 Phase 2（2026-10-24）：自 cjk_font.c 迁出（数据搬家非
 * 渲染变化）。由 tools/gen_cjk_font.swift 生成；改引文编辑
 * tools/chuanxilu_quotes.txt 后重跑 swift tools/gen_cjk_font.swift。
 */
#include "quotes_app.h"

"""
q += "const char *const k_chuanxilu_quotes[\(quotes.count)] = {\n"
q += quotes.map { "    \"" + $0.joined(separator: "\\n") + "\"," }.joined(separator: "\n")
q += "\n};\n\n"
q += "/* 引文出处（右下角署名，与引文同字库 24px 级） */\n"
q += "const char k_chuanxilu_attrib[] = \"\(ATTRIB)\";\n"
try! q.write(to: URL(fileURLWithPath: "src/quotes_app.c"), atomically: true, encoding: .utf8)

let h = """
/**
 * @file cjk_font.h
 * @brief 中文点阵字库接口（四级 16/20/24/32px；生成文件勿手改）
 *
 * 字形数据 cjk_font_data.bin（EMBED_FILES 编入固件），码点升序二分查找。
 * 位图行主序 MSB-first，bit=1 着色，可直接 blit 到 epd_gfx_draw_bitmap。
 * level 档位：0=16px / 1=20px / 2=24px / 3=32px（32px 级 LARGE 档大屏，
 * 2026-09-03）；阅读器按级取形并做墨迹盒变宽渲染（reader_engine）。
 * 引文表已拆出至 quotes_app.h（App 层，开源通用化 Phase 2 2026-10-24）。
 * 由 tools/gen_cjk_font.swift 生成。
 */
#ifndef INKWORD_CJK_FONT_H
#define INKWORD_CJK_FONT_H

#include <stdint.h>

#define CJK_FONT_LEVELS    \(LEVELS.count)                 /**< 字号级数 */
#define CJK_GLYPH_W       \(LEVELS.last!)                 /**< 兼容宏：最大级字形宽（零消费方，随级数自适） */
#define CJK_GLYPH_H       \(LEVELS.last!)                 /**< 兼容宏：最大级字形高 */
#define CJK_GLYPH_STRIDE  \((LEVELS.last! + 7) / 8)       /**< 兼容宏：最大级每行字节数 */
#define CJK_GLYPH_N       \(cps.count)                 /**< 字形总数（各级共用码点表） */

/** UTF-32 码点 -> 指定级字形位图；未收录返回 NULL（调用方画占位框） */
const uint8_t *cjk_glyph_lookup_level(uint32_t cp, int level);

/** 兼容 API：UTF-32 码点 -> 最大级字形位图（零外部消费方）；未收录返回 NULL */
const uint8_t *cjk_glyph_lookup(uint32_t cp);

/** 指定级字形边长（px）：16/20/24/32；level 越界返回 0 */
int cjk_glyph_cell_size(int level);

/** 指定级每行字节数：2/3/3/4；level 越界返回 0 */
int cjk_glyph_stride_size(int level);

#endif /* INKWORD_CJK_FONT_H */
"""
try! h.write(to: URL(fileURLWithPath: "src/cjk_font.h"), atomically: true, encoding: .utf8)

let qh = """
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

#define CHUANXILU_QUOTE_N \(quotes.count)                /**< 引文条数（=小时数） */
/** 待机页逐时轮换引文（UTF-8，\\n 分行，每行 <=\(MAX_COLS)字） */
extern const char *const k_chuanxilu_quotes[CHUANXILU_QUOTE_N];

/** 引文出处（右下角署名，UTF-8 单行） */
extern const char k_chuanxilu_attrib[];

#endif /* INKWORD_QUOTES_APP_H */
"""
try! qh.write(to: URL(fileURLWithPath: "src/quotes_app.h"), atomically: true, encoding: .utf8)

// ---------- 6. stderr 报告 + ASCII 预览 ----------
say("// fonts=\(fontsDesc) glyphs=\(cps.count) (gb2312-1=\(gbCount)) quotes=\(quotes.count)")
for l in levelOuts {
    say("// level cell=\(l.cell) fontSize=\(Int(l.fontSize)) stride=\(l.stride) " +
        "bytes=\(l.bits.count * l.stride * l.cell) edge-touch=\(l.touched) demoted=\(l.demoted)")
}
say("// bin total = \(bin.count) bytes")
let previewSet: [Character: Int] = {
    var m: [Character: Int] = [:]
    for (i, ch) in sortedChars.enumerated() { m[ch] = i }
    return m
}()
let previews: [Character] = ["知", "行", "A", "，", "ə", "ˈ"]
for preview in previews {
    guard let idx = previewSet[preview] else { continue }
    let l = levelOuts[LEVELS.count - 1]   // 最大级预览
    say("// preview '\(preview)' (\(l.cell)px):")
    for gy in 0..<l.cell {
        var row = ""
        for gx in 0..<l.cell {
            let bit = l.bits[idx][gy * l.stride + gx / 8] & UInt8(0x80 >> (gx % 8))
            row += bit != 0 ? "#" : "."
        }
        say("// " + row)
    }
}
