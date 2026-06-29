#include "NMTNParser.h"
#include <cstring>
#include <algorithm>
#include <cmath>
#include <set>
#include <vector>

namespace {

    // 3x3 rotation matrix used internally to compare Euler triplets.
    struct Mat3 {
        float m[9];
        static Mat3 EulerXYZ(float rx, float ry, float rz) {
            // Composition Rz * Ry * Rx, matching the Animviewport AVEulerXYZ.
            float cx = std::cos(rx), sx = std::sin(rx);
            float cy = std::cos(ry), sy = std::sin(ry);
            float cz = std::cos(rz), sz = std::sin(rz);
            Mat3 M;
            M.m[0] = cy * cz;
            M.m[1] = sx * sy * cz - cx * sz;
            M.m[2] = cx * sy * cz + sx * sz;
            M.m[3] = cy * sz;
            M.m[4] = sx * sy * sz + cx * cz;
            M.m[5] = cx * sy * sz - sx * cz;
            M.m[6] = -sy;
            M.m[7] = sx * cy;
            M.m[8] = cx * cy;
            return M;
        }
        float diff(const Mat3& o) const {
            float s = 0.f;
            for (int i = 0; i < 9; ++i) { float d = m[i] - o.m[i]; s += d * d; }
            return std::sqrt(s);
        }
    };

