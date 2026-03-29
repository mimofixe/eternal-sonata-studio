#pragma once
#include "AIScriptParser.h"
#include <string>

class AIScriptViewer {
public:
    AIScriptViewer();
    void Load(const std::string& filepath);
    void Clear();
    void Render();
    bool HasScript() const { return m_Script.valid; }

private:
    void RenderOverview();
    void RenderDisassembly();
    void RenderActionTable();
    void RenderApiCatalog();
    void RenderControlFlow();
    void RenderStrings();

    AIScriptFile m_Script;
    int  m_SelectedNode = 0;
    int  m_ActiveTab = 0;
    char m_NodeSearch[32] = {};
};