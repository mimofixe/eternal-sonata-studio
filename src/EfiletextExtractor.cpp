#include "EFileTextExtractor.h"
#include <imgui.h>
#include <cstring>
#include <algorithm>

namespace {

uint32_t ReadU32BE(const uint8_t* data, size_t size, size_t off, bool& ok) {
    if (off + 4 > size) {
        ok = false;
        return 0;
    }
    return (uint32_t(data[off]) << 24) | (uint32_t(data[off + 1]) << 16) |
           (uint32_t(data[off + 2]) << 8) | uint32_t(data[off + 3]);
}

// The western BTX blocks are single-byte, Latin-1-like (see
// docs/asset-formats.md §3.4.2 - not confirmed to be true Latin-1 vs. a
// custom glyph table, but Latin-1 is the working assumption). ImGui expects
// UTF-8, so any byte >= 0x80 handed to it raw is an invalid lead byte and
// renders as '?'. Widen each such byte to its two-byte UTF-8 encoding.
std::string Latin1ToUtf8(const std::string& input) {
    std::string out;
    out.reserve(input.size());
    for (unsigned char c : input) {
        if (c < 0x80) {
            out.push_back((char)c);
        } else {
            out.push_back((char)(0xC0 | (c >> 6)));
            out.push_back((char)(0x80 | (c & 0x3F)));
        }
    }
    return out;
}

} // namespace

EFileTextExtractor::EFileTextExtractor()
    : m_HasData(false) {
}

EFileTextExtractor::~EFileTextExtractor() {
    Clear();
}

void EFileTextExtractor::Clear() {
    m_Blobs.clear();
    m_HasData = false;
}

// Parses one BTX blob starting at `base`. Returns false (and leaves `out`
// untouched) if the bytes at `base` don't validate as a real blob - the
// caller is scanning for the magic, so false positives are expected.
bool EFileTextExtractor::ParseBtxBlobAt(const uint8_t* data, size_t size, size_t base, BtxBlob& out) {
    if (base + 0x10 > size) return false;
    if (memcmp(data + base, "BTX ", 4) != 0) return false;

    bool ok = true;
    uint32_t first = ReadU32BE(data, size, base + 0x04, ok);
    uint32_t blob_size = ReadU32BE(data, size, base + 0x08, ok);
    uint32_t nlangs = ReadU32BE(data, size, base + 0x0C, ok);
    if (!ok) return false;
    if (nlangs == 0 || nlangs > 16) return false;
    if (blob_size == 0 || blob_size > size - base) return false;

    size_t blob_end = base + blob_size;

    BtxBlob blob;
    blob.file_offset = base;

    size_t q = base + first;
    for (uint32_t li = 0; li < nlangs; li++) {
        if (q + 0x14 > size) return false;

        char fourcc[5] = { 0 };
        memcpy(fourcc, data + q, 4);
        std::string lang(fourcc);
        // Every known language fourcc is 3 letters + a trailing space.
        if (lang.size() != 4 || lang[3] != ' ') return false;
        for (int i = 0; i < 3; i++) {
            if (!isalpha((unsigned char)lang[i])) return false;
        }

        uint32_t etab_off = ReadU32BE(data, size, q + 0x04, ok);
        uint32_t next_off = ReadU32BE(data, size, q + 0x08, ok);
        uint32_t entry_count = ReadU32BE(data, size, q + 0x10, ok);
        if (!ok) return false;
        if (entry_count > 0x10000) return false;

        BtxLanguageBlock language_block;
        language_block.language = lang;

        for (uint32_t i = 0; i < entry_count; i++) {
            size_t e = q + etab_off + 8 * (size_t)i;
            uint32_t sid = ReadU32BE(data, size, e, ok);
            uint32_t soff = ReadU32BE(data, size, e + 4, ok);
            if (!ok) return false;

            size_t start = q + soff;
            if (start < base || start >= blob_end) return false;

            size_t stop = start;
            while (stop < blob_end && data[stop] != 0) stop++;
            if (stop >= blob_end) return false; // unterminated

            std::string raw((const char*)data + start, stop - start);

            BtxEntry entry;
            entry.string_id = sid;
            // JPN is Shift-JIS, not Latin-1; leave it as raw bytes rather
            // than mis-decoding it through the western conversion.
            entry.text = (lang == "JPN ") ? raw : Latin1ToUtf8(raw);
            language_block.entries.push_back(std::move(entry));
        }

        std::sort(language_block.entries.begin(), language_block.entries.end(),
            [](const BtxEntry& a, const BtxEntry& b) { return a.string_id < b.string_id; });

        blob.languages.push_back(std::move(language_block));

        // The sub-block chain does not reliably self-terminate (the last
        // language's "next" can point past the blob into unrelated data),
        // so advance by it but trust the declared language count to stop.
        q += next_off;
    }

    out = std::move(blob);
    return true;
}

