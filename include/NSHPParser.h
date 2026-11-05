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

// NMTR material, 96 bytes. Field use follows the game's material setup
// (Xbox 360 sub_82115F80):
//   +0x00 u16 flags        bit 0x0001 specular (+0x2C colour, +0x3A power /256)
//                          bit 0x0008 environment map (+0x42 texture, +0x3C strength)
//   +0x02 u8  layer count  +0x04 layer 0: u32 colour, or u16 texture index when
//                          bit 0 of +0x0B is set (colours RGBA on PS3, ARGB on 360)
//   +0x30 u16 alpha texture (0xFFFF none)
//   +0x34 i16 transparency: opacity = 1 - v / 16384 (used when +0x30 is none)
//   +0x3C i16 environment-map strength / 16384
//   +0x42 u16 environment-map texture (0xFFFF none)
//   +0x44 u16 1 = alpha test
struct NMTREntry {
    bool    has_diffuse = false;
    int16_t diffuse_img_id = -1;
    int16_t alpha_img_id = -1;
    bool    alpha_cutout = false;   // NMTR +0x44==1: needs GL_CLAMP_TO_EDGE
    uint16_t flags = 0;             // NMTR +0x00
    bool    has_color = false;      // layer 0 is a flat colour instead of a texture
    uint8_t color[4] = { 255, 255, 255, 255 };   // RGBA
    float   opacity = 1.f;          // 1 - i16(+0x34)/16384
    int16_t env_img_id = -1;        // +0x42 when flags & 0x0008
    float   env_strength = 0.f;     // i16(+0x3C)/16384
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
    // argb: colours are stored ARGB (Xbox 360, NMDL version 0x82) instead of RGBA.
    static bool ParseNMTR(const uint8_t* data, size_t size,
        std::vector<NMTREntry>& out, bool argb = false);

private:
    static float    ReadF32BE(const uint8_t* d);
    static uint32_t ReadU32BE(const uint8_t* d);
    static uint16_t ReadU16BE(const uint8_t* d);
    static int16_t  ReadI16BE(const uint8_t* d);
    static float    HalfToFloat(uint16_t h);
    static float    DecodeNormal10_10_10(uint32_t packed, int comp);
    static float    DecodeCMP(uint32_t packed, int comp);   // RSX 11:11:10 signed

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