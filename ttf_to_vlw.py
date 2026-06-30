#!/usr/bin/env python3
"""
Convert a TTF font to TFT_eSPI .vlw format + C header (.h) for PROGMEM.
Uses freetype-py. Generates anti-aliased (8-bit alpha) glyphs.

Usage:
  python3 ttf_to_vlw.py input.ttf output_basename point_size [point_size ...]
Example:
  python3 ttf_to_vlw.py AlbertSans.ttf AlbertSans 14 28
Produces:
  AlbertSans14.vlw  AlbertSans14.h
  AlbertSans28.vlw  AlbertSans28.h
"""

import sys
import struct
import freetype


def generate_font(ttf_path, base_name, point_size, char_range=range(0x20, 0x7F)):
    face = freetype.Face(ttf_path)
    # point_size in 26.6 fixed point (points * 64); 96 DPI
    face.set_char_size(point_size * 64, point_size * 64, 96, 96)

    # Get font metrics (ascender/descender in 26.6 fixed point)
    ascent = face.size.ascender >> 6
    raw_desc = face.size.descender >> 6
    descent = abs(raw_desc) if raw_desc < 0 else raw_desc

    glyphs = []
    for cp in char_range:
        if not face.get_char_index(cp):
            continue  # skip chars not in font
        face.load_glyph(face.get_char_index(cp), freetype.FT_LOAD_RENDER)
        slot = face.glyph
        bm = slot.bitmap

        width = bm.width
        height = bm.rows
        pitch = bm.pitch
        # xAdvance in pixels (26.6 fixed point >> 6)
        x_advance = slot.advance.x >> 6

        # gdX: left edge of bitmap relative to cursor (bitmap_left)
        gdx = slot.bitmap_left
        # gdY: top edge of bitmap relative to baseline (bitmap_top, + = up)
        gdy = slot.bitmap_top

        # Extract 8-bit alpha bitmap, row-major, no padding
        bitmap_data = bytearray(width * height)
        if bm.buffer and width > 0 and height > 0:
            for y in range(height):
                src = y * abs(pitch)
                dst = y * width
                for x in range(width):
                    bitmap_data[dst + x] = bm.buffer[src + x]

        glyphs.append(
            {
                "unicode": cp,
                "width": width,
                "height": height,
                "xAdvance": x_advance,
                "dY": gdy,
                "dX": gdx,
                "bitmap": bytes(bitmap_data),
            }
        )

    g_count = len(glyphs)

    # Build .vlw binary
    vlw = bytearray()

    # Header: 6 x uint32 big-endian
    vlw += struct.pack(">6I", g_count, 11, point_size, 0, ascent, descent)

    # Glyph table: gCount x 7 x int32 big-endian
    for g in glyphs:
        vlw += struct.pack(
            ">7i",
            g["unicode"],
            g["height"],
            g["width"],
            g["xAdvance"],
            g["dY"],
            g["dX"],
            0,
        )  # padding

    # Bitmap data
    for g in glyphs:
        vlw += g["bitmap"]

    # Trailer (minimal)
    name_bytes = base_name.encode("ascii")[:31]
    vlw += bytes([len(name_bytes)])
    vlw += name_bytes
    vlw += bytes([0])  # psname length = 0
    vlw += bytes([1])  # smooth = 1

    # Write .vlw file
    vlw_path = f"{base_name}{point_size}.vlw"
    with open(vlw_path, "wb") as f:
        f.write(vlw)
    print(
        f"  {vlw_path}: {len(vlw)} bytes, {g_count} glyphs, "
        f"ascent={ascent} descent={descent}"
    )

    # Write .h header file (PROGMEM array)
    h_path = f"{base_name}{point_size}.h"
    var_name = f"{base_name}{point_size}"
    with open(h_path, "w") as f:
        f.write(f"// Auto-generated from {ttf_path} at {point_size}pt\n")
        f.write(f"// {len(vlw)} bytes, {g_count} glyphs\n\n")
        f.write("#pragma once\n")
        f.write("#include <pgmspace.h>\n\n")
        f.write(f"const uint8_t {var_name}[] PROGMEM = {{\n")
        for i in range(0, len(vlw), 16):
            chunk = vlw[i : i + 16]
            hex_vals = ", ".join(f"0x{b:02X}" for b in chunk)
            if i + 16 < len(vlw):
                f.write(f"  {hex_vals},\n")
            else:
                f.write(f"  {hex_vals}\n")
        f.write("};\n")
    print(f"  {h_path}: {len(vlw)} bytes as PROGMEM array")

    return vlw_path, h_path


def main():
    if len(sys.argv) < 4:
        print(__doc__)
        sys.exit(1)

    ttf_path = sys.argv[1]
    base_name = sys.argv[2]
    sizes = [int(s) for s in sys.argv[3:]]

    print(f"Font: {ttf_path}")
    print(f"Base: {base_name}")
    print(f"Sizes: {sizes}\n")

    for size in sizes:
        print(f"Generating {base_name} at {size}pt:")
        generate_font(ttf_path, base_name, size)
        print()

    print("Done!")


if __name__ == "__main__":
    main()
