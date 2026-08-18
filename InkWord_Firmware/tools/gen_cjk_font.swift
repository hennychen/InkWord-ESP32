// gen_cjk_font.swift —— 待机页《传习录》引文显示的中文子集字库生成器
//
// 输入：tools/chuanxilu_quotes.txt（引文唯一事实源，空行分条，# 注释）
// 输出：src/cjk_font.c / src/cjk_font.h（码点表 + 24x24 1bpp 字形 + 引文/出处）
//
// 渲染：macOS CoreText（优先楷体 Kaiti SC，加粗：Bold 变体或 3x3 膨胀），
//       24x24 像素格、字号 23、
//       灰度抗锯齿后阈值化（<128 为墨），行主序 MSB-first，
//       与 epd_gfx_draw_bitmap 位图格式一致（bit=1 着色）。
//
// 用法：cd InkWord_Firmware && swift tools/gen_cjk_font.swift
//       （改引文/字号后重新运行即可，勿手改生成文件）

import Foundation
import CoreText
import CoreGraphics

let CELL = 24          // 像素格边长（= CJK_GLYPH_W/H；8 字/行 x 24 = 192px
                      //  恰填满引文带宽，2026-08-18 用户要求加大自 20px 升级）
let FONT_SIZE: CGFloat = 22  // 23pt 时墨迹盒+Bold 外扩 ≈满格，底部笔画（句号/
                      //  封口横）被格底裁切（真机实测"文字底部显示不全"），
                      //  缩 1pt 留上下裕量（2026-08-18）
let STRIDE = (CELL + 7) / 8   // 3 字节/行
let GLYPH_BYTES = STRIDE * CELL
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

// ---------- 2. 收集字符集（引文 + 出处） ----------
/* 出处串（右下角署名，与引文同字库渲染；字符集必须一并收录） */
let ATTRIB = "——王阳明《传习录》"
var charset = Set<Character>()
for q in quotes { for line in q { for ch in line { charset.insert(ch) } } }
for ch in ATTRIB { charset.insert(ch) }
let cps = charset.map { $0.unicodeScalars.first!.value }.sorted()
for cp in cps {
    precondition(cp < 0x10000, "non-BMP codepoint U+\(String(cp, radix: 16)) unsupported")
}

// ---------- 3. 选字体家族（覆盖全部字符的第一个）+ Bold 能力 ----------
let fontNames = ["Kaiti SC", "Kaiti TC", "Songti SC", "STHeiti SC", "Hiragino Sans GB"]
var fontFamily = ""
for name in fontNames {
    guard let f = CTFontCreateWithName(name as CFString, FONT_SIZE, nil) as CTFont? else { continue }
    let u16 = Array(String(charset).utf16)
    var glyphs = [CGGlyph](repeating: 0, count: u16.count)
    if u16.isEmpty || CTFontGetGlyphsForCharacters(f, u16, &glyphs, u16.count) {
        fontFamily = name; break
    }
}
precondition(!fontFamily.isEmpty, "no covering CJK font found")

/* 加粗（2026-08-18 用户要求）：优先真 Bold 变体（字体设计级笔画，钩捺有
 * 笔锋）；无 Bold 家族则渲染后 3x3 膨胀 1px 机械加粗 */
