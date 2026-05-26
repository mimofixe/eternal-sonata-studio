#include "TcxDecompressor.h"
#include <cstdint>
#include <cstring>
#include <fstream>

namespace {

    constexpr uint32_t MAGIC_BMD = 0x424D4420; // "BMD "
    constexpr uint32_t MAGIC_BOP = 0x424F5020; // "BOP "
    constexpr uint32_t MAGIC_NOBJ = 0x4E4F424A; // "NOBJ"
    constexpr uint32_t MAGIC_NTX3 = 0x4E545833; // "NTX3"
    constexpr uint32_t MAGIC_NTX2 = 0x4E545832; // "NTX2"
    constexpr uint32_t MAGIC_NTEX = 0x4E544558; // "NTEX"
    constexpr uint32_t MAGIC_CSF = 0x43534620; // "CSF "
    constexpr uint32_t MAGIC_CAMP = 0x43414D50; // "CAMP"
    constexpr uint32_t MAGIC_SCP = 0x53435020; // "SCP "
    constexpr uint32_t MAGIC_PACK = 0x5041434B; // "PACK"
    constexpr uint32_t MAGIC_FONT = 0x464F4E54; // "FONT"
    constexpr uint32_t MAGIC_AISCRIPT = 0x00000181; // ai script
    constexpr uint32_t MAGIC_MEFC = 0x4D656663; // "Mefc"
    constexpr uint32_t MAGIC_BOOK = 0x424F4F4B; // "BOOK"

    static inline uint32_t LoadBE32(const uint8_t* p) {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16)
            | (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }

    static bool IsKnownUncompressedMagic(uint32_t m) {
        switch (m) {
        case MAGIC_BMD:
        case MAGIC_BOP:
        case MAGIC_NOBJ:
        case MAGIC_NTX3:
        case MAGIC_NTX2:
        case MAGIC_NTEX:
        case MAGIC_CSF:
        case MAGIC_CAMP:
        case MAGIC_SCP:
        case MAGIC_PACK:
        case MAGIC_FONT:
        case MAGIC_AISCRIPT:
        case MAGIC_MEFC:
        case MAGIC_BOOK:
            return true;
        default:
            return false;
        }
    }

    // For formats that store their own decompressed size in a BE32 at offset 0x04,
    // returns the size. Returns 0 if the magic is unknown or has no embedded size.
    // This is used to trim the LZSS over-shoot at the end of a decompressed stream.
    static uint32_t EmbeddedSizeForMagic(uint32_t magic, const uint8_t* data, size_t avail) {
        if (avail < 8) return 0;
        switch (magic) {
        case MAGIC_BMD:
        case MAGIC_BOP:
        case MAGIC_CAMP:
        case MAGIC_SCP:
            return LoadBE32(data + 4);
        default:
            return 0;
        }
    }

    struct Decoder {
        const uint8_t* input;
        size_t input_size;
        size_t input_pos;

        // Arithmetic coder state (matches PE state struct):
        //   low   @ +0x2330
        //   range @ +0x2334
        //   code  @ +0x2338
        uint32_t low;
        uint32_t range;
        uint32_t code;

        // Frequency tables (matches PE):
        //   freqs at state+0x2E  (256 bytes copied from input header)
        //   cum   at state+0x130 (256 halfwords, cum[N] = sum freqs[0..N])
        //   inv   at state+0x330 (size = total = cum[255])
        uint8_t  freqs[256];
        uint16_t cum[256];
        uint32_t total;
        std::vector<uint8_t> inv;

        // LZSS state (matches PE):
        //   dict   @ +0x233C  (4096 bytes, zero-init)
        //   dpos   @ +0x333C  (init 0xFEE)
        //   ctrl   @ +0x3340  (current control byte, 8 literal/ref flags)
        //   mask   @ +0x3341  (single-bit rotating mask)
        //   mode   @ +0x3342  (0=read control, 1=literal, 2=offset_lo, 3=offset_hi+len)
        //   offlo  @ +0x3344  (captured low byte of back-ref offset)
        uint8_t  dict[4096];
        uint16_t dpos;
        uint8_t  ctrl;
        uint8_t  mask;
        uint8_t  mode;
        uint8_t  offlo;

