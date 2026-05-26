#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <unordered_map>

//  NMTN Animation Format — Eternal Sonata PS3 (guess for now)
//
//  TRACK LAYOUT  (all bones, no exceptions):
//    +0x00  char[16]   bone name (null-padded)
//    +0x10  u32 BE     total track size
//    +0x14  u32 BE     flags / type
//    +0x18  12 bytes   varies (local-pos floats, or small count fields)
//    +0x24  ...        keyframe data  (total_size − 36 bytes)
//
//  FORMAT DETERMINATION  (kf_bytes = total_size − 36):
//    kf_bytes % 8 == 4  →  FORMAT_A  (has 4-byte [n0][n1] header)
//    kf_bytes % 8 == 0  →  FORMAT_BC (no header, dual-slot state machine)
//
//  FORMAT_A:
//    kf[0..1] = n0  (c0 record count)
//    kf[2..3] = n1  (tangent handle count — SKIP)
//    kf[4..]  = records (8 bytes each)
//    section 0 (n0 records) slot-A → c0
//    section 1 (n1 records) slot-A → Bezier tangents — DISCARD
//    section 2 (remainder)  slot-A → c2
//    c1 is always empty for FORMAT_A tracks.
//
//  FORMAT_BC  (dual-slot state machine):
//    Each 8-byte record: [fA u16][vA i16][fB u16][vB i16]
//    Slot is EMPTY when f==0 AND v==0.
//    State machine (seen_empty_A starts False):
//      if slotA not empty:
//          if not seen_empty_A → add (fA,vA) to c0
//          else                → add (fA,vA) to c2
//      else: set seen_empty_A = True
//      if slotB not empty AND slotA empty → add (fB,vB) to c1
//
//  SCALE:
//    Rotation bones : i16 × (π / 32768) = radians
//    Translation    : i16 / 4096         = metres   (move / pos / Top1)
//
//  CHANNEL MAPPING  (YXZ order, confirmed anatomically):
//    c0 → ry   c1 → rx   c2 → rz
//    Translation: c0→dx  c1→dz(skip/root-motion)  c2→dy
//
//  All values are DELTAS added to the NBN2 bind-pose angles.

static constexpr float kNMTNRotScale = 3.14159265f / 32768.f;
static constexpr float kNMTNTransScale = 1.f / 4096.f;

inline bool NMTNIsTranslationBone(const std::string& name) {
    return name == "move" || name == "pos" || name == "Top1";
}

struct NMTNKeyframe {
    uint16_t frame;
    float    value;   // radians or metres, delta from bind
};

struct NMTNChannel {
    std::vector<NMTNKeyframe> keys;
    bool Empty() const { return keys.empty(); }

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

// c0→ry, c1→rx, c2→rz
// Translation bones: ry→dx, rx→dz(skip), rz→dy
struct NMTNTrack {
    std::string  bone_name;
    bool         is_translation = false;
    bool         is_static = false;  // all channels empty

    NMTNChannel ry;   // c0
    NMTNChannel rx;   // c1
    NMTNChannel rz;   // c2
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
    static uint32_t U32(const uint8_t* d) { return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3]; }
    static int16_t  I16(const uint8_t* d) { return (int16_t)U16(d); }

    static void ParseTrack(const uint8_t* t, uint32_t t_size,
        uint16_t fc, NMTNTrack& out);

    using FV = std::pair<uint16_t, float>;
    // Push one slot (0=A bytes 0-3, 1=B bytes 4-7), filter sentinels.
    static void Push(const uint8_t* rec, int slot, float scale,
        uint16_t fc, std::vector<FV>& out);
    // Dedup (keep last per frame), sort ascending, fill channel.
    static void Fill(std::vector<FV>& raw, NMTNChannel& ch);
};