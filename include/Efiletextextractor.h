#pragma once
#include <vector>
#include <string>
#include <cstdint>
#include <cstddef>

// Decodes the `BTX ` text sections of a decoded .e file. Layout is from
// sub_8223B780 in default.xex (see EternalSonataReprise/docs/asset-formats.md
// §3.4). All fields are u32 big-endian, offsets relative to their own struct.
// The sub-block chain does not reliably self-terminate, so the walk is
// bounded by the declared language count rather than a null "next" link.

struct BtxEntry {
    uint32_t string_id;
    std::string text; // raw bytes, not language-decoded (JPN is Shift-JIS, western blocks are single-byte)
};

struct BtxLanguageBlock {
    std::string language; // fourcc, e.g. "USA "
    std::vector<BtxEntry> entries; // sorted by string_id
};

struct BtxBlob {
    size_t file_offset;
    std::vector<BtxLanguageBlock> languages;
};

class EFileTextExtractor {
public:
    EFileTextExtractor();
    ~EFileTextExtractor();

    void LoadFromFile(const std::vector<uint8_t>& file_data);
    void Clear();
    void Render();

private:
    std::vector<BtxBlob> ParseBtxBlobs(const uint8_t* data, size_t size);
    bool ParseBtxBlobAt(const uint8_t* data, size_t size, size_t base, BtxBlob& out);

    std::vector<BtxBlob> m_Blobs;
    bool m_HasData;
};
