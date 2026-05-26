#include "P3TexParser.h"
#include "Ntx2parser.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <cmath>

#define BCDEC_IMPLEMENTATION
#include "../libs/bcdec/bcdec.h"

P3TexParser::P3TexParser() : m_Loaded(false) {}
P3TexParser::~P3TexParser() { Clear(); }

// Load

bool P3TexParser::Load(const std::string& filename) {
    Clear();

    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open texture file: " << filename << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(file_size);
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);
    file.close();

    std::cout << "Loading texture file: " << filename << std::endl;
    std::cout << "  Size: " << file_size << " bytes" << std::endl;

    const uint8_t* buf = buffer.data();
    size_t pos = 0;
    int texture_count = 0;

    while (pos < file_size) {
        // NTX2 (Xbox 360)
        if (pos + 8 <= file_size &&
            std::memcmp(buf + pos, "NTX2", 4) == 0) {

            uint32_t chunk_size =
                (buf[pos + 4] << 24) | (buf[pos + 5] << 16) |
                (buf[pos + 6] << 8) | buf[pos + 7];

            if (chunk_size < 0x1010 || pos + chunk_size > file_size) {
                pos++;
                continue;
            }

            NTX2Info info;
            std::vector<uint8_t> rgba =
                Ntx2Parser::Decode(buf + pos, chunk_size, info, pos);

            if (!rgba.empty() && info.width > 0 && info.height > 0) {
                P3Texture tex;
                tex.offset = pos;
                tex.size = chunk_size;
                tex.width = info.width;
                tex.height = info.height;
                tex.format = P3TEX_FORMAT_RGBA8_DECODED;
                tex.name = info.name;
                tex.data = std::move(rgba);

                std::cout << "  [NTX2] " << texture_count
                    << ": " << info.name
                    << "  " << info.width << "×" << info.height
                    << std::endl;

                m_Textures.push_back(std::move(tex));
                texture_count++;
            }

            pos += chunk_size;
            continue;
        }

        // NTX3 (PS3)
        if (pos + 128 <= file_size &&
            std::memcmp(buf + pos, "NTX3", 4) == 0) {

            uint32_t chunk_size =
                (buf[pos + 4] << 24) | (buf[pos + 5] << 16) |
                (buf[pos + 6] << 8) | buf[pos + 7];

            if (pos + chunk_size > file_size) {
                std::cerr << "  Warning: truncated NTX3 at offset " << pos << std::endl;
                break;
            }

            uint16_t width = (buf[pos + 0x20] << 8) | buf[pos + 0x21];
            uint16_t height = (buf[pos + 0x22] << 8) | buf[pos + 0x23];
            uint8_t  fmt = buf[pos + 0x18];

            // 0xA5 = raw RGBA8: derive height from data size
            if (fmt == 0xA5 && width > 0 && chunk_size > 0x88)
                height = static_cast<uint16_t>((chunk_size - 0x88) / (width * 4));

            P3Texture tex;
            tex.offset = pos + 0x88;
            tex.size = chunk_size - 0x88;
            tex.format = fmt;

            if (tex.offset + tex.size <= file_size) {
                tex.data.resize(tex.size);
                std::memcpy(tex.data.data(), buf + tex.offset, tex.size);

                if (width == 0 || height == 0) {
                    int dw = 0, dh = 0;
                    DeduceDimensions(tex.size, dw, dh);
                    width = static_cast<uint16_t>(dw);
                    height = static_cast<uint16_t>(dh);
                }
                tex.width = width;
                tex.height = height;

                std::cout << "  [NTX3] " << texture_count
                    << ": " << width << "×" << height;
                if ((fmt & 0xDF) == 0x86)
                    std::cout << " [DXT1]";
                else
                    std::cout << " [DXT5]";
                std::cout << std::endl;

                m_Textures.push_back(std::move(tex));
                texture_count++;
            }

            pos += chunk_size;
            continue;
        }

        pos++;
    }

    m_Filename = filename;
    m_Loaded = true;
    std::cout << "  Loaded " << texture_count << " texture(s)" << std::endl;
    return texture_count > 0;
}

// Clear