let BOLD = true
var hasBold = false
if BOLD,
   let probe = CTFontCreateWithName(fontFamily as CFString, FONT_SIZE, nil) as CTFont?,
   CTFontCreateCopyWithSymbolicTraits(
       probe, FONT_SIZE, nil,
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

// ---------- 3.5 两遍法定字号/基线：实测墨迹盒，不再猜 em 字面框 ----------
/* em 字面框是猜测量：Kaiti Bold 钩捺下探可超 0.15em，阈值化与膨胀的
 * 外扩也不可预知（字面框基线时 61/163 字贴底行，部分笔画尖仍被画布
 * 边界裁掉，即真机"文字底部显示不全"）。改为 Pass1 大画布逐字实测
 * 全部字符相对 pen 的墨迹极值 A(基线上)/B(基线下探，正)/L/R，
 * Pass2 据此在格内居中定基线与 pen x；墨迹盒装不下 CELL-2 时
 * 自动缩 1pt 重测（保证上下各留 ≥1px，四边 edge-touch 恒为 0） */
let sortedChars = charset.sorted(by: { $0.unicodeScalars.first!.value < $1.unicodeScalars.first!.value })
let BIGPAD = 16
let BIG = CELL + BIGPAD * 2
var fontSize = FONT_SIZE
var baseline: CGFloat = 0
var textX: CGFloat = 0
var inkA: CGFloat = 0, inkB: CGFloat = 0

func measureGlyph(_ ch: Character, _ f: CTFont) -> (l: CGFloat, r: CGFloat, t: CGFloat, b: CGFloat)? {
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
    if maxx < 0 { return nil }   // 空字形（全白）
    /* 内存第 0 行 = 视觉顶部 = CG y 最大（翻转关系，勿把内存行当 CG y）；
 * x 无翻转。t = 墨迹最高 CG y - penY ≥ 0（基线上方），
 * b = 墨迹最低 CG y - penY ≤ 0（下探） */
    return (CGFloat(minx) - penX, CGFloat(maxx) - penX,
            CGFloat(BIG - 1 - miny) - penY, CGFloat(BIG - 1 - maxy) - penY)
}

while true {
    let f = makeFont(fontSize)
    var A: CGFloat = 0, B: CGFloat = 0, L: CGFloat = 0, R: CGFloat = 0
    for ch in sortedChars {
        guard let m = measureGlyph(ch, f) else { continue }
        if m.t > A { A = m.t }
        if m.b < B { B = m.b }   // B 取最负 = 下探最深
        if m.l < L { L = m.l }
        if m.r > R { R = m.r }
    }
    if A - B <= CGFloat(CELL) - 2 && R - L <= CGFloat(CELL) - 2 {
        inkA = A; inkB = -B
        baseline = (CGFloat(CELL) + (-B) - A) / 2   // 格内 CG 基线（距格底）
        textX = (CGFloat(CELL) - L - R) / 2
        break
    }
    fontSize -= 1
    precondition(fontSize >= 10, "glyph ink box cannot fit even at 10pt")
}
var font = makeFont(fontSize)

// ---------- 4. 渲染每个字符 ----------
func renderGlyph(_ ch: Character) -> [UInt8] {
    var buf = [UInt8](repeating: 255, count: CELL * CELL)   // 灰度，白底
    let ctx = CGContext(data: &buf, width: CELL, height: CELL,
                        bitsPerComponent: 8, bytesPerRow: CELL,
                        space: CGColorSpaceCreateDeviceGray(),
                        bitmapInfo: CGImageAlphaInfo.none.rawValue)!
    let attr = NSAttributedString(string: String(ch), attributes: [
        NSAttributedString.Key(kCTFontAttributeName as String): font as Any
    ])
    let line = CTLineCreateWithAttributedString(attr)
    ctx.textPosition = CGPoint(x: textX, y: baseline)
    CTLineDraw(line, ctx)

    var bits = [UInt8](repeating: 0, count: GLYPH_BYTES)
    for gy in 0..<CELL {
        for gx in 0..<CELL {
            /* CGBitmapContext 内存第 0 行即视觉顶部（CG y 轴向上、内存自顶向下），
             * 图像行 gy 直接读内存行 gy，不要再翻转（双重翻转会上下颠倒） */
            let gray = buf[gy * CELL + gx]
            if gray < 128 {
                bits[gy * STRIDE + gx / 8] |= UInt8(0x80 >> (gx % 8))
            }
        }
    }

    /* 无 Bold 变体时的机械加粗：3x3 膨胀（每个墨像素向八邻域扩散 1px） */
    if useDilate {
        var out = bits
        func bit(_ y: Int, _ x: Int) -> Bool {
            bits[y * STRIDE + x / 8] & UInt8(0x80 >> (x % 8)) != 0
        }
        for gy in 0..<CELL {
            for gx in 0..<CELL where bit(gy, gx) {
                for dy in -1...1 {
                    for dx in -1...1 {
                        let y = gy + dy, x = gx + dx
                        if y >= 0 && y < CELL && x >= 0 && x < CELL {
                            out[y * STRIDE + x / 8] |= UInt8(0x80 >> (x % 8))
                        }
                    }
                }
            }
        }
        bits = out
    }
    return bits
}

var glyphBits: [[UInt8]] = []
var touched = 0
for ch in charset.sorted(by: { $0.unicodeScalars.first!.value < $1.unicodeScalars.first!.value }) {
    let b = renderGlyph(ch)
    glyphBits.append(b)
    // 边界触碰检测（四边全查；上/下行检测是 CELL=24 输出截断 bug 后补上，
    // 触边 >0 说明字号/基线需调整——此前只查左右列，上下贴边是假阴性）
    for gx in [0, CELL - 1] { for gy in 0..<CELL {
        if b[gy * STRIDE + gx / 8] & UInt8(0x80 >> (gx % 8)) != 0 { touched += 1 } } }
    for gy in [0, CELL - 1] { for gx in 0..<CELL {
        if b[gy * STRIDE + gx / 8] & UInt8(0x80 >> (gx % 8)) != 0 { touched += 1 } } }
}

// ---------- 5. 输出 C ----------
func hexRow(_ bytes: ArraySlice<UInt8>) -> String {
    bytes.map { String(format: "0x%02X", $0) }.joined(separator: ",")
}

var c = ""
c += """
/**
 * @file cjk_font.c
 * @brief 中文子集点阵字库 + 《传习录》引文表（生成文件，勿手改）
 *
 * 由 tools/gen_cjk_font.swift 从 tools/chuanxilu_quotes.txt 生成：
 *   字体 \(fontLabel) \(Int(fontSize))pt（两遍法实测墨迹盒自适应）
 *     -> \(CELL)x\(CELL) 1bpp（灰度阈值 128）
 *   字形 \(cps.count) 个 x \(GLYPH_BYTES) 字节 = \(cps.count * GLYPH_BYTES) 字节；码点升序，二分查找
 *   引文 \(quotes.count) 条（待机页按小时轮换，tm_hour 直接作下标）
 * 位图格式与 epd_gfx_draw_bitmap 一致：行主序 MSB-first，bit=1 着色。
 * 修改引文/字号：编辑 txt 后重跑 swift tools/gen_cjk_font.swift。
 */
#include "cjk_font.h"
#include <stddef.h>

"""
c += "static const uint16_t k_cp[\(cps.count)] = {\n"
var cpLines: [String] = []
for i in stride(from: 0, to: cps.count, by: 12) {
    cpLines.append("    " + cps[i..<min(i+12, cps.count)].map { String(format: "0x%04X", $0) }.joined(separator: ", ") + ",")
}
c += cpLines.joined(separator: "\n") + "\n};\n\n"

c += "static const uint8_t k_bits[\(cps.count)][\(GLYPH_BYTES)] = {\n"
for (i, b) in glyphBits.enumerated() {
    var rows: [String] = []
    for r in 0..<CELL {
        let s = r * STRIDE
        rows.append(hexRow(b[s..<s+STRIDE]))
    }
    c += "    { /* U+\(String(format: "%04X", cps[i])) \(Character(UnicodeScalar(cps[i])!)) */\n"
    /* 输出全部 CELL 行（按 4 行/组折叠）。⚠ CELL=20 时代遗留的 0..<5
     * 在 CELL=24 时只输出 20 行，尾部由 C 零初始化补成空白 —— 每字底部
     * 4 行笔画丢失，即 2026-08-18 真机"文字底部显示不全"的根因 */
    for j in 0..<(CELL + 3) / 4 { c += "        " + rows[j*4..<min((j+1)*4, CELL)].joined(separator: ",") + ",\n" }
    c += "    },\n"
}
c += "};\n\n"

c += """
const uint8_t *cjk_glyph_lookup(uint32_t cp)
{
    int lo = 0, hi = (int)CJK_GLYPH_N - 1;
    while (lo <= hi) {
        int mid = (lo + hi) / 2;
        if (k_cp[mid] == (uint16_t)cp) return k_bits[mid];
        if (k_cp[mid] < cp) lo = mid + 1; else hi = mid - 1;
    }
    return NULL;
}

"""
c += "const char *const k_chuanxilu_quotes[\(quotes.count)] = {\n"
for q in quotes {
    let s = q.joined(separator: "\\n")
    c += "    \"\(s)\",\n"
}
c += "};\n\n"

c += "/* 引文出处（右下角署名，与引文同字库） */\n"
c += "const char k_chuanxilu_attrib[] = \"\(ATTRIB)\";\n"

let h = """
/**
 * @file cjk_font.h
 * @brief 中文子集点阵字库接口（《传习录》引文显示，生成文件勿手改）
 *
 * 字形 \(CELL)x\(CELL) 1bpp，行主序 MSB-first，bit=1 着色，
 * 可直接逐字 blit 到 epd_gfx_draw_bitmap。
 * 由 tools/gen_cjk_font.swift 生成；改引文编辑 tools/chuanxilu_quotes.txt 后重跑。
 */
#ifndef INKWORD_CJK_FONT_H
#define INKWORD_CJK_FONT_H

#include <stdint.h>

#define CJK_GLYPH_W       \(CELL)
#define CJK_GLYPH_H       \(CELL)
#define CJK_GLYPH_STRIDE  \(STRIDE)               /**< 每行字节数 */
#define CJK_GLYPH_N       \(cps.count)                 /**< 字形总数 */

/** UTF-32 码点 -> 字形位图（60 字节）；未收录返回 NULL */
const uint8_t *cjk_glyph_lookup(uint32_t cp);

#define CHUANXILU_QUOTE_N \(quotes.count)                /**< 引文条数（=小时数） */
/** 待机页逐时轮换引文（UTF-8，\\n 分行，每行 <=\(MAX_COLS)字） */
extern const char *const k_chuanxilu_quotes[CHUANXILU_QUOTE_N];

/** 引文出处（右下角署名，UTF-8 单行） */
extern const char k_chuanxilu_attrib[];

#endif /* INKWORD_CJK_FONT_H */
"""

try! c.write(to: URL(fileURLWithPath: "src/cjk_font.c"), atomically: true, encoding: .utf8)
try! h.write(to: URL(fileURLWithPath: "src/cjk_font.h"), atomically: true, encoding: .utf8)

// ---------- 6. stderr 报告 + ASCII 预览 ----------
let err = FileHandle.standardError
func say(_ s: String) { try? err.write(contentsOf: Data((s + "\n").utf8)) }
say("// font=\(fontLabel) size=\(Int(fontSize)) glyphs=\(cps.count) bytes=\(cps.count * GLYPH_BYTES)")
say("// inkbox: above=\(inkA) below=\(inkB) baseline=\(baseline) penX=\(textX)")
say("// quotes=\(quotes.count) edge-touch-pixels=\(touched)")
for preview in ["知", "行", "。", "花"] where charset.contains(Character(preview)) {
    let idx = cps.firstIndex(of: String(preview).unicodeScalars.first!.value)!
    say("// preview '\(preview)':")
    for gy in 0..<CELL {
        var row = ""
        for gx in 0..<CELL {
            let bit = glyphBits[idx][gy * STRIDE + gx / 8] & UInt8(0x80 >> (gx % 8))
            row += bit != 0 ? "#" : "."
        }
        say("// " + row)
    }
}
