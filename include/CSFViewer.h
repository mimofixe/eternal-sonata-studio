#pragma once
#include "CSFParser.h"
#include "AudioPlayer.h"
#include <string>
#include <vector>
#include <thread>
#include <atomic>
#include <cstdint>

// Forward-declare Chunk so we don't pull in EFileParser.h here.
// The real definition lives in EFileParser.h / ContainerParser.h.
struct Chunk;

class CSFViewer {
public:
    CSFViewer();
    ~CSFViewer();

    // Load a standalone .csf file from disk
    void LoadFile(const std::string& filepath);

    // Load a standalone .cxs streaming BGM track. CXS is a different container
    // from CSF but decodes to the same XMA2, so it shares this panel rather
    // than getting one of its own.
    void LoadCXSFile(const std::string& filepath);

    // Load one of the big-endian PCM .wav files that sit alongside the .cxs
    // tracks. Header only - these run to 250 MB and are never read whole.
    void LoadWavFile(const std::string& filepath);

    // Load a CSF chunk that is already in memory (e.g. clicked inside a BMD/.e file)
    void LoadFromMemory(const uint8_t* data, size_t size, const std::string& label);

    // Called by Application whenever a new file is opened.
    // Stores a reference to the full file buffer and chunk list so the viewer
    // can offer "Export All CSFs in File" without the user having to click each one.
    void SetFileContext(const std::vector<uint8_t>& file_data,
        const std::vector<Chunk>& chunks);

    // Clear all state (called on new file load)
    void Clear();

    // Render the ImGui panel
    void Render();

private:
    // currently-loaded CSF
    CSFFile     m_CSF;
    bool        m_HasFile;
    int         m_SelectedClipIdx;

    // currently-loaded CXS / big-endian WAV (each exclusive with the CSF above)
    CXSFile     m_CXS;
    bool        m_HasCXS = false;
    BEWavInfo   m_Wav;
    bool        m_HasWav = false;

    // Background big-endian -> little-endian WAV conversion. These files are
    // hundreds of MB, so the conversion cannot run on the UI thread.
    std::thread        m_ConvertThread;
    std::atomic<float> m_ConvertProgress{ 0.0f };
    std::atomic<bool>  m_ConvertCancel{ false };
    std::atomic<bool>  m_ConvertRunning{ false };
    std::atomic<bool>  m_ConvertOk{ false };
    std::string        m_ConvertPath;
    void JoinConvert();

    // whole-file context (for multi-CSF export)
    const std::vector<uint8_t>* m_FileData = nullptr;  // non-owning pointer
    const std::vector<Chunk>* m_FileChunks = nullptr;  // non-owning pointer

    // export popup state
    bool        m_ShowExportPopup;
    bool        m_ExportSuccess;
    std::string m_ExportMessage;
    std::string m_LastExportPath;

    // multi-CSF batch state (shown in popup)
    bool        m_ShowBatchResult = false;
    int         m_BatchExported = 0;
    int         m_BatchTotal = 0;

    // preview playback (no-op when ffmpeg isn't installed)
    AudioPlayer m_Player;

    // sub-renders
    void RenderInfo();
    void RenderClipList();
    void RenderTransport();
    void RenderExportOptions();
    void RenderCXS();
    void RenderWav();

    // Shared Play/Stop button + status line, driven by AudioPlayer::State().
    void RenderPlayButton(const char* label, bool enabled,
        void (CSFViewer::* start)());
    void StartClipPlayback();
    void StartCXSPlayback();
    void StartWavPlayback();

    // export helpers
    void ExportSingleClip(int clipIdx);
    void ExportAllClips();          // all clips from the currently-loaded CSF
    void ExportAllCSFsInFile();     // all clips from every CSF in the file
};