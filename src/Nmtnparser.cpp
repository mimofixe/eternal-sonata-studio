#include "NMTNParser.h"
#include <cstring>
#include <algorithm>

// helpers
static uint16_t U16(const uint8_t* d) { return (uint16_t)((d[0] << 8) | d[1]); }
static uint32_t U32(const uint8_t* d) { return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3]; }
static int16_t  I16(const uint8_t* d) { return (int16_t)U16(d); }

// Slot-A is "useful" (carries real animation data) when:
//   - it is not the empty sentinel (0,0)
//   - the frame is within range (< 0xFE00 and <= frame_count)
static bool SlotUseful(uint16_t f, int16_t v, uint16_t fc) {
    if (f == 0 && v == 0) return false;          // empty slot
    if (f >= 0xFE00u) return false;          // Bezier tangent marker
    if (f > fc)       return false;          // out-of-range sentinel
    return true;
}

void NMTNParser::Fill(std::vector<FV>& raw, NMTNChannel& ch) {
    if (raw.empty()) return;
    std::sort(raw.begin(), raw.end(),
        [](const FV& a, const FV& b) { return a.first < b.first; });
    ch.keys.clear();
    for (auto& kv : raw) {
        if (!ch.keys.empty() && ch.keys.back().frame == kv.first)
            ch.keys.back().value = kv.second;
        else
            ch.keys.push_back({ kv.first, kv.second });
    }
}

// Unified dual-slot state machine
//
// Works identically for FORMAT_A and FORMAT_BC.
// FORMAT_A has a 4-byte [n0 u16][n1 u16] header before the records
// which is simply skipped — the state machine handles boundaries implicitly.
//
// State: seen_transition (starts false)
//   Slot-A useful  +  NOT seen_transition  →  c0
//   Slot-A useful  +  seen_transition      →  c2
//   Slot-A NOT useful (any reason)         →  set seen_transition = true
//   Slot-B useful  +  Slot-A NOT useful    →  c1
//   wrong, will be fixed eventually
// 
// This correctly handles:
//   • Standard BC bones: 3 equal groups (slotA→c0, slotB→c1, slotA→c2)
//   • FORMAT_A bones: slotA section-0→c0, slotB sections-1&2→c1, slotA tail→c2
//   • Single-channel bones: slotA→c0, slotB always paired with useful slotA→ignored

void NMTNParser::ParseTrack(const uint8_t* t, uint32_t t_size,
    uint16_t fc, NMTNTrack& out) {
    if (t_size < 36) return;

    const char* nm = (const char*)t;
    size_t nl = 0;
    while (nl < 16 && nm[nl] != 0) ++nl;
    out.bone_name = std::string(nm, nl);
    out.is_translation = NMTNIsTranslationBone(out.bone_name);

    uint32_t kb = t_size - 36;
    if (kb < 8) { out.is_static = true; return; }

    const uint8_t* kf = t + 0x24;
    const float scale = out.is_translation ? kNMTNTransScale : kNMTNRotScale;

    // Skip 4-byte header for FORMAT_A
    uint32_t start = (kb % 8 == 4) ? 4u : 0u;
    uint32_t n_recs = (kb - start) / 8;

    std::vector<FV> raw0, raw1, raw2;
    bool seen = false;

    for (uint32_t i = 0; i < n_recs; ++i) {
        const uint8_t* rec = kf + start + i * 8;
        uint16_t fA = U16(rec);     int16_t vA = I16(rec + 2);
        uint16_t fB = U16(rec + 4);   int16_t vB = I16(rec + 6);

        bool aUseful = SlotUseful(fA, vA, fc);
        bool bUseful = SlotUseful(fB, vB, fc);

        if (aUseful) {
            if (!seen) raw0.push_back({ fA, vA * scale });
            else      raw2.push_back({ fA, vA * scale });
        }
        else {
            // Any non-useful slot-A (empty, tangent, sentinel) triggers transition
            seen = true;
        }

        if (bUseful && !aUseful)
            raw1.push_back({ fB, vB * scale });
    }

    // Channel assignment:
    // c0→ry  c1→rx  c2→rz   (YXZ mapping, confirmed anatomically)
    // Translation: c0→lx  c1→lz(skip/root-motion)  c2→ly
    Fill(raw0, out.ry);   // c0
    Fill(raw1, out.rx);   // c1
    Fill(raw2, out.rz);   // c2

    out.is_static = out.ry.Empty() && out.rx.Empty() && out.rz.Empty();
}

// Top-level Parse
bool NMTNParser::Parse(const uint8_t* data, size_t size, NMTNAnimation& out) {
    if (size < 0x30) return false;
    if (std::memcmp(data, "NMTN", 4) != 0) return false;

    uint32_t chunk_size = U32(data + 4);
    out.frame_count = U16(data + 0x20);

    const char* nm = (const char*)(data + 0x10);
    size_t nl = 0;
    while (nl < 16 && nm[nl] != 0) ++nl;
    out.name = std::string(nm, nl);
    if (out.name.empty()) out.name = "anim";

    out.tracks.clear();
    out.bone_map.clear();

    uint32_t limit = (uint32_t)std::min((size_t)chunk_size, size);
    uint32_t pos = 0x30;

    while (pos < limit) {
        if (pos + 20 > limit) break;

        bool all_null = true;
        for (int i = 0; i < 16; i++) if (data[pos + i]) { all_null = false; break; }
        if (all_null) break;

        uint32_t t_size = U32(data + pos + 0x10);
        if (t_size < 20 || t_size > limit - pos) break;

        NMTNTrack track;
        ParseTrack(data + pos, t_size, out.frame_count, track);
        if (!track.bone_name.empty()) {
            out.bone_map[track.bone_name] = (int)out.tracks.size();
            out.tracks.push_back(std::move(track));
        }
        pos += t_size;
    }

    return !out.tracks.empty();
}