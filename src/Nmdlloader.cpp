#include "NMDLLoader.h"
#include "NSHPParser.h"
#include "Ntx2parser.h"
#include "Ntexparser.h"
#include <algorithm>
#include <cstring>
#include <iostream>
#include <unordered_map>
#include <vector>
#include "../libs/bcdec/bcdec.h"

namespace {

    GLuint UploadRGBA8(const std::vector<uint8_t>& rgba, int w, int h) {
        if (rgba.empty() || w <= 0 || h <= 0) return 0;
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

} // namespace

uint32_t NMDLLoader::ReadU32BE(const uint8_t* d) {
    return (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) | (uint32_t(d[2]) << 8) | d[3];
}
uint16_t NMDLLoader::ReadU16BE(const uint8_t* d) {
    return static_cast<uint16_t>((uint16_t(d[0]) << 8) | d[1]);
}

bool NMDLLoader::NeedsAlphaBlend(const std::vector<uint8_t>& rgba) {
    // Two independent conditions, because DXT5 alpha is noisy in both
    // directions and either test alone gives false positives:
    //
    //  1. Something must actually be see-through. A nominally opaque DXT5
    //     surface interpolates a few percent of its texels down into the 220s
    //     -- etc21_v1's two 512x512 body maps bottom out at 225, which is 12%
    //     transparency, i.e. invisible. Requiring the darkest texel in the
    //     whole texture to fall below OPAQUE_FLOOR rejects that noise.
    //  2. It must not be punch-through. Foliage/cutout maps are 0-or-255 by
    //     design and belong on the alpha-discard path (depth writes on), so
    //     the texture also has to carry texels in the interior band.
    constexpr uint8_t OPAQUE_FLOOR = 200;
    constexpr uint8_t BAND_LO = 24, BAND_HI = 232;

    uint8_t min_alpha = 255;
    bool    has_interior = false;
    for (size_t i = 3; i < rgba.size(); i += 4) {
        const uint8_t a = rgba[i];
        if (a < min_alpha) min_alpha = a;
        if (a >= BAND_LO && a <= BAND_HI) has_interior = true;
    }
    return min_alpha < OPAQUE_FLOOR && has_interior;
}

GLuint NMDLLoader::UploadNTX3(const uint8_t* data, size_t size, bool& is_dxt5_out) {
    is_dxt5_out = false;
    if (size < 0x90) return 0;

    const uint8_t  fmt = data[0x18];
    const bool     is_bc1 = ((fmt & 0xDF) == 0x86);
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
            if (is_bc1) {
                bcdec_bc1(block, dest, row_pitch);
            }
            else {
                // PS3 Namco DXT5: colour block first (0-7), alpha second (8-15).
                uint8_t swapped[16];
                std::memcpy(swapped, block + 8, 8);
                std::memcpy(swapped + 8, block, 8);
                bcdec_bc3(swapped, dest, row_pitch);
            }
        }

    is_dxt5_out = NeedsAlphaBlend(rgba);
    GLuint tex = UploadRGBA8(rgba, w, h);
    return tex;
}

GLuint NMDLLoader::UploadNTX2(const uint8_t* chunk_data, size_t chunk_size,
    size_t chunk_abs_offset, bool& is_dxt5_out) {
    is_dxt5_out = false;

    NTX2Info info;
    std::vector<uint8_t> rgba = Ntx2Parser::Decode(chunk_data, chunk_size, info, chunk_abs_offset);
    if (rgba.empty()) return 0; // only DXT1 decodes today; see Ntx2Parser::Decode

    is_dxt5_out = NeedsAlphaBlend(rgba);
    return UploadRGBA8(rgba, info.width, info.height);
}

