#include "TutorialParser.h"
#include "ATRAC3Writer.h"
#include <fstream>
#include <cstring>
#include <algorithm>
#include <filesystem>

namespace fs = std::filesystem;

uint32_t TutorialParser::ReadU32BE(const uint8_t* d) {
    return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
}

bool TutorialParser::IsTutorialFile(const uint8_t* data, size_t size) {
    if (size < 0x18) return false;
    return ReadU32BE(data + 0x00) == 0x00000181 &&
        ReadU32BE(data + 0x10) == TUTORIAL_CODE_SIZE;
}

bool TutorialParser::IsTutorialFile(const std::string& filepath) {
    std::ifstream f(filepath, std::ios::binary);
    if (!f) return false;
    uint8_t buf[0x18] = {};
    f.read(reinterpret_cast<char*>(buf), sizeof(buf));
    return IsTutorialFile(buf, sizeof(buf));
}

uint32_t TutorialParser::FindSequenceId(const std::vector<uint8_t>& data,
    uint32_t csf_start, uint32_t header_end) {
    for (uint32_t p = csf_start + 0x10; p + 0x18 <= header_end; p++) {
        if (data[p] == 'S' && data[p + 1] == 'O' && data[p + 2] == 'N' && data[p + 3] == 'G') {
            uint32_t id = ReadU32BE(data.data() + p + 0x14);
            if (id > 10000 && id < 100000) return id;
        }
    }
    return 0;
}

std::vector<LipEvent> TutorialParser::FindLipEvents(const std::vector<uint8_t>& data,
    uint32_t csf_start, uint32_t header_end) {
    std::vector<LipEvent> events;
    for (uint32_t p = csf_start + 0x10; p + 16 <= header_end; p++) {
        if (data[p] == 'L' && data[p + 1] == 'I' && data[p + 2] == 'P' && data[p + 3] == ' ') {
            uint32_t lip_end = std::min(p + ReadU32BE(data.data() + p + 4), header_end);
            // 16-byte LIP header: magic(4) + size(4) + field1(4) + field2(4)
            uint32_t ep = p + 16;
            while (ep + 1 < lip_end) {
                uint8_t ph = data[ep], dur = data[ep + 1];
                if (ph == 0 && dur == 0) break;
                if (ph <= 5 && dur > 0) events.push_back({ ph, dur });
                ep += 2;
            }
            return events;
        }
    }
    return events;
}

bool TutorialParser::ParseCSFSection(const std::vector<uint8_t>& data,
    uint32_t abs_offset, uint32_t idx,
    TutorialSection& sec) {
    if (abs_offset + 16 > data.size()) return false;
    if (memcmp(data.data() + abs_offset, "CSF ", 4) != 0) return false;

    sec.index = idx;
    sec.abs_offset = abs_offset;
    sec.header_size = ReadU32BE(data.data() + abs_offset + 0x08);
    sec.audio_size = ReadU32BE(data.data() + abs_offset + 0x0C);
    sec.sample_rate = 48000;

    uint32_t header_end = abs_offset + sec.header_size;
    uint32_t audio_start = abs_offset + sec.header_size;
    uint32_t audio_end = std::min(audio_start + sec.audio_size, (uint32_t)data.size());

    if (header_end > data.size()) return false;

    // Try to read sample rate from TIM chunk
    for (uint32_t p = abs_offset + 0x10; p + 16 <= header_end; p++) {
        if (data[p] == 'T' && data[p + 1] == 'I' && data[p + 2] == 'M' && data[p + 3] == ' ') {
            uint32_t sr = ReadU32BE(data.data() + p + 0x0C);
            if (sr >= 8000 && sr <= 192000) { sec.sample_rate = sr; break; }
        }
    }

    sec.sequence_id = FindSequenceId(data, abs_offset, header_end);
    sec.lip_events = FindLipEvents(data, abs_offset, header_end);

    if (audio_start < data.size() && sec.audio_size > 0)
        sec.audio_data.assign(data.begin() + audio_start, data.begin() + audio_end);

    return true;
}

bool TutorialParser::ParseCSL(const std::vector<uint8_t>& data,
    uint32_t csl_offset, TutorialTrack& track) {
    if (csl_offset + 16 > data.size()) return false;
    if (memcmp(data.data() + csl_offset, "CSL ", 4) != 0) return false;

    track.csl_offset = csl_offset;

    std::vector<uint32_t> rel_offsets;
    for (uint32_t i = 0; ; i++) {
        uint32_t p = csl_offset + 0x0C + i * 4;
        if (p + 4 > data.size()) break;
        uint32_t v = ReadU32BE(data.data() + p);
        if (v == 0) break;
        rel_offsets.push_back(v);
    }

    track.section_count = (uint32_t)rel_offsets.size();
    track.sections.reserve(rel_offsets.size());

    for (uint32_t i = 0; i < rel_offsets.size(); i++) {
        TutorialSection sec;
        if (ParseCSFSection(data, csl_offset + rel_offsets[i], i, sec))
            track.sections.push_back(std::move(sec));
    }

    return !track.sections.empty();
}

uint32_t TutorialParser::FindSecondCSL(const std::vector<uint8_t>& data,
    const TutorialTrack& track_a) {
    if (track_a.sections.empty()) return 0;
    const auto& last = track_a.sections.back();
    uint32_t search = last.abs_offset + last.header_size + last.audio_size;

    // Scan forward aligned to 0x10
    while (search + 4 < data.size()) {
        if (memcmp(data.data() + search, "CSL ", 4) == 0) return search;
        search += 0x10;
    }
    // Byte-by-byte fallback
    for (uint32_t p = last.abs_offset + last.header_size + last.audio_size;
        p + 4 < data.size(); p++)
        if (memcmp(data.data() + p, "CSL ", 4) == 0) return p;

    return 0;
}

TutorialFile TutorialParser::Parse(const std::string& filepath) {
    TutorialFile result;
    result.filename = filepath;

    std::ifstream file(filepath, std::ios::binary);
    if (!file) return result;

    file.seekg(0, std::ios::end);
    std::vector<uint8_t> data((size_t)file.tellg());
    file.seekg(0);
    file.read(reinterpret_cast<char*>(data.data()), data.size());
    file.close();

    if (!IsTutorialFile(data.data(), data.size())) return result;

    result.hash = ReadU32BE(data.data() + 0x04);
    result.secondary_id = ReadU32BE(data.data() + 0x08);
    result.file_size = ReadU32BE(data.data() + 0x0C);

    if (!ParseCSL(data, 0x2000, result.track_a)) return result;

    uint32_t csl_b = FindSecondCSL(data, result.track_a);
    if (csl_b != 0) ParseCSL(data, csl_b, result.track_b);

    result.valid = true;
    return result;
}

bool TutorialParser::ExportSectionAT3(const TutorialSection& sec,
    const std::string& output_path) {
    if (sec.audio_data.empty()) return false;
    return ATRAC3Writer::WriteAT3File(sec.audio_data, sec.sample_rate, 1, 0xC0, output_path);
}

int TutorialParser::ExportTrackAT3(const TutorialTrack& track,
    const std::string& base_dir,
    const std::string& prefix) {
    fs::create_directories(base_dir);
    int count = 0;
    for (const auto& sec : track.sections) {
        char fname[64];
        snprintf(fname, sizeof(fname), "%s_%05u.at3", prefix.c_str(), sec.sequence_id);
        if (ExportSectionAT3(sec, (fs::path(base_dir) / fname).string()))
            count++;
    }
    return count;
}