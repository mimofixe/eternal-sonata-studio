#include "TEXParser.h"
#include <fstream>
#include <cstring>
#include <iostream>

uint32_t TEXParser::ReadU32BE(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
        (static_cast<uint32_t>(data[1]) << 16) |
        (static_cast<uint32_t>(data[2]) << 8) |
        (static_cast<uint32_t>(data[3]));
}

uint16_t TEXParser::ReadU16BE(const uint8_t* data) {
    return static_cast<uint16_t>((data[0] << 8) | data[1]);
}

TEXFile TEXParser::Parse(const std::string& filepath) {
    TEXFile result;
    result.filename = filepath;

    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        return result;
    }

    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0);

    std::vector<uint8_t> data(file_size);
    file.read(reinterpret_cast<char*>(data.data()), file_size);
    file.close();

    if (file_size < 16) {
        return result;
    }

    // Verify magic
    uint32_t magic = ReadU32BE(data.data());
    if (magic != MAGIC) {
        return result;
    }

    // Walk the entry table starting at 0x08.
    // Each entry is 8 bytes:
    //   [0]    type  (0x01=NTX3, 0x02=UV data, 0x09=MLNG/end)
    //   [1]    index (for type 0x02)
    //   [2]    unused
    //   [3]    count (for type 0x02: number of float values)
    //   [4-7]  absolute offset (BE u32)
    size_t pos = 8;
    int ntx3_idx = 0;

    while (pos + 8 <= file_size) {
        uint8_t entry_type = data[pos];
        uint32_t offset = ReadU32BE(data.data() + pos + 4);

        if (entry_type == 0x01) {
            // NTX3 texture chunk
            if (offset + 8 > file_size) { pos += 8; continue; }
            if (memcmp(data.data() + offset, "NTX3", 4) != 0) { pos += 8; continue; }

            uint32_t chunk_size = ReadU32BE(data.data() + offset + 4);
            if (chunk_size < 128 || offset + chunk_size > file_size) { pos += 8; continue; }

            uint16_t width = ReadU16BE(data.data() + offset + 0x20);
            uint16_t height = ReadU16BE(data.data() + offset + 0x22);

            Chunk chunk;
            chunk.offset = offset;
            chunk.size = chunk_size;
            chunk.magic = *reinterpret_cast<const uint32_t*>(data.data() + offset);
            chunk.type = ChunkType::NTX3;
            snprintf(chunk.name, sizeof(chunk.name), "texture_%d_%dx%d", ntx3_idx, width, height);

            result.chunks.push_back(chunk);
            ntx3_idx++;

        }
        else if (entry_type == 0x09) {
            // MLNG section — language table, also signals end of entry list
            if (offset + 0x18 < file_size) {
                // Language codes start at MLNG+0x18, 4 bytes each (e.g. "USA ", "GRB ")
                for (int i = 0; i < 6; i++) {
                    size_t lang_pos = offset + 0x18 + i * 4;
                    if (lang_pos + 4 > file_size) break;
                    char code[5] = { 0 };
                    memcpy(code, data.data() + lang_pos, 4);
                    // Strip trailing spaces and null bytes
                    std::string lang(code);
                    while (!lang.empty() && (lang.back() == ' ' || lang.back() == '\0'))
                        lang.pop_back();
                    if (!lang.empty())
                        result.languages.push_back(lang);
                }
            }
            break; // 0x09 always terminates the table
        }
        // type 0x02 (UV data) is informational — skip for now

        pos += 8;
    }

    result.valid = !result.chunks.empty();

    if (result.valid) {
        std::cout << "TEX: " << filepath << std::endl;
        std::cout << "  NTX3 chunks: " << result.chunks.size() << std::endl;
        if (!result.languages.empty()) {
            std::cout << "  Languages: ";
            for (const auto& l : result.languages) std::cout << l << " ";
            std::cout << std::endl;
        }
    }

    return result;
}