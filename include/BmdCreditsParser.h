#pragma once
#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

// Parser for the credits / staff-roll BMD subtype (op.bmd, ab.bmd, ed1.bmd,
// ed2.bmd).
//
// These files carry the scrolling credits, one section per language slot.
// They are NOT laid out like the BMD containers ContainerParser handles: there
// is no section count at 0x08 and no chunk magic + size per section. Instead:
//
//   0x00  "BMD "
//   0x04  BE32 total file size
//   0x08  BE32 section offset[0]      absolute, ascending
//   0x0C  BE32 section offset[1]
//    ..   ..                          terminated by a zero entry
//
// Each section is a table of fixed-size records followed by the string pool
// they point into. In both variants the table stops 4 bytes short of the pool.
//
// Two record layouts exist:
//
//   Opening (op.bmd, ab.bmd) - 16 bytes, one row per credit:
//     +0x00 BE32  line count (or 0xFFFFFFFF)
//     +0x04 BE32  timing, around -10000
//     +0x08 BE32  timing, around -10000
//     +0x0C BE32  string offset, always set and strictly ascending
//
//   Ending (ed1.bmd, ed2.bmd) - 12 bytes, one row per scroll step:
//     +0x00 BE32  timing, around -10000
//     +0x04 BE32  vertical advance in pixels (0x18, 0x20, 0x80, 0x100, ...)
//     +0x08 BE32  string offset, or 0 for a blank spacer row
//
// The two are told apart by whether the first field looks like a timing value.
//
// Strings are NUL-terminated Shift-JIS and carry inline layout tags:
//   <d24> <d32> <d48>  following text is drawn at that size
//   \n                 literal backslash + n, a line break
//   <#201>             a codepoint escape, here E-acute
//
// A language slot that was never populated still gets a full record table, but
// every entry resolves to empty text.

struct CreditRecord {
    int32_t  timing = 0;         // scroll timing, around -10000
    int32_t  timing2 = 0;        // opening layout only; 0 otherwise
    uint32_t param = 0;          // opening: line count. ending: pixel advance
    uint32_t string_offset = 0;  // 0 means a spacer row
    std::string text;            // UTF-8, converted from Shift-JIS
    bool     is_spacer = false;
    bool     truncated = false;  // ran to EOF without a terminator
};

struct CreditSection {
    size_t   offset = 0;         // where this section starts in the file
    size_t   pool_offset = 0;    // where its string pool starts
    size_t   record_size = 0;    // 16 (opening) or 12 (ending)
    bool     is_placeholder = false;  // parsed fine, but holds no visible text
    std::vector<CreditRecord> records;
};

struct CreditsFile {
    bool valid = false;
    std::vector<CreditSection> sections;
    size_t TotalRecords() const;
};

class BmdCreditsParser {
public:
    // Cheap structural test - safe to call on any BMD. It is deliberately
    // strict so an ordinary BMD container (whose 0x08 holds a section *count*,
    // not an offset) cannot be mistaken for this subtype.
    static bool IsCreditsBmd(const uint8_t* data, size_t size);

    // Parses a whole decompressed credits BMD. Returns false, leaving
    // out.valid == false, if the file is not this subtype.
    static bool Parse(const uint8_t* data, size_t size, CreditsFile& out);

    // Strips the layout tags and turns the literal two-character "\n" into a
    // real newline, giving the text roughly as the game lays it out.
    static std::string StripTags(const std::string& tagged);

    // Shift-JIS (cp932) to UTF-8. Falls back to a byte-wise copy of the ASCII
    // range if the platform conversion is unavailable, so the result is always
    // valid UTF-8 and safe to hand to ImGui.
    static std::string ShiftJisToUtf8(const std::string& sjis);
};
