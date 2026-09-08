#!/usr/bin/env python3
# -*- coding: utf-8 -*-
"""中文点阵字库生成器 —— Linux 平行链（开源通用化 Phase 4，2026-10-24）。

双轨纪律：tools/gen_cjk_font.swift = macOS 权威链（CoreText 渲染，
PingFang/Kaiti 字形基线，真机黄金帧锚定）；本脚本 = Linux 平行链
（freetype-py 渲染 + fontTools/fc-match 字体管理），算法、字符集五源、
bin 格式、输出 C/H 模板与 swift 链逐条对齐（对齐 CMakeLists.txt
arduino/CMake 双轨同步先例）。改任一链的格式/算法/模板须同步另一链，
并重跑 tools/font_check.py 对拍。swift 链产物（src/）为发布权威，
py 链覆盖 src/ 需显式 --force。

bin 布局（与 cjk_font.c:21 头一致，小端自描述，消费端动态计算勿写死）：
  [0..3]  "CKF1"          [4..5]  u16 version=1
  [6..7]  u16 levels=4    [8..11] u32 n（字形数）
  [12..19] u16 cell[4]    [20..27] u16 stride[4]
  [28..]   u16 cp[n] 升序（4 对齐后）逐级行主序 MSB-first 位图

字体源（平台自适应，覆盖检测自动跳缺字家族）：
  小字级 16/20px 黑体链：Noto Sans CJK SC → PingFang SC → Heiti SC ...
  大字级 24/32px 楷体链：Noto Serif CJK SC → Kaiti SC → Songti SC ...
  音标字符专属链（IPA/Phonetic，渲染时分派）：DejaVu Sans → Noto Sans
  → Arial Unicode MS（macOS 实测唯一 IPA 21 字符全覆盖，2026-10-24）
  发现顺序：--font-* 显式指定 > fontconfig fc-match > 常见目录扫描。
  Bold：fc-match 'family:bold' 命中真字重 face 则用（macOS PingFang →
  Semibold，与 swift traitBold 同源）；无则 Regular。

与 swift 链的已知差异（font_check.py 结构对拍可容）：
  - 渲染引擎（FreeType vs CoreText）光栅化差异 → 位图非逐字节一致；
  - GBK 解码：swift GBK_95 对未定义槽映射 PUA（0xE810-E81F 噪音字
    收录）；Python gbk codec 报 UnicodeDecodeError，本链显式排除 PUA
    → 字形数差 ~16，设备端永不显示该区段（swift fallbackOK 白名单证）。

用法（项目根或任意 CWD）：
  pip install -r tools/requirements.txt
  python3 tools/gen_cjk_font.py                      # 全量 → src/（覆盖需 --force）
  python3 tools/gen_cjk_font.py --out-dir /tmp/pyf   # 隔离输出（对拍/验证）
  python3 tools/gen_cjk_font.py --subset cs.txt d1   # 子集 → deck_d1.bin（根目录）
"""

import argparse
import json
import os
import struct
import subprocess
import sys

try:
    import freetype
except ImportError:
    sys.stderr.write("freetype-py missing: pip install -r tools/requirements.txt\n")
    sys.exit(1)

# ---------- 0. 常量（与 swift 链逐条对齐，改须双链同步） ----------

LEVELS = [16, 20, 24, 32]            # 像素格边长（level 0/1/2/3；32px 级 LARGE 档大屏）
FONT_SIZE_HINT = {16: 15, 20: 19, 24: 22, 32: 30}
MAX_COLS = 8                         # 引文区每行最多字符数
MAX_LINES = 5                        # 引文区每条最多行数
INK_THRESHOLD = 100                  # 二值化阈值（覆盖率 >=40% 保留，与 swift 共识）
TOUCH_TOLERANCE = 256                # 整级贴边容忍（少量残余走 per-glyph 降级）

ATTRIB = "——王阳明《传习录》"        # 出处串（右下角署名，字符集一并收录）
PUNCT = "，。、；：？！“”‘’（）《》〈〉【】「」『』…—·～‰℃°＋－×÷＝／　"
IPA = "ˈˌəɪɛæʊɑɔʌɡʃʤŋɜʧθðʒːɒ"

SMALL_CHAIN = ["Noto Sans CJK SC", "PingFang SC", "Heiti SC",
               "WenQuanYi Zen Hei", "Source Han Sans SC"]
LARGE_CHAIN = ["Noto Serif CJK SC", "Noto Serif SC", "Kaiti SC",
               "Songti SC", "Noto Sans CJK SC", "STHeiti SC"]
PHON_CHAIN = ["DejaVu Sans", "Noto Sans", "Arial Unicode MS"]

FONT_DIRS = ["/usr/share/fonts", "/usr/local/share/fonts", "/Library/Fonts",
             "/System/Library/Fonts",
             os.path.expanduser("~/.fonts"),
             os.path.expanduser("~/.local/share/fonts")]