    // FIX #1: Euler antipodal flip detection.
    //
    // The XYZ Euler representation of a rotation is NOT unique. The identity
    //   E(a, b, c) ≡ E(a - π, π - b, c - π)
    // gives the same rotation matrix. The exporter sometimes "flips" representation
    // mid-animation — two consecutive keyframes look very different in Euler values
    // but represent the same orientation. Naive per-channel Hermite interpolation
    // between them traces out arbitrary intermediate rotations (the bone "tweaks"
    // through 180° flips in 1 frame).
    //
    // Example seen in ep147.bop bosFGA_Prologue, bone JsebB at frame 12 → 13:
    //   frame 12: ( -0.10, -0.016, -0.086 )  ≈ identity rotation
    //   frame 13: ( +3.04, -3.13,  +3.06 )   ← antipodal: SAME matrix, different Euler
    //   At frame 12.5 (Hermite midpoint), the bone is rotated ~180° away. Visible
    //   in the rendered animation as a violent "tweak" of the entire body.
    //
    // Fix: walk through the unique keyframe frames of slots 3,4,5 together. At each
    // frame, sample the (rx, ry, rz) triplet via Hermite. If a frame's matrix is
    // essentially equal to the previous frame's matrix but the Euler values differ
    // dramatically (max axis diff > 1.5 rad), an antipodal flip is detected. We
    // toggle a parity bit; all subsequent keyframes (until the next flip) get the
    // identity transform applied to their values (and the rotY tangent negated,
    // because π - v has slope opposite to v).
    void FixEulerAntipodalFlips(NMTNTrack& track) {
        const int S_RX = 3, S_RY = 4, S_RZ = 5;
        const float PI_F = 3.14159265358979323846f;

        auto has_chan = [&](int s) {
            return track.field[s] == 2 && !track.channel[s].keys.empty();
            };
        if (!has_chan(S_RX) && !has_chan(S_RY) && !has_chan(S_RZ)) return;

        auto get_default = [&](int s) -> float {
            if (track.field[s] == 1) return track.static_val[s];
            return 0.f;
            };
        auto get_triplet = [&](float t, float& rx, float& ry, float& rz) {
            rx = has_chan(S_RX) ? track.channel[S_RX].Sample(t) : get_default(S_RX);
            ry = has_chan(S_RY) ? track.channel[S_RY].Sample(t) : get_default(S_RY);
            rz = has_chan(S_RZ) ? track.channel[S_RZ].Sample(t) : get_default(S_RZ);
            };

        // Build sorted unique frame list across the rotation channels.
        std::set<uint16_t> frame_set;
        for (int s : { S_RX, S_RY, S_RZ }) {
            if (!has_chan(s)) continue;
            for (auto& k : track.channel[s].keys) frame_set.insert(k.frame);
        }
        if (frame_set.size() < 2) return;
        std::vector<uint16_t> frames(frame_set.begin(), frame_set.end());

        // Detect flips. After fixing for the running parity, the "applied" triplet
        // should be continuous with the previous applied triplet.
        auto wrap_diff = [PI_F](float d) {
            while (d > PI_F) d -= 2.f * PI_F;
            while (d < -PI_F) d += 2.f * PI_F;
            return std::fabs(d);
            };

        std::vector<uint16_t> flip_frames;
        int parity = 0;
        float prev_rx, prev_ry, prev_rz;
        get_triplet((float)frames[0], prev_rx, prev_ry, prev_rz);

        for (size_t i = 1; i < frames.size(); ++i) {
            uint16_t f = frames[i];
            float rx, ry, rz;
            get_triplet((float)f, rx, ry, rz);

            // Apply current parity to this triplet.
            float rx_a = rx, ry_a = ry, rz_a = rz;
            if (parity) { rx_a = rx - PI_F; ry_a = PI_F - ry; rz_a = rz - PI_F; }

            float max_diff = std::max({ wrap_diff(rx_a - prev_rx),
                                        wrap_diff(ry_a - prev_ry),
                                        wrap_diff(rz_a - prev_rz) });

            if (max_diff > 1.5f) {
                // Try toggling parity — see if the antipodal triplet is closer.
                float rx_t, ry_t, rz_t;
                if (parity) {
                    // un-flip: go back to raw
                    rx_t = rx; ry_t = ry; rz_t = rz;
                }
                else {
                    // flip: apply identity
                    rx_t = rx - PI_F; ry_t = PI_F - ry; rz_t = rz - PI_F;
                }
                float test_diff = std::max({ wrap_diff(rx_t - prev_rx),
                                             wrap_diff(ry_t - prev_ry),
                                             wrap_diff(rz_t - prev_rz) });

                // Verify the matrix is actually preserved by the identity — otherwise
                // this is real motion, not a representation flip.
                Mat3 M_orig = Mat3::EulerXYZ(rx, ry, rz);
                Mat3 M_test = Mat3::EulerXYZ(rx_t, ry_t, rz_t);
                float mat_diff = M_orig.diff(M_test);

                if (test_diff < max_diff && mat_diff < 0.01f) {
                    parity ^= 1;
                    rx_a = rx_t; ry_a = ry_t; rz_a = rz_t;
                    flip_frames.push_back(f);
                }
            }

            prev_rx = rx_a; prev_ry = ry_a; prev_rz = rz_a;
        }

        if (flip_frames.empty()) return;

        // For each keyframe, count flips at or before its frame. Odd count = needs
        // the identity transform applied.
        auto flips_before = [&](uint16_t frame) {
            int n = 0;
            for (uint16_t fl : flip_frames) if (fl <= frame) ++n;
            return n;
            };

        for (int s : { S_RX, S_RY, S_RZ }) {
            if (track.field[s] != 2) continue;
            for (auto& k : track.channel[s].keys) {
                if ((flips_before(k.frame) & 1) == 0) continue;
                if (s == S_RX || s == S_RZ) {
                    // rx → rx - π, rz → rz - π. Tangent slope unchanged (additive).
                    k.value -= PI_F;
                }
                else { // S_RY: ry → π - ry. Slope inverts sign.
                    k.value = PI_F - k.value;
                    k.tan_in = -k.tan_in;
                    k.tan_out = -k.tan_out;
                }
            }
        }
    }

