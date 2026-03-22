#include "P3TexParser.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <cmath>

#define BCDEC_IMPLEMENTATION
#include "../libs/bcdec/bcdec.h"

P3TexParser::P3TexParser() : m_Loaded(false) {
}

P3TexParser::~P3TexParser() {
    Clear();
}

bool P3TexParser::Load(const std::string& filename) {
    Clear();

    std::ifstream file(filename, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open P3TEX: " << filename << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(file_size);
    file.read(reinterpret_cast<char*>(buffer.data()), file_size);
    file.close();

    std::cout << "Loading P3TEX: " << filename << std::endl;
    std::cout << "  Size: " << file_size << " bytes" << std::endl;

    size_t pos = 0;
    int texture_count = 0;

    while (pos + 128 <= file_size) {
        if (memcmp(&buffer[pos], "NTX3", 4) != 0) {
            pos++;
            continue;
        }

        uint32_t chunk_size = (buffer[pos + 4] << 24) |
            (buffer[pos + 5] << 16) |
            (buffer[pos + 6] << 8) |
            buffer[pos + 7];

        if (pos + chunk_size > file_size) {
            std::cerr << "  Warning: Truncated chunk at offset " << pos << std::endl;
            break;
        }

        uint16_t width = (buffer[pos + 0x20] << 8) | buffer[pos + 0x21];
        uint16_t height = (buffer[pos + 0x22] << 8) | buffer[pos + 0x23];
        uint8_t format_byte = buffer[pos + 0x18];

        P3Texture texture;
        texture.offset = pos + 128;
        texture.size = chunk_size - 128;
        texture.format = format_byte;

        if (texture.offset + texture.size <= file_size) {
            texture.data.resize(texture.size);
            memcpy(texture.data.data(), &buffer[texture.offset], texture.size);

            if (width == 0 || height == 0) {
                if (DeduceDimensions(texture.size, (int&)width, (int&)height)) {
                    texture.width = width;
                    texture.height = height;
                    std::cout << "    Texture " << texture_count
                        << ": " << width << "x" << height << " (deduced)";
                }
                else {
                    texture.width = 0;
                    texture.height = 0;
                    std::cout << "    Texture " << texture_count
                        << ": Could not determine dimensions (size: " << texture.size << ")";
                }
            }
            else {
                texture.width = width;
                texture.height = height;
                std::cout << "    Texture " << texture_count
                    << ": " << width << "x" << height;
            }

            if (format_byte == 0x86) {
                std::cout << " [DXT1]";
            }
            else {
                std::cout << " [DXT5]";
            }
            std::cout << std::endl;

            m_Textures.push_back(texture);
            texture_count++;
        }

        pos += chunk_size;
    }

    m_Filename = filename;
    m_Loaded = true;

    std::cout << "  Loaded " << texture_count << " textures" << std::endl;

    return texture_count > 0;
}

void P3TexParser::Clear() {
    m_Textures.clear();
    m_Filename.clear();
    m_Loaded = false;
}

const P3Texture* P3TexParser::GetTexture(uint8_t id) const {
    if (id >= m_Textures.size()) {
        return nullptr;
    }
    return &m_Textures[id];
}

bool P3TexParser::DeduceDimensions(size_t dataSize, int& width, int& height) {
    int sizes[] = { 16, 32, 64, 128, 256, 512, 1024, 2048, 4096 };

    for (int w : sizes) {
        for (int h : sizes) {
            // DXT1: 8 bytes per 4x4 block + mipmaps
            size_t totalSize = 0;
            int mipWidth = w;
            int mipHeight = h;

            while (mipWidth >= 4 && mipHeight >= 4) {
                size_t mipBlocks = (mipWidth / 4) * (mipHeight / 4);
                totalSize += mipBlocks * 8;

                mipWidth /= 2;
                mipHeight /= 2;
            }

            if (totalSize == dataSize) {
                width = w;
                height = h;
                return true;
            }
        }
    }

    for (int w : sizes) {
        for (int h : sizes) {
            // DXT5: 16 bytes per 4x4 block + mipmaps
            size_t totalSize = 0;
            int mipWidth = w;
            int mipHeight = h;

            while (mipWidth >= 4 && mipHeight >= 4) {
                size_t mipBlocks = (mipWidth / 4) * (mipHeight / 4);
                totalSize += mipBlocks * 16;

                mipWidth /= 2;
                mipHeight /= 2;
            }

            if (totalSize == dataSize) {
                width = w;
                height = h;
                return true;
            }
        }
    }

    return false;
}

std::vector<uint8_t> P3TexParser::DecompressDXT1(const uint8_t* data, int width, int height) {
    // NO internal header - DXT data starts immediately
    const uint8_t* dxt1Data = data;

    std::vector<uint8_t> rgba(width * height * 4);

    int blockCountX = width / 4;
    int blockCountY = height / 4;

    for (int by = 0; by < blockCountY; by++) {
        for (int bx = 0; bx < blockCountX; bx++) {
            const uint8_t* blockData = dxt1Data + (by * blockCountX + bx) * 8;

            uint8_t blockRGBA[64];
            bcdec_bc1(blockData, blockRGBA, 16);

            for (int py = 0; py < 4; py++) {
                for (int px = 0; px < 4; px++) {
                    int x = bx * 4 + px;
                    int y = by * 4 + py;

                    if (x < width && y < height) {
                        int srcIdx = (py * 4 + px) * 4;
                        int dstIdx = (y * width + x) * 4;

                        rgba[dstIdx + 0] = blockRGBA[srcIdx + 0];
                        rgba[dstIdx + 1] = blockRGBA[srcIdx + 1];
                        rgba[dstIdx + 2] = blockRGBA[srcIdx + 2];
                        rgba[dstIdx + 3] = blockRGBA[srcIdx + 3];
                    }
                }
            }
        }
    }

    return rgba;
}

std::vector<uint8_t> P3TexParser::DecompressDXT5(const uint8_t* data, int width, int height) {
    // NO internal header - DXT data starts immediately
    const uint8_t* dxt5Data = data;

    std::vector<uint8_t> rgba(width * height * 4);

    int blockCountX = width / 4;
    int blockCountY = height / 4;

    for (int by = 0; by < blockCountY; by++) {
        for (int bx = 0; bx < blockCountX; bx++) {
            const uint8_t* blockData = dxt5Data + (by * blockCountX + bx) * 16;

            uint8_t blockRGBA[64];
            bcdec_bc3(blockData, blockRGBA, 16);

            for (int py = 0; py < 4; py++) {
                for (int px = 0; px < 4; px++) {
                    int x = bx * 4 + px;
                    int y = by * 4 + py;

                    if (x < width && y < height) {
                        int srcIdx = (py * 4 + px) * 4;
                        int dstIdx = (y * width + x) * 4;

                        rgba[dstIdx + 0] = blockRGBA[srcIdx + 0];
                        rgba[dstIdx + 1] = blockRGBA[srcIdx + 1];
                        rgba[dstIdx + 2] = blockRGBA[srcIdx + 2];
                        rgba[dstIdx + 3] = blockRGBA[srcIdx + 3];
                    }
                }
            }
        }
    }

    return rgba;
}