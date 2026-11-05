// ImGui front end of EventPlayer, plus the folder logic that finds an event's assets and
// its map (kept apart from the GL/evaluation code so the player can run headless).
#include "EventPlayer.h"
#include "Cutscenescriptparser.h"
#include "FileDialog.h"
#include <imgui.h>
#include <algorithm>
#include <cstdio>
#include <cstring>
#include <filesystem>

namespace fs = std::filesystem;

static const char* kLangs[] = { "JPN", "USA", "GBR", "FRA", "ITA", "DEU", "ESP" };

static void SetPath(char* dst, size_t n, const std::string& s) { snprintf(dst, n, "%s", s.c_str()); }
static std::string Lower(std::string s) { for (char& c : s) c = (char)tolower((unsigned char)c); return s; }
static bool Exists(const std::string& p) { std::error_code ec; return !p.empty() && fs::is_regular_file(p, ec); }

// First file named `name` (case-insensitive, also with a flattened "cfdata_" prefix) in
// dir, its parent, or one level of subfolders of either (cfdata/maptex, USRDIR, ...).
static std::string FindNear(const std::string& dir, const std::string& name) {
    std::error_code ec;
    const std::string a = Lower(name), b = "cfdata_" + a;
    auto match = [&](const fs::path& p) { std::string f = Lower(p.filename().string()); return f == a || f == b; };
    for (const fs::path& d : { fs::path(dir), fs::path(dir).parent_path() }) {
        if (d.empty() || !fs::is_directory(d, ec)) continue;
        for (auto& e : fs::directory_iterator(d, ec)) if (e.is_regular_file(ec) && match(e.path())) return e.path().string();
        for (auto& e : fs::directory_iterator(d, ec)) {
            if (!e.is_directory(ec)) continue;
            for (auto& f : fs::directory_iterator(e.path(), ec)) if (f.is_regular_file(ec) && match(f.path())) return f.path().string();
        }
    }
    return "";
}

static bool IsDir(const std::string& p) { std::error_code ec; return !p.empty() && fs::is_directory(p, ec); }

// FindNear from several roots and their ancestors (up to 3 levels): events and maps can
// live in different extraction trees (evdata_extracted\cfdata, cfdata_extracted\cfdata).
static std::string FindUp(const std::vector<std::string>& roots, const std::string& name) {
    for (const std::string& r : roots) {
        fs::path p(r);
        for (int up = 0; up < 3 && !p.empty(); up++, p = p.parent_path()) {
            std::string f = FindNear(p.string(), name);
            if (!f.empty()) return f;
            if (p == p.parent_path()) break;
        }
    }
    return "";
}

// Count of map scripts (.e files that are not events and not lib.e) in a folder
static int CountMapScripts(const fs::path& d) {
    std::error_code ec; int n = 0;
    if (!fs::is_directory(d, ec)) return 0;
    for (auto& e : fs::directory_iterator(d, ec)) {
        if (!e.is_regular_file(ec) || Lower(e.path().extension().string()) != ".e") continue;
        uint32_t ev, part; std::string p = e.path().string();
        if (!es::EventNumberFromPath(p, ev, part) && es::MapStemFromPath(p) != "lib") n++;
    }
    return n;
}

// The maps folder next to the events one: the event folder itself, or any folder up to
// two levels below the event folder's ancestors (ps3\cfdata_extracted\cfdata for events
// in ps3\evdata_extracted\cfdata). The folder with the most map scripts wins.
static std::string FindMapsDir(const std::string& event_dir) {
    std::error_code ec;
    fs::path best; int best_n = CountMapScripts(event_dir);
    if (best_n > 0) best = event_dir;
    fs::path p = fs::path(event_dir).parent_path();
    for (int up = 0; up < 2 && !p.empty(); up++, p = p.parent_path()) {
        if (!fs::is_directory(p, ec)) break;
        for (auto& a : fs::directory_iterator(p, ec)) {
            if (!a.is_directory(ec)) continue;
            int n = CountMapScripts(a.path());
            if (n > best_n) { best_n = n; best = a.path(); }
            for (auto& b : fs::directory_iterator(a.path(), ec)) {
                if (!b.is_directory(ec)) continue;
                int m = CountMapScripts(b.path());
                if (m > best_n) { best_n = m; best = b.path(); }
            }
        }
        if (p == p.parent_path()) break;
    }
    return best.string();
}