FONT_EXTS = (".ttf", ".otf", ".ttc", ".otc")

SYNTH_CPS = (0x02C8, 0x02CC, 0x02D0, 0x00B7)   # ˈ ˌ ː · 几何合成（见 synth_modifier）


def say(msg):
    sys.stderr.write(msg + "\n")


def project_root():
    """项目根定位：脚本上溯找 platformio.ini；失败回退 CWD
    （check_layering.py 同约定）。"""
    try:
        d = os.path.dirname(os.path.abspath(__file__))
        for _ in range(3):
            if os.path.exists(os.path.join(d, "platformio.ini")):
                return d
            d = os.path.dirname(d)
    except NameError:
        pass
    return os.getcwd()


# ---------- 1. 解析引文（全量模式） ----------

def load_quotes(root):
    path = os.path.join(root, "tools", "chuanxilu_quotes.txt")
    try:
        raw = open(path, encoding="utf-8").read()
    except OSError:
        sys.stderr.write("cannot read %s\n" % path)
        sys.exit(1)
    quotes, cur = [], []
    for line in raw.split("\n"):
        if line.startswith("#"):
            continue
        if not line.strip():
            if cur:
                quotes.append(cur)
                cur = []
            continue
        cur.append(line)
    if cur:
        quotes.append(cur)
    # 布局校验（超标即失败，防止生成跑版数据；与 swift precondition 同）
    for qi, q in enumerate(quotes):
        if len(q) > MAX_LINES:
            raise SystemExit("quote #%d: %d lines > %d" % (qi, len(q), MAX_LINES))
        for li, l in enumerate(q):
            if len(l) > MAX_COLS:
                raise SystemExit("quote #%d line %d: %r %d chars > %d"
                                 % (qi, li, l, len(l), MAX_COLS))
    return quotes


# ---------- 2. 收集字符集（五源，与 swift 同源） ----------

def fallback_ok(cp, phon_cps):
    """主链覆盖检测白名单（与 swift fallbackOK 同源）：全角符号系统级
    联补 + PUA 噪音字（设备永不显示）+ IPA 区段（渲染分派专属链）+
    音标列收集集（人名等）。"""
    return (cp in (0x00F7, 0x00B0, 0x2103, 0x2030, 0x00D7)
            or 0xE810 <= cp <= 0xE81F
            or 0x0250 <= cp <= 0x02AF
            or 0x02C0 <= cp <= 0x02DF
            or chr(cp) in phon_cps)


def collect_charset(root, subset_mode, subset_file, phon_cps):
    """返回 charset: set[int(unicode cp)]。全量 = 引文 ∪ 出处 ∪ 标点 ∪
    IPA ∪ ASCII ∪ 音标列 ∪ GB2312 一级；子集 = 文件字符 − 主集已收录。"""
    charset = set()

    if subset_mode:
        try:
            cs = open(subset_file, encoding="utf-8").read()
        except OSError:
            sys.stderr.write("cannot read %s\n" % subset_file)
            sys.exit(1)
        charset = {ord(c) for c in cs if not c.isspace()}
        main_path = os.path.join(root, "src", "cjk_font_data.bin")
        try:
            main_bin = open(main_path, "rb").read()
        except OSError:
            sys.stderr.write("cannot read %s (run full mode first)\n" % main_path)
            sys.exit(1)
        n = struct.unpack_from("<I", main_bin, 8)[0]
        # cp 表起点动态：头自描述（12 + levels*4，2026-09-03 四级化教训）
        main_levels = struct.unpack_from("<H", main_bin, 6)[0]
        main_cp_off = 12 + main_levels * 4
        main_cps = set(struct.unpack_from("<%dH" % n, main_bin, main_cp_off))
        charset -= main_cps
        if not charset:
            say("// subset empty: all chars covered by main font, no deck font needed")
            sys.exit(0)
        return charset, 0

    for q in load_quotes(root):
        for line in q:
            charset.update(ord(c) for c in line)
    charset.update(ord(c) for c in ATTRIB)
    charset.update(ord(c) for c in PUNCT)
    charset.update(ord(c) for c in IPA)
    charset.update(range(0x20, 0x7F))                      # ASCII

    # 词卡音标行全量收集（default_words.json phonetic 列；诗词类该列填
    # 作者名，生僻人名不入 GB 一级库，不收录则真机画空心框）
    try:
        words = json.load(open(os.path.join(root, "src", "default_words.json"),
                               encoding="utf-8"))["words"]
        for w in words:
            ph = w.get("phonetic", "")
            phon_cps.update(c for c in ph if c not in "/[]")
    except (OSError, KeyError, ValueError):
        say("// WARN: default_words.json phonetic collect failed (ignored)")
    charset.update(ord(c) for c in phon_cps)

    # GB2312 一级字库 3755 字（区位 16-55）。Python gbk codec 是 GB2312
    # 超集且未定义槽报错（不映射 PUA，与 swift GBK_95 差异见模块头注释）
    gb_count = 0
    for qu in range(16, 56):
        for wei in range(1, 95):
            try:
                ch = bytes([0xA0 + qu, 0xA0 + wei]).decode("gbk")
            except UnicodeDecodeError:
                continue
            if len(ch) != 1:
                continue
            cp = ord(ch)
            if cp in (0xFFFD, 0):
                continue
            charset.add(cp)
            gb_count += 1
    if gb_count < 3700:
        raise SystemExit("GB2312 level-1 decode suspiciously small: %d" % gb_count)

    # PUA 噪音字防御排除（swift 链收录但设备永不显示；py 链不收，
    # 结构对拍容差见模块头注释）
    charset -= set(range(0xE810, 0xE820))

    for cp in charset:
        if cp >= 0x10000:
            raise SystemExit("non-BMP codepoint U+%04X unsupported" % cp)
    return charset, gb_count


