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
    constexpr uint32_t MAGIC_CXS = 0x43585320; // "CXS "
    constexpr uint32_t MAGIC_RIFF = 0x52494646; // "RIFF" (the big-endian WAVs)
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
        case MAGIC_CXS:
        case MAGIC_RIFF:
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

    // Bare LZSS, no arithmetic layer: the stream is a run of groups, each a
    // control byte followed by its 8 items. A set flag (LSB first) means one
    // literal byte; a clear flag means a 2-byte back-reference into the 4 KB
    // window - offset low byte, then (offset high nibble << 4) | (length - 3).
    //
    // `target` caps the output; 0 means "run until the input is exhausted".
    // A back-reference is always copied whole, so the result can overshoot
    // `target` by up to 17 bytes and callers are expected to trim.
    static std::vector<uint8_t> DecodeLzss(const uint8_t* in, size_t in_size,
        size_t target) {
        std::vector<uint8_t> out;
        if (target) out.reserve(target);

        uint8_t dict[4096] = { 0 };
        uint16_t dpos = 0xFEE;
        size_t i = 0;

        while (i < in_size) {
            const uint8_t ctrl = in[i++];
            for (int bit = 0; bit < 8; ++bit) {
                if (target && out.size() >= target) return out;
                if (i >= in_size) return out;

                if (ctrl & (1u << bit)) {
                    const uint8_t c = in[i++];
                    out.push_back(c);
                    dict[dpos] = c;
                    dpos = (dpos + 1) & 0xFFF;
                }
                else {
                    if (i + 1 >= in_size) return out;
                    const uint8_t lo = in[i++];
                    const uint8_t hi = in[i++];
                    const uint32_t length = (uint32_t)(hi & 0x0F) + 3;
                    const uint32_t offset =
                        ((((uint32_t)(hi & 0xF0)) << 4) | lo) & 0xFFF;
                    for (uint32_t k = 0; k < length; ++k) {
                        const uint8_t v = dict[(offset + k) & 0xFFF];
                        out.push_back(v);
                        dict[dpos] = v;
                        dpos = (dpos + 1) & 0xFFF;
                    }
                }
            }
        }
        return out;
    }

    // Trial-decodes just the 8-byte header of a candidate LZSS stream.
    // Returns false unless it yields a known magic; on success `out_size` gets
    // the format's embedded decompressed size (0 if the format has none).
    static bool ProbeLzss(const uint8_t* data, size_t size, uint32_t* out_size) {
        // 8 output bytes need at most 9 input bytes (all literals).
        const size_t probe_in = size < 16 ? size : 16;
        std::vector<uint8_t> head = DecodeLzss(data, probe_in, 8);
        if (head.size() < 8) return false;

        const uint32_t magic = LoadBE32(head.data());
        if (!IsKnownUncompressedMagic(magic)) return false;

        const uint32_t embedded = EmbeddedSizeForMagic(magic, head.data(), head.size());
        // Formats without an embedded size are still valid; reject only sizes
        // that are present but implausible.
        if (embedded != 0 && (embedded < 16 || embedded > (256u << 20))) return false;

        if (out_size) *out_size = embedded;
        return true;
    }

    // Detects an LZSS stream whose NUL bytes were all replaced by 0x20.
    // Two independent signals have to agree, which keeps this from firing on
    // healthy files: the stream must contain no NUL at all (a real compressed
    // stream of any size always does), and undoing the substitution on the
    // header must produce a known magic.
    static bool ProbeLzssNulEaten(const uint8_t* data, size_t size) {
        // Signal 1: not a single NUL anywhere. Any real compressed stream of
        // this size has some, so their total absence is the damage itself.
        const size_t scan = size < 4096 ? size : 4096;
        bool saw_space = false;
        for (size_t i = 0; i < scan; ++i) {
            if (data[i] == 0x00) return false;
            if (data[i] == 0x20) saw_space = true;
        }
        if (!saw_space) return false;

        // Signal 2: the header still decodes to a known magic - the magic and
        // the leading control byte survive the substitution untouched - but
        // the size behind it only becomes sane once the spaces are read back
        // as NULs. Note the magic itself ends in a genuine space ("BMD "), so
        // the header must NOT be blanket-patched; only the size field is.
        const size_t probe_in = size < 16 ? size : 16;
        std::vector<uint8_t> head = DecodeLzss(data, probe_in, 8);
        if (head.size() < 8) return false;

        const uint32_t magic = LoadBE32(head.data());
        if (!IsKnownUncompressedMagic(magic)) return false;
        if (EmbeddedSizeForMagic(magic, head.data(), head.size()) == 0) {
            return false;  // no size field to cross-check against
        }

        auto plausible = [](uint32_t s) { return s >= 16 && s <= (256u << 20); };

        const uint32_t as_read = LoadBE32(head.data() + 4);
        if (plausible(as_read)) return false;  // healthy; ProbeLzss owns this

        uint8_t unmasked[4];
        for (int i = 0; i < 4; ++i) {
            unmasked[i] = (head[4 + i] == 0x20) ? 0x00 : head[4 + i];
        }
        return plausible(LoadBE32(unmasked));
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

TcxDecompressor::Codec TcxDecompressor::DetectCodec(const uint8_t* data, size_t size) {
    if (!data || size < 8) return Codec::None;
    if (IsKnownUncompressedMagic(LoadBE32(data))) return Codec::None;

    // Bare LZSS first: this test actually decodes and validates a magic, so it
    // is specific. The Tcx test below is only a histogram heuristic, which an
    // LZSS stream passes just as readily.
    if (ProbeLzss(data, size, nullptr)) return Codec::Lzss;
    if (ProbeLzssNulEaten(data, size)) return Codec::LzssNulEaten;

    if (size < 260) return Codec::None;

    // Sanity check on the freq table: must have non-trivial sum and variety.
    uint32_t sum = 0;
    int distinct = 0;
    uint8_t seen[256] = { 0 };
    for (int i = 0; i < 256; ++i) {
        sum += data[i];
        if (!seen[data[i]]) { seen[data[i]] = 1; ++distinct; }
    }
    if (sum < 64 || distinct < 8) return Codec::None;
    return Codec::Tcx;
}

bool TcxDecompressor::IsCompressed(const uint8_t* data, size_t size) {
    return DetectCodec(data, size) != Codec::None;
}

const char* TcxDecompressor::DescribeCodec(Codec c) {
    switch (c) {
    case Codec::None:         return "uncompressed";
    case Codec::Tcx:          return "Tcx (freq table + arithmetic coder + LZSS)";
    case Codec::Lzss:         return "LZSS";
    case Codec::LzssNulEaten: return "LZSS, damaged: NUL bytes replaced by spaces";
    }
    return "unknown";
}

std::vector<uint8_t> TcxDecompressor::Decompress(const uint8_t* input,
    size_t input_size,
    size_t expected_size_hint) {
    std::vector<uint8_t> empty;

    const Codec codec = DetectCodec(input, input_size);

    // Lossy damage - decoding would desync partway and emit noise, so refuse.
    if (codec == Codec::LzssNulEaten) return empty;

    if (codec == Codec::Lzss) {
        uint32_t embedded = 0;
        ProbeLzss(input, input_size, &embedded);

        // Prefer the caller's cap, else the size the format declares. Without
        // either we would decode whatever trails the stream - op.bmd/ab.bmd
        // carry an ASCII tool marker past the last group.
        const size_t target = expected_size_hint ? expected_size_hint : embedded;
        std::vector<uint8_t> out = DecodeLzss(input, input_size, target);
        if (target && out.size() > target) out.resize(target);
        return out;
    }

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

// Header-only compression test. IsCompressed needs just 260 bytes, so reading
// the whole file to answer the question wastes an entire pass over it - which
// on the 250 MB WAVs in sound/cxs is seconds of stalled UI per click.
bool TcxDecompressor::IsFileCompressed(const std::string& path) {
    return DetectFileCodec(path) != Codec::None;
}

TcxDecompressor::Codec TcxDecompressor::DetectFileCodec(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return Codec::None;

    uint8_t head[260] = { 0 };
    f.read(reinterpret_cast<char*>(head), sizeof(head));
    const size_t got = (size_t)f.gcount();
    return DetectCodec(head, got);
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