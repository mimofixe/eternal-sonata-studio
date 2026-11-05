#pragma once
#include <string>
#include <vector>
#include <cstdint>

// CXS - the Xbox 360 build's streaming BGM format (assets/sound/cxs).
//
// A flat 0x1000-byte big-endian header followed by a single raw XMA2 packet
// stream. Unlike CSF there is no clip table: one file is one track.
//
//   0x00  "CXS "
//   0x04  packet size, always 2048
//   0x08  sample rate (48000 across every retail file)
//   0x0C  channel count (2 across every retail file)
//   0x10  total decoded samples per channel - exact, unlike CSF
//   0x14  loop start in samples (0 when the track doesn't loop)
//   0x18  loop end in samples   (0 when the track doesn't loop)
//   0x1C  block count, = ceil(data_size / 0x10000)
//   0x20  data offset, always 0x1000
//   0x24  data size
//   0x28  \_ both 2048; the encoder's packet/alignment granularity, repeated
//   0x2C  /  (kept as fields since nothing observed varies them)
//
// Verified across all 62 .cxs in the retail Xbox 360 build: every one is
// 48 kHz stereo, data_offset + data_size == file size, every packet header
// passes the XMA2 sanity check, and 47 carry loop points.
struct CXSFile {
    std::string filename;
    bool valid = false;

    uint32_t packet_size = 0;
    uint32_t sample_rate = 0;
    uint32_t channels = 0;
    uint32_t total_samples = 0;   // per channel
    uint32_t loop_start = 0;      // samples; 0 = no loop
    uint32_t loop_end = 0;
    uint32_t block_count = 0;
    uint32_t data_offset = 0;
    uint32_t data_size = 0;

    std::vector<uint8_t> audio_data;   // raw XMA2 packet stream

    bool HasLoop() const { return loop_end > loop_start; }
    double DurationSeconds() const {
        return sample_rate ? double(total_samples) / double(sample_rate) : 0.0;
    }
};

class CXSParser {
public:
    // Cheap magic test, for routing a file to this parser.
    static bool IsCXS(const uint8_t* data, size_t size);

    static bool Load(const std::string& filepath, CXSFile& out);
    static bool LoadFromMemory(const uint8_t* data, size_t size, CXSFile& out);

    // Rewrap the stream as a playable RIFF/XMA2 file. Uses the header's exact
    // sample count and loop points rather than the estimate XMAWriter derives
    // from packet headers.
    static bool ExportXMA(const CXSFile& cxs, const std::string& output_path);

private:
    static bool ParseHeader(CXSFile& out, const uint8_t* data, size_t size);
};
