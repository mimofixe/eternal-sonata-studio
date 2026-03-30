#include "NSHPParser.h"
#include <cstring>
#include <cmath>
#include <iostream>
#include <algorithm>

// Binary helpers
// Binary helpers

float NSHPParser::ReadF32BE(const uint8_t* d) {
    uint32_t u = (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) | (uint32_t(d[2]) << 8) | d[3];
    float f; std::memcpy(&f, &u, 4); return f;
}
uint16_t NSHPParser::ReadU16BE(const uint8_t* d) {
    return static_cast<uint16_t>((uint16_t(d[0]) << 8) | d[1]);
}
int16_t NSHPParser::ReadI16BE(const uint8_t* d) {
    return static_cast<int16_t>((uint16_t(d[0]) << 8) | d[1]);
}
float NSHPParser::HalfToFloat(uint16_t h) {
    uint32_t s = (h >> 15) & 1u, e = (h >> 10) & 0x1Fu, m = h & 0x3FFu, r;
    if (e == 0)  r = (s << 31) | (m << 13);
    else if (e == 31) r = (s << 31) | 0x7F800000u | (m << 13);
    else            r = (s << 31) | ((e + 112u) << 23) | (m << 13);
    float f; std::memcpy(&f, &r, 4); return f;
}
float NSHPParser::DecodeNormal10_10_10(uint32_t packed, int comp) {
    int shift = 22 - comp * 10;
    int val = (packed >> shift) & 0x3FF;
    if (val >= 512) val -= 1024;
    return val / 511.0f;
}

// Restart-aware tristrip → GL_TRIANGLES 
// 0xFFFF = NV_primitive_restart: end current strip, start a new one.
// Winding parity resets with each new strip.
// Blender 2.49's native tristrip importer handles this identically.

void NSHPParser::TristripToTriangles(const uint16_t* strip, size_t len,
    std::vector<uint16_t>& out) {
    std::vector<uint16_t> seg;
    seg.reserve(64);

    auto flush = [&]() {
        for (size_t i = 0; i + 2 < seg.size(); i++) {
            uint16_t a = seg[i], b = seg[i + 1], c = seg[i + 2];
            if (a == b || b == c || a == c) continue;
            if (i & 1) { out.push_back(a); out.push_back(c); out.push_back(b); }
            else { out.push_back(a); out.push_back(b); out.push_back(c); }
        }
        seg.clear();
        };

    for (size_t i = 0; i < len; i++) {
        if (strip[i] == 0xFFFF) { flush(); }
        else { seg.push_back(strip[i]); }
    }
    flush();
}

// Stride validation
// Returns true if stride makes the vertex block end exactly where face section
// headers begin, the strip counts are plausible, and real vertex indices are in range.

bool NSHPParser::TryStride(const uint8_t* data, size_t size,
    size_t vstart, uint16_t vc, uint16_t fc, int stride) {
    const size_t vb = (vstart + size_t(vc) * stride + 3u) & ~3u;
    const size_t fsh = fc * 16u;
    if (vb + fsh > size) return false;

    uint32_t total_raw = 0;
    for (uint16_t s = 0; s < fc; s++) {
        uint16_t sc = ReadU16BE(data + vb + s * 16 + 14);
        // strip count should be at least 3 (one triangle) and not obviously insane
        if (sc < 3 || sc > uint32_t(vc) * 8) return false;
        total_raw += sc;
    }
    if (vb + fsh + total_raw * 2 > size) return false;

    // Quick position sanity: spot-check first and last vertex
    for (int i : {0, vc - 1}) {
        const float px = ReadF32BE(data + vstart + i * stride);
        if (std::isnan(px) || std::isinf(px) || std::abs(px) > 100000.f) return false;
    }

    // Validate that real index values (ignoring 0xFFFF) are within [0, vc)
    const uint8_t* idx_ptr = data + vb + fsh;
    uint16_t max_real = 0;
    for (uint32_t i = 0; i < std::min(total_raw, uint32_t(200)); i++) {
        uint16_t v = ReadU16BE(idx_ptr + i * 2);
        if (v != 0xFFFF) max_real = std::max(max_real, v);
    }
    if (max_real >= vc) return false;

    return true;
}

