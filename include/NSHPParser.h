#pragma once
#include <vector>
#include <string>
#include <cstdint>

struct Vertex {
    float position[3];
    float normal[3];
    float uv[2];
    float bone_weights[4];
    uint8_t bone_ids[4];
};

struct NSHPMesh {
    std::string name;
    std::vector<Vertex> vertices;
    std::vector<uint16_t> indices;

    uint16_t flags;
    bool has_skinning;
    uint16_t vertex_count;

    uint8_t texture_id;
    bool has_texture;

    NSHPMesh() : flags(0), has_skinning(false), vertex_count(0),
        texture_id(0), has_texture(false) {
    }
};

class NSHPParser {
public:
    static bool Parse(const uint8_t* data, size_t size, NSHPMesh& out);

private:
    static float ReadFloatBE(const uint8_t* data);
    static uint16_t ReadU16BE(const uint8_t* data);
    static uint32_t ReadU32BE(const uint8_t* data);

    static bool ParseStaticMesh(const uint8_t* data, size_t size, NSHPMesh& out);
    static bool ParseSkinnedMesh(const uint8_t* data, size_t size, NSHPMesh& out);
};