#pragma once
#include "TutorialParser.h"
#include <string>

class TutorialViewer {
public:
    TutorialViewer();
    void Load(const std::string& filepath);
    void Clear();
    void Render();
    bool HasFile() const { return m_File.valid; }

private:
    void RenderOverview();
    void RenderSections();
    void RenderLipSync();
    void RenderExport();

    TutorialFile m_File;
    int  m_SelectedTrack = 0;
    int  m_SelectedSection = 0;
    bool m_ExportDone = false;
    int  m_ExportCount = 0;
    std::string m_ExportMsg;
};