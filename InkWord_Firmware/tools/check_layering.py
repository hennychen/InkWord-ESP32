#!/usr/bin/env python3
"""Core/App 分层守卫（开源通用化 Phase 2，2026-10-24）。

规则：CORE 文件不得 #include APP 头（依赖方向 App -> Core 单向）。
扫描 src/**/*.{c,cpp,h} 的本地 #include 图，违规先 warn 不阻塞构建
（接入 platformio.ini [env:inkword-s3] extra_scripts pre 阶段）；
独立运行：python3 tools/check_layering.py [--strict]（--strict 违规时
exit 1，供 CI 升级用）。

白名单 tools/layering_whitelist.txt 承载已知例外，每行一条：
    <违规文件名>: <被 include 的头名>
'#' 开头为注释行。

装配层（main.cpp / toolchain_stubs.c）豁免；stubs/（离线档桩，App API
替身）、probe/（探针）不扫描。分层清单变更须同步本文件 CORE 集合。
"""

import os
import re
import sys

# ---- 分层清单（与 README「分层规则」节同源，Phase 2 定稿版） ----

CORE = {
    # 显示栈
    "epd_driver", "epd_panel", "epd_geom", "epd_bus",
    "GxEPD2_374_DEPG0370", "GxEPD2_gdeq031t10",
    # 页面路由 / 布局
    "page_router", "layout_profile",
    # 通用文本渲染
    "cjk_text", "cjk_font", "cjk_font_sd",
    # 输入 / 电源 / 存储 / 总线 / 日志
    "button_handler", "power_manager", "storage_manager",
    "i2c_bus", "refresh_scheduler", "haptic", "debug_log",
    # 学科无关算法
    "srs_engine", "learning_state",
    # 纯头 / 配置
    "gpio_config", "inkword_features",
    # panels/ 目录全部（新屏 desc，Core 显示栈成员）
    "panel_depg0370_uc8253", "panel_e042a13_ssd1619", "panel_e042a13bw",
    "panel_wf0270_ssd1680", "panel_gdew027c44_il91874", "panel_wft0290",
    "panel_opm021eb", "panel_hink_e0213a31", "panel_gdeq031t10_uc8253",
    "panel_gdeq0426t82_ssd1677", "uc8253_ops",
}

# 装配层豁免：装配一切（含 App 头）是其职责
ASSEMBLY = {"main", "toolchain_stubs"}

SKIP_DIRS = {"stubs", "probe", "mp3", "Fonts"}   # 桩/探针/资源目录不扫描
EXTS = (".c", ".cpp", ".h")

INCLUDE_RE = re.compile(r'^\s*#\s*include\s*"([^"]+)"', re.M)


def project_root():
    """定位项目根：优先 __file__ 上溯（标准 Python import 路径）；
    防御性 CWD 回退（PIO SCons 某些边界场景 __file__ 不可用时）。
    均以 platformio.ini 存在性校验。"""
    try:
        root = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
        if os.path.exists(os.path.join(root, "platformio.ini")):
            return root
    except (NameError, AttributeError):
        pass
    return os.getcwd()


def layer_of(stem):
    if stem in CORE:
        return "CORE"
    if stem in ASSEMBLY:
        return "ASSEMBLY"
    return "APP"


def resolve_include(src_root, include_from, inc):
    """include 解析到 src/ 内实际文件路径；系统头/库头返回 None。
    先相对 include 所在目录（支撑 panels/../debug_log.h 写法），
    再退回 src/ 根（支撑简单名）。"""
    cand = os.path.normpath(os.path.join(os.path.dirname(include_from), inc))
    if os.path.isfile(cand):
        return cand
    cand = os.path.join(src_root, inc)
    if os.path.isfile(cand):
        return cand
    return None


def scan(root):
    """返回违规列表 [(core_file, included_header)，...]（含白名单内项，
    供调用方区分）。"""
    src = os.path.join(root, "src")
    files = []
    for dirpath, dirnames, filenames in os.walk(src):
        rel = os.path.relpath(dirpath, src)
        parts = set(rel.split(os.sep))
        if parts & SKIP_DIRS:
            continue
        for fn in filenames:
            if fn.endswith(EXTS):
                files.append(os.path.join(dirpath, fn))

    violations = []
    for path in sorted(files):
        fn = os.path.basename(path)
        stem = os.path.splitext(fn)[0]
        if layer_of(stem) != "CORE":
            continue
        try:
            text = open(path, encoding="utf-8", errors="replace").read()
        except OSError:
            continue
        for inc in INCLUDE_RE.findall(text):
            # 系统头/库头（freertos、driver、esp_* 等，src 内无此文件）
            # 不参与分层判定；资源目录（Fonts 等）视为 Core 资源
            target = resolve_include(src, path, inc)
            if target is None:
                continue
            rel_dir = set(os.path.relpath(os.path.dirname(target), src)
                          .split(os.sep))
            if rel_dir & SKIP_DIRS:
                continue
            inc_stem = os.path.splitext(os.path.basename(inc))[0]
            if layer_of(inc_stem) == "APP":
                violations.append((fn, os.path.basename(inc)))
    return violations


def load_whitelist(root):
    wl = set()
    path = os.path.join(root, "tools", "layering_whitelist.txt")
    if os.path.exists(path):
        for line in open(path, encoding="utf-8"):
            line = line.strip()
            if not line or line.startswith("#"):
                continue
            if ":" in line:
                violator, included = line.split(":", 1)
                wl.add((violator.strip(), included.strip()))
    return wl


def main():
    strict = "--strict" in sys.argv
    root = project_root()

    if not os.path.isdir(os.path.join(root, "src")):
        print("layering check: ERROR src/ not found under %s "
              "(run from project root)" % root)
        return 1

    violations = scan(root)
    whitelist = load_whitelist(root)

    new = [v for v in violations if v not in whitelist]
    # 白名单条目失效（对应违规已消除）也提示，防清单腐化
    stale = sorted(whitelist - set(violations))

    for f, inc in sorted(new):
        print("LAYERING WARN: CORE %s includes APP header <%s>" % (f, inc))
    for f, inc in stale:
        print("LAYERING NOTE: whitelist entry stale (violation gone): "
              "%s: %s" % (f, inc))

    if not new:
        print("layering check: OK (%d CORE files, %d whitelisted)"
              % (len(CORE), len(violations)))
        return 0
    if strict:
        print("layering check: %d violation(s) (strict mode)" % len(new))
        return 1
    print("layering check: %d violation(s) (warn-only; see "
          "tools/check_layering.py header for rules)" % len(new))
    return 0


if __name__ == "__main__":
    sys.exit(main())
else:
    # PlatformIO pre 脚本路径：import 即执行（此时 sys.argv 无 --strict，
    # 恒 warn-only 不阻塞构建；升 strict 由 CI 独立运行 --strict 负责）
    main()