// Parse 

bool NSHPParser::Parse(const uint8_t* data, size_t size, NSHPMesh& out) {
    if (size < 0x40) {
        std::cerr << "[NSHP] Chunk too small (" << size << ")\n";
        return false;
    }

    char name_buf[17] = {};
    std::memcpy(name_buf, data + 0x08, 16);
    out.name = name_buf;

    out.vertex_count = ReadU16BE(data + 0x1A);
    out.face_section_count = ReadU16BE(data + 0x1E);
    const uint8_t bone_count = data[0x20];
    out.has_bones = (bone_count > 0);
    out.has_skinning = out.has_bones;

    // boneIDList at +0x38 
    size_t off = 0x38;
    out.boneIDList.clear();
    for (uint8_t b = 0; b < bone_count; b++) {
        if (off + 2 > size) break;
        out.boneIDList.push_back(ReadU16BE(data + off));
        off += 2;
    }
    off = (off + 3u) & ~3u;   // align to 4

    const size_t vstart = off;

    //Detect stride 
    int stride = 0;
    if (out.has_bones) {
        stride = 32;  // skinned: always 32
    }
    else {
        // Static geometry: try all known strides in order of commonality
        for (int s : {24, 28, 20, 32, 16}) {
            if (TryStride(data, size, vstart, out.vertex_count, out.face_section_count, s)) {
                stride = s;
                break;
            }
        }
    }

    if (stride == 0) {
        std::cerr << "[NSHP] '" << out.name << "': cannot detect vertex stride\n";
        return false;
    }
    out.vertex_stride = stride;

    // Decode vertices
    out.vertices.clear();
    out.vertices.reserve(out.vertex_count);

    for (uint16_t i = 0; i < out.vertex_count; i++) {
        if (off + stride > size) {
            std::cerr << "[NSHP] Vertex data truncated at vertex " << i << "\n";
            break;
        }
        const uint8_t* vd = data + off;
        Vertex v;

        v.position[0] = ReadF32BE(vd + 0x00);
        v.position[1] = ReadF32BE(vd + 0x04);
        v.position[2] = ReadF32BE(vd + 0x08);

        if (out.has_bones) {
            // stride=32: weights@+0x0C, ids@+0x10, unknown@+0x14, UV f16@+0x1C
            v.bone_weights[0] = vd[0x0C] / 255.f;
            v.bone_weights[1] = vd[0x0D] / 255.f;
            v.bone_weights[2] = vd[0x0E] / 255.f;
            v.bone_weights[3] = vd[0x0F] / 255.f;
            v.bone_ids[0] = vd[0x10]; v.bone_ids[1] = vd[0x11];
            v.bone_ids[2] = vd[0x12]; v.bone_ids[3] = vd[0x13];
            // Attempt i8×3 packed normal from +0x14
            float nx = int8_t(vd[0x14]) / 127.f, ny = int8_t(vd[0x15]) / 127.f, nz = int8_t(vd[0x16]) / 127.f;
            float m2 = nx * nx + ny * ny + nz * nz;
            if (m2 > 0.5f && m2 < 2.0f) { v.normal[0] = nx; v.normal[1] = ny; v.normal[2] = nz; }
            v.uv[0] = HalfToFloat(ReadU16BE(vd + 0x1C));
            v.uv[1] = HalfToFloat(ReadU16BE(vd + 0x1E));

        }
        else if (stride == 16) {
            // pos only; +0x0C = 4 unknown bytes (no normal, no UV in vertex)
            // leave normal and UV as zero

        }
        else {
            // stride 20/24/28: normal 10:10:10:2 @+0x0C, UV i16/32767 @+0x10
            uint32_t packed = (uint32_t(vd[0x0C]) << 24) | (uint32_t(vd[0x0D]) << 16)
                | (uint32_t(vd[0x0E]) << 8) | vd[0x0F];
            v.normal[0] = DecodeNormal10_10_10(packed, 0);
            v.normal[1] = DecodeNormal10_10_10(packed, 1);
            v.normal[2] = DecodeNormal10_10_10(packed, 2);
            v.uv[0] = ReadI16BE(vd + 0x10) / 32767.f;
            v.uv[1] = ReadI16BE(vd + 0x12) / 32767.f;
            // extra bytes at +0x14 (stride 24) or +0x14..+0x1B (stride 28): ignored
        }

        out.vertices.push_back(v);
        off += stride;
    }

    // Face sections
    constexpr size_t FS_HDR = 16u;  // 8×u16
    const size_t sections_base = (off + 3u) & ~3u;  // align after vertex block
    off = sections_base;

    std::vector<uint16_t> raw_counts(out.face_section_count, 0);
    uint32_t total_raw = 0;
    for (uint16_t s = 0; s < out.face_section_count; s++) {
        if (off + FS_HDR > size) break;
        raw_counts[s] = ReadU16BE(data + off + 14);  // 8th u16
        total_raw += raw_counts[s];
        off += FS_HDR;
    }

    // Read all raw tristrip indices (all sections concatenated)
    std::vector<uint16_t> raw;
    raw.reserve(total_raw);
    for (uint32_t i = 0; i < total_raw; i++) {
        if (off + 2 > size) break;
        raw.push_back(ReadU16BE(data + off));
        off += 2;
    }

    // Convert sections using restart-aware tristrip decoder
    out.faceSections.clear();
    out.indices.clear();
    uint32_t cursor = 0;
    for (uint16_t s = 0; s < out.face_section_count; s++) {
        FaceSection fs;
        fs.mat_id = ReadU16BE(data + sections_base + s * FS_HDR);  // 1st u16 = mat index
        fs.index_start = static_cast<uint16_t>(out.indices.size());

        const uint16_t rc = raw_counts[s];
        if (cursor + rc <= uint32_t(raw.size()))
            TristripToTriangles(raw.data() + cursor, rc, out.indices);

        fs.index_count = static_cast<uint16_t>(out.indices.size() - fs.index_start);
        cursor += rc;
        out.faceSections.push_back(fs);
    }

    // Backward-compatible fields
    if (!out.faceSections.empty()) {
        out.texture_id = static_cast<uint8_t>(out.faceSections[0].mat_id & 0xFF);
        out.has_texture = true;  // caller resolves mat_id → NTX3 via NMTR
    }

    std::cout << "[NSHP] '" << out.name << "'"
        << "  stride=" << stride
        << "  verts=" << out.vertices.size()
        << "  tris=" << out.indices.size() / 3
        << (out.has_bones ? "  SKINNED" : "  static") << "\n";

    return !out.vertices.empty();
}

