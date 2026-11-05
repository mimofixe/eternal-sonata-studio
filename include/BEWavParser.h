#pragma once
#include <string>
#include <cstdint>
#include <atomic>

// Big-endian PCM WAV, as shipped in assets/sound/cxs alongside the .cxs music.
//
// These are ordinary RIFF/WAVE files except that every size and every field is
// big-endian, and the 16-bit samples are stored big-endian too. Converting one
// is a byte-swap, not a decode. They are also large - the retail set runs to
// 250 MB each - so nothing here loads a whole file into memory.
//
// This mirrors scripts/convert_wav.py in the Reprise repo.
struct BEWavInfo {
    std::string filename;
    bool valid = false;

    uint16_t audio_format = 0;   // 1 = PCM
    uint16_t channels = 0;
    uint32_t sample_rate = 0;
    uint32_t byte_rate = 0;
    uint16_t block_align = 0;
    uint16_t bits = 0;

    uint64_t data_offset = 0;    // absolute offset of PCM in the source file
    uint64_t data_size = 0;
    uint64_t file_size = 0;

    uint64_t Frames() const {
        const uint32_t fsz = uint32_t(channels) * (bits / 8);
        return fsz ? data_size / fsz : 0;
    }
    double DurationSeconds() const {
        return sample_rate ? double(Frames()) / double(sample_rate) : 0.0;
    }
};

class BEWavParser {
public:
    // A big-endian WAV looks identical to a little-endian one in its first 12
    // bytes, so telling them apart means checking which way the sizes read.
    // Needs at least 44 bytes.
    static bool IsBEWav(const uint8_t* data, size_t size);

    // Reads only the header; never touches the PCM.
    static bool Load(const std::string& filepath, BEWavInfo& out);

    // Streams the file through a byte-swap into a standard little-endian WAV.
    // Never holds more than a small window in memory, so a 250 MB source is
    // fine. progress (0..1) and cancel are both optional.
    static bool ConvertToLE(const BEWavInfo& info,
        const std::string& output_path,
        std::atomic<float>* progress = nullptr,
        std::atomic<bool>* cancel = nullptr);
};
