#pragma once
#include "CSFParser.h"
#include <string>

// ImGui viewer for CSF audio files
// Displays file info, clip list, and export functionality
class CSFViewer {
public:
    CSFViewer();
    ~CSFViewer();

    void LoadFile(const std::string& filepath);
    void LoadFromMemory(const uint8_t* data, size_t size, const std::string& label);
    void Clear();
    void Render();

private:
    void RenderInfo();
    void RenderClipList();
    void RenderExportOptions();

    void ExportSingleClip(int clipIdx);
    void ExportAllClips();

    CSFFile m_CSF;
    bool m_HasFile;
    int m_SelectedClipIdx;

    bool m_ShowExportPopup;
    bool m_ExportSuccess;
    std::string m_LastExportPath;
    std::string m_ExportMessage;
};