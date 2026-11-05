#include "BEWavParser.h"
#include <fstream>
#include <iostream>
#include <vector>
#include <cstring>

namespace {

    uint32_t BE32(const uint8_t* p) {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
            (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }
    uint16_t BE16(const uint8_t* p) {
        return uint16_t((uint32_t(p[0]) << 8) | uint32_t(p[1]));
    }
    uint32_t LE32(const uint8_t* p) {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
            (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }

    void PutLE32(std::vector<uint8_t>& b, uint32_t v) {
        b.push_back(uint8_t(v & 0xFF));
        b.push_back(uint8_t((v >> 8) & 0xFF));
        b.push_back(uint8_t((v >> 16) & 0xFF));
        b.push_back(uint8_t((v >> 24) & 0xFF));
    }
    void PutLE16(std::vector<uint8_t>& b, uint16_t v) {
        b.push_back(uint8_t(v & 0xFF));
        b.push_back(uint8_t((v >> 8) & 0xFF));
    }

}  // namespace

bool BEWavParser::IsBEWav(const uint8_t* data, size_t size) {
    if (size < 44) return false;
    if (memcmp(data, "RIFF", 4) != 0 || memcmp(data + 8, "WAVE", 4) != 0)
        return false;

    // Both variants share the same 12-byte magic, so the endianness has to be
    // decided from a field whose value we know. The fmt chunk size is the
    // cleanest one: it is 16 (or 18/40) in every real file, and a big-endian
    // 16 read little-endian comes out as 0x10000000. So whichever way reads
    // as a legal size tells us how the file is stored.
    if (memcmp(data + 12, "fmt ", 4) != 0) return false;

    const uint32_t fmt_be = BE32(data + 16);
    const uint32_t fmt_le = LE32(data + 16);

    auto legal = [](uint32_t v) { return v == 16 || v == 18 || v == 40; };

    if (legal(fmt_le)) return false;   // ordinary little-endian WAV
    return legal(fmt_be);
}

bool BEWavParser::Load(const std::string& filepath, BEWavInfo& out) {
    std::ifstream f(filepath, std::ios::binary);
    if (!f.is_open()) {
        std::cerr << "Failed to open: " << filepath << std::endl;
        return false;
    }

    f.seekg(0, std::ios::end);
    const std::streamoff total = f.tellg();
    if (total < 44) return false;
    f.seekg(0, std::ios::beg);

    uint8_t head[64] = { 0 };
    f.read(reinterpret_cast<char*>(head), sizeof(head));

    if (!IsBEWav(head, sizeof(head))) {
        std::cerr << "Not a big-endian WAV: " << filepath << std::endl;
        return false;
    }

    out.filename = filepath;
    out.file_size = uint64_t(total);
    out.audio_format = BE16(head + 20);
    out.channels = BE16(head + 22);
    out.sample_rate = BE32(head + 24);
    out.byte_rate = BE32(head + 28);
    out.block_align = BE16(head + 32);
    out.bits = BE16(head + 34);

    if (out.channels == 0 || out.channels > 8 ||
        out.sample_rate == 0 || out.sample_rate > 192000) {
        std::cerr << "BE WAV header out of range" << std::endl;
        return false;
    }
    if (out.bits != 16) {
        // Only 16-bit is byte-swappable the simple way, and it's all the
        // retail set uses. Anything else would need real format handling.
        std::cerr << "BE WAV is " << out.bits << "-bit; only 16-bit is supported"
            << std::endl;
        return false;
    }

    // Walk to the data chunk rather than assuming it sits at 36.
    uint64_t pos = 12;
    bool found = false;
    while (pos + 8 <= uint64_t(total)) {
        uint8_t ch[8];
        f.seekg((std::streamoff)pos, std::ios::beg);
        f.read(reinterpret_cast<char*>(ch), 8);
        if (!f) break;

        const uint32_t sz = BE32(ch + 4);
        if (memcmp(ch, "data", 4) == 0) {
            out.data_offset = pos + 8;
            // Trailing padding past the chunk is common; clamp so we never
            // read off the end.
            out.data_size = (out.data_offset + sz <= uint64_t(total))
                ? sz : (uint64_t(total) - out.data_offset);
            found = true;
            break;
        }
        if (sz == 0) break;
        pos += 8 + sz + (sz & 1);
    }

    if (!found || out.data_size == 0) {
        std::cerr << "BE WAV has no usable data chunk" << std::endl;
        return false;
    }

    out.valid = true;
    std::cout << "\n=== BE WAV ===\n" << filepath << "\n"
        << out.sample_rate << " Hz, " << out.channels << "ch, "
        << out.bits << "-bit, " << out.DurationSeconds() << "s, "
        << out.data_size << " bytes PCM\n==============\n" << std::endl;
    return true;
}

bool BEWavParser::ConvertToLE(const BEWavInfo& info,
    const std::string& output_path,
    std::atomic<float>* progress,
    std::atomic<bool>* cancel) {
    if (!info.valid) return false;

    std::ifstream in(info.filename, std::ios::binary);
    if (!in.is_open()) {
        std::cerr << "Failed to reopen: " << info.filename << std::endl;
        return false;
    }
    std::ofstream out(output_path, std::ios::binary | std::ios::trunc);
    if (!out.is_open()) {
        std::cerr << "Failed to create: " << output_path << std::endl;
        return false;
    }

    // Canonical 44-byte little-endian header. The source may carry extra
    // chunks, but none of them survive a format conversion meaningfully, and
    // a minimal header is what every player handles best.
    const uint32_t data_size = (uint32_t)info.data_size;
    std::vector<uint8_t> hdr;
    hdr.insert(hdr.end(), { 'R','I','F','F' });
    PutLE32(hdr, 36 + data_size);
    hdr.insert(hdr.end(), { 'W','A','V','E' });
    hdr.insert(hdr.end(), { 'f','m','t',' ' });
    PutLE32(hdr, 16);
    PutLE16(hdr, 1);                    // PCM
    PutLE16(hdr, info.channels);
    PutLE32(hdr, info.sample_rate);
    PutLE32(hdr, info.sample_rate * info.channels * (info.bits / 8));
    PutLE16(hdr, uint16_t(info.channels * (info.bits / 8)));
    PutLE16(hdr, info.bits);
    hdr.insert(hdr.end(), { 'd','a','t','a' });
    PutLE32(hdr, data_size);
    out.write(reinterpret_cast<const char*>(hdr.data()), (std::streamsize)hdr.size());

    // Stream the PCM through a 16-bit byte swap in 1 MiB windows.
    constexpr size_t CHUNK = 1 << 20;
    std::vector<uint8_t> buf(CHUNK);
    in.seekg((std::streamoff)info.data_offset, std::ios::beg);

    uint64_t remaining = info.data_size;
    while (remaining > 0) {
        if (cancel && cancel->load()) {
            out.close();
            std::remove(output_path.c_str());
            return false;
        }

        const size_t want = (size_t)((remaining < CHUNK) ? remaining : CHUNK);
        in.read(reinterpret_cast<char*>(buf.data()), (std::streamsize)want);
        const size_t got = (size_t)in.gcount();
        if (got == 0) break;

        // Swap whole 16-bit samples only. A window ending on an odd byte
        // would split a sample across iterations; CHUNK is even and the data
        // size is a multiple of block_align, so this can only trip on a
        // truncated file - in which case the odd trailing byte is dropped.
        const size_t pairs = got / 2;
        for (size_t i = 0; i < pairs; i++)
            std::swap(buf[i * 2], buf[i * 2 + 1]);

        out.write(reinterpret_cast<const char*>(buf.data()),
            (std::streamsize)(pairs * 2));
        remaining -= got;

        if (progress && info.data_size)
            progress->store(float(double(info.data_size - remaining) /
                double(info.data_size)));
    }

    out.close();
    if (progress) progress->store(1.0f);
    std::cout << "Converted to LE WAV: " << output_path << std::endl;
    return true;
}
