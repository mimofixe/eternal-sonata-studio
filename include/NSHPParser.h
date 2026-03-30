#pragma once
#include <vector>
#include <string>
#include <cstdint>

// Vertex
// Unified struct for all vertex formats. Unused fields are zero.

struct Vertex {
    float   position[3] = {};
    float   normal[3] = {};   // decoded unit normal (zero when not available)
    float   uv[2] = {};   // primary UV set

    // Skinning (zero for static geometry)
    float   bone_weights[4] = {};
    uint8_t bone_ids[4] = {};   // local → remap via NSHPMesh::boneIDList
};

//FaceSection
// One section = 8×u16 header (last u16 = raw tristrip index count).
// The tristrip uses 0xFFFF restart tokens (NV_primitive_restart).
// NSHPParser converts the restartable tristrip to a GL_TRIANGLES index list.

struct FaceSection {
    uint16_t mat_id = 0;   // NMTR material index (first u16 of header)
    uint16_t index_start = 0;   // first index in NSHPMesh::indices
    uint16_t index_count = 0;   // number of indices (triangles×3)
};

//NSHPMesh

struct NSHPMesh {
    std::string              name;
    std::vector<Vertex>      vertices;
    std::vector<uint16_t>    indices;         // GL_TRIANGLES (restart-aware conversion)
    std::vector<uint16_t>    boneIDList;      // local_bone_idx → NBN2 global index
    std::vector<FaceSection> faceSections;

    uint16_t vertex_count = 0;
    uint16_t face_section_count = 0;
    int      vertex_stride = 0;   // detected stride: 16/20/24/28 (static) or 32 (skinned)

    // Skinning
    bool  has_bones = false;
    bool  has_skinning = false;  // alias of has_bones (API compat)

    // Backward-compatible single texture ref (from faceSections[0].mat_id)
    bool     has_texture = false;
    uint8_t  texture_id = 0;   // NMTR material index for first section
};

//NMTR material entry (96 bytes each)
// From EternalSonataPS3.py:
//   w = g.H(8)              → +0x00: 8×u16
//   if w[5]==1 → has_diffuse, diffuse_img_id = w[2]
//   g.f(4); g.i(4); g.h(8); g.h(8); g.h(8)  → total = 16+16+16+16+16 = 80 bytes body
//   entry total = 8×u16(16) + 80 = 96 bytes

struct NMTREntry {
    bool    has_diffuse = false;
    int16_t diffuse_img_id = -1;   // NTX3 index (-1 = none)
    int16_t alpha_img_id = -1;   // NTX3 index (-1 = none)
};

//NSHPParser
//
// Vertex format summary (all static formats are for bone_count == 0):
//
//  SKINNED  (bone_count > 0)  stride = 32
//    +0x00  f32×3  position XYZ
//    +0x0C  u8×4   bone_weights (/ 255)
//    +0x10  u8×4   bone_indices (local → boneIDList)
//    +0x14  u8×8   unknown (normals candidate i8×3 at +0x14..+0x16)
//    +0x1C  f16×2  UV
//
//  STATIC stride 16 — position only
//    +0x00  f32×3  position XYZ
//    +0x0C  u8×4   unknown (always 00 20 08 00 in observed meshes)
//
//  STATIC stride 20 — pos + packed normal + UV
//    +0x00  f32×3  position XYZ
//    +0x0C  u32    normal 10:10:10:2 snorm BE
//    +0x10  i16×2  UV / 32767
//
//  STATIC stride 24 — pos + packed normal + UV + extra 4 bytes
//    +0x00  f32×3  position XYZ
//    +0x0C  u32    normal 10:10:10:2 snorm BE
//    +0x10  i16×2  UV / 32767
//    +0x14  u8×4   unknown (lightmap UV?)
//
//  STATIC stride 28 — pos + packed normal + UV + extra 8 bytes
//    +0x00  f32×3  position XYZ
//    +0x0C  u32    normal 10:10:10:2 snorm BE
//    +0x10  i16×2  UV / 32767
//    +0x14  u8×8   unknown (tangent? lightmap?)
//
// All face sections use restartable tristrips (0xFFFF = strip restart).

class NSHPParser {
public:
    // Parse a full NSHP chunk. Tristrips (including 0xFFFF restarts) are
    // converted to a GL_TRIANGLES index list in out.indices.
    static bool Parse(const uint8_t* data, size_t size, NSHPMesh& out);

    // Parse an NMTR chunk into a flat list of material entries (96 bytes each).
    // out[i] corresponds to material index i referenced by FaceSection::mat_id.
    static bool ParseNMTR(const uint8_t* data, size_t size,
        std::vector<NMTREntry>& out);

private:
    static float    ReadF32BE(const uint8_t* d);
    static uint16_t ReadU16BE(const uint8_t* d);
    static int16_t  ReadI16BE(const uint8_t* d);
    static float    HalfToFloat(uint16_t h);
    static float    DecodeNormal10_10_10(uint32_t packed, int comp);

    // Returns true if the given stride+offset produces a valid face-section
    // header that fits in the chunk and has plausible index counts.
    static bool TryStride(const uint8_t* data, size_t size,
        size_t vstart, uint16_t vc, uint16_t fc, int stride);

    // Convert a restartable tristrip (0xFFFF = restart) to GL_TRIANGLES.
    static void TristripToTriangles(const uint16_t* strip, size_t len,
        std::vector<uint16_t>& out);
};