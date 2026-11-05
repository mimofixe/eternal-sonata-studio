#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <glm/glm.hpp>

// Bone
// Layout confirmed from EternalSonataPS3.py (old scrippt found in a forum lol)
// + reverse-engineering with rot2 found through pcalg sword position validation.
//
//   offset  type      field
//   +0x00   char[16]  name          (null-terminated)
//   +0x10   i16       flags         96=rigid, 355=dynamic-A, 353=dynamic-B, 361=effector
//   +0x12   i16       parent_idx    (-1 = root)
//   +0x14   i16       child_idx     (first child, -1 = leaf)
//   +0x16   i16       sibling_idx   (next sibling, -1 = last)
//   +0x18   i16       rot1_x_fixed  Euler X (radians × 4096)  — primary rotation
//   +0x1A   i16       rot1_y_fixed  Euler Y
//   +0x1C   i16       rot1_z_fixed  Euler Z
//   +0x1E..0x23       (6 bytes, unused — always zero in pcalg/pcbet)
//   +0x24   i16       rot2_x_fixed  Euler X (radians × 4096)  — SECONDARY rotation
//   +0x26   i16       rot2_y_fixed  Euler Y                    overridden per-slot
//   +0x28   i16       rot2_z_fixed  Euler Z                    by animation tracks
//   +0x2A   i16       scale_x       local scale, fixed-point (256 = 1.0)
//   +0x2C   i16       scale_y
//   +0x2E   i16       scale_z
//   +0x30   u16       ndyn_index    index into the NDYN 48-byte record array
//                                   (EBOOT @0x2d1d88: lhz +0x30, x48, + NDYN base;
//                                   a 0..n-1 permutation over the dynamic bones,
//                                   0 for rigid bones)
//   +0x32   u16       (zero in pcalg/pcplk)
//   +0x34   f32       local_x       position relative to parent
//   +0x38   f32       local_y
//   +0x3C   f32       local_z
//
// Entry stride = 64 bytes.
//
// The scale is *not* cosmetic: it accumulates down the hierarchy and multiplies
// every descendant offset, so ignoring it puts the rig in the wrong units (a
// bone's own scale applies to its children, not to its own offset). It is 1.0
// on 58844 of the 58889 bones across the 818 skeletons in the game, which is why
// it went unnoticed — but e3120_110.e (the seagull) scales its root 'Top1' by
// 31/256 = 0.121, i.e. its skeleton is authored ~8x larger than its mesh. Others:
// Uns* 'joint1' 921/256 on X only, AppKeep 'Jhatpos' 6/256, Wau* 'joint26' 230/256.
//
// World positions are computed with Blender's BONESPACE algorithm:
//   world_pos = parent_world_rot * (parent_world_scale * local_pos) + parent_world_pos
//   world_rot = parent_world_rot * (E(rot1) * E(rot2))
//   world_scale = parent_world_scale * local_scale
//
// where E(rx,ry,rz).toMatrix() uses XYZ order (Rz*Ry*Rx). The bone's effective
// local rotation is the composition E(rot1) * E(rot2). Animation tracks (NMTN)
// override per-slot values of rot2; when a slot has neither channel nor static
// in NMTN, the default for that slot is the rot2 value from this struct.

struct Bone {
    std::string  name;
    int16_t      parent_idx;    // -1 = root
    int16_t      child_idx;
    int16_t      sibling_idx;
    uint16_t     flags;         // 96=rigid, 355/353=dynamic chain, 361=effector
    uint16_t     ndyn_index;    // index into the NDYN record array (0 for rigid bones)

    // Raw local bind-pose data from file
    float        rot_x;         // rot1 Euler X, radians
    float        rot_y;         // rot1 Euler Y, radians
    float        rot_z;         // rot1 Euler Z, radians
    float        rot2_x;        // rot2 Euler X, radians (secondary, animation default)
    float        rot2_y;        // rot2 Euler Y, radians
    float        rot2_z;        // rot2 Euler Z, radians
    float        local_x;       // local position
    float        local_y;
    float        local_z;
    glm::vec3    local_scale;   // i16/256 triplet at +0x2A; (1,1,1) on almost every bone

    // Computed world-space bind pose (BONESPACE, set by ComputeWorldPositions)
    glm::vec3    world_pos;
    glm::mat3    world_rot;
    // Accumulated scale (product down the parent chain). Applied to the child
    // offsets only -- world_rot deliberately stays a pure rotation so that a
    // skinning matrix (world * inverse(bind_world)) is rigid and a non-uniform
    // scale can never shear the mesh. Nothing animates scale, so this is fixed
    // at bind time and simply carried along by the pose.
    glm::vec3    world_scale;

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