// gen_cjk_font.swift —— 中文点阵字库生成器（三级 16/20/24px，P3 阅读模式）
//
// 输入：tools/chuanxilu_quotes.txt（待机页引文，字符集一并收录）
// 输出：src/cjk_font_data.bin（二进制字库：头 + 码点表 + 三级位图）
//       src/cjk_font.c / src/cjk_font.h（lookup 实现 + 引文表，小文件）
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
// bin 布局（小端，EMBED_FILES 编入固件，避免 ~5MB 的 C 数组源码）：
//   [0..3]  "CKF1"          [4..5]  u16 version=1
//   [6..7]  u16 levels=3    [8..11] u32 n（字形数）
//   [12..17] u16 cell[3]    [18..23] u16 stride[3]
//   [24..]   u16 cp[n] 升序（4 对齐后） level0 位图 n*32B → level1 n*60B → level2 n*72B
//
// 用法：cd InkWord_Firmware && swift tools/gen_cjk_font.swift
//       （改引文/字表后重新运行即可，勿手改生成文件）

import Foundation
import CoreText
import CoreGraphics

let LEVELS = [16, 20, 24]   // 像素格边长（level 0/1/2；阅读器三级字号）
let FONT_SIZE_HINT: [Int: CGFloat] = [16: 15, 20: 19, 24: 22]
                      // 24 级 23pt 时墨迹盒+Bold 外扩贴底（真机实测），缩 1pt 留裕量；
                      // 其余级按比例给初值，两遍法会自动再缩
let MAX_COLS = 8       // 引文区每行最多字符数
let MAX_LINES = 5      // 引文区每条最多行数

// ---------- 1. 解析引文 ----------
let src = URL(fileURLWithPath: "tools/chuanxilu_quotes.txt")
guard let raw = try? String(contentsOf: src, encoding: .utf8) else {
    FileHandle.standardError.write("cannot read tools/chuanxilu_quotes.txt\n".data(using: .utf8)!); exit(1)
}
var quotes: [[String]] = []
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

// ---------- 2. 收集字符集 ----------
/* 出处串（右下角署名，与引文同字库渲染；字符集必须一并收录） */
let ATTRIB = "——王阳明《传习录》"

/* 常用全角标点（阅读正文 + 引文） */
let PUNCT = "，。、；：？！“”‘’（）《》〈〉【】「」『』…—·～‰℃°＋－×÷＝／　"
             // 尾字符为 U+3000 全角空格（渲染为空格宽）

var charset = Set<Character>()
for q in quotes { for line in q { for ch in line { charset.insert(ch) } } }
for ch in ATTRIB { charset.insert(ch) }
for ch in PUNCT { charset.insert(ch) }
for cp in 0x20...0x7E { charset.insert(Character(UnicodeScalar(cp)!)) }   // ASCII

/* GB2312 一级字库 3755 字（区位 16-55），GBK 双字节解码取 Unicode。
 * 注：GB_2312_80(0x0630) 在新 macOS 解码失效（逐字节返回 nil，实测
 * 2026-08-20），改用 GBK_95(0x0631，GBK 对 GB2312 超集，解码 3760 槽
 * 含一级全部 + 少量空位填充字，多收无妨） */
let gbEnc = String.Encoding(
    rawValue: CFStringConvertEncodingToNSStringEncoding(
        CFStringEncoding(0x0631)))
var gbCount = 0
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

let cps = charset.map { $0.unicodeScalars.first!.value }.sorted()
for cp in cps {
    precondition(cp < 0x10000, "non-BMP codepoint U+\(String(cp, radix: 16)) unsupported")
}

// ---------- 3. 选字体家族（覆盖全部字符的第一个）+ Bold 能力 ----------
let fontNames = ["Kaiti SC", "Kaiti TC", "Songti SC", "STHeiti SC", "Hiragino Sans GB"]
var fontFamily = ""
for name in fontNames {
    guard let f = CTFontCreateWithName(name as CFString, 16, nil) as CTFont? else { continue }
    let u16 = Array(String(charset).utf16)
    var glyphs = [CGGlyph](repeating: 0, count: u16.count)
    if u16.isEmpty || CTFontGetGlyphsForCharacters(f, u16, &glyphs, u16.count) {
        fontFamily = name; break
    }
}
precondition(!fontFamily.isEmpty, "no covering CJK font found")

/* 加粗（墨水屏笔画细则发虚，全级 Bold）：优先真 Bold 变体；无则渲染后 3x3 膨胀 */
let BOLD = true
var hasBold = false
if BOLD,
   let probe = CTFontCreateWithName(fontFamily as CFString, 16, nil) as CTFont?,
   CTFontCreateCopyWithSymbolicTraits(
       probe, 16, nil,
       CTFontSymbolicTraits.traitBold, CTFontSymbolicTraits.traitBold) != nil {
    hasBold = true
}
let useDilate = BOLD && !hasBold
let fontLabel = fontFamily + (hasBold ? " Bold" : "")

