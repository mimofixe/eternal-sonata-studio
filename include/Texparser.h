#pragma once
#include "EFileParser.h"
#include <string>
#include <vector>
#include <cstdint>

// TEX file — UI texture atlas with per-language variants
// Magic: 0x03339010
//
// Header entry table (8 bytes each, starting at 0x08):
//   Type 0x01  — NTX3 chunk offset (texture atlas)
//   Type 0x02  — UV float array (source rect per language variant)
//   Type 0x09  — MLNG section offset (language table, also terminates the entry table)
//
// MLNG section contains the list of supported language codes
// (USA, GRB, FRA, ITA, DEU, ESP) that map to the UV variant entries.

struct TEXFile {
    std::string filename;
    bool valid;

    std::vector<Chunk> chunks;          // NTX3 chunks, ready for EFileTextureViewer
    std::vector<std::string> languages; // from MLNG: USA, GRB, FRA, ITA, DEU, ESP

    TEXFile() : valid(false) {}
};

class TEXParser {
public:
    static const uint32_t MAGIC = 0x03339010;

    static TEXFile Parse(const std::string& filepath);

private:
    static uint32_t ReadU32BE(const uint8_t* data);
    static uint16_t ReadU16BE(const uint8_t* data);
};