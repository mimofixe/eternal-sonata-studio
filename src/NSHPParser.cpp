#include "NSHPParser.h"
#include <cstring>
#include <cmath>
#include <iostream>
#include <algorithm>

float NSHPParser::ReadF32BE(const uint8_t* d) {
    uint32_t u = (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) | (uint32_t(d[2]) << 8) | d[3];
    float f; std::memcpy(&f, &u, 4); return f;
}
uint32_t NSHPParser::ReadU32BE(const uint8_t* d) {
    return (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) | (uint32_t(d[2]) << 8) | d[3];
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
        if (strip[i] == 0xFFFF) flush();
        else                    seg.push_back(strip[i]);
    }
    flush();
}

void NSHPParser::DecodeVertices(const uint8_t* data, size_t vstart,
    uint16_t vc, int stride, bool skinned, NSHPMesh& out) {
    out.vertices.clear();
    out.vertices.reserve(vc);

    for (uint16_t i = 0; i < vc; i++) {
        const uint8_t* vd = data + vstart + i * stride;
        Vertex v;
        v.position[0] = ReadF32BE(vd + 0x00);
        v.position[1] = ReadF32BE(vd + 0x04);
        v.position[2] = ReadF32BE(vd + 0x08);
        if (skinned) {
            v.bone_weights[0] = vd[0x0C] / 255.f; v.bone_weights[1] = vd[0x0D] / 255.f;
            v.bone_weights[2] = vd[0x0E] / 255.f; v.bone_weights[3] = vd[0x0F] / 255.f;
            v.bone_ids[0] = vd[0x10]; v.bone_ids[1] = vd[0x11];
            v.bone_ids[2] = vd[0x12]; v.bone_ids[3] = vd[0x13];
            float nx = int8_t(vd[0x14]) / 127.f, ny = int8_t(vd[0x15]) / 127.f, nz = int8_t(vd[0x16]) / 127.f;
            float m2 = nx * nx + ny * ny + nz * nz;
            if (m2 > 0.5f && m2 < 2.0f) { v.normal[0] = nx; v.normal[1] = ny; v.normal[2] = nz; }
            v.uv[0] = HalfToFloat(ReadU16BE(vd + 0x1C));
            v.uv[1] = HalfToFloat(ReadU16BE(vd + 0x1E));
        }
        else if (stride == 16) {
            // position only
        }
        else {
            // stride 20/24/28/48: normal 10:10:10:2 @+0x0C, UV @+0x10
            // UV = base (i16/32767) + per-vertex delta (f16 at +0x14).
            // Without the delta every vertex in a quad samples the same texel,
            // producing flat-colour squares on leaf and canopy meshes.
            uint32_t packed = (uint32_t(vd[0x0C]) << 24) | (uint32_t(vd[0x0D]) << 16)
                | (uint32_t(vd[0x0E]) << 8) | vd[0x0F];
            v.normal[0] = DecodeNormal10_10_10(packed, 0);
            v.normal[1] = DecodeNormal10_10_10(packed, 1);
            v.normal[2] = DecodeNormal10_10_10(packed, 2);
            // UV encoding differs by stride:
            // stride=24: the f16 pair at +0x14 are the actual UV coordinates.
            //             The i16 pair at +0x10 is an atlas tile selector (same value
            //             for all 4 verts of one quad — used by the PS3 shader).
            // stride=28: the i16 pair at +0x10 /32767 are the UV coordinates.
            //             The bytes at +0x14..+0x1B encode lightmap/secondary UV.
            // stride=20/48: use i16 at +0x10 (same as stride=28).
            // Stride-24: UV = f16 at +0x14/+0x16 (always).
            // The i16 at +0x10/+0x12 is NOT a UV component — it encodes
            // terrain blend weights or similar per-vertex data.
            // Stride-28/20: UV = i16/32767 at +0x10.
            // stride=24: two UV encoding modes depending on how f16 is used.
            //
            // TILING TERRAIN (|f16| > 1):  UV = i16/32767 + f16
            //   f16 holds large tiling offsets (-10..10). Without i16, the integer
            //   parts collapse to 0 after GL_REPEAT, making the terrain flat.
            //   i16 provides the sub-pixel fractional part for proper 2D coverage.
            //
            // SPRITE / PROP (|f16| <= 1):  UV = f16 only
            //   f16 holds the actual 0..1 corner positions of the sprite/quad.
            //   i16 may vary across vertices (0..1 atlas selector); adding it to f16
            //   pushes the combined UV over 1.0, causing a visible wrap seam.
            if (stride == 24) {
                // Stride-24: UV is always the f16 pair at +0x14/+0x16.
                // The i16 at +0x10/+0x12 is not a UV component.
                v.uv[0] = HalfToFloat(ReadU16BE(vd + 0x14));
                v.uv[1] = HalfToFloat(ReadU16BE(vd + 0x16));
            }
            else {
                v.uv[0] = ReadI16BE(vd + 0x10) / 32767.f;
                v.uv[1] = ReadI16BE(vd + 0x12) / 32767.f;
            }
        }
        out.vertices.push_back(v);
    }
}

