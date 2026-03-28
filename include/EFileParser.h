#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include <cstring>

constexpr uint32_t MAGIC_NOBJ = 0x4A424F4E;
constexpr uint32_t MAGIC_NMDL = 0x4C444D4E;
constexpr uint32_t MAGIC_NSHP = 0x5048534E;
constexpr uint32_t MAGIC_NTX3 = 0x3358544E;
constexpr uint32_t MAGIC_NMTN = 0x4E544D4E;
constexpr uint32_t MAGIC_NCAM = 0x4D41434E;
constexpr uint32_t MAGIC_NLIT = 0x54494C4E;
constexpr uint32_t MAGIC_NFOG = 0x474F464E;
constexpr uint32_t MAGIC_NMTR = 0x52544D4E;
constexpr uint32_t MAGIC_SONG = 0x474E4F53;
constexpr uint32_t MAGIC_BOOK = 0x4B4F4F42;
constexpr uint32_t MAGIC_PGHD = 0x44484750;
constexpr uint32_t MAGIC_TIM = 0x204D4954;
constexpr uint32_t MAGIC_PROG = 0x474F5250;
constexpr uint32_t MAGIC_CSF = 0x20465343;
constexpr uint32_t MAGIC_FONT = 0x544E4F46;

enum class ChunkType {
    Unknown,
    NOBJ,
    NMDL,
    NSHP,
    NTX3,
    NMTN,
    NCAM,
    NLIT,
    NFOG,
    NMTR,
    SONG,
    BOOK,
    PGHD,
    TIM,
    PROG,
    CSF,
    FONT
};

struct Chunk {
    uint32_t magic;
    uint32_t size;
    char name[64];
    size_t offset;
    ChunkType type;

    std::string GetMagicString() const {
        char buf[5] = { 0 };
        memcpy(buf, &magic, 4);
        return std::string(buf);
    }

    std::string GetTypeString() const {
        switch (type) {
        case ChunkType::NOBJ: return "NOBJ";
        case ChunkType::NMDL: return "NMDL";
        case ChunkType::NSHP: return "NSHP";
        case ChunkType::NTX3: return "NTX3";
        case ChunkType::NMTN: return "NMTN";
        case ChunkType::NCAM: return "NCAM";
        case ChunkType::NLIT: return "NLIT";
        case ChunkType::NFOG: return "NFOG";
        case ChunkType::NMTR: return "NMTR";
        case ChunkType::SONG: return "SONG";
        case ChunkType::BOOK: return "BOOK";
        case ChunkType::PGHD: return "PGHD";
        case ChunkType::TIM:  return "TIM";
        case ChunkType::PROG: return "PROG";
        case ChunkType::CSF:  return "CSF";
        case ChunkType::FONT: return "FONT";
        default: return "????";
        }
    }

    std::string GetDescription() const {
        switch (type) {
        case ChunkType::NOBJ: return "Object Container";
        case ChunkType::NMDL: return "Model Metadata";
        case ChunkType::NSHP: return "3D Mesh";
        case ChunkType::NTX3: return "Texture";
        case ChunkType::NMTN: return "Animation";
        case ChunkType::NCAM: return "Camera";
        case ChunkType::NLIT: return "Light";
        case ChunkType::NFOG: return "Fog";
        case ChunkType::NMTR: return "Material";
        case ChunkType::SONG: return "Audio/Music";
        case ChunkType::BOOK: return "Container";
        case ChunkType::PGHD: return "Polygon Header";
        case ChunkType::TIM:  return "Texture Info";
        case ChunkType::PROG: return "Program";
        case ChunkType::CSF:  return "Audio Container";
        case ChunkType::FONT: return "Font Data";
        default: return "Unknown";
        }
    }
};

struct CameraData {
    float position[3];
    float rotation[3];
    float fov;
    bool parsed;
};

struct LightData {
    char light_name[32];
    uint8_t color[3];
    float direction[3];
    float intensity;
    bool is_ambient;
    bool parsed;
};

struct FogData {
    uint8_t color[3];
    float near_distance;
    float far_distance;
    bool parsed;
};

struct MaterialData {
    uint32_t flags;
    uint8_t diffuse_color[4];
    uint8_t specular_color[4];
    uint8_t ambient_color[4];
    bool parsed;
};

class EFileParser {
public:
    static std::vector<Chunk> Parse(const std::string& filepath);

    static bool ParseCamera(const uint8_t* data, size_t size, CameraData& out);
    static bool ParseLight(const uint8_t* data, size_t size, LightData& out);
    static bool ParseFog(const uint8_t* data, size_t size, FogData& out);
    static bool ParseMaterial(const uint8_t* data, size_t size, MaterialData& out);

private:
    static uint32_t SwapEndian(uint32_t val);
    static uint32_t ReadU32LE(const uint8_t* data);
    static uint32_t ReadU32BE(const uint8_t* data);
    static float ReadFloatBE(const uint8_t* data);

    static ChunkType GetChunkType(uint32_t magic);
};