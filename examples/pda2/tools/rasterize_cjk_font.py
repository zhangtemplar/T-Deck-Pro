#!/usr/bin/env python3
"""
rasterize_cjk_font.py — offline CJK font rasterizer for T-Deck-Pro.

Runtime TTF rasterization on the ESP32-S3 is far too slow for CJK (~1-2 s per
glyph, minutes for a screen), even with the whole font cached in PSRAM — the
bottleneck is stb_truetype rendering, not I/O. This script pre-rasterizes the
glyphs ONCE, offline, at a single pixel size and dumps them as a flat binary
blob. The firmware loads that blob straight into PSRAM and blits pre-rendered
1-bpp bitmaps with zero rasterization at runtime (instant).

Output format (little-endian) — consumed by cjk_font.cpp:

  Header (36 bytes, packed):
    char     magic[4]        "CJK1"
    uint16   version         1
    uint8    bpp             1
    uint8    flags           0
    int16    px_size         nominal pixel size (e.g. 16)
    int16    line_height     font line height in px
    int16    base_line       baseline from top of line, in px
    uint16   reserved
    uint32   glyph_count
    uint32   codepoints_off  byte offset of the codepoint table
    uint32   glyphs_off      byte offset of the glyph-descriptor table
    uint32   bitmap_off      byte offset of the bitmap blob
    uint32   bitmap_size     size of the bitmap blob in bytes

  Codepoint table: uint32 * glyph_count, sorted ascending (binary-searchable).

  Glyph-descriptor table (parallel to codepoints), 10 bytes each, packed:
    uint32   bmp_off         offset into the bitmap blob
    uint16   adv_w           advance width, whole px
    uint8    box_w           bitmap width  in px
    uint8    box_h           bitmap height in px
    int8     ofs_x           left bearing  (= FreeType bitmap_left)
    int8     ofs_y           bottom offset from baseline (= bitmap_top - box_h)

  Bitmap blob: each glyph's bitmap, 1 bpp, MSB-first, box_w bits per row, rows
  concatenated with NO per-row padding, each glyph starting on a byte boundary.
  This matches LVGL's software letter renderer exactly (bitmask_init = 0x80,
  width_bit = box_w * bpp, rows continuous — see lv_draw_sw_letter.c).

Requires FreeType via freetype-py:   pip install freetype-py

Example:
  python3 rasterize_cjk_font.py --font dict_font.ttf --size 16 --out cjk_16.bin
  # then copy cjk_16.bin to the SD card as /fonts/cjk_16.bin
"""

import argparse
import struct
import sys

try:
    import freetype
except ImportError:
    sys.exit("error: freetype-py not installed. Run:  pip install freetype-py")

MAGIC = b"CJK1"
VERSION = 1

# Default Unicode ranges to cover (inclusive). Kept modest so the blob stays
# ~1 MB at 16px; add --ext-a for the rarer CJK Extension A block.
DEFAULT_RANGES = [
    (0x2000, 0x206F),   # general punctuation (smart quotes, dashes, ellipsis)
    (0x3000, 0x303F),   # CJK symbols and punctuation
    (0x3040, 0x30FF),   # Hiragana + Katakana
    (0x4E00, 0x9FFF),   # CJK unified ideographs (the bulk)
    (0xFF00, 0xFFEF),   # halfwidth and fullwidth forms
]
EXT_A_RANGE = (0x3400, 0x4DBF)  # CJK Extension A (opt-in via --ext-a)


def parse_ranges(spec):
    """Parse '0x4E00-0x9FFF,0x3000-0x303F' into a list of (lo, hi) tuples."""
    out = []
    for part in spec.split(","):
        part = part.strip()
        if not part:
            continue
        lo, hi = part.split("-")
        out.append((int(lo, 0), int(hi, 0)))
    return out


def collect_codepoints(args):
    """Build the set of codepoints to rasterize."""
    cps = set()
    if args.charset_file:
        with open(args.charset_file, encoding="utf-8") as fh:
            for ch in fh.read():
                if ord(ch) >= 0x80:      # skip ASCII (handled by the base font)
                    cps.add(ord(ch))
    else:
        ranges = parse_ranges(args.ranges) if args.ranges else list(DEFAULT_RANGES)
        if args.ext_a:
            ranges.append(EXT_A_RANGE)
        for lo, hi in ranges:
            cps.update(range(lo, hi + 1))
    return sorted(cps)


