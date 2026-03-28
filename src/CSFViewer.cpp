#include "CSFViewer.h"
#include "FileDialog.h"
#include <imgui.h>
#include <iostream>
#include <filesystem>

namespace fs = std::filesystem;

CSFViewer::CSFViewer()
    : m_HasFile(false),
    m_SelectedClipIdx(-1),
    m_ShowExportPopup(false),
    m_ExportSuccess(false) {
}

CSFViewer::~CSFViewer() {
    Clear();
}

void CSFViewer::LoadFile(const std::string& filepath) {
    Clear();
    m_HasFile = CSFParser::Load(filepath, m_CSF);
}

// Load a CSF chunk that is already in memory (e.g. clicked inside a BMD file)
void CSFViewer::LoadFromMemory(const uint8_t* data, size_t size, const std::string& label) {
    Clear();
    m_HasFile = CSFParser::LoadFromMemory(data, size, m_CSF);
    if (m_HasFile) {
        m_CSF.filename = label;
    }
}

void CSFViewer::Clear() {
    m_CSF = CSFFile();
    m_HasFile = false;
    m_SelectedClipIdx = -1;
}

void CSFViewer::Render() {
    if (!m_HasFile) {
        ImGui::TextDisabled("No CSF file loaded");
        ImGui::TextWrapped("Load a .csf audio file from the file browser.");
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "ATRAC3 export ready");
        ImGui::TextWrapped("Exports valid .at3 files with RIFF/WAVE container.");
        ImGui::TextWrapped("Convert with: ffmpeg -i file.at3 file.wav");
        return;
    }

    RenderInfo();
    ImGui::Separator();
    RenderClipList();
    ImGui::Separator();
    RenderExportOptions();

    // Export result popup
    if (m_ShowExportPopup) {
        ImGui::OpenPopup("Export Result");
        m_ShowExportPopup = false;
    }

    if (ImGui::BeginPopupModal("Export Result", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (m_ExportSuccess) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Export Successful");
            ImGui::Separator();

            if (!m_ExportMessage.empty()) {
                ImGui::TextWrapped("%s", m_ExportMessage.c_str());
                ImGui::Separator();
            }

            ImGui::TextWrapped("Saved to:");
            ImGui::TextWrapped("%s", m_LastExportPath.c_str());
            ImGui::Separator();
            ImGui::TextWrapped("Files are valid ATRAC3 WAV.");
            ImGui::TextWrapped("Convert with: ffmpeg -i file.at3 file.wav");
        }
        else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Export Failed");
            ImGui::Separator();
            ImGui::TextWrapped("%s", m_ExportMessage.c_str());
        }

        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

// Display CSF file information
void CSFViewer::RenderInfo() {
    ImGui::Text("CSF Audio File");
    ImGui::Separator();

    fs::path filePath(m_CSF.filename);
    std::string filename = filePath.filename().string();
    if (filename.empty()) filename = m_CSF.filename;

    ImGui::Text("File: %s", filename.c_str());
    ImGui::Text("Format: CSF (ATRAC3)");

    ImGui::Separator();
    ImGui::Text("Total Size: %u bytes", m_CSF.file_size);
    ImGui::Text("Audio Start: 0x%X", m_CSF.audio_start);
    ImGui::Text("Audio Size: %u bytes", m_CSF.audio_size);

    ImGui::Separator();
    ImGui::Text("Clips: %u", m_CSF.clip_count);
}

// Display table of audio clips
void CSFViewer::RenderClipList() {
    if (m_CSF.clips.empty()) {
        ImGui::TextDisabled("No clips found");
        return;
    }

    ImGui::Text("Audio Clips:");

    if (ImGui::BeginTable("ClipList", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY, ImVec2(0, 200))) {
        ImGui::TableSetupColumn("Clip", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Sample Rate", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableHeadersRow();

        for (size_t i = 0; i < m_CSF.clips.size(); i++) {
            const CSFClip& clip = m_CSF.clips[i];

            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);

            bool selected = (m_SelectedClipIdx == (int)i);
            if (ImGui::Selectable(("##clip" + std::to_string(i)).c_str(),
                selected,
                ImGuiSelectableFlags_SpanAllColumns)) {
                m_SelectedClipIdx = (int)i;
            }

            ImGui::SameLine();
            ImGui::Text("#%d", (int)i);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("0x%X", clip.start_offset);

            ImGui::TableSetColumnIndex(2);
            if (clip.size_bytes < 1024) {
                ImGui::Text("%u B", clip.size_bytes);
            }
            else {
                ImGui::Text("%.1f KB", clip.size_bytes / 1024.0f);
            }

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u Hz", clip.sample_rate);
        }

        ImGui::EndTable();
    }
}

// Export buttons and info
void CSFViewer::RenderExportOptions() {
    ImGui::Text("Export Options:");

    if (ImGui::Button("Export All Clips as .at3", ImVec2(-1, 0))) {
        ExportAllClips();
    }

    ImGui::BeginDisabled(m_SelectedClipIdx < 0);
    if (ImGui::Button("Export Selected Clip as .at3", ImVec2(-1, 0))) {
        ExportSingleClip(m_SelectedClipIdx);
    }
    ImGui::EndDisabled();

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
    ImGui::TextWrapped("Exports valid ATRAC3 files (48kHz mono, 0xC0 block).");
    ImGui::TextWrapped("Files can be converted with ffmpeg or played in VLC.");
    ImGui::PopStyleColor();
}

void CSFViewer::ExportSingleClip(int clipIdx) {
    if (clipIdx < 0 || clipIdx >= (int)m_CSF.clips.size()) {
        m_ExportSuccess = false;
        m_ExportMessage = "Invalid clip selection";
        m_ShowExportPopup = true;
        return;
    }

    std::string at3Path = FileDialog::SaveFile("ATRAC3 Audio\0*.at3\0All Files\0*.*\0");

    if (at3Path.empty()) {
        return;
    }

    if (at3Path.find(".at3") == std::string::npos) {
        at3Path += ".at3";
    }

    bool success = CSFParser::ExportClipAT3(m_CSF, clipIdx, at3Path);

    m_ExportSuccess = success;
    m_LastExportPath = at3Path;
    m_ExportMessage = success ? "Clip exported successfully" : "Export failed";
    m_ShowExportPopup = true;
}

void CSFViewer::ExportAllClips() {
    if (m_CSF.clips.empty()) {
        m_ExportSuccess = false;
        m_ExportMessage = "No clips to export";
        m_ShowExportPopup = true;
        return;
    }

    std::string at3Path = FileDialog::SaveFile("ATRAC3 Audio\0*.at3\0All Files\0*.*\0");

    if (at3Path.empty()) {
        return;
    }

    bool success = CSFParser::ExportAllClipsAT3(m_CSF, at3Path);

    size_t ext_pos = at3Path.find_last_of('.');
    std::string base_path = at3Path;
    if (ext_pos != std::string::npos) {
        base_path = at3Path.substr(0, ext_pos);
    }

    m_ExportSuccess = success;
    m_LastExportPath = base_path + "_*.at3";
    m_ExportMessage = success ? "All clips exported successfully" : "Export failed";
    m_ShowExportPopup = true;
}