    // FIX #2: per-axis wraparound at ±π (applied AFTER the antipodal fix).
    //
    // Even after the antipodal fix, an individual rotation channel can cross ±π if
    // the bone is genuinely rotating continuously past 180°. The file then stores
    // values like ... +3.13, -3.13 ... and tangents that are the central diff of
    // these wrapped values (so a tangent of ~-π appears at the wrap). Naive Hermite
    // then walks the long way around (through 0), causing a 360° flip in 1 frame.
    //
    // Fix: walk keyframes, unwrap each by ±2π relative to the previous; at wrap
    // boundaries (where the unwrap shifts a value), replace the now-invalid stored
    // tangents with the central difference of the unwrapped neighbors. Non-wrap
    // keyframes keep their original animator-authored tangents.
    void UnwrapRotationChannel(NMTNChannel& ch) {
        auto& keys = ch.keys;
        if (keys.size() < 2) return;

        const float PI_F = 3.14159265358979323846f;
        const float TWO_PI = 2.f * PI_F;

        std::vector<float> unwrapped(keys.size());
        std::vector<bool>  shifted(keys.size(), false);
        unwrapped[0] = keys[0].value;
        for (size_t k = 1; k < keys.size(); ++k) {
            float v = keys[k].value;
            while (v - unwrapped[k - 1] > PI_F) v -= TWO_PI;
            while (v - unwrapped[k - 1] < -PI_F) v += TWO_PI;
            unwrapped[k] = v;
            if (std::fabs(v - keys[k].value) > 1e-4f) shifted[k] = true;
        }

        bool any_shifted = false;
        for (bool b : shifted) { if (b) { any_shifted = true; break; } }
        if (!any_shifted) return;

        for (size_t k = 0; k < keys.size(); ++k) {
            bool wrap_here = shifted[k];
            bool wrap_prev = (k > 0) && (shifted[k] != shifted[k - 1]);
            bool wrap_next = (k + 1 < keys.size()) && (shifted[k] != shifted[k + 1]);
            if (!wrap_here && !wrap_prev && !wrap_next) {
                keys[k].value = unwrapped[k];
                continue;
            }
            float slope;
            if (k == 0) {
                float dt = float(keys[1].frame) - float(keys[0].frame);
                slope = (dt > 0.f) ? (unwrapped[1] - unwrapped[0]) / dt : 0.f;
            }
            else if (k + 1 == keys.size()) {
                float dt = float(keys[k].frame) - float(keys[k - 1].frame);
                slope = (dt > 0.f) ? (unwrapped[k] - unwrapped[k - 1]) / dt : 0.f;
            }
            else {
                float dt = float(keys[k + 1].frame) - float(keys[k - 1].frame);
                slope = (dt > 0.f) ? (unwrapped[k + 1] - unwrapped[k - 1]) / dt : 0.f;
            }
            keys[k].value = unwrapped[k];
            keys[k].tan_in = slope;
            keys[k].tan_out = slope;
        }
    }
    // Rebuild rotation-channel tangents from the corrected values.
    // The antipodal and unwrap passes above fix keyframe VALUES at flip/wrap frames
    // but leave the original tangents untouched. At those frames the stored tangents
    // encode the steep ~pi/2 representation-jump slope, not the true local motion, so
    // cubic Hermite overshoots ~0.15 rad between keyframes. That overshoot is the
    // intermittent "tweak"/tremble (it clusters on bones whose Euler sits in the
    // flip-prone range, e.g. the left side and spine). Replacing the tangents with a
    // clamped central difference of the corrected values keeps the curve passing
    // through every key while removing the overshoot. Tangents in this format are
    // already close to the central-difference slope, so clean motion is unaffected.
    void RecomputeRotationTangents(NMTNChannel& ch) {
        auto& k = ch.keys;
        const size_t n = k.size();
        if (n < 2) return;
        for (size_t i = 0; i < n; ++i) {
            // central difference of the corrected values
            float slope;
            if (i == 0) {
                float dt = float(k[1].frame) - float(k[0].frame);
                slope = (dt > 0.f) ? (k[1].value - k[0].value) / dt : 0.f;
            }
            else if (i == n - 1) {
                float dt = float(k[n - 1].frame) - float(k[n - 2].frame);
                slope = (dt > 0.f) ? (k[n - 1].value - k[n - 2].value) / dt : 0.f;
            }
            else {
                float dt = float(k[i + 1].frame) - float(k[i - 1].frame);
                slope = (dt > 0.f) ? (k[i + 1].value - k[i - 1].value) / dt : 0.f;
            }
            // monotone clamp so a segment never overshoots its endpoints:
            // zero the tangent at flats/extrema, cap it at 3x the local secant.
            if (i > 0) {
                float dt = float(k[i].frame) - float(k[i - 1].frame);
                float sec = (dt > 0.f) ? (k[i].value - k[i - 1].value) / dt : 0.f;
                if (sec == 0.f) slope = 0.f;
                else if (slope * sec < 0.f) slope = 0.f;
                else if (std::fabs(slope) > 3.f * std::fabs(sec)) slope = 3.f * sec;
            }
            if (i + 1 < n) {
                float dt = float(k[i + 1].frame) - float(k[i].frame);
                float sec = (dt > 0.f) ? (k[i + 1].value - k[i].value) / dt : 0.f;
                if (sec == 0.f) slope = 0.f;
                else if (slope * sec < 0.f) slope = 0.f;
                else if (std::fabs(slope) > 3.f * std::fabs(sec)) slope = 3.f * sec;
            }
            k[i].tan_in = slope;
            k[i].tan_out = slope;
        }
    }
} // anonymous namespace

