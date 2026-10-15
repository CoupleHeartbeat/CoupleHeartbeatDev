#!/usr/bin/env python3
"""Regenerate assets/fonts/couple_font_{16,20}.c from NotoSansSC-Regular.ttf.

Why this script exists: lv_font_conv requires Node.js, which is not reliably
available in this repository's environments. This generator reproduces the
exact lv_font_conv output format (LVGL ``lv_font_fmt_txt`` 4bpp plain bitmaps)
using Pillow + fontTools only, and self-verifies every packed glyph.

Output contract (must stay byte-compatible with the previous lv_font_conv
fonts so no layout code changes):

* glyph id 0 is the reserved all-zero entry; ASCII 0x20..0x7E uses ids 1..95
  via a FORMAT0_TINY cmap; the sparse block (arrows + CJK) starts at id 96
  via a SPARSE_TINY cmap whose ``unicode_list`` holds uint16 offsets relative
  to ``range_start``.
* 4bpp, high nibble first, rows packed bit-contiguously (stride = 0).
* ``adv_w`` is derived from unhinted font units (1/16 px units), the ink box
  comes from Pillow's hinted rasterization so the on-screen result is crisp.
* Vertical placement follows LVGL's baseline rule
  ``letter.y1 = line_top + (line_height - base_line) - ofs_y``; with Pillow's
  baseline anchor this maps to ``ofs_y = baseline_y - bbox.y1_exclusive``.

Usage:
    python3 tools/gen_couple_fonts.py            # regenerate both fonts
    python3 tools/gen_couple_fonts.py --check    # regenerate + coverage gate

The coverage gate scans ``main/*.c`` string literals and fails when a CJK
character used by the firmware UI is missing from the generated fonts (it
would render as tofu because the fallback Montserrat font has no CJK glyphs).
"""

from __future__ import annotations

import argparse
import os
import re
import sys
import textwrap

try:
    from PIL import Image, ImageDraw, ImageFont
except ImportError:  # pragma: no cover
    sys.exit("Pillow is required: python3 -m pip install pillow")

try:
    from fontTools.ttLib import TTFont
except ImportError:  # pragma: no cover
    sys.exit("fontTools is required: python3 -m pip install fonttools")

REPO_ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TTF_PATH = os.path.join(
    REPO_ROOT,
    "managed_components/lvgl__lvgl/tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf",
)
FONT_DIR = os.path.join(REPO_ROOT, "assets/fonts")

ASCII_START, ASCII_END = 0x20, 0x7E
ARROW_START, ARROW_END = 0x2191, 0x2193  # up / down / right

# line_height / base_line / underline taken from the previous lv_font_conv
# output so existing widget layouts keep the exact same vertical metrics.
SIZES = [
    {"size": 16, "line_height": 20, "base_line": 5},
    {"size": 20, "line_height": 24, "base_line": 6},
]

# Fallback sparse set (used only when the previous font cannot be parsed):
# arrows 0x2191-0x2193 plus the 63 CJK characters of the original fonts.
BASE_CJK = (
    "一三中二互五亲侣六出切功加动厂周四回复对已式心态恢情感成择按换搜"
    "日月未模爱状用电的知码确索置色角认设请调贴跳载返选速配量键长音"
)

# Every device-side UI string that must be renderable. Keep this list in sync
# with the strings in main/*.c; the --check coverage gate enforces it.
UI_STRINGS = [
    # pair / home
    "情侣互动", "加载中...", "角色", "未配对", "请用App配对", "配对码:%04u",
    "OK切换 ↑↓调音量", "已恢复出厂", "音量%d%%", "AI PASSPORT",
    # 工牌页靠近时的一行距离/信号（光效分档本身不需要文字）
    "距离 %.0f cm  信号 %d", "距离 %.1f m  信号 %d",
    # settings menu (short press OK)
    "设置", "关于设备", "立即休眠", "恢复出厂设置", "↑↓选择  OK确认", "确认", "取消",
    # low-power deep sleep warning overlay
    "即将休眠", "即将休眠\n按任意键取消",
    "名称:", "固件: v1.0.0", "短按OK设置", "长按OK对话", "↑↓音量",
    "长按OK返回", "OK返回",
    # AI dialog page (long press OK)
    "AI 对话", "待机", "聆听中%s %d", "再短按OK结束", "思考中...", "播放中...",
    "连接失败", "短按OK说话\n长按OK返回", "短按OK重试\n长按OK返回",
    "请先在App配置Key", "请先在App配置Wi-Fi", "没听清，请再说一次",
]


