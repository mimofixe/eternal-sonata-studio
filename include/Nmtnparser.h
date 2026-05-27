#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <unordered_map>

//  NMTN Animation Format — Eternal Sonata PS3 (verified against pcalg_v1.p3obj)
//
//  TRACK LAYOUT:
//    +0x00  char[16]   bone name (null-padded)
//    +0x10  u32 BE     total track size
//    +0x14  u32 BE     flags
//    +0x18  variable   preamble: 0/4/8/12 bytes of f32 BE values
//                      = bind-pose local position (lx, ly, lz) with any
//                        component that is exactly 0 omitted.
//                      Redundant copy of NBN2 data — useful for sanity check.
//    +0x..  variable   N channels CONTIGUOUS (N = 1, 2, or 3)
//
//  CHANNEL LAYOUT:
//    +0x00  u16 BE     keyframe count
//    +0x02  u16 BE     format flag (always 0x000F seen)
//    +0x04  N*8 bytes  N keyframes
//
//  KEYFRAME (8 bytes):
//    +0x00  u16 BE     frame number
//    +0x02  i16 BE     value
//    +0x04  4 bytes    tangent / reserved (always 0 in tested files —
//                      bezier tangents probably unused in this dataset)
//
//  INTERPRETATION  (verified by loop check + visual confirmation):
//
//    Translation bones (move / pos / Top1):
//        c0 → local x      ABSOLUTE position (overrides bind)
//        c1 → local y      ABSOLUTE
//        c2 → local z      ABSOLUTE
//        scale = 1 / 32768
//
//    Rotation bones (everything else, when 3 channels present):
//        (c0, c1, c2) → quaternion (qx, qy, qz), scale 1/32768
//        qw = sqrt(max(0, 1 - qx² - qy² - qz²))   ← reconstructed (smallest-three)
//        applied as DELTA against bind rotation:
//            local_rot = bind_local_rot * quat_to_mat(qx, qy, qz, qw)
//
//    Rotation bones with 1 or 2 channels: not yet decoded.
//        The flags field at +0x14 of the track header probably encodes which
//        axis/axes are present. For now these bones stay in bind pose.
//
//  WHY i16/32768 AND NOT i16*pi/32768?
//    Because the channels are quaternion components, not Euler angles.
//    Quaternion (x, y, z) components must lie in [-1, 1]; an i16 / 32768
//    maps i16 [-32768, 32767] to roughly [-1, 1).
//
//  LOOP PROPERTY:
//    Every channel satisfies value[0] == value[count-1].
//    Frame 0 and frame (count-1) are identical, so the animation seamlessly loops.

static constexpr float kNMTNScale = 1.0f / 32768.0f;   // both rotation and translation

inline bool NMTNIsTranslationBone(const std::string& name) {
    return name == "move" || name == "pos" || name == "Top1";
}

struct NMTNKeyframe {
    uint16_t frame;
    float    value;     // already scaled (multiplied by kNMTNScale)
};

struct NMTNChannel {
    std::vector<NMTNKeyframe> keys;
    bool Empty() const { return keys.empty(); }

    // Linear interpolation. `base` is returned for empty channels.
    float Sample(float t, float base = 0.f) const {
        if (keys.empty()) return base;
        if (t <= keys.front().frame) return keys.front().value;
        if (t >= keys.back().frame)  return keys.back().value;
        for (size_t i = 0; i + 1 < keys.size(); ++i) {
            float f0 = keys[i].frame, f1 = keys[i + 1].frame;
            if (t >= f0 && t <= f1) {
                float a = (f1 > f0) ? (t - f0) / (f1 - f0) : 0.f;
                return keys[i].value + a * (keys[i + 1].value - keys[i].value);
            }
        }
        return keys.back().value;
    }
};

struct NMTNTrack {
    std::string  bone_name;
    bool         is_translation = false;
    bool         is_static = false;       // no channels parsed

    int          channel_count = 0;       // 0, 1, 2, or 3
    NMTNChannel  c0;
    NMTNChannel  c1;
    NMTNChannel  c2;

    // Preamble (bind-pose position floats from track header).
    // Mostly for debugging — the actual bind position comes from NBN2.
    int   preamble_count = 0;
    float preamble[3] = { 0.f, 0.f, 0.f };
};

struct NMTNAnimation {
    std::string  name;
    uint16_t     frame_count = 0;
    std::vector<NMTNTrack>               tracks;
    std::unordered_map<std::string, int> bone_map;

    int FindTrack(const std::string& bone) const {
        auto it = bone_map.find(bone);
        return it != bone_map.end() ? it->second : -1;
    }
};

class NMTNParser {
public:
    static bool Parse(const uint8_t* data, size_t size, NMTNAnimation& out);

private:
    static uint16_t U16(const uint8_t* d) { return (uint16_t)((d[0] << 8) | d[1]); }
    static uint32_t U32(const uint8_t* d) {
        return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16)
            | ((uint32_t)d[2] << 8) | d[3];
    }
    static int16_t  I16(const uint8_t* d) { return (int16_t)U16(d); }
    static float    F32(const uint8_t* d);

    static void ParseTrack(const uint8_t* t, uint32_t t_size, NMTNTrack& out);
    static bool ParseChannel(const uint8_t* p, uint32_t remaining,
        NMTNChannel& out, uint32_t& consumed);
};