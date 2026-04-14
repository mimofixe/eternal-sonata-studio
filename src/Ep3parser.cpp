#include "Ep3Parser.h"
#include <fstream>
#include <cstring>
#include <iostream>

// Helpers

uint16_t Ep3Parser::U16BE(const uint8_t* p) {
    return uint16_t(p[0] << 8 | p[1]);
}

uint32_t Ep3Parser::U32BE(const uint8_t* p) {
    return uint32_t(p[0] << 24 | p[1] << 16 | p[2] << 8 | p[3]);
}

// Spread low bits of v into even bit positions:
// bit 0 of v → bit 0 of result
// bit 1 of v → bit 2 of result
// bit 2 of v → bit 4 of result  ... etc.
// Used to compute Morton (Z-order) block indices.
uint32_t Ep3Parser::SpreadBits(uint32_t v) {
    uint32_t r = 0;
    for (int i = 0; i < 12; i++)
        r |= ((v >> i) & 1u) << (2 * i);
    return r;
}

// Load

bool Ep3Parser::Load(const std::string& path, Ep3File& out) {
    out = {};

    std::ifstream f(path, std::ios::binary);
    if (!f) {
        std::cerr << "[Ep3Parser] Cannot open: " << path << "\n";
        return false;
    }
    f.seekg(0, std::ios::end);
    size_t sz = (size_t)f.tellg();
    f.seekg(0, std::ios::beg);
    out.raw.resize(sz);
    f.read(reinterpret_cast<char*>(out.raw.data()), sz);

    const uint8_t* data = out.raw.data();

    if (sz < 8 || memcmp(data, "Mefc", 4) != 0) {
        std::cerr << "[Ep3Parser] Not a Mefc file: " << path << "\n";
        return false;
    }

    out.filename = path;

    // Scan the entire file for NTX3 blocks.
    // EP3 NTX3 blocks always have:
    //   - "NTX3" magic
    //   - block size BE u32
    //   - format byte at +0x18 in the 0x80-byte header (0x88 or 0xA8 = DXT5)
    //   - width/height at +0x20/+0x22
    //   - pixel data starting at block_offset + 0x80

    size_t pos = 8;
    while (pos + 0x80 <= sz) {
        if (memcmp(data + pos, "NTX3", 4) != 0) {
            pos += 4;
            continue;
        }

        uint32_t block_sz = U32BE(data + pos + 4);
        if (block_sz < 0x80 || pos + block_sz > sz) {
            pos += 4;
            continue;
        }

        uint8_t  fmt = data[pos + 0x18];
        uint16_t w = U16BE(data + pos + 0x20);
        uint16_t h = U16BE(data + pos + 0x22);
        uint32_t px_sz = U32BE(data + pos + 0x14);

        if (w == 0 || h == 0) { pos += block_sz; continue; }

        // Only DXT5 variants (0x88 = linear, 0xA8 = swizzled).
        // Skip DXT1 (0x86/0xA6) and unknown formats for now.
        uint8_t fmt_base = fmt & 0xDF;  // strip swizzle bit
        if (fmt_base != 0x88 && fmt_base != 0x86) {
            pos += block_sz;
            continue;
        }

        Ep3Frame fr;
        fr.width = w;
        fr.height = h;
        fr.fmt = fmt;
        fr.swizzled = (fmt & 0x20) != 0;
        fr.pixel_offset = pos + 0x80;
        fr.pixel_size = std::min<size_t>(px_sz, block_sz - 0x80);

        out.frames.push_back(fr);

        std::cout << "[Ep3Parser] Frame " << out.frames.size() - 1
            << ": " << w << "x" << h
            << " fmt=0x" << std::hex << (int)fmt << std::dec
            << (fr.swizzled ? " swizzled" : " linear")
            << " px_bytes=" << fr.pixel_size << "\n";

        pos += block_sz;
    }

    if (out.frames.empty()) {
        std::cerr << "[Ep3Parser] No frames found in: " << path << "\n";
        return false;
    }

    out.valid = true;
    return true;
}

