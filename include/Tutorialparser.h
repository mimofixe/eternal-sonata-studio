#pragma once
#include <vector>
#include <string>
#include <cstdint>

// Tutorial voiceover files: t0001.e, t0002.e, t0003.e
// Magic 0x00000181 shared with AI scripts; distinguished by code_size == 0x1FE8.
//
// Layout:
//   [0x00] header (24 bytes): magic, hash, secondary_id, file_size, code_size=0x1FE8, unknown
//   [0x18] VM bytecode (8168 bytes, identical in all three files)
//   [0x2000] CSL table A — Japanese audio
//   [after A] CSL table B — English audio
//
// CSL: "CSL " + 0x18 + 0x1000 + N×u32BE relative offsets + 0x00000000
// Each offset points to a CSF section:
//   "CSF " + total_size + header_size + audio_size
//   Inside header: BOOK→SONG (sequence_id at SONG+0x14),
//                  PGHD→PROG→TIM→LIP (lip events at LIP+0x10)
//   After header: raw ATRAC3 (48000 Hz mono, 192 B/frame)
//
// LIP events: (phoneme:u8, duration:u8) pairs, 00 00 terminated
//   0=neutral  1=A/ah  2=E/ee  3=O/oh  4=U/oo  5=M/mm

static const uint32_t TUTORIAL_CODE_SIZE = 0x1FE8;

inline const char* PhonemeName(uint8_t ph) {
    switch (ph) {
    case 0: return "Neutral";
    case 1: return "A / ah";
    case 2: return "E / ee";
    case 3: return "O / oh";
    case 4: return "U / oo";
    case 5: return "M / mm";
    default: return "?";
    }
}

struct LipEvent {
    uint8_t phoneme;
    uint8_t duration;  // frames
};

struct TutorialSection {
    uint32_t index;
    uint32_t sequence_id;
    uint32_t sample_rate;
    uint32_t audio_size;
    uint32_t header_size;
    uint32_t abs_offset;
    std::vector<LipEvent> lip_events;
    std::vector<uint8_t>  audio_data;
};

struct TutorialTrack {
    uint32_t                     csl_offset;
    uint32_t                     section_count;
    std::vector<TutorialSection> sections;
};

struct TutorialFile {
    std::string   filename;
    bool          valid = false;
    uint32_t      hash;
    uint32_t      secondary_id;
    uint32_t      file_size;
    TutorialTrack track_a;  // English
    TutorialTrack track_b;  // Japanese

    uint32_t TotalSections()   const { return (uint32_t)track_a.sections.size(); }
    uint32_t FirstSequenceId() const { return track_a.sections.empty() ? 0 : track_a.sections.front().sequence_id; }
    uint32_t LastSequenceId()  const { return track_a.sections.empty() ? 0 : track_a.sections.back().sequence_id; }
};

class TutorialParser {
public:
    static bool          IsTutorialFile(const uint8_t* data, size_t size);
    static bool          IsTutorialFile(const std::string& filepath);
    static TutorialFile  Parse(const std::string& filepath);
    static bool          ExportSectionAT3(const TutorialSection& sec, const std::string& output_path);
    static int           ExportTrackAT3(const TutorialTrack& track, const std::string& base_dir, const std::string& prefix);

private:
    static uint32_t              ReadU32BE(const uint8_t* d);
    static bool                  ParseCSL(const std::vector<uint8_t>& data, uint32_t csl_offset, TutorialTrack& track);
    static bool                  ParseCSFSection(const std::vector<uint8_t>& data, uint32_t abs_offset, uint32_t idx, TutorialSection& sec);
    static uint32_t              FindSequenceId(const std::vector<uint8_t>& data, uint32_t csf_start, uint32_t header_end);
    static std::vector<LipEvent> FindLipEvents(const std::vector<uint8_t>& data, uint32_t csf_start, uint32_t header_end);
    static uint32_t              FindSecondCSL(const std::vector<uint8_t>& data, const TutorialTrack& track_a);
};