        std::vector<uint8_t> output;

        // Decodes one symbol from the arithmetic stream.
        // Returns -1 on EOF, otherwise the symbol value (0..255).
        int decode_one_symbol() {
            // Norm 1: while top byte of `low` equals top byte of `low+range`,
            //         shift code/range/low left by 8 and read a fresh byte.
            for (;;) {
                uint32_t high = low + range;
                if ((high ^ low) >= 0x01000000u) break;
                if (input_pos >= input_size) return -1;
                uint8_t b = input[input_pos++];
                code = (code << 8) | b;
                range = (range << 8);
                low = (low << 8);
            }
            // Norm 2 (underflow): while range < 0x2000, shift code/low and rebuild
            // range = ((-low) << 8) masked with 0x001FFF00.
            while (range < 0x2000u) {
                if (input_pos >= input_size) return -1;
                uint8_t b = input[input_pos++];
                code = (code << 8) | b;
                uint32_t neg_low = (uint32_t)(-(int32_t)low);
                range = (neg_low << 8) & 0x001FFF00u;
                low = (low << 8);
            }
            // Decode step:
            //   scale  = range / total
            //   offset = (code - low) / scale
            //   sym    = inv[offset]
            //   low   += scale * cum[sym - 1]    (cum_below for sym, 0 if sym==0)
            //   range  = scale * freqs[sym]
            uint32_t scale = range / total;
            if (scale == 0) return -1;
            uint32_t offset = (code - low) / scale;
            if (offset >= total) offset = total - 1;
            uint8_t sym = inv[offset];
            uint32_t cum_below = (sym == 0) ? 0u : (uint32_t)cum[sym - 1];
            low += scale * cum_below;
            range = scale * (uint32_t)freqs[sym];
            return sym;
        }
    };

    static bool InitDecoder(Decoder& d, const uint8_t* input, size_t input_size) {
        if (input_size < 256 + 4) return false;
        d.input = input;
        d.input_size = input_size;
        d.input_pos = 0;

        std::memcpy(d.freqs, input, 256);
        d.input_pos = 256;

        uint32_t s = 0;
        for (int i = 0; i < 256; ++i) {
            s += d.freqs[i];
            d.cum[i] = (uint16_t)s;
        }
        d.total = s;
        if (d.total == 0) return false;

        d.inv.assign(d.total, 0);
        for (int sym = 0; sym < 256; ++sym) {
            uint32_t start = (sym == 0) ? 0u : d.cum[sym - 1];
            uint32_t end = d.cum[sym];
            for (uint32_t k = start; k < end; ++k) d.inv[k] = (uint8_t)sym;
        }

        d.low = 0;
        d.range = 0xFFFFFFFFu;
        d.code = 0;
        for (int i = 0; i < 4; ++i) {
            d.code = (d.code << 8) | input[d.input_pos++];
        }

        std::memset(d.dict, 0, sizeof(d.dict));
        d.dpos = 0xFEE;
        d.ctrl = 0;
        d.mask = 1;
        d.mode = 0;
        d.offlo = 0;
        return true;
    }

