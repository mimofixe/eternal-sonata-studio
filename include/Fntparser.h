#pragma once
#include <cstdint>
#include <vector>
#include <string>

// FNT bitmap font format (Eternal Sonata / Trusty Bell). Big-endian.
//
// A FONT block can live in its own .fnt file or be embedded inside another
// container (e.g. a BMD). Layout of the block:
//
//   0x00  'FONT'                 magic
//   0x04  uint32                 block size (used to bound an embedded block)
//   0x08  uint32  0x010x0200     version
//   0x0c  uint32  0x00300030     cell size (48 x 48)
//   0x10  byte[4] 40 80 c0 00    anti-alias gamma levels (64,128,192)
//   0x14  'BC  '                 sub-font tag (Basic Characters)
//   0x18  uint32                 offset to end of BC glyph data
//   0x1c  uint32  0              reserved
//   0x20  uint32[N]              BC offset table, indexed by character code.
//                                A zero entry means the slot is empty. The first
//                                non-zero entry marks the start of glyph data,
//                                which also gives N = (first - 0x20) / 4.
//
// BC GLYPH RECORD (pointed to by the offset table, offset is relative to the
// start of the FONT block):
//   The 4 metric bytes sit INSIDE the bitmap stream at +0x14:
//     +0x14  byte[4]  [advance_w][advance_h][width_px][height_rows]
//   The bitmap pixel data runs from +0x18 of this glyph up to +0x14 of the next
//   non-empty glyph. Pixels are 2 bits per pixel (4 anti-alias levels), packed
//   MSB-first, 4 pixels per byte.
//     height = metric[3] (rows)
//     stride = bitmap_len / height
//     visible width = metric[2] px
//
// The slot index equals the character's CP437/ASCII code, so slot 'A' (0x41)
// holds the glyph for 'A'. This viewer only decodes the BC sub-font (Latin
// letters, digits, symbols); the optional MBC Japanese section is ignored.

struct FntGlyph {
    uint16_t code;       // slot index = character code (CP437/ASCII)
    uint8_t  advance_w;  // metric[0]
    uint8_t  advance_h;  // metric[1]
    uint8_t  width;      // metric[2] - visible width in pixels
    uint8_t  height;     // metric[3] - height in rows
    uint8_t  stride;     // bytes per pixel row (bitmap_len / height)
    std::vector<uint8_t> bitmap;  // raw 2bpp rows (stride * height bytes)

    FntGlyph()
        : code(0), advance_w(0), advance_h(0),
        width(0), height(0), stride(0) {
    }
};

struct FntFont {
    uint16_t cell_w;     // header +0x0c high half (48)
    uint16_t cell_h;     // header +0x0c low half  (48)
    std::string name;    // sub-font tag, e.g. "BC  "
    std::vector<FntGlyph> glyphs;   // decoded BC glyphs (non-empty slots)

    FntFont() : cell_w(0), cell_h(0) {}

    // Find a glyph by its character code (ASCII). Returns nullptr if absent.
    const FntGlyph* FindGlyph(uint16_t code) const;
};

class FntParser {
public:
    // Parse a FONT block already in memory. font_offset is where the 'FONT'
    // magic sits inside file_data (0 for a standalone .fnt). Returns false if
    // there is no valid FONT block at that offset.
    static bool Parse(const uint8_t* file_data, size_t file_size,
        size_t font_offset, FntFont& out_font);

    // Read a file from disk and parse it. If the file does not start with the
    // 'FONT' magic, the whole file is scanned for an embedded FONT block (so a
    // BMD or any other container that carries a font can be opened directly).
    static bool ParseFile(const std::string& path, FntFont& out_font);

    // Scan a buffer for the first 'FONT' magic and return its offset, or
    // SIZE_MAX if none is found.
    static size_t FindFontBlock(const uint8_t* file_data, size_t file_size);

    // Expand one glyph's 2bpp bitmap to an 8-bit coverage buffer
    // (width * height bytes, 0..255, row-major). Levels map to 0/85/170/255.
    static void GlyphToAlpha(const FntGlyph& glyph, std::vector<uint8_t>& out_alpha);

private:
    static uint16_t ReadU16BE(const uint8_t* data);
    static uint32_t ReadU32BE(const uint8_t* data);
};