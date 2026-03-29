#include "TutorialViewer.h"
#include "FileDialog.h"
#include <imgui.h>
#include <filesystem>
#include <cstring>
#include <algorithm>

namespace fs = std::filesystem;

TutorialViewer::TutorialViewer() {}

void TutorialViewer::Load(const std::string& filepath) {
    m_File = TutorialParser::Parse(filepath);
    m_SelectedTrack = 0;
    m_SelectedSection = 0;
    m_ExportDone = false;
    m_ExportMsg.clear();
}

void TutorialViewer::Clear() { m_File = TutorialFile(); }

void TutorialViewer::Render() {
    if (!m_File.valid) {
        ImGui::TextDisabled("No tutorial file loaded");
        ImGui::TextWrapped("Open a t0001.e, t0002.e or t0003.e file.");
        return;
    }
    if (ImGui::BeginTabBar("TutTabs")) {
        if (ImGui::BeginTabItem("Overview")) { RenderOverview(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Sections")) { RenderSections(); ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Lip Sync")) { RenderLipSync();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Export")) { RenderExport();   ImGui::EndTabItem(); }
        ImGui::EndTabBar();
    }
}

void TutorialViewer::RenderOverview() {
    fs::path p(m_File.filename);
    ImGui::Text("File:  %s", p.filename().string().c_str());
    ImGui::Separator();
    ImGui::Text("Hash:        0x%08X", m_File.hash);
    ImGui::Text("Build ID:    0x%08X", m_File.secondary_id);
    ImGui::Text("File size:   %u bytes  (%.1f MB)", m_File.file_size, m_File.file_size / 1048576.0f);
    ImGui::Separator();

    auto TrackSummary = [](const TutorialTrack& t) -> std::pair<uint32_t, int> {
        uint32_t audio = 0; int lip = 0;
        for (auto& s : t.sections) { audio += s.audio_size; lip += (int)s.lip_events.size(); }
        return { audio, lip };
        };

    auto [a_audio, a_lip] = TrackSummary(m_File.track_a);
    auto [b_audio, b_lip] = TrackSummary(m_File.track_b);

    uint32_t b0 = m_File.track_b.sections.empty() ? 0 : m_File.track_b.sections.front().sequence_id;
    uint32_t b1 = m_File.track_b.sections.empty() ? 0 : m_File.track_b.sections.back().sequence_id;

    ImGui::TextColored({ 0.4f,1.0f,0.7f,1.0f }, "Track A — English");
    ImGui::Text("  %u sections  |  seq %u–%u  |  %.0f KB  |  %d lip events",
        (uint32_t)m_File.track_a.sections.size(), m_File.FirstSequenceId(), m_File.LastSequenceId(),
        a_audio / 1024.0f, a_lip);

    ImGui::TextColored({ 0.4f,0.7f,1.0f,1.0f }, "Track B — Japanese");
    ImGui::Text("  %u sections  |  seq %u–%u  |  %.0f KB  |  %d lip events",
        (uint32_t)m_File.track_b.sections.size(), b0, b1,
        b_audio / 1024.0f, b_lip);

    ImGui::Separator();
    ImGui::TextDisabled("Both tracks cover the same tutorial sequence IDs.");
    ImGui::TextDisabled("JP has more lip events than EN (more syllables per line).");
    ImGui::TextDisabled("Audio: raw ATRAC3, 48000 Hz mono, 192 B/frame.");
}

void TutorialViewer::RenderSections() {
    ImGui::Text("Track:");
    ImGui::SameLine();
    if (ImGui::RadioButton("A — English", m_SelectedTrack == 0)) m_SelectedTrack = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("B — Japanese", m_SelectedTrack == 1)) m_SelectedTrack = 1;
    ImGui::Separator();

    const auto& track = (m_SelectedTrack == 0) ? m_File.track_a : m_File.track_b;
    if (track.sections.empty()) { ImGui::TextDisabled("No sections."); return; }

    ImGui::Text("%u sections", (uint32_t)track.sections.size());

    if (ImGui::BeginTable("SecTbl", 6,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 35);
        ImGui::TableSetupColumn("Seq ID", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Audio (KB)", ImGuiTableColumnFlags_WidthFixed, 85);
        ImGui::TableSetupColumn("Hdr (bytes)", ImGuiTableColumnFlags_WidthFixed, 85);
        ImGui::TableSetupColumn("Lip events", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int)track.sections.size(); i++) {
            const auto& s = track.sections[i];
            ImGui::TableNextRow();

            bool sel = (m_SelectedSection == i);
            if (sel) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                ImGui::GetColorU32(ImVec4{ 0.15f,0.30f,0.20f,1.0f }));

            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(std::to_string(i).c_str(), sel,
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                m_SelectedSection = i;

            ImGui::TableSetColumnIndex(1); ImGui::Text("%u", s.sequence_id);
            ImGui::TableSetColumnIndex(2); ImGui::Text("%.1f", s.audio_size / 1024.0f);
            ImGui::TableSetColumnIndex(3); ImGui::Text("%u", s.header_size);

            ImGui::TableSetColumnIndex(4);
            int n = (int)s.lip_events.size();
            ImVec4 lc = n >= 80 ? ImVec4{ 1.0f,0.5f,0.5f,1.0f } :
                n >= 40 ? ImVec4{ 1.0f,0.85f,0.4f,1.0f } :
                ImVec4{ 0.75f,0.75f,0.75f,1.0f };
            ImGui::TextColored(lc, "%d", n);

            ImGui::TableSetColumnIndex(5); ImGui::TextDisabled("0x%06X", s.abs_offset);
        }
        ImGui::EndTable();
    }
}

