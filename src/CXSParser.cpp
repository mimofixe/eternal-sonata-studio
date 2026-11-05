#include "CXSParser.h"
#include "XMAWriter.h"
#include <fstream>
#include <iostream>
#include <cstring>

namespace {
    uint32_t ReadU32BE(const uint8_t* p) {
        return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) |
            (uint32_t(p[2]) << 8) | uint32_t(p[3]);
    }
}

bool CXSParser::IsCXS(const uint8_t* data, size_t size) {
    return size >= 0x30 && memcmp(data, "CXS ", 4) == 0;
}

bool CXSParser::ParseHeader(CXSFile& out, const uint8_t* data, size_t size) {
    if (!IsCXS(data, size)) {
        std::cerr << "Not a CXS file" << std::endl;
        return false;
    }

    out.packet_size = ReadU32BE(data + 0x04);
    out.sample_rate = ReadU32BE(data + 0x08);
    out.channels = ReadU32BE(data + 0x0C);
    out.total_samples = ReadU32BE(data + 0x10);
    out.loop_start = ReadU32BE(data + 0x14);
    out.loop_end = ReadU32BE(data + 0x18);
    out.block_count = ReadU32BE(data + 0x1C);
    out.data_offset = ReadU32BE(data + 0x20);
    out.data_size = ReadU32BE(data + 0x24);

    if (out.sample_rate == 0 || out.sample_rate > 192000 ||
        out.channels == 0 || out.channels > 8) {
        std::cerr << "CXS header out of range: " << out.sample_rate << " Hz, "
            << out.channels << "ch" << std::endl;
        return false;
    }

    if (out.data_offset < 0x30 ||
        size_t(out.data_offset) + out.data_size > size) {
        std::cerr << "CXS audio section runs past the end of the file"
            << std::endl;
        return false;
    }

    // Every retail file uses 2048; a different value would mean the packet
    // walk below (and the XMA2 wrapper) is reading the stream wrong.
    if (out.packet_size != XMAWriter::PACKET_SIZE) {
        std::cerr << "CXS packet size is " << out.packet_size
            << ", expected " << XMAWriter::PACKET_SIZE << std::endl;
        return false;
    }

    out.audio_data.assign(data + out.data_offset,
        data + out.data_offset + out.data_size);
    out.valid = true;
    return true;
}

bool CXSParser::Load(const std::string& filepath, CXSFile& out) {
    std::ifstream file(filepath, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to open: " << filepath << std::endl;
        return false;
    }

    file.seekg(0, std::ios::end);
    const std::streamoff n = file.tellg();
    if (n <= 0) return false;
    file.seekg(0, std::ios::beg);

    std::vector<uint8_t> buffer((size_t)n);
    file.read(reinterpret_cast<char*>(buffer.data()), n);
    file.close();

    std::cout << "\n=== CXS Parser ===" << std::endl;
    std::cout << "File: " << filepath << std::endl;

    if (!ParseHeader(out, buffer.data(), buffer.size())) {
        out.valid = false;
        return false;
    }

    out.filename = filepath;

    std::cout << out.sample_rate << " Hz, " << out.channels << "ch, "
        << out.DurationSeconds() << "s, " << out.data_size << " bytes";
    if (out.HasLoop())
        std::cout << ", loop " << out.loop_start << ".." << out.loop_end;
    std::cout << "\n==================\n" << std::endl;

    return true;
}

bool CXSParser::LoadFromMemory(const uint8_t* data, size_t size, CXSFile& out) {
    if (!ParseHeader(out, data, size)) {
        out.valid = false;
        return false;
    }
    return true;
}

bool CXSParser::ExportXMA(const CXSFile& cxs, const std::string& output_path) {
    if (!cxs.valid || cxs.audio_data.empty()) return false;

    XMAStreamInfo info;
    info.sample_rate = cxs.sample_rate;
    info.channels = (uint16_t)cxs.channels;
    info.samples_encoded = cxs.total_samples;   // exact, straight from 0x10
    if (cxs.HasLoop()) {
        info.loop_start = cxs.loop_start;
        info.loop_end = cxs.loop_end;
    }

    return XMAWriter::WriteXMAFile(cxs.audio_data, info, output_path);
}