func makeFont(_ size: CGFloat) -> CTFont {
    var f = CTFontCreateWithName(fontFamily as CFString, size, nil) as CTFont
    if hasBold,
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
        for x in 0..<BIG where buf[y * BIG + x] < 128 {
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
            if buf[gy * cell + gx] < 128 {
                bits[gy * stride + gx / 8] |= UInt8(0x80 >> (gx % 8))
            }
        }
    }

    /* 无 Bold 变体时的机械加粗：3x3 膨胀 */
    if useDilate {
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
        bits = out
    }
    return bits
}

/* 逐级渲染（两遍法自适应字号 + 位图 + 边界触碰检测） */
struct LevelOut {
    let cell: Int, stride: Int
    var fontSize: CGFloat = 0
    var bits: [[UInt8]] = []
    var touched = 0
}
var levelOuts: [LevelOut] = []

for cell in LEVELS {
    let stride = (cell + 7) / 8
    var out = LevelOut(cell: cell, stride: stride)
    var fontSize = FONT_SIZE_HINT[cell] ?? CGFloat(cell - 2)

    while true {
        let f = makeFont(fontSize)
        var A: CGFloat = 0, B: CGFloat = 0, L: CGFloat = 0, R: CGFloat = 0
        for ch in sortedChars {
            guard let m = measureGlyph(ch, f, cell: cell) else { continue }
            if m.t > A { A = m.t }
            if m.b < B { B = m.b }
            if m.l < L { L = m.l }
            if m.r > R { R = m.r }
        }
        if A - B <= CGFloat(cell) - 2 && R - L <= CGFloat(cell) - 2 {
            let baseline = (CGFloat(cell) + (-B) - A) / 2
            let textX = (CGFloat(cell) - L - R) / 2
            let font = makeFont(fontSize)
            out.fontSize = fontSize
            for ch in sortedChars {
                let b = renderGlyph(ch, font, cell: cell, stride: stride,
                                    textX: textX, baseline: baseline)
                out.bits.append(b)
                for gx in [0, cell - 1] { for gy in 0..<cell {
                    if b[gy * stride + gx / 8] & UInt8(0x80 >> (gx % 8)) != 0 { out.touched += 1 } } }
                for gy in [0, cell - 1] { for gx in 0..<cell {
                    if b[gy * stride + gx / 8] & UInt8(0x80 >> (gx % 8)) != 0 { out.touched += 1 } } }
            }
            break
        }
        fontSize -= 1
        precondition(fontSize >= 8, "glyph ink box cannot fit even at 8pt (cell=\(cell))")
    }
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
try! bin.write(to: URL(fileURLWithPath: "src/cjk_font_data.bin"))

// ---------- 5. 输出 C（lookup 实现 + 引文表，数据在 bin） ----------
/* 先算好各级描述（避免多行字符串插值内嵌套引号字面量，Swift 解析器不支持） */
let levelDesc = levelOuts.map { "\($0.cell)px=\($0.stride * $0.cell)B" }.joined(separator: " + ")
let levelCells = LEVELS.map { String($0) }.joined(separator: "/")
let totalGlyphBytes = cps.count * levelOuts.reduce(0) { $0 + $1.stride * $1.cell }
var c = """
/**
 * @file cjk_font.c
 * @brief 中文点阵字库 lookup + 《传习录》引文表（生成文件，勿手改）
 *
 * 字形数据在 cjk_font_data.bin（CMake EMBED_FILES 编入固件）：
 *   \(fontLabel)，三级 \(levelCells)px，
 *   \(cps.count) 字形 x \(levelDesc)
 *   = \(totalGlyphBytes) 字节。
 * 码点升序二分查找；位图行主序 MSB-first，bit=1 着色（epd_gfx_draw_bitmap 格式）。
 * 由 tools/gen_cjk_font.swift 生成；改字表/引文后重跑 swift tools/gen_cjk_font.swift。
 */
#include "cjk_font.h"
#include <stddef.h>

/* EMBED_FILES 链接符号（CMakeLists：EMBED_FILES "cjk_font_data.bin"） */
extern const uint8_t _binary_cjk_font_data_bin_start[];

/* bin 头（小端）：0..3 magic, 4..5 ver, 6..7 levels, 8..11 n,
 * 12..17 cell[3], 18..23 stride[3], 24.. cp 表 u16[n]，4 对齐后三级位图 */
static uint32_t rd_le32(const uint8_t *p)
{
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) |
           ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static uint32_t glyph_n(void)      { return rd_le32(_binary_cjk_font_data_bin_start + 8); }
static uint16_t glyph_cell(int lvl)  { const uint8_t *p = _binary_cjk_font_data_bin_start + 12 + lvl * 2; return (uint16_t)(p[0] | (p[1] << 8)); }
static uint16_t glyph_stride(int lvl){ const uint8_t *p = _binary_cjk_font_data_bin_start + 18 + lvl * 2; return (uint16_t)(p[0] | (p[1] << 8)); }

static const uint16_t *cp_table(void)
{
    return (const uint16_t *)(_binary_cjk_font_data_bin_start + 24);
}

static const uint8_t *level_base(int lvl)
{
    uint32_t n = glyph_n();
    uint32_t off = 24 + 2 * n;
    off = (off + 3) & ~3u;
    for (int i = 0; i < lvl; i++)
        off += n * (uint32_t)glyph_stride(i) * (uint32_t)glyph_cell(i);
    return _binary_cjk_font_data_bin_start + off;
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
    return cjk_glyph_lookup_level(cp, CJK_FONT_LEVELS - 1);  /* 24px 兼容（待机页） */
}

int cjk_glyph_cell_size(int level)   { return glyph_cell(level); }
int cjk_glyph_stride_size(int level) { return glyph_stride(level); }

"""
c += "const char *const k_chuanxilu_quotes[\(quotes.count)] = {\n"
c += quotes.map { "    \"" + $0.joined(separator: "\\n") + "\"," }.joined(separator: "\n")
c += "\n};\n\n"
c += "/* 引文出处（右下角署名，与引文同字库 24px 级） */\n"
c += "const char k_chuanxilu_attrib[] = \"\(ATTRIB)\";\n"
try! c.write(to: URL(fileURLWithPath: "src/cjk_font.c"), atomically: true, encoding: .utf8)

let h = """
/**
 * @file cjk_font.h
 * @brief 中文点阵字库接口（三级 16/20/24px + 引文表；生成文件勿手改）
 *
 * 字形数据 cjk_font_data.bin（EMBED_FILES 编入固件），码点升序二分查找。
 * 位图行主序 MSB-first，bit=1 着色，可直接 blit 到 epd_gfx_draw_bitmap。
 * level 档位：0=16px / 1=20px / 2=24px；阅读器按级取形并做墨迹盒变宽
 * 渲染（reader_engine），待机页沿用 24px 兼容 API。
 * 由 tools/gen_cjk_font.swift 生成。
 */
#ifndef INKWORD_CJK_FONT_H
#define INKWORD_CJK_FONT_H

#include <stdint.h>

#define CJK_FONT_LEVELS    3                 /**< 字号级数 */
#define CJK_GLYPH_W       24                 /**< 兼容宏：默认级(24px) 字形宽 */
#define CJK_GLYPH_H       24                 /**< 兼容宏：默认级(24px) 字形高 */
#define CJK_GLYPH_STRIDE  3                  /**< 兼容宏：默认级每行字节数 */
#define CJK_GLYPH_N       \(cps.count)                 /**< 字形总数（三级共用码点表） */

/** UTF-32 码点 -> 指定级字形位图；未收录返回 NULL（调用方画占位框） */
const uint8_t *cjk_glyph_lookup_level(uint32_t cp, int level);

/** 兼容 API（待机页）：UTF-32 码点 -> 24px 级字形位图；未收录返回 NULL */
const uint8_t *cjk_glyph_lookup(uint32_t cp);

/** 指定级字形边长（px）：16/20/24；level 越界返回 0 */
int cjk_glyph_cell_size(int level);

/** 指定级每行字节数：2/3/3；level 越界返回 0 */
int cjk_glyph_stride_size(int level);

#define CHUANXILU_QUOTE_N \(quotes.count)                /**< 引文条数（=小时数） */
/** 待机页逐时轮换引文（UTF-8，\\n 分行，每行 <=\(MAX_COLS)字） */
extern const char *const k_chuanxilu_quotes[CHUANXILU_QUOTE_N];

/** 引文出处（右下角署名，UTF-8 单行） */
extern const char k_chuanxilu_attrib[];

#endif /* INKWORD_CJK_FONT_H */
"""
try! h.write(to: URL(fileURLWithPath: "src/cjk_font.h"), atomically: true, encoding: .utf8)

// ---------- 6. stderr 报告 + ASCII 预览 ----------
let err = FileHandle.standardError
func say(_ s: String) { try? err.write(contentsOf: Data((s + "\n").utf8)) }
say("// font=\(fontLabel) glyphs=\(cps.count) (gb2312-1=\(gbCount)) quotes=\(quotes.count)")
for l in levelOuts {
    say("// level cell=\(l.cell) fontSize=\(Int(l.fontSize)) stride=\(l.stride) " +
        "bytes=\(l.bits.count * l.stride * l.cell) edge-touch=\(l.touched)")
}
say("// bin total = \(bin.count) bytes")
let previewSet: [Character: Int] = {
    var m: [Character: Int] = [:]
    for (i, ch) in sortedChars.enumerated() { m[ch] = i }
    return m
}()
let previews: [Character] = ["知", "行", "A", "，"]
for preview in previews {
    guard let idx = previewSet[preview] else { continue }
    let l = levelOuts[LEVELS.count - 1]   // 24px 级预览
    say("// preview '\(preview)' (24px):")
    for gy in 0..<l.cell {
        var row = ""
        for gx in 0..<l.cell {
            let bit = l.bits[idx][gy * l.stride + gx / 8] & UInt8(0x80 >> (gx % 8))
            row += bit != 0 ? "#" : "."
        }
        say("// " + row)
    }
}
