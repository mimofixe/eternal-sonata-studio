#include "ATRAC3Writer.h"
#include <fstream>
#include <iostream>

void ATRAC3Writer::WriteU32LE(std::vector<uint8_t>& buf, uint32_t value) {
    buf.push_back(value & 0xFF);
    buf.push_back((value >> 8) & 0xFF);
    buf.push_back((value >> 16) & 0xFF);
    buf.push_back((value >> 24) & 0xFF);
}

void ATRAC3Writer::WriteU16LE(std::vector<uint8_t>& buf, uint16_t value) {
    buf.push_back(value & 0xFF);
    buf.push_back((value >> 8) & 0xFF);
}

// ATRAC3 extradata format (14 bytes):
// [0-1]   always 1
// [2-5]   samples_per_channel (u32)
// [6-7]   coding_mode
// [8-9]   coding_mode duplicate (required)
// [10-11] frame_factor
// [12-13] padding
std::vector<uint8_t> ATRAC3Writer::MakeATRAC3Extradata(uint32_t samples_per_channel,
    uint16_t coding_mode,
    uint16_t frame_factor) {
    std::vector<uint8_t> extra;

    WriteU16LE(extra, 1);
    WriteU32LE(extra, samples_per_channel);
    WriteU16LE(extra, coding_mode);
    WriteU16LE(extra, coding_mode);
    WriteU16LE(extra, frame_factor);
    WriteU16LE(extra, 0);

    return extra;
}

bool ATRAC3Writer::WriteAT3File(const std::vector<uint8_t>& atrac3_payload,
    uint32_t sample_rate,
    uint16_t channels,
    uint16_t block_align,
    const std::string& output_path) {

    std::vector<uint8_t> frames = atrac3_payload;

    // Trim to block_align boundary
    if (frames.size() % block_align != 0) {
        size_t trim = frames.size() % block_align;
        frames.resize(frames.size() - trim);
    }

    // ATRAC3: 1024 samples per frame per channel
    uint32_t nframes = frames.size() / block_align;
    uint32_t samples_per_channel = nframes * 1024;

    // Build extradata
    std::vector<uint8_t> extradata = MakeATRAC3Extradata(samples_per_channel, 0, 1);
    uint16_t cbSize = extradata.size();

    // WAV header parameters
    uint32_t nAvgBytesPerSec = sample_rate * (block_align * channels) / 1024;
    uint16_t blockAlign = block_align * channels;
    uint16_t bitsPerSample = 0;
    uint16_t wFormatTag = 0x0270; // ATRAC3

    // Build fmt chunk
    std::vector<uint8_t> fmt;
    WriteU16LE(fmt, wFormatTag);
    WriteU16LE(fmt, channels);
    WriteU32LE(fmt, sample_rate);
    WriteU32LE(fmt, nAvgBytesPerSec);
    WriteU16LE(fmt, blockAlign);
    WriteU16LE(fmt, bitsPerSample);
    WriteU16LE(fmt, cbSize);
    fmt.insert(fmt.end(), extradata.begin(), extradata.end());

    std::vector<uint8_t> fmt_chunk;
    fmt_chunk.push_back('f'); fmt_chunk.push_back('m');
    fmt_chunk.push_back('t'); fmt_chunk.push_back(' ');
    WriteU32LE(fmt_chunk, fmt.size());
    fmt_chunk.insert(fmt_chunk.end(), fmt.begin(), fmt.end());

    // Build fact chunk
    std::vector<uint8_t> fact_chunk;
    fact_chunk.push_back('f'); fact_chunk.push_back('a');
    fact_chunk.push_back('c'); fact_chunk.push_back('t');
    WriteU32LE(fact_chunk, 4);
    WriteU32LE(fact_chunk, samples_per_channel * channels);

    // Build data chunk
    std::vector<uint8_t> data_chunk;
    data_chunk.push_back('d'); data_chunk.push_back('a');
    data_chunk.push_back('t'); data_chunk.push_back('a');
    WriteU32LE(data_chunk, frames.size());
    data_chunk.insert(data_chunk.end(), frames.begin(), frames.end());

    // Assemble RIFF/WAVE structure
    uint32_t riff_size = 4 + fmt_chunk.size() + fact_chunk.size() + data_chunk.size();

    std::vector<uint8_t> output;
    output.push_back('R'); output.push_back('I');
    output.push_back('F'); output.push_back('F');
    WriteU32LE(output, riff_size);
    output.push_back('W'); output.push_back('A');
    output.push_back('V'); output.push_back('E');
    output.insert(output.end(), fmt_chunk.begin(), fmt_chunk.end());
    output.insert(output.end(), fact_chunk.begin(), fact_chunk.end());
    output.insert(output.end(), data_chunk.begin(), data_chunk.end());

    std::ofstream file(output_path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to create: " << output_path << std::endl;
        return false;
    }

    file.write(reinterpret_cast<const char*>(output.data()), output.size());
    file.close();

    std::cout << "Created AT3: " << output_path << std::endl;

    return true;
}