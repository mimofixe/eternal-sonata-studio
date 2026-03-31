#include "NMDLLoader.h"
#include "NSHPParser.h"
#include <cstring>
#include <cmath>
#include <iostream>
#include <vector>

// bcdec is a single-header library.  The implementation is defined once in the
// project (typically in NTX3Parser.cpp or a dedicated stub).  Here we only
// include the header for the function declarations.
#include "../libs/bcdec/bcdec.h"

// Binary helpers

uint32_t NMDLLoader::ReadU32BE(const uint8_t* d) {
    return (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) | (uint32_t(d[2]) << 8) | d[3];
}
uint16_t NMDLLoader::ReadU16BE(const uint8_t* d) {
    return static_cast<uint16_t>((uint16_t(d[0]) << 8) | d[1]);
}

//  NTX3 → GL texture 
// NTX3 chunk layout (offsets from chunk start, i.e. from the 'N' of 'NTX3'):
//   +0x00  magic 'NTX3'  (4 bytes)
//   +0x04  chunk_size    u32 BE  (includes magic+size)
//   +0x18  format byte   0x86=DXT1, other=DXT5
//   +0x20  width         u16 BE  (pixels, direct)
//   +0x22  height        u16 BE  (pixels, direct)
//   +0x88  compressed data begins

GLuint NMDLLoader::UploadNTX3(const uint8_t* data, size_t size) {
    if (size < 0x90) return 0;

    const uint8_t fmt = data[0x18];
    const bool    is_bc1 = (fmt == 0x86);
    const uint16_t w = ReadU16BE(data + 0x20);
    const uint16_t h = ReadU16BE(data + 0x22);
    if (w == 0 || h == 0 || w > 4096 || h > 4096) return 0;

    const size_t block_bytes = is_bc1 ? 8u : 16u;
    const size_t blocks_x = (w + 3) / 4;
    const size_t blocks_y = (h + 3) / 4;
    const size_t data_needed = 0x88 + blocks_x * blocks_y * block_bytes;
    if (data_needed > size) return 0;

    const uint8_t* compressed = data + 0x88;

    // Decompress into a flat RGBA8 buffer
    std::vector<uint8_t> rgba(size_t(w) * h * 4, 0);
    const int row_pitch = w * 4;

    for (size_t by = 0; by < blocks_y; by++) {
        for (size_t bx = 0; bx < blocks_x; bx++) {
            const uint8_t* block = compressed + (by * blocks_x + bx) * block_bytes;
            uint8_t* dest = rgba.data() + (by * 4 * w + bx * 4) * 4;
            if (is_bc1)
                bcdec_bc1(block, dest, row_pitch);
            else
                bcdec_bc3(block, dest, row_pitch);
        }
    }

    // Upload to GL
    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glGenerateMipmap(GL_TEXTURE_2D);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
    glBindTexture(GL_TEXTURE_2D, 0);

    return tex;
}

//  Load 

bool NMDLLoader::Load(const uint8_t* file_data, size_t file_size,
    size_t nmdl_offset, NMDLModel& out) {
    out = NMDLModel{};

    if (nmdl_offset + 8 > file_size) return false;
    if (memcmp(file_data + nmdl_offset, "NMDL", 4) != 0) return false;

    const uint32_t nmdl_sz = ReadU32BE(file_data + nmdl_offset + 4);
    const size_t   nmdl_end = std::min(nmdl_offset + size_t(nmdl_sz), file_size);

    // NMDL header: +0x10 = name[16]
    if (nmdl_offset + 0x20 <= file_size) {
        char buf[17] = {};
        std::memcpy(buf, file_data + nmdl_offset + 0x10, 16);
        out.name = buf;
    }

    // ── Single-pass scan of all sub-chunks 
    // Collected in order:  NTX3 first, then NMTR, then NSHP.
    // (In practice they appear in that order in every observed file, so a
    //  single pass and post-processing is safe.)

    std::vector<GLuint>    ntx3_list;   // in discovery order → index = NTX3 slot
    std::vector<NMTREntry> materials;

    size_t pos = nmdl_offset + 8;      // skip NMDL magic + size
    while (pos + 8 <= nmdl_end) {
        const uint8_t* p = file_data + pos;
        const uint32_t sz = ReadU32BE(p + 4);

        if (sz < 8 || pos + sz > nmdl_end) {
            pos += 4;   // advance and try realign
            continue;
        }

        if (memcmp(p, "NTX3", 4) == 0) {
            GLuint tex = UploadNTX3(p, sz);
            ntx3_list.push_back(tex);
            if (tex) std::cout << "[NMDLLoader] NTX3[" << ntx3_list.size() - 1
                << "] uploaded (" << sz << " bytes)\n";
            pos += sz;

        }
        else if (memcmp(p, "NMTR", 4) == 0) {
            NSHPParser::ParseNMTR(p, sz, materials);
            pos += sz;

        }
        else if (memcmp(p, "NSHP", 4) == 0) {
            NSHPMesh mesh;
            if (NSHPParser::Parse(p, sz, mesh) && !mesh.vertices.empty()) {
                // Update bounding box
                for (const auto& v : mesh.vertices) {
                    glm::vec3 vp(v.position[0], v.position[1], v.position[2]);
                    out.bbox_min = glm::min(out.bbox_min, vp);
                    out.bbox_max = glm::max(out.bbox_max, vp);
                }
                out.meshes.push_back(std::move(mesh));
            }
            pos += sz;

        }
        else {
            // Unknown bytes — advance by 4 (same as EFileParser byte-scan).
            // Do NOT skip by sz: inside the NMDL header the bytes are not chunks
            // and a garbage sz value can jump past all sub-chunks.
            pos += 4;
        }
    }

    if (out.meshes.empty()) {
        for (GLuint t : ntx3_list) if (t) glDeleteTextures(1, &t);
        std::cerr << "[NMDLLoader] No meshes found in '" << out.name << "'\n";
        return false;
    }

    out.materials = std::move(materials);
    out.owned_textures = std::move(ntx3_list);

    // Build mat_id → GL texture map 
    // Chain: FaceSection.mat_id → materials[mat_id].diffuse_img_id → ntx3_list[img_id]
    for (size_t m = 0; m < out.materials.size(); m++) {
        const NMTREntry& e = out.materials[m];
        if (!e.has_diffuse) continue;
        const int16_t img_id = e.diffuse_img_id;
        if (img_id >= 0 && img_id < static_cast<int16_t>(out.owned_textures.size())) {
            GLuint tex = out.owned_textures[img_id];
            if (tex) out.mat_to_tex[static_cast<uint16_t>(m)] = tex;
        }
    }

    out.valid = true;
    std::cout << "[NMDLLoader] '" << out.name << "': "
        << out.meshes.size() << " meshes, "
        << out.owned_textures.size() << " textures, "
        << out.mat_to_tex.size() << " textured materials\n";
    return true;
}