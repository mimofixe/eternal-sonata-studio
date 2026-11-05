#include "AudioPlayer.h"

#include <windows.h>
#include <mmsystem.h>
#include <objbase.h>

#include <fstream>
#include <iostream>
#include <cstring>
#include <algorithm>

namespace {

    // Locate ffmpeg once. SearchPath honours PATH and the usual app-path
    // registry entries, which is how a normal install will be found.
    std::string FindFFmpeg() {
        char buf[MAX_PATH] = { 0 };
        DWORD n = SearchPathA(nullptr, "ffmpeg.exe", nullptr, MAX_PATH, buf, nullptr);
        if (n == 0 || n >= MAX_PATH) return std::string();
        return std::string(buf);
    }

    std::string TempFilePath(const char* suffix) {
        char dir[MAX_PATH] = { 0 };
        if (GetTempPathA(MAX_PATH, dir) == 0) return std::string();

        // The counter keeps a new preview from colliding with a temp file the
        // previous one hasn't finished releasing yet.
        static std::atomic<uint32_t> seq{ 0 };
        char name[MAX_PATH];
        snprintf(name, sizeof(name), "%ses_studio_preview_%lu_%u%s",
            dir, GetCurrentProcessId(), seq.fetch_add(1), suffix);
        return std::string(name);
    }

    // waveOut is backed by WASAPI on modern Windows, and opening a device from
    // a thread with no COM apartment can fail outright. The UI thread has COM
    // initialized (the shell folder picker does it), which is why playback
    // could look fine after something else had warmed the stack up - so the
    // worker has to initialize it for itself rather than depend on that.
    struct ComScope {
        bool owned = false;
        ComScope() {
            const HRESULT hr = CoInitializeEx(nullptr, COINIT_MULTITHREADED);
            // S_FALSE means someone already initialized this thread; we then
            // still owe a matching CoUninitialize. RPC_E_CHANGED_MODE means a
            // different apartment model is in force, which we must not undo.
            owned = (hr == S_OK || hr == S_FALSE);
        }
        ~ComScope() { if (owned) CoUninitialize(); }
    };

    uint32_t LE32(const uint8_t* p) {
        return uint32_t(p[0]) | (uint32_t(p[1]) << 8) |
            (uint32_t(p[2]) << 16) | (uint32_t(p[3]) << 24);
    }
    uint16_t LE16(const uint8_t* p) {
        return uint16_t(uint32_t(p[0]) | (uint32_t(p[1]) << 8));
    }

    // Read the format and locate the PCM of a little-endian WAV, without
    // loading it. Walks the chunk list because ffmpeg can emit LIST/INFO.
    bool ProbeLEWav(const std::string& path, PcmSource& out) {
        std::ifstream f(path, std::ios::binary);
        if (!f.is_open()) return false;

        f.seekg(0, std::ios::end);
        const std::streamoff total = f.tellg();
        if (total < 44) return false;

        uint8_t head[12];
        f.seekg(0, std::ios::beg);
        f.read(reinterpret_cast<char*>(head), 12);
        if (memcmp(head, "RIFF", 4) != 0 || memcmp(head + 8, "WAVE", 4) != 0)
            return false;

        bool have_fmt = false, have_data = false;
        uint64_t pos = 12;
        while (pos + 8 <= uint64_t(total)) {
            uint8_t ch[8];
            f.seekg((std::streamoff)pos, std::ios::beg);
            f.read(reinterpret_cast<char*>(ch), 8);
            if (!f) break;
            const uint32_t sz = LE32(ch + 4);

            if (memcmp(ch, "fmt ", 4) == 0 && sz >= 16) {
                uint8_t fmt[16];
                f.read(reinterpret_cast<char*>(fmt), 16);
                out.channels = LE16(fmt + 2);
                out.sample_rate = LE32(fmt + 4);
                out.bits = LE16(fmt + 14);
                have_fmt = true;
            }
            else if (memcmp(ch, "data", 4) == 0) {
                out.data_offset = pos + 8;
                out.data_size = (pos + 8 + sz <= uint64_t(total))
                    ? sz : (uint64_t(total) - (pos + 8));
                have_data = true;
                break;
            }

            if (sz == 0) break;
            pos += 8 + sz + (sz & 1);
        }

        out.path = path;
        out.byteswap = false;
        return have_fmt && have_data && out.channels && out.sample_rate &&
            out.bits == 16;
    }

}  // namespace

