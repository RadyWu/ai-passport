<p align="right">
  <a href="README.zh_CN.md">简体中文</a> · <strong>English</strong>
</p>

# Font assets

Reusable LVGL font sources for applications.

## Bathroom short-read font

- `font_toilet_14.c` / `font_toilet_14.h`: LVGL 9 PLAIN bpp4 subset (~6600 code points (level-1+2)) covering ASCII, common Chinese punctuation, UI chrome, and Table of General Standard Chinese Characters level-1 (3500).
- `font_toilet_20.c` / `font_toilet_20.h`: 20px body (same inventory; PLAIN bpp2; ~10 CJK glyphs/line).
- `toilet_font_inventory.txt`: glyph inventory used to regenerate the subset (also the input for `tests/test_toilet_font_coverage.py`).
- Rasterized from **Source Han Sans SC Normal** (SIL Open Font License; copy shipped with the LVGL component). Do **not** commit other full source TTF/OTF files.
- Generator: `tools/fonts/gen_toilet_font.py` (Pillow/FreeType; avoids `lv_font_conv` corrupt bitmaps on modern Node.js). Example:

```bash
python3 -m venv /tmp/fontvenv && /tmp/fontvenv/bin/pip install pillow
/tmp/fontvenv/bin/python tools/fonts/gen_toilet_font.py --size 20 --bpp 2 \
  --font managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf
```

Arbitrary LLM text may still hit characters outside level-1; LVGL will show placeholder boxes for those.