bool NSHPParser::TryStride(const uint8_t* data, size_t chunk_end,
    size_t vstart, uint16_t vc, uint16_t fc, int stride, NSHPMesh& out) {
    const size_t fs_off = vstart + size_t(vc) * stride;
    if (fs_off + size_t(fc) * 16 > chunk_end) return false;
    for (int i : {0, vc - 1}) {
        const float px = ReadF32BE(data + vstart + i * stride);
        if (std::isnan(px) || std::isinf(px) || std::abs(px) > 10000.f) return false;
    }
    if (vc > 1) {
        const float px1 = ReadF32BE(data + vstart + stride);
        if (std::isnan(px1) || std::isinf(px1) || std::abs(px1) > 10000.f) return false;
    }
    uint32_t total_raw = 0;
    for (uint16_t s = 0; s < fc; s++) {
        uint16_t sc = ReadU16BE(data + fs_off + s * 16 + 14);
        if (sc < 3 || sc > vc * 8u) return false;
        total_raw += sc;
    }
    const size_t idx_end = fs_off + size_t(fc) * 16 + total_raw * 2;
    if (idx_end > chunk_end || chunk_end - idx_end > 4) return false;
    const size_t idx_base = fs_off + size_t(fc) * 16;
    uint16_t max_real = 0;
    uint32_t sample = std::min(total_raw, uint32_t(200));
    for (uint32_t i = 0; i < sample; i++) {
        uint16_t v = ReadU16BE(data + idx_base + i * 2);
        if (v != 0xFFFF) max_real = std::max(max_real, v);
    }
    if (max_real >= vc) return false;
    // Detect stride-24 UV mode from all vertex data before decoding.
    // If any |f16@+0x14| > 2.0, both i16 and f16 contribute to the final UV.
    DecodeVertices(data, vstart, vc, stride, false, out);
    out.vertex_stride = stride;
    out.faceSections.clear();
    out.indices.clear();
    uint32_t cursor = 0;
    std::vector<uint16_t> raw;
    raw.reserve(total_raw);
    for (uint32_t i = 0; i < total_raw; i++)
        raw.push_back(ReadU16BE(data + idx_base + i * 2));
    std::vector<uint16_t> raw_counts(fc);
    for (uint16_t s = 0; s < fc; s++)
        raw_counts[s] = ReadU16BE(data + fs_off + s * 16 + 14);
    for (uint16_t s = 0; s < fc; s++) {
        FaceSection fs;
        fs.mat_id = ReadU16BE(data + fs_off + s * 16);
        fs.index_start = static_cast<uint16_t>(out.indices.size());
        TristripToTriangles(raw.data() + cursor, raw_counts[s], out.indices);
        fs.index_count = static_cast<uint16_t>(out.indices.size() - fs.index_start);
        cursor += raw_counts[s];
        out.faceSections.push_back(fs);
    }
    return true;
}

bool NSHPParser::TrySequential(const uint8_t* data, size_t chunk_end,
    size_t vdata_start, uint16_t vc, NSHPMesh& out) {
    constexpr int SEQ_STRIDE = 48;
    const size_t footer_start = vdata_start + size_t(vc) * SEQ_STRIDE;
    if (footer_start + 12 != chunk_end) return false;
    if (ReadU16BE(data + footer_start + 4) != vc) return false;
    if (ReadU16BE(data + footer_start + 6) != 0) return false;
    if (ReadU16BE(data + footer_start + 8) != 0) return false;
    if (ReadU16BE(data + footer_start + 10) != 0) return false;
    const float px = ReadF32BE(data + vdata_start);
    if (std::isnan(px) || std::isinf(px) || std::abs(px) > 10000.f) return false;
    DecodeVertices(data, vdata_start, vc, SEQ_STRIDE, false, out);
    out.vertex_stride = SEQ_STRIDE;
    out.draw_sequential = true;
    FaceSection fs;
    fs.mat_id = ReadU16BE(data + footer_start);
    fs.index_start = 0;
    fs.index_count = 0;
    out.faceSections.push_back(fs);
    return true;
}

