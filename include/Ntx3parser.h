#pragma once
#include <cstdint>
#include <vector>
#include <glad/glad.h>

// NTX3 format byte (at header offset 0x18):
//   0x86 => DXT1  (no alpha, 8 bytes/block)
//   0xA5 => Unknown / skip (raw non-DXT, not supported)
//   anything else => DXT5  (with alpha, 16 bytes/block)
//
// Pixel data always starts at chunk_offset + 128 (0x80).
// Width and height are always BE u16 at offsets 0x20 and 0x22.
// Mip count is at offset 0x19 (informational, decompressor uses base mip only).

enum class NTX3Format {
    DXT1,    // b18 = 0x86
    DXT5,    // b18 = 0x88, 0xA6, 0xA8, etc.
    Unknown  // b18 = 0xA5 or unrecognized — skip this texture
};

struct NTX3TextureInfo {
    uint16_t width;
    uint16_t height;
    uint8_t  b18;           // raw format byte, for diagnostics
    uint8_t  mip_count;
    NTX3Format format;
    size_t pixel_data_offset;   // absolute byte offset in the file
    size_t pixel_data_size;     // bytes from pixel_data_offset to end of chunk

    NTX3TextureInfo()
        : width(0), height(0), b18(0), mip_count(0),
        format(NTX3Format::Unknown),
        pixel_data_offset(0), pixel_data_size(0) {
    }
};

class NTX3Parser {
public:
    // Parse NTX3 header at chunk_offset inside file_data.
    static bool ParseHeader(const uint8_t* file_data, size_t chunk_offset,
        size_t file_size, NTX3TextureInfo& out_info);

    // Decompress the base mip to a flat RGBA8 buffer (width * height * 4 bytes).
    // Blocks that are entirely 0xEE fill are written as transparent (alpha=0).
    static bool DecompressToRGBA(const uint8_t* file_data,
        const NTX3TextureInfo& info,
        std::vector<uint8_t>& out_rgba);

    // Create an OpenGL texture from the parsed info.
    static bool CreateGLTexture(const uint8_t* file_data,
        const NTX3TextureInfo& info,
        GLuint& out_texture);

    // All-in-one convenience: parse + create GL texture.
    static bool LoadTexture(const uint8_t* file_data, size_t chunk_offset,
        size_t file_size, GLuint& out_texture,
        uint16_t* out_width = nullptr,
        uint16_t* out_height = nullptr);

public:
    // Called externally for .bmd/.e inline NTX3 (no block-swap — use NTX3Parser,
    // not P3TexParser::DecompressDXT5, for these files).
    static bool DecompressDXT1(const uint8_t* src, uint16_t width,
        uint16_t height, std::vector<uint8_t>& rgba);
    static bool DecompressDXT5(const uint8_t* src, uint16_t width,
        uint16_t height, std::vector<uint8_t>& rgba);

private:
    static uint16_t ReadU16BE(const uint8_t* data);
    static uint32_t ReadU32BE(const uint8_t* data);

    // Returns true if all 16 bytes of a DXT block are 0xEE (fill block).
    static bool IsEEFillBlock(const uint8_t* block);
};