    // Drives the LZSS state machine until target bytes are produced or EOF.
    // If target == 0, runs until EOF.
    static void DecodeLoop(Decoder& d, size_t target) {
        auto needMore = [&]() -> bool {
            return target == 0 || d.output.size() < target;
            };

        while (needMore()) {
            int s = d.decode_one_symbol();
            if (s < 0) return;
            uint8_t sym = (uint8_t)s;

            switch (d.mode) {
            case 0:
                d.ctrl = sym;
                d.mask = 1;
                d.mode = (sym & 1) ? 1 : 2;
                break;

            case 1: {
                d.output.push_back(sym);
                d.dict[d.dpos] = sym;
                d.dpos = (d.dpos + 1) & 0xFFF;
                d.mask <<= 1;
                if (d.mask == 0) d.mode = 0;
                else d.mode = (d.ctrl & d.mask) ? 1 : 2;
                break;
            }

            case 2:
                d.offlo = sym;
                d.mode = 3;
                break;

            case 3: {
                uint32_t length = (sym & 0x0F) + 3;
                uint32_t offset_hi = ((uint32_t)(sym & 0xF0)) << 4;
                uint32_t full_off = (offset_hi | d.offlo) & 0xFFF;
                for (uint32_t k = 0; k < length; ++k) {
                    uint8_t b = d.dict[(full_off + k) & 0xFFF];
                    d.output.push_back(b);
                    d.dict[d.dpos] = b;
                    d.dpos = (d.dpos + 1) & 0xFFF;
                }
                d.mask <<= 1;
                if (d.mask == 0) d.mode = 0;
                else d.mode = (d.ctrl & d.mask) ? 1 : 2;
                break;
            }
            }
        }
    }

}

bool TcxDecompressor::IsCompressed(const uint8_t* data, size_t size) {
    if (!data || size < 260) return false;
    uint32_t magic = LoadBE32(data);
    if (IsKnownUncompressedMagic(magic)) return false;

    // Sanity check on the freq table: must have non-trivial sum and variety.
    uint32_t sum = 0;
    int distinct = 0;
    uint8_t seen[256] = { 0 };
    for (int i = 0; i < 256; ++i) {
        sum += data[i];
        if (!seen[data[i]]) { seen[data[i]] = 1; ++distinct; }
    }
    if (sum < 64 || distinct < 8) return false;
    return true;
}

std::vector<uint8_t> TcxDecompressor::Decompress(const uint8_t* input,
    size_t input_size,
    size_t expected_size_hint) {
    std::vector<uint8_t> empty;
    Decoder d;
    if (!InitDecoder(d, input, input_size)) return empty;
    size_t reserve = expected_size_hint ? expected_size_hint
        : (input_size * 3);
    d.output.reserve(reserve);
    DecodeLoop(d, expected_size_hint);

    // If the caller didn't provide a target size, the LZSS state machine may
    // have produced a few trailing bytes past the end of the logical file
    // (a back-reference whose copy length spilled beyond the true end). Many
    // game container formats (BMD, BOP, CAMP, SCP) store their decompressed
    // size as a BE32 at offset 0x04 - trim to that value if we recognise it.
    if (expected_size_hint == 0 && d.output.size() >= 8) {
        uint32_t magic = LoadBE32(d.output.data());
        uint32_t embedded = EmbeddedSizeForMagic(magic, d.output.data(),
            d.output.size());
        if (embedded != 0 && embedded < d.output.size() &&
            (d.output.size() - embedded) < 32) {
            d.output.resize(embedded);
        }
    }

    return d.output;
}

std::vector<uint8_t> TcxDecompressor::LoadAndMaybeDecompress(const std::string& path,
    bool* was_compressed) {
    if (was_compressed) *was_compressed = false;
    std::ifstream f(path, std::ios::binary);
    if (!f) return {};
    f.seekg(0, std::ios::end);
    size_t size = (size_t)f.tellg();
    f.seekg(0);
    std::vector<uint8_t> bytes(size);
    if (size) f.read(reinterpret_cast<char*>(bytes.data()), (std::streamsize)size);

    if (!IsCompressed(bytes.data(), bytes.size())) return bytes;

    std::vector<uint8_t> out = Decompress(bytes.data(), bytes.size());
    if (out.empty()) return bytes;
    if (was_compressed) *was_compressed = true;
    return out;
}