const std::string& AudioPlayer::FFmpegPath() {
    static const std::string path = FindFFmpeg();
    return path;
}

bool AudioPlayer::IsAvailable() {
    return !FFmpegPath().empty();
}

AudioPlayer::~AudioPlayer() {
    StopWorker();
}

void AudioPlayer::SetError(const std::string& msg) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Error = msg;
}

void AudioPlayer::SetLabel(const std::string& label) {
    std::lock_guard<std::mutex> lock(m_Mutex);
    m_Label = label;
}

std::string AudioPlayer::Error() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Error;
}

std::string AudioPlayer::TrackLabel() const {
    std::lock_guard<std::mutex> lock(m_Mutex);
    return m_Label;
}

double AudioPlayer::DurationSeconds() const {
    const uint32_t sr = m_SampleRate.load();
    return sr ? double(m_TotalFrames.load()) / double(sr) : 0.0;
}

double AudioPlayer::PositionSeconds() const {
    const uint32_t sr = m_SampleRate.load();
    return sr ? double(m_PositionFrames.load()) / double(sr) : 0.0;
}

void AudioPlayer::StopWorker() {
    m_Cancel.store(true);

    // Kill a running ffmpeg rather than waiting out its decode - otherwise
    // Stop() would block the UI for as long as the decode had left to run.
    if (void* h = m_FFmpegProc.exchange(nullptr))
        TerminateProcess((HANDLE)h, 1);

    if (m_Worker.joinable())
        m_Worker.join();

    m_Cancel.store(false);
}

void AudioPlayer::Stop() {
    StopWorker();
    m_State.store(AudioState::Idle);
    m_CurrentClip.store(-1);
    m_PositionFrames.store(0);
    m_TotalFrames.store(0);
    SetLabel("");
}

