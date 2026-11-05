#include "Ntexparser.h"
#include <cstring>
#include <iostream>

#include "../libs/bcdec/bcdec.h"

namespace {

uint32_t ReadU32LE(const uint8_t* d) {
    return uint32_t(d[0]) | (uint32_t(d[1]) << 8) |
        (uint32_t(d[2]) << 16) | (uint32_t(d[3]) << 24);
}

// DDS_HEADER field offsets, relative to the "DDS " magic.
constexpr size_t DDS_HEIGHT = 0x0C;
constexpr size_t DDS_WIDTH = 0x10;
constexpr size_t DDS_PF_FLAGS = 0x50;
constexpr size_t DDS_PF_FOURCC = 0x54;
constexpr size_t DDS_PIXELS = 0x80;   // 4 magic + 124 header

constexpr uint32_t DDPF_FOURCC = 0x4;

} // namespace

std::vector<uint8_t> NtexParser::Decode(const uint8_t* chunk_data,
    size_t chunk_size,
    NTEXInfo& info_out) {
    info_out = NTEXInfo{};
    if (!chunk_data || chunk_size < 8 + DDS_PIXELS) return {};
    if (std::memcmp(chunk_data, "NTEX", 4) != 0) return {};

    const uint8_t* dds = chunk_data + 8;
    const size_t   dds_size = chunk_size - 8;
    if (dds_size < DDS_PIXELS) return {};
    if (std::memcmp(dds, "DDS ", 4) != 0) return {};
    if (ReadU32LE(dds + 4) != 124) return {};

    const uint32_t h = ReadU32LE(dds + DDS_HEIGHT);
    const uint32_t w = ReadU32LE(dds + DDS_WIDTH);
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return {};
    info_out.width = int(w);
    info_out.height = int(h);

    // Only the FourCC block formats are used by anything a material points
    // at. The remaining variant in the wild is a 16bpp uncompressed surface
    // (pf flags 0x80000) used for light/shadow maps, which is never
    // referenced by an NMTR entry - it just occupies a texture slot.
    if ((ReadU32LE(dds + DDS_PF_FLAGS) & DDPF_FOURCC) == 0) return {};

    const uint8_t* fourcc = dds + DDS_PF_FOURCC;
    // DXT2/DXT3 (BC2) would need a decoder this build of bcdec doesn't ship;
    // no NTEX chunk in the game's data uses it.
    bool is_bc1 = std::memcmp(fourcc, "DXT1", 4) == 0;
    bool is_bc3 = std::memcmp(fourcc, "DXT4", 4) == 0 ||
        std::memcmp(fourcc, "DXT5", 4) == 0;
    if (!is_bc1 && !is_bc3) {
        std::cerr << "[NTEX] Unsupported FourCC '"
            << std::string(reinterpret_cast<const char*>(fourcc), 4) << "'\n";
        return {};
    }
    info_out.is_dxt5 = is_bc3;

    const size_t block_bytes = is_bc1 ? 8u : 16u;
    const size_t blocks_x = (w + 3) / 4;
    const size_t blocks_y = (h + 3) / 4;
    if (DDS_PIXELS + blocks_x * blocks_y * block_bytes > dds_size) return {};

    const uint8_t* src = dds + DDS_PIXELS;
    std::vector<uint8_t> rgba(size_t(w) * h * 4, 0);
    const int row_pitch = int(w) * 4;

    for (size_t by = 0; by < blocks_y; by++) {
        for (size_t bx = 0; bx < blocks_x; bx++) {
            const uint8_t* block = src + (by * blocks_x + bx) * block_bytes;
            uint8_t* dest = rgba.data() + (by * 4 * w + bx * 4) * 4;
            // Straight PC layout - no endian swap, no Namco half-swap.
            if (is_bc1) bcdec_bc1(block, dest, row_pitch);
            else        bcdec_bc3(block, dest, row_pitch);
        }
    }

    return rgba;
}
