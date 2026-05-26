#pragma once
#include <cstdint>
#include <string>
#include <vector>

// Decompressor for Eternal Sonata Xbox 360 RETAIL files (.e / .bmd / .bop).
//
// Retail files use a proprietary tri-Crescendo (Tcx) compression:
//   - 256-byte static frequency table at the start of the file
//   - Arithmetic coded stream (32-bit low/range/code, double normalization)
//   - LZSS layer on top (4KB dict, 12-bit offset + 4-bit length+3,
//     1 control byte = 8 bits of literal/back-ref flags)
//
// Reverse-engineered from the Xbox 360 PE:
//   - 0x821189B8  init        (reads freq table, builds cum + inverse tables)
//   - 0x82118AF8  decode_sym  (arithmetic coder, returns one byte/symbol)
//   - 0x82118C70  decode_loop (LZSS state machine driving decode_sym)
//
// Demo files (PS3 / Xbox 360 demo) are NOT compressed and pass straight through.

class TcxDecompressor {
public:
    // Returns true if `data` starts with a Tcx-compressed stream.
    // The check is: if the first 4 bytes don't match any known uncompressed magic
    // (BMD , BOP , NOBJ, NTX3, CSF , CAMP, PACK, AI-script 0x00000181, etc.)
    // and the file is large enough to hold a 256-byte freq table + body,
    // we assume compressed.
    static bool IsCompressed(const uint8_t* data, size_t size);

    // Decompresses `input` (the whole file contents, including 256-byte header)
    // and returns the decompressed bytes. On failure returns an empty vector.
    //
    // If `expected_size_hint` is non-zero it caps the output size;
    // otherwise the decoder runs until the arithmetic stream is exhausted.
    static std::vector<uint8_t> Decompress(const uint8_t* input,
        size_t input_size,
        size_t expected_size_hint = 0);

    // Convenience: load file, detect, decompress (if needed), return the bytes
    // that should be parsed. Returns empty on I/O error.
    // `was_compressed` (if non-null) is set to true iff decompression happened.
    static std::vector<uint8_t> LoadAndMaybeDecompress(const std::string& path,
        bool* was_compressed = nullptr);
};