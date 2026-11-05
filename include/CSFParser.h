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

// Which codec the audio section holds. The CSF framing is identical on both
// platforms - only the payload differs - so this is detected from the data
// rather than read out of a header field (see CSFParser::DetectCodec).
enum class CSFCodec {
    ATRAC3,   // PS3: Sony ATRAC3, 192-byte frames -> .at3
    XMA2      // Xbox 360: XMA2, 2048-byte packets -> .xma
};

const char* CSFCodecName(CSFCodec codec);
const char* CSFCodecExtension(CSFCodec codec);   // ".at3" / ".xma"

// Complete CSF file structure
struct CSFFile {
    std::string filename;
    bool valid;

    CSFCodec codec = CSFCodec::ATRAC3;

    // CSF header (offsets 0x00-0x0F)
    uint32_t file_size;
    uint32_t audio_start;
    uint32_t audio_size;

    // BOOK section metadata
    uint32_t book_length;
    uint32_t clip_count;
    uint32_t book_version;

    std::vector<CSFClip> clips;
    std::vector<uint8_t> audio_data;  // Raw codec streams concatenated

    CSFFile() : valid(false), file_size(0), audio_start(0), audio_size(0),
        book_length(0), clip_count(0), book_version(0) {
    }
};

// Parses CSF audio container files
class CSFParser {
public:
    static bool Load(const std::string& filepath, CSFFile& out);
    static bool LoadFromMemory(const uint8_t* data, size_t size, CSFFile& out);
    // Export one clip in whatever codec the container actually holds. The
    // extension of output_path should match CSFCodecExtension(csf.codec);
    // callers that build their own names should use that helper.
    static bool ExportClip(const CSFFile& csf, uint32_t clip_index, const std::string& output_path);
    static bool ExportAllClips(const CSFFile& csf, const std::string& base_path);

    // Codec-specific entry points. Prefer ExportClip unless you have already
    // established which codec you are dealing with.
    static bool ExportClipAT3(const CSFFile& csf, uint32_t clip_index, const std::string& output_path);
    static bool ExportClipXMA(const CSFFile& csf, uint32_t clip_index, const std::string& output_path);

private:
    static uint32_t ReadU32BE(const uint8_t* data);
    static bool ValidateHeader(const uint8_t* data, size_t size);
    static bool ParseBOOK(CSFFile& csf, const uint8_t* data, size_t size);
    static bool ExtractTIMSections(CSFFile& csf, const uint8_t* data, size_t size);
    static CSFCodec DetectCodec(const CSFFile& csf);

    // Shared by ExportClipAT3/ExportClipXMA: bounds-checks the clip against
    // the audio section and copies its payload out.
    static bool SliceClip(const CSFFile& csf, uint32_t clip_index,
        std::vector<uint8_t>& payload_out);
};