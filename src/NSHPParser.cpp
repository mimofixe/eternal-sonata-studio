#include "NSHPParser.h"
#include <cstring>
#include <iostream>
#include <cmath>

float NSHPParser::ReadFloatBE(const uint8_t* data) {
    uint32_t val = ReadU32BE(data);
    float result;
    memcpy(&result, &val, 4);
    return result;
}

uint16_t NSHPParser::ReadU16BE(const uint8_t* data) {
    return (data[0] << 8) | data[1];
}

uint32_t NSHPParser::ReadU32BE(const uint8_t* data) {
    return (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
}

bool NSHPParser::Parse(const uint8_t* data, size_t size, NSHPMesh& out) {
    if (size < 0x38) {
        std::cerr << "NSHP too small" << std::endl;
        return false;
    }

    char name_buf[16];
    memcpy(name_buf, data + 8, 16);
    name_buf[15] = '\0';
    out.name = std::string(name_buf);

    uint8_t flag_18 = data[0x18];
    uint8_t flag_19 = data[0x19];

    out.flags = (flag_18 << 8) | flag_19;
    out.has_skinning = (flag_19 & 0x04) != 0;

    out.vertex_count = ReadU16BE(data + 0x1a);

    std::cout << "Parsing NSHP: " << out.name << std::endl;
    std::cout << "  Flags: 0x" << std::hex << out.flags << std::dec << std::endl;
    std::cout << "  Skinning: " << (out.has_skinning ? "Yes" : "No") << std::endl;
    std::cout << "  Vertices: " << out.vertex_count << std::endl;

    if (out.has_skinning) {
        return ParseSkinnedMesh(data, size, out);
    }
    else {
        return ParseStaticMesh(data, size, out);
    }
}

bool NSHPParser::ParseStaticMesh(const uint8_t* data, size_t size, NSHPMesh& out) {
    size_t offset = 0x38;

    out.vertices.clear();
    out.indices.clear();

    if (size > 0x1D) {
        out.texture_id = data[0x1D];
        out.has_texture = (out.texture_id > 0);
        std::cout << "  Texture ID: " << (int)out.texture_id << std::endl;
    }

    for (int test_stride : {24, 28, 32, 36, 48}) {
        std::vector<Vertex> test_vertices;
        size_t test_offset = 0x38;

        while (test_offset + test_stride <= size) {
            Vertex v = {};

            v.position[0] = ReadFloatBE(data + test_offset + 0);
            v.position[1] = ReadFloatBE(data + test_offset + 4);
            v.position[2] = ReadFloatBE(data + test_offset + 8);

            if (std::isnan(v.position[0]) || std::isnan(v.position[1]) || std::isnan(v.position[2])) {
                break;
            }
            if (v.position[0] < -10000 || v.position[0] > 10000 ||
                v.position[1] < -10000 || v.position[1] > 10000 ||
                v.position[2] < -10000 || v.position[2] > 10000) {
                break;
            }

            int8_t nx = (int8_t)data[test_offset + 12];
            int8_t ny = (int8_t)data[test_offset + 13];
            int8_t nz = (int8_t)data[test_offset + 14];
            v.normal[0] = nx / 127.0f;
            v.normal[1] = ny / 127.0f;
            v.normal[2] = nz / 127.0f;

            uint16_t u_raw = ReadU16BE(data + test_offset + 16);
            uint16_t v_raw = ReadU16BE(data + test_offset + 18);
            v.uv[0] = u_raw / 65535.0f;
            v.uv[1] = v_raw / 65535.0f;

            test_vertices.push_back(v);
            test_offset += test_stride;
        }

        if (test_vertices.size() > out.vertices.size()) {
            out.vertices = test_vertices;
            std::cout << "  Best stride: " << test_stride << " (" << test_vertices.size() << " vertices)" << std::endl;
        }
    }

    std::cout << "  Parsed " << out.vertices.size() << " vertices (triangle list)" << std::endl;

    if (!out.vertices.empty()) {
        std::cout << "  First vertex: ("
            << out.vertices[0].position[0] << ", "
            << out.vertices[0].position[1] << ", "
            << out.vertices[0].position[2] << ")" << std::endl;
        std::cout << "  Triangles: " << (out.vertices.size() / 3) << std::endl;
    }

    return out.vertices.size() > 0;
}

bool NSHPParser::ParseSkinnedMesh(const uint8_t* data, size_t size, NSHPMesh& out) {
    size_t offset = 0x38;

    out.vertices.clear();
    out.indices.clear();

    if (size > 0x1D) {
        out.texture_id = data[0x1D];
        out.has_texture = (out.texture_id > 0);
        std::cout << "  Texture ID: " << (int)out.texture_id << std::endl;
    }

    int stride = 36;

    for (uint16_t i = 0; i < out.vertex_count && offset + stride <= size; i++) {
        Vertex v = {};

        v.position[0] = ReadFloatBE(data + offset + 0);
        v.position[1] = ReadFloatBE(data + offset + 4);
        v.position[2] = ReadFloatBE(data + offset + 8);

        int8_t nx = (int8_t)data[offset + 12];
        int8_t ny = (int8_t)data[offset + 13];
        int8_t nz = (int8_t)data[offset + 14];
        v.normal[0] = nx / 127.0f;
        v.normal[1] = ny / 127.0f;
        v.normal[2] = nz / 127.0f;

        uint16_t u_raw = ReadU16BE(data + offset + 16);
        uint16_t v_raw = ReadU16BE(data + offset + 18);
        v.uv[0] = u_raw / 65535.0f;
        v.uv[1] = v_raw / 65535.0f;

        if (offset + 24 < size) {
            v.bone_weights[0] = data[offset + 20] / 255.0f;
            v.bone_weights[1] = data[offset + 21] / 255.0f;
            v.bone_weights[2] = data[offset + 22] / 255.0f;
            v.bone_weights[3] = data[offset + 23] / 255.0f;
        }

        if (offset + 28 < size) {
            v.bone_ids[0] = data[offset + 24];
            v.bone_ids[1] = data[offset + 25];
            v.bone_ids[2] = data[offset + 26];
            v.bone_ids[3] = data[offset + 27];
        }

        out.vertices.push_back(v);
        offset += stride;
    }

    std::cout << "  Parsed " << out.vertices.size() << " vertices (skinned)" << std::endl;

    return out.vertices.size() > 0;
}