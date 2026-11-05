#pragma once
#include <vector>
#include <cstdint>
#include <cstddef>

// NTEX chunks: "NTEX" + BE32 chunk size, followed by a bog-standard
// little-endian PC DDS file (header magic "DDS ", 124-byte header, then
// mip level 0 followed by the mip chain). Unlike NTX2, the payload is
// *not* Xbox-tiled and *not* byte-swapped - verified by decoding
// btldata/map/adg90.bop's NTEX chunks straight with a stock BC1 decoder.
//
// NTEX chunks sit in the same NMDL texture list as NTX3/NTX2 and consume a
// texture index, so any traversal that builds the material -> texture table
// must count them even when it cannot decode them.

struct NTEXInfo {
    int  width = 0;
    int  height = 0;
    bool is_dxt5 = false;   // full 8-bit alpha -> needs GL_BLEND
};

class NtexParser {
public:
    // Decodes mip level 0 of the DDS inside an NTEX chunk to RGBA8.
    // Returns an empty vector if the chunk isn't a DDS we can decode
    // (e.g. the 16bpp uncompressed light/shadow maps, which no material
    // ever references). info_out.width/height are still filled in when the
    // header parses, so callers can report the slot.
    static std::vector<uint8_t> Decode(const uint8_t* chunk_data,
        size_t chunk_size,
        NTEXInfo& info_out);
};