bool NSHPParser::Parse(const uint8_t* data, size_t size, NSHPMesh& out) {
    if (size < 0x40) { std::cerr << "[NSHP] Chunk too small (" << size << ")\n"; return false; }
    char name_buf[17] = {};
    std::memcpy(name_buf, data + 0x08, 16);
    out.name = name_buf;
    out.vertex_count = ReadU16BE(data + 0x1A);
    out.face_section_count = ReadU16BE(data + 0x1E);
    const uint8_t bone_count = data[0x20];
    out.has_bones = (bone_count > 0);
    out.has_skinning = out.has_bones;
    size_t off = 0x38;
    out.boneIDList.clear();
    for (uint8_t b = 0; b < bone_count; b++) {
        if (off + 2 > size) break;
        out.boneIDList.push_back(ReadU16BE(data + off));
        off += 2;
    }
    off = (off + 3u) & ~3u;
    const size_t vdata_base = off;
    const size_t chunk_end = std::min(static_cast<size_t>(ReadU32BE(data + 4)), size);
    out.draw_sequential = false;
    out.vertices.clear();
    out.indices.clear();
    out.faceSections.clear();
    bool parsed = false;
    if (out.has_bones) {
        parsed = TryStride(data, chunk_end, vdata_base, out.vertex_count,
            out.face_section_count, 32, out);
        if (parsed) {
            DecodeVertices(data, vdata_base, out.vertex_count, 32, true, out);
            out.vertex_stride = 32;
        }
    }
    else {
        bool has_pre_block = false;
        if (vdata_base + 32 < chunk_end) {
            uint32_t hint = ReadU16BE(data + vdata_base) << 16 | ReadU16BE(data + vdata_base + 2);
            if (data[vdata_base] == 0 && data[vdata_base + 1] == 0) {
                bool all_zero = true;
                for (int z = 4; z < 32 && all_zero; z++)
                    if (data[vdata_base + z] != 0) all_zero = false;
                has_pre_block = all_zero;
            }
            (void)hint;
        }
        const size_t vstart_a = vdata_base;
        const size_t vstart_b = vdata_base + 32;
        const int STATIC_STRIDES[] = { 24, 28, 20, 16, 0 };
        auto try_pos = [&](size_t vstart) -> bool {
            if (TrySequential(data, chunk_end, vstart, out.vertex_count, out)) return true;
            for (const int* sp = STATIC_STRIDES; *sp; ++sp)
                if (TryStride(data, chunk_end, vstart, out.vertex_count,
                    out.face_section_count, *sp, out)) return true;
            return false;
            };
        if (try_pos(vstart_a)) parsed = true;
        else if (has_pre_block && try_pos(vstart_b)) parsed = true;
        else if (!has_pre_block && try_pos(vstart_b)) parsed = true;
    }
    if (!parsed) { std::cerr << "[NSHP] '" << out.name << "': failed to detect vertex format\n"; return false; }
    if (!out.faceSections.empty()) {
        out.texture_id = static_cast<uint8_t>(out.faceSections[0].mat_id & 0xFF);
        out.has_texture = true;
    }
    std::cout << "[NSHP] '" << out.name << "'"
        << "  stride=" << out.vertex_stride
        << "  verts=" << out.vertices.size()
        << "  tris=" << (out.draw_sequential ? out.vertices.size() / 3 : out.indices.size() / 3)
        << (out.has_bones ? "  SKINNED" : "  static")
        << (out.draw_sequential ? "  SEQ-DRAW" : "")
        << "\n";
    return !out.vertices.empty();
}

bool NSHPParser::ParseNMTR(const uint8_t* data, size_t size,
    std::vector<NMTREntry>& out) {
    out.clear();
    if (size < 8 + 96) return false;
    const size_t mat_count = (size - 8) / 96;
    out.reserve(mat_count);
    for (size_t m = 0; m < mat_count; m++) {
        const uint8_t* entry = data + 8 + m * 96;
        NMTREntry e;
        uint16_t w5 = ReadU16BE(entry + 0x0A);
        if (w5 == 1) {
            e.has_diffuse = true;
            e.diffuse_img_id = static_cast<int16_t>(ReadU16BE(entry + 0x04));
        }
        e.alpha_img_id = ReadI16BE(entry + 0x30);
        e.alpha_cutout = (ReadU16BE(entry + 0x44) == 1);
        out.push_back(e);
    }
    std::cout << "[NMTR] " << out.size() << " materials\n";
    return !out.empty();
}