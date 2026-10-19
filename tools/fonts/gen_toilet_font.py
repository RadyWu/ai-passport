#!/usr/bin/env python3
"""Generate assets/fonts/font_toilet_14.c from toilet_font_inventory.txt.

Rasterizes with Pillow/FreeType (PLAIN 4bpp, ASCII FORMAT0_TINY + SPARSE_TINY).
Avoids lv_font_conv, which has produced corrupt bitmaps on modern Node.js.

Usage (from repo root):
  python3 tools/fonts/gen_toilet_font.py \\
    --font managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf

Self-checks: round-trip every glyph bitmap against the rasterizer.
"""
from __future__ import annotations

import argparse
import re
import sys
from pathlib import Path

from PIL import Image, ImageDraw, ImageFont

REPO = Path(__file__).resolve().parents[2]
DEFAULT_INV = REPO / "assets/fonts/toilet_font_inventory.txt"
DEFAULT_OUT_C = REPO / "assets/fonts/font_toilet_14.c"
DEFAULT_OUT_H = REPO / "assets/fonts/font_toilet_14.h"
DEFAULT_FONT = (
    REPO
    / "managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf"
)

SIZE_PX = 14
BPP = 4  # default; main() may override
BASE_LINE = 3  # distance from bottom of line box to baseline (LVGL convention)




def pack_plain(pixels: list[int], box_w: int, box_h: int, bpp: int) -> bytes:
    """Pack alpha values PLAIN, MSB first, byte-aligned per glyph."""
    if bpp not in (2, 4):
        raise ValueError(bpp)
    maxv = (1 << bpp) - 1
    out = bytearray()
    acc = 0
    bits = 0
    for y in range(box_h):
        for x in range(box_w):
            v = pixels[y * box_w + x] & maxv
            acc = (acc << bpp) | v
            bits += bpp
            if bits == 8:
                out.append(acc & 0xFF)
                acc = 0
                bits = 0
    if bits:
        acc <<= (8 - bits)
        out.append(acc & 0xFF)
    return bytes(out)


def unpack_plain(data: bytes, box_w: int, box_h: int, bpp: int) -> list[int]:
    maxv = (1 << bpp) - 1
    pixels: list[int] = []
    needed = box_w * box_h
    bitpos = 0
    total_bits = len(data) * 8
    while len(pixels) < needed and bitpos + bpp <= total_bits:
        byte_i = bitpos // 8
        offset = bitpos % 8
        # read bpp bits MSB-first from stream
        b = data[byte_i]
        # may span bytes
        avail = 8 - offset
        if avail >= bpp:
            shift = avail - bpp
            v = (b >> shift) & maxv
        else:
            # spanning — our pack never spans mid-byte across pixels oddly for 2/4
            v = (b & ((1 << avail) - 1)) << (bpp - avail)
            b2 = data[byte_i + 1]
            v |= (b2 >> (8 - (bpp - avail))) & ((1 << (bpp - avail)) - 1)
        pixels.append(v)
        bitpos += bpp
    return pixels


