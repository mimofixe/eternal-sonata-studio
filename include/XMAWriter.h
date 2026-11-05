#pragma once
#include <vector>
#include <string>
#include <cstdint>

// Writes Xbox 360 XMA2 bitstreams into a RIFF/WAVE container (format tag
// 0x0166, XMA2WAVEFORMATEX). The payload inside a CSF is already a raw XMA2
// packet stream - 2048-byte packets, big-endian packet headers - so nothing is
// transcoded here; we only prepend the header a decoder needs to find the
// sample rate, channel count and block layout.
//
// The result plays in ffmpeg ("ffmpeg -i clip.xma clip.wav") and vgmstream.
// Optional per-stream detail. CSF has to estimate the sample count from
// packet headers because its TIM descriptor doesn't carry one; CXS states it
// exactly and adds loop points, so it fills these in instead.
struct XMAStreamInfo {
    uint32_t sample_rate = 0;
    uint16_t channels = 1;

    // 0 = derive an upper bound by summing packet frame counts.
    uint32_t samples_encoded = 0;

    // Both 0 = no loop.
    uint32_t loop_start = 0;
    uint32_t loop_end = 0;
};

class XMAWriter {
public:
    // XMA2 packets are a fixed 2048 bytes; a payload that isn't a whole
    // number of packets has been mis-sliced upstream.
    static constexpr uint32_t PACKET_SIZE = 2048;

    // Number of packets grouped into one seek/decode block. 64 KiB is what the
    // XDK encoder emits by default and what the CSF payloads are cut to.
    static constexpr uint32_t BYTES_PER_BLOCK = 0x10000;

    static bool WriteXMAFile(const std::vector<uint8_t>& xma2_payload,
        uint32_t sample_rate,
        uint16_t channels,
        const std::string& output_path);

    static bool WriteXMAFile(const std::vector<uint8_t>& xma2_payload,
        const XMAStreamInfo& info,
        const std::string& output_path);

private:
    static void WriteU32LE(std::vector<uint8_t>& buf, uint32_t value);
    static void WriteU16LE(std::vector<uint8_t>& buf, uint16_t value);
};
