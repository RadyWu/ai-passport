<p align="right">
  <strong>简体中文</strong> · <a href="README.md">English</a>
</p>

# 字体资源

可供应用复用的 LVGL 字库源文件。

## 解闷阅读字体

- `font_toilet_14.c` / `font_toilet_14.h`：14px 状态栏/提示用子集（约 6600 码点（一级+二级字表））。
- `font_toilet_20.c` / `font_toilet_20.h`：20px 正文（同清单；PLAIN **bpp2**，约 10 汉字/行；bpp4 会超过 LVGL 20-bit `bitmap_index`）。
- `toilet_font_inventory.txt`：用于重新生成子集的字形清单（也是 `tests/test_toilet_font_coverage.py` 的门禁输入）。
- 由 **Source Han Sans SC Normal**（SIL Open Font License；仓库内 LVGL 组件自带副本）栅格化。请**不要**把其它完整源 TTF/OTF 提交进仓库。
- 生成器：`tools/fonts/gen_toilet_font.py`（Pillow/FreeType，不用 `lv_font_conv`，避免新 Node.js 下坏位图）。示例：

```bash
python3 -m venv /tmp/fontvenv && /tmp/fontvenv/bin/pip install pillow
/tmp/fontvenv/bin/python tools/fonts/gen_toilet_font.py --size 20 --bpp 2 \
  --font managed_components/lvgl__lvgl/scripts/built_in_font/SourceHanSansSC-Normal.otf
```

任意 LLM 长文仍可能落到一级字表之外；缺字时 LVGL 会显示占位方框。
