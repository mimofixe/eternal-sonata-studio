#pragma once
#include "BOPParser.h"
#include <string>

// ImGui viewer for BOP files
// Displays file info, section list, and export functionality
class BOPViewer {
public:
    BOPViewer();
    ~BOPViewer();

    void LoadFile(const std::string& filepath);
    void Clear();
    void Render();

private:
    void RenderFileInfo();
    void RenderSectionList();
    void RenderExportOptions();

    void ExportSingleSection(int sectionIdx);
    void ExportAllSections();

    BOPFile m_BOP;
    bool m_HasFile;
    int m_SelectedSectionIdx;

    bool m_ShowExportPopup;
    bool m_ExportSuccess;
    std::string m_LastExportPath;
    std::string m_ExportMessage;
};