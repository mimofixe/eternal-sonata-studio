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

void CSFViewer::LoadCXSFile(const std::string& filepath) {
    Clear();
    m_HasCXS = CXSParser::Load(filepath, m_CXS);
}

void CSFViewer::LoadWavFile(const std::string& filepath) {
    Clear();
    m_HasWav = BEWavParser::Load(filepath, m_Wav);
}

void CSFViewer::JoinConvert() {
    m_ConvertCancel.store(true);
    if (m_ConvertThread.joinable()) m_ConvertThread.join();
    m_ConvertCancel.store(false);
    m_ConvertRunning.store(false);
}

void CSFViewer::LoadFromMemory(const uint8_t* data, size_t size, const std::string& label) {
    // Don't call Clear() here: we want to keep the file context set by
    // SetFileContext so the "Export All CSFs in File" button keeps working
    // after the user clicks through individual chunks.
    m_Player.Stop();
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
    m_Player.Stop();
    JoinConvert();
    m_Wav = BEWavInfo();
    m_HasWav = false;
    m_CXS = CXSFile();
    m_HasCXS = false;
    m_CSF = CSFFile();
    m_HasFile = false;
    m_SelectedClipIdx = -1;
    m_FileData = nullptr;
    m_FileChunks = nullptr;
}

// Render

void CSFViewer::Render() {
    if (m_HasWav) {
        RenderWav();
        return;
    }

    if (m_HasCXS) {
        RenderCXS();
        return;
    }

    if (!m_HasFile) {
        ImGui::TextDisabled("No CSF file loaded");
        ImGui::TextWrapped("Load a .csf audio file from the file browser, "
            "or click a CSF chunk in the Chunks panel.");
        ImGui::Separator();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Audio export ready");
        ImGui::TextWrapped("PS3 containers export as .at3 (ATRAC3), Xbox 360 "
            "containers as .xma (XMA2); the codec is detected per file.");
        ImGui::TextWrapped("Convert with: ffmpeg -i file.at3 file.wav");
        return;
    }

    RenderInfo();
    ImGui::Separator();
    RenderClipList();
    ImGui::Separator();
    RenderTransport();
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
            ImGui::TextWrapped("Format: %s", CSFCodecName(m_CSF.codec));
            ImGui::TextWrapped("Convert with: ffmpeg -i file%s file.wav",
                CSFCodecExtension(m_CSF.codec));
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
            ImGui::Text("Exported %d / %d %s file(s).", m_BatchExported,
                m_BatchTotal, CSFCodecExtension(m_CSF.codec));
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
    ImGui::Text("Format: CSF / %s", CSFCodecName(m_CSF.codec));
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
                ImGuiSelectableFlags_SpanAllColumns |
                ImGuiSelectableFlags_AllowDoubleClick)) {
                m_SelectedClipIdx = (int)i;
                if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) &&
                    AudioPlayer::IsAvailable()) {
                    m_SelectedClipIdx = (int)i;
                    StartClipPlayback();
                }
            }

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

