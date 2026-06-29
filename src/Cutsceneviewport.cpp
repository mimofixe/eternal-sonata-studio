#include "Cutsceneviewport.h"
#include <imgui.h>
#include <cstring>
#include <cstdio>
#include <fstream>
#include <algorithm>
#include <map>
#include "FileDialog.h"
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glad/glad.h>
#include "Ntx3parser.h"

CutsceneViewport::CutsceneViewport() {}

// File stem: "C:\\maps\\agn01.e" -> "agn01"
static std::string FileStem(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    size_t dot = base.find_last_of('.');
    return (dot == std::string::npos) ? base : base.substr(0, dot);
}

void CutsceneViewport::Load(const std::string& filepath) {
    FreeSlideTextures();
    m_Script = CutsceneScriptParser::Parse(filepath);
    m_ActiveTab = 0;
    m_SlideTime = 0.0f;
    m_SlidePlaying = false;
    memset(m_Filter, 0, sizeof(m_Filter));

    // Index by file load: when a FIELD script (cfdata map) is opened in the Studio,
    // its hosted events are merged into the persistent event->map index. Opening the
    // map files one by one (or in batch via the app) grows the index; cutscenes then
    // resolve their map automatically.
    if (m_Script.valid && !m_Script.hosted_events.empty()) {
        for (uint32_t ev : m_Script.hosted_events)
            m_EventMapIndex[ev] = m_Script.filename;  // full path (stem shown in UI)
        char st[96];
        snprintf(st, sizeof(st), "%zu events indexed", m_EventMapIndex.size());
        m_IndexStatus = st;
    }
}

void CutsceneViewport::Clear() {
    FreeSlideTextures();
    m_Script = CutsceneScript();
}

static ImVec4 ArgColor(CSArgKind k) {
    switch (k) {
    case CSArgKind::Float:    return { 0.7f, 0.9f, 0.7f, 1.0f };  // green
    case CSArgKind::Offset:   return { 0.4f, 0.75f, 1.0f, 1.0f }; // blue
    case CSArgKind::ActorId:  return { 1.0f, 0.85f, 0.3f, 1.0f }; // gold
    case CSArgKind::Target:   return { 1.0f, 0.5f, 1.0f, 1.0f };  // pink
    case CSArgKind::Duration: return { 0.4f, 1.0f, 1.0f, 1.0f };  // cyan
    case CSArgKind::Imm:      return { 0.8f, 0.8f, 0.8f, 1.0f };
    default:                  return { 0.65f, 0.65f, 0.65f, 1.0f };
    }
}

void CutsceneViewport::RenderOverview() {
    const CutsceneScript& s = m_Script;
    ImGui::Text("File: %s", s.filename.c_str());
    {
        uint32_t ev = CutsceneScriptParser::EventNumberFromFilename(s.filename);
        std::string title = ev ? CutsceneScriptParser::EventTitle(ev) : "";
        if (!title.empty())
            ImGui::TextColored({ 1.0f,0.85f,0.3f,1.0f }, "e%04u \"%s\"", ev, title.c_str());
    }
    ImGui::Text("hash=0x%08X  file_size=0x%X  code_size=0x%X", s.hash, s.file_size, s.code_size);
    ImGui::Separator();
    ImGui::Text("Nodes: %zu", s.nodes.size());
    ImGui::Text("Scene commands (CALLAPI): %zu", s.all_commands.size());
    ImGui::Text("Distinct actors: %zu", s.actor_ids.size());
    ImGui::Text("Data offsets (0x07): %zu", s.data_offsets.size());

    ImGui::Spacing();
    ImGui::TextColored({ 0.5f,1.0f,0.6f,1.0f }, "Footer relocation:");
    ImGui::Text("%d pairs (%d unclassified)", s.reloc_pairs, s.reloc_unclassified);
    ImGui::Text("CALLAPI sites: %zu | CALLNODE sites: %zu | data refs: %zu",
        s.api_by_site.size(), s.node_by_site.size(), s.data_by_site.size());

    ImGui::Spacing();
    ImGui::TextColored({ 1.0f,0.6f,0.2f,1.0f }, "API usage:");
    for (const auto& kv : s.api_usage) {
        std::string nm = CutsceneScriptParser::CommandNameById(kv.first);
        ImGui::BulletText("%s (id %u): %dx", nm.c_str(), kv.first, kv.second);
    }

    ImGui::Spacing();
    if (!s.hosted_events.empty()) {
        // Field (map) script: it declares the events it hosts (API_1021).
        ImGui::TextColored({ 0.5f,1.0f,0.6f,1.0f }, "Hosted events (this is a FIELD script):");
        for (uint32_t ev : s.hosted_events)
            ImGui::BulletText("event %u (e%04u)", ev, ev);
        ImGui::TextDisabled("Indexed for cutscene map resolution (%zu events total).", m_EventMapIndex.size());
    }
    else {
        // Cutscene: resolve its map from the index built by loading cfdata map files.
        ImGui::TextColored({ 0.5f,1.0f,0.6f,1.0f }, "Map resolution:");
        uint32_t ev = CutsceneScriptParser::EventNumberFromFilename(s.filename);
        bool self_contained = false;
        for (const auto& m : s.embedded_models)
            if (m.top_level && m.size > 0x100000) { self_contained = true; break; }
        if (ev) {
            auto it = m_EventMapIndex.find(ev);
            if (it != m_EventMapIndex.end()) {
                std::string stem = FileStem(it->second);
                std::string bank = CutsceneScriptParser::MapTextureBank(stem);
                if (!bank.empty())
                    ImGui::Text("event %u -> map %s (texture bank %s.p3tex)", ev, stem.c_str(), bank.c_str());
                else
                    ImGui::Text("event %u -> map %s", ev, stem.c_str());
            }
            else if (self_contained) {
                ImGui::Text("event %u: SELF-CONTAINED cutscene (scene embedded in the file, no cfdata map)", ev);
            }
            else if (!m_EventMapIndex.empty()) {
                ImGui::Text("event %u: not in the index (%zu events indexed). Load more cfdata maps,", ev, m_EventMapIndex.size());
                ImGui::Text("or this event is system-launched (no hosting field).");
            }
            else {
                ImGui::TextDisabled("event %u: open cfdata map files in the Studio to index them", ev);
            }
        }
        else {
            ImGui::TextDisabled("no event number in the filename");
        }
    }

    if (!s.embedded_models.empty()) {
        ImGui::Spacing();
        ImGui::TextColored({ 0.7f,0.85f,1.0f,1.0f }, "Embedded models (NMDL in this file):");
        for (const auto& m : s.embedded_models) {
            if (!m.top_level) continue;
            ImGui::BulletText("%s (0x%X bytes @0x%X)", m.name.c_str(), m.size, m.offset);
        }
    }

    ImGui::Spacing();
    ImGui::TextColored({ 0.7f,0.85f,1.0f,1.0f }, "Embedded animations (scene actors):");
    for (const std::string& a : s.embedded_anims)
        ImGui::BulletText("%s", a.c_str());
}