void EventPlayer::LoadSettings() {
    m_SettingsLoaded = true;
    FILE* f = fopen("event_player.ini", "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof(line), f)) {
        std::string l = line; while (!l.empty() && (l.back() == '\n' || l.back() == '\r')) l.pop_back();
        size_t eq = l.find('='); if (eq == std::string::npos) continue;
        std::string k = l.substr(0, eq), v = l.substr(eq + 1);
        if (k == "maps_dir") SetPath(m_PathMaps, sizeof(m_PathMaps), v);
        else if (k == "lib") SetPath(m_PathLib, sizeof(m_PathLib), v);
        else if (k == "appkeep2") SetPath(m_PathAk2, sizeof(m_PathAk2), v);
        else if (k == "party") SetPath(m_PathParty, sizeof(m_PathParty), v);
        else if (k == "lang") m_LangIdx = std::max(0, std::min(6, atoi(v.c_str())));
    }
    fclose(f);
}

void EventPlayer::SaveSettings() const {
    FILE* f = fopen("event_player.ini", "w");
    if (!f) return;
    fprintf(f, "maps_dir=%s\nlib=%s\nappkeep2=%s\nparty=%s\nlang=%d\n", m_PathMaps, m_PathLib, m_PathAk2, m_PathParty, m_LangIdx);
    fclose(f);
}

// Scan every map script of a folder once: which events each one sets up or starts.
void EventPlayer::IndexFolder(const std::string& dir) {
    if (dir == m_IndexDir) return;
    m_IndexDir = dir; m_Index.clear(); m_MapFiles.clear();
    std::error_code ec;
    if (!fs::is_directory(dir, ec)) return;
    for (auto& e : fs::directory_iterator(dir, ec)) {
        if (!e.is_regular_file(ec) || Lower(e.path().extension().string()) != ".e") continue;
        std::string p = e.path().string();
        uint32_t ev, part;
        if (es::EventNumberFromPath(p, ev, part)) continue;          // event files
        std::string stem = es::MapStemFromPath(p);
        if (stem == "lib") continue;
        m_MapFiles[stem] = p;
        for (auto& r : es::ScanMapEvents(p)) m_Index.push_back(r);
    }
}

void EventPlayer::ApplyMapPick(int i) {
    m_MapPick = i;
    if (i < 0 || i >= (int)m_MapChoices.size() || m_MapChoices[i].path.empty()) { m_PathMap[0] = 0; m_PathTex[0] = 0; return; }
    const MapChoice& c = m_MapChoices[i];
    SetPath(m_PathMap, sizeof(m_PathMap), c.path);
    std::string bank = CutsceneScriptParser::MapTextureBank(c.stem);   // eboot map table
    SetPath(m_PathTex, sizeof(m_PathTex), bank.empty() ? "" : FindNear(fs::path(c.path).parent_path().string(), bank + ".p3tex"));
}