std::vector<BtxBlob> EFileTextExtractor::ParseBtxBlobs(const uint8_t* data, size_t size) {
    std::vector<BtxBlob> blobs;
    if (size < 4) return blobs;

    for (size_t i = 0; i + 4 <= size; i++) {
        if (memcmp(data + i, "BTX ", 4) != 0) continue;

        BtxBlob blob;
        if (ParseBtxBlobAt(data, size, i, blob)) {
            blobs.push_back(std::move(blob));
        }
    }

    return blobs;
}

void EFileTextExtractor::LoadFromFile(const std::vector<uint8_t>& file_data) {
    Clear();

    if (file_data.empty()) {
        return;
    }

    m_Blobs = ParseBtxBlobs(file_data.data(), file_data.size());
    m_HasData = true;
}

void EFileTextExtractor::Render() {
    if (!m_HasData) {
        ImGui::TextDisabled("No file loaded");
        ImGui::Separator();
        ImGui::TextWrapped("Select a .e file in the File Browser to decode its BTX text section(s).");
        return;
    }

    if (m_Blobs.empty()) {
        ImGui::TextDisabled("No BTX text section found in this file");
        ImGui::Separator();
        ImGui::TextWrapped("This .e file has no `BTX ` blob. Map/model/AI-script .e files "
            "typically carry no text; tutorial and cfdata dialogue files do.");
        return;
    }

    static char search_buf[256] = "";

    ImGui::Text("%zu BTX blob(s) found", m_Blobs.size());
    ImGui::SetNextItemWidth(300);
    ImGui::InputTextWithHint("##search", "Filter text...", search_buf, sizeof(search_buf));
    ImGui::Separator();

    std::string filter(search_buf);
    std::transform(filter.begin(), filter.end(), filter.begin(), ::tolower);

    ImGui::BeginChild("FileTextScrollRegion", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);

    for (size_t bi = 0; bi < m_Blobs.size(); bi++) {
        const BtxBlob& blob = m_Blobs[bi];

        char header[64];
        snprintf(header, sizeof(header), "Blob #%zu @ offset 0x%zX", bi, blob.file_offset);
        if (!ImGui::CollapsingHeader(header, bi == 0 ? ImGuiTreeNodeFlags_DefaultOpen : 0)) {
            continue;
        }

        if (ImGui::BeginTabBar((std::string("##langs") + std::to_string(bi)).c_str())) {
            for (const BtxLanguageBlock& lang : blob.languages) {
                if (!ImGui::BeginTabItem((lang.language + "###" + lang.language).c_str())) {
                    continue;
                }

                if (ImGui::BeginTable("##entries", 2,
                        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {
                    ImGui::TableSetupColumn("Id", ImGuiTableColumnFlags_WidthFixed, 50.0f);
                    ImGui::TableSetupColumn("Text", ImGuiTableColumnFlags_WidthStretch);
                    ImGui::TableHeadersRow();

                    for (const BtxEntry& entry : lang.entries) {
                        if (!filter.empty()) {
                            std::string lower_text = entry.text;
                            std::transform(lower_text.begin(), lower_text.end(), lower_text.begin(), ::tolower);
                            if (lower_text.find(filter) == std::string::npos) continue;
                        }

                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::Text("%u", entry.string_id);
                        ImGui::TableSetColumnIndex(1);

                        // Newlines in this format are the literal two
                        // characters '\' 'n', not a real line break.
                        bool is_dialogue = entry.text.find("<w") != std::string::npos;
                        ImVec4 color = is_dialogue ? ImVec4(0.5f, 1.0f, 0.5f, 1.0f) : ImVec4(0.85f, 0.85f, 0.85f, 1.0f);
                        ImGui::TextColored(color, "%s", entry.text.c_str());
                    }

                    ImGui::EndTable();
                }

                ImGui::EndTabItem();
            }
            ImGui::EndTabBar();
        }
    }

    ImGui::EndChild();
}
