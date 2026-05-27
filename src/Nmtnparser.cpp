#include "NMTNParser.h"
#include <cstring>
#include <cmath>
#include <algorithm>

float NMTNParser::F32(const uint8_t* d) {
    uint32_t u = U32(d);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

// Try to parse a channel at byte position `p`. A valid channel header is:
//     u16 count    (1 <= count <= 500)
//     u16 format   (always 0x000F)
// followed by count * 8 bytes of keyframes.
// Returns true and sets `consumed` to (4 + count*8) on success.
bool NMTNParser::ParseChannel(const uint8_t* p, uint32_t remaining,
    NMTNChannel& out, uint32_t& consumed) {
    if (remaining < 4) return false;
    uint16_t count = U16(p);
    uint16_t fmt = U16(p + 2);
    if (fmt != 0x000F) return false;
    if (count == 0 || count > 500) return false;
    uint32_t needed = 4u + (uint32_t)count * 8u;
    if (needed > remaining) return false;

    out.keys.clear();
    out.keys.reserve(count);
    for (uint16_t k = 0; k < count; ++k) {
        const uint8_t* kfb = p + 4 + k * 8;
        uint16_t frame = U16(kfb);
        int16_t  value = I16(kfb + 2);
        // bytes 4..7 are tangent / reserved, ignored (0 in tested files)
        out.keys.push_back({ frame, (float)value * kNMTNScale });
    }
    consumed = needed;
    return true;
}

void NMTNParser::ParseTrack(const uint8_t* t, uint32_t t_size, NMTNTrack& out) {
    if (t_size < 24) return;

    const char* nm = (const char*)t;
    size_t nl = 0;
    while (nl < 16 && nm[nl] != 0) ++nl;
    out.bone_name = std::string(nm, nl);
    out.is_translation = NMTNIsTranslationBone(out.bone_name);

    // Data area starts at +0x18 (after 16-byte name + 8-byte track header)
    if (t_size <= 0x18) {
        out.is_static = true;
        return;
    }

    // The preamble (0 / 4 / 8 / 12 bytes of f32 BE) sits between +0x18 and
    // the first channel header. Find the first plausible channel header by
    // scanning at 4-byte alignment for `u16 count + u16 0x000F`.
    uint32_t pos = 0x18;
    uint32_t first_channel_offset = 0;

    while (pos + 4 <= t_size) {
        uint16_t maybe_count = U16(t + pos);
        uint16_t maybe_fmt = U16(t + pos + 2);
        if (maybe_fmt == 0x000F && maybe_count >= 1 && maybe_count <= 500) {
            uint32_t needed = 4u + (uint32_t)maybe_count * 8u;
            if (pos + needed <= t_size) {
                first_channel_offset = pos;
                break;
            }
        }
        pos += 4;
    }

    // Capture the preamble (0, 4, 8 or 12 bytes between +0x18 and first channel)
    if (first_channel_offset > 0x18) {
        uint32_t pre_bytes = first_channel_offset - 0x18;
        if (pre_bytes >= 4 && pre_bytes <= 12 && (pre_bytes % 4) == 0) {
            out.preamble_count = (int)(pre_bytes / 4);
            for (int i = 0; i < out.preamble_count; ++i)
                out.preamble[i] = F32(t + 0x18 + i * 4);
        }
    }

    if (first_channel_offset == 0) {
        // No channels found — track is static (just the name + header,
        // and possibly some unused data we don't decode)
        out.is_static = true;
        return;
    }

    // Parse up to 3 contiguous channels starting at first_channel_offset
    pos = first_channel_offset;
    NMTNChannel* slots[3] = { &out.c0, &out.c1, &out.c2 };

    for (int ci = 0; ci < 3; ++ci) {
        if (pos + 4 > t_size) break;
        uint32_t consumed = 0;
        if (!ParseChannel(t + pos, t_size - pos, *slots[ci], consumed)) break;
        pos += consumed;
        out.channel_count = ci + 1;
    }

    out.is_static = (out.channel_count == 0);
}

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

        // Terminator: 16-byte all-null name block
        bool all_null = true;
        for (int i = 0; i < 16; ++i) if (data[pos + i]) { all_null = false; break; }
        if (all_null) break;

        uint32_t t_size = U32(data + pos + 0x10);
        if (t_size < 20 || t_size > limit - pos) break;

        NMTNTrack track;
        ParseTrack(data + pos, t_size, track);
        if (!track.bone_name.empty()) {
            out.bone_map[track.bone_name] = (int)out.tracks.size();
            out.tracks.push_back(std::move(track));
        }
        pos += t_size;
    }

    return !out.tracks.empty();
}