bool EventPlayer::OpenEvent(const std::string& path) {
    uint32_t ev = 0, part = 0;
    if (!es::EventNumberFromPath(path, ev, part)) return false;
    if (!m_SettingsLoaded) LoadSettings();
    m_LastHint = path;
    std::string dir = fs::path(path).parent_path().string();
    SetPath(m_PathEvent, sizeof(m_PathEvent), path);
    // Maps folder: the saved one, else found next to the events tree
    if (!IsDir(m_PathMaps)) SetPath(m_PathMaps, sizeof(m_PathMaps), FindMapsDir(dir));
    std::string maps = m_PathMaps;
    std::vector<std::string> roots = { dir };
    if (!maps.empty() && maps != dir) roots.push_back(maps);
    if (!Exists(m_PathLib)) SetPath(m_PathLib, sizeof(m_PathLib), FindUp(maps.empty() ? roots : std::vector<std::string>{ maps, dir }, "lib.e"));
    if (!Exists(m_PathAk2)) SetPath(m_PathAk2, sizeof(m_PathAk2), FindUp(roots, "appkeep2.bmd"));
    if (!IsDir(m_PathParty)) {
        std::string p = FindUp(roots, "pcplk_v1.p3obj");
        SetPath(m_PathParty, sizeof(m_PathParty), p.empty() ? "" : fs::path(p).parent_path().string());
    }
    SaveSettings();
    // Map candidates: maps whose script branches on this event (they build its scene),
    // then maps named by sym20018 calls that start it (same part).
    IndexFolder(maps.empty() ? dir : maps);
    m_MapChoices.clear();
    auto add = [&](const std::string& stem, const std::string& how) {
        for (auto& c : m_MapChoices) if (c.stem == stem) { if (c.how.find(how) == std::string::npos) c.how += "; " + how; return; }
        auto f = m_MapFiles.find(stem);
        m_MapChoices.push_back({ stem, f == m_MapFiles.end() ? "" : f->second, how });
        };
    for (auto& r : m_Index) if (r.event == ev && r.kind == es::EventRef::Branch) add(r.map, "sets up e" + std::to_string(ev));
    for (auto& r : m_Index) if (r.event == ev && r.kind == es::EventRef::Start && r.part == part)
        add(r.map, "started from " + r.from + (r.part ? " (part " + std::to_string(r.part) + ")" : ""));
    int pick = -1;
    for (size_t i = 0; i < m_MapChoices.size(); i++) if (!m_MapChoices[i].path.empty()) { pick = (int)i; break; }
    ApplyMapPick(pick);
    Assets a;
    a.event_path = m_PathEvent; a.lib_path = m_PathLib; a.appkeep2_path = m_PathAk2; a.party_dir = m_PathParty;
    a.map_path = m_PathMap; a.p3tex_path = m_PathTex; a.lang = kLangs[m_LangIdx];
    Load(a);
    m_Playing = false;
    return true;
}

