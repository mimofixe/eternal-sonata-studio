#pragma once
#include "CSFParser.h"
#include "CXSParser.h"
#include "BEWavParser.h"
#include <string>
#include <vector>
#include <thread>
#include <mutex>
#include <atomic>
#include <cstdint>

// Preview playback for the audio formats the studio understands.
//
// Two constraints shape this. First, neither codec a CSF or CXS holds has a
// usable in-process decoder - XMA2 is a WMA-Pro variant, and XAudio2 on
// Windows does not decode XMA (it's Xbox only) - so decoding shells out to
// ffmpeg when it's on PATH. Second, everything here has to stay off the UI
// thread: decoding a full BGM track takes seconds, and the big-endian WAVs in
// sound/cxs run to 250 MB.
//
// So all work happens on a worker thread and the UI polls State(). Playback
// itself streams through waveOut rather than PlaySound, because PlaySound can
// only loop a whole file - it cannot loop the region between a CXS track's
// loop points - and because streaming avoids holding a 250 MB file in memory.
enum class AudioState {
    Idle,
    Preparing,   // extracting / decoding / converting on the worker
    Playing,
    Failed
};

// Where the player pulls PCM from. Always a file on disk, streamed: for CSF
// and CXS that's the temp WAV ffmpeg produced, for a big-endian WAV it's the
// source file itself, byte-swapped on the fly so no conversion pass is needed
// just to hear it.
struct PcmSource {
    std::string path;
    uint64_t data_offset = 0;
    uint64_t data_size = 0;

    uint32_t sample_rate = 0;
    uint16_t channels = 0;
    uint16_t bits = 16;

    bool byteswap = false;      // source stores 16-bit samples big-endian
    bool temp = false;          // delete path once playback finishes

    // Loop region in frames. loop_end == 0 means play once.
    uint64_t loop_start = 0;
    uint64_t loop_end = 0;

    uint32_t FrameSize() const { return uint32_t(channels) * (bits / 8); }
    uint64_t Frames() const {
        const uint32_t f = FrameSize();
        return f ? data_size / f : 0;
    }
};

class AudioPlayer {
public:
    AudioPlayer() = default;
    ~AudioPlayer();

    AudioPlayer(const AudioPlayer&) = delete;
    AudioPlayer& operator=(const AudioPlayer&) = delete;

    // True if ffmpeg was found on PATH. Cached after the first lookup.
    // Only CSF/CXS need it; big-endian WAVs play without it.
    static bool IsAvailable();
    static const std::string& FFmpegPath();

    // All three return immediately, having handed the work to the worker.
    // Any previous playback is stopped first.
    void PlayClip(const CSFFile& csf, uint32_t clip_index);
    void PlayCXS(const CXSFile& cxs);
    void PlayBEWav(const BEWavInfo& wav);

    // Stops playback and cancels any in-flight preparation. Safe to call at
    // any time; blocks only as long as it takes the worker to notice.
    void Stop();

    AudioState  State() const { return m_State.load(); }
    bool        IsPlaying() const { return m_State.load() == AudioState::Playing; }
    bool        IsBusy() const { return m_State.load() == AudioState::Preparing; }
    std::string Error() const;

    // Seconds. Position is the stream cursor, so it reflects loop wrap-around.
    double DurationSeconds() const;
    double PositionSeconds() const;

    bool IsLooping() const { return m_Looping.load(); }
    int  CurrentClip() const { return m_CurrentClip.load(); }

    // What is loaded, for the UI to label the transport.
    std::string TrackLabel() const;

private:
    std::thread             m_Worker;
    std::atomic<AudioState> m_State{ AudioState::Idle };
    std::atomic<bool>       m_Cancel{ false };
    std::atomic<bool>       m_Looping{ false };
    std::atomic<int>        m_CurrentClip{ -1 };
    std::atomic<uint64_t>   m_PositionFrames{ 0 };
    std::atomic<uint64_t>   m_TotalFrames{ 0 };
    std::atomic<uint32_t>   m_SampleRate{ 0 };

    mutable std::mutex      m_Mutex;
    std::string             m_Error;
    std::string             m_Label;

    // Handle of the running ffmpeg, so Stop() can kill it instead of waiting
    // out a long decode. void* to keep <windows.h> out of this header.
    std::atomic<void*>      m_FFmpegProc{ nullptr };

    void StopWorker();
    void SetError(const std::string& msg);
    void SetLabel(const std::string& label);

    // Worker entry points.
    void RunPrepared(PcmSource src);
    void StreamOut(const PcmSource& src);

    // Runs ffmpeg src -> wav_out. Returns false on failure or cancellation.
    bool DecodeWithFFmpeg(const std::string& src_path,
        const std::string& wav_out);
};