void P3TexParser::Clear() {
    m_Textures.clear();
    m_Filename.clear();
    m_Loaded = false;
}

// GetTexture

const P3Texture* P3TexParser::GetTexture(uint8_t id) const {
    if (id >= m_Textures.size()) return nullptr;
    return &m_Textures[id];
}

// DeduceDimensions

bool P3TexParser::DeduceDimensions(size_t dataSize, int& width, int& height) {
    const int sizes[] = { 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 };

    // Try DXT1 (8 bytes/block)
    for (int w : sizes) {
        for (int h : sizes) {
            size_t total = 0;
            int mw = w, mh = h;
            while (mw >= 4 && mh >= 4) {
                total += static_cast<size_t>((mw / 4) * (mh / 4)) * 8;
                mw /= 2; mh /= 2;
            }
            if (total == dataSize) { width = w; height = h; return true; }
        }
    }
    // Try DXT5 (16 bytes/block)
    for (int w : sizes) {
        for (int h : sizes) {
            size_t total = 0;
            int mw = w, mh = h;
            while (mw >= 4 && mh >= 4) {
                total += static_cast<size_t>((mw / 4) * (mh / 4)) * 16;
                mw /= 2; mh /= 2;
            }
            if (total == dataSize) { width = w; height = h; return true; }
        }
    }
    return false;
}

// DecompressDXT1  (PS3 – standard BC1)

std::vector<uint8_t> P3TexParser::DecompressDXT1(
    const uint8_t* data, int width, int height) {
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);

    const int bx_count = width / 4;
    const int by_count = height / 4;

    for (int by = 0; by < by_count; ++by) {
        for (int bx = 0; bx < bx_count; ++bx) {
            const uint8_t* src = data + (by * bx_count + bx) * 8;
            uint8_t decoded[64];
            bcdec_bc1(src, decoded, 16);

            for (int py = 0; py < 4; ++py) {
                for (int px = 0; px < 4; ++px) {
                    const int x = bx * 4 + px, y = by * 4 + py;
                    if (x < width && y < height) {
                        const int s = (py * 4 + px) * 4;
                        const int d = (y * width + x) * 4;
                        rgba[d] = decoded[s];
                        rgba[d + 1] = decoded[s + 1];
                        rgba[d + 2] = decoded[s + 2];
                        rgba[d + 3] = decoded[s + 3];
                    }
                }
            }
        }
    }
    return rgba;
}

// DecompressDXT5  (PS3 – Namco-swapped BC3)

std::vector<uint8_t> P3TexParser::DecompressDXT5(
    const uint8_t* data, int width, int height) {
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4);

    const int bx_count = width / 4;
    const int by_count = height / 4;

    for (int by = 0; by < by_count; ++by) {
        for (int bx = 0; bx < bx_count; ++bx) {
            const uint8_t* src = data + (by * bx_count + bx) * 16;

            // PS3 Namco DXT5: colour block first, alpha block second.
            // bcdec_bc3 expects alpha first → swap the two 8-byte halves.
            uint8_t swapped[16];
            std::memcpy(swapped, src + 8, 8);
            std::memcpy(swapped + 8, src, 8);

            uint8_t decoded[64];
            bcdec_bc3(swapped, decoded, 16);

            for (int py = 0; py < 4; ++py) {
                for (int px = 0; px < 4; ++px) {
                    const int x = bx * 4 + px, y = by * 4 + py;
                    if (x < width && y < height) {
                        const int s = (py * 4 + px) * 4;
                        const int d = (y * width + x) * 4;
                        rgba[d] = decoded[s];
                        rgba[d + 1] = decoded[s + 1];
                        rgba[d + 2] = decoded[s + 2];
                        rgba[d + 3] = decoded[s + 3];
                    }
                }
            }
        }
    }
    return rgba;
}

// DecompressRGBA8  (PS3 raw uncompressed)

std::vector<uint8_t> P3TexParser::DecompressRGBA8(
    const uint8_t* data, int width, int height) {
    std::vector<uint8_t> rgba(static_cast<size_t>(width) * height * 4, 0);
    std::memcpy(rgba.data(), data, rgba.size());
    return rgba;
}