def parse_previous_sparse() -> set[int]:
    """Recover the sparse codepoint set from the previous font (best effort)."""
    path = os.path.join(FONT_DIR, "couple_font_16.c")
    try:
        src = open(path, encoding="utf-8").read()
        rng = re.search(r"\.range_start = (\d+), \.range_length = (\d+), "
                        r"\.glyph_id_start = \d+,\s*\.unicode_list = unicode_list_1",
                        src)
        lst = re.search(r"unicode_list_1\[\]\s*=\s*\{(.*?)\};", src, re.S)
        if not rng or not lst:
            raise ValueError("cmap/unicode_list not found")
        start = int(rng.group(1))
        offsets = [int(x, 0) for x in re.findall(r"0x[0-9a-fA-F]+|\d+", lst.group(1))]
        return {start + off for off in offsets}
    except (OSError, ValueError):
        return {ARROW_START + i for i in range(ARROW_END - ARROW_START + 1)} | set(
            ord(c) for c in BASE_CJK
        )


def collect_charset() -> tuple[list[int], list[int]]:
    """Return (ascii_codepoints, sparse_codepoints_sorted)."""
    ascii_cps = list(range(ASCII_START, ASCII_END + 1))
    sparse = set(parse_previous_sparse())
    for s in UI_STRINGS:
        for ch in s:
            cp = ord(ch)
            if cp > ASCII_END:
                sparse.add(cp)
    return ascii_cps, sorted(sparse)


def advance16(font_units: "TTFont", cp: int, size: int) -> int:
    """Unhinted advance width in 1/16 px units (matches lv_font_conv)."""
    upem = font_units["head"].unitsPerEm
    cmap = font_units.getBestCmap()
    glyph_name = cmap.get(cp)
    if glyph_name is None:
        return 0
    adv_units = font_units["hmtx"][glyph_name][0]
    return int(round(adv_units * size / upem * 16))