void CSFViewer::RenderCXS() {
    fs::path filePath(m_CXS.filename);
    std::string filename = filePath.filename().string();
    if (filename.empty()) filename = m_CXS.filename;

    ImGui::Text("CXS Streaming BGM");
    ImGui::Separator();
    ImGui::Text("File: %s", filename.c_str());
    ImGui::Text("Format: CXS / XMA2 (Xbox 360)");
    ImGui::Separator();
    ImGui::Text("%u Hz, %u channel%s", m_CXS.sample_rate, m_CXS.channels,
        m_CXS.channels == 1 ? "" : "s");

    const double dur = m_CXS.DurationSeconds();
    ImGui::Text("Duration: %d:%05.2f  (%u samples)",
        int(dur) / 60, dur - 60.0 * (int(dur) / 60), m_CXS.total_samples);
    ImGui::Text("Stream: %u bytes, %u packets", m_CXS.data_size,
        m_CXS.data_size / 2048);

    if (m_CXS.HasLoop()) {
        const double ls = double(m_CXS.loop_start) / m_CXS.sample_rate;
        const double le = double(m_CXS.loop_end) / m_CXS.sample_rate;
        ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
            "Loop: %.2fs -> %.2fs", ls, le);
    }
    else {
        ImGui::TextDisabled("Loop: none");
    }

    ImGui::Separator();
    ImGui::Text("Preview:");

    if (!AudioPlayer::IsAvailable()) {
        ImGui::BeginDisabled();
        ImGui::Button("Play", ImVec2(-1, 0));
        ImGui::EndDisabled();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.7f, 0.3f, 1.0f));
        ImGui::TextWrapped("Playback needs ffmpeg on PATH. Export still works "
            "without it.");
        ImGui::PopStyleColor();
    }
    else {
        RenderPlayButton("Play", true, &CSFViewer::StartCXSPlayback);
        if (m_CXS.HasLoop() && m_Player.IsPlaying())
            ImGui::TextDisabled("Looping between the header's loop points.");
    }

    ImGui::Separator();
    ImGui::Text("Export Options:");
    if (ImGui::Button("Export as .xma", ImVec2(-1, 0))) {
        std::string path =
            FileDialog::SaveFile("XMA2 Audio\0*.xma\0All Files\0*.*\0");
        if (!path.empty()) {
            if (path.find(".xma") == std::string::npos) path += ".xma";
            m_ExportSuccess = CXSParser::ExportXMA(m_CXS, path);
            m_LastExportPath = path;
            m_ExportMessage = m_ExportSuccess ? "Track exported successfully"
                : "Export failed";
            m_ShowExportPopup = true;
        }
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
    ImGui::TextWrapped("Rewraps the XMA2 stream without transcoding, carrying "
        "the exact sample count and loop points from the CXS header. "
        "Decode with: ffmpeg -i track.xma track.wav");
    ImGui::PopStyleColor();

    if (m_ShowExportPopup) {
        ImGui::OpenPopup("Export Result");
        m_ShowExportPopup = false;
    }
    if (ImGui::BeginPopupModal("Export Result", NULL,
        ImGuiWindowFlags_AlwaysAutoResize)) {
        if (m_ExportSuccess) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Export Successful");
            ImGui::Separator();
            ImGui::TextWrapped("Saved to:");
            ImGui::TextWrapped("%s", m_LastExportPath.c_str());
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
}

void CSFViewer::RenderTransport() {
    ImGui::Text("Preview:");

    if (!AudioPlayer::IsAvailable()) {
        ImGui::BeginDisabled();
        ImGui::Button("Play Selected Clip", ImVec2(-1, 0));
        ImGui::EndDisabled();
        ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.9f, 0.7f, 0.3f, 1.0f));
        ImGui::TextWrapped("Playback needs ffmpeg on PATH - neither ATRAC3 nor "
            "XMA2 can be decoded in-process. Export still works without it.");
        ImGui::PopStyleColor();
        return;
    }

    RenderPlayButton("Play Selected Clip", m_SelectedClipIdx >= 0,
        &CSFViewer::StartClipPlayback);

    if (m_SelectedClipIdx < 0)
        ImGui::TextDisabled("Select a clip above, or double-click one to play.");
}

// Shared transport: one button whose meaning follows the player's state, plus
// a status line. Every path here is non-blocking - the player does its work on
// a worker thread and this only reads atomics.
void CSFViewer::RenderPlayButton(const char* label, bool enabled,
    void (CSFViewer::* start)()) {
    const AudioState state = m_Player.State();
    const bool busy = (state == AudioState::Preparing);
    const bool playing = (state == AudioState::Playing);

    ImGui::BeginDisabled(!enabled && !playing && !busy);
    const char* text = busy ? "Cancel" : (playing ? "Stop" : label);
    if (ImGui::Button(text, ImVec2(-1, 0))) {
        if (busy || playing) m_Player.Stop();
        else (this->*start)();
    }
    ImGui::EndDisabled();

    if (busy) {
        ImGui::Text("Decoding %s...", m_Player.TrackLabel().c_str());
    }
    else if (playing) {
        const double pos = m_Player.PositionSeconds();
        const double dur = m_Player.DurationSeconds();
        ImGui::Text("Playing %s  %d:%04.1f / %d:%04.1f%s",
            m_Player.TrackLabel().c_str(),
            int(pos) / 60, pos - 60.0 * (int(pos) / 60),
            int(dur) / 60, dur - 60.0 * (int(dur) / 60),
            m_Player.IsLooping() ? "  (looping)" : "");
        if (dur > 0.0)
            ImGui::ProgressBar((float)(pos / dur), ImVec2(-1, 4), "");
    }

    if (state == AudioState::Failed) {
        const std::string err = m_Player.Error();
        if (!err.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.4f, 0.4f, 1.0f));
            ImGui::TextWrapped("%s", err.c_str());
            ImGui::PopStyleColor();
        }
    }
}

