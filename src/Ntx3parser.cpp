#include "NTX3Parser.h"
#include <cstring>
#include "../libs/bcdec/bcdec.h"

// ---------------------------------------------------------------------------
// NTX3 header layout (confirmed by binary analysis):
//
//   0x00-0x03  "NTX3" magic
//   0x04-0x07  chunk total size (BE u32)
//   0x08-0x17  version/flags (unused)
//   0x18       format byte: 0x86=DXT1, 0xA5=unsupported raw, else=DXT5
//   0x19       mip count
//   0x1A       always 0x02
//   0x20-0x21  width  (BE u16)
//   0x22-0x23  height (BE u16)
//   0x58-0x7F  PS3 RSX padding (0x50 bytes)
//   0x80-0x87  8-byte NTX3 sub-header (size/flags, NOT pixel data)
//   0x88+      pixel data — base mip first, smaller mips after
//
// Pixel data ALWAYS starts at chunk_offset + 0x88 (136 bytes), NOT +0x80.
// The 8 bytes at 0x80..0x87 are a sub-header, not DXT blocks.
// P3TexParser already uses 0x88 — this file must match.
// ---------------------------------------------------------------------------

static constexpr size_t NTX3_HEADER_SIZE = 0x88;  // 136 bytes

bool NTX3Parser::ParseHeader(const uint8_t* file_data, size_t chunk_offset,
    size_t file_size, NTX3TextureInfo& out_info) {
    if (chunk_offset + NTX3_HEADER_SIZE > file_size) {
        return false;
    }

    const uint8_t* h = file_data + chunk_offset;

    if (memcmp(h, "NTX3", 4) != 0) {
        return false;
    }

    uint32_t chunk_size = ReadU32BE(h + 4);
    if (chunk_size < NTX3_HEADER_SIZE || chunk_offset + chunk_size > file_size) {
        return false;
    }

    uint8_t b18 = h[0x18];
    uint8_t b19 = h[0x19];

    out_info.b18 = b18;
    out_info.mip_count = b19;
    out_info.width = ReadU16BE(h + 0x20);
    out_info.height = ReadU16BE(h + 0x22);

    if (out_info.width == 0 || out_info.height == 0) {
        return false;
    }

    if ((b18 & 0xDF) == 0x86) {
        out_info.format = NTX3Format::DXT1;
    }
    else if (b18 == 0xA5) {
        out_info.format = NTX3Format::Unknown;
        return false;  // unsupported raw format
    }
    else {
        out_info.format = NTX3Format::DXT5;
    }

    // Pixel data starts at +0x88, not +0x80.
    // Bytes 0x80..0x87 are a NTX3 sub-header, NOT part of the DXT stream.
    out_info.pixel_data_offset = chunk_offset + NTX3_HEADER_SIZE;
    out_info.pixel_data_size = chunk_size - NTX3_HEADER_SIZE;

    return true;
}

// DecompressToRGBA — decompresses only the base mip (first in the data stream).
// Blocks that are entirely 0xEE are made transparent so fill areas show as
// alpha=0 in the viewer instead of the golden-yellow colour.

bool NTX3Parser::DecompressToRGBA(const uint8_t* file_data,
    const NTX3TextureInfo& info,
    std::vector<uint8_t>& out_rgba) {
    if (info.format == NTX3Format::Unknown) {
        return false;
    }

    if (info.width == 0 || info.height == 0) {
        return false;
    }

    out_rgba.resize(static_cast<size_t>(info.width) * info.height * 4, 0);

    const uint8_t* src = file_data + info.pixel_data_offset;

    if (info.format == NTX3Format::DXT1) {
        return DecompressDXT1(src, info.width, info.height, out_rgba);
    }
    else {
        return DecompressDXT5(src, info.width, info.height, out_rgba);
    }
}

// CreateGLTexture
bool NTX3Parser::CreateGLTexture(const uint8_t* file_data,
    const NTX3TextureInfo& info,
    GLuint& out_texture) {
    std::vector<uint8_t> rgba;
    if (!DecompressToRGBA(file_data, info, rgba)) {
        return false;
    }

    glGenTextures(1, &out_texture);
    glBindTexture(GL_TEXTURE_2D, out_texture);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
        info.width, info.height, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    return true;
}

