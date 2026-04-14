#pragma once
#include <cstdint>
#include <vector>
#include <string>
#include <glad/glad.h>

// EP3 file format (Mefc container — PS3 demo UI screens)
//
// Layout:
//   +0x00  "Mefc" magic
//   +0x04  total file size (BE u32) — may be 0 for some files
//   +0x08  flags/version
//   +0x18  padding (0x50 fill)
//   +0x28  "CK\x0C<count>" — chunk-key block
//            byte 2 = entry size (12 bytes)
//            byte 3 = entry count (e.g. 3)
//          Each 12-byte entry: type[4] + v1[4] + v2[4]
//            TEX0 → v2 = 0x80 (header size per NTX3 frame)
//            TRC0 → v2 = file offset of float transform track
//            EFCT → v2 = file offset of "etbl" effect table
//
// After the CK table: one or more NTX3 blocks.
//
// EP3 NTX3 block (format byte 0xA8 = 0x20|0x88 = swizzled DXT5):
//   +0x00  "NTX3"
//   +0x04  block size (BE u32) — includes header + pixel data
//   +0x08  0x00000001  (count)
//   +0x10  0x00000080  (header size = 128 bytes)
//   +0x14  pixel data size (BE u32)
//   +0x18  format word: 0xA80B0200 (swizzled DXT5, log2-pitch, etc.)
//   +0x20  width  (BE u16)
//   +0x22  height (BE u16)
//   +0x30..+0x7F  padding (0x50 fill)
//   +0x80  pixel data — DXT5 blocks in Morton (Z-order) block order
//          Block order: swizzled using bit-interleaving of block coords.
//          For each output block at (bx, by), source index =
//            spread_bits(bx) | (spread_bits(by) << 1)
//          where spread_bits interleaves the bits of the coordinate
//          into even bit positions.
//
// Frame count  = total NTX3 blocks found sequentially in the file.
// pause_trial* = DXT5 texture atlases (multiple sizes, no swizzle for 256x256,
//                swizzle for 768x256). Includes full mipchain per NTX3.
// controller*  = single 1280x720 swizzled DXT5 frame per NTX3.

struct Ep3Frame {
    uint16_t width = 0;
    uint16_t height = 0;
    uint8_t  fmt = 0;     // raw format byte at NTX3+0x18
    bool     swizzled = false; // bit 0x20 of fmt
    size_t   pixel_offset = 0; // absolute byte offset of DXT5 data
    size_t   pixel_size = 0;
};

struct Ep3File {
    std::vector<Ep3Frame>  frames;
    std::vector<uint8_t>   raw;          // full file bytes
    std::string            filename;
    bool                   valid = false;
};

class Ep3Parser {
public:
    // Load and parse an .ep3 file.
    static bool Load(const std::string& path, Ep3File& out);

    // Decompress frame to flat RGBA8 (width*height*4 bytes).
    // Handles both swizzled (Morton block order) and linear DXT5.
    static bool DecompressFrame(const Ep3File& ep3, size_t frame_idx,
        std::vector<uint8_t>& out_rgba);

    // Create an OpenGL texture from a decompressed frame.
    static bool FrameToGLTexture(const Ep3File& ep3, size_t frame_idx,
        GLuint& out_tex);

private:
    static uint16_t U16BE(const uint8_t* p);
    static uint32_t U32BE(const uint8_t* p);

    // Spread low bits of v into even bit positions (for Morton index).
    static uint32_t SpreadBits(uint32_t v);

    static bool DecompressDXT5Linear(const uint8_t* src, uint16_t w, uint16_t h,
        std::vector<uint8_t>& rgba);
    static bool DecompressDXT5Swizzled(const uint8_t* src, uint16_t w, uint16_t h,
        std::vector<uint8_t>& rgba);
    static void DecodeDXT5Block(const uint8_t* block, uint8_t out[4 * 4 * 4]);
};