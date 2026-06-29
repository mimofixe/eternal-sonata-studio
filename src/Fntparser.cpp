#include "Fntparser.h"
#include <cstring>
#include <fstream>

// The BC offset table always begins at this fixed offset inside the FONT block.
static const size_t FNT_TABLE_START = 0x20;
// Within a glyph record the metrics occupy 4 bytes at +0x14; the bitmap pixel
// data begins right after, at +0x18.
static const size_t FNT_METRICS_OFFSET = 0x14;
static const size_t FNT_BITMAP_OFFSET = 0x18;

uint16_t FntParser::ReadU16BE(const uint8_t* data) {
    return (uint16_t)((data[0] << 8) | data[1]);
}

uint32_t FntParser::ReadU32BE(const uint8_t* data) {
    return ((uint32_t)data[0] << 24) | ((uint32_t)data[1] << 16) |
        ((uint32_t)data[2] << 8) | (uint32_t)data[3];
}

const FntGlyph* FntFont::FindGlyph(uint16_t code) const {
    for (const auto& g : glyphs)
        if (g.code == code) return &g;
    return nullptr;
}

size_t FntParser::FindFontBlock(const uint8_t* file_data, size_t file_size) {
    if (!file_data || file_size < 4) return SIZE_MAX;
    for (size_t p = 0; p + 4 <= file_size; p++) {
        if (file_data[p] == 'F' && file_data[p + 1] == 'O' &&
            file_data[p + 2] == 'N' && file_data[p + 3] == 'T') {
            return p;
        }
    }
    return SIZE_MAX;
}

bool FntParser::Parse(const uint8_t* file_data, size_t file_size,
    size_t font_offset, FntFont& out_font) {
    if (!file_data || font_offset + FNT_TABLE_START + 8 > file_size) return false;

    const uint8_t* base = file_data + font_offset;
    if (std::memcmp(base, "FONT", 4) != 0) return false;

    out_font = FntFont();

    // Size of this FONT block (at +0x04). Bound all reads to it so an embedded
    // block never spills into a neighbouring resource of the container.
    size_t block_size = ReadU32BE(base + 0x04);
    size_t avail = file_size - font_offset;
    if (block_size < FNT_TABLE_START || block_size > avail)
        block_size = avail;

    // cell size at +0x0c (two 16-bit halves, 0x0030 0x0030 = 48 x 48)
    out_font.cell_w = ReadU16BE(base + 0x0C);
    out_font.cell_h = ReadU16BE(base + 0x0E);

    // sub-font tag at +0x14 (e.g. "BC  ")
    out_font.name.assign((const char*)(base + 0x14), 4);

    // The offset table runs from 0x20 up to the first non-zero entry, which is
    // the (block-relative) offset of the first glyph record.
    uint32_t first_glyph = 0;
    size_t max_entries = (block_size - FNT_TABLE_START) / 4;
    for (size_t i = 0; i < max_entries; i++) {
        uint32_t v = ReadU32BE(base + FNT_TABLE_START + i * 4);
        if (v > 0) { first_glyph = v; break; }
    }
    if (first_glyph == 0 || first_glyph > block_size) return false;

    size_t n_entries = (first_glyph - FNT_TABLE_START) / 4;

    // Collect the non-zero offsets in table order. The bitmap of glyph k ends at
    // +0x14 of glyph k+1, so we keep each glyph's successor offset.
    std::vector<std::pair<uint16_t, uint32_t>> entries;  // (code, block-rel offset)
    entries.reserve(n_entries);
    for (size_t i = 0; i < n_entries; i++) {
        uint32_t off = ReadU32BE(base + FNT_TABLE_START + i * 4);
        if (off > 0 && off < block_size)
            entries.push_back({ (uint16_t)i, off });
    }

    for (size_t k = 0; k < entries.size(); k++) {
        uint16_t code = entries[k].first;
        uint32_t go = entries[k].second;
        uint32_t next = (k + 1 < entries.size()) ? entries[k + 1].second
            : (uint32_t)(go + 92);

        if (go + FNT_BITMAP_OFFSET > block_size) continue;

        FntGlyph glyph;
        glyph.code = code;
        glyph.advance_w = base[go + FNT_METRICS_OFFSET + 0];
        glyph.advance_h = base[go + FNT_METRICS_OFFSET + 1];
        glyph.width = base[go + FNT_METRICS_OFFSET + 2];
        glyph.height = base[go + FNT_METRICS_OFFSET + 3];

        // Bitmap runs from this glyph's +0x18 up to the next glyph's +0x14.
        size_t bmp_start = go + FNT_BITMAP_OFFSET;
        size_t bmp_end = (size_t)next + FNT_METRICS_OFFSET;
        if (bmp_end > block_size) bmp_end = block_size;
        if (bmp_end <= bmp_start) continue;

        size_t bmp_len = bmp_end - bmp_start;
        if (glyph.height == 0) continue;
        glyph.stride = (uint8_t)(bmp_len / glyph.height);
        if (glyph.stride == 0) continue;

        glyph.bitmap.assign(base + bmp_start, base + bmp_end);
        out_font.glyphs.push_back(std::move(glyph));
    }

    return !out_font.glyphs.empty();
}

bool FntParser::ParseFile(const std::string& path, FntFont& out_font) {
    std::ifstream f(path, std::ios::binary | std::ios::ate);
    if (!f) return false;
    std::streamsize size = f.tellg();
    if (size <= 0) return false;
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> buf((size_t)size);
    if (!f.read((char*)buf.data(), size)) return false;

    // Standalone .fnt starts with 'FONT'. Otherwise scan the container for the
    // first embedded FONT block (covers BMD and any other file type).
    size_t off = 0;
    if (buf.size() < 4 ||
        std::memcmp(buf.data(), "FONT", 4) != 0) {
        off = FindFontBlock(buf.data(), buf.size());
        if (off == SIZE_MAX) return false;
    }
    return Parse(buf.data(), buf.size(), off, out_font);
}

void FntParser::GlyphToAlpha(const FntGlyph& glyph, std::vector<uint8_t>& out_alpha) {
    out_alpha.assign((size_t)glyph.width * glyph.height, 0);
    if (glyph.width == 0 || glyph.height == 0 || glyph.stride == 0) return;

    // 2 bits per pixel, MSB-first, 4 pixels per byte. Scale 0..3 to 0..255.
    static const uint8_t kLevel[4] = { 0, 85, 170, 255 };
    for (int row = 0; row < glyph.height; row++) {
        for (int col = 0; col < glyph.width; col++) {
            int bit = col * 2;
            size_t bi = (size_t)row * glyph.stride + bit / 8;
            int shift = 6 - (bit % 8);
            uint8_t v = 0;
            if (bi < glyph.bitmap.size())
                v = (glyph.bitmap[bi] >> shift) & 3;
            out_alpha[(size_t)row * glyph.width + col] = kLevel[v];
        }
    }
}