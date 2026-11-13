#!/usr/bin/env python3
"""Render the 80×25 VGA bytes captured from the C03 El Torito guest."""

import argparse
import html
import json
from pathlib import Path


PALETTE = (
    "#000", "#00a", "#0a0", "#0aa", "#a00", "#a0a", "#a50", "#aaa",
    "#555", "#55f", "#5f5", "#5ff", "#f55", "#f5f", "#ff5", "#fff",
)


def render(columns: int, rows: int, cells: bytes) -> str:
    if (columns, rows) != (80, 25) or len(cells) != columns * rows * 2:
        raise ValueError("expected a complete 80×25 VGA text frame")
    lines = [
        '<svg xmlns="http://www.w3.org/2000/svg" width="640" height="400" viewBox="0 0 640 400">',
        '<title>Text buffer captured from the C03 QSSD boot test</title>',
        '<rect width="640" height="400" fill="#000"/>',
        '<g font-family="DejaVu Sans Mono, monospace" font-size="13">',
    ]
    for i, (character, attribute) in enumerate(zip(cells[::2], cells[1::2])):
        x, y = (i % columns) * 8, (i // columns) * 16
        background = (attribute >> 4) & 7
        if background:
            lines.append(f'<rect x="{x}" y="{y}" width="8" height="16" fill="{PALETTE[background]}"/>')
        if character > 32:
            glyph = html.escape(bytes((character,)).decode("cp437"))
            lines.append(f'<text x="{x}" y="{y + 13}" fill="{PALETTE[attribute & 15]}">{glyph}</text>')
    lines.extend(("</g>", "</svg>"))
    return "\n".join(lines) + "\n"


def main() -> None:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("evidence", type=Path)
    parser.add_argument("output", type=Path)
    args = parser.parse_args()
    evidence = json.loads(args.evidence.read_text(encoding="utf-8"))
    cells = bytes.fromhex(evidence["vga_cells_hex"])
    args.output.write_text(render(evidence["columns"], evidence["rows"], cells), encoding="utf-8")


if __name__ == "__main__":
    main()