// DXT5 block decoder
// Writes 4*4*4 = 64 RGBA bytes.

void Ep3Parser::DecodeDXT5Block(const uint8_t* b, uint8_t out[64]) {
    // Alpha
    uint8_t a0 = b[0], a1 = b[1];
    uint64_t abits = 0;
    for (int i = 0; i < 6; i++) abits |= (uint64_t)b[2 + i] << (8 * i);

    uint8_t atab[8];
    atab[0] = a0; atab[1] = a1;
    if (a0 > a1) {
        atab[2] = uint8_t((6 * a0 + 1 * a1) / 7);
        atab[3] = uint8_t((5 * a0 + 2 * a1) / 7);
        atab[4] = uint8_t((4 * a0 + 3 * a1) / 7);
        atab[5] = uint8_t((3 * a0 + 4 * a1) / 7);
        atab[6] = uint8_t((2 * a0 + 5 * a1) / 7);
        atab[7] = uint8_t((1 * a0 + 6 * a1) / 7);
    }
    else {
        atab[2] = uint8_t((4 * a0 + 1 * a1) / 5);
        atab[3] = uint8_t((3 * a0 + 2 * a1) / 5);
        atab[4] = uint8_t((2 * a0 + 3 * a1) / 5);
        atab[5] = uint8_t((1 * a0 + 4 * a1) / 5);
        atab[6] = 0;
        atab[7] = 255;
    }

    // Colour
    uint16_t c0 = uint16_t(b[8] | (b[9] << 8));
    uint16_t c1 = uint16_t(b[10] | (b[11] << 8));
    uint32_t cbits = uint32_t(b[12]) | (uint32_t(b[13]) << 8)
        | (uint32_t(b[14]) << 16) | (uint32_t(b[15]) << 24);

    auto expand565 = [](uint16_t c, uint8_t& r, uint8_t& g, uint8_t& bl) {
        r = uint8_t(((c >> 11) & 0x1F) * 255 / 31);
        g = uint8_t(((c >> 5) & 0x3F) * 255 / 63);
        bl = uint8_t((c & 0x1F) * 255 / 31);
        };
    uint8_t r0, g0, b0, r1, g1, b1;
    expand565(c0, r0, g0, b0);
    expand565(c1, r1, g1, b1);

    uint8_t ctab[4][3];
    ctab[0][0] = r0; ctab[0][1] = g0; ctab[0][2] = b0;
    ctab[1][0] = r1; ctab[1][1] = g1; ctab[1][2] = b1;
    ctab[2][0] = uint8_t((2 * r0 + r1) / 3); ctab[2][1] = uint8_t((2 * g0 + g1) / 3); ctab[2][2] = uint8_t((2 * b0 + b1) / 3);
    ctab[3][0] = uint8_t((r0 + 2 * r1) / 3); ctab[3][1] = uint8_t((g0 + 2 * g1) / 3); ctab[3][2] = uint8_t((b0 + 2 * b1) / 3);

    for (int i = 0; i < 16; i++) {
        int ai = (abits >> (3 * i)) & 7;
        int ci = (cbits >> (2 * i)) & 3;
        int py = i / 4, px = i % 4;
        uint8_t* dst = out + (py * 4 + px) * 4;
        dst[0] = ctab[ci][0];
        dst[1] = ctab[ci][1];
        dst[2] = ctab[ci][2];
        dst[3] = atab[ai];
    }
}

// Linear DXT5 decode (row-major block order)

