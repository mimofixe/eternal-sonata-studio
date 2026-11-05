#include "BmdCreditsParser.h"
#include <cstring>

#ifdef _WIN32
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#endif

namespace {

    constexpr size_t kTableStart = 0x08;   // section offset table
    constexpr size_t kMaxSections = 64;    // the format only ever uses ~10
    constexpr size_t kPoolGap = 4;         // dead bytes between table and pool

    inline uint32_t ReadBE32(const uint8_t* p) {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
            | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }

    // The timing fields sit just under -10000 in every file seen. Anything in
    // that band is treated as a timing value; nothing else in these records is
    // a small negative number, so this cleanly separates the two layouts.
    inline bool IsTiming(uint32_t v) {
        const int32_t s = static_cast<int32_t>(v);
        return s < 0 && s > -32768;
    }

    // Picks the record stride for a section: 12 bytes if the first field is a
    // timing value (ending credits), 16 if the timings are fields 1 and 2
    // instead (opening credits). Returns 0 if neither shape fits.
    size_t StrideFor(const uint8_t* data, size_t size, uint32_t sec) {
        if (sec + 16 > size) return 0;
        if (IsTiming(ReadBE32(data + sec))) return 12;
        if (IsTiming(ReadBE32(data + sec + 4)) &&
            IsTiming(ReadBE32(data + sec + 8))) {
            return 16;
        }
        return 0;
    }

    // Walks a section's record table. The table runs until it reaches the
    // string pool, whose start is the lowest offset any record points at; the
    // last 4 bytes before the pool are a gap. Returns the record count, and
    // sets `pool` to where the strings begin.
    size_t ScanTable(const uint8_t* data, size_t size, uint32_t sec,
        size_t stride, uint32_t& pool) {
        pool = 0;
        uint32_t lowest = 0;
        size_t count = 0;

        for (size_t pos = sec; pos + stride <= size; pos += stride) {
            if (lowest && pos + kPoolGap > lowest) break;

            // Field carrying the timing, and the field carrying the offset.
            const uint32_t timing = ReadBE32(data + pos + (stride == 12 ? 0 : 4));
            if (!IsTiming(timing)) break;

            const uint32_t off = ReadBE32(data + pos + stride - 4);
            if (off >= size) break;
            if (off != 0 && (lowest == 0 || off < lowest)) lowest = off;

            ++count;
            if (count > 4096) break;  // runaway guard
        }

        if (!count || !lowest) return 0;
        // The table must land exactly on the pool, minus the 4-byte gap.
        if (sec + count * stride + kPoolGap != lowest) return 0;

        pool = lowest;
        return count;
    }

    // Reads the NUL-terminated section offset table at 0x08.
    std::vector<uint32_t> ReadSectionTable(const uint8_t* data, size_t size) {
        std::vector<uint32_t> offsets;
        for (size_t p = kTableStart; p + 4 <= size && offsets.size() < kMaxSections; p += 4) {
            const uint32_t v = ReadBE32(data + p);
            if (v == 0) break;              // terminator
            if (v >= size) return {};       // not an offset table after all
            if (!offsets.empty() && v <= offsets.back()) return {};  // must ascend
            offsets.push_back(v);
        }
        return offsets;
    }

}  // namespace

size_t CreditsFile::TotalRecords() const {
    size_t n = 0;
    for (const CreditSection& s : sections) n += s.records.size();
    return n;
}

bool BmdCreditsParser::IsCreditsBmd(const uint8_t* data, size_t size) {
    if (!data || size < 0x40) return false;
    if (std::memcmp(data, "BMD ", 4) != 0) return false;
    if (ReadBE32(data + 4) != static_cast<uint32_t>(size)) return false;

    const std::vector<uint32_t> offsets = ReadSectionTable(data, size);
    if (offsets.empty()) return false;

    // The first entry must clear the table itself. This is what separates the
    // subtype from an ordinary BMD, where 0x08 is a small section *count* and
    // would point back into the header.
    const size_t table_end = kTableStart + (offsets.size() + 1) * 4;
    if (offsets.front() < table_end) return false;

    // At least the first section has to produce a sane record table.
    const size_t stride = StrideFor(data, size, offsets.front());
    if (!stride) return false;
    uint32_t pool = 0;
    return ScanTable(data, size, offsets.front(), stride, pool) > 0;
}

