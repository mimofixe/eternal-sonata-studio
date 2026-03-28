#include "CSFParser.h"
#include "ATRAC3Writer.h"
#include <fstream>
#include <iostream>
#include <cstring>

uint32_t CSFParser::ReadU32BE(const uint8_t* data) {
    return (data[0] << 24) | (data[1] << 16) | (data[2] << 8) | data[3];
}

bool CSFParser::ValidateHeader(const uint8_t* data, size_t size) {
    if (size < 16) {
        return false;
    }

    if (memcmp(data, "CSF ", 4) != 0) {
        return false;
    }

    return true;
}

// Parse BOOK section which contains clip count and metadata
bool CSFParser::ParseBOOK(CSFFile& csf, const uint8_t* data, size_t size) {
    const uint32_t book_pos = 0x10;

    if (book_pos + 16 > size) {
        std::cerr << "File too small for BOOK section" << std::endl;
        return false;
    }

    if (memcmp(&data[book_pos], "BOOK", 4) != 0) {
        std::cerr << "BOOK magic not found" << std::endl;
        return false;
    }

    csf.book_length = ReadU32BE(&data[book_pos + 4]);
    csf.clip_count = ReadU32BE(&data[book_pos + 8]);
    csf.book_version = ReadU32BE(&data[book_pos + 12]);

    std::cout << "BOOK: length=" << csf.book_length
        << ", clips=" << csf.clip_count << std::endl;

    return true;
}

// Extract TIM sections which describe each audio clip
// TIM structure: "TIM " + length + fields
// Important fields: [1]=sample_rate, [2]=offset, [3]=size
bool CSFParser::ExtractTIMSections(CSFFile& csf, const uint8_t* data, size_t size) {
    const uint32_t book_pos = 0x10;
    const uint32_t search_start = book_pos + 8 + csf.book_length;
    const uint32_t search_end = csf.audio_start;

    uint32_t pos = search_start;
    uint32_t tim_count = 0;

    while (pos < search_end - 20) {
        if (memcmp(&data[pos], "TIM ", 4) == 0) {
            CSFClip clip;
            clip.tim_offset = pos;
            clip.tim_length = ReadU32BE(&data[pos + 4]);
            clip.index = tim_count;

            clip.format = ReadU32BE(&data[pos + 8]);
            clip.sample_rate = ReadU32BE(&data[pos + 12]);
            clip.start_offset = ReadU32BE(&data[pos + 16]);
            clip.size_bytes = ReadU32BE(&data[pos + 20]);

            csf.clips.push_back(clip);
            tim_count++;

            pos += 8 + clip.tim_length;
        }
        else {
            pos++;
        }
    }

    std::cout << "Found " << tim_count << " TIM sections" << std::endl;

    return tim_count > 0;
}

// Load and parse CSF file
// CSF format: header (0x00-0x0F) + BOOK section + TIM sections + audio data
bool CSFParser::Load(const std::string& filepath, CSFFile& out) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open: " << filepath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    size_t fileSize = file.tellg();
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer(fileSize);
    file.read(reinterpret_cast<char*>(buffer.data()), fileSize);
    file.close();

    std::cout << "\n=== CSF Parser ===" << std::endl;
    std::cout << "File: " << filepath << std::endl;
    std::cout << "Size: " << fileSize << " bytes" << std::endl;

    if (!ValidateHeader(buffer.data(), fileSize)) {
        std::cerr << "Invalid CSF header" << std::endl;
        out.valid = false;
        return false;
    }

    out.filename = filepath;
    out.valid = true;

    // Read CSF header
    out.file_size = ReadU32BE(&buffer[0x04]);
    out.audio_start = ReadU32BE(&buffer[0x08]);
    out.audio_size = ReadU32BE(&buffer[0x0C]);

    std::cout << "Audio start: 0x" << std::hex << out.audio_start << std::dec << std::endl;
    std::cout << "Audio size: " << out.audio_size << " bytes" << std::endl;

    if (!ParseBOOK(out, buffer.data(), fileSize)) {
        out.valid = false;
        return false;
    }

    if (!ExtractTIMSections(out, buffer.data(), fileSize)) {
        out.valid = false;
        return false;
    }

    // Extract audio section (concatenated ATRAC3 streams)
    if (out.audio_start < fileSize) {
        size_t audio_size = fileSize - out.audio_start;
        out.audio_data.resize(audio_size);
        memcpy(out.audio_data.data(), &buffer[out.audio_start], audio_size);
        std::cout << "Extracted audio: " << audio_size << " bytes" << std::endl;
    }
    else {
        std::cerr << "Invalid audio_start" << std::endl;
        out.valid = false;
        return false;
    }

    std::cout << "==================\n" << std::endl;

    return true;
}

// Parse a CSF chunk that is already loaded in memory (e.g. inside a BMD file)
bool CSFParser::LoadFromMemory(const uint8_t* data, size_t size, CSFFile& out) {
    if (!ValidateHeader(data, size)) {
        out.valid = false;
        return false;
    }

    out.valid = true;

    out.file_size = ReadU32BE(&data[0x04]);
    out.audio_start = ReadU32BE(&data[0x08]);
    out.audio_size = ReadU32BE(&data[0x0C]);

    if (!ParseBOOK(out, data, size)) {
        out.valid = false;
        return false;
    }

    if (!ExtractTIMSections(out, data, size)) {
        out.valid = false;
        return false;
    }

    if (out.audio_start < size) {
        size_t audio_size = size - out.audio_start;
        out.audio_data.resize(audio_size);
        memcpy(out.audio_data.data(), &data[out.audio_start], audio_size);
    }
    else {
        out.valid = false;
        return false;
    }

    return true;
}

// Export single clip as valid AT3 file with RIFF/WAVE container
bool CSFParser::ExportClipAT3(const CSFFile& csf, uint32_t clip_index, const std::string& output_path) {
    if (!csf.valid || clip_index >= csf.clips.size()) {
        return false;
    }

    const CSFClip& clip = csf.clips[clip_index];

    if (clip.start_offset + clip.size_bytes > csf.audio_data.size()) {
        std::cerr << "Clip data out of bounds" << std::endl;
        return false;
    }

    // Extract raw ATRAC3 payload
    std::vector<uint8_t> payload(csf.audio_data.begin() + clip.start_offset,
        csf.audio_data.begin() + clip.start_offset + clip.size_bytes);

    // ATRAC3 parameters for Eternal Sonata voice clips
    uint32_t sample_rate = clip.sample_rate;
    uint16_t channels = 1;         // Mono
    uint16_t block_align = 0xC0;   // 192 bytes per frame

    return ATRAC3Writer::WriteAT3File(payload, sample_rate, channels, block_align, output_path);
}

// Export all clips with naming pattern: base_000.at3, base_001.at3, etc.
bool CSFParser::ExportAllClipsAT3(const CSFFile& csf, const std::string& base_path) {
    if (!csf.valid) {
        return false;
    }

    int success_count = 0;

    for (size_t i = 0; i < csf.clips.size(); i++) {
        std::string filename = base_path;
        size_t ext_pos = filename.find_last_of('.');
        if (ext_pos != std::string::npos) {
            filename = filename.substr(0, ext_pos);
        }

        char num_buf[16];
        snprintf(num_buf, sizeof(num_buf), "_%03d.at3", (int)i);
        filename += num_buf;

        if (ExportClipAT3(csf, i, filename)) {
            success_count++;
        }
    }

    std::cout << "\nExported " << success_count << "/" << csf.clips.size() << " clips" << std::endl;

    return (success_count > 0);
}