# ---------- 3. 字体发现与管理（fontconfig > 目录扫描） ----------

class FontPick(object):
    """一个角色的字形来源：regular + 可选真字重 Bold face
    （swift traitBold 语义：有 Bold 变体则渲染用之）。"""
    def __init__(self, family, path, index, bold_path=None, bold_index=None):
        self.family = family
        self.path, self.index = path, index
        self.bold_path, self.bold_index = bold_path, bold_index

    @property
    def bold_capable(self):
        return self.bold_path is not None

    def label(self):
        return self.family + (" Bold" if self.bold_capable else "")


def _dec(x):
    return x.decode("utf-8", "replace") if isinstance(x, bytes) else x


def fc_match(family, bold=False):
    """fontconfig 单家族匹配；未命中（fontconfig 家族替换）返回 None。
    bold=True 时要求 style 为真字重（Bold/Semibold/...，非 Regular）。"""
    pat = "%s:bold" % family if bold else family
    try:
        r = subprocess.run(
            ["fc-match", "-f", "%{file}|%{index}|%{family}|%{style}", pat],
            capture_output=True, timeout=10)
    except (OSError, subprocess.TimeoutExpired):
        return None
    parts = _dec(r.stdout).strip().split("|")
    if len(parts) < 4 or not parts[0]:
        return None
    path, idx, fams, styles = parts[0], int(parts[1]), parts[2], parts[3]
    if family not in [f.strip() for f in fams.split(",")]:
        return None
    if bold:
        style_tokens = [t.strip().lower() for t in styles.split(",")]
        if not any("bold" in t or t == "medium" for t in style_tokens):
            return None
    return path, idx


def scan_family(family, bold=False):
    """目录扫描 fallback（无 fc-match 的环境）：fontTools 读 name 表
    匹配家族（ttc 逐 face），返回 (path, index) 或 None。"""
    try:
        from fontTools.ttLib import TTFont, TTCollection
    except ImportError:
        sys.stderr.write("fonttools missing (scan fallback needs it; "
                         "pip install -r tools/requirements.txt)\n")
        sys.exit(1)
    want = (family + " Bold") if bold else family
    for d in FONT_DIRS:
        if not os.path.isdir(d):
            continue
        for dirpath, _, filenames in sorted(os.walk(d)):
            for fn in sorted(filenames):
                if not fn.lower().endswith(FONT_EXTS):
                    continue
                p = os.path.join(dirpath, fn)
                try:
                    fonts = ([TTFont(p, fontNumber=i, lazy=True)
                              for i in range(_ttc_count(p))]
                             if fn.lower().endswith((".ttc", ".otc"))
                             else [TTFont(p, lazy=True)])
                except Exception:
                    continue
                for i, t in enumerate(fonts):
                    try:
                        name = t["name"].getDebugName(4) or ""
                        fam = t["name"].getDebugName(1) or ""
                        sub = t["name"].getDebugName(2) or ""
                    except Exception:
                        continue
                    if fam != family:
                        continue
                    if bold:
                        if "bold" not in sub.lower():
                            continue
                    elif name != want and sub != "Regular":
                        continue
                    # 匹配成功：关闭已打开的 TTFont 对象再返回，避免 fd 泄漏
                    for ft in fonts:
                        try:
                            ft.close()
                        except Exception:
                            pass
                    return p, i
                # 未匹配：关闭本文件所有 face
                for ft in fonts:
                    try:
                        ft.close()
                    except Exception:
                        pass
    return None


def _ttc_count(path):
    with open(path, "rb") as fh:
        tag = fh.read(4)
        if tag != b"ttcf":
            return 1
        fh.read(2)  # skip version
        return struct.unpack(">H", fh.read(2))[0]


def find_family(family, bold=False):
    return fc_match(family, bold) or scan_family(family, bold)


