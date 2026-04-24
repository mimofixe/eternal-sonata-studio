#include "CSFViewer.h"
#include "EFileParser.h"   // for Chunk / ChunkType
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

void CSFViewer::LoadFromMemory(const uint8_t* data, size_t size, const std::string& label) {
    // Don't call Clear() here: we want to keep the file context set by
    // SetFileContext so the "Export All CSFs in File" button keeps working
    // after the user clicks through individual chunks.
    m_CSF = CSFFile();
    m_SelectedClipIdx = -1;
    m_HasFile = CSFParser::LoadFromMemory(data, size, m_CSF);
    if (m_HasFile) {
        m_CSF.filename = label;
    }
}

void CSFViewer::SetFileContext(const std::vector<uint8_t>& file_data,
    const std::vector<Chunk>& chunks) {
    m_FileData = &file_data;
    m_FileChunks = &chunks;
}

void CSFViewer::Clear() {
    m_CSF = CSFFile();
    m_HasFile = false;
    m_SelectedClipIdx = -1;
    m_FileData = nullptr;
    m_FileChunks = nullptr;
}

// Render

void CSFViewer::Render() {
    if (!m_HasFile) {
        ImGui::TextDisabled("No CSF file loaded");
        ImGui::TextWrapped("Load a .csf audio file from the file browser, "
            "or click a CSF chunk in the Chunks panel.");
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

    // single-clip / all-clips result popup
    if (m_ShowExportPopup) {
        ImGui::OpenPopup("Export Result");
        m_ShowExportPopup = false;
    }
    if (ImGui::BeginPopupModal("Export Result", NULL,
        ImGuiWindowFlags_AlwaysAutoResize)) {
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
        if (ImGui::Button("OK", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    // batch (all-CSFs-in-file) result popup
    if (m_ShowBatchResult) {
        ImGui::OpenPopup("Batch Export Result");
        m_ShowBatchResult = false;
    }
    if (ImGui::BeginPopupModal("Batch Export Result", NULL,
        ImGuiWindowFlags_AlwaysAutoResize)) {
        if (m_BatchExported > 0) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Export Successful");
            ImGui::Separator();
            ImGui::Text("Exported %d / %d .at3 file(s).", m_BatchExported, m_BatchTotal);
            ImGui::Separator();
            ImGui::TextWrapped("Saved to: %s", m_LastExportPath.c_str());
        }
        else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Export Failed");
            ImGui::Separator();
            ImGui::TextWrapped("No clips could be exported.");
        }
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

// Sub-panels

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

    // Show how many CSF chunks exist in the file when context is available
    if (m_FileChunks) {
        int csf_count = 0;
        for (const auto& c : *m_FileChunks)
            if (c.type == ChunkType::CSF) csf_count++;
        if (csf_count > 1) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                "  (%d CSFs in file)", csf_count);
        }
    }
}

void CSFViewer::RenderClipList() {
    if (m_CSF.clips.empty()) {
        ImGui::TextDisabled("No clips found");
        return;
    }

    ImGui::Text("Audio Clips:");

    if (ImGui::BeginTable("ClipList", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY,
        ImVec2(0, 200))) {
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
                ImGuiSelectableFlags_SpanAllColumns))
                m_SelectedClipIdx = (int)i;

            ImGui::SameLine();
            ImGui::Text("#%d", (int)i);

            ImGui::TableSetColumnIndex(1);
            ImGui::Text("0x%X", clip.start_offset);

            ImGui::TableSetColumnIndex(2);
            if (clip.size_bytes < 1024)
                ImGui::Text("%u B", clip.size_bytes);
            else
                ImGui::Text("%.1f KB", clip.size_bytes / 1024.0f);

            ImGui::TableSetColumnIndex(3);
            ImGui::Text("%u Hz", clip.sample_rate);
        }

        ImGui::EndTable();
    }
}

