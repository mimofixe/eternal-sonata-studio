#include "Ntx2parser.h"
#include <cstring>
#include <algorithm>
#include <iostream>

// bcdec is already available via P3TexParser.cpp (BCDEC_IMPLEMENTATION defined
// there).  We only need the declarations here.
#include "../libs/bcdec/bcdec.h"

// Endian helpers

uint16_t Ntx2Parser::ReadU16BE(const uint8_t* d) {
    return static_cast<uint16_t>((d[0] << 8) | d[1]);
}
uint32_t Ntx2Parser::ReadU32BE(const uint8_t* d) {
    return (static_cast<uint32_t>(d[0]) << 24) |
        (static_cast<uint32_t>(d[1]) << 16) |
        (static_cast<uint32_t>(d[2]) << 8) |
        static_cast<uint32_t>(d[3]);
}

// ParseHeader

bool Ntx2Parser::ParseHeader(const uint8_t* data, size_t size, NTX2Info& out,
    size_t chunk_abs_offset) {
    if (size < 0x80) return false;
    if (std::memcmp(data, "NTX2", 4) != 0) return false;
    if (std::memcmp(data + 0x08, "XPR2", 4) != 0) return false;
    if (std::memcmp(data + 0x18, "TX2D", 4) != 0) return false;

    // Basic chunk dimensions
    const uint32_t hdr_sz = ReadU32BE(data + 0x10); // pixel data end / tail start
    if (hdr_sz < 0x1000 || hdr_sz >= size) return false;

    // Pixel data is at the first 4 KB-aligned absolute address >= chunk_start + 0x1000.
    // For aligned chunks this equals chunk + 0x1000 (same as before).
    // For misaligned chunks (chunk_abs_offset & 0xFFF != 0) the correct offset is
    // larger, and using 0x1000 would land in zero-padding → noise.
    out.pix_offset = static_cast<uint32_t>(
        ((chunk_abs_offset + 0x1FFFu) & ~size_t{ 0xFFF }) - chunk_abs_offset);
    if (out.pix_offset >= hdr_sz) return false;
    out.pix_size = hdr_sz - out.pix_offset;
    if (out.pix_size == 0) return false;

    // Name
    // TX2D descriptor: "TX2D" at +0x18, name follows at +0x2C (max 32 bytes)
    {
        const uint8_t* name_ptr = data + 0x2C;
        size_t name_max = std::min(static_cast<size_t>(32),
            size - 0x2C);
        size_t name_len = 0;
        while (name_len < name_max && name_ptr[name_len] != 0) ++name_len;
        out.name.assign(reinterpret_cast<const char*>(name_ptr), name_len);
    }

    // GPUTEXTURE_FETCH_CONSTANT
    // fc_base = NTX2 + 0x18 + tx2d_name_sz + 0x18
    const uint32_t tx2d_name_sz = ReadU32BE(data + 0x1C);
    const uint32_t fc_off = 0x18 + tx2d_name_sz + 0x18;
    if (fc_off + 12 > size) return false;

    // W1 – bits [5:0] = Xenos TextureFormat
    // W2 – bits [12:0] = width-1, bits [25:13] = height-1
    // The fetch-constant dwords are stored big-endian in the NTX2 file,
    // consistent with the rest of the Namco header.
    const uint8_t* fc = data + fc_off;
    const uint32_t w0_be = ReadU32BE(fc);
    const uint32_t w1_be = ReadU32BE(fc + 4);
    const uint32_t w2_be = ReadU32BE(fc + 8);

    const uint32_t fmt_id = w1_be & 0x3F;
    out.width = static_cast<int>((w2_be & 0x1FFF) + 1);
    out.height = static_cast<int>(((w2_be >> 13) & 0x1FFF) + 1);
    out.tiled = ((w0_be >> 31) & 1) != 0;

    // W0 bits[30:22] = row_pitch_pixels >> 5
    // pitch_blocks   = (pitch_pixels / 4)   (DXT block covers 4 pixels)
    // pitch_macro_tiles = pitch_blocks / 32
    // Combined: pitch_macro_tiles = W0_pitch_field / 4
    // This is correct even for narrow textures: Xbox 360 pads to 32-block
    // boundaries, so a 64px-wide DXT1 gets pitch=128px → pmt=1, not 0.
    {
        const uint32_t pitch_field = (w0_be >> 22) & 0x1FF; // units: px >> 5
        const uint32_t pitch_pixels = pitch_field * 32;
        const uint32_t pitch_blocks = pitch_pixels / 4;       // DXT block = 4px
        out.pitch_macro_tiles = static_cast<int>(std::max(1u, pitch_blocks / 32u));
    }

    switch (fmt_id) {
    case 18: out.format = XeTextureFormat::k_DXT1;   break;
    case 19: out.format = XeTextureFormat::k_DXT2_3; break;
    case 20: out.format = XeTextureFormat::k_DXT4_5; break;
    case  6: out.format = XeTextureFormat::k_8_8_8_8; break;
    default:
        // Unknown / unsupported format for this game
        std::cerr << "[NTX2] Unknown format " << fmt_id
            << " in \"" << out.name << "\"\n";
        return false;
    }

    if (out.width <= 0 || out.height <= 0 ||
        out.width > 8192 || out.height > 8192) {
        return false;
    }

    return true;
}