bool AudioPlayer::DecodeWithFFmpeg(const std::string& src_path,
    const std::string& wav_out) {
    // -bitexact suppresses the LIST/ISFT tag; combined with pcm_s16le it
    // keeps the output to a plain fmt+data WAV, which is what the streamer
    // below expects to find.
    const std::string cmd = "\"" + FFmpegPath() + "\" -y -v error -i \"" +
        src_path + "\" -acodec pcm_s16le -bitexact \"" + wav_out + "\"";

    STARTUPINFOA si;
    PROCESS_INFORMATION pi;
    ZeroMemory(&si, sizeof(si));
    si.cb = sizeof(si);
    ZeroMemory(&pi, sizeof(pi));

    std::vector<char> mutable_cmd(cmd.begin(), cmd.end());
    mutable_cmd.push_back('\0');

    if (!CreateProcessA(nullptr, mutable_cmd.data(), nullptr, nullptr, FALSE,
        CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        SetError("Could not launch ffmpeg");
        return false;
    }

    CloseHandle(pi.hThread);
    m_FFmpegProc.store(pi.hProcess);

    const DWORD wait = WaitForSingleObject(pi.hProcess, 120000);
    DWORD exit_code = 1;
    if (wait == WAIT_TIMEOUT) TerminateProcess(pi.hProcess, 1);
    else GetExitCodeProcess(pi.hProcess, &exit_code);

    // Only clear the slot if it's still ours; Stop() may have taken it.
    void* expected = pi.hProcess;
    m_FFmpegProc.compare_exchange_strong(expected, nullptr);
    CloseHandle(pi.hProcess);

    if (m_Cancel.load()) return false;
    if (wait == WAIT_TIMEOUT) {
        SetError("ffmpeg timed out decoding this track");
        return false;
    }
    if (exit_code != 0) {
        SetError("ffmpeg failed to decode this track");
        return false;
    }
    return true;
}

// Streams src through waveOut until it ends or playback is cancelled.
void AudioPlayer::StreamOut(const PcmSource& src) {
    ComScope com;   // must outlive every waveOut call below

    const uint32_t frame_size = src.FrameSize();
    if (!frame_size || !src.sample_rate) {
        SetError("Decoded audio has an unusable format");
        m_State.store(AudioState::Failed);
        return;
    }

    std::ifstream in(src.path, std::ios::binary);
    if (!in.is_open()) {
        SetError("Could not open the decoded audio");
        m_State.store(AudioState::Failed);
        return;
    }

    WAVEFORMATEX wf;
    ZeroMemory(&wf, sizeof(wf));
    wf.wFormatTag = WAVE_FORMAT_PCM;
    wf.nChannels = src.channels;
    wf.nSamplesPerSec = src.sample_rate;
    wf.wBitsPerSample = src.bits;
    wf.nBlockAlign = (WORD)frame_size;
    wf.nAvgBytesPerSec = src.sample_rate * frame_size;

    HANDLE ev = CreateEventA(nullptr, FALSE, FALSE, nullptr);
    HWAVEOUT hwo = nullptr;

    // Open can fail transiently while the endpoint is still settling (a device
    // just switched, or this is the first open in the process), so give it a
    // few tries before reporting failure, and fall back from the mapper to
    // device 0 in case the mapper specifically is unhappy.
    MMRESULT mr = MMSYSERR_ERROR;
    for (int attempt = 0; attempt < 5 && !m_Cancel.load(); attempt++) {
        const UINT dev = (attempt < 3) ? WAVE_MAPPER : 0;
        mr = waveOutOpen(&hwo, dev, &wf, (DWORD_PTR)ev, 0, CALLBACK_EVENT);
        if (mr == MMSYSERR_NOERROR) break;
        hwo = nullptr;
        Sleep(60);
    }

    if (mr != MMSYSERR_NOERROR || !hwo) {
        CloseHandle(ev);
        char msg[192];
        snprintf(msg, sizeof(msg),
            "Could not open an audio output device (waveOutOpen error %u; "
            "%u Hz, %uch, %u-bit)",
            (unsigned)mr, src.sample_rate, src.channels, src.bits);
        SetError(msg);
        m_State.store(AudioState::Failed);
        return;
    }

    // Four ~125 ms buffers: enough to ride out scheduling hiccups without
    // making Stop() feel laggy, since a stopped stream still drains what is
    // already queued.
    constexpr int NBUF = 4;
    const size_t buf_frames = std::max<size_t>(1, src.sample_rate / 8);
    const size_t buf_bytes = buf_frames * frame_size;

    std::vector<std::vector<uint8_t>> mem(NBUF, std::vector<uint8_t>(buf_bytes));
    std::vector<WAVEHDR> hdr(NBUF);
    ZeroMemory(hdr.data(), sizeof(WAVEHDR) * NBUF);
    for (int i = 0; i < NBUF; i++) {
        hdr[i].lpData = (LPSTR)mem[i].data();
        hdr[i].dwBufferLength = (DWORD)buf_bytes;
        waveOutPrepareHeader(hwo, &hdr[i], sizeof(WAVEHDR));
        hdr[i].dwFlags |= WHDR_DONE;   // mark free so the fill loop claims it
    }

    const bool loop = src.loop_end > src.loop_start;
    const uint64_t loop_start_b = src.loop_start * frame_size;
    const uint64_t loop_end_b = src.loop_end * frame_size;
    const uint64_t end_b = loop ? std::min<uint64_t>(loop_end_b, src.data_size)
        : src.data_size;

    m_TotalFrames.store(src.Frames());
    m_SampleRate.store(src.sample_rate);
    m_Looping.store(loop);
    m_State.store(AudioState::Playing);

    uint64_t cursor = 0;             // byte offset within the PCM region
    int queued = 0;
    bool finished = false;

    while (!m_Cancel.load()) {
        bool filled_any = false;

        for (int i = 0; i < NBUF && !m_Cancel.load(); i++) {
            if (!(hdr[i].dwFlags & WHDR_DONE)) continue;
            if (hdr[i].dwFlags & WHDR_INQUEUE) continue;

            if (finished) { continue; }

            size_t got = 0;
            while (got < buf_bytes) {
                if (cursor >= end_b) {
                    if (loop) {
                        cursor = loop_start_b;   // wrap inside the loop region
                    }
                    else {
                        finished = true;
                        break;
                    }
                }

                const size_t want = (size_t)std::min<uint64_t>(
                    buf_bytes - got, end_b - cursor);
                in.seekg((std::streamoff)(src.data_offset + cursor), std::ios::beg);
                in.read(reinterpret_cast<char*>(mem[i].data() + got),
                    (std::streamsize)want);
                const size_t n = (size_t)in.gcount();
                if (n == 0) {
                    // Short read: treat as end of stream rather than spinning.
                    if (loop && cursor > loop_start_b) { cursor = loop_start_b; continue; }
                    finished = true;
                    break;
                }
                got += n;
                cursor += n;
            }

            if (got == 0) continue;

            if (src.byteswap) {
                const size_t pairs = got / 2;
                uint8_t* p = mem[i].data();
                for (size_t k = 0; k < pairs; k++)
                    std::swap(p[k * 2], p[k * 2 + 1]);
            }

            // Pad a short final buffer with silence so waveOut isn't handed a
            // partial frame.
            if (got < buf_bytes)
                std::memset(mem[i].data() + got, 0, buf_bytes - got);

            hdr[i].dwBufferLength = (DWORD)buf_bytes;
            hdr[i].dwFlags &= ~WHDR_DONE;
            if (waveOutWrite(hwo, &hdr[i], sizeof(WAVEHDR)) == MMSYSERR_NOERROR) {
                queued++;
                filled_any = true;
            }
            else {
                hdr[i].dwFlags |= WHDR_DONE;
            }

            m_PositionFrames.store(cursor / frame_size);
        }

        // Everything played out and there is nothing left to queue.
        if (finished && !filled_any) {
            bool all_done = true;
            for (int i = 0; i < NBUF; i++)
                if (hdr[i].dwFlags & WHDR_INQUEUE) { all_done = false; break; }
            if (all_done) break;
        }

        WaitForSingleObject(ev, 100);
    }

    waveOutReset(hwo);
    for (int i = 0; i < NBUF; i++)
        waveOutUnprepareHeader(hwo, &hdr[i], sizeof(WAVEHDR));
    waveOutClose(hwo);
    CloseHandle(ev);
    in.close();

    if (src.temp)
        DeleteFileA(src.path.c_str());

    // Leave a Failed state alone so the UI can report it.
    if (m_State.load() == AudioState::Playing)
        m_State.store(AudioState::Idle);
    m_Looping.store(false);
}

void AudioPlayer::RunPrepared(PcmSource src) {
    if (m_Cancel.load()) {
        if (src.temp) DeleteFileA(src.path.c_str());
        m_State.store(AudioState::Idle);
        return;
    }
    StreamOut(src);
}

void AudioPlayer::PlayClip(const CSFFile& csf, uint32_t clip_index) {
    StopWorker();
    SetError("");

    if (!IsAvailable()) {
        SetError("ffmpeg was not found on PATH");
        m_State.store(AudioState::Failed);
        return;
    }
    if (!csf.valid || clip_index >= csf.clips.size()) {
        SetError("Invalid clip selection");
        m_State.store(AudioState::Failed);
        return;
    }

    m_State.store(AudioState::Preparing);
    m_CurrentClip.store((int)clip_index);
    SetLabel("clip #" + std::to_string(clip_index));

    // Copy what the worker needs; the caller's CSF may be replaced meanwhile.
    CSFFile job = csf;
    m_Worker = std::thread([this, job = std::move(job), clip_index]() {
        const std::string enc = TempFilePath(CSFCodecExtension(job.codec));
        const std::string wav = TempFilePath(".wav");

        if (enc.empty() || wav.empty()) {
            SetError("Could not resolve a temp directory");
            m_State.store(AudioState::Failed);
            return;
        }

        // Reuse the export path so preview and export agree about framing.
        if (!CSFParser::ExportClip(job, clip_index, enc)) {
            DeleteFileA(enc.c_str());
            SetError("Could not extract the clip");
            m_State.store(AudioState::Failed);
            return;
        }

        const bool ok = DecodeWithFFmpeg(enc, wav);
        DeleteFileA(enc.c_str());

        if (!ok) {
            DeleteFileA(wav.c_str());
            m_State.store(m_Cancel.load() ? AudioState::Idle : AudioState::Failed);
            return;
        }

        PcmSource src;
        if (!ProbeLEWav(wav, src)) {
            DeleteFileA(wav.c_str());
            SetError("Decoded audio was not usable 16-bit PCM");
            m_State.store(AudioState::Failed);
            return;
        }
        src.temp = true;
        RunPrepared(std::move(src));
        });
}

void AudioPlayer::PlayCXS(const CXSFile& cxs) {
    StopWorker();
    SetError("");

    if (!IsAvailable()) {
        SetError("ffmpeg was not found on PATH");
        m_State.store(AudioState::Failed);
        return;
    }
    if (!cxs.valid) {
        SetError("No CXS track loaded");
        m_State.store(AudioState::Failed);
        return;
    }

    m_State.store(AudioState::Preparing);
    m_CurrentClip.store(0);
    SetLabel("CXS track");

    CXSFile job = cxs;
    m_Worker = std::thread([this, job = std::move(job)]() {
        const std::string enc = TempFilePath(".xma");
        const std::string wav = TempFilePath(".wav");

        if (enc.empty() || wav.empty()) {
            SetError("Could not resolve a temp directory");
            m_State.store(AudioState::Failed);
            return;
        }

        if (!CXSParser::ExportXMA(job, enc)) {
            DeleteFileA(enc.c_str());
            SetError("Could not extract the track");
            m_State.store(AudioState::Failed);
            return;
        }

        const bool ok = DecodeWithFFmpeg(enc, wav);
        DeleteFileA(enc.c_str());

        if (!ok) {
            DeleteFileA(wav.c_str());
            m_State.store(m_Cancel.load() ? AudioState::Idle : AudioState::Failed);
            return;
        }

        PcmSource src;
        if (!ProbeLEWav(wav, src)) {
            DeleteFileA(wav.c_str());
            SetError("Decoded audio was not usable 16-bit PCM");
            m_State.store(AudioState::Failed);
            return;
        }
        src.temp = true;

        // Loop the region the CXS header declares, so the preview behaves the
        // way the track does in game instead of stopping at the end.
        if (job.HasLoop()) {
            src.loop_start = job.loop_start;
            src.loop_end = job.loop_end;
        }

        RunPrepared(std::move(src));
        });
}

void AudioPlayer::PlayBEWav(const BEWavInfo& wav) {
    StopWorker();
    SetError("");

    if (!wav.valid) {
        SetError("No WAV loaded");
        m_State.store(AudioState::Failed);
        return;
    }

    m_State.store(AudioState::Preparing);
    m_CurrentClip.store(0);
    SetLabel("WAV");

    BEWavInfo job = wav;
    m_Worker = std::thread([this, job]() {
        // No decode step and no temp file: the samples are PCM already, so
        // they're streamed straight off the source and byte-swapped in the
        // buffer. That's what keeps a 250 MB file from costing 250 MB of RAM.
        PcmSource src;
        src.path = job.filename;
        src.data_offset = job.data_offset;
        src.data_size = job.data_size;
        src.sample_rate = job.sample_rate;
        src.channels = job.channels;
        src.bits = job.bits;
        src.byteswap = true;
        src.temp = false;

        RunPrepared(std::move(src));
        });
}
