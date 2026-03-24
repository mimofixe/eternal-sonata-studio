#pragma once
#include <vector>
#include <string>
#include <cstdint>

// Creates valid AT3 files with RIFF/WAVE container for ATRAC3 codec
class ATRAC3Writer {
public:
    static bool WriteAT3File(const std::vector<uint8_t>& atrac3_payload,
        uint32_t sample_rate,
        uint16_t channels,
        uint16_t block_align,
        const std::string& output_path);

private:
    static void WriteU32LE(std::vector<uint8_t>& buf, uint32_t value);
    static void WriteU16LE(std::vector<uint8_t>& buf, uint16_t value);

    // Creates 14-byte extradata required by ATRAC3 decoder
    static std::vector<uint8_t> MakeATRAC3Extradata(uint32_t samples_per_channel,
        uint16_t coding_mode,
        uint16_t frame_factor);
};