def face_covers(path, index, cps, phon_cps):
    """freetype cmap 覆盖检测（swift CTFontGetGlyphsForCharacters 等价）：
    白名单缺口容忍（级联/分派补），白名单外缺字打印诊断返回 False。"""
    try:
        f = freetype.Face(path, index)
    except freetype.FT_Exception:
        return False
    missing = []
    for cp in cps:
        if f.get_char_index(cp) == 0 and not fallback_ok(cp, phon_cps):
            missing.append(cp)
            if len(missing) > 8:
                break
    if missing:
        say("// font '%s' skipped ... missing: %s"
            % (os.path.basename(path),
               " ".join("U+%04X" % c for c in missing)))
        return False
    return True


def pick_font(chain, cps, phon_cps, cli_override):
    """链上取第一个覆盖字符集的家族（swift pickFont 语义）+ Bold face。"""
    if cli_override:
        path, _, idx = cli_override.rpartition(":")
        if not path:
            path, idx = cli_override, "0"
        if face_covers(path, int(idx), cps, phon_cps):
            return FontPick(os.path.basename(path), path, int(idx))
        raise SystemExit("font %s does not cover charset" % cli_override)
    for family in chain:
        hit = find_family(family)
        if not hit:
            continue
        path, idx = hit
        if face_covers(path, idx, cps, phon_cps):
            bold = find_family(family, bold=True)
            if bold and bold != (path, idx):
                return FontPick(family, path, idx, bold[0], bold[1])
            return FontPick(family, path, idx)
    raise SystemExit("no covering CJK font found for chain: %s" % "/".join(chain))


class FacePool(object):
    """(path, index, size) → set_pixel_sizes 就绪的 Face（线程不安全，
    单进程顺序复用；FreeType Face 有状态，缓存键含 size 防串档）。"""
    def __init__(self):
        self._cache = {}

    def get(self, pick, size, bold=False):
        if bold and pick.bold_path:
            path, idx = pick.bold_path, pick.bold_index
        else:
            path, idx = pick.path, pick.index
        key = (path, idx, size)
        f = self._cache.get(key)
        if f is None:
            f = freetype.Face(path, idx)
            f.set_pixel_sizes(0, size)
            self._cache[key] = f
        return f


# ---------- 4. 渲染核心（两遍法：FreeType 墨迹盒直读） ----------

POOL = FacePool()
SMALL_PICK = LARGE_PICK = PHON_PICK = None
PHON_SET = set()


def _glyph_face(cp, cell, size):
    """音标字符分派专属链（swift phonFont 语义：IPA/Phonetic 字符主链
    常缺，如 PingFang 缺 IPA 全套）；其余按级走黑体/楷体链（有真字重
    Bold face 则渲染用之，swift makeFont traitBold 同源；PHON 分派
    恒 Regular——swift STHeitiSC-Medium 本身即目标字重无变体切换）。
    PHON_SET 是 int 码点集（2026-10-24 首版曾写成 chr(cp) in PHON_SET
    类型不匹配恒 False，分派整体失效致 PingFang 缺的 IPA 字符全零
    位图——font_check all-zero 统计揪出，勿再犯）。"""
    if cp in PHON_SET:
        return POOL.get(PHON_PICK, size)
    pick = SMALL_PICK if cell < 24 else LARGE_PICK
    return POOL.get(pick, size, bold=pick.bold_capable)


def measure_glyph(cp, cell, size):
    """Pass1：FreeType 位图墨迹盒（灰度 < INK_THRESHOLD），返回相对 pen
    （原点=基线上落笔点）的 (l, r, t, b)；t 为基线上高（正）、b 为基线下
    探（负）；空字形返回 None。swift 大画布扫描的等价直读——bitmap_top/
    bitmap_left 即排版盒，免去画布（见模块头双轨差异说明）。"""
    face = _glyph_face(cp, cell, size)
    gi = face.get_char_index(cp)
    if gi == 0:
        return None
    face.load_glyph(gi, freetype.FT_LOAD_RENDER)
    g, b = face.glyph, face.glyph.bitmap
    w, h, pitch = b.width, b.rows, b.pitch
    buf = b.buffer
    minx = miny = 1 << 30
    maxx = maxy = -1
    for y in range(h):
        base = y * pitch
        row = buf[base:base + w]
        # 行级快速过滤（min 为 C 速度），有墨行才逐像素定位首尾
        if not row or min(row) >= INK_THRESHOLD:
            continue
        for x in range(w):
            if row[x] < INK_THRESHOLD:
                if x < minx:
                    minx = x
                if x > maxx:
                    maxx = x
                if y < miny:
                    miny = y
                if y > maxy:
                    maxy = y
    if maxx < 0:
        return None                       # 空字形（空格等全白字符）
    # b 与 swift 同号语义（墨迹最低点相对基线，上正下负；2026-10-24
    # 首版曾写反成 maxy-bitmap_top，居中计算连锁错位致整级贴边）
    return (g.bitmap_left + minx, g.bitmap_left + maxx,
            g.bitmap_top - miny, g.bitmap_top - maxy)


