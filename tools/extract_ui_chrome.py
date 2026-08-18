#!/usr/bin/env python3
"""One-shot helper: extract UI chrome sections from main.cpp."""
from pathlib import Path

root = Path(__file__).resolve().parent.parent / "src" / "app" / "juce"
main_path = root / "main.cpp"
lines = main_path.read_text(encoding="utf-8").splitlines(keepends=True)

ranges = {
    "ui_scale_body": (1156, 1364),
    "ui_fonts_body": (1371, 1528),
    "switch_laf_body": (1531, 1617),
    "about_panel_body": (2808, 2952),
    "ui_layout_body": (4798, 5035),
    "apply_layout_metrics": (5037, 5039),
    "apply_scaled": (5045, 5068),
    "layout_chrome": (5072, 5094),
    "paint_body": (5097, 5142),
    "mgstc_laf_body": (7000, 7529),
}
for name, (start, end) in ranges.items():
    text = "".join(lines[start - 1 : end])
    (root / f"_extract_{name}.txt").write_text(text, encoding="utf-8")
    print(f"{name}: {end - start + 1} lines")