// NMTR parser
// NMTR chunk: 8-byte header ("NMTR" + u32 size), then mat_count × 96 bytes.
// mat_count comes from the NMDL header (w[2]), which the caller must provide.
// We infer it from chunk size: (chunk_size - 8) / 96.

bool NSHPParser::ParseNMTR(const uint8_t* data, size_t size,
    std::vector<NMTREntry>& out) {
    out.clear();
    if (size < 8 + 96) return false;

    const size_t body_size = size - 8;
    const size_t mat_count = body_size / 96;
    out.reserve(mat_count);

    for (size_t m = 0; m < mat_count; m++) {
        const uint8_t* entry = data + 8 + m * 96;
        NMTREntry e;
        // w[5] at +0x0A (5th u16) == 1 → has diffuse texture, index = w[2] at +0x04
        uint16_t w5 = ReadU16BE(entry + 0x0A);
        if (w5 == 1) {
            e.has_diffuse = true;
            e.diffuse_img_id = static_cast<int16_t>(ReadU16BE(entry + 0x04));
        }
        // alpha_img_id: first i16 at +0x30
        int16_t alpha = ReadI16BE(entry + 0x30);
        e.alpha_img_id = alpha;   // -1 = none
        out.push_back(e);
    }

    std::cout << "[NMTR] " << out.size() << " materials parsed\n";
    return !out.empty();
}