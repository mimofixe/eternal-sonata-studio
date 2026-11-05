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
    std::unordered_set<uint16_t>         mat_blend_ids;  // GL_BLEND (textures with translucent texels)
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
    // True when decoded RGBA8 contains texels that are neither fully opaque nor
    // fully transparent, i.e. the texture actually needs GL_BLEND.
    //
    // Being DXT5 is NOT the same thing. Character textures are routinely stored
    // as DXT5 (etc21_v1, the squirrel in e0020_020.e, is five DXT5 NTEX chunks)
    // with every texel at alpha 255. Treating "is DXT5" as "needs blending" put
    // those meshes on the blend path, which also turns off depth writes - so the
    // far side of the model draws over the near side and the whole thing reads
    // as half-transparent. Punch-through (0/255 only) textures also don't want
    // blending: they belong on the alpha-discard path with depth writes on.
    static bool NeedsAlphaBlend(const std::vector<uint8_t>& rgba);

    // external_textures: GPU handles from a paired .p3tex (for map NMDLs).
    // external_dxt5: parallel bool vector — true if that texture needs blending
    //   (see NeedsAlphaBlend).
    //   Pass nullptr for internal NTX3 models (handled automatically).
    static bool Load(const uint8_t* file_data, size_t file_size,
        size_t nmdl_offset, NMDLModel& out,
        const std::vector<GLuint>* external_textures = nullptr,
        const std::vector<bool>* external_dxt5 = nullptr);

private:
    // Upload one NTX3 chunk (PS3). Sets needs_blend_out per NeedsAlphaBlend.
    static GLuint    UploadNTX3(const uint8_t* chunk_data, size_t chunk_size,
        bool& is_dxt5_out);
    // Upload one embedded NTX2 chunk (Xbox 360). chunk_abs_offset is the
    // chunk's absolute file offset, needed for NTX2's page-alignment math.
    // Only DXT1 decodes today (see Ntx2Parser::Decode); returns 0 otherwise.
    static GLuint    UploadNTX2(const uint8_t* chunk_data, size_t chunk_size,
        size_t chunk_abs_offset, bool& is_dxt5_out);
    // Upload one embedded NTEX chunk (Xbox 360): a plain PC DDS. Returns 0
    // for the uncompressed 16bpp lightmap variant, which still occupies a
    // texture slot.
    static GLuint    UploadNTEX(const uint8_t* chunk_data, size_t chunk_size,
        bool& is_dxt5_out);
    static uint32_t  ReadU32BE(const uint8_t* d);
    static uint16_t  ReadU16BE(const uint8_t* d);
};