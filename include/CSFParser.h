#pragma once
#include <vector>
#include <string>
#include <cstdint>

// Represents a single audio clip within CSF container
struct CSFClip {
    uint32_t index;
    uint32_t format;
    uint32_t sample_rate;
    uint32_t start_offset;  // Relative to audio section
    uint32_t size_bytes;
    uint32_t tim_offset;    // Position of TIM descriptor in file
    uint32_t tim_length;
};

// Complete CSF file structure
struct CSFFile {
    std::string filename;
    bool valid;

    // CSF header (offsets 0x00-0x0F)
    uint32_t file_size;
    uint32_t audio_start;
    uint32_t audio_size;

    // BOOK section metadata
    uint32_t book_length;
    uint32_t clip_count;
    uint32_t book_version;

    std::vector<CSFClip> clips;
    std::vector<uint8_t> audio_data;  // Raw ATRAC3 streams concatenated

    CSFFile() : valid(false), file_size(0), audio_start(0), audio_size(0),
        book_length(0), clip_count(0), book_version(0) {
    }
};

// Parses CSF audio container files
class CSFParser {
public:
    static bool Load(const std::string& filepath, CSFFile& out);
    static bool LoadFromMemory(const uint8_t* data, size_t size, CSFFile& out);
    static bool ExportClipAT3(const CSFFile& csf, uint32_t clip_index, const std::string& output_path);
    static bool ExportAllClipsAT3(const CSFFile& csf, const std::string& base_path);

private:
    static uint32_t ReadU32BE(const uint8_t* data);
    static bool ValidateHeader(const uint8_t* data, size_t size);
    static bool ParseBOOK(CSFFile& csf, const uint8_t* data, size_t size);
    static bool ExtractTIMSections(CSFFile& csf, const uint8_t* data, size_t size);
};