bool Ep3Parser::DecompressDXT5Linear(const uint8_t* src, uint16_t w, uint16_t h,
    std::vector<uint8_t>& rgba) {
    const int bx = (w + 3) / 4, by = (h + 3) / 4;
    rgba.assign(size_t(w) * h * 4, 0);
    uint8_t block_out[64];

    for (int ty = 0; ty < by; ty++) {
        for (int tx = 0; tx < bx; tx++) {
            size_t src_idx = size_t(ty * bx + tx) * 16;
            DecodeDXT5Block(src + src_idx, block_out);
            for (int py = 0; py < 4; py++) {
                for (int px = 0; px < 4; px++) {
                    int ox = tx * 4 + px, oy = ty * 4 + py;
                    if (ox >= w || oy >= h) continue;
                    size_t dst = (size_t(oy) * w + ox) * 4;
                    memcpy(rgba.data() + dst, block_out + (py * 4 + px) * 4, 4);
                }
            }
        }
    }
    return true;
}

// Swizzled DXT5 decode (Morton / Z-order block order)
//
// For each output block at (bx, by), the source block index is:
//   morton = SpreadBits(bx) | (SpreadBits(by) << 1)

bool Ep3Parser::DecompressDXT5Swizzled(const uint8_t* src, uint16_t w, uint16_t h,
    std::vector<uint8_t>& rgba) {
    const int nbx = (w + 3) / 4, nby = (h + 3) / 4;
    rgba.assign(size_t(w) * h * 4, 0);
    uint8_t block_out[64];

    for (int ty = 0; ty < nby; ty++) {
        for (int tx = 0; tx < nbx; tx++) {
            uint32_t morton = SpreadBits(tx) | (SpreadBits(ty) << 1);
            size_t src_idx = size_t(morton) * 16;
            // Safety: skip if morton index exceeds available data
            // (can happen for non-POT textures near the edges)
            if (src_idx + 16 > size_t(nbx) * nby * 16) continue;

            DecodeDXT5Block(src + src_idx, block_out);
            for (int py = 0; py < 4; py++) {
                for (int px = 0; px < 4; px++) {
                    int ox = tx * 4 + px, oy = ty * 4 + py;
                    if (ox >= w || oy >= h) continue;
                    size_t dst = (size_t(oy) * w + ox) * 4;
                    memcpy(rgba.data() + dst, block_out + (py * 4 + px) * 4, 4);
                }
            }
        }
    }
    return true;
}

// Public: decompress a frame to RGBA8

// Returns true if both block dimensions (in 4x4 blocks) are powers of two.
static bool BlockDimsArePOT(uint16_t w, uint16_t h) {
    auto isPOT = [](uint16_t v) { return v > 0 && (v & (v - 1)) == 0; };
    uint16_t bx = (w + 3) / 4, by = (h + 3) / 4;
    return isPOT(bx) && isPOT(by);
}

bool Ep3Parser::DecompressFrame(const Ep3File& ep3, size_t idx,
    std::vector<uint8_t>& out_rgba) {
    if (!ep3.valid || idx >= ep3.frames.size()) return false;

    const Ep3Frame& fr = ep3.frames[idx];
    const uint8_t* px = ep3.raw.data() + fr.pixel_offset;

    // Morton swizzle only applies when both block dimensions are powers of two.
    // Non-POT textures (e.g. 768x256 = 192x64 blocks) store blocks linearly
    // even when the format byte has the swizzle flag (0x20) set.
    if (fr.swizzled && BlockDimsArePOT(fr.width, fr.height))
        return DecompressDXT5Swizzled(px, fr.width, fr.height, out_rgba);
    else
        return DecompressDXT5Linear(px, fr.width, fr.height, out_rgba);
}

// Public: decompress + upload to GL texture

bool Ep3Parser::FrameToGLTexture(const Ep3File& ep3, size_t idx, GLuint& out_tex) {
    std::vector<uint8_t> rgba;
    if (!DecompressFrame(ep3, idx, rgba)) return false;

    const Ep3Frame& fr = ep3.frames[idx];

    if (out_tex) glDeleteTextures(1, &out_tex);
    glGenTextures(1, &out_tex);
    glBindTexture(GL_TEXTURE_2D, out_tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, fr.width, fr.height, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);
    return true;
}