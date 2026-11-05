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

// Counts occurrences of a 4-byte magic in the buffer. Used only for the
// scene-content test below, so a plain scan is fine.
static uint32_t CountMagic(const uint8_t* data, size_t size, const char magic[4]) {
    if (size < 4) return 0;
    uint32_t n = 0;
    for (size_t i = 0; i + 4 <= size; i++)
        if (data[i] == uint8_t(magic[0]) && memcmp(data + i, magic, 4) == 0) n++;
    return n;
}

// True when the buffer carries in-engine scene data (models, skeletons, animation,
// cameras, textures). Tutorial files are audio + subtitles only and contain none.
bool TutorialParser::HasSceneContent(const uint8_t* data, size_t size) {
    static const char* kSceneMagics[] = {
        "NSHP", "NMTN", "NCAM", "NOBJ", "NMDL", "NBN2", "NTX2", "NTX3"
    };
    uint32_t hits = 0;
    for (const char* m : kSceneMagics) {
        hits += CountMagic(data, size, m);
        if (hits >= 4) return true;   // early out; see threshold note below
    }
    return hits >= 4;
}

bool TutorialParser::IsTutorialFile(const uint8_t* data, size_t size) {
    if (size < 0x18) return false;
    if (ReadU32BE(data + 0x00) != 0x00000181) return false;
    if (ReadU32BE(data + 0x10) != TUTORIAL_CODE_SIZE) return false;

    // 1. The 4 bytes at 0x2000 must be "CSL ".
    //    Scene files that have Mefc/NOBJ at 0x2000 (e.g. Tnk01.e) are rejected here.
    if (size < 0x2004) return false;
    if (memcmp(data + 0x2000, "CSL ", 4) != 0) return false;

    // 2. The u32 at 0x2004 is the section COUNT, not a byte size (t0001 = 24,
    //    t0002 = 25, t0003 = 33). Only sanity-bound it. The old code required
    //    it to be >= 0x10 "to hold one offset entry", which was both the wrong
    //    interpretation of the field and an accidental filter.
    if (size < 0x2008) return false;
    const uint32_t section_count = ReadU32BE(data + 0x2004);
    if (section_count == 0 || section_count > 4096) return false;

    // 3. Reject cutscene files. This is the part that actually separates the two,
    //    and it cannot be done from the header alone.
    //
    //    Scene .e files share the magic, the 0x1FE8 code size, a "CSL " table at
    //    0x2000, and CSF/ATRAC3 sections with LIP and SONG sub-chunks — the audio
    //    container is identical in kind, so nothing in the CSL distinguishes them.
    //    The old test assumed tutorial audio sat immediately after the CSL table
    //    (first offset < 0x4000). That is false on the Xbox 360 retail build,
    //    where the two ranges overlap completely: tutorials span 0xB000..0x26000
    //    and scenes 0x0..0x24000, so the check rejected all three tutorials.
    //
    //    What does separate them is content: a tutorial is audio and subtitles
    //    with no in-engine scene. Measured over all 678 .e files in the retail
    //    build, 63 pass the header checks above; of those, exactly 3 contain zero
    //    scene chunks (t0001/t0002/t0003) and the other 60 contain at least 7.
    //    The threshold of 4 sits in that gap, leaving margin against a stray magic
    //    appearing by chance inside several MB of ATRAC3 payload.
    if (HasSceneContent(data, size)) return false;

    return true;
}

bool TutorialParser::IsTutorialFile(const std::string& filepath) {
    // The scene-content test needs the whole file, not just a header: the chunks
    // that mark a cutscene live well past 0x2000. Reading a short prefix here
    // would classify every scene file as a tutorial.
    std::ifstream f(filepath, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff len = f.tellg();
    if (len <= 0) return false;
    f.seekg(0);
    std::vector<uint8_t> buf(static_cast<size_t>(len));
    f.read(reinterpret_cast<char*>(buf.data()), len);
    return IsTutorialFile(buf.data(), buf.size());
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