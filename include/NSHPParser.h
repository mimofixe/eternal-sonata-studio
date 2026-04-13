#pragma once
#include <vector>
#include <string>
#include <cstdint>

struct Vertex {
    float    position[3];
    float    normal[3];
    float    uv[2];
    float    bone_weights[4];
    uint8_t  bone_ids[4];
};

struct FaceSection {
    uint16_t mat_id = 0;
    uint16_t index_start = 0;
    uint16_t index_count = 0;
};

struct NMTREntry {
    bool    has_diffuse = false;
    int16_t diffuse_img_id = -1;
    int16_t alpha_img_id = -1;
    bool    alpha_cutout = false;   // NMTR +0x44==1: needs GL_CLAMP_TO_EDGE
};

struct NSHPMesh {
    std::string              name;
    std::vector<Vertex>      vertices;
    std::vector<uint16_t>    indices;
    std::vector<FaceSection> faceSections;
    std::vector<uint16_t>    boneIDList;

    int      vertex_stride = 0;
    uint16_t vertex_count = 0;
    uint16_t face_section_count = 0;
    bool     has_bones = false;
    bool     has_skinning = false;
    bool     draw_sequential = false;
    bool     needs_depth_offset = false;  // NSHP +0x18 bit 0x0004: overlay mesh, needs glPolygonOffset
    bool     has_vertex_alpha = false;  // bone_weights[0] repurposed as per-vertex alpha for non-skinned

    // Backward-compatible fields
    uint8_t  texture_id = 0;
    bool     has_texture = false;
};

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

    static void TristripToTriangles(const uint16_t* strip, size_t len,
        std::vector<uint16_t>& out);
    static void DecodeVertices(const uint8_t* data, size_t vstart,
        uint16_t vc, int stride, bool skinned,
        NSHPMesh& out);
    static bool TryStride(const uint8_t* data, size_t chunk_end,
        size_t vstart, uint16_t vc, uint16_t fc,
        int stride, NSHPMesh& out);
    static bool TrySequential(const uint8_t* data, size_t chunk_end,
        size_t vdata_start, uint16_t vc, NSHPMesh& out);
};