GLuint NMDLLoader::UploadNTEX(const uint8_t* chunk_data, size_t chunk_size,
    bool& is_dxt5_out) {
    is_dxt5_out = false;

    NTEXInfo info;
    std::vector<uint8_t> rgba = NtexParser::Decode(chunk_data, chunk_size, info);
    if (rgba.empty()) return 0;

    is_dxt5_out = NeedsAlphaBlend(rgba);
    return UploadRGBA8(rgba, info.width, info.height);
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

    // Xbox 360 NMDLs (version byte 0x82) store material colours ARGB, PS3 (0x83) RGBA.
    const bool argb = nmdl_offset + 9 <= file_size && file_data[nmdl_offset + 8] == 0x82;

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
        else if (memcmp(p, "NTX2", 4) == 0) {
            // Xbox 360 non-map models can embed NTX2 directly, the same
            // way PS3 models embed NTX3. Same texture-index slot either way.
            if (!external_textures) {
                bool dxt5 = false;
                GLuint tex = UploadNTX2(p, sz, pos, dxt5);
                ntx3_list.push_back(tex);
                ntx3_dxt5.push_back(dxt5);
                if (tex) std::cout << "[NMDLLoader] NTX2[" << ntx3_list.size() - 1
                    << "] (" << sz << " bytes)\n";
            }
            pos += sz;

        }
        else if (memcmp(p, "NTEX", 4) == 0) {
            // NTEX is the third texture chunk kind (a plain PC DDS, used by
            // the Xbox 360 build for lightmaps and for whole sub-models).
            // It shares the NMDL texture index space with NTX3/NTX2, so it
            // must occupy a slot even when it doesn't decode - otherwise
            // every material past it lands on the wrong texture.
            if (!external_textures) {
                bool dxt5 = false;
                GLuint tex = UploadNTEX(p, sz, dxt5);
                ntx3_list.push_back(tex);
                ntx3_dxt5.push_back(dxt5);
                std::cout << "[NMDLLoader] NTEX[" << ntx3_list.size() - 1 << "] "
                    << (tex ? (dxt5 ? "DXT5" : "DXT1/DXT3")
                        : "undecoded (slot reserved)")
                    << " (" << sz << " bytes)\n";
            }
            pos += sz;

        }
        else if (memcmp(p, "NMTR", 4) == 0) {
            std::vector<NMTREntry> local_mats;
            NSHPParser::ParseNMTR(p, sz, local_mats, argb);
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

    // diffuse_img_id is a direct index into the model's texture chunks in
    // file order, counting every NTX3/NTX2/NTEX chunk. Gaps in the ids a
    // model's materials reference are not dropped textures to be closed up
    // - they're slots holding a texture nothing draws with (typically an
    // NTEX lightmap). Verified on the Xbox 360 build: xlp90.bop's 17
    // materials reference {0..5,7..10} against 6 NTX2 + 1 NTEX + 4 NTX2,
    // and the unreferenced id 6 is exactly the NTEX slot. Same alignment
    // holds across adg90/agg90/bel90/cst90/gls90 - every unreferenced id
    // below the max lands on an NTEX or an otherwise unused texture, and
    // no id ever runs past the end of the slot list.
    for (size_t m = 0; m < mat_limit; m++) {
        const NMTREntry& e = out.materials[m];

        if (e.has_diffuse) {
            const int idx = e.diffuse_img_id;
            if (idx >= 0 && size_t(idx) < tex_source.size()) {
                GLuint tex = tex_source[idx];
                if (tex) {
                    out.mat_to_tex[static_cast<uint16_t>(m)] = tex;
                    // DXT5 textures need GL_BLEND (full 8-bit alpha, not punch-through)
                    if (dxt5_source && size_t(idx) < dxt5_source->size()
                        && (*dxt5_source)[idx])
                        out.mat_blend_ids.insert(static_cast<uint16_t>(m));
                }
            }
        }

        if (e.alpha_img_id >= 0) {
            const int idx = e.alpha_img_id;
            if (size_t(idx) < tex_source.size()) {
                GLuint tex = tex_source[idx];
                if (tex) out.mat_to_alpha[static_cast<uint16_t>(m)] = tex;
            }
        }

        if (e.alpha_cutout)
            out.mat_clamp_ids.insert(static_cast<uint16_t>(m));

        if (e.env_img_id >= 0 && size_t(e.env_img_id) < tex_source.size()) {
            GLuint tex = tex_source[e.env_img_id];
            if (tex) out.mat_to_env[static_cast<uint16_t>(m)] = tex;
        }
    }

    // NMTR +0x44 says "alpha test", not "clamp". Clamping is only a safe
    // stand-in for it while the geometry's UVs stay inside the [0,1] tile:
    // a material whose UVs run outside that range is tiling on purpose, and
    // clamping collapses every one of its samples onto one edge texel.
    //
    // bosFGA_v2's monocle in cfdata/e1190.e is the case that exposed this.
    // 'megane01' is material 9 (+0x44==1) with u in [1.02, 1.99] -- the eye
    // patch lives in the second horizontal wrap of bosFGAv1_08.tga. Clamped,
    // all 337 vertices sample column 511, which is flat grey down to y~140
    // and alpha-0 black below: the lens rendered as a plain grey patch
    // covering only the top half of the eye. The standalone NSHP view draws
    // untextured, which is why the same mesh looked correct there.
    //
    // Margin is deliberately loose: UVs a hair past the tile edge are
    // rounding, not tiling, and clamp and repeat look the same there anyway.
    if (!out.mat_clamp_ids.empty()) {
        constexpr float TILE_LO = -0.05f, TILE_HI = 1.05f;
        std::unordered_set<uint16_t> tiling;
        for (const NSHPMesh& mesh : out.meshes) {
            for (const FaceSection& sec : mesh.faceSections) {
                if (!out.mat_clamp_ids.count(sec.mat_id)) continue;
                if (tiling.count(sec.mat_id)) continue;
                auto check = [&](const Vertex& v) {
                    if (v.uv[0] < TILE_LO || v.uv[0] > TILE_HI ||
                        v.uv[1] < TILE_LO || v.uv[1] > TILE_HI)
                        tiling.insert(sec.mat_id);
                    };
                if (mesh.draw_sequential || sec.index_count == 0) {
                    for (const Vertex& v : mesh.vertices) check(v);
                }
                else {
                    const size_t end = std::min<size_t>(
                        size_t(sec.index_start) + sec.index_count,
                        mesh.indices.size());
                    for (size_t i = sec.index_start; i < end; i++) {
                        const uint16_t vi = mesh.indices[i];
                        if (vi < mesh.vertices.size()) check(mesh.vertices[vi]);
                    }
                }
            }
        }
        for (uint16_t id : tiling) {
            out.mat_clamp_ids.erase(id);
            std::cout << "[NMDLLoader] material " << id
                << ": UVs outside [0,1], keeping GL_REPEAT\n";
        }
    }

    out.valid = true;
    std::cout << "[NMDLLoader] '" << out.name << "': "
        << out.meshes.size() << " meshes, "
        << tex_source.size() << " textures"
        << (external_textures ? " (external)" : " (internal)")
        << ", " << out.mat_to_tex.size() << " textured"
        << ", " << out.mat_blend_ids.size() << " DXT5-blend"
        << ", " << out.mat_to_env.size() << " env-mapped\n";
    return true;
}