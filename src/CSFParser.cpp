#include "CSFParser.h"
#include "ATRAC3Writer.h"
#include "XMAWriter.h"
#include <fstream>
#include <iostream>
#include <cstring>

const char* CSFCodecName(CSFCodec codec) {
    return codec == CSFCodec::XMA2 ? "XMA2 (Xbox 360)" : "ATRAC3 (PS3)";
}

const char* CSFCodecExtension(CSFCodec codec) {
    return codec == CSFCodec::XMA2 ? ".xma" : ".at3";
}

// The CSF container is byte-identical between the PS3 and Xbox 360 builds -
// same "CSF "/BOOK/TIM framing, same big-endian fields, and the TIM format
// word is 0 on both - so the codec has to be inferred from the payload.
//
// XMA2 is a stream of fixed 2048-byte packets, and the encoder pads every clip
// out to a whole packet; ATRAC3 uses 192-byte frames and has no such
// alignment. On top of the size test we sanity-check each packet header (a
// big-endian dword whose top 6 bits are a frame count and whose bottom 8 are a
// skip count), which is what rules out an ATRAC3 clip that happens to land on
// a 2048-byte boundary. Verified against all 68 CSF chunks in the Xbox 360
// campdata/scp.bmd: 3174/3174 packets pass, and every clip is packet-aligned
// while only 25 of 68 are 192-aligned.
CSFCodec CSFParser::DetectCodec(const CSFFile& csf) {
    if (csf.clips.empty() || csf.audio_data.empty())
        return CSFCodec::ATRAC3;

    for (const CSFClip& clip : csf.clips) {
        if (clip.size_bytes == 0 ||
            clip.size_bytes % XMAWriter::PACKET_SIZE != 0 ||
            clip.start_offset % XMAWriter::PACKET_SIZE != 0)
            return CSFCodec::ATRAC3;
    }

    size_t checked = 0;
    for (const CSFClip& clip : csf.clips) {
        for (uint32_t off = 0; off + 4 <= clip.size_bytes;
            off += XMAWriter::PACKET_SIZE) {
            const size_t abs = size_t(clip.start_offset) + off;
            if (abs + 4 > csf.audio_data.size())
                return CSFCodec::ATRAC3;

            const uint32_t hdr = ReadU32BE(&csf.audio_data[abs]);
            const uint32_t frame_count = hdr >> 26;
            const uint32_t skip_count = hdr & 0xFF;

            // 0xFF is the "no frame starts in this packet" sentinel; anything
            // else above 63 can't be a real count.
            if (frame_count > 63 || (skip_count != 0xFF && skip_count > 63))
                return CSFCodec::ATRAC3;
            checked++;
        }
    }

    return checked > 0 ? CSFCodec::XMA2 : CSFCodec::ATRAC3;
}

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
        out.codec = DetectCodec(out);
        std::cout << "Codec: " << CSFCodecName(out.codec) << std::endl;
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
        out.codec = DetectCodec(out);
    }
    else {
        out.valid = false;
        return false;
    }

    return true;
}

// Bounds-check a clip against the audio section and copy its payload out
bool CSFParser::SliceClip(const CSFFile& csf, uint32_t clip_index,
    std::vector<uint8_t>& payload_out) {
    if (!csf.valid || clip_index >= csf.clips.size()) {
        return false;
    }

    const CSFClip& clip = csf.clips[clip_index];

    if (size_t(clip.start_offset) + clip.size_bytes > csf.audio_data.size()) {
        std::cerr << "Clip data out of bounds" << std::endl;
        return false;
    }

    payload_out.assign(csf.audio_data.begin() + clip.start_offset,
        csf.audio_data.begin() + clip.start_offset + clip.size_bytes);
    return true;
}

// Export single clip as valid AT3 file with RIFF/WAVE container
bool CSFParser::ExportClipAT3(const CSFFile& csf, uint32_t clip_index, const std::string& output_path) {
    std::vector<uint8_t> payload;
    if (!SliceClip(csf, clip_index, payload)) return false;

    // ATRAC3 parameters for Eternal Sonata voice clips
    uint32_t sample_rate = csf.clips[clip_index].sample_rate;
    uint16_t channels = 1;         // Mono
    uint16_t block_align = 0xC0;   // 192 bytes per frame

    return ATRAC3Writer::WriteAT3File(payload, sample_rate, channels, block_align, output_path);
}

// Export single clip as an XMA2 RIFF (Xbox 360)
bool CSFParser::ExportClipXMA(const CSFFile& csf, uint32_t clip_index, const std::string& output_path) {
    std::vector<uint8_t> payload;
    if (!SliceClip(csf, clip_index, payload)) return false;

    // Every Xbox 360 CSF clip observed is a single mono 48 kHz stream; the
    // rate still comes from the TIM descriptor rather than being hardcoded.
    return XMAWriter::WriteXMAFile(payload, csf.clips[clip_index].sample_rate,
        1, output_path);
}

bool CSFParser::ExportClip(const CSFFile& csf, uint32_t clip_index, const std::string& output_path) {
    return csf.codec == CSFCodec::XMA2
        ? ExportClipXMA(csf, clip_index, output_path)
        : ExportClipAT3(csf, clip_index, output_path);
}

// Export all clips with naming pattern: base_000.at3, base_001.at3, etc.
// (or _000.xma ... for Xbox 360 containers)
bool CSFParser::ExportAllClips(const CSFFile& csf, const std::string& base_path) {
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
        snprintf(num_buf, sizeof(num_buf), "_%03d", (int)i);
        filename += num_buf;
        filename += CSFCodecExtension(csf.codec);

        if (ExportClip(csf, (uint32_t)i, filename)) {
            success_count++;
        }
    }

    std::cout << "\nExported " << success_count << "/" << csf.clips.size() << " clips" << std::endl;

    return (success_count > 0);
}