void TutorialViewer::RenderLipSync() {
    const auto& track = (m_SelectedTrack == 0) ? m_File.track_a : m_File.track_b;

    ImGui::Text("Track:");
    ImGui::SameLine();
    if (ImGui::RadioButton("A##ls", m_SelectedTrack == 0)) m_SelectedTrack = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("B##ls", m_SelectedTrack == 1)) m_SelectedTrack = 1;
    ImGui::SameLine();
    ImGui::Spacing(); ImGui::SameLine();
    ImGui::Text("Section:"); ImGui::SameLine();
    ImGui::SetNextItemWidth(80);
    if (ImGui::InputInt("##sec", &m_SelectedSection))
        m_SelectedSection = std::clamp(m_SelectedSection, 0, (int)track.sections.size() - 1);

    if (m_SelectedSection < 0 || m_SelectedSection >= (int)track.sections.size())
        return;

    const auto& sec = track.sections[m_SelectedSection];
    ImGui::Separator();
    ImGui::Text("Section %d  |  Seq ID: %u  |  Audio: %.1f KB  |  %d events",
        m_SelectedSection, sec.sequence_id, sec.audio_size / 1024.0f, (int)sec.lip_events.size());
    ImGui::Separator();

    if (sec.lip_events.empty()) { ImGui::TextDisabled("No lip sync events."); return; }

    auto PhCol = [](uint8_t ph) -> ImVec4 {
        switch (ph) {
        case 0: return { 0.45f,0.45f,0.45f,1.0f };
        case 1: return { 0.30f,0.90f,0.45f,1.0f };
        case 2: return { 0.30f,0.75f,1.00f,1.0f };
        case 3: return { 1.00f,0.65f,0.20f,1.0f };
        case 4: return { 0.80f,0.40f,1.00f,1.0f };
        case 5: return { 1.00f,0.40f,0.40f,1.0f };
        default: return { 0.5f,0.5f,0.5f,1.0f };
        }
        };

    // Legend
    ImGui::TextDisabled("Phonemes:"); ImGui::SameLine();
    for (int i = 0; i < 6; i++) {
        ImGui::TextColored(PhCol(i), "  %d:%s", i, PhonemeName(i));
        ImGui::SameLine();
    }
    ImGui::NewLine();
    ImGui::Separator();

    // Timeline bar
    uint32_t total_frames = 0;
    for (auto& e : sec.lip_events) total_frames += e.duration;
    ImGui::Text("Total: %u frames  (%.2f s @ 30fps)", total_frames, total_frames / 30.0f);
    ImGui::Spacing();

    float avail_w = ImGui::GetContentRegionAvail().x;
    float bar_h = 28.0f;
    float scale = avail_w / (float)total_frames;
    ImDrawList* dl = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();

    float x = origin.x;
    for (auto& e : sec.lip_events) {
        float w = e.duration * scale;
        ImVec4 cv = PhCol(e.phoneme);
        ImU32  col = IM_COL32((int)(cv.x * 255), (int)(cv.y * 255), (int)(cv.z * 255), 200);
        dl->AddRectFilled({ x, origin.y }, { x + w, origin.y + bar_h }, col);
        dl->AddRect({ x, origin.y }, { x + w, origin.y + bar_h }, IM_COL32(0, 0, 0, 80));
        if (w > 22) {
            char lbl[4]; snprintf(lbl, sizeof(lbl), "%d", e.phoneme);
            dl->AddText({ x + 3, origin.y + 7 }, IM_COL32(255, 255, 255, 220), lbl);
        }
        x += w;
    }
    ImGui::Dummy({ avail_w, bar_h + 4 });
    ImGui::Separator();

    if (ImGui::BeginTable("LipTbl", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 40);
        ImGui::TableSetupColumn("Phoneme", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Dur (frm)", ImGuiTableColumnFlags_WidthFixed, 75);
        ImGui::TableSetupColumn("Time (ms)", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        uint32_t t_ms = 0;
        for (int i = 0; i < (int)sec.lip_events.size(); i++) {
            auto& e = sec.lip_events[i];
            uint32_t dur_ms = (e.duration * 1000) / 30;
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextDisabled("%d", i);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored(PhCol(e.phoneme), "%d  %s", e.phoneme, PhonemeName(e.phoneme));
            ImGui::TableSetColumnIndex(2); ImGui::Text("%d", e.duration);
            ImGui::TableSetColumnIndex(3); ImGui::TextDisabled("%u ms  (t=%u ms)", dur_ms, t_ms);
            t_ms += dur_ms;
        }
        ImGui::EndTable();
    }

    if (m_SelectedSection < (int)m_File.track_b.sections.size()) {
        ImGui::Separator();
        const auto& sa = m_File.track_a.sections[m_SelectedSection];
        const auto& sb = m_File.track_b.sections[m_SelectedSection];
        ImGui::TextColored({ 0.7f,0.9f,0.7f,1.0f }, "A vs B — same section:");
        ImGui::Text("  EN: %d events, %.1f KB", (int)sa.lip_events.size(), sa.audio_size / 1024.0f);
        ImGui::Text("  JP: %d events, %.1f KB", (int)sb.lip_events.size(), sb.audio_size / 1024.0f);
    }
}

void TutorialViewer::RenderExport() {
    ImGui::TextColored({ 0.8f,0.9f,0.6f,1.0f }, "Export audio (.at3)");
    ImGui::TextDisabled("One .at3 per section, named by sequence ID.");
    ImGui::Separator();

    ImGui::Text("Track:");
    ImGui::SameLine();
    if (ImGui::RadioButton("A — English##e", m_SelectedTrack == 0)) m_SelectedTrack = 0;
    ImGui::SameLine();
    if (ImGui::RadioButton("B — Japanese##e", m_SelectedTrack == 1)) m_SelectedTrack = 1;
    ImGui::SameLine();
    if (ImGui::RadioButton("Both##e", m_SelectedTrack == 2)) m_SelectedTrack = 2;
    ImGui::Separator();

    if (m_SelectedTrack == 0 || m_SelectedTrack == 2)
        ImGui::Text("Track A: %u sections → tut_a_XXXXX.at3", (uint32_t)m_File.track_a.sections.size());
    if (m_SelectedTrack == 1 || m_SelectedTrack == 2)
        ImGui::Text("Track B: %u sections → tut_b_XXXXX.at3", (uint32_t)m_File.track_b.sections.size());

    ImGui::Spacing();

    if (ImGui::Button("Export all sections", { 200, 30 })) {
        std::string folder = FileDialog::OpenFolder();
        if (!folder.empty()) {
            m_ExportCount = 0;
            if (m_SelectedTrack == 0 || m_SelectedTrack == 2)
                m_ExportCount += TutorialParser::ExportTrackAT3(m_File.track_a, folder, "tut_a");
            if (m_SelectedTrack == 1 || m_SelectedTrack == 2)
                m_ExportCount += TutorialParser::ExportTrackAT3(m_File.track_b, folder, "tut_b");
            m_ExportDone = true;
            char buf[256];
            snprintf(buf, sizeof(buf), "Exported %d files to: %s", m_ExportCount, folder.c_str());
            m_ExportMsg = buf;
        }
    }

    ImGui::SameLine();

    if (ImGui::Button("Export selected section", { 220, 30 })) {
        const auto& trk = (m_SelectedTrack == 1) ? m_File.track_b : m_File.track_a;
        if (m_SelectedSection < (int)trk.sections.size()) {
            const auto& sec = trk.sections[m_SelectedSection];
            char filter[64];
            snprintf(filter, sizeof(filter), "tut_%s_%05u.at3",
                (m_SelectedTrack == 1) ? "b" : "a", sec.sequence_id);
            std::string path = FileDialog::SaveFile("ATRAC3 Audio\0*.at3\0All Files\0*.*\0");
            if (!path.empty()) {
                if (path.find(".at3") == std::string::npos) path += ".at3";
                bool ok = TutorialParser::ExportSectionAT3(sec, path);
                m_ExportCount = ok ? 1 : 0;
                m_ExportDone = true;
                m_ExportMsg = ok ? ("Exported: " + path) : "Export failed.";
            }
        }
    }

    if (m_ExportDone && !m_ExportMsg.empty()) {
        ImGui::Spacing();
        ImGui::TextColored(m_ExportCount > 0 ? ImVec4{ 0.4f,1.0f,0.5f,1.0f }
            : ImVec4{ 1.0f,0.4f,0.4f,1.0f },
            "%s", m_ExportMsg.c_str());
    }

    ImGui::Separator();
    ImGui::TextDisabled("Select a row to target it with 'Export selected section'.");
    ImGui::Spacing();

    const auto& vt = (m_SelectedTrack == 1) ? m_File.track_b : m_File.track_a;
    const char* vp = (m_SelectedTrack == 1) ? "tut_b" : "tut_a";

    if (ImGui::BeginTable("ExpSec", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("#", ImGuiTableColumnFlags_WidthFixed, 35);
        ImGui::TableSetupColumn("Seq ID", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 75);
        ImGui::TableSetupColumn("Filename", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (int i = 0; i < (int)vt.sections.size(); i++) {
            const auto& s = vt.sections[i];
            char fname[64];
            snprintf(fname, sizeof(fname), "%s_%05u.at3", vp, s.sequence_id);

            ImGui::TableNextRow();
            bool sel = (m_SelectedSection == i);
            if (sel) ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                ImGui::GetColorU32(ImVec4{ 0.15f,0.30f,0.20f,1.0f }));

            ImGui::TableSetColumnIndex(0);
            if (ImGui::Selectable(std::to_string(i).c_str(), sel,
                ImGuiSelectableFlags_SpanAllColumns | ImGuiSelectableFlags_AllowOverlap))
                m_SelectedSection = i;

            ImGui::TableSetColumnIndex(1); ImGui::Text("%u", s.sequence_id);
            ImGui::TableSetColumnIndex(2); ImGui::TextDisabled("%.1f KB", s.audio_size / 1024.0f);
            ImGui::TableSetColumnIndex(3); ImGui::TextDisabled("%s", fname);
        }
        ImGui::EndTable();
    }
}