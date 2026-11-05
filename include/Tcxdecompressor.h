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
// Some files (e.g. the credits tables op.bmd / ab.bmd) instead use only the
// LZSS layer, with no frequency table and no arithmetic coder - the stream
// starts directly with a control byte. These are Haruhiko Okumura's classic
// LZSS parameters (4 KB window zero-filled, write position 0xFEE, 12-bit
// offset + 4-bit length+3, 1 control byte = 8 LSB-first flags), which is the
// same window layout the Tcx path drives, just fed raw bytes.
//
// Demo files (PS3 / Xbox 360 demo) are NOT compressed and pass straight through.

class TcxDecompressor {
public:
    enum class Codec {
        None,          // already decompressed
        Tcx,           // 256-byte freq table + arithmetic coder + LZSS
        Lzss,          // bare LZSS stream, no arithmetic layer
        LzssNulEaten,  // bare LZSS, but every 0x00 byte was replaced by 0x20
    };

    // LzssNulEaten marks a file damaged in transit: it is a valid LZSS stream
    // whose NUL bytes have all become spaces (a text-mode transfer will do
    // this). The damage is lossy - a space and an eaten NUL are no longer
    // distinguishable - so such files cannot be decompressed, only recognised.
    // Decompress() returns empty for them rather than emitting noise.
    static const char* DescribeCodec(Codec c);

    // Identifies which codec (if any) `data` is encoded with.
    //
    // Lzss is tested first and is the more specific test: we trial-decode the
    // first 8 bytes and require a known magic plus a plausible embedded size.
    // The Tcx test is only a frequency-table sanity check, which a bare LZSS
    // stream would also pass, so order matters here.
    static Codec DetectCodec(const uint8_t* data, size_t size);

    // Returns true if `data` starts with a compressed stream of either codec.
    // The check is: if the first 4 bytes don't match any known uncompressed magic
    // (BMD , BOP , NOBJ, NTX3, CSF , CXS , CAMP, PACK, AI-script 0x00000181, etc.)
    // and the file is large enough to hold a 256-byte freq table + body,
    // we assume compressed.
    static bool IsCompressed(const uint8_t* data, size_t size);

    // Same test, reading only the file's first 260 bytes. Prefer this over
    // loading a file just to find out whether it needs decompressing.
    static bool IsFileCompressed(const std::string& path);

    // DetectCodec against the file's first 260 bytes only.
    static Codec DetectFileCodec(const std::string& path);

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