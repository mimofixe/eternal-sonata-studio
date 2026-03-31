#pragma once
#include <vector>
#include <string>
#include <cstdint>

// vertex 

struct Vertex {
    float   position[3] = {};
    float   normal[3] = {};   // decoded unit normal (zero when not available)
    float   uv[2] = {};

    // Skinning (zero for static geometry)
    float   bone_weights[4] = {};
    uint8_t bone_ids[4] = {};   // local → remap via NSHPMesh::boneIDList
};

// FaceSection
// One section = 8×u16 header (last u16 = raw tristrip index count).
// The tristrip uses 0xFFFF restart tokens (NV_primitive_restart).

struct FaceSection {
    uint16_t mat_id = 0;   // NMTR material index (first u16 of header)
    uint16_t index_start = 0;   // first index in NSHPMesh::indices
    uint16_t index_count = 0;   // number of indices (triangles×3)
};

// NSHPMesh

struct NSHPMesh {
    std::string              name;
    std::vector<Vertex>      vertices;
    std::vector<uint16_t>    indices;         // GL_TRIANGLES (restart-aware)
    std::vector<uint16_t>    boneIDList;      // local → NBN2 global index
    std::vector<FaceSection> faceSections;

    uint16_t vertex_count = 0;
    uint16_t face_section_count = 0;
    int      vertex_stride = 0;

    // Skinning
    bool has_bones = false;
    bool has_skinning = false;  // alias for API compat

    // When true: no index buffer present.
    // Render with glDrawArrays(GL_TRIANGLES, 0, vertex_count) instead of glDrawElements.
    // Occurs on stride=48 meshes with a 12-byte sequential footer.
    bool draw_sequential = false;

    // Texture ref derived from faceSections[0].mat_id after parsing
    bool    has_texture = false;
    uint8_t texture_id = 0;
};

// NMTR material entry (96 bytes each)

struct NMTREntry {
    bool    has_diffuse = false;
    int16_t diffuse_img_id = -1;
    int16_t alpha_img_id = -1;
};

// NSHPParser
//
// Vertex formats (static bc==0):
//
//   stride 16  — position only (water/collision)
//     +0x00  f32×3  position
//     +0x0C  u8×4   unknown
//
//   stride 20  — pos + packed normal + UV
//     +0x00  f32×3  position
//     +0x0C  u32    normal 10:10:10:2 snorm BE
//     +0x10  i16×2  UV / 32767
//
//   stride 24  — pos + normal + UV + 4 extra
//     +0x00  f32×3  position
//     +0x0C  u32    normal 10:10:10:2 snorm BE
//     +0x10  i16×2  UV / 32767
//     +0x14  u8×4   unknown
//
//   stride 28  — pos + normal + UV + 8 extra
//     +0x00  f32×3  position
//     +0x0C  u32    normal 10:10:10:2 snorm BE
//     +0x10  i16×2  UV / 32767
//     +0x14  u8×8   unknown
//
//   stride 48  — sequential (no index buffer), 12-byte footer
//     +0x00  f32×3  position
//     +0x0C  u32    normal 10:10:10:2 snorm BE
//     +0x10  i16×2  UV / 32767
//     +0x14  u8×20  unknown
//     draw_sequential = true; render with glDrawArrays
//
// Vertex format (skinned bc>0):
//   stride 32  — pos + weights + ids + unknown + UV f16
//     +0x00  f32×3  position
//     +0x0C  u8×4   bone_weights (/255)
//     +0x10  u8×4   bone_indices (local → boneIDList)
//     +0x14  u8×8   unknown (i8×3 normals attempted)
//     +0x1C  f16×2  UV
//
// Pre-vertex block:
//   Some static meshes have a 32-byte block between the boneIDList and the
//   vertex data. The first 4 bytes contain a small u32 count; the remaining
//   28 bytes are zero. The parser detects this automatically.
//
// Face section padding:
//   Chunks may end with up to 4 bytes of alignment padding after the last
//   index. The size check allows leftover ≤ 4 bytes rather than requiring
//   an exact match.

class NSHPParser {
public:
    static bool Parse(const uint8_t* data, size_t size, NSHPMesh& out);
    static bool ParseNMTR(const uint8_t* data, size_t size,
        std::vector<NMTREntry>& out);

private:
    static float    ReadF32BE(const uint8_t* d);
    static uint32_t ReadU32BE(const uint8_t* d);
    static uint16_t ReadU16BE(const uint8_t* d);
    static int16_t  ReadI16BE(const uint8_t* d);
    static float    HalfToFloat(uint16_t h);
    static float    DecodeNormal10_10_10(uint32_t packed, int comp);

    // Try to parse vertex data starting at vstart with the given stride.
    // Reads actual strip counts from face section headers, allows ≤4 bytes
    // of trailing padding.  Returns true and fills out on success.
    static bool TryStride(const uint8_t* data, size_t chunk_end,
        size_t vstart, uint16_t vc, uint16_t fc,
        int stride, NSHPMesh& out);

    // Detect and handle the stride=48 sequential (no-index) mesh format.
    // Returns true if the chunk matches the 12-byte footer pattern.
    static bool TrySequential(const uint8_t* data, size_t chunk_end,
        size_t vdata_start, uint16_t vc, NSHPMesh& out);

    static void DecodeVertices(const uint8_t* data, size_t vstart,
        uint16_t vc, int stride, bool skinned,
        NSHPMesh& out);

    static void TristripToTriangles(const uint16_t* strip, size_t len,
        std::vector<uint16_t>& out);
};