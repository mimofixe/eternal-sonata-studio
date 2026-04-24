#pragma once
#include "CSFParser.h"
#include <string>
#include <vector>
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

    // sub-renders
    void RenderInfo();
    void RenderClipList();
    void RenderExportOptions();

    // export helpers
    void ExportSingleClip(int clipIdx);
    void ExportAllClips();          // all clips from the currently-loaded CSF
    void ExportAllCSFsInFile();     // all clips from every CSF in the file
};