bool BmdCreditsParser::Parse(const uint8_t* data, size_t size, CreditsFile& out) {
    out = CreditsFile{};
    if (!IsCreditsBmd(data, size)) return false;

    for (const uint32_t sec : ReadSectionTable(data, size)) {
        CreditSection section;
        section.offset = sec;

        const size_t stride = StrideFor(data, size, sec);
        uint32_t pool = 0;
        const size_t count = stride ? ScanTable(data, size, sec, stride, pool) : 0;

        section.record_size = count ? stride : 0;
        section.pool_offset = pool;

        bool any_text = false;
        for (size_t i = 0; i < count; ++i) {
            const uint8_t* rec = data + sec + i * stride;

            CreditRecord r;
            if (stride == 12) {
                r.timing = static_cast<int32_t>(ReadBE32(rec));
                r.param = ReadBE32(rec + 4);
            }
            else {
                r.param = ReadBE32(rec);
                r.timing = static_cast<int32_t>(ReadBE32(rec + 4));
                r.timing2 = static_cast<int32_t>(ReadBE32(rec + 8));
            }
            r.string_offset = ReadBE32(rec + stride - 4);
            r.is_spacer = (r.string_offset == 0);

            if (!r.is_spacer && r.string_offset < size) {
                const uint8_t* begin = data + r.string_offset;
                const uint8_t* stop = static_cast<const uint8_t*>(
                    std::memchr(begin, 0x00, size - r.string_offset));
                r.truncated = (stop == nullptr);
                const size_t len = stop ? static_cast<size_t>(stop - begin)
                    : (size - r.string_offset);
                r.text = ShiftJisToUtf8(std::string(
                    reinterpret_cast<const char*>(begin), len));
                if (!StripTags(r.text).empty()) any_text = true;
            }

            section.records.push_back(std::move(r));
        }

        section.is_placeholder = (count > 0) && !any_text;
        out.sections.push_back(std::move(section));
    }

    out.valid = true;
    return true;
}

std::string BmdCreditsParser::StripTags(const std::string& tagged) {
    std::string s;
    s.reserve(tagged.size());

    for (size_t i = 0; i < tagged.size(); ) {
        // "<dNN>" sizing tags drop out entirely; "<#NNN>" escapes are kept as
        // written, since we have no table mapping them back to characters.
        if (tagged[i] == '<' && i + 1 < tagged.size() && tagged[i + 1] == 'd') {
            const size_t close = tagged.find('>', i);
            if (close != std::string::npos && close - i <= 6) {
                bool digits = close > i + 2;
                for (size_t k = i + 2; k < close && digits; ++k) {
                    if (tagged[k] < '0' || tagged[k] > '9') digits = false;
                }
                if (digits) { i = close + 1; continue; }
            }
        }
        if (tagged[i] == '\\' && i + 1 < tagged.size() && tagged[i + 1] == 'n') {
            s.push_back('\n');
            i += 2;
            continue;
        }
        s.push_back(tagged[i]);
        ++i;
    }

    // Trim, so a tags-only string reports as empty.
    const size_t first = s.find_first_not_of(" \t\r\n");
    if (first == std::string::npos) return {};
    const size_t last = s.find_last_not_of(" \t\r\n");
    return s.substr(first, last - first + 1);
}

std::string BmdCreditsParser::ShiftJisToUtf8(const std::string& sjis) {
    if (sjis.empty()) return {};

#ifdef _WIN32
    const int wide_len = MultiByteToWideChar(
        932, 0, sjis.data(), static_cast<int>(sjis.size()), nullptr, 0);
    if (wide_len > 0) {
        std::wstring wide(static_cast<size_t>(wide_len), L'\0');
        MultiByteToWideChar(932, 0, sjis.data(), static_cast<int>(sjis.size()),
            wide.data(), wide_len);

        const int utf8_len = WideCharToMultiByte(
            CP_UTF8, 0, wide.data(), wide_len, nullptr, 0, nullptr, nullptr);
        if (utf8_len > 0) {
            std::string utf8(static_cast<size_t>(utf8_len), '\0');
            WideCharToMultiByte(CP_UTF8, 0, wide.data(), wide_len,
                utf8.data(), utf8_len, nullptr, nullptr);
            return utf8;
        }
    }
#endif

    // Fallback: keep ASCII, drop the rest. ImGui asserts on malformed UTF-8,
    // so passing the raw high bytes through is not an option.
    std::string ascii;
    ascii.reserve(sjis.size());
    for (unsigned char c : sjis) {
        ascii.push_back(c < 0x80 ? static_cast<char>(c) : '?');
    }
    return ascii;
}
