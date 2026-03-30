#include "EFileParser.h"
#include <fstream>
#include <iostream>
#include <cstring>
#include <cmath>

uint32_t EFileParser::SwapEndian(uint32_t val) {
    return ((val >> 24) & 0xFF) |
        ((val >> 8) & 0xFF00) |
        ((val << 8) & 0xFF0000) |
        ((val << 24) & 0xFF000000);
}

uint32_t EFileParser::ReadU32LE(const uint8_t* data) {
    return data[0] | (data[1] << 8) | (data[2] << 16) | (data[3] << 24);
}

uint32_t EFileParser::ReadU32BE(const uint8_t* data) {
    return (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
}

float EFileParser::ReadFloatBE(const uint8_t* data) {
    uint32_t val = ReadU32BE(data);
    float result;
    memcpy(&result, &val, 4);
    return result;
}

ChunkType EFileParser::GetChunkType(uint32_t magic) {
    //e file
    if (magic == MAGIC_NOBJ) return ChunkType::NOBJ;
    if (magic == MAGIC_NMDL) return ChunkType::NMDL;
    if (magic == MAGIC_NSHP) return ChunkType::NSHP;
    if (magic == MAGIC_NTX3) return ChunkType::NTX3;
    if (magic == MAGIC_NMTN) return ChunkType::NMTN;
    if (magic == MAGIC_NCAM) return ChunkType::NCAM;
    if (magic == MAGIC_NLIT) return ChunkType::NLIT;
    if (magic == MAGIC_NFOG) return ChunkType::NFOG;
    if (magic == MAGIC_NMTR) return ChunkType::NMTR;
    if (magic == MAGIC_SONG) return ChunkType::SONG;

    //others
    if (magic == MAGIC_BOOK) return ChunkType::BOOK;
    if (magic == MAGIC_PGHD) return ChunkType::PGHD;
    if (magic == MAGIC_TIM)  return ChunkType::TIM;
    if (magic == MAGIC_PROG) return ChunkType::PROG;
    if (magic == MAGIC_CSF)  return ChunkType::CSF;
    if (magic == MAGIC_FONT) return ChunkType::FONT;
    if (magic == MAGIC_NBN2) return ChunkType::NBN2;
    if (magic == MAGIC_NMTB) return ChunkType::NMTB;
    if (magic == MAGIC_NDYN) return ChunkType::NDYN;
    if (magic == MAGIC_NLC2) return ChunkType::NLC2;
    if (magic == MAGIC_NCLS) return ChunkType::NCLS;

    return ChunkType::Unknown;
}

std::vector<Chunk> EFileParser::Parse(const std::string& filepath) {
    std::vector<Chunk> chunks;

    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        std::cerr << "Failed to open: " << filepath << std::endl;
        return chunks;
    }

    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> data(fileSize);
    file.read((char*)data.data(), fileSize);
    file.close();

    std::cout << "\n=== EFile Parser ===" << std::endl;
    std::cout << "File: " << filepath << std::endl;
    std::cout << "Size: " << fileSize << " bytes (" << (fileSize / 1024.0f / 1024.0f) << " MB)" << std::endl;

    const uint32_t magics[] = {
        MAGIC_NOBJ, MAGIC_NMDL, MAGIC_NSHP, MAGIC_NTX3, MAGIC_NMTN,
        MAGIC_NCAM, MAGIC_NLIT, MAGIC_NFOG, MAGIC_NMTR, MAGIC_SONG,
        MAGIC_NBN2, MAGIC_NMTB, MAGIC_NDYN, MAGIC_NLC2, MAGIC_NCLS
    };

    for (size_t i = 0; i < fileSize - 64; i++) {
        uint32_t magic = ReadU32LE(&data[i]);

        bool found = false;
        for (uint32_t test_magic : magics) {
            if (magic == test_magic) {
                found = true;
                break;
            }
        }

        if (found) {
            Chunk chunk;
            chunk.magic = magic;
            chunk.offset = i;
            chunk.type = GetChunkType(magic);

            if (i + 8 <= fileSize) {
                chunk.size = ReadU32BE(&data[i + 4]);
            }
            else {
                chunk.size = 0;
            }

            memset(chunk.name, 0, sizeof(chunk.name));

            size_t name_start = 0;

            switch (chunk.type) {
            case ChunkType::NOBJ:
                strncpy(chunk.name, "[container]", sizeof(chunk.name) - 1);
                break;

            case ChunkType::NMDL:
                name_start = i + 16;
                for (size_t j = 0; j < sizeof(chunk.name) - 1 && name_start + j < fileSize; j++) {
                    if (data[name_start + j] >= 32 && data[name_start + j] < 127) {
                        chunk.name[j] = data[name_start + j];
                    }
                    else {
                        break;
                    }
                }
                break;

            case ChunkType::NSHP:
            case ChunkType::NBN2:
            case ChunkType::NMTB:
            case ChunkType::NDYN:
            case ChunkType::NLC2:
            case ChunkType::NCLS:
            case ChunkType::NMTN:
                name_start = i + 8;
                for (size_t j = 0; j < sizeof(chunk.name) - 1 && name_start + j < fileSize; j++) {
                    if (data[name_start + j] != 0) {
                        chunk.name[j] = data[name_start + j];
                    }
                    else {
                        break;
                    }
                }
                break;

            case ChunkType::NLIT:
                name_start = i + 8;
                for (size_t j = 0; j < sizeof(chunk.name) - 1 && name_start + j < fileSize; j++) {
                    if (data[name_start + j] >= 32 && data[name_start + j] < 127) {
                        chunk.name[j] = data[name_start + j];
                    }
                    else {
                        break;
                    }
                }
                break;

            case ChunkType::NTX3:
                snprintf(chunk.name, sizeof(chunk.name), "texture_%zu", chunks.size());
                break;

            case ChunkType::NCAM:
                snprintf(chunk.name, sizeof(chunk.name), "camera_%zu", chunks.size());
                break;

            case ChunkType::NFOG:
                strncpy(chunk.name, "[fog]", sizeof(chunk.name) - 1);
                break;

            case ChunkType::NMTR:
                snprintf(chunk.name, sizeof(chunk.name), "material_%zu", chunks.size());
                break;

            case ChunkType::SONG:
                snprintf(chunk.name, sizeof(chunk.name), "audio_%zu", chunks.size());
                break;

            default:
                strncpy(chunk.name, "[unknown]", sizeof(chunk.name) - 1);
                break;
            }

            chunks.push_back(chunk);
        }
    }

    int counts[20] = { 0 };   // sized for all ChunkType values (enum count + margin)
    for (const auto& chunk : chunks) {
        int idx = (int)chunk.type;
        if (idx >= 0 && idx < 20) counts[idx]++;
    }

    std::cout << "\nSummary:" << std::endl;
    std::cout << "  NOBJ (containers): " << counts[(int)ChunkType::NOBJ] << std::endl;
    std::cout << "  NMDL (models): " << counts[(int)ChunkType::NMDL] << std::endl;
    std::cout << "  NSHP (meshes): " << counts[(int)ChunkType::NSHP] << std::endl;
    std::cout << "  NTX3 (textures): " << counts[(int)ChunkType::NTX3] << std::endl;
    std::cout << "  NMTN (animations): " << counts[(int)ChunkType::NMTN] << std::endl;
    std::cout << "  NBN2 (skeletons): " << counts[(int)ChunkType::NBN2] << std::endl;
    std::cout << "  NCAM (cameras): " << counts[(int)ChunkType::NCAM] << std::endl;
    std::cout << "  NLIT (lights): " << counts[(int)ChunkType::NLIT] << std::endl;
    std::cout << "  NFOG (fog): " << counts[(int)ChunkType::NFOG] << std::endl;
    std::cout << "  NMTR (materials): " << counts[(int)ChunkType::NMTR] << std::endl;
    std::cout << "  SONG (audio): " << counts[(int)ChunkType::SONG] << std::endl;
    std::cout << "  NMTB/NDYN/NLC2/NCLS: "
        << counts[(int)ChunkType::NMTB] + counts[(int)ChunkType::NDYN]
        + counts[(int)ChunkType::NLC2] + counts[(int)ChunkType::NCLS] << std::endl;
    std::cout << "  Total: " << chunks.size() << std::endl;
    std::cout << "==================\n" << std::endl;

    return chunks;
}

