#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <glm/glm.hpp>

// Bone
// Layout confirmed from EternalSonataPS3.py (old scrippt found in a forum lol)
//
//   offset  type      field
//   +0x00   char[16]  name          (null-terminated)
//   +0x10   i16       flags         96=rigid, 355=dynamic-A, 353=dynamic-B, 361=effector
//   +0x12   i16       parent_idx    (-1 = root)
//   +0x14   i16       child_idx     (first child, -1 = leaf)
//   +0x16   i16       sibling_idx   (next sibling, -1 = last)
//   +0x18   i16       rot_x_fixed   Euler X in radians × 4096  (i16 / 4096 = rad)
//   +0x1A   i16       rot_y_fixed   Euler Y
//   +0x1C   i16       rot_z_fixed   Euler Z
//   +0x34   f32       local_x       position relative to parent
//   +0x38   f32       local_y
//   +0x3C   f32       local_z
//
// Entry stride = 64 bytes.
//
// World positions are computed with Blender's BONESPACE algorithm:
//   world_pos = transpose(parent_world_rot) * local_pos + parent_world_pos
//   world_rot = local_rot * parent_world_rot
//
// where local_rot = Euler(rx,ry,rz).toMatrix() using XYZ order (Rz*Ry*Rx).

struct Bone {
    std::string  name;
    int16_t      parent_idx;    // -1 = root
    int16_t      child_idx;
    int16_t      sibling_idx;
    uint16_t     flags;         // 96=rigid, 355/353=dynamic chain, 361=effector

    // Raw local bind-pose data from file
    float        rot_x;         // Euler X, radians
    float        rot_y;         // Euler Y, radians
    float        rot_z;         // Euler Z, radians
    float        local_x;       // local position
    float        local_y;
    float        local_z;

    // Computed world-space bind pose (BONESPACE, set by ComputeWorldPositions)
    glm::vec3    world_pos;
    glm::mat3    world_rot;

    bool IsRigid()    const { return flags == 96; }
    bool IsDynamic()  const { return flags == 355 || flags == 353; }
    bool IsEffector() const { return flags == 361; }
};

//NBN2Parser

class NBN2Parser {
public:
    // Parse a full NBN2 chunk (data[0..3] = "NBN2", data[4..7] = size u32 BE).
    // Fills `out` with bones in file order (parents guaranteed before children).
    // Calls ComputeWorldPositions internally.
    static bool Parse(const uint8_t* data, size_t size, std::vector<Bone>& out);

    // Recompute world_pos / world_rot for all bones using BONESPACE.
    // Call this after any manual modification of local fields.
    static void ComputeWorldPositions(std::vector<Bone>& bones);

private:
    static int16_t  ReadI16BE(const uint8_t* d);
    static uint16_t ReadU16BE(const uint8_t* d);
    static float    ReadF32BE(const uint8_t* d);

    // Euler XYZ rotation matrix: Rz * Ry * Rx, column-major (GLM convention)
    static glm::mat3 EulerXYZ(float rx, float ry, float rz);
};