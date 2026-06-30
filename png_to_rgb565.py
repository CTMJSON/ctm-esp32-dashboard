#!/usr/bin/env python3
"""Convert a PNG to an RGB565 C header for TFT_eSPI pushImage()."""

import sys, struct

try:
    from PIL import Image
except ImportError:
    import subprocess

    subprocess.check_call([sys.executable, "-m", "pip", "install", "Pillow", "--quiet"])
    from PIL import Image


def rgb888_to_rgb565(r, g, b):
    return ((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3)


def main():
    if len(sys.argv) < 4:
        print("Usage: png_to_rgb565.py input.png output.h array_name")
        sys.exit(1)

    png_path = sys.argv[1]
    out_path = sys.argv[2]
    var_name = sys.argv[3]

    img = Image.open(png_path).convert("RGBA")
    w, h = img.size
    pixels = img.load()

    # Collect non-transparent pixels; use alpha to build a 1bpp mask + RGB565
    # For simplicity, output full RGB565 array (transparent = 0x0001 sentinel)
    # TFT_eSPI pushImage with alpha is complex; simpler: pre-blend onto a
    # transparent-aware format. We'll output RGB565 where alpha>128, else
    # a special transparent marker we skip in draw code.

    lines = []
    lines.append(f"// Auto-generated from {png_path}")
    lines.append(f"// {w}x{h} RGB565, transparent pixels = 0x0000")
    lines.append(f"#pragma once")
    lines.append(f"#include <pgmspace.h>")
    lines.append(f"")
    lines.append(f"#define {var_name.upper()}_W {w}")
    lines.append(f"#define {var_name.upper()}_H {h}")
    lines.append(f"")
    lines.append(f"const uint16_t {var_name}[] PROGMEM = {{")

    row_vals = []
    for y in range(h):
        row = []
        for x in range(w):
            r, g, b, a = pixels[x, y]
            if a < 128:
                row.append(0x0000)  # transparent marker
            else:
                row.append(rgb888_to_rgb565(r, g, b))
        row_vals.append(row)

    for y, row in enumerate(row_vals):
        hex_vals = ", ".join(f"0x{v:04X}" for v in row)
        if y < h - 1:
            lines.append(f"  {hex_vals},")
        else:
            lines.append(f"  {hex_vals}")

    lines.append("};")

    with open(out_path, "w") as f:
        f.write("\n".join(lines) + "\n")

    print(f"  {out_path}: {w}x{h} = {w * h} pixels ({w * h * 2} bytes)")


if __name__ == "__main__":
    main()