float NMTNParser::F32(const uint8_t* d) {
    uint32_t u = U32(d);
    float f;
    std::memcpy(&f, &u, 4);
    return f;
}

void NMTNParser::ParseTrack(const uint8_t* t, uint32_t t_size, NMTNTrack& out) {
    if (t_size < 0x18) return;

    const char* nm = (const char*)t;
    size_t nl = 0;
    while (nl < 16 && nm[nl] != 0) ++nl;
    out.bone_name = std::string(nm, nl);
    out.flags = U32(t + 0x14);

    // Decode the 9 per-slot 3-bit fields from flags.
    // Slot s uses bits [(8-s)*3 .. (8-s)*3 + 2]. Values: 0=inactive, 1=static, 2=channel.
    for (int s = 0; s < 9; ++s) {
        out.field[s] = uint8_t((out.flags >> ((8 - s) * 3)) & 7);
    }

    uint32_t pos = 0x18;

    // Slot entries are interleaved in slot order: for each slot 0..8, a static
    // slot is one f32 and a channel slot is a channel block, in whatever order
    // the slots fall. Statics and channels are NOT grouped, so walk slots in
    // order and dispatch on the field (single pass).
    for (int s = 0; s < 9; ++s) {
        if (out.field[s] == 1) {            // static: one f32
            if (pos + 4 > t_size) return;
            out.static_val[s] = F32(t + pos);
            pos += 4;
        }
        else if (out.field[s] == 2) {     // channel: count/fmt/keyframes
            if (pos + 4 > t_size) return;

            uint16_t count = U16(t + pos);
            uint8_t  fmt = t[pos + 3];
            // fmt is the bit-shift exponent: scale = 1.0 / (1 << fmt)
            if (fmt < 0x0C || fmt > 0x0F)         return;  // unsupported
            if (count == 0 || count > 1000)       return;
            uint32_t need = 4u + (uint32_t)count * 8u;
            if (pos + need > t_size)              return;

            const float scale = 1.0f / float(1u << fmt);
            out.channel[s].keys.clear();
            out.channel[s].keys.reserve(count);
            for (uint16_t k = 0; k < count; ++k) {
                const uint8_t* kf = t + pos + 4 + k * 8;
                NMTNKeyframe kfo;
                kfo.frame = U16(kf);
                kfo.value = (float)I16(kf + 2) * scale;
                kfo.tan_in = (float)I16(kf + 4) * scale;
                kfo.tan_out = (float)I16(kf + 6) * scale;
                out.channel[s].keys.push_back(kfo);
            }
            pos += need;
        }
    }

    // POST-PROCESS rotation channels (slots 3,4,5) to fix two interpolation
    // pathologies that produce visible "tweaks" in the rendered animation:
    //
    //  (1) Euler antipodal flips: detected by sampling all 3 rotation channels
    //      together. Must run BEFORE the per-axis unwrap because the antipodal
    //      transform itself can push values outside [-π,π] which the unwrap
    //      step then normalises consistently.
    //
    //  (2) Per-axis ±π wraparound: handles a single rotation channel crossing
    //      ±π during a continuous rotation. Runs AFTER the antipodal fix.
    FixEulerAntipodalFlips(out);
    for (int s = 3; s <= 5; ++s) {
        if (out.field[s] == 2) UnwrapRotationChannel(out.channel[s]);
    }

    // Final pass: rebuild rotation tangents from the now-corrected values so the
    // stale flip/wrap tangents can no longer drive Hermite into overshoot (tremble).
    for (int s = 3; s <= 5; ++s) {
        if (out.field[s] == 2) RecomputeRotationTangents(out.channel[s]);
    }
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

    while (pos + 20 <= limit) {
        // A track's size lives at +0x10. The list ends when that size is no
        // longer a sane track length (a trailing null/pad block reads as 0).
        // Do NOT terminate on an all-null name: some files (e.g. ep001 em07_*)
        // begin with a valid but unnamed root track, and bailing on the empty
        // name dropped every track in the chunk. ParseTrack already skips
        // empty-named tracks, so an unnamed slot is simply walked over.
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