def render_glyph(cp, cell, stride, size, text_x, baseline):
    """Pass2：cell 画布贴图 + 二值化（阈值与 measure 共用），行主序
    MSB-first，bit=1 着色。画布外墨迹裁掉（与 CTLineDraw 画布裁剪一致，
    贴边检测只看画布内像素）。缺字形返回全 0（调用方占位）。"""
    bits = bytearray(stride * cell)
    face = _glyph_face(cp, cell, size)
    gi = face.get_char_index(cp)
    if gi == 0:
        return bytes(bits)
    face.load_glyph(gi, freetype.FT_LOAD_RENDER)
    g, b = face.glyph, face.glyph.bitmap
    base_x = text_x + g.bitmap_left
    for y in range(b.rows):
        dy = baseline - g.bitmap_top + y
        if dy < 0 or dy >= cell:
            continue
        row = b.buffer[y * b.pitch:y * b.pitch + b.width]
        out_base = dy * stride
        for x in range(b.width):
            if row[x] < INK_THRESHOLD:
                dx = base_x + x
                if 0 <= dx < cell:
                    bits[out_base + (dx >> 3)] |= 0x80 >> (dx & 7)
    return bytes(bits)


def edge_touch(bits, cell, stride):
    """位图四边贴边像素数（整级/单字形共用；验收硬指标四边=0 的度量）。"""
    t = 0
    for gx in (0, cell - 1):
        for gy in range(cell):
            if bits[gy * stride + (gx >> 3)] & (0x80 >> (gx & 7)):
                t += 1
    for gy in (0, cell - 1):
        row = gy * stride
        for gx in range(cell):
            if bits[row + (gx >> 3)] & (0x80 >> (gx & 7)):
                t += 1
    return t