def render_glyph(pil_font: "ImageFont.FreeTypeFont", cp: int, size: int,
                 ch: str) -> tuple[int, int, int, int, bytes] | None:
    """Rasterize one glyph.

    Returns (box_w, box_h, ofs_x, ofs_y, packed_4bpp_bytes) or None when the
    glyph has no ink (e.g. space): callers then emit an empty box.
    """
    canvas_w = size * 2 + size * 2 + 8
    canvas_h = size * 4
    baseline = size * 2
    origin_x = size
    img = Image.new("L", (canvas_w, canvas_h), 0)
    ImageDraw.Draw(img).text((origin_x, baseline), ch, font=pil_font,
                             anchor="ls", fill=255)
    bbox = img.getbbox()
    if bbox is None:
        return None
    box_w = bbox[2] - bbox[0]
    box_h = bbox[3] - bbox[1]
    ofs_x = bbox[0] - origin_x
    ofs_y = baseline - bbox[3]  # LVGL baseline rule, see module docstring

    # Quantize 8bpp coverage to 4bpp (16 levels), then pack high-nibble-first
    # with rows contiguous in the bit stream (stride == 0 format).
    pixels = []
    for y in range(bbox[1], bbox[3]):
        for x in range(bbox[0], bbox[2]):
            v = img.getpixel((x, y))
            pixels.append((v * 15 + 127) // 255)

    total_bits = box_w * box_h * 4
    packed = bytearray((total_bits + 7) // 8)
    bitpos = 0
    for row in range(box_h):
        for col in range(box_w):
            nibble = pixels[row * box_w + col]
            byte_i = bitpos // 8
            if bitpos % 8 == 0:
                packed[byte_i] = nibble << 4
            else:
                packed[byte_i] |= nibble
            bitpos += 4

    # Self-verify: decode the packed stream and compare against quantized ink.
    dec = []
    bitpos = 0
    for _ in range(box_w * box_h):
        byte_i = bitpos // 8
        nib = (packed[byte_i] >> 4) if bitpos % 8 == 0 else (packed[byte_i] & 0xF)
        dec.append(nib)
        bitpos += 4
    assert dec == pixels, f"packed bitmap mismatch for U+{cp:04X}"
    return box_w, box_h, ofs_x, ofs_y, bytes(packed)


def char_comment(cp: int) -> str:
    if cp == 0x22:
        glyph = '\\"'
    elif cp == 0x5C:
        glyph = "\\\\"
    elif cp == 0x20:
        glyph = " "
    else:
        glyph = chr(cp)
    return f'/* U+{cp:04X} "{glyph}" */'


def emit_font_with_offsets(size_cfg, ascii_cps, sparse_cps, tt, out_path):
    """Wrapper that assigns bitmap_index offsets before emitting."""
    size = size_cfg["size"]
    pil_font = ImageFont.truetype(TTF_PATH, size)
    glyphs: list[dict] = []
    bitmap = bytearray()

    def add(cp: int) -> None:
        adv = advance16(tt, cp, size)
        entry = {"cp": cp, "adv": adv, "w": 0, "h": 0, "x": 0, "y": 0, "data": b""}
        if cp != 0x20:
            r = render_glyph(pil_font, cp, size, chr(cp))
            if r is not None:
                w, h, x, y, data = r
                entry.update({"w": w, "h": h, "x": x, "y": y, "data": data})
        entry["off"] = len(bitmap)
        bitmap.extend(entry["data"])
        glyphs.append(entry)

    for cp in ascii_cps:
        add(cp)
    for cp in sparse_cps:
        add(cp)

    name = f"couple_font_{size}"
    out: list[str] = []
    out.append(
        "#ifdef LV_LVGL_H_INCLUDE_SIMPLE\n"
        '#include "lvgl.h"\n'
        "#else\n"
        "#include <lvgl.h>\n"
        "#endif\n"
        '#include "font/fmt_txt/lv_font_fmt_txt.h"\n\n'
        f"#ifndef {name.upper()}\n"
        f"#define {name.upper()} 1\n"
        f"#endif\n\n"
        f"#if {name.upper()}\n\n"
    )
    out.append(
        "/* Generated by tools/gen_couple_fonts.py (Pillow + fontTools).\n"
        " * Source: managed_components/lvgl__lvgl/tests/src/test_files/fonts/noto/NotoSansSC-Regular.ttf (OFL).\n"
        f" * Charset: U+0020..U+007E, U+2191..U+2193 arrows, {len(sparse_cps) - 3} CJK characters.\n"
        f" * Rasterizer: Pillow hinted ink box + unhinted font-unit advance, {size}px, 4bpp plain.\n"
        " * Reproduce: python3 tools/gen_couple_fonts.py\n"
        " */\n\n"
    )
    out.append(
        "/*---------------------\n"
        " *  GLYPH BITMAPS\n"
        " *--------------------*/\n\n"
        "static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {\n"
    )
    for g in glyphs:
        out.append(char_comment(g["cp"]) + "\n")
        if not g["data"]:
            continue
        buf: list[str] = []
        for b in g["data"]:
            buf.append(f"0x{b:02x},")
            if len(buf) == 12:
                out.append(" ".join(buf) + "\n")
                buf = []
        if buf:
            out.append(" ".join(buf) + "\n")
    out.append("};\n\n")

    out.append(
        "/*---------------------\n"
        " *  GLYPH DESCRIPTIONS\n"
        " *--------------------*/\n\n"
        "static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {\n"
        "    {.bitmap_index = 0, .adv_w = 0, .box_w = 0, .box_h = 0, .ofs_x = 0, .ofs_y = 0}, /* id0 reserved */\n"
    )
    for g in glyphs:
        out.append(
            f"    {{.bitmap_index = {g['off']}, .adv_w = {g['adv']}, "
            f".box_w = {g['w']}, .box_h = {g['h']}, "
            f".ofs_x = {g['x']}, .ofs_y = {g['y']}}}, {char_comment(g['cp'])}\n"
        )
    out.append("};\n\n")

    offsets = [g["cp"] - sparse_cps[0] for g in glyphs[len(ascii_cps):]]
    out.append("static const uint16_t unicode_list_1[] = {\n")
    for i in range(0, len(offsets), 8):
        chunk = ", ".join(f"0x{o:x}" for o in offsets[i:i + 8])
        out.append(f"    {chunk},\n")
    out.append("};\n\n")

    out.append(
        "/*---------------------\n"
        " *  CHARACTER MAPPING\n"
        " *--------------------*/\n\n"
        "static const lv_font_fmt_txt_cmap_t cmaps[] = {\n"
        "    {\n"
        "        .range_start = 32, .range_length = 95, .glyph_id_start = 1,\n"
        "        .unicode_list = NULL, .glyph_id_ofs_list = NULL, .list_length = 0,\n"
        "        .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY\n"
        "    },\n"
        "    {\n"
        f"        .range_start = {sparse_cps[0]}, .range_length = {sparse_cps[-1] - sparse_cps[0] + 1}, .glyph_id_start = 96,\n"
        "        .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL, "
        f".list_length = {len(sparse_cps)},\n"
        "        .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY\n"
        "    },\n"
        "};\n\n"
    )
    out.append(
        "/*---------------------\n"
        " *  ALL CUSTOM DATA\n"
        " *--------------------*/\n\n"
        "#if LVGL_VERSION_MAJOR == 8\n"
        "/*Store all the custom data of the font*/\n"
        "static lv_font_fmt_txt_glyph_cache_t cache;\n"
        "#endif\n\n"
        "#if LVGL_VERSION_MAJOR >= 8\n"
        "static const lv_font_fmt_txt_dsc_t font_dsc = {\n"
        "#else\n"
        "static lv_font_fmt_txt_dsc_t font_dsc = {\n"
        "#endif\n"
        "    .glyph_bitmap = glyph_bitmap,\n"
        "    .glyph_dsc = glyph_dsc,\n"
        "    .cmaps = cmaps,\n"
        "    .kern_dsc = NULL,\n"
        "    .kern_scale = 0,\n"
        "    .cmap_num = 2,\n"
        "    .bpp = 4,\n"
        "    .kern_classes = 0,\n"
        "    .bitmap_format = 0,\n"
        "#if LVGL_VERSION_MAJOR == 8\n"
        "    .cache = &cache\n"
        "#endif\n"
        "};\n\n"
        "/*---------------------\n"
        " *  PUBLIC FONT\n"
        " *--------------------*/\n\n"
        "/*Initialize a public general font descriptor*/\n"
        "#if LVGL_VERSION_MAJOR >= 8\n"
        f"const lv_font_t {name} = {{\n"
        "#else\n"
        f"lv_font_t {name} = {{\n"
        "#endif\n"
        "    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,\n"
        "    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,\n"
        f"    .line_height = {size_cfg['line_height']},\n"
        f"    .base_line = {size_cfg['base_line']},\n"
        "    .underline_position = -2,\n"
        "    .underline_thickness = 1,\n"
        "    .dsc = &font_dsc,\n"
        "    .fallback = NULL,\n"
        "    .user_data = NULL,\n"
        "};\n\n"
        f"#endif /*#if {name.upper()}*/\n"
    )
    with open(out_path, "w", encoding="utf-8") as f:
        f.writelines(out)
    print(f"wrote {os.path.relpath(out_path, REPO_ROOT)}: "
          f"{len(glyphs)} glyphs, {len(bitmap)} bitmap bytes")


def strip_non_ui_text(src: str) -> str:
    """Blank out comments and ESP_LOGx(...) calls so only UI text is scanned.

    A char scanner keeps string-literal contents opaque (parentheses inside
    strings don't affect depth) and blanks ``//``, ``/* */``, and the full
    argument list of any ``ESP_LOGx(`` call. Only text that reaches an LVGL
    label must be covered by the couple fonts; log/comment CJK is excluded.
    """
    out = list(src)
    n = len(src)
    i = 0

    def blank(a: int, b: int) -> None:
        for k in range(a, b):
            if out[k] != "\n":
                out[k] = " "

    while i < n:
        c = src[i]
        # line comment
        if c == "/" and i + 1 < n and src[i + 1] == "/":
            j = src.find("\n", i)
            j = n if j == -1 else j
            blank(i, j)
            i = j
            continue
        # block comment
        if c == "/" and i + 1 < n and src[i + 1] == "*":
            j = src.find("*/", i + 2)
            j = n if j == -1 else j + 2
            blank(i, j)
            i = j
            continue
        # ESP_LOGx( ... ) — blank whole call
        m = re.match(r"ESP_LOG[WTIDE]\s*\(", src[i:])
        if m:
            start = i
            j = i + m.end() - 1  # position of '('
            depth = 0
            while j < n:
                ch = src[j]
                if ch == '"':
                    j += 1
                    while j < n and src[j] != '"':
                        j += 2 if src[j] == "\\" else 1
                elif ch == "(":
                    depth += 1
                elif ch == ")":
                    depth -= 1
                    if depth == 0:
                        j += 1
                        break
                j += 1
            blank(start, j)
            i = j
            continue
        # ordinary string literal: keep, but skip over it verbatim
        if c == '"':
            i += 1
            while i < n and src[i] != '"':
                i += 2 if src[i] == "\\" else 1
            i += 1
            continue
        # character literal, including '\'' and '"'. Skipping it keeps the
        # quote parity intact: without this branch a '"' inside a char literal
        # (e.g. ``if (*p != '"')``) is mistaken for a string opening quote, so
        # the scanner loses track of whether it is inside a literal and can
        # scan a following ``//`` comment as if it were UI text.
        if c == "'":
            i += 1
            while i < n and src[i] != "'":
                i += 2 if src[i] == "\\" else 1
            i += 1
            continue
        i += 1
    return "".join(out)


def coverage_gate(ascii_cps: list[int], sparse_cps: list[int]) -> int:
    """Fail when main/*.c UI literals use CJK missing from the fonts."""
    available = set(ascii_cps) | set(sparse_cps)
    missing: dict[str, set[str]] = {}
    main_dir = os.path.join(REPO_ROOT, "main")
    string_re = re.compile(r'"((?:[^"\\]|\\.)*)"')
    for fn in sorted(os.listdir(main_dir)):
        if not fn.endswith(".c"):
            continue
        src = strip_non_ui_text(open(os.path.join(main_dir, fn), encoding="utf-8").read())
        for m in string_re.finditer(src):
            lit = m.group(1)
            for ch in lit:
                cp = ord(ch)
                if cp >= 0x2000 and cp not in available:  # CJK / fullwidth / CJK punct
                    missing.setdefault(ch, set()).add(fn)
    if missing:
        print("coverage gate FAILED: UI characters missing from couple fonts:")
        for ch, files in sorted(missing.items()):
            print(f"  {ch!r} U+{ord(ch):04X} used in {', '.join(sorted(files))}")
        return 1
    print("coverage gate OK: every CJK character in main/*.c UI literals is in the fonts")
    return 0


def main() -> int:
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument("--check", action="store_true",
                    help="also run the main/*.c CJK coverage gate")
    args = ap.parse_args()

    if not os.path.exists(TTF_PATH):
        print(f"source font not found: {TTF_PATH}", file=sys.stderr)
        return 1

    ascii_cps, sparse_cps = collect_charset()
    tt = TTFont(TTF_PATH)
    for size_cfg in SIZES:
        out_path = os.path.join(FONT_DIR, f"couple_font_{size_cfg['size']}.c")
        emit_font_with_offsets(size_cfg, ascii_cps, sparse_cps, tt, out_path)

    if args.check:
        return coverage_gate(ascii_cps, sparse_cps)
    return 0


if __name__ == "__main__":
    sys.exit(main())