void CSFViewer::StartClipPlayback() {
    if (m_SelectedClipIdx >= 0)
        m_Player.PlayClip(m_CSF, (uint32_t)m_SelectedClipIdx);
}

void CSFViewer::StartCXSPlayback() { m_Player.PlayCXS(m_CXS); }

void CSFViewer::StartWavPlayback() { m_Player.PlayBEWav(m_Wav); }

// Big-endian PCM WAV panel. These are the largest assets in the game (up to
// 250 MB), so nothing here reads the file whole: playback streams and swaps
// on the fly, and conversion runs on its own thread with a progress bar.
void CSFViewer::RenderWav() {
    fs::path filePath(m_Wav.filename);
    std::string filename = filePath.filename().string();
    if (filename.empty()) filename = m_Wav.filename;

    ImGui::Text("WAV (Xbox 360, big-endian PCM)");
    ImGui::Separator();
    ImGui::Text("File: %s", filename.c_str());
    ImGui::Text("%u Hz, %u channel%s, %u-bit", m_Wav.sample_rate,
        m_Wav.channels, m_Wav.channels == 1 ? "" : "s", m_Wav.bits);

    const double dur = m_Wav.DurationSeconds();
    ImGui::Text("Duration: %d:%05.2f", int(dur) / 60,
        dur - 60.0 * (int(dur) / 60));
    ImGui::Text("PCM: %.1f MB at offset 0x%llX",
        m_Wav.data_size / (1024.0 * 1024.0),
        (unsigned long long)m_Wav.data_offset);

    ImGui::Separator();
    ImGui::Text("Preview:");
    // No ffmpeg needed: the samples are already PCM, just the wrong byte
    // order, so the player swaps them as it streams.
    RenderPlayButton("Play", true, &CSFViewer::StartWavPlayback);
    ImGui::TextDisabled("Streamed and byte-swapped on the fly - the file is "
        "never loaded whole.");

    ImGui::Separator();
    ImGui::Text("Export Options:");

    const bool converting = m_ConvertRunning.load();
    ImGui::BeginDisabled(converting);
    if (ImGui::Button("Convert to little-endian .wav", ImVec2(-1, 0))) {
        std::string path =
            FileDialog::SaveFile("WAV Audio\0*.wav\0All Files\0*.*\0");
        if (!path.empty()) {
            if (path.size() < 4 ||
                path.compare(path.size() - 4, 4, ".wav") != 0)
                path += ".wav";

            JoinConvert();
            m_ConvertPath = path;
            m_ConvertProgress.store(0.0f);
            m_ConvertRunning.store(true);
            m_ConvertOk.store(false);

            BEWavInfo job = m_Wav;
            m_ConvertThread = std::thread([this, job, path]() {
                const bool ok = BEWavParser::ConvertToLE(
                    job, path, &m_ConvertProgress, &m_ConvertCancel);
                m_ConvertOk.store(ok);
                m_ConvertRunning.store(false);
                });
        }
    }
    ImGui::EndDisabled();

    if (converting) {
        ImGui::ProgressBar(m_ConvertProgress.load(), ImVec2(-1, 0));
        if (ImGui::Button("Cancel Conversion", ImVec2(-1, 0)))
            m_ConvertCancel.store(true);
    }
    else if (!m_ConvertPath.empty()) {
        if (m_ConvertOk.load()) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Converted:");
            ImGui::TextWrapped("%s", m_ConvertPath.c_str());
        }
        else {
            ImGui::TextColored(ImVec4(1.0f, 0.6f, 0.4f, 1.0f),
                "Conversion cancelled or failed.");
        }
    }

    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
    ImGui::TextWrapped("Byte-swaps the 16-bit samples and writes a standard "
        "little-endian WAV - the same job as scripts/convert_wav.py, streamed "
        "so file size doesn't matter.");
    ImGui::PopStyleColor();
}