def pack_glyph_bitmap(bmp):
    """Repack a FreeType MONO bitmap into a continuous, no-padding, MSB-first
    1-bpp bitstream (LVGL's expected layout)."""
    w, h, pitch, buf = bmp.width, bmp.rows, bmp.pitch, bmp.buffer
    if w == 0 or h == 0:
        return b""
    nbits = w * h
    out = bytearray((nbits + 7) // 8)
    bit = 0
    for row in range(h):
        base = row * pitch
        for col in range(w):
            src = buf[base + (col >> 3)]
            if src & (0x80 >> (col & 7)):        # ink pixel
                out[bit >> 3] |= 0x80 >> (bit & 7)
            bit += 1
    return bytes(out)


def main():
    ap = argparse.ArgumentParser(description="Offline CJK font rasterizer for T-Deck-Pro")
    ap.add_argument("--font", required=True, help="input TTF/OTF path")
    ap.add_argument("--size", type=int, default=16, help="pixel size (default 16)")
    ap.add_argument("--out", required=True, help="output .bin path")
    ap.add_argument("--ranges", help="comma list like 0x4E00-0x9FFF,0x3000-0x303F "
                                     "(default: common CJK + kana + punctuation)")
    ap.add_argument("--ext-a", action="store_true", help="also include CJK Extension A (large)")
    ap.add_argument("--charset-file", help="UTF-8 text file; only rasterize the "
                                           "characters it contains (smallest blob)")
    args = ap.parse_args()

    face = freetype.Face(args.font)
    face.set_pixel_sizes(0, args.size)

    m = face.size
    line_height = m.height >> 6
    base_line = -(m.descender >> 6)
    if line_height <= 0:
        line_height = (m.ascender - m.descender) >> 6
    print(f"size={args.size}px line_height={line_height} base_line={base_line}")

    want = collect_codepoints(args)
    print(f"candidate codepoints: {len(want)}")

    LOAD_FLAGS = freetype.FT_LOAD_RENDER | freetype.FT_LOAD_TARGET_MONO

    glyphs = []          # (cp, adv, box_w, box_h, ofs_x, ofs_y, bitmap_bytes)
    bitmap_blob = bytearray()
    missing = 0
    for cp in want:
        if face.get_char_index(cp) == 0:
            missing += 1
            continue
        face.load_char(cp, LOAD_FLAGS)
        g = face.glyph
        bmp = g.bitmap
        box_w, box_h = bmp.width, bmp.rows
        adv = (g.advance.x + 32) >> 6
        ofs_x = g.bitmap_left
        ofs_y = g.bitmap_top - box_h

        # Clamp to the field widths; warn if a glyph won't fit (shouldn't at 16px).
        if not (0 <= box_w <= 255 and 0 <= box_h <= 255):
            print(f"  skip U+{cp:04X}: box {box_w}x{box_h} exceeds 255", file=sys.stderr)
            continue
        if not (-128 <= ofs_x <= 127 and -128 <= ofs_y <= 127):
            print(f"  skip U+{cp:04X}: offset ({ofs_x},{ofs_y}) out of int8", file=sys.stderr)
            continue
        adv = max(0, min(adv, 0xFFFF))

        packed = pack_glyph_bitmap(bmp)
        bmp_off = len(bitmap_blob)
        bitmap_blob.extend(packed)
        glyphs.append((cp, adv, box_w, box_h, ofs_x, ofs_y, bmp_off))

    glyphs.sort(key=lambda t: t[0])
    n = len(glyphs)
    print(f"rasterized {n} glyphs ({missing} codepoints absent from the font)")

    # Assemble the file. Layout: header | codepoints | glyph descs | bitmaps.
    HDR = 36
    cp_off = HDR
    gl_off = cp_off + 4 * n
    bm_off = gl_off + 10 * n
    bm_size = len(bitmap_blob)

    header = struct.pack(
        "<4sHBBhhhHIIIII",
        MAGIC, VERSION, 1, 0,
        args.size, line_height, base_line, 0,
        n, cp_off, gl_off, bm_off, bm_size,
    )
    assert len(header) == HDR, len(header)

    cp_table = b"".join(struct.pack("<I", g[0]) for g in glyphs)
    gl_table = b"".join(
        struct.pack("<IHBBbb", g[6], g[1], g[2], g[3], g[4], g[5]) for g in glyphs
    )

    with open(args.out, "wb") as fh:
        fh.write(header)
        fh.write(cp_table)
        fh.write(gl_table)
        fh.write(bitmap_blob)

    total = HDR + len(cp_table) + len(gl_table) + bm_size
    print(f"wrote {args.out}: {total} bytes "
          f"(index {HDR + len(cp_table) + len(gl_table)} B, bitmaps {bm_size} B)")
    print(f"copy it to the SD card as /fonts/cjk_{args.size}.bin")


if __name__ == "__main__":
    main()
