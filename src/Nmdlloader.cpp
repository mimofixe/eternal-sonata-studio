#include "NMDLLoader.h"
#include "NSHPParser.h"
#include <cstring>
#include <iostream>
#include <vector>
#include "../libs/bcdec/bcdec.h"

uint32_t NMDLLoader::ReadU32BE(const uint8_t* d) {
    return (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) | (uint32_t(d[2]) << 8) | d[3];
}
uint16_t NMDLLoader::ReadU16BE(const uint8_t* d) {
    return static_cast<uint16_t>((uint16_t(d[0]) << 8) | d[1]);
}

GLuint NMDLLoader::UploadNTX3(const uint8_t* data, size_t size, bool& is_dxt5_out) {
    is_dxt5_out = false;
    if (size < 0x90) return 0;

    const uint8_t  fmt = data[0x18];
    const bool     is_bc1 = ((fmt & 0xDF) == 0x86);
    is_dxt5_out = !is_bc1;
    const uint16_t w = ReadU16BE(data + 0x20);
    const uint16_t h = ReadU16BE(data + 0x22);
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return 0;

    const size_t block_bytes = is_bc1 ? 8u : 16u;
    const size_t blocks_x = (w + 3) / 4, blocks_y = (h + 3) / 4;
    if (0x88 + blocks_x * blocks_y * block_bytes > size) return 0;

    const uint8_t* compressed = data + 0x88;
    std::vector<uint8_t> rgba(size_t(w) * h * 4, 0);
    const int row_pitch = w * 4;

    for (size_t by = 0; by < blocks_y; by++)
        for (size_t bx = 0; bx < blocks_x; bx++) {
            const uint8_t* block = compressed + (by * blocks_x + bx) * block_bytes;
            uint8_t* dest = rgba.data() + (by * 4 * w + bx * 4) * 4;
            if (is_bc1) bcdec_bc1(block, dest, row_pitch);
            else        bcdec_bc3(block, dest, row_pitch);
        }

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);
    return tex;
}

bool NMDLLoader::Load(const uint8_t* file_data, size_t file_size,
    size_t nmdl_offset, NMDLModel& out,
    const std::vector<GLuint>* external_textures,
    const std::vector<bool>* external_dxt5) {
    out = NMDLModel{};

    if (nmdl_offset + 8 > file_size) return false;
    if (memcmp(file_data + nmdl_offset, "NMDL", 4) != 0) return false;

    const uint32_t nmdl_sz = ReadU32BE(file_data + nmdl_offset + 4);
    const size_t   nmdl_end = std::min(nmdl_offset + size_t(nmdl_sz), file_size);

    if (nmdl_offset + 0x20 <= file_size) {
        char buf[17] = {};
        std::memcpy(buf, file_data + nmdl_offset + 0x10, 16);
        out.name = buf;
    }

    std::vector<GLuint>    ntx3_list;
    std::vector<bool>      ntx3_dxt5;
    std::vector<NMTREntry> materials;
    size_t first_nmtr_count = 0;

    size_t pos = nmdl_offset + 8;
    while (pos + 8 <= nmdl_end) {
        const uint8_t* p = file_data + pos;
        const uint32_t sz = ReadU32BE(p + 4);
        if (sz < 8 || pos + sz > nmdl_end) { pos += 4; continue; }

        if (memcmp(p, "NTX3", 4) == 0) {
            if (!external_textures) {
                bool dxt5 = false;
                GLuint tex = UploadNTX3(p, sz, dxt5);
                ntx3_list.push_back(tex);
                ntx3_dxt5.push_back(dxt5);
                if (tex) std::cout << "[NMDLLoader] NTX3[" << ntx3_list.size() - 1
                    << "] " << (dxt5 ? "DXT5" : "DXT1") << " (" << sz << " bytes)\n";
            }
            pos += sz;

        }
        else if (memcmp(p, "NMTR", 4) == 0) {
            std::vector<NMTREntry> local_mats;
            NSHPParser::ParseNMTR(p, sz, local_mats);
            if (first_nmtr_count == 0) first_nmtr_count = local_mats.size();
            for (auto& e : local_mats) materials.push_back(e);
            pos += sz;

        }
        else if (memcmp(p, "NSHP", 4) == 0) {
            NSHPMesh mesh;
            if (NSHPParser::Parse(p, sz, mesh) && !mesh.vertices.empty()) {
                for (const auto& v : mesh.vertices) {
                    glm::vec3 vp(v.position[0], v.position[1], v.position[2]);
                    out.bbox_min = glm::min(out.bbox_min, vp);
                    out.bbox_max = glm::max(out.bbox_max, vp);
                }
                out.meshes.push_back(std::move(mesh));
            }
            pos += sz;

        }
        else if (memcmp(p, "NOBJ", 4) == 0) {
            pos += sz;
        }
        else {
            pos += 4;
        }
    }

    if (out.meshes.empty()) {
        for (GLuint t : ntx3_list) if (t) glDeleteTextures(1, &t);
        std::cerr << "[NMDLLoader] No meshes in '" << out.name << "'\n";
        return false;
    }

    out.materials = std::move(materials);
    out.owned_textures = std::move(ntx3_list);

    const std::vector<GLuint>& tex_source =
        external_textures ? *external_textures : out.owned_textures;
    const std::vector<bool>* dxt5_source =
        external_textures ? external_dxt5 : &ntx3_dxt5;

    const size_t mat_limit = (external_textures && first_nmtr_count > 0)
        ? first_nmtr_count : out.materials.size();

    for (size_t m = 0; m < mat_limit; m++) {
        const NMTREntry& e = out.materials[m];

        if (e.has_diffuse) {
            const int16_t img_id = e.diffuse_img_id;
            if (img_id >= 0 && size_t(img_id) < tex_source.size()) {
                GLuint tex = tex_source[img_id];
                if (tex) {
                    out.mat_to_tex[static_cast<uint16_t>(m)] = tex;
                    // DXT5 textures need GL_BLEND (full 8-bit alpha, not punch-through)
                    if (dxt5_source && size_t(img_id) < dxt5_source->size()
                        && (*dxt5_source)[img_id])
                        out.mat_blend_ids.insert(static_cast<uint16_t>(m));
                }
            }
        }

        if (e.alpha_img_id >= 0 && size_t(e.alpha_img_id) < tex_source.size()) {
            GLuint tex = tex_source[e.alpha_img_id];
            if (tex) out.mat_to_alpha[static_cast<uint16_t>(m)] = tex;
        }

        if (e.alpha_cutout)
            out.mat_clamp_ids.insert(static_cast<uint16_t>(m));
    }

    out.valid = true;
    std::cout << "[NMDLLoader] '" << out.name << "': "
        << out.meshes.size() << " meshes, "
        << tex_source.size() << " textures"
        << (external_textures ? " (external)" : " (internal)")
        << ", " << out.mat_to_tex.size() << " textured"
        << ", " << out.mat_blend_ids.size() << " DXT5-blend\n";
    return true;
}