// Xenia tiling address functions  (XenosTextureTiledAddress2D)
// Source: xenia/gpu/texture_util.cc / texture_address.xesli

int Ntx2Parser::XenosCombine(int oib, int bank, int pipe, int y_lsb) {
    return (y_lsb << 4) |
        (pipe << 6) |
        (bank << 11) |
        (oib & 0xF) |
        (((oib >> 4) & 1) << 5) |
        (((oib >> 5) & 7) << 8) |
        ((oib >> 8) << 12);
}

int Ntx2Parser::XenosTiledAddr2D(int bx, int by,
    int pitch_macro_tiles, int bpp_log2) {
    const int outer_blocks = (((by >> 5) * pitch_macro_tiles + (bx >> 5)) << 6);
    const int inner_blocks = (((by >> 1) & 7) << 3) | (bx & 7);
    const int oib = (outer_blocks | inner_blocks) << bpp_log2;
    const int bank = (by >> 4) & 1;
    const int pipe = ((bx >> 3) & 3) ^ (((by >> 3) & 1) << 1);
    return XenosCombine(oib, bank, pipe, by & 1);
}

// UntileAndDecodeDXT1

std::vector<uint8_t> Ntx2Parser::UntileAndDecodeDXT1(
    const uint8_t* tiled_data, size_t tiled_size,
    int width, int height, int pitch_macro_tiles) {

    const int width_blocks = width / 4;
    const int height_blocks = height / 4;

    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4, 0);

    for (int by = 0; by < height_blocks; ++by) {
        for (int bx = 0; bx < width_blocks; ++bx) {
            const int byte_off = XenosTiledAddr2D(bx, by, pitch_macro_tiles, 3);
            if (byte_off < 0 ||
                static_cast<size_t>(byte_off) + 8 > tiled_size) {
                continue;
            }

            // DXT1 block: 8 bytes
            //   bytes 0-1: c0 endpoint (stored BE → swap to LE for bcdec)
            //   bytes 2-3: c1 endpoint (stored BE → swap to LE for bcdec)
            //   bytes 4-7: 2-bit index table (LE, already correct)
            const uint8_t* src = tiled_data + byte_off;
            uint8_t block[8];
            block[0] = src[1]; block[1] = src[0];   // c0 BE→LE
            block[2] = src[3]; block[3] = src[2];   // c1 BE→LE
            block[4] = src[4]; block[5] = src[5];   // lookup table LE (as-is)
            block[6] = src[6]; block[7] = src[7];

            uint8_t decoded[64];  // 4×4 RGBA8 block
            bcdec_bc1(block, decoded, 16);  // bcdec stride = 4*4 bytes = 16

            for (int py = 0; py < 4; ++py) {
                for (int px = 0; px < 4; ++px) {
                    const int x = bx * 4 + px;
                    const int y = by * 4 + py;
                    if (x >= width || y >= height) continue;
                    const int src_i = (py * 4 + px) * 4;
                    const int dst_i = (y * width + x) * 4;
                    rgba[dst_i + 0] = decoded[src_i + 0];
                    rgba[dst_i + 1] = decoded[src_i + 1];
                    rgba[dst_i + 2] = decoded[src_i + 2];
                    rgba[dst_i + 3] = decoded[src_i + 3];
                }
            }
        }
    }

    return rgba;
}

// Decode  (parse + copy slice + decompress)

std::vector<uint8_t> Ntx2Parser::Decode(
    const uint8_t* chunk_data, size_t chunk_size,
    NTX2Info& info_out, size_t chunk_abs_offset) {

    if (!ParseHeader(chunk_data, chunk_size, info_out, chunk_abs_offset)) return {};

    const size_t data_end = info_out.pix_offset +
        static_cast<size_t>(info_out.pix_size);
    if (data_end > chunk_size) return {};

    const uint8_t* tiled = chunk_data + info_out.pix_offset;

    if (info_out.format == XeTextureFormat::k_DXT1) {
        return UntileAndDecodeDXT1(tiled, info_out.pix_size,
            info_out.width, info_out.height,
            info_out.pitch_macro_tiles);
    }

    // For DXT3/DXT5/RGBA8 add decoders here if needed in the future
    std::cerr << "[NTX2] Decode: unsupported format for \""
        << info_out.name << "\"\n";
    return {};
}