void CutsceneViewport::RenderCommands() {
    const CutsceneScript& s = m_Script;
    ImGui::InputTextWithHint("##filter", "filter (e.g. actor, dur, SETTER, API_1025)", m_Filter, sizeof(m_Filter));
    ImGui::Separator();

    if (ImGui::BeginTable("cmds", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("offset", ImGuiTableColumnFlags_WidthFixed, 70.0f);
        ImGui::TableSetupColumn("id", ImGuiTableColumnFlags_WidthFixed, 50.0f);
        ImGui::TableSetupColumn("command", ImGuiTableColumnFlags_WidthFixed, 140.0f);
        ImGui::TableSetupColumn("arguments", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        std::string flt = m_Filter;
        for (const CSCommand& c : s.all_commands) {
            if (!flt.empty() && c.summary.find(flt) == std::string::npos)
                continue;
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            ImGui::Text("+0x%04X", c.offset);
            ImGui::TableNextColumn();
            if (c.api_id) ImGui::Text("%u", c.api_id);
            else          ImGui::TextDisabled("-");
            ImGui::TableNextColumn();
            bool resolved = (c.api_id != 0) || (c.api_addr != 0);
            ImVec4 nc = resolved ? ImVec4{ 1.0f,0.6f,0.2f,1.0f } : ImVec4{ 0.7f,0.7f,0.7f,1.0f };
            ImGui::TextColored(nc, "%s", c.name.c_str());
            ImGui::TableNextColumn();
            for (size_t i = 0; i < c.args.size(); i++) {
                if (i) { ImGui::SameLine(0, 0); ImGui::TextUnformatted(", "); ImGui::SameLine(0, 0); }
                else    ImGui::SameLine(0, 0);
                ImGui::TextColored(ArgColor(c.args[i].kind), "%s", c.args[i].text.c_str());
            }
        }
        ImGui::EndTable();
    }
}

void CutsceneViewport::RenderActors() {
    const CutsceneScript& s = m_Script;

    // Characters derived from the embedded NMTN names (the cutscene has no explicit
    // model-load command; slots are bound at runtime by game state).
    if (!s.scene_actors.empty()) {
        ImGui::TextColored({ 0.5f,1.0f,0.6f,1.0f }, "Scene characters (from embedded animation names):");
        if (ImGui::BeginTable("chars", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("code", ImGuiTableColumnFlags_WidthFixed, 70.0f);
            ImGui::TableSetupColumn("model source", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (const auto& a : s.scene_actors) {
                ImGui::TableNextRow();
                ImGui::TableNextColumn();
                ImGui::Text("%s", a.code.c_str());
                ImGui::TableNextColumn();
                if (a.model_hint == "unknown") ImGui::TextDisabled("unknown");
                else                           ImGui::Text("%s", a.model_hint.c_str());
            }
            ImGui::EndTable();
        }
        ImGui::Spacing();
    }

    ImGui::TextColored({ 1.0f,0.85f,0.3f,1.0f }, "Actor/object slots:");
    if (ImGui::BeginTable("actors", 2, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("id", ImGuiTableColumnFlags_WidthFixed, 90.0f);
        ImGui::TableSetupColumn("uses", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();
        for (const auto& kv : s.actor_ids) {
            ImGui::TableNextRow();
            ImGui::TableNextColumn();
            if (kv.first == 0x0c40) ImGui::TextColored({ 1.0f,0.5f,1.0f,1.0f }, "0x%04X (TARGET)", kv.first);
            else                    ImGui::Text("0x%04X", kv.first);
            ImGui::TableNextColumn();
            ImGui::Text("%dx", kv.second);
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextColored({ 0.4f,0.75f,1.0f,1.0f }, "Referenced data offsets (0x07):");
    int col = 0;
    std::string acc;
    for (uint32_t o : s.data_offsets) {
        char b[24]; snprintf(b, sizeof(b), "@0x%X  ", o);
        acc += b;
        if (++col % 6 == 0) { ImGui::TextUnformatted(acc.c_str()); acc.clear(); }
    }
    if (!acc.empty()) ImGui::TextUnformatted(acc.c_str());
}

void CutsceneViewport::RenderDisassembly() {
    const CutsceneScript& s = m_Script;
    if (s.nodes.empty()) return;
    ImGui::Checkbox("Commands only (CALLAPI)", &m_OnlyCommands);
    ImGui::Separator();

    if (ImGui::BeginChild("disasm", ImVec2(0, 0), false)) {
        for (const CSNode& nd : s.nodes) {
            ImGui::TextColored({ 0.5f,1.0f,0.6f,1.0f }, "-- node %u @0x%04X (size 0x%X, %zu cmds) --",
                nd.index, nd.bc_offset, nd.size, nd.commands.size());
            for (const CSInstr& i : nd.instrs) {
                if (m_OnlyCommands && !i.is_callapi) continue;
                ImVec4 c;
                if (i.is_callapi)      c = { 1.0f,0.6f,0.2f,1.0f };
                else if (i.is_call)    c = { 0.4f,1.0f,1.0f,1.0f };
                else if (i.op == 0x03)   c = { 0.7f,0.9f,0.7f,1.0f };
                else if (i.op == 0x07)   c = { 0.4f,0.75f,1.0f,1.0f };
                else if (i.op == 0x02)   c = { 1.0f,0.85f,0.3f,1.0f };
                else                   c = { 0.7f,0.7f,0.7f,1.0f };
                ImGui::TextColored({ 0.5f,0.5f,0.5f,1.0f }, "+0x%04X", i.offset);
                ImGui::SameLine();
                ImGui::TextColored(c, "%-9s", i.mnemonic.c_str());
                if (!i.operand.empty()) { ImGui::SameLine(); ImGui::TextUnformatted(i.operand.c_str()); }
            }
            ImGui::Spacing();
        }
    }
    ImGui::EndChild();
}

void CutsceneViewport::FreeSlideTextures() {
    for (unsigned int t : m_SlideTextures)
        if (t) { GLuint g = t; glDeleteTextures(1, &g); }
    m_SlideTextures.clear();
    m_SlideTexturesBuilt = false;
}

void CutsceneViewport::BuildSlideTextures() {
    // Decode every slide image (NTX3 DXT) to a GL texture, in slide_images order.
    // Re-reads the file from disk (the parser kept offsets, not the bytes).
    m_SlideTextures.assign(m_Script.slide_images.size(), 0);
    m_SlideTexturesBuilt = true;
    if (m_Script.filename.empty()) return;

    std::ifstream f(m_Script.filename, std::ios::binary);
    if (!f) return;
    f.seekg(0, std::ios::end);
    size_t sz = (size_t)f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(sz);
    f.read((char*)data.data(), sz);
    f.close();

    for (size_t i = 0; i < m_Script.slide_images.size(); i++) {
        const auto& im = m_Script.slide_images[i];
        GLuint tex = 0;
        if (NTX3Parser::LoadTexture(data.data(), im.offset, sz, tex))
            m_SlideTextures[i] = tex;
    }
}

int CutsceneViewport::ActiveImageAt(float t) const {
    // The parser builds s.image_segments (image vs time), anchored to the caption clock
    // so each photo holds for the right length. Segments flagged title_overlay are the
    // Revolution strip composited over the opening castle; here we return the BASE image
    // (the most recent non-overlay segment), so the castle stays under the title and
    // holds through captions 1-2 until the first story photo enters.
    const CutsceneScript& s = m_Script;
    if (s.image_segments.empty()) {
        for (size_t i = 0; i < s.slide_images.size(); i++)
            if (!s.slide_images[i].is_title_strip) return (int)i;
        return s.slide_images.empty() ? -1 : 0;
    }
    int result = -1;
    for (const auto& sg : s.image_segments) {
        if (sg.title_overlay) continue;            // overlays handled separately
        if (t + 1e-4f >= sg.start_s) result = sg.image;
        else break;
    }
    if (result < 0) {
        for (const auto& sg : s.image_segments)
            if (!sg.title_overlay) { result = sg.image; break; }
    }
    return result;
}

int CutsceneViewport::ActiveOverlayAt(float t) const {
    // Returns the title-strip overlay image visible at time t (the Revolution strip over
    // or -1 if none. The overlay shows from its start until the next
    // non-overlay segment begins.
    const CutsceneScript& s = m_Script;
    for (size_t i = 0; i < s.image_segments.size(); i++) {
        const auto& sg = s.image_segments[i];
        if (!sg.title_overlay) continue;
        // find when the overlay ends: the next non-overlay segment after it
        float end = s.narrative_runtime_s;
        for (size_t j = i + 1; j < s.image_segments.size(); j++)
            if (!s.image_segments[j].title_overlay) { end = s.image_segments[j].start_s; break; }
        if (t + 1e-4f >= sg.start_s && t < end) return sg.image;
    }
    return -1;
}
void CutsceneViewport::RenderSlideshow() {
    const CutsceneScript& s = m_Script;

    const char* kindName =
        (s.kind == CutsceneScript::EventKind::Narrative) ? "Narrative slideshow"
        : "Title card";
    ImGui::TextColored({ 1.0f, 0.85f, 0.3f, 1.0f }, "%s", kindName);
    ImGui::Text("Reconstructed runtime: %.1f s  (%.1f min)",
        s.narrative_runtime_s, s.narrative_runtime_s / 60.0f);
    if (!s.music_track.empty())
        ImGui::Text("Music: %s  (%zu sync cues)", s.music_track.c_str(), s.music_cues.size());

    // ---- language selector (works for any event with subtitles) -------- //
    if (!s.languages.empty()) {
        int sel = s.selected_language < 0 ? 0 : s.selected_language;
        std::string preview = s.languages[sel].code;
        ImGui::SetNextItemWidth(160.0f);
        if (ImGui::BeginCombo("Language", preview.c_str())) {
            for (int i = 0; i < (int)s.languages.size(); i++) {
                bool isSel = (i == s.selected_language);
                if (ImGui::Selectable(s.languages[i].code.c_str(), isSel))
                    CutsceneScriptParser::SelectLanguage(m_Script, i);
                if (isSel) ImGui::SetItemDefaultFocus();
            }
            ImGui::EndCombo();
        }
    }
    ImGui::Separator();

    // ---- transport: play / scrub -------------------------------------- //
    if (ImGui::Button(m_SlidePlaying ? "Pause" : "Play")) m_SlidePlaying = !m_SlidePlaying;
    ImGui::SameLine();
    if (ImGui::Button("Restart")) { m_SlideTime = 0.0f; m_SlidePlaying = false; }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(-1.0f);
    ImGui::SliderFloat("##playhead", &m_SlideTime, 0.0f,
        s.narrative_runtime_s > 0 ? s.narrative_runtime_s : 1.0f,
        "t = %.1f s");
    if (m_SlidePlaying) {
        m_SlideTime += ImGui::GetIO().DeltaTime;
        if (m_SlideTime >= s.narrative_runtime_s) {
            m_SlideTime = s.narrative_runtime_s;
            m_SlidePlaying = false;
        }
    }

    // Map the playhead to the current subtitle line by its REAL appearance time
    // (start_s, taken from the id-219 site in the bytecode), not by summing the <w>
    // durations from t=0. A line is visible from its start_s until the next line's
    // start_s (or its start_s + duration_s, whichever the data gives). This makes the
    // captions line up with the images: the story text starts only after the opening.
    int activeLine = -1;
    {
        // find the latest line whose start_s has passed
        float bestStart = -1.0f;
        for (size_t i = 0; i < s.subtitle_lines.size(); i++) {
            float st = s.subtitle_lines[i].start_s;
            if (st < 0.0f) continue;                 // no timing for this line
            if (m_SlideTime + 1e-4f >= st && st >= bestStart) {
                // ensure it has not already ended before the next line starts
                bestStart = st;
                activeLine = (int)i;
            }
        }
        // if the active line has an explicit duration and we are past it with no newer
        // line yet, clear it (caption gap)
        if (activeLine >= 0) {
            const auto& ln = s.subtitle_lines[activeLine];
            float endT = ln.start_s + (ln.duration_s > 0.0f ? ln.duration_s : 1e9f);
            // only clear if there is no later line that should already be showing
            bool laterShowing = false;
            for (size_t i = 0; i < s.subtitle_lines.size(); i++)
                if (s.subtitle_lines[i].start_s > ln.start_s &&
                    s.subtitle_lines[i].start_s <= m_SlideTime + 1e-4f) laterShowing = true;
            if (!laterShowing && m_SlideTime > endT) activeLine = -1;
        }
    }

    ImGui::Separator();

    // ---- current full-screen image ------------------------------------ //
    if (!m_SlideTexturesBuilt) BuildSlideTextures();
    int imgIdx = ActiveImageAt(m_SlideTime);
    if (imgIdx >= 0 && imgIdx < (int)m_SlideTextures.size() && m_SlideTextures[imgIdx]) {
        const auto& im = m_Script.slide_images[imgIdx];
        // fit the image into the available width, preserving aspect ratio
        float availW = ImGui::GetContentRegionAvail().x;
        float aspect = im.height > 0 ? (float)im.width / (float)im.height : 16.0f / 9.0f;
        float dispW = availW;
        float dispH = dispW / aspect;
        // cap height so the rest of the UI stays visible
        float maxH = 360.0f;
        if (dispH > maxH) { dispH = maxH; dispW = dispH * aspect; }
        ImGui::Image((ImTextureID)(intptr_t)m_SlideTextures[imgIdx],
            ImVec2(dispW, dispH));
        ImGui::TextDisabled("image #%d  %ux%u", imgIdx, im.width, im.height);
        // opening title overlay
        int ov = ActiveOverlayAt(m_SlideTime);
        if (ov >= 0 && ov < (int)m_SlideTextures.size() && m_SlideTextures[ov]) {
            const auto& os = m_Script.slide_images[ov];
            float oAspect = os.height > 0 ? (float)os.width / (float)os.height : 4.0f;
            float oW = dispW * 0.5f;
            float oH = oW / oAspect;
            ImGui::Image((ImTextureID)(intptr_t)m_SlideTextures[ov], ImVec2(oW, oH));
            ImGui::TextDisabled("title overlay #%d", ov);
        }
    }
    else {
        ImGui::TextDisabled("(no decoded image at this time)");
    }

    ImGui::Separator();

    // ---- current caption ---------------------------------------------- //
    ImGui::TextColored({ 0.7f, 0.85f, 1.0f, 1.0f }, "Caption:");
    if (activeLine >= 0 && activeLine < (int)s.subtitle_lines.size()) {
        const auto& ln = s.subtitle_lines[activeLine];
        ImGui::TextWrapped("%s", ln.text.c_str());
        ImGui::TextDisabled("line %d/%zu  <d%d> v%d  %.1fs%s",
            activeLine + 1, s.subtitle_lines.size(),
            ln.line_type, ln.voice_index, ln.duration_s,
            ln.fade ? "  (fade)" : "");
    }
    else {
        ImGui::TextDisabled("(no caption at this time)");
    }

    ImGui::Separator();

    // ---- image list (slides) ------------------------------------------ //
    ImGui::TextColored({ 0.7f, 0.85f, 1.0f, 1.0f }, "Slide images: %zu", s.slide_images.size());
    int fullScreen = 0, titleStrips = 0;
    for (auto& im : s.slide_images) { if (im.is_title_strip) titleStrips++; else fullScreen++; }
    ImGui::Text("  full-screen: %d   title strips (localized): %d", fullScreen, titleStrips);
    if (ImGui::BeginTable("slides", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
        ImGui::TableSetupColumn("#");
        ImGui::TableSetupColumn("size");
        ImGui::TableSetupColumn("kind");
        ImGui::TableSetupColumn("offset");
        ImGui::TableHeadersRow();
        for (size_t i = 0; i < s.slide_images.size(); i++) {
            const auto& im = s.slide_images[i];
            ImGui::TableNextRow();
            ImGui::TableNextColumn(); ImGui::Text("%zu", i);
            ImGui::TableNextColumn(); ImGui::Text("%ux%u", im.width, im.height);
            ImGui::TableNextColumn(); ImGui::Text("%s", im.is_title_strip ? "title strip" : "full-screen");
            ImGui::TableNextColumn(); ImGui::Text("0x%X", im.offset);
        }
        ImGui::EndTable();
    }

    ImGui::Separator();

    // ---- subtitle timeline -------------------------------------------- //
    ImGui::TextColored({ 0.7f, 0.85f, 1.0f, 1.0f }, "Subtitle timeline:");
    ImGui::BeginChild("subs", ImVec2(0, 200), true);
    for (size_t i = 0; i < s.subtitle_lines.size(); i++) {
        const auto& ln = s.subtitle_lines[i];
        bool active = ((int)i == activeLine);
        if (active) ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(1.0f, 0.9f, 0.4f, 1.0f));
        // show the real appearance time (start_s); fall back to index if unknown
        if (ln.start_s >= 0.0f)
            ImGui::Text("[%6.1fs] %s", ln.start_s, ln.text.c_str());
        else
            ImGui::Text("[  --  ] %s", ln.text.c_str());
        if (active) ImGui::PopStyleColor();
    }
    ImGui::EndChild();
}

void CutsceneViewport::Render() {
    if (!m_Script.valid) {
        ImGui::TextDisabled("No cutscene loaded.");
        return;
    }
    if (ImGui::BeginTabBar("cs_tabs")) {
        if (ImGui::BeginTabItem("Overview")) { RenderOverview();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Commands")) { RenderCommands();    ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Actors")) { RenderActors();      ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Disassembly")) { RenderDisassembly(); ImGui::EndTabItem(); }
        if (m_Script.kind == CutsceneScript::EventKind::Narrative ||
            m_Script.kind == CutsceneScript::EventKind::TitleCard) {
            if (ImGui::BeginTabItem("Slideshow")) { RenderSlideshow(); ImGui::EndTabItem(); }
        }
        if (ImGui::BeginTabItem("Scene")) {
            uint32_t ev = CutsceneScriptParser::EventNumberFromFilename(m_Script.filename);
            std::string title = ev ? CutsceneScriptParser::EventTitle(ev) : "";
            if (!title.empty())
                ImGui::TextColored({ 1.0f,0.85f,0.3f,1.0f }, "e%04u \"%s\"", ev, title.c_str());
            // Auto-assembly: map from the index, .p3tex auto-located, actors from the
            // assets folder, initial poses from the scene animations ("move" t=0).
            if (ImGui::Button("Assets folder...")) {
                std::string dir = FileDialog::OpenFolder();
                if (!dir.empty())
                    snprintf(m_AssetsRoot, sizeof(m_AssetsRoot), "%s", dir.c_str());
            }
            ImGui::SameLine();
            if (m_AssetsRoot[0]) ImGui::TextDisabled("%s", m_AssetsRoot);
            else                 ImGui::TextDisabled("(folder with pcplk_v1.p3obj, appkeep2.bmd)");
            ImGui::SameLine();
            if (ImGui::Button("Assemble scene")) AssembleScene();
            // Director's shot list (camera slot setups in script order)
            if (m_Script.camera_shots.size() > 1) {
                ImGui::SameLine();
                ImGui::TextDisabled("Shot:");
                for (size_t i = 0; i < m_Script.camera_shots.size(); i++) {
                    ImGui::SameLine();
                    char lbl[16];
                    snprintf(lbl, sizeof(lbl), "%zu", i + 1);
                    if (ImGui::SmallButton(lbl)) {
                        const auto& sh = m_Script.camera_shots[i];
                        m_Scene.SetSceneCamera(
                            glm::vec3(sh.eye[0], sh.eye[1], sh.eye[2]),
                            glm::vec3(sh.target[0], sh.target[1], sh.target[2]));
                    }
                }
            }
            std::string map_hint, bank_hint;
            if (ev) {
                auto it = m_EventMapIndex.find(ev);
                if (it != m_EventMapIndex.end()) {
                    map_hint = FileStem(it->second);
                    bank_hint = CutsceneScriptParser::MapTextureBank(map_hint);
                }
            }
            m_Scene.RenderPanel(map_hint, bank_hint);
            ImGui::EndTabItem();
        }
        ImGui::EndTabBar();
    }
}

// File directory: "C:\maps\agn01.e" -> "C:\maps"
static std::string FileDir(const std::string& path) {
    size_t slash = path.find_last_of("/\\");
    return (slash == std::string::npos) ? "." : path.substr(0, slash);
}

static bool FileExists(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return (bool)f;
}

// One-click scene assembly for the loaded cutscene:
//   map     -> from the event->map index (full path), .p3tex auto-located near the
//              map (<dir>/<BANK>.p3tex, <dir>/maptex/<BANK>.p3tex, case variants)
//   actors  -> from the assets folder (root .p3obj / appkeep2.bmd) or, for
//              self-contained cutscenes, from the cutscene file's embedded NMDLs
//   poses   -> scene-local "move" t=0 per actor (lowest scene part)
void CutsceneViewport::AssembleScene() {
    const CutsceneScript& s = m_Script;
    if (!s.valid) return;
    // Guard: assembling only makes sense from an EVENT file. A field script (map)
    // has hosted_events; assembling from it would dump the map's embedded enemy/prop
    // NMDLs into the scene (untextured) - exactly the wrong thing.
    if (!s.hosted_events.empty()) {
        char b[160];
        snprintf(b, sizeof(b),
            "This is a FIELD script (hosts event %u). Open the event file "
            "(e%04u.e) and press Assemble there.",
            s.hosted_events[0], s.hosted_events[0]);
        m_Scene.SetStatus(b);
        return;
    }
    m_Scene.Clear();
    std::string report;

    // 1. Map + texture bank
    uint32_t ev = CutsceneScriptParser::EventNumberFromFilename(s.filename);
    auto it = (ev ? m_EventMapIndex.find(ev) : m_EventMapIndex.end());
    if (it != m_EventMapIndex.end()) {
        std::string map_path = it->second;
        std::string stem = FileStem(map_path);
        std::string bank = CutsceneScriptParser::MapTextureBank(stem);
        std::string dir = FileDir(map_path);
        std::string p3tex;
        if (!bank.empty()) {
            std::string lo = bank, up = bank;
            for (char& ch : lo) ch = (char)tolower((unsigned char)ch);
            const std::string cands[] = {
                dir + "/" + bank + ".p3tex",  dir + "/" + lo + ".p3tex",
                dir + "/maptex/" + bank + ".p3tex", dir + "/maptex/" + lo + ".p3tex",
                dir + "/../maptex/" + bank + ".p3tex", dir + "/../maptex/" + lo + ".p3tex",
            };
            for (const std::string& cnd : cands)
                if (FileExists(cnd)) { p3tex = cnd; break; }
        }
        if (p3tex.empty() && !bank.empty()) {
            // Not found near the map: ask once via the file dialog (the user keeps
            // texture banks elsewhere, e.g. a separate maptex folder)
            p3tex = FileDialog::OpenFile("P3TEX bank\0*.p3tex\0All Files\0*.*\0");
        }
        if (!p3tex.empty()) {
            if (m_Scene.LoadMap(map_path, p3tex)) {
                report += "map " + stem + " + " + bank + ".p3tex; ";
                // Seed the scene anchor at the event's map-space zone (from the
                // field's event tables) so the actors land at the right spot.
                float ap[3];
                if (ev && CutsceneScriptParser::FindEventAnchor(map_path, ev, ap)) {
                    m_Scene.SetAnchor(glm::vec3(ap[0], ap[1], ap[2]), 0.0f);
                    char ab[80];
                    snprintf(ab, sizeof(ab), "anchor (%.1f, %.1f, %.1f); ", ap[0], ap[1], ap[2]);
                    report += ab;
                }
            }
            else report += "map FAILED; ";
        }
        else {
            report += "map " + stem + ": " + bank + ".p3tex not found near the map; ";
        }
    }
    else {
        // Self-contained cutscene (e.g. e0005): the file embeds its own scene set.
        // Require real evidence (a big top-level NMDL) before going down this path.
        bool big = false;
        for (const auto& m : s.embedded_models)
            if (m.top_level && m.size > 0x100000) { big = true; break; }
        if (big) {
            bool any = false;
            for (const auto& m : s.embedded_models) {
                if (!m.top_level) continue;
                bool is_actor_model = false;
                for (const auto& a : s.scene_actors)
                    if (a.model_hint.find(m.name) != std::string::npos) { is_actor_model = true; break; }
                if (is_actor_model) continue;  // actors handled below with poses
                if (m_Scene.AddActor(s.filename, m.name)) { any = true; }
            }
            if (any) report += "embedded scene models; ";
        }
        else if (ev) {
            char b[120];
            snprintf(b, sizeof(b),
                "event %u: map not indexed yet. Open the hosting cfdata map "
                "(.e) once, then reopen this event and assemble. ", ev);
            report += b;
        }
        else {
            report += "no event number in the filename; ";
        }
    }

    // 2a. Script placements: first SET_POSITION/SET_ANGLES master-library call per
    //     actor slot (floats pushed Z, Y, X). These are THE engine placements
    //     (validated live: 0x0c01 -> (-0.326, -0.047, -5.351) yaw 0.485, exactly the
    //     debugger values), in the same local frame as the camera, feet on ground.
    struct SlotPose { float x = 0, y = 0, z = 0, yaw = 0; bool has_pos = false, has_yaw = false; };
    std::map<uint32_t, SlotPose> slot_poses;
    for (const auto& c : s.all_commands) {
        if (!c.is_lib || (c.api_id != 0x0F && c.api_id != 0x10)) continue;
        uint32_t slot = 0;
        float f[3]; int nf = 0;
        for (const auto& arg : c.args) {
            if (arg.kind == CSArgKind::ActorId) slot = arg.u;
            if (arg.kind == CSArgKind::Float && nf < 3) f[nf++] = arg.f;
        }
        if (!slot || slot == 0x0c03 || nf < 3) continue;  // skip the camera slot
        SlotPose& sp = slot_poses[slot];
        if (c.api_id == 0x0F && !sp.has_pos) {
            sp.x = f[2]; sp.y = f[1]; sp.z = f[0];  // pushed Z, Y, X
            sp.has_pos = true;
        }
        else if (c.api_id == 0x10 && !sp.has_yaw) {
            sp.yaw = f[1];  // SET_ANGLES(z?, yaw, pitch?) - middle value is the yaw
            sp.has_yaw = true;
        }
    }

    // 2b. Actors: load models and pose them. Slot <-> character mapping heuristic:
    //     slots in ascending order match the actors in file order (0x0c01 = cpn for
    //     e1180, confirmed by the debugger captures).
    std::vector<uint32_t> pose_slots;
    for (const auto& kv : slot_poses)
        if (kv.second.has_pos) pose_slots.push_back(kv.first);
    size_t slot_i = 0;
    for (const auto& a : s.scene_actors) {
        std::string path, filter;
        if (a.model_hint.rfind("embedded: ", 0) == 0) {
            path = s.filename;
            filter = a.model_hint.substr(10);
        }
        else if (a.model_hint.find("appkeep2.bmd") != std::string::npos) {
            path = std::string(m_AssetsRoot) + "/appkeep2.bmd";
            filter = a.code;
        }
        else if (a.model_hint.rfind("root: ", 0) == 0) {
            path = std::string(m_AssetsRoot) + "/pc" + a.code + "_v1.p3obj";
            filter = "";
        }
        else {
            continue;  // unknown code (facial/aux tags)
        }
        if (!FileExists(path)) { report += a.code + ": file missing; "; continue; }
        if (!m_Scene.AddActor(path, filter)) { report += a.code + ": load failed; "; continue; }
        if (slot_i < pose_slots.size()) {
            const SlotPose& sp = slot_poses[pose_slots[slot_i++]];
            m_Scene.SetLastInstancePose(glm::vec3(sp.x, sp.y, sp.z),
                glm::degrees(sp.yaw));
            report += a.code + " posed; ";
        }
        else {
            // Fallback: the "move" track's t=0 (hip-driven: use X/Z + yaw, feet at 0)
            char evpfx[16];
            snprintf(evpfx, sizeof(evpfx), "e%04u_", ev);
            const CutsceneScript::CSScenePlacement* best = nullptr;
            bool best_own = false;
            for (const auto& p : s.scene_placements) {
                if (p.code != a.code) continue;
                bool own = (p.anim.rfind(evpfx, 0) == 0);
                if (!best || (own && !best_own) || (own == best_own && p.part < best->part)) {
                    best = &p;
                    best_own = own;
                }
            }
            if (best) {
                m_Scene.SetLastInstancePose(
                    glm::vec3(best->pos[0], 0.0f, best->pos[2]),
                    glm::degrees(best->rot[1]));
                report += a.code + " posed (anim); ";
            }
            else {
                report += a.code + " (no placement); ";
            }
        }
    }

    // 3. Director's camera from the script (slot 0x0c03)
    if (s.has_camera) {
        m_Scene.SetSceneCamera(
            glm::vec3(s.cam_eye[0], s.cam_eye[1], s.cam_eye[2]),
            glm::vec3(s.cam_target[0], s.cam_target[1], s.cam_target[2]));
        // Build the camera path. Each beat is the 0x0c03 framing; when a 0x0c04 pair
        // exists (confirmed in runtime to run with it on the black-screen opening,
        // with pitch), add it as a second key for that beat so the opening reads as
        // the two-pose move the game actually performs.
        std::vector<CutsceneScene::CSCamKey> path;
        for (const auto& sh : s.camera_shots) {
            CutsceneScene::CSCamKey k;
            k.eye = glm::vec3(sh.eye[0], sh.eye[1], sh.eye[2]);
            k.target = glm::vec3(sh.target[0], sh.target[1], sh.target[2]);
            k.dur = sh.duration;
            path.push_back(k);
            if (sh.has_pair) {
                // The 0x0c04 pair is a second confirmed pose for this beat. Added as
                // its own key. The transition between the 0x0c03 and 0x0c04 poses is
                // not reproduced here (its mechanism is unconfirmed - see Cutscenescene).
                CutsceneScene::CSCamKey kp;
                kp.eye = glm::vec3(sh.pair_eye[0], sh.pair_eye[1], sh.pair_eye[2]);
                kp.target = glm::vec3(sh.pair_target[0], sh.pair_target[1], sh.pair_target[2]);
                kp.dur = sh.duration;
                path.push_back(kp);
            }
        }
        m_Scene.SetCameraPath(path);

        // (3D camera VM timeline omitted in this build - the slideshow path does
        // not use it; 3D cutscenes fall back to the snap-hold camera path above.)

        char cb[48];
        snprintf(cb, sizeof(cb), "camera set (%zu shots); ", s.camera_shots.size());
        report += cb;
    }
    m_Scene.SetStatus(report.empty() ? "assembled" : report);
}