bool EFileParser::ParseCamera(const uint8_t* data, size_t size, CameraData& out) {
    if (size < 40) return false;

    out.position[0] = ReadFloatBE(data + 8);
    out.position[1] = ReadFloatBE(data + 12);
    out.position[2] = ReadFloatBE(data + 16);

    out.rotation[0] = ReadFloatBE(data + 20);
    out.rotation[1] = ReadFloatBE(data + 24);
    out.rotation[2] = ReadFloatBE(data + 28);

    out.fov = ReadFloatBE(data + 32);
    out.parsed = true;

    return true;
}

bool EFileParser::ParseLight(const uint8_t* data, size_t size, LightData& out) {
    if (size < 32) return false;

    memset(out.light_name, 0, sizeof(out.light_name));
    for (int i = 0; i < 16 && data[8 + i] != 0; i++) {
        out.light_name[i] = data[8 + i];
    }

    out.color[0] = data[24];
    out.color[1] = data[25];
    out.color[2] = data[26];

    out.is_ambient = (strstr(out.light_name, "ambient") != nullptr);

    out.direction[0] = 0.0f;
    out.direction[1] = -1.0f;
    out.direction[2] = 0.0f;

    out.intensity = 1.0f;
    out.parsed = true;

    return true;
}

bool EFileParser::ParseFog(const uint8_t* data, size_t size, FogData& out) {
    if (size < 24) return false;

    out.color[0] = data[8];
    out.color[1] = data[9];
    out.color[2] = data[10];

    out.near_distance = ReadFloatBE(data + 12);
    out.far_distance = ReadFloatBE(data + 16);
    out.parsed = true;

    return true;
}

bool EFileParser::ParseMaterial(const uint8_t* data, size_t size, MaterialData& out) {
    if (size < 32) return false;

    out.flags = ReadU32BE(data + 8);

    out.diffuse_color[0] = data[12];
    out.diffuse_color[1] = data[13];
    out.diffuse_color[2] = data[14];
    out.diffuse_color[3] = data[15];

    out.specular_color[0] = data[16];
    out.specular_color[1] = data[17];
    out.specular_color[2] = data[18];
    out.specular_color[3] = data[19];

    out.ambient_color[0] = data[20];
    out.ambient_color[1] = data[21];
    out.ambient_color[2] = data[22];
    out.ambient_color[3] = data[23];

    out.parsed = true;

    return true;
}