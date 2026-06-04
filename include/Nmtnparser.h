#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <unordered_map>

// NMTN Animation Format - Eternal Sonata (PS3/Xbox360)
// Reverse-engineered from Xbox360 EBOOT.BIN (decrypted XEX):
//   - Channel init:    fn 0x8211cc18  -> value_scale = 1.0/(1<<fmt_byte)
//   - Value sampler:   fn 0x8211ccc8  (Hermite with tangents)
//   - Euler->matrix:   fn 0x82117b70 -> 0x820c0668 (sin/cos polynomial)
//   - Channel binding: fn 0x8211b720  (flags -> per-slot fields)
//
// TRACK LAYOUT (one per bone):
//   +0x00  char[16]   bone name
//   +0x10  u32 BE     track size
//   +0x14  u32 BE     flags: 9 slots * 3 bits each, field = (flags>>((8-slot)*3))&7
//                            field 0 = inactive, 1 = static (preamble),
//                            2 = keyframed channel
//   +0x18  N*f32 BE   preamble: one f32 per static slot, in slot order
//   ...    channels:  one per keyframed slot, in slot order
//
// SLOTS:  0,1,2 = posX/Y/Z   3,4,5 = rotX/Y/Z   6,7,8 = sclX/Y/Z
//
// CHANNEL LAYOUT:
//   +0x00  u16 BE     keyframe count
//   +0x02  u8         (high byte of fmt, 0)
//   +0x03  u8         fmt byte (0x0C..0x0F) -> value_scale = 1.0/(1<<fmt)
//   +0x04  count*8    keyframes
//
// KEYFRAME (8 bytes):
//   +0x00  u16 BE     frame number
//   +0x02  i16 BE     value          (* scale = radians for rot, units for pos)
//   +0x04  i16 BE     tan_in         (* scale, slope at this kf coming in)
//   +0x06  i16 BE     tan_out        (* scale, slope going out)
//
// APPLICATION:
//   pos: channel value overrides bind local position
//   rot: local_rot = Euler(bind) * Euler(anim)  (anim in bone-local frame)
//   scale: (similar to pos, applies to bind scale)

struct NMTNKeyframe {
    uint16_t frame;
    float    value;     // already scaled to radians (or position units)
    float    tan_in;    // already scaled
    float    tan_out;
};

struct NMTNChannel {
    std::vector<NMTNKeyframe> keys;
    bool Empty() const { return keys.empty(); }

    // Cubic Hermite interpolation using stored tangents.
    float Sample(float t) const {
        if (keys.empty()) return 0.f;
        if (t <= keys.front().frame) return keys.front().value;
        if (t >= keys.back().frame)  return keys.back().value;
        for (size_t i = 0; i + 1 < keys.size(); ++i) {
            const auto& k0 = keys[i];
            const auto& k1 = keys[i + 1];
            if (t >= k0.frame && t <= k1.frame) {
                float dt = float(k1.frame) - float(k0.frame);
                if (dt <= 0.f) return k0.value;
                float s = (t - k0.frame) / dt;
                float m0 = k0.tan_out * dt;
                float m1 = k1.tan_in * dt;
                float s2 = s * s, s3 = s2 * s;
                float h00 = 2 * s3 - 3 * s2 + 1;
                float h10 = s3 - 2 * s2 + s;
                float h01 = -2 * s3 + 3 * s2;
                float h11 = s3 - s2;
                return h00 * k0.value + h10 * m0 + h01 * k1.value + h11 * m1;
            }
        }
        return keys.back().value;
    }
};

struct NMTNTrack {
    std::string  bone_name;
    uint32_t     flags = 0;

    // For each of the 9 slots: field value from flags (0/1/2).
    uint8_t      field[9] = { 0,0,0,0,0,0,0,0,0 };

    // Static slot values (from preamble) when field[slot] == 1.
    float        static_val[9] = { 0,0,0,0,0,0,0,0,0 };

    // Keyframed channels when field[slot] == 2.
    NMTNChannel  channel[9];

    bool HasChannel(int slot) const { return field[slot] == 2 && !channel[slot].Empty(); }
    bool HasStatic(int slot) const { return field[slot] == 1; }
    bool HasAnyRot() const { return HasChannel(3) || HasChannel(4) || HasChannel(5); }
    bool HasAnyPos() const { return HasChannel(0) || HasChannel(1) || HasChannel(2); }
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
};