void CSFViewer::RenderExportOptions() {
    ImGui::Text("Export Options:");

    // current CSF
    if (ImGui::Button("Export All Clips as .at3", ImVec2(-1, 0)))
        ExportAllClips();

    ImGui::BeginDisabled(m_SelectedClipIdx < 0);
    if (ImGui::Button("Export Selected Clip as .at3", ImVec2(-1, 0)))
        ExportSingleClip(m_SelectedClipIdx);
    ImGui::EndDisabled();

    // whole-file batch
    if (m_FileChunks && m_FileData) {
        // Count how many CSF chunks are in the file
        int csf_count = 0;
        int total_clips = 0;
        for (const auto& c : *m_FileChunks) {
            if (c.type == ChunkType::CSF) {
                csf_count++;
                // Quick peek at clip count (BOOK section at +0x10, count at +0x18)
                if (c.offset + 0x1C <= m_FileData->size()) {
                    const uint8_t* p = m_FileData->data() + c.offset;
                    uint32_t cnt = (p[0x18] << 24) | (p[0x19] << 16) |
                        (p[0x1A] << 8) | p[0x1B];
                    if (cnt < 4096) total_clips += (int)cnt;
                }
            }
        }

        if (csf_count > 1) {
            ImGui::Separator();

            char label[80];
            snprintf(label, sizeof(label),
                "Export All CSFs in File  (%d CSFs, ~%d clips)",
                csf_count, total_clips);

            ImGui::PushStyleColor(ImGuiCol_Button,
                ImVec4(0.20f, 0.45f, 0.20f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered,
                ImVec4(0.28f, 0.60f, 0.28f, 1.0f));
            ImGui::PushStyleColor(ImGuiCol_ButtonActive,
                ImVec4(0.15f, 0.35f, 0.15f, 1.0f));
            if (ImGui::Button(label, ImVec2(-1, 0)))
                ExportAllCSFsInFile();
            ImGui::PopStyleColor(3);

            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.6f, 0.8f, 1.0f, 1.0f));
            ImGui::TextWrapped("Exports every AT3 clip from all %d CSF chunks "
                "in the currently loaded file.", csf_count);
            ImGui::PopStyleColor();
        }
    }

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
    ImGui::TextWrapped("Exports valid ATRAC3 files (48kHz mono, 0xC0 block).");
    ImGui::TextWrapped("Files can be converted with ffmpeg or played in VLC.");
    ImGui::PopStyleColor();
}

// Export helpers

void CSFViewer::ExportSingleClip(int clipIdx) {
    if (clipIdx < 0 || clipIdx >= (int)m_CSF.clips.size()) {
        m_ExportSuccess = false;
        m_ExportMessage = "Invalid clip selection";
        m_ShowExportPopup = true;
        return;
    }

    std::string at3Path =
        FileDialog::SaveFile("ATRAC3 Audio\0*.at3\0All Files\0*.*\0");
    if (at3Path.empty()) return;
    if (at3Path.find(".at3") == std::string::npos)
        at3Path += ".at3";

    bool ok = CSFParser::ExportClipAT3(m_CSF, clipIdx, at3Path);
    m_ExportSuccess = ok;
    m_LastExportPath = at3Path;
    m_ExportMessage = ok ? "Clip exported successfully" : "Export failed";
    m_ShowExportPopup = true;
}

void CSFViewer::ExportAllClips() {
    if (m_CSF.clips.empty()) {
        m_ExportSuccess = false;
        m_ExportMessage = "No clips to export";
        m_ShowExportPopup = true;
        return;
    }

    std::string at3Path =
        FileDialog::SaveFile("ATRAC3 Audio\0*.at3\0All Files\0*.*\0");
    if (at3Path.empty()) return;

    bool ok = CSFParser::ExportAllClipsAT3(m_CSF, at3Path);

    size_t ext = at3Path.find_last_of('.');
    std::string base = (ext != std::string::npos) ? at3Path.substr(0, ext) : at3Path;

    m_ExportSuccess = ok;
    m_LastExportPath = base + "_*.at3";
    m_ExportMessage = ok ? "All clips exported successfully" : "Export failed";
    m_ShowExportPopup = true;
}

void CSFViewer::ExportAllCSFsInFile() {
    if (!m_FileChunks || !m_FileData) return;

    // Ask user for a base name; files will be named <base>_csf000_000.at3
    std::string base =
        FileDialog::SaveFile("ATRAC3 Audio\0*.at3\0All Files\0*.*\0");
    if (base.empty()) return;

    size_t ext = base.find_last_of('.');
    if (ext != std::string::npos) base = base.substr(0, ext);

    int exported = 0, total = 0;
    int csf_idx = 0;

    for (const auto& c : *m_FileChunks) {
        if (c.type != ChunkType::CSF) continue;
        if (c.offset + c.size > m_FileData->size()) { csf_idx++; continue; }

        CSFFile csf;
        if (CSFParser::LoadFromMemory(
            m_FileData->data() + c.offset, c.size, csf)) {
            for (size_t ci = 0; ci < csf.clips.size(); ci++) {
                char suffix[32];
                snprintf(suffix, sizeof(suffix),
                    "_csf%03d_%03d.at3", csf_idx, (int)ci);
                total++;
                if (CSFParser::ExportClipAT3(csf, (uint32_t)ci, base + suffix))
                    exported++;
            }
        }
        csf_idx++;
    }

    m_BatchExported = exported;
    m_BatchTotal = total;
    m_LastExportPath = base + "_csf*_*.at3";
    m_ShowBatchResult = true;
}