#pragma once
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include "NSHPParser.h"

struct NMDLModel {
    std::string                          name;
    std::vector<NSHPMesh>                meshes;
    std::vector<NMTREntry>               materials;
    std::unordered_map<uint16_t, GLuint> mat_to_tex;
    std::unordered_map<uint16_t, GLuint> mat_to_alpha;
    std::unordered_set<uint16_t>         mat_clamp_ids;  // GL_CLAMP_TO_EDGE (stride-24 billboards)
    std::unordered_set<uint16_t>         mat_blend_ids;  // GL_BLEND (DXT5 alpha textures)
    std::vector<GLuint>                  owned_textures;
    glm::vec3  bbox_min{ 1e9f,  1e9f,  1e9f };
    glm::vec3  bbox_max{ -1e9f, -1e9f, -1e9f };
    bool valid = false;

    void Destroy() {
        for (GLuint t : owned_textures) if (t) glDeleteTextures(1, &t);
        owned_textures.clear();
    }
};

class NMDLLoader {
public:
    // external_textures: GPU handles from a paired .p3tex (for map NMDLs).
    // external_dxt5: parallel bool vector — true if that texture is DXT5.
    //   Pass nullptr for internal NTX3 models (handled automatically).
    static bool Load(const uint8_t* file_data, size_t file_size,
        size_t nmdl_offset, NMDLModel& out,
        const std::vector<GLuint>* external_textures = nullptr,
        const std::vector<bool>* external_dxt5 = nullptr);

private:
    // Upload one NTX3 chunk. Sets is_dxt5_out to true if format is DXT5.
    static GLuint    UploadNTX3(const uint8_t* chunk_data, size_t chunk_size,
        bool& is_dxt5_out);
    static uint32_t  ReadU32BE(const uint8_t* d);
    static uint16_t  ReadU16BE(const uint8_t* d);
};