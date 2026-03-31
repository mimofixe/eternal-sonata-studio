#pragma once
#include <vector>
#include <string>
#include <unordered_map>
#include <cstdint>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include "NSHPParser.h"

// NMDLModel
// Returned by NMDLLoader::Load.  CPU mesh data + GPU texture handles.
// After passing to Viewport3D::LoadModel, the texture GLuints are owned by the
// viewport; owned_textures is empty-moved and Destroy() becomes a no-op.

struct NMDLModel {
    std::string                          name;
    std::vector<NSHPMesh>                meshes;       // CPU mesh data
    std::vector<NMTREntry>               materials;    // indexed by FaceSection::mat_id
    std::unordered_map<uint16_t, GLuint>  mat_to_tex;  // mat_id → GL texture (0=none)
    std::vector<GLuint>                  owned_textures;
    glm::vec3  bbox_min{ 1e9f,  1e9f,  1e9f };
    glm::vec3  bbox_max{ -1e9f, -1e9f, -1e9f };
    bool valid = false;

    // Release owned GPU textures.  Call only if LoadModel was NOT called.
    void Destroy() {
        for (GLuint t : owned_textures) if (t) glDeleteTextures(1, &t);
        owned_textures.clear();
    }
};

// NMDLLoader
// Scans an NMDL chunk inside a game file, parses all NSHP meshes, uploads
// NTX3 textures to the GPU, and builds the mat_id → GLuint mapping.

class NMDLLoader {
public:
    // file_data / file_size: the entire source file in memory.
    // nmdl_offset: byte offset of the NMDL magic inside that file.
    // Returns true and populates 'out' on success.
    static bool Load(const uint8_t* file_data, size_t file_size,
        size_t nmdl_offset, NMDLModel& out);

private:
    // Decompress and upload one NTX3 chunk to the GPU.
    // Returns the GL texture handle, or 0 on failure.
    static GLuint UploadNTX3(const uint8_t* chunk_data, size_t chunk_size);

    static uint32_t ReadU32BE(const uint8_t* d);
    static uint16_t ReadU16BE(const uint8_t* d);
};