def rasterize(font: ImageFont.FreeTypeFont, ch: str) -> tuple[bytes, dict]:
    """Return (bitmap_bytes, dsc) for one character."""
    # Large canvas; find ink bounds from alpha mask.
    canvas = Image.new("L", (SIZE_PX * 3, SIZE_PX * 3), 0)
    draw = ImageDraw.Draw(canvas)
    # Anchor at a known baseline point inside the canvas.
    # Pillow "ls" = left baseline.
    anchor_x, anchor_y = SIZE_PX, SIZE_PX + BASE_LINE
    draw.text((anchor_x, anchor_y), ch, font=font, fill=255, anchor="ls")

    bbox = canvas.getbbox()
    if bbox is None:
        # Empty glyph (space etc.): advance from font metrics.
        try:
            adv = font.getlength(ch)
        except Exception:
            adv = SIZE_PX * 0.5
        adv_w = max(1, int(round(adv * 16)))
        return b"", {
            "adv_w": adv_w,
            "box_w": 0,
            "box_h": 0,
            "ofs_x": 0,
            "ofs_y": 0,
        }

    left, top, right, bottom = bbox
    box_w = right - left
    box_h = bottom - top
    crop = canvas.crop((left, top, right, bottom))
    maxv = (1 << BPP) - 1
    pix = []
    for p in crop.get_flattened_data():
        pix.append(min(maxv, (int(p) * maxv + 127) // 255))

    ofs_x = left - anchor_x
    # LVGL ofs_y: offset of bitmap bottom from baseline (can be negative).
    # Bitmap rows go from top to bottom. Bottom row is at `bottom-1`.
    # Distance from baseline (anchor_y) up to bottom of bitmap:
    # ofs_y in LVGL is the y offset of the TOP? No —
    # From LVGL docs: ofs_y is "The Y offset with base line".
    # In fmt_txt: ofs_y is signed; drawing starts at baseline - (ofs_y + box_h)? 
    # Looking at existing space glyph: ofs_y = 17 with box_h=1 — odd.
    # Looking at '!' : box_h=11, ofs_y=6 — so top is drawn at baseline - ofs_y - ? 
    #
    # From lv_draw_sw_letter: 
    #   letter_pos.y = pos.y + (font->line_height - font->base_line) - glyph_dsc->box_h - glyph_dsc->ofs_y
    # Wait for LVGL 9, check glyph_dsc meaning.
    #
    # Existing 'A'-like: ofs_y=6, box_h=11 → ink from (baseline_area).
    # Existing generator used: ofs_y = (baseline_in_bitmap_coords relative).
    #
    # Our anchor_y is baseline. Bitmap top is `top`. 
    # In LVGL 8/9 fmt_txt: ofs_y is the distance from the baseline to the BOTTOM of the box?
    # Actually from lv_font_fmt_txt.h:
    #   ofs_y: "The Y offset with base line" — typically negative or small.
    # Montserrat convention: ofs_y positive means box sits above baseline starting
    # at (baseline - ofs_y - box_h)? Let's match empirically from existing ASCII.
    #
    # Existing '!': box_h=11, ofs_y=6. Char height ~11, so bottom of box is
    # roughly at baseline - 6 + something.
    #
    # Safer approach: match LVGL's own expectation used by lv_font_conv:
    # ofs_y = -(bottom - anchor_y)   i.e. how far the bitmap bottom is below baseline
    # (negative if above). And drawing uses:
    #   y = baseline - ofs_y - box_h   for top? 
    #
    # From LVGL 9.5 lv_font.h comments for lv_font_glyph_dsc_t:
    #   ofs_x, ofs_y: offsets relative to the current position / baseline.
    # Drawing in lv_draw_label: 
    #   pos_y = base_line_y - glyph_dsc.box_h - glyph_dsc.ofs_y
    # So if we want bitmap top at `top`, and baseline at `anchor_y`:
    #   top = anchor_y - box_h - ofs_y  =>  ofs_y = anchor_y - box_h - top
    #                                                     = anchor_y - bottom
    ofs_y = anchor_y - bottom

    try:
        adv = font.getlength(ch)
    except Exception:
        adv = box_w
    adv_w = max(1, int(round(adv * 16)))

    return pack_plain(pix, box_w, box_h, BPP), {
        "adv_w": adv_w,
        "box_w": box_w,
        "box_h": box_h,
        "ofs_x": ofs_x,
        "ofs_y": ofs_y,
        "pixels": pix,
    }


def load_inventory(path: Path) -> list[str]:
    raw = path.read_text(encoding="utf-8")
    # Unique, keep order of first occurrence but ensure ASCII run is contiguous
    seen = set()
    out: list[str] = []
    for ch in raw:
        if ch in "\n\r\t":
            continue
        if ch in seen:
            continue
        if ord(ch) < 0x20:
            continue
        seen.add(ch)
        out.append(ch)
    # Ensure full ASCII printable present, in order, at the front for FORMAT0_TINY
    ascii_chars = [chr(c) for c in range(0x20, 0x7F)]
    rest = [c for c in out if not (0x20 <= ord(c) <= 0x7E)]
    rest = sorted(rest, key=ord)
    return ascii_chars + rest


def emit_c(
    chars: list[str],
    glyphs: list[dict],
    bitmaps: list[bytes],
    out_c: Path,
    out_h: Path,
    *,
    symbol: str,
    line_height: int,
    base_line: int,
    guard_macro: str,
):
    # glyph 0 = .notdef empty
    all_dsc = [
        {"bitmap_index": 0, "adv_w": 0, "box_w": 0, "box_h": 0, "ofs_x": 0, "ofs_y": 0}
    ]
    bitmap_blob = bytearray()
    for dsc, bmp in zip(glyphs, bitmaps):
        all_dsc.append(
            {
                "bitmap_index": len(bitmap_blob),
                "adv_w": dsc["adv_w"],
                "box_w": dsc["box_w"],
                "box_h": dsc["box_h"],
                "ofs_x": dsc["ofs_x"],
                "ofs_y": dsc["ofs_y"],
            }
        )
        bitmap_blob.extend(bmp)

    ascii_chars = chars[:95]  # 0x20-0x7E
    assert [ord(c) for c in ascii_chars] == list(range(0x20, 0x7F))
    sparse = chars[95:]

    lines: list[str] = []
    lines.append(f"/* Auto-generated toilet reader font ({symbol}). Do not edit. */")
    lines.append('#include "lvgl.h"')
    lines.append("")
    lines.append(f"#ifndef {guard_macro}")
    lines.append(f"#define {guard_macro} 1")
    lines.append("#endif")
    lines.append("")
    lines.append("static LV_ATTRIBUTE_LARGE_CONST const uint8_t glyph_bitmap[] = {")
    row: list[str] = []
    for i, b in enumerate(bitmap_blob):
        row.append(f"0x{b:02X},")
        if len(row) == 16:
            lines.append("    " + " ".join(row))
            row = []
    if row:
        lines.append("    " + " ".join(row))
    if not bitmap_blob:
        lines.append("    0x00,")
    lines.append("};")
    lines.append("")

    lines.append("static const lv_font_fmt_txt_glyph_dsc_t glyph_dsc[] = {")
    for d in all_dsc:
        lines.append(
            "    {.bitmap_index = %d, .adv_w = %d, .box_w = %d, .box_h = %d, "
            ".ofs_x = %d, .ofs_y = %d},"
            % (
                d["bitmap_index"],
                d["adv_w"],
                d["box_w"],
                d["box_h"],
                d["ofs_x"],
                d["ofs_y"],
            )
        )
    lines.append("};")
    lines.append("")

    if sparse:
        range_start = ord(sparse[0])
        unicode_list = [ord(c) - range_start for c in sparse]
        assert unicode_list == sorted(unicode_list)
        range_length = unicode_list[-1] + 1
        lines.append(
            "static const uint16_t unicode_list_1[%d] = {" % len(unicode_list)
        )
        row = []
        for v in unicode_list:
            row.append(f"{v},")
            if len(row) == 12:
                lines.append("    " + " ".join(row))
                row = []
        if row:
            lines.append("    " + " ".join(row))
        lines.append("};")
        lines.append("")

    lines.append("static const lv_font_fmt_txt_cmap_t cmaps[] = {")
    lines.append("    {")
    lines.append(
        "        .range_start = 0x20, .range_length = 95, .glyph_id_start = 1,"
    )
    lines.append(
        "        .unicode_list = NULL, .glyph_id_ofs_list = NULL, "
        ".list_length = 0, .type = LV_FONT_FMT_TXT_CMAP_FORMAT0_TINY"
    )
    lines.append("    },")
    if sparse:
        lines.append("    {")
        lines.append(
            "        .range_start = %d, .range_length = %d, .glyph_id_start = 96,"
            % (range_start, range_length)
        )
        lines.append(
            "        .unicode_list = unicode_list_1, .glyph_id_ofs_list = NULL, "
            ".list_length = %d, .type = LV_FONT_FMT_TXT_CMAP_SPARSE_TINY"
            % len(unicode_list)
        )
        lines.append("    },")
    lines.append("};")
    lines.append("")

    lines.append("static const lv_font_fmt_txt_dsc_t font_dsc = {")
    lines.append("    .glyph_bitmap = glyph_bitmap,")
    lines.append("    .glyph_dsc = glyph_dsc,")
    lines.append("    .cmaps = cmaps,")
    lines.append("    .kern_dsc = NULL,")
    lines.append("    .kern_scale = 0,")
    lines.append("    .cmap_num = %d," % (2 if sparse else 1))
    lines.append("    .bpp = %d," % BPP)
    lines.append("    .kern_classes = 0,")
    lines.append("    .bitmap_format = 0,")
    lines.append("};")
    lines.append("")

    lines.append(f"static const lv_font_t {symbol}_dsc = {{")
    lines.append("    .get_glyph_dsc = lv_font_get_glyph_dsc_fmt_txt,")
    lines.append("    .get_glyph_bitmap = lv_font_get_bitmap_fmt_txt,")
    lines.append("    .line_height = %d," % line_height)
    lines.append("    .base_line = %d," % base_line)
    lines.append("    .subpx = LV_FONT_SUBPX_NONE,")
    lines.append("    .underline_position = -1,")
    lines.append("    .underline_thickness = 1,")
    lines.append("    .dsc = &font_dsc,")
    lines.append("    .fallback = NULL,")
    lines.append("    .user_data = NULL,")
    lines.append("};")
    lines.append("")
    lines.append(f"const lv_font_t {symbol} = {symbol}_dsc;")
    lines.append("")

    out_c.write_text("\n".join(lines), encoding="utf-8")
    out_h.write_text(
        f'#pragma once\n#include "lvgl.h"\nLV_FONT_DECLARE({symbol});\n',
        encoding="utf-8",
    )



def verify(glyphs: list[dict], bitmaps: list[bytes]) -> None:
    for i, (dsc, bmp) in enumerate(zip(glyphs, bitmaps)):
        w, h = dsc["box_w"], dsc["box_h"]
        if w == 0 or h == 0:
            if bmp:
                raise SystemExit(f"glyph {i}: empty box but non-empty bitmap")
            continue
        expected = dsc.get("pixels")
        if expected is None:
            continue
        got = unpack_plain(bmp, w, h, BPP)
        if got != expected:
            raise SystemExit(
                f"glyph {i} round-trip mismatch (box {w}x{h}, bmp {len(bmp)} bytes)"
            )


def main() -> int:
    global SIZE_PX, BPP
    ap = argparse.ArgumentParser()
    ap.add_argument("--font", type=Path, default=DEFAULT_FONT)
    ap.add_argument("--inventory", type=Path, default=DEFAULT_INV)
    ap.add_argument("--size", type=int, default=14)
    ap.add_argument("--bpp", type=int, default=4, choices=[2, 4])
    ap.add_argument("--name", type=str, default="",
                    help="LVGL symbol, default font_toilet_<size>")
    ap.add_argument("--out-c", type=Path, default=None)
    ap.add_argument("--out-h", type=Path, default=None)
    args = ap.parse_args()

    symbol = args.name or f"font_toilet_{args.size}"
    out_c = args.out_c or (REPO / f"assets/fonts/{symbol}.c")
    out_h = args.out_h or (REPO / f"assets/fonts/{symbol}.h")
    # Tight line box: size + ~21% leading (14→17, 28→34)
    line_height = max(args.size + 3, int(round(args.size * 1.21)))
    base_line = max(2, line_height - args.size + 1)

    if not args.font.is_file():
        print(f"ERROR: font not found: {args.font}", file=sys.stderr)
        return 1

    # rasterize() uses module-level SIZE_PX for canvas
    SIZE_PX = args.size
    BPP = args.bpp

    chars = load_inventory(args.inventory)
    print(f"inventory: {len(chars)} glyphs from {args.inventory}; size={args.size}")

    font = ImageFont.truetype(str(args.font), args.size)

    glyphs: list[dict] = []
    bitmaps: list[bytes] = []
    for i, ch in enumerate(chars):
        bmp, dsc = rasterize(font, ch)
        bitmaps.append(bmp)
        glyphs.append(dsc)
        if (i + 1) % 500 == 0:
            print(f"  rasterized {i+1}/{len(chars)}")

    verify(glyphs, bitmaps)
    guard = symbol.upper()
    emit_c(
        chars, glyphs, bitmaps, out_c, out_h,
        symbol=symbol,
        line_height=line_height,
        base_line=base_line,
        guard_macro=guard,
    )
    total_bmp = sum(len(b) for b in bitmaps)
    print(
        f"wrote {out_c} ({out_c.stat().st_size} bytes source, "
        f"{total_bmp} bitmap bytes, {len(chars)} glyphs, "
        f"line_height={line_height})"
    )
    print(f"wrote {out_h}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