void CSFViewer::RenderExportOptions() {
    ImGui::Text("Export Options:");

    const char* ext = CSFCodecExtension(m_CSF.codec);
    char btn[64];

    // current CSF
    snprintf(btn, sizeof(btn), "Export All Clips as %s", ext);
    if (ImGui::Button(btn, ImVec2(-1, 0)))
        ExportAllClips();

    ImGui::BeginDisabled(m_SelectedClipIdx < 0);
    snprintf(btn, sizeof(btn), "Export Selected Clip as %s", ext);
    if (ImGui::Button(btn, ImVec2(-1, 0)))
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

            char label[96];
            snprintf(label, sizeof(label),
                "Export All CSFs in File  (%d CSFs, ~%d clips, %s)",
                csf_count, total_clips, ext);

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
            ImGui::TextWrapped("Exports every clip from all %d CSF chunks "
                "in the currently loaded file.", csf_count);
            ImGui::PopStyleColor();
        }
    }

    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.7f, 0.7f, 0.7f, 1.0f));
    if (m_CSF.codec == CSFCodec::XMA2)
        ImGui::TextWrapped("Exports XMA2 in a RIFF container (mono, 2048-byte "
            "packets). Decode with: ffmpeg -i clip.xma clip.wav");
    else
        ImGui::TextWrapped("Exports valid ATRAC3 files (48kHz mono, 0xC0 block). "
            "Decode with: ffmpeg -i clip.at3 clip.wav");
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

    const char* ext = CSFCodecExtension(m_CSF.codec);

    std::string path = FileDialog::SaveFile(m_CSF.codec == CSFCodec::XMA2
        ? "XMA2 Audio\0*.xma\0All Files\0*.*\0"
        : "ATRAC3 Audio\0*.at3\0All Files\0*.*\0");
    if (path.empty()) return;
    if (path.find(ext) == std::string::npos)
        path += ext;

    bool ok = CSFParser::ExportClip(m_CSF, clipIdx, path);
    m_ExportSuccess = ok;
    m_LastExportPath = path;
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

    std::string path = FileDialog::SaveFile(m_CSF.codec == CSFCodec::XMA2
        ? "XMA2 Audio\0*.xma\0All Files\0*.*\0"
        : "ATRAC3 Audio\0*.at3\0All Files\0*.*\0");
    if (path.empty()) return;

    bool ok = CSFParser::ExportAllClips(m_CSF, path);

    size_t dot = path.find_last_of('.');
    std::string base = (dot != std::string::npos) ? path.substr(0, dot) : path;

    m_ExportSuccess = ok;
    m_LastExportPath = base + "_*" + CSFCodecExtension(m_CSF.codec);
    m_ExportMessage = ok ? "All clips exported successfully" : "Export failed";
    m_ShowExportPopup = true;
}

void CSFViewer::ExportAllCSFsInFile() {
    if (!m_FileChunks || !m_FileData) return;

    // Ask user for a base name; files will be named <base>_csf000_000.<ext>
    std::string base = FileDialog::SaveFile(m_CSF.codec == CSFCodec::XMA2
        ? "XMA2 Audio\0*.xma\0All Files\0*.*\0"
        : "ATRAC3 Audio\0*.at3\0All Files\0*.*\0");
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
            // Each chunk carries its own detected codec rather than inheriting
            // the selected one, so a mixed container still exports correctly.
            for (size_t ci = 0; ci < csf.clips.size(); ci++) {
                char suffix[40];
                snprintf(suffix, sizeof(suffix), "_csf%03d_%03d%s",
                    csf_idx, (int)ci, CSFCodecExtension(csf.codec));
                total++;
                if (CSFParser::ExportClip(csf, (uint32_t)ci, base + suffix))
                    exported++;
            }
        }
        csf_idx++;
    }

    m_BatchExported = exported;
    m_BatchTotal = total;
    m_LastExportPath = base + "_csf*_*" + CSFCodecExtension(m_CSF.codec);
    m_ShowBatchResult = true;
}