void EventPlayer::RenderPanel(const std::string& event_hint) {
    // An event opened elsewhere (file browser) that OpenEvent has not seen yet
    if (event_hint != m_LastHint) {
        m_LastHint = event_hint;
        uint32_t ev, part;
        if (es::EventNumberFromPath(event_hint, ev, part)) OpenEvent(event_hint);
    }
    auto reload = [&]() {
        Assets a;
        a.event_path = m_PathEvent; a.lib_path = m_PathLib; a.appkeep2_path = m_PathAk2;
        a.party_dir = m_PathParty; a.map_path = m_PathMap; a.p3tex_path = m_PathTex; a.lang = kLangs[m_LangIdx];
        Load(a); m_Playing = false;
        };

    // Map for this event (from the folder's map scripts) + language
    {
        std::string cur = (m_MapPick >= 0 && m_MapPick < (int)m_MapChoices.size()) ? m_MapChoices[m_MapPick].stem : "(no map)";
        ImGui::SetNextItemWidth(220);
        if (ImGui::BeginCombo("map", cur.c_str())) {
            if (ImGui::Selectable("(no map)", m_MapPick < 0)) { ApplyMapPick(-1); reload(); }
            for (int i = 0; i < (int)m_MapChoices.size(); i++) {
                const MapChoice& c = m_MapChoices[i];
                std::string lbl = c.stem + "  -  " + c.how + (c.path.empty() ? "  [not in this folder]" : "");
                ImGui::BeginDisabled(c.path.empty());
                if (ImGui::Selectable(lbl.c_str(), i == m_MapPick)) { ApplyMapPick(i); reload(); }
                ImGui::EndDisabled();
            }
            ImGui::EndCombo();
        }
        ImGui::SameLine();
        if (m_IndexDir.empty()) ImGui::TextDisabled("no maps folder yet: set it under Files");
        else if (m_MapChoices.empty()) ImGui::TextDisabled("none of the %zu map scripts in the maps folder names this event", m_MapFiles.size());
        else ImGui::TextDisabled("%zu candidate map(s) from %zu map scripts", m_MapChoices.size(), m_MapFiles.size());
        ImGui::SameLine();
        ImGui::SetNextItemWidth(70);
        if (ImGui::Combo("text", &m_LangIdx, kLangs, 7)) { SaveSettings(); if (m_Loaded) reload(); }
    }
    if (ImGui::CollapsingHeader("Files")) {
        auto row = [](const char* label, char* buf, size_t n, const char* filter, bool folder) {
            ImGui::PushID(label);
            ImGui::SetNextItemWidth(-140);
            ImGui::InputText(label, buf, n);
            ImGui::SameLine();
            if (ImGui::SmallButton("...")) {
                std::string p = folder ? FileDialog::OpenFolder() : FileDialog::OpenFile(filter);
                if (!p.empty()) snprintf(buf, n, "%s", p.c_str());
            }
            ImGui::PopID();
            };
        row("event .e", m_PathEvent, sizeof(m_PathEvent), "Event\0*.e\0All\0*.*\0", false);
        row("maps folder", m_PathMaps, sizeof(m_PathMaps), nullptr, true);
        row("lib.e", m_PathLib, sizeof(m_PathLib), "lib.e\0*.e\0All\0*.*\0", false);
        row("appkeep2.bmd", m_PathAk2, sizeof(m_PathAk2), "BMD\0*.bmd\0All\0*.*\0", false);
        row("party folder", m_PathParty, sizeof(m_PathParty), nullptr, true);
        row("host map .e", m_PathMap, sizeof(m_PathMap), "Map\0*.e\0All\0*.*\0", false);
        row("map .p3tex", m_PathTex, sizeof(m_PathTex), "P3TEX\0*.p3tex\0All\0*.*\0", false);
        if (ImGui::Button("Reload")) { SaveSettings(); reload(); }
        ImGui::SameLine();
        if (ImGui::Button("Re-index maps folder")) { m_IndexDir.clear(); SaveSettings(); OpenEvent(m_PathEvent); }
        ImGui::SameLine();
        ImGui::TextDisabled("maps folder, lib.e, appkeep2 and party folder are remembered in event_player.ini");
    }
    if (!m_Log.empty()) ImGui::TextDisabled("%s", m_Log.c_str());
    for (auto& r : m_Report) ImGui::TextColored({ 0.95f, 0.75f, 0.45f, 1.0f }, "%s", r.c_str());
    if (!m_Loaded) { ImGui::TextDisabled("Open an event file (eNNNN.e) in the Studio, or fill the paths under Files and press Reload."); return; }

    // Transport
    ImGuiIO& io = ImGui::GetIO();
    if (m_Playing) {
        SetTime(m_T + io.DeltaTime * m_Speed);
        if (m_T >= Duration()) m_Playing = false;
    }
    if (ImGui::Button(m_Playing ? "Pause" : "Play")) { if (!m_Playing && m_T >= Duration()) SetTime(0); m_Playing = !m_Playing; }
    ImGui::SameLine();
    float t = (float)m_T;
    ImGui::SetNextItemWidth(-330);
    if (ImGui::SliderFloat("##t", &t, 0.0f, (float)Duration(), "%.2f s")) SetTime(t);
    ImGui::SameLine(); ImGui::SetNextItemWidth(70);
    ImGui::SliderFloat("speed", &m_Speed, 0.25f, 2.0f, "%.2fx");
    ImGui::SameLine(); ImGui::Checkbox("free camera", &m_UseFree);

    // View (16:9)
    float W = ImGui::GetContentRegionAvail().x;
    float H = std::min(W * 9.0f / 16.0f, ImGui::GetContentRegionAvail().y - 170.0f);
    if (H < 120) H = 120;
    W = H * 16.0f / 9.0f;
    Render((int)W, (int)H, m_UseFree);
    ImVec2 p0 = ImGui::GetCursorScreenPos(), p1(p0.x + W, p0.y + H);
    ImGui::InvisibleButton("##epview", ImVec2(W, H), ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);
    ImDrawList* dl = ImGui::GetWindowDrawList();
    dl->AddImage((ImTextureID)(intptr_t)m_FBOTex, p0, p1, ImVec2(0, 1), ImVec2(1, 0));
    float fa = m_UseFree ? 0.0f : FadeAlpha();
    if (fa > 0.001f) dl->AddRectFilled(p0, p1, IM_COL32(0, 0, 0, (int)(fa * 255)));
    dl->AddText(ImVec2(p0.x + 8, p0.y + 6), IM_COL32(230, 222, 200, 230), CameraInfo().c_str());
    std::string sub = Subtitle();
    if (!sub.empty()) {
        ImVec2 ts = ImGui::CalcTextSize(sub.c_str());
        ImVec2 sp(p0.x + (W - ts.x) * 0.5f, p1.y - ts.y - 18);
        dl->AddText(ImVec2(sp.x + 1, sp.y + 1), IM_COL32(0, 0, 0, 255), sub.c_str());
        dl->AddText(sp, IM_COL32(255, 255, 255, 255), sub.c_str());
    }
    if (m_UseFree && (ImGui::IsItemHovered() || ImGui::IsItemActive())) {
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0)) m_FreeCam.Rotate(io.MouseDelta.x * 0.005f, io.MouseDelta.y * 0.005f);
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0)) m_FreeCam.Pan(-io.MouseDelta.x, io.MouseDelta.y);
        if (io.MouseWheel != 0) m_FreeCam.Zoom(-io.MouseWheel * m_FreeCam.GetDistance() * 0.1f);
    }

    // Timeline lanes: camera activations, motions per actor, screen layers, text, fade
    std::vector<uint16_t> actors;
    for (auto& r : m_TL.records) if (r.type == es::EvRecord::Motion && r.nmtn && std::find(actors.begin(), actors.end(), r.obj) == actors.end()) actors.push_back(r.obj);
    const bool slides = !m_Slides.empty();
    const float lh = 16.0f, lx = 90.0f;
    int lanes = 3 + (int)actors.size() + (slides ? 1 : 0);
    ImVec2 q0 = ImGui::GetCursorScreenPos();
    float tw = ImGui::GetContentRegionAvail().x;
    ImGui::InvisibleButton("##eptl", ImVec2(tw, lanes * lh + 6));
    double dur = std::max(1.0, Duration());
    auto X = [&](double tt) { return q0.x + lx + (float)((tw - lx - 6) * tt / dur); };
    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
        SetTime((io.MousePos.x - q0.x - lx) / (tw - lx - 6) * dur);
    auto laneY = [&](int i) { return q0.y + i * lh; };
    const ImU32 cCam = IM_COL32(127, 166, 217, 220), cMo = IM_COL32(159, 196, 154, 220), cTx = IM_COL32(217, 154, 143, 220),
        cSl = IM_COL32(205, 180, 120, 220), cTxt = IM_COL32(170, 170, 180, 255);
    int L = 0;
    dl->AddText(ImVec2(q0.x + 2, laneY(L)), cTxt, "camera");
    for (size_t i = 0; i < m_CamActs.size(); i++) {
        const auto& a = m_TL.records[m_CamActs[i]];
        double nx = i + 1 < m_CamActs.size() ? m_TL.records[m_CamActs[i + 1]].t : Duration();
        if (nx - a.t < 1e-3) continue;
        dl->AddRectFilled(ImVec2(X(a.t), laneY(L) + 2), ImVec2(std::max(X(nx) - 1, X(a.t) + 2), laneY(L) + lh - 2), (i & 1) ? IM_COL32(127, 166, 217, 150) : cCam);
        if (a.blend > 0) dl->AddRectFilledMultiColor(ImVec2(X(a.t), laneY(L) + 2), ImVec2(X(std::min(nx, a.t + a.blend)), laneY(L) + lh - 2), IM_COL32(20, 20, 26, 200), 0, 0, IM_COL32(20, 20, 26, 200));
    }
    for (size_t k = 0; k < actors.size(); k++) {
        L++; char lb[32]; snprintf(lb, sizeof(lb), "%u", actors[k]);
        dl->AddText(ImVec2(q0.x + 2, laneY(L)), cTxt, lb);
        for (auto& r : m_TL.records) {
            if (r.type != es::EvRecord::Motion || r.obj != actors[k] || !r.nmtn) continue;
            Anim* A = GetAnim(r.nmtn);
            double len = A ? std::max(0.15, (A->a.frame_count * A->mult - r.start_ticks) / 300.0) : 0.5;
            bool face = r.str.rfind("f_", 0) == 0 || r.str.rfind("hr_", 0) == 0 || r.str.rfind("hl_", 0) == 0;
            dl->AddRectFilled(ImVec2(X(r.t), laneY(L) + (face ? lh - 6 : 2)), ImVec2(std::max(X(r.t + len), X(r.t) + 2), laneY(L) + lh - 2), face ? IM_COL32(159, 196, 154, 110) : cMo);
        }
    }
    if (slides) {   // one bar per picture: from creation to its hide (1028 b=1) or delete
        L++;
        dl->AddText(ImVec2(q0.x + 2, laneY(L)), cTxt, "pictures");
        int n = 0;
        for (const Slide& sl : m_Slides) {
            auto o = m_TL.objects.find(sl.id);
            double a = o != m_TL.objects.end() ? o->second.t_create : 0.0, b = Duration();
            for (auto& r : m_TL.records)
                if (r.obj == sl.id && r.t > a && (r.type == es::EvRecord::Delete || (r.type == es::EvRecord::RenderFlag && r.idx == 1028 && r.v[0] != 0))) { b = r.t; break; }
            bool strip = !sl.fullscreen;
            dl->AddRectFilled(ImVec2(X(a), laneY(L) + (strip ? lh - 6 : 2)), ImVec2(std::max(X(b) - 1, X(a) + 2), laneY(L) + lh - 2),
                strip ? IM_COL32(205, 180, 120, 120) : ((n++ & 1) ? IM_COL32(205, 180, 120, 150) : cSl));
        }
    }
    L++;
    dl->AddText(ImVec2(q0.x + 2, laneY(L)), cTxt, "text");
    for (auto& r : m_TL.records) {
        if (r.type != es::EvRecord::Text) continue;
        double d = 3.0; size_t w = r.str.find("<w");
        if (w != std::string::npos && isdigit((unsigned char)r.str[w + 2])) d = atoi(r.str.c_str() + w + 2) / 1000.0;
        dl->AddRectFilled(ImVec2(X(r.t), laneY(L) + 2), ImVec2(X(r.t + d), laneY(L) + lh - 2), cTx);
    }
    L++;
    dl->AddText(ImVec2(q0.x + 2, laneY(L)), cTxt, "fade");
    double keep = m_T;
    for (float px = 0; px < tw - lx - 6; px += 3) {
        m_T = px / (tw - lx - 6) * dur;
        float a = FadeAlpha();
        if (a > 0.02f) dl->AddRectFilled(ImVec2(q0.x + lx + px, laneY(L) + 2), ImVec2(q0.x + lx + px + 3, laneY(L) + lh - 2), IM_COL32(140, 138, 156, (int)(a * 255)));
    }
    m_T = keep;
    dl->AddLine(ImVec2(X(m_T), q0.y), ImVec2(X(m_T), q0.y + lanes * lh), IM_COL32(216, 179, 106, 255), 2.0f);

    // Objects
    if (ImGui::TreeNode("Objects")) {
        for (auto& kv : m_TL.objects) {
            const es::EvObject& o = kv.second;
            if (o.kind == es::ObjKind::Actor) ImGui::Text("%u  actor, 1141 id %d", o.id, o.char_id);
            else if (o.kind == es::ObjKind::Model || o.kind == es::ObjKind::Effect) ImGui::Text("%u  %s %s %s @0x%X", o.id, es::ObjKindName(o.kind), o.tag.c_str(), o.name.c_str(), o.ref);
            else ImGui::Text("%u  %s", o.id, es::ObjKindName(o.kind));
        }
        ImGui::TreePop();
    }
}
