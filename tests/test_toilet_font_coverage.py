#!/usr/bin/env python3
"""Fail if hardcoded UI/article Chinese is missing from the toilet font inventory."""
from __future__ import annotations

import re
import sys
from pathlib import Path

ROOT = Path(__file__).resolve().parents[1]
INV = ROOT / "assets/fonts/toilet_font_inventory.txt"
SOURCES = sorted((ROOT / "main").glob("*.c")) + sorted((ROOT / "main").glob("*.h"))


def decode_c_string(raw: str) -> str:
    return (
        raw.replace("\\\\", "\0")
        .replace("\\n", "\n")
        .replace("\\t", "\t")
        .replace('\\"', '"')
        .replace("\\'", "'")
        .replace("\0", "\\")
    )


def main() -> int:
    inv = set(INV.read_text(encoding="utf-8"))
    missing: dict[str, set[str]] = {}
    for path in SOURCES:
        src = path.read_text(encoding="utf-8", errors="replace")
        for m in re.finditer(r'"((?:[^"\\]|\\.)*)"', src):
            text = decode_c_string(m.group(1))
            for ch in text:
                cp = ord(ch)
                if cp < 0x80:
                    continue  # ASCII covered by FORMAT0_TINY + Montserrat fallback
                if ch not in inv:
                    missing.setdefault(path.name, set()).add(ch)
    if missing:
        print("Toilet font inventory missing glyphs for hardcoded strings:", file=sys.stderr)
        for name, chars in sorted(missing.items()):
            print(f"  {name}: {''.join(sorted(chars))}", file=sys.stderr)
        return 1
    print(f"Toilet font coverage: PASS ({len(inv)} inventory code points, {len(SOURCES)} sources)")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
