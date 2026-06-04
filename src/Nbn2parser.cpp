#include "NBN2Parser.h"
#include <cstring>
#include <cmath>
#include <iostream>
#include <glm/gtc/matrix_transform.hpp>  // glm::transpose

static constexpr size_t BONE_STRIDE = 64;
static constexpr size_t CHUNK_HEADER = 8;  // "NBN2" + u32 size

//Binary helpers

int16_t NBN2Parser::ReadI16BE(const uint8_t* d) {
    return static_cast<int16_t>((uint16_t(d[0]) << 8) | d[1]);
}
uint16_t NBN2Parser::ReadU16BE(const uint8_t* d) {
    return static_cast<uint16_t>((uint16_t(d[0]) << 8) | d[1]);
}
float NBN2Parser::ReadF32BE(const uint8_t* d) {
    uint32_t u = (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16)
        | (uint32_t(d[2]) << 8) | uint32_t(d[3]);
    float f; std::memcpy(&f, &u, 4); return f;
}

//Rotation matrix

// Euler XYZ: result = Rz * Ry * Rx
// GLM mat3 is column-major: mat3(col0, col1, col2).
// Verified against Blender 2.49 Euler().toMatrix() using pcalg_v1.p3obj data.
glm::mat3 NBN2Parser::EulerXYZ(float rx, float ry, float rz) {
    float cx = std::cos(rx), sx = std::sin(rx);
    float cy = std::cos(ry), sy = std::sin(ry);
    float cz = std::cos(rz), sz = std::sin(rz);

    glm::mat3 Rx(
        1.f, 0.f, 0.f,   // col0
        0.f, cx, sx,   // col1
        0.f, -sx, cx    // col2
    );
    glm::mat3 Ry(
        cy, 0.f, -sy,   // col0
        0.f, 1.f, 0.f,   // col1
        sy, 0.f, cy    // col2
    );
    glm::mat3 Rz(
        cz, sz, 0.f,   // col0
        -sz, cz, 0.f,   // col1
        0.f, 0.f, 1.f    // col2
    );
    return Rz * Ry * Rx;
}

//Parse

bool NBN2Parser::Parse(const uint8_t* data, size_t size, std::vector<Bone>& out) {
    out.clear();

    if (size < CHUNK_HEADER + BONE_STRIDE) {
        std::cerr << "[NBN2] Chunk too small: " << size << " bytes\n";
        return false;
    }

    const uint8_t* body = data + CHUNK_HEADER;
    const size_t   body_size = size - CHUNK_HEADER;
    const size_t   max_bones = body_size / BONE_STRIDE;

    out.reserve(max_bones);

    for (size_t i = 0; i < max_bones; i++) {
        const uint8_t* b = body + i * BONE_STRIDE;

        // Empty name slot = end of table
        if (b[0] == '\0') break;

        char name_buf[17] = {};
        std::memcpy(name_buf, b, 16);

        Bone bone;
        bone.name = name_buf;
        bone.flags = ReadU16BE(b + 0x10);
        bone.parent_idx = ReadI16BE(b + 0x12);
        bone.child_idx = ReadI16BE(b + 0x14);
        bone.sibling_idx = ReadI16BE(b + 0x16);
        bone.rot_x = ReadI16BE(b + 0x18) / 4096.0f;  // rot1: i16 → radians
        bone.rot_y = ReadI16BE(b + 0x1A) / 4096.0f;
        bone.rot_z = ReadI16BE(b + 0x1C) / 4096.0f;
        bone.rot2_x = ReadI16BE(b + 0x24) / 4096.0f;  // rot2: secondary rotation
        bone.rot2_y = ReadI16BE(b + 0x26) / 4096.0f;
        bone.rot2_z = ReadI16BE(b + 0x28) / 4096.0f;
        bone.local_x = ReadF32BE(b + 0x34);
        bone.local_y = ReadF32BE(b + 0x38);
        bone.local_z = ReadF32BE(b + 0x3C);
        bone.world_pos = glm::vec3(0.f);
        bone.world_rot = glm::mat3(1.f);

        out.push_back(std::move(bone));
    }

    std::cout << "[NBN2] " << out.size() << " bones parsed\n";
    if (out.empty()) return false;

    ComputeWorldPositions(out);
    return true;
}

// BONESPACE algorithm from Blender 2.49 skeletonLib.py (row-vector convention):
//   bone.head   = posVec * parent.matrix + parent.head
//   bone.matrix = localRot * parent.matrix
//
// Converting row→column (M_col = M_row^T, so (A*B)_col = B_col * A_col):
//   world_pos = parent_world_rot * local_pos + parent_world_pos
//   world_rot = parent_world_rot * local_rot        ← parent THEN local
//
// Note: the previous formulas (world_rot = lr * parent, world_pos = transpose(parent)*lp)
// were wrong. Z-axis-only rotations commute so that test did not catch the error.

void NBN2Parser::ComputeWorldPositions(std::vector<Bone>& bones) {
    for (size_t i = 0; i < bones.size(); i++) {
        Bone& bone = bones[i];

        glm::vec3 lp(bone.local_x, bone.local_y, bone.local_z);
        // Local rotation is the composition of rot1 and rot2. Validated against
        // pcalg sword bind position: Jsword bind world with rot1*rot2 matches
        // the weapon mesh center exactly. With rot1 only, the sword is offset.
        glm::mat3 r1 = EulerXYZ(bone.rot_x, bone.rot_y, bone.rot_z);
        glm::mat3 r2 = EulerXYZ(bone.rot2_x, bone.rot2_y, bone.rot2_z);
        glm::mat3 lr = r1 * r2;

        const int par = bone.parent_idx;
        if (par < 0 || par >= static_cast<int>(bones.size())) {
            // Root bone: world = local
            bone.world_pos = lp;
            bone.world_rot = lr;
        }
        else {
            const Bone& p = bones[par];
            bone.world_pos = p.world_rot * lp + p.world_pos;
            bone.world_rot = p.world_rot * lr;
        }
    }
}