#include "XMAWriter.h"
#include <fstream>
#include <iostream>

void XMAWriter::WriteU32LE(std::vector<uint8_t>& buf, uint32_t value) {
    buf.push_back(value & 0xFF);
    buf.push_back((value >> 8) & 0xFF);
    buf.push_back((value >> 16) & 0xFF);
    buf.push_back((value >> 24) & 0xFF);
}

void XMAWriter::WriteU16LE(std::vector<uint8_t>& buf, uint16_t value) {
    buf.push_back(value & 0xFF);
    buf.push_back((value >> 8) & 0xFF);
}

bool XMAWriter::WriteXMAFile(const std::vector<uint8_t>& xma2_payload,
    uint32_t sample_rate,
    uint16_t channels,
    const std::string& output_path) {
    XMAStreamInfo info;
    info.sample_rate = sample_rate;
    info.channels = channels;
    return WriteXMAFile(xma2_payload, info, output_path);
}

bool XMAWriter::WriteXMAFile(const std::vector<uint8_t>& xma2_payload,
    const XMAStreamInfo& info,
    const std::string& output_path) {

    const uint32_t sample_rate = info.sample_rate;
    const uint16_t channels = info.channels;

    if (xma2_payload.empty() || channels == 0 || sample_rate == 0) {
        std::cerr << "XMAWriter: refusing to write an empty/degenerate stream"
            << std::endl;
        return false;
    }

    // A partial trailing packet would make the decoder read past the end of
    // the last frame, so drop it rather than emit a stream that fails midway.
    std::vector<uint8_t> data = xma2_payload;
    if (data.size() % PACKET_SIZE != 0)
        data.resize(data.size() - (data.size() % PACKET_SIZE));

    if (data.empty()) {
        std::cerr << "XMAWriter: payload is smaller than one 2048-byte packet"
            << std::endl;
        return false;
    }

    const uint32_t block_count =
        (uint32_t)((data.size() + BYTES_PER_BLOCK - 1) / BYTES_PER_BLOCK);

    // When the container states the sample count (CXS does), trust it. The
    // CSF TIM descriptor doesn't, so derive one from the stream: each packet
    // header is a big-endian dword whose top 6 bits count the frames starting
    // in that packet, and every XMA2 frame decodes to 512 samples.
    //
    // This must not under-shoot - decoders clamp playback to PlayLength, so a
    // low value silently truncates the clip - but over-shooting is harmless
    // because they stop at the real end of the stream anyway. Summing frame
    // counts gives a tight upper bound: on campdata/scp.bmd's first clip it
    // yields 450560 against 449774 actually decoded, the difference being the
    // final partial frame. (A flat guess of 16 frames per packet would have
    // cut that clip off at 78% - real packets here carry up to 62.)
    const uint32_t packets = (uint32_t)(data.size() / PACKET_SIZE);

    uint32_t frames = 0;
    for (size_t off = 0; off + 4 <= data.size(); off += PACKET_SIZE) {
        const uint32_t hdr = (uint32_t(data[off]) << 24) |
            (uint32_t(data[off + 1]) << 16) |
            (uint32_t(data[off + 2]) << 8) |
            uint32_t(data[off + 3]);
        frames += hdr >> 26;
    }

    const uint32_t samples_encoded =
        info.samples_encoded ? info.samples_encoded : frames * 512;

    // nAvgBytesPerSec should describe the *compressed* rate. Tools estimate
    // duration from it when they don't decode the whole stream, so a nominal
    // PCM-sized value makes ffprobe report a length several times short. With
    // a real sample count available we can state it properly.
    const uint32_t avg_bytes_per_sec = samples_encoded
        ? uint32_t((double(data.size()) * sample_rate) / double(samples_encoded) + 0.5)
        : sample_rate * channels * 2;

    // XMA2WAVEFORMATEX = 18-byte WAVEFORMATEX + 34 bytes of XMA2 fields.
    std::vector<uint8_t> fmt;
    WriteU16LE(fmt, 0x0166);                       // WAVE_FORMAT_XMA2
    WriteU16LE(fmt, channels);
    WriteU32LE(fmt, sample_rate);
    WriteU32LE(fmt, avg_bytes_per_sec);
    WriteU16LE(fmt, 2);                            // nBlockAlign
    WriteU16LE(fmt, 16);                           // wBitsPerSample
    WriteU16LE(fmt, 34);                           // cbSize

    WriteU16LE(fmt, 1);                            // NumStreams
    WriteU32LE(fmt, channels == 1 ? 0 : 3);        // ChannelMask (mono / L+R)
    WriteU32LE(fmt, samples_encoded);              // SamplesEncoded
    WriteU32LE(fmt, BYTES_PER_BLOCK);              // BytesPerBlock
    WriteU32LE(fmt, 0);                            // PlayBegin
    WriteU32LE(fmt, samples_encoded);              // PlayLength
    const bool has_loop = info.loop_end > info.loop_start;
    WriteU32LE(fmt, has_loop ? info.loop_start : 0);                  // LoopBegin
    WriteU32LE(fmt, has_loop ? info.loop_end - info.loop_start : 0);  // LoopLength
    fmt.push_back(has_loop ? 0xFF : 0);            // LoopCount (0xFF = forever)
    fmt.push_back(4);                              // EncoderVersion (XMA2)
    WriteU16LE(fmt, (uint16_t)block_count);        // BlockCount

    std::vector<uint8_t> out;
    out.push_back('R'); out.push_back('I');
    out.push_back('F'); out.push_back('F');
    const uint32_t riff_size =
        4 + (8 + (uint32_t)fmt.size()) + (8 + (uint32_t)data.size());
    WriteU32LE(out, riff_size);
    out.push_back('W'); out.push_back('A');
    out.push_back('V'); out.push_back('E');

    out.push_back('f'); out.push_back('m');
    out.push_back('t'); out.push_back(' ');
    WriteU32LE(out, (uint32_t)fmt.size());
    out.insert(out.end(), fmt.begin(), fmt.end());

    out.push_back('d'); out.push_back('a');
    out.push_back('t'); out.push_back('a');
    WriteU32LE(out, (uint32_t)data.size());
    out.insert(out.end(), data.begin(), data.end());

    std::ofstream file(output_path, std::ios::binary);
    if (!file.is_open()) {
        std::cerr << "Failed to create: " << output_path << std::endl;
        return false;
    }

    file.write(reinterpret_cast<const char*>(out.data()), out.size());
    file.close();

    std::cout << "Created XMA2: " << output_path << " ("
        << packets << " packets, " << sample_rate << " Hz, "
        << channels << "ch)" << std::endl;

    return true;
}