// LoadTexture — convenience wrapper
bool NTX3Parser::LoadTexture(const uint8_t* file_data, size_t chunk_offset,
    size_t file_size, GLuint& out_texture,
    uint16_t* out_width, uint16_t* out_height) {
    NTX3TextureInfo info;
    if (!ParseHeader(file_data, chunk_offset, file_size, info)) {
        return false;
    }

    if (!CreateGLTexture(file_data, info, out_texture)) {
        return false;
    }

    if (out_width)  *out_width = info.width;
    if (out_height) *out_height = info.height;

    return true;
}

// IsEEFillBlock
bool NTX3Parser::IsEEFillBlock(const uint8_t* block) {
    for (int i = 0; i < 16; i++) {
        if (block[i] != 0xEE) return false;
    }
    return true;
}

// Helpers
uint16_t NTX3Parser::ReadU16BE(const uint8_t* data) {
    return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

uint32_t NTX3Parser::ReadU32BE(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
        (static_cast<uint32_t>(data[1]) << 16) |
        (static_cast<uint32_t>(data[2]) << 8) |
        (static_cast<uint32_t>(data[3]));
}

// DecompressDXT1 (BC1)
bool NTX3Parser::DecompressDXT1(const uint8_t* src, uint16_t width,
    uint16_t height, std::vector<uint8_t>& rgba) {
    int bx_count = (width + 3) / 4;
    int by_count = (height + 3) / 4;

    for (int by = 0; by < by_count; by++) {
        for (int bx = 0; bx < bx_count; bx++) {
            // 0xEE fill check (DXT1 block = 8 bytes)
            bool is_fill = true;
            for (int k = 0; k < 8; k++) {
                if (src[k] != 0xEE) { is_fill = false; break; }
            }

            if (!is_fill) {
                uint8_t block_rgba[64];
                bcdec_bc1(src, block_rgba, 16);

                for (int py = 0; py < 4 && (by * 4 + py) < height; py++) {
                    for (int px = 0; px < 4 && (bx * 4 + px) < width; px++) {
                        int dst = ((by * 4 + py) * width + (bx * 4 + px)) * 4;
                        int s = (py * 4 + px) * 4;
                        rgba[dst + 0] = block_rgba[s + 0];
                        rgba[dst + 1] = block_rgba[s + 1];
                        rgba[dst + 2] = block_rgba[s + 2];
                        rgba[dst + 3] = block_rgba[s + 3];
                    }
                }
            }
            // else: leave as RGBA(0,0,0,0) — transparent

            src += 8;
        }
    }

    return true;
}

// DecompressDXT5 (BC3)
bool NTX3Parser::DecompressDXT5(const uint8_t* src, uint16_t width,
    uint16_t height, std::vector<uint8_t>& rgba,
    bool ps3_swap) {
    int bx_count = (width + 3) / 4;
    int by_count = (height + 3) / 4;

    for (int by = 0; by < by_count; by++) {
        for (int bx = 0; bx < bx_count; bx++) {
            // 0xEE fill check (DXT5 block = 16 bytes)
            if (!IsEEFillBlock(src)) {
                uint8_t block_rgba[64];
                if (ps3_swap) {
                    uint8_t swapped[16];
                    std::memcpy(swapped, src + 8, 8);
                    std::memcpy(swapped + 8, src, 8);
                    bcdec_bc3(swapped, block_rgba, 16);
                }
                else {
                    bcdec_bc3(src, block_rgba, 16);
                }

                for (int py = 0; py < 4 && (by * 4 + py) < height; py++) {
                    for (int px = 0; px < 4 && (bx * 4 + px) < width; px++) {
                        int dst = ((by * 4 + py) * width + (bx * 4 + px)) * 4;
                        int s = (py * 4 + px) * 4;
                        rgba[dst + 0] = block_rgba[s + 0];
                        rgba[dst + 1] = block_rgba[s + 1];
                        rgba[dst + 2] = block_rgba[s + 2];
                        rgba[dst + 3] = block_rgba[s + 3];
                    }
                }
            }
            // else: leave as RGBA(0,0,0,0) — transparent

            src += 16;
        }
    }

    return true;
}