def synth_modifier(cp, cell, stride):
    """IPA 重音/长音/间隔号几何合成（swift synthModifier 逐式对齐）：
    字体渲染的 ˈ ˌ 细竖笔低于二值化阈值被整体丢弃（真机 /səˈsaɪəti/
    显为 sə saɪəti 报障），四记号按级合成；笔画宽 cell/8 取整与主链
    Bold 笔宽同源；合成图严格避开四边不引入贴边。非四记号返回 None。"""
    if cp not in SYNTH_CPS:
        return None
    bits = bytearray(stride * cell)
    pen = max(2, (cell + 7) // 8)
    d = max(2, (cell + 7) // 8)

    def fill(x0, y0, w, h):
        for y in range(y0, y0 + h):
            for x in range(x0, x0 + w):
                bits[y * stride + (x >> 3)] |= 0x80 >> (x & 7)

    if cp == 0x02C8:                     # ˈ 主重音：顶部竖笔
        fill((cell - pen) // 2, 1, pen, max(4, cell // 3))
    elif cp == 0x02CC:                   # ˌ 次重音：底部竖笔
        h = max(3, cell // 5)
        fill((cell - pen) // 2, cell - 2 - h, pen, h)
    elif cp == 0x02D0:                   # ː 长音符：中部双点
        fill((cell - d) // 2, cell * 2 // 5, d, d)
        fill((cell - d) // 2, cell * 3 // 5, d, d)
    else:                                # · 间隔号：中心方点
        fill((cell - d) // 2, (cell - d) // 2, d, d)
    return bytes(bits)


def render_glyph_fit(cp, cell, stride, base_size):
    """单字形自适应降级：从 base_size 起独立两遍法居中渲染、贴边则缩
    1pt 直至装下（字号不同基线不同，不能用整级基线；cell 制点阵按格
    对齐，降级字形仅比同级略小、无基线错乱）。"""
    size = base_size
    last = bytes(stride * cell)
    while size >= 8:
        m = measure_glyph(cp, cell, size)
        if m is None:                    # 空字形无墨不贴边
            break
        l, r, t, b = m
        baseline, text_x = center_placement(l, r, t, b, cell)
        bits = render_glyph(cp, cell, stride, size, text_x, baseline)
        if edge_touch(bits, cell, stride) == 0:
            return bits
        last = bits
        size -= 1
    return last                          # 8pt 仍贴边（理论不至）：接受裁切


def center_placement(l, r, t, b, cell):
    """墨迹盒居中定位（视觉行坐标，y 向下；返回 (baseline, text_x)）。
    墨迹像素跨度含端点：高 A-B+1 行、宽 R-L+1 列；起点取整后字形
    严格落在 [0, cell) 内。勿照抄 swift 公式 (cell-B-A)/2 ——那是 CG
    坐标（y 向上）且含 0.5px 上偏，直接沿用会把字形抬出画布顶部
    （2026-10-24 首版事故：16px 级整级渲染只剩字形底部 3 行，闭环
    连锁缩到 8pt 才收敛，真因是坐标语义未转换非字号问题）。"""
    a, bb = t, b                        # A=基线上高，B=基线下探（负）
    top_row = (cell - (a - bb + 1)) // 2
    baseline = top_row + a
    left_col = (cell - (r - l + 1)) // 2
    text_x = left_col - l
    return baseline, text_x


# ---------- 5. 逐级渲染主循环（两遍法闭环 + 降级 + 合成替换） ----------

class LevelOut(object):
    def __init__(self, cell, stride):
        self.cell, self.stride = cell, stride
        self.font_size = 0
        self.bits = []
        self.touched = 0
        self.demoted = 0                 # per-glyph 降级字数


def render_level(cell, sorted_cps):
    stride = (cell + 7) // 8
    out = LevelOut(cell, stride)
    font_size = FONT_SIZE_HINT.get(cell, cell - 2)

    while True:
        # Pass1：全字符墨迹盒极值（A 基线上/B 下探/L 左/R 右；初值 0
        # 与 swift 同源——B/L 取 min(0, 极值)，全库无下探时不顶布局）
        A = B = L = R = 0
        for cp in sorted_cps:
            m = measure_glyph(cp, cell, font_size)
            if m is None:
                continue
            l, r, t, b = m
            if t > A:
                A = t
            if b < B:
                B = b
            if l < L:
                L = l
            if r > R:
                R = r
        if A - B <= cell - 2 and R - L <= cell - 2:
            # Pass2：格内居中（墨迹盒含端点跨度，见 center_placement）
            baseline, text_x = center_placement(L, R, A, B, cell)
            out.font_size = font_size
            out.bits = [render_glyph(cp, cell, stride, font_size, text_x, baseline)
                        for cp in sorted_cps]
            out.touched = sum(edge_touch(b, cell, stride) for b in out.bits)
            if out.touched <= TOUCH_TOLERANCE:    # 少量贴边走 per-glyph 降级
                break
            say("// level cell=%d fontSize=%d edge-touch=%d -> shrink & rerender"
                % (cell, font_size, out.touched))
            out.bits = []
        font_size -= 1
        if font_size < 8:
            raise SystemExit("glyph ink box cannot fit even at 8pt (cell=%d)" % cell)

    # per-glyph 降级：残余贴边字形单独缩 1pt 独立居中重渲（保整级字面）
    for i, cp in enumerate(sorted_cps):
        if edge_touch(out.bits[i], cell, stride) > 0:
            out.bits[i] = render_glyph_fit(cp, cell, stride, font_size - 1)
            out.demoted += 1
    if out.demoted:
        say("// level cell=%d demoted %d glyphs to %dpt (extreme ink box)"
            % (cell, out.demoted, font_size - 1))

    # IPA 记号合成替换：在降级后、进库前覆盖（合成图避开四边不贴边）
    for i, cp in enumerate(sorted_cps):
        s = synth_modifier(cp, cell, stride)
        if s is not None:
            out.bits[i] = s
    return out


# ---------- 6. 打包 bin ----------

def pack_bin(cps, level_outs):
    bin_ = bytearray()
    bin_ += struct.pack("<4sHHI", b"CKF1", 1, len(level_outs), len(cps))
    bin_ += struct.pack("<%dH" % len(level_outs), *[l.cell for l in level_outs])
    bin_ += struct.pack("<%dH" % len(level_outs), *[l.stride for l in level_outs])
    bin_ += struct.pack("<%dH" % len(cps), *cps)
    while len(bin_) % 4:
        bin_ += b"\0"
    for l in level_outs:
        for b in l.bits:
            bin_ += b
    return bytes(bin_)


# ---------- 7. C/H 输出模板（与 swift 链模板逐行对齐，改须双链同步） ----------

GEN_NOTE = ("由 tools/gen_cjk_font.py（Linux 平行链）或 "
            "tools/gen_cjk_font.swift（macOS 权威链）生成")

C_TEMPLATE = """/**
 * @file cjk_font.c
 * @brief 中文点阵字库 lookup（生成文件，勿手改）
 *
 * 字形数据在 cjk_font_data.bin（CMake EMBED_FILES 编入固件）：
 *   %(fonts_desc)s，四级 %(level_cells)spx，
 *   %(n)d 字形 x %(level_desc)s
 *   = %(total_bytes)d 字节。
 * 码点升序二分查找；位图行主序 MSB-first，bit=1 着色（epd_gfx_draw_bitmap 格式）。
 * %(gen_note)s。
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

H_TEMPLATE = """/**
 * @file cjk_font.h
 * @brief 中文点阵字库接口（四级 16/20/24/32px；生成文件勿手改）
 *
 * 字形数据 cjk_font_data.bin（EMBED_FILES 编入固件），码点升序二分查找。
 * 位图行主序 MSB-first，bit=1 着色，可直接 blit 到 epd_gfx_draw_bitmap。
 * level 档位：0=16px / 1=20px / 2=24px / 3=32px（32px 级 LARGE 档大屏，
 * 2026-09-03）；阅读器按级取形并做墨迹盒变宽渲染（reader_engine）。
 * 引文表已拆出至 quotes_app.h（App 层，开源通用化 Phase 2 2026-10-24）。
 * %(gen_note)s。
 */
#ifndef INKWORD_CJK_FONT_H
#define INKWORD_CJK_FONT_H

#include <stdint.h>

#define CJK_FONT_LEVELS    %(levels)d                 /**< 字号级数 */
#define CJK_GLYPH_W       %(max_cell)d                 /**< 兼容宏：最大级字形宽（零消费方，随级数自适） */
#define CJK_GLYPH_H       %(max_cell)d                 /**< 兼容宏：最大级字形高 */
#define CJK_GLYPH_STRIDE  %(max_stride)d       /**< 兼容宏：最大级每行字节数 */
#define CJK_GLYPH_N       %(n)d                 /**< 字形总数（各级共用码点表） */

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

QUOTES_C_TEMPLATE = """/**
 * @file quotes_app.c
 * @brief 《传习录》引文表数据（生成文件，勿手改）
 *
 * 开源通用化 Phase 2（2026-10-24）：自 cjk_font.c 迁出（数据搬家非
 * 渲染变化）。%(gen_note)s；改引文编辑
 * tools/chuanxilu_quotes.txt 后重跑生成器。
 */
#include "quotes_app.h"

const char *const k_chuanxilu_quotes[%(quote_n)d] = {
%(quote_rows)s
};

/* 引文出处（右下角署名，与引文同字库 24px 级） */
const char k_chuanxilu_attrib[] = "%(attrib)s";
"""

QUOTES_H_TEMPLATE = """/**
 * @file quotes_app.h
 * @brief 待机页《传习录》引文表接口（App 层内容；生成文件勿手改）
 *
 * 开源通用化 Phase 2（2026-10-24）：App 内容自 Core 渲染层（cjk_font.h/c）
 * 迁出——字库（Core）只管字形渲染，学科内容归 App。引文字符仍收录进
 * 字库 bin（生成器字符集输入不变）。
 * %(gen_note)s。
 */
#ifndef INKWORD_QUOTES_APP_H
#define INKWORD_QUOTES_APP_H

#define CHUANXILU_QUOTE_N %(quote_n)d                /**< 引文条数（=小时数） */
/** 待机页逐时轮换引文（UTF-8，\\n 分行，每行 <=%(max_cols)d字） */
extern const char *const k_chuanxilu_quotes[CHUANXILU_QUOTE_N];

/** 引文出处（右下角署名，UTF-8 单行） */
extern const char k_chuanxilu_attrib[];

#endif /* INKWORD_QUOTES_APP_H */
"""


# ---------- 8. main ----------

def main():
    global SMALL_PICK, LARGE_PICK, PHON_PICK, PHON_SET

    ap = argparse.ArgumentParser(description="CJK bitmap font generator "
                                             "(Linux parallel chain)")
    ap.add_argument("--subset", nargs=2, metavar=("CHARSET.TXT", "DECK_ID"),
                    help="subset mode: render only chars not in main bin")
    ap.add_argument("--font-small", help="override small-chain font path[:index]")
    ap.add_argument("--font-large", help="override large-chain font path[:index]")
    ap.add_argument("--font-ipa", help="override IPA-chain font path[:index]")
    ap.add_argument("--out-dir", default=None,
                    help="output dir (default: <root>/src)")
    ap.add_argument("--force", action="store_true",
                    help="allow overwriting existing generated files")
    args = ap.parse_args()

    root = project_root()
    if not os.path.isdir(os.path.join(root, "src")):
        sys.stderr.write("src/ not found under %s (run from project root)\n" % root)
        return 1

    subset_mode = args.subset is not None
    phon_cps = set()
    charset, gb_count = collect_charset(root, subset_mode,
                                        args.subset[0] if subset_mode else None,
                                        phon_cps)
    sorted_cps = sorted(charset)
    n = len(sorted_cps)
    say("// charset: %d codepoints" % n)

    # 音标分派集：IPA 常量 + 音标列收集的 Latin/希腊扩展（< 0x3000）；
    # 人名汉字主链覆盖不分派（swift 同语义）
    PHON_SET = set(ord(c) for c in IPA)
    PHON_SET.update(ord(c) for c in phon_cps
                    if 0x7F < ord(c) < 0x3000)

    SMALL_PICK = pick_font(SMALL_CHAIN, charset, phon_cps, args.font_small)
    LARGE_PICK = pick_font(LARGE_CHAIN, charset, phon_cps, args.font_large)
    PHON_PICK = pick_font(PHON_CHAIN, PHON_SET or {ord("ə")}, phon_cps,
                          args.font_ipa)
    fonts_desc = "16/20px %s + 24/32px %s" % (SMALL_PICK.label(),
                                              LARGE_PICK.label())

    level_outs = []
    for cell in LEVELS:
        say("// rendering level cell=%d ..." % cell)
        level_outs.append(render_level(cell, sorted_cps))

    bin_data = pack_bin(sorted_cps, level_outs)

    if subset_mode:
        deck_id = args.subset[1]
        out_path = os.path.join(root, "deck_%s.bin" % deck_id)
        if os.path.exists(out_path) and not args.force:
            sys.stderr.write("%s exists (use --force)\n" % out_path)
            return 1
        with open(out_path, "wb") as fh:
            fh.write(bin_data)
        say("// subset deck=%s glyphs=%d bin=%dB -> %s"
            % (deck_id, n, len(bin_data), out_path))
        say("// copy to SD: /fonts/deck_%s.bin (cjk_font_sd 级联加载)" % deck_id)
        report(fonts_desc, sorted_cps, level_outs, bin_data, gb_count)
        return 0                        # 子集不生成 c/h 与引文表（主集不变）

    out_dir = args.out_dir or os.path.join(root, "src")
    outputs = write_outputs(root, out_dir, sorted_cps, level_outs,
                            fonts_desc, bin_data, args.force)
    for p in outputs:
        say("// wrote %s" % p)
    report(fonts_desc, sorted_cps, level_outs, bin_data, gb_count)
    return 0


def write_outputs(root, out_dir, sorted_cps, level_outs, fonts_desc,
                  bin_data, force):
    """写五个产物（bin/c/h/quotes c/h）；已存在且无 --force 拒绝
    （swift 链产物为 src/ 发布权威，py 覆盖须显式授权）。"""
    quotes = load_quotes(root)
    n = len(sorted_cps)
    level_desc = " + ".join("%dpx=%dB" % (l.cell, l.stride * l.cell)
                            for l in level_outs)
    c_text = C_TEMPLATE % {
        "fonts_desc": fonts_desc,
        "level_cells": "/".join(str(x) for x in LEVELS),
        "n": n,
        "level_desc": level_desc,
        "total_bytes": n * sum(l.stride * l.cell for l in level_outs),
        "gen_note": GEN_NOTE,
    }
    h_text = H_TEMPLATE % {
        "levels": len(LEVELS),
        "max_cell": LEVELS[-1],
        "max_stride": (LEVELS[-1] + 7) // 8,
        "n": n,
        "gen_note": GEN_NOTE,
    }
    quote_rows = "\n".join('    "%s",' % "\\n".join(q) for q in quotes)
    qc_text = QUOTES_C_TEMPLATE % {
        "quote_n": len(quotes),
        "quote_rows": quote_rows,
        "attrib": ATTRIB,
        "gen_note": GEN_NOTE,
    }
    qh_text = QUOTES_H_TEMPLATE % {
        "quote_n": len(quotes),
        "max_cols": MAX_COLS,
        "gen_note": GEN_NOTE,
    }

    files = {
        "cjk_font_data.bin": bin_data,
        "cjk_font.c": c_text.encode("utf-8"),
        "cjk_font.h": h_text.encode("utf-8"),
        "quotes_app.c": qc_text.encode("utf-8"),
        "quotes_app.h": qh_text.encode("utf-8"),
    }
    os.makedirs(out_dir, exist_ok=True)
    existing = [fn for fn in files if os.path.exists(os.path.join(out_dir, fn))]
    if existing and not force:
        sys.stderr.write("refusing to overwrite (swift chain output is "
                         "authoritative): %s\nuse --force or --out-dir\n"
                         % ", ".join(existing))
        sys.exit(1)
    for fn, data in files.items():
        with open(os.path.join(out_dir, fn), "wb") as fh:
            fh.write(data)
    return [os.path.join(out_dir, fn) for fn in files]


def report(fonts_desc, sorted_cps, level_outs, bin_data, gb_count):
    """stderr 报告 + ASCII 预览（swift 链报告项对齐；gb_count 仅全量
    模式有意义，子集传 0）。"""
    say("// fonts=%s glyphs=%d (gb2312-1=%d) bin=%dB"
        % (fonts_desc, len(sorted_cps), gb_count, len(bin_data)))
    for l in level_outs:
        say("// level cell=%d fontSize=%d stride=%d bytes=%d edge-touch=%d demoted=%d"
            % (l.cell, l.font_size, l.stride,
               len(l.bits) * l.stride * l.cell, l.touched, l.demoted))
    preview_chars = ["知", "行", "A", "，", "ə", "ˈ"]
    top = level_outs[-1]                 # 最大级预览
    cp_set = set(sorted_cps)
    for ch in preview_chars:
        cp = ord(ch)
        if cp not in cp_set:
            continue
        idx = sorted_cps.index(cp)
        say("// preview %r (%dpx):" % (ch, top.cell))
        bits = top.bits[idx]
        for gy in range(top.cell):
            row = "".join("#" if bits[gy * top.stride + (gx >> 3)]
                          & (0x80 >> (gx & 7)) else "."
                          for gx in range(top.cell))
            say("// " + row)


if __name__ == "__main__":
    sys.exit(main())
