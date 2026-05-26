#pragma once
#include <cstdint>
#include <vector>
#include <string>

// Xenos TextureFormat (from xenia/gpu/xenos.h)
// Only the values relevant to Eternal Sonata Xbox 360 textures:
//   k_DXT1       = 18   (DXT1 / BC1)
//   k_DXT2_3     = 19   (DXT3 / BC2)
//   k_DXT4_5     = 20   (DXT5 / BC3)
//   k_8_8_8_8    =  6   (RGBA8)
enum class XeTextureFormat : uint32_t {
    k_8_8_8_8 = 6,
    k_DXT1 = 18,
    k_DXT2_3 = 19,
    k_DXT4_5 = 20,
};

// NTX2 texture descriptor
struct NTX2Info {
    std::string name;         // e.g. "face_alg.tga"
    int         width = 0;
    int         height = 0;
    // Pitch in macro-tiles from the W0 pitch field. Correct even for narrow
    // textures (Xbox 360 pads pitch to the nearest 32-block boundary, so a
    // 64px-wide DXT1 texture gets pitch=128px → pitch_macro_tiles=1, not 0).
    int         pitch_macro_tiles = 0;
    XeTextureFormat format = XeTextureFormat::k_DXT1;
    bool        tiled = true;

    // Byte offset of pixel data from the *start of the NTX2 chunk*
    uint32_t    pix_offset = 0;
    // Byte length of the full mip-chain pixel data (tiled layout)
    uint32_t    pix_size = 0;
};

// Ntx2Parser
// Reads Namco's NTX2 wrapper around an XPR2 Xbox 360 tiled texture.
//
// NTX2 chunk layout (all multi-byte fields including the
// GPUTEXTURE_FETCH_CONSTANT are big-endian):
//
//  +0x00  "NTX2"          magic
//  +0x04  chunk_size      total size of this NTX2 entry (u32 BE)
//  +0x08  "XPR2"          sub-magic
//  +0x0C  xpr2_tail_size  size of the trailing pad region  (u32 BE)
//  +0x10  hdr_sz          offset where pixel data ends / tail begins (u32 BE)
//  +0x14  1               num_textures, always 1  (u32 BE)
//  +0x18  "TX2D"          descriptor magic
//  +0x1C  tx2d_name_sz    size of the name-area block (0x20 or 0x30) (u32 BE)
//  +0x20  0x34            data_section_offset  (u32 BE, ignored here)
//  +0x24  0x18            (u32 BE, ignored here)
//  +0x28  0               padding  (u32 BE)
//  +0x2C  <name>          null-terminated ASCII filename (fits in 32 bytes)
//
//  GPUTEXTURE_FETCH_CONSTANT (6 x u32, stored big-endian):
//    base = NTX2 + 0x18 + tx2d_name_sz + 0x18
//    W0 @ base+0  : tiled (bit 31), pitch, clamp, …
//    W1 @ base+4  : format (bits 5:0), endian (bits 7:6), base_address, …
//    W2 @ base+8  : width-1 (bits 12:0), height-1 (bits 25:13), depth/stack
//    W3..W5        (filter settings, not used here)
//
//  From xenia/gpu/xenos.h xe_gpu_texture_fetch_t::size_2d:
//    width  = (W2 & 0x1FFF) + 1
//    height = ((W2 >> 13) & 0x1FFF) + 1
//    format = W1 & 0x3F  → 18 = k_DXT1
//
//  Pixel data begins at the first 4 KB-aligned absolute address that is
//  at least 0x1000 bytes past the chunk start:
//    pix_abs   = (chunk_abs_offset + 0x1FFF) & ~0xFFF
//    pix_offset = pix_abs - chunk_abs_offset
//    pix_size   = hdr_sz - pix_offset
//  For aligned chunks (chunk_abs_offset & 0xFFF == 0): pix_offset = 0x1000 (same as before).
//  For misaligned chunks: pix_offset > 0x1000 (aligns to next 4 KB page).
class Ntx2Parser {
public:
    // Parse the NTX2 header at data[0].  Returns false if magic is wrong or
    // the chunk is too small.
    static bool ParseHeader(const uint8_t* data, size_t size, NTX2Info& out,
        size_t chunk_abs_offset = 0);

    // Untile and decompress a tiled DXT1 texture to RGBA8.
    // tiled_data must point to the mip-0 pixel block (pix_offset already
    // applied by the caller).  width / height are in pixels.
    // pitch_macro_tiles must come from NTX2Info::pitch_macro_tiles (W0 field),
    // NOT from width_blocks>>5 (wrong for narrow/non-PoT textures).
    static std::vector<uint8_t> UntileAndDecodeDXT1(
        const uint8_t* tiled_data, size_t tiled_size,
        int width, int height, int pitch_macro_tiles);

    // Convenience: parse header + copy pixel slice + decode to RGBA8.
    // chunk_data points to the start of the NTX2 chunk, chunk_size is its
    // total byte length.  Returns empty vector on failure.
    static std::vector<uint8_t> Decode(
        const uint8_t* chunk_data, size_t chunk_size,
        NTX2Info& info_out,
        size_t chunk_abs_offset = 0);

private:
    // Xenia XenosTextureTiledAddress2D
    // Returns the byte offset of block (bx, by) inside the tiled buffer.
    //  pitch_macro_tiles = width_blocks / 32
    //  bpp_log2          = 3 for DXT1 (8 bytes/block)
    static int  XenosTiledAddr2D(int bx, int by,
        int pitch_macro_tiles, int bpp_log2);
    static int  XenosCombine(int oib, int bank, int pipe, int y_lsb);

    // Big-endian helpers
    static uint16_t ReadU16BE(const uint8_t* d);
    static uint32_t ReadU32BE(const uint8_t* d);
};