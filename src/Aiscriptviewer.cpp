#include "AIScriptViewer.h"
#include <imgui.h>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstring>

namespace fs = std::filesystem;

AIScriptViewer::AIScriptViewer() {}

void AIScriptViewer::Load(const std::string& filepath) {
    m_Script = AIScriptParser::Parse(filepath);
    m_SelectedNode = 0;
    m_ActiveTab = 0;
    memset(m_NodeSearch, 0, sizeof(m_NodeSearch));
}

void AIScriptViewer::Clear() {
    m_Script = AIScriptFile();
}

// Colour helpers
static ImVec4 MnemColor(const std::string& m) {
    if (m == "JMPNODE")                          return { 0.4f,1.0f,1.0f,1.0f };   // cyan
    if (m == "CALLAPI")                          return { 1.0f,0.6f,0.2f,1.0f };   // orange
    if (m == "JMP" || m == "JMP.B")              return { 1.0f,0.9f,0.3f,1.0f };   // yellow
    if (m == "BRT" || m == "BRF")               return { 1.0f,0.85f,0.4f,1.0f };  // light yellow
    if (m == "JTRUE" || m == "JFALSE")           return { 1.0f,0.75f,0.3f,1.0f };
    if (m == "LOOP.B")                           return { 0.9f,0.7f,1.0f,1.0f };   // lavender
    if (m == "RETURN")                           return { 1.0f,0.4f,0.4f,1.0f };   // red
    if (m == "PUSH" || m == "PUSH.F" ||
        m == "PUSH.I" || m == "PUSH.SI" ||
        m == "PUSH.FLAGS" || m == "PUSH.W")      return { 0.5f,0.9f,0.5f,1.0f };   // green
    if (m == "PUSH_ADDR")                        return { 0.4f,0.75f,1.0f,1.0f };  // steel blue
    if (m == "LDVAL")                            return { 0.7f,0.85f,1.0f,1.0f };  // light blue
    if (m == "STORE")                            return { 0.9f,0.9f,0.5f,1.0f };   // gold
    if (m == "COND")                             return { 0.8f,0.5f,1.0f,1.0f };   // purple
    if (m == "SWITCH")                           return { 1.0f,0.5f,1.0f,1.0f };   // pink
    if (m == "IFTRUE")                           return { 0.4f,1.0f,0.6f,1.0f };   // green
    if (m == "IFFALSE")                          return { 1.0f,0.45f,0.45f,1.0f }; // red
    if (m == "IFEITH")                           return { 0.9f,0.6f,0.9f,1.0f };
    if (m == "GETRES" || m == "GETRES_ALT" ||
        m == "GETRES_89")                        return { 0.8f,0.8f,0.5f,1.0f };   // tan
    if (m == "YIELD")                            return { 0.6f,0.9f,1.0f,1.0f };   // light blue
    if (m == "MOV" || m == "MOVC")              return { 0.75f,0.75f,0.75f,1.0f };
    if (m == "ADD" || m == "SUB" ||
        m == "MUL" || m == "ABS")               return { 0.7f,0.9f,0.7f,1.0f };
    if (m == "EQ" || m == "NE" ||
        m == "LT" || m == "GT" || m == "LE"
        || m == "CMP")                             return { 0.9f,0.75f,0.5f,1.0f };
    if (m == "CMP_TIMER")                       return { 1.0f,0.7f,0.4f,1.0f };   // warm orange
    if (m == "SETBIT" || m == "CLRBIT")         return { 0.7f,0.9f,0.7f,1.0f };
    if (m == "STFLAG" || m == "GETFL" ||
        m == "SETFG" || m == "GETST" ||
        m == "SETST" || m == "SETST2")         return { 0.7f,0.7f,1.0f,1.0f };   // periwinkle
    if (m == "TEST" || m == "LOOPN" ||
        m == "MODF")                            return { 0.8f,0.8f,0.8f,1.0f };
    if (m == "POPN")                            return { 1.0f,0.6f,0.8f,1.0f };   // pink
    if (m == "MASK")                            return { 0.6f,0.6f,0.6f,1.0f };
    return { 0.65f,0.65f,0.65f,1.0f };
}

// Special operand coloring
static ImVec4 OperandColor(const AIInstr& instr) {
    if (instr.uses_shared_mem) return { 0.3f, 1.0f, 0.8f, 1.0f };  // teal — shared memory
    if (instr.is_action_write) return { 1.0f, 0.85f, 0.2f, 1.0f }; // gold  — action register write
    if (instr.uses_global)     return { 0.7f, 0.85f, 1.0f, 1.0f }; // blue  — local register
    if (instr.is_jump)         return { 1.0f, 0.85f, 0.3f, 1.0f }; // yellow — jump target
    if (instr.is_call)         return MnemColor(instr.mnemonic);
    return { 0.82f, 0.82f, 0.82f, 1.0f };
}

// Top-level render
void AIScriptViewer::Render() {
    if (!m_Script.valid) {
        ImGui::TextDisabled("No AI script loaded");
        ImGui::Spacing();
        ImGui::TextWrapped("Open a bos*.e or em*.e file in the File Browser.");
        return;
    }

    if (ImGui::BeginTabBar("AITabs")) {
        if (ImGui::BeginTabItem("Overview")) { RenderOverview();     ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Disassembly")) { RenderDisassembly();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Actions")) { RenderActionTable();  ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("API Calls")) { RenderApiCatalog();   ImGui::EndTabItem(); }
        if (ImGui::BeginTabItem("Ctrl Flow")) { RenderControlFlow();  ImGui::EndTabItem(); }
        if (m_Script.has_debug_strings) {
            if (ImGui::BeginTabItem("Strings")) { RenderStrings();      ImGui::EndTabItem(); }
        }
        ImGui::EndTabBar();
    }
}

// OVERVIEW TAB
void AIScriptViewer::RenderOverview() {
    fs::path p(m_Script.filename);
    ImGui::Text("File: %s", p.filename().string().c_str());
    ImGui::Separator();

    bool isComplex = (m_Script.tier == AIScriptTier::Complex);
    if (isComplex)
        ImGui::TextColored({ 0.4f,1.0f,0.6f,1.0f },
            "COMPLEX  — Unique behaviour tree  (%u nodes)", m_Script.node_count);
    else
        ImGui::TextColored({ 0.7f,0.7f,0.7f,1.0f },
            "SIMPLE   — Default combat template (%u nodes)", m_Script.node_count);

    ImGui::Spacing();
    ImGui::Text("Hash:       0x%08X", m_Script.hash);
    ImGui::Text("Build ID:   0x%08X  (v1/v2 = semantically identical)", m_Script.secondary_id);
    ImGui::Text("File size:  %u bytes", m_Script.file_size);
    ImGui::Text("Bytecode:   %u bytes", m_Script.code_size);
    ImGui::Separator();

    // Action table summary
    if (!m_Script.action_table.empty()) {
        ImGui::TextColored({ 1.0f,0.85f,0.3f,1.0f },
            "Action table:  %zu registered actions (SWITCH #24)",
            m_Script.action_table.size());
        for (auto& a : m_Script.action_table) {
            ImGui::Text("  0x%02X → 0x%02X  %s",
                a.action_id, a.handler_id, a.action_name.c_str());
        }
        ImGui::Separator();
    }

    // Special ability
    if (m_Script.special_ability_api) {
        ImGui::TextColored({ 1.0f,0.5f,0.2f,1.0f },
            "Special ability:  CALLAPI(0x%06X)", m_Script.special_ability_api);
        std::string name = AIScriptParser::ApiMeaning(m_Script.special_ability_api, 1, isComplex);
        if (!name.empty())
            ImGui::TextColored({ 0.8f,0.9f,0.6f,1.0f }, "  %s", name.c_str());
        ImGui::Separator();
    }

    // API summary
    ImGui::Text("API calls:   %zu unique addresses", m_Script.api_catalog.size());
    ImGui::Text("Debug strs:  %zu", m_Script.strings.size());

    // Main loop node
    if (!m_Script.nodes.empty()) {
        auto& ml = m_Script.nodes[m_Script.main_loop_node];
        ImGui::TextColored({ 0.6f,0.9f,1.0f,1.0f },
            "Main loop:   Node %u  (%u bytes, %zu instrs)",
            m_Script.main_loop_node, ml.size, ml.instrs.size());
    }
    ImGui::Separator();

    // Node size distribution
    ImGui::TextColored({ 0.8f,0.8f,0.8f,1.0f }, "Node size distribution:");
    int tiny = 0, small_ = 0, med = 0, large = 0;
    for (auto& n : m_Script.nodes) {
        if (n.size <= 8)  tiny++;
        else if (n.size <= 30) small_++;
        else if (n.size <= 100) med++;
        else                    large++;
    }
    ImGui::Text("  Stubs   (<=  8B): %3d  — JMP redirect / empty", tiny);
    ImGui::Text("  Small   (<= 30B): %3d  — simple condition check", small_);
    ImGui::Text("  Medium  (<=100B): %3d  — behaviour evaluation", med);
    ImGui::TextColored({ 0.6f,0.9f,1.0f,1.0f },
        "  Large   (> 100B): %3d  — complex state logic", large);

    // Complex-file behaviour summary
    if (isComplex && !m_Script.strings.empty()) {
        ImGui::Separator();
        ImGui::TextColored({ 0.8f,0.8f,0.4f,1.0f }, "Detected behaviours:");
        bool hasFlank = false, hasDecide = false, hasHeal = false, hasSpecial = false, hasTurn = false;
        for (auto& s : m_Script.strings) {
            if (s.find("Flank") != std::string::npos || s.find("A-F-") != std::string::npos) hasFlank = true;
            if (s.find("Decide") != std::string::npos) hasDecide = true;
            if (s.find("heal") != std::string::npos || s.find("Heal") != std::string::npos) hasHeal = true;
            if (s.find("special") != std::string::npos || s.find("Special") != std::string::npos) hasSpecial = true;
            if (s.find("Turn") != std::string::npos || s.find("turn") != std::string::npos) hasTurn = true;
        }
        if (hasDecide) ImGui::Text("  ✓  Target selection / DecideToAttack");
        if (hasTurn)   ImGui::Text("  ✓  Turn-to-target logic");
        if (hasFlank)  ImGui::Text("  ✓  Flanking (A-F-L / A-F-R)");
        if (hasHeal)   ImGui::Text("  ✓  Healing behaviour");
        if (hasSpecial)ImGui::Text("  ✓  Special attack sequences");
        if (m_Script.api_catalog.size() > 5)
            ImGui::Text("  ✓  %zu engine API calls (movement, combat, state)",
                m_Script.api_catalog.size());
    }
}

// DISASSEMBLY TAB
void AIScriptViewer::RenderDisassembly() {
    if (m_Script.nodes.empty()) { ImGui::TextDisabled("No nodes decoded."); return; }

    // Colour legend
    ImGui::TextColored({ 0.4f,0.75f,1.0f,1.0f }, "■"); ImGui::SameLine();
    ImGui::TextDisabled("PUSH_ADDR (local)");         ImGui::SameLine(); ImGui::Spacing(); ImGui::SameLine();
    ImGui::TextColored({ 0.3f,1.0f,0.8f,1.0f }, "■"); ImGui::SameLine();
    ImGui::TextDisabled("PUSH_ADDR (shared)");        ImGui::SameLine(); ImGui::Spacing(); ImGui::SameLine();
    ImGui::TextColored({ 1.0f,0.85f,0.2f,1.0f }, "■"); ImGui::SameLine();
    ImGui::TextDisabled("Action register write");     ImGui::SameLine(); ImGui::Spacing(); ImGui::SameLine();
    ImGui::TextColored({ 1.0f,0.5f,1.0f,1.0f }, "■"); ImGui::SameLine();
    ImGui::TextDisabled("SWITCH");
    ImGui::Separator();

    //Left panel: node list
    ImGui::BeginChild("NodeList", ImVec2(190, 0), true);
    ImGui::InputText("##ns", m_NodeSearch, sizeof(m_NodeSearch));
    ImGui::SameLine(); ImGui::TextDisabled("filter");
    ImGui::Separator();

    for (int i = 0; i < (int)m_Script.nodes.size(); i++) {
        auto& n = m_Script.nodes[i];
        char label[56];
        snprintf(label, sizeof(label), "N%-4d %5uB", i, n.size);

        // Filter by search text
        if (m_NodeSearch[0] != '\0') {
            char idx_str[12]; snprintf(idx_str, sizeof(idx_str), "%d", i);
            if (!strstr(label, m_NodeSearch) && !strstr(idx_str, m_NodeSearch))
                continue;
        }

        // Colour code nodes
        ImVec4 col = { 0.65f,0.65f,0.65f,1.0f };
        if (n.is_main_loop)              col = { 0.4f,1.0f,1.0f,1.0f };   // cyan — main loop
        else if (n.has_special_ability)  col = { 1.0f,0.65f,0.2f,1.0f };  // orange — special
        else if (!n.calls_nodes.empty()) col = { 0.55f,0.85f,1.0f,1.0f }; // blue  — caller
        else if (n.size <= 8)           col = { 0.4f,0.4f,0.4f,1.0f };   // grey  — stub

        ImGui::PushStyleColor(ImGuiCol_Text, col);

        char full_label[72];
        const char* badge = n.is_main_loop ? " [MAIN]" :
            n.has_special_ability ? " [SPEC]" : "";
        snprintf(full_label, sizeof(full_label), "%s%s", label, badge);

        if (ImGui::Selectable(full_label, m_SelectedNode == i))
            m_SelectedNode = i;
        ImGui::PopStyleColor();
    }
    ImGui::EndChild();
    ImGui::SameLine();

    //Right panel: instruction listing
    ImGui::BeginChild("InstrList", ImVec2(0, 0), false);

    if (m_SelectedNode < (int)m_Script.nodes.size()) {
        auto& node = m_Script.nodes[m_SelectedNode];

        // Node header
        ImVec4 hdr_col = node.is_main_loop ? ImVec4{ 0.4f,1.0f,1.0f,1.0f } :
            node.has_special_ability ? ImVec4{ 1.0f,0.65f,0.2f,1.0f } :
            ImVec4{ 0.6f,0.9f,1.0f,1.0f };

        ImGui::TextColored(hdr_col,
            "Node %d  [bc:0x%04X  file:0x%05X  size:%uB  instrs:%zu]%s",
            node.index, node.bc_offset, node.file_offset,
            node.size, node.instrs.size(),
            node.is_main_loop ? "  ← MAIN AI LOOP" :
            node.has_special_ability ? "  ← SPECIAL ABILITY" : "");

        if (!node.calls_nodes.empty()) {
            ImGui::TextDisabled("  JMPNODE targets: ");
            ImGui::SameLine();
            for (uint32_t n : node.calls_nodes) {
                ImGui::TextColored({ 0.4f,1.0f,1.0f,1.0f }, "N%u ", n);
                ImGui::SameLine();
            }
            ImGui::NewLine();
        }
        if (!node.api_calls.empty()) {
            ImGui::TextDisabled("  CALLAPI: ");
            ImGui::SameLine();
            for (uint32_t a : node.api_calls) {
                if (a == 0)
                    ImGui::TextColored({ 1.0f,0.7f,0.3f,1.0f }, "execute_action()");
                else
                    ImGui::TextColored({ 1.0f,0.55f,0.15f,1.0f }, "0x%06X", a);
                ImGui::SameLine();
            }
            ImGui::NewLine();
        }
        ImGui::Separator();

        // Instruction table
        if (ImGui::BeginTable("InstrTbl", 4,
            ImGuiTableFlags_BordersInnerV | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {

            ImGui::TableSetupScrollFreeze(0, 1);
            ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 55);
            ImGui::TableSetupColumn("Bytes", ImGuiTableColumnFlags_WidthFixed, 140);
            ImGui::TableSetupColumn("Mnemonic", ImGuiTableColumnFlags_WidthFixed, 100);
            ImGui::TableSetupColumn("Operand", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();

            for (auto& instr : node.instrs) {
                ImGui::TableNextRow();

                // Tint entire row if it's an action-related write
                if (instr.is_action_write)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                        ImGui::GetColorU32(ImVec4{ 0.25f, 0.20f, 0.05f, 1.0f }));
                else if (instr.uses_shared_mem)
                    ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                        ImGui::GetColorU32(ImVec4{ 0.03f, 0.20f, 0.20f, 1.0f }));

                // Offset
                ImGui::TableSetColumnIndex(0);
                ImGui::TextDisabled("%04X", instr.offset);

                // Raw bytes
                ImGui::TableSetColumnIndex(1);
                char raw_str[32] = {};
                for (int j = 0; j < instr.length && j < 8; j++) {
                    char tmp[4]; snprintf(tmp, sizeof(tmp), "%02X ", instr.raw[j]);
                    strncat(raw_str, tmp, sizeof(raw_str) - strlen(raw_str) - 1);
                }
                ImGui::TextDisabled("%s", raw_str);

                // Mnemonic
                ImGui::TableSetColumnIndex(2);
                ImGui::TextColored(MnemColor(instr.mnemonic), "%s", instr.mnemonic.c_str());

                // Operand
                ImGui::TableSetColumnIndex(3);
                if (!instr.operand.empty())
                    ImGui::TextColored(OperandColor(instr), "%s", instr.operand.c_str());
            }
            ImGui::EndTable();
        }
    }
    ImGui::EndChild();
}

// ACTION TABLE TAB
// Decoded from SWITCH #24 in the main AI loop node
void AIScriptViewer::RenderActionTable() {
    ImGui::TextColored({ 1.0f,0.85f,0.3f,1.0f }, "Action Registration Table");
    ImGui::TextDisabled("Decoded from SWITCH #24 in the main AI loop (node %u).",
        m_Script.main_loop_node);
    ImGui::TextDisabled("The VM sets g_action_id (g[F4]) and g_handler_id (g[F0]).");
    ImGui::TextDisabled("The C engine reads these values and dispatches the movement/attack.");
    ImGui::Separator();

    if (m_Script.action_table.empty()) {
        ImGui::TextColored({ 1.0f,0.5f,0.5f,1.0f },
            "No action table found (SWITCH #24 not present in this file).");
        ImGui::TextDisabled("This may be a complex-tier file with a different dispatch mechanism.");
        return;
    }

    // Header info
    ImGui::Text("%zu actions registered", m_Script.action_table.size());
    ImGui::Spacing();

    // Action table
    if (ImGui::BeginTable("ActTbl", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_Resizable)) {

        ImGui::TableSetupColumn("g[F4] (action_id)", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("g[F0] (handler_id)", ImGuiTableColumnFlags_WidthFixed, 130);
        ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("C handler", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (auto& a : m_Script.action_table) {
            ImGui::TableNextRow();

            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored({ 1.0f,0.85f,0.3f,1.0f },
                "0x%02X  (%3u)", a.action_id, a.action_id);

            ImGui::TableSetColumnIndex(1);
            ImGui::TextColored({ 0.7f,0.85f,1.0f,1.0f },
                "0x%02X  (%3u)", a.handler_id, a.handler_id);

            ImGui::TableSetColumnIndex(2);
            // Colour by action category
            ImVec4 acol = { 0.8f,0.8f,0.8f,1.0f };
            if (a.action_id == 0x5E || a.action_id == 0x58 ||
                a.action_id == 0x5A || a.action_id == 0x5C)
                acol = { 1.0f, 0.5f, 0.5f, 1.0f };   // red — attack
            else if (a.action_id == 0x60)
                acol = { 1.0f, 0.4f, 0.2f, 1.0f };   // orange — special
            else if (a.action_id == 0x0A)
                acol = { 0.4f, 1.0f, 0.6f, 1.0f };   // green — movement
            else if (a.action_id == 0x0C)
                acol = { 0.5f, 0.9f, 1.0f, 1.0f };   // cyan — turn
            else if (a.action_id == 0x1E)
                acol = { 0.8f, 0.8f, 0.4f, 1.0f };   // yellow — guard
            ImGui::TextColored(acol, "%s", a.action_name.c_str());

            ImGui::TableSetColumnIndex(3);
            ImGui::TextColored({ 0.55f,0.55f,0.9f,1.0f }, "%s", a.handler_name.c_str());
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextColored({ 0.8f,0.8f,0.8f,1.0f }, "Memory map for action dispatch:");
    ImGui::Spacing();

    // Register explanation table
    if (ImGui::BeginTable("RegTbl", 3,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {

        ImGui::TableSetupColumn("Register", ImGuiTableColumnFlags_WidthFixed, 120);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        struct RegRow { const char* name; const char* addr; const char* role; ImVec4 col; };
        static const RegRow rows[] = {
            {"g_action_id",   "g[F4]", "Current action ID — set by VM, read by C engine (2818 uses total)",  {1.0f,0.85f,0.3f,1.0f}},
            {"g_handler_id",  "g[F0]", "C-side handler index (0x22..0x29) — C engine dispatches via this",   {0.7f,0.85f,1.0f,1.0f}},
            {"g_prev_action", "g[F8]", "Previous action / state flags",                                       {0.8f,0.8f,0.8f,1.0f}},
            {"g_special_slot","g[FC]", "Special ability function pointer — set per-file, exec via CALLAPI(0)",{1.0f,0.6f,0.2f,1.0f}},
            {"g_combat_flags","g[E4]", "Combat state bitmask (SETBIT/CLRBIT)",                                {0.7f,1.0f,0.7f,1.0f}},
            {"g_dist_timer",  "g[E8]", "Distance to target / animation progress",                             {0.7f,1.0f,0.7f,1.0f}},
            {"g_frame_ctr",   "g[D0]", "Frame counter / AI state",                                            {0.8f,0.8f,0.8f,1.0f}},
        };
        for (auto& r : rows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored(r.col, "%s", r.name);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", r.addr);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", r.role);
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextColored({ 0.3f,1.0f,0.8f,1.0f }, "Shared memory (cross-entity coordination):");
    ImGui::Spacing();

    if (ImGui::BeginTable("SharedTbl", 3,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {

        ImGui::TableSetupColumn("Register", ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        struct SharedRow { const char* name; const char* addr; const char* role; };
        static const SharedRow srows[] = {
            {"gs_atk_coord",   "0x08FF...67", "Attack coordination between enemies"},
            {"gs_pos_state",   "0x08FF...68", "Formation position state"},
            {"gs_approach_thr","0x08FF...74", "Approach distance threshold (5.0f)"},
            {"gs_formation_bm","0x08FF...78", "Formation bitmask (SETBIT/CLRBIT)"},
            {"gs_form_slot",   "0x08FF...7C", "Formation slot control"},
        };
        for (auto& r : srows) {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::TextColored({ 0.3f,1.0f,0.8f,1.0f }, "%s", r.name);
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%s", r.addr);
            ImGui::TableSetColumnIndex(2);
            ImGui::TextDisabled("%s", r.role);
        }
        ImGui::EndTable();
    }
}

// API CATALOG TAB
void AIScriptViewer::RenderApiCatalog() {
    bool isComplex = (m_Script.tier == AIScriptTier::Complex);

    if (m_Script.api_catalog.empty()) {
        ImGui::TextDisabled("No API calls found.");
        return;
    }

    ImGui::Text("%zu unique engine API addresses", m_Script.api_catalog.size());

    // Group A / Group B legend
    ImGui::Spacing();
    ImGui::TextColored({ 0.9f,0.6f,0.2f,1.0f },
        "Group A (0x2200–0x2C00):");
    ImGui::SameLine();
    ImGui::TextDisabled(" boss special ability functions (1 per simple file)");

    ImGui::TextColored({ 0.5f,0.9f,1.0f,1.0f },
        "Group B (0x36F4–0x4200):");
    ImGui::SameLine();
    ImGui::TextDisabled(" complex enemy vtables (+0 init, +4 speed, +8 range, +10 seek, +1C state, +3C special)");

    if (m_Script.special_ability_api) {
        ImGui::TextColored({ 1.0f,0.5f,0.2f,1.0f },
            "► This file's special ability: 0x%06X", m_Script.special_ability_api);
    }
    ImGui::Separator();

    if (ImGui::BeginTable("ApiTbl", 5,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Address", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Role", ImGuiTableColumnFlags_WidthFixed, 65);
        ImGui::TableSetupColumn("Calls", ImGuiTableColumnFlags_WidthFixed, 45);
        ImGui::TableSetupColumn("Inferred function name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Nodes", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableHeadersRow();

        for (auto& e : m_Script.api_catalog) {
            ImGui::TableNextRow();

            // Address
            ImGui::TableSetColumnIndex(0);
            ImVec4 addr_col = { 0.7f,0.7f,0.7f,1.0f };
            if (e.address == 0)
                addr_col = { 1.0f,0.8f,0.3f,1.0f };
            else if (e.address >= 0x2200 && e.address <= 0x2C00)
                addr_col = { 1.0f,0.6f,0.2f,1.0f };
            else if (e.address >= 0x3600 && e.address <= 0x4200)
                addr_col = { 0.5f,0.9f,1.0f,1.0f };
            if (e.address == 0)
                ImGui::TextColored(addr_col, "0x000000");
            else
                ImGui::TextColored(addr_col, "0x%06X", e.address);

            // Role badge
            ImGui::TableSetColumnIndex(1);
            if (!e.role.empty()) {
                ImVec4 role_col = { 0.7f,0.7f,0.7f,1.0f };
                if (e.role == "core")    role_col = { 1.0f,0.85f,0.3f,1.0f };
                else if (e.role == "special") role_col = { 1.0f,0.5f,0.2f,1.0f };
                else if (e.role == "seek")    role_col = { 0.4f,1.0f,0.7f,1.0f };
                else if (e.role == "combat")  role_col = { 1.0f,0.5f,0.5f,1.0f };
                else if (e.role == "init")    role_col = { 0.8f,0.8f,0.8f,1.0f };
                else if (e.role == "math")    role_col = { 0.7f,0.9f,0.7f,1.0f };
                ImGui::TextColored(role_col, "[%s]", e.role.c_str());
            }

            // Call count
            ImGui::TableSetColumnIndex(2);
            if (e.call_count >= 20)
                ImGui::TextColored({ 1.0f,0.5f,0.5f,1.0f }, "%d", e.call_count);
            else if (e.call_count >= 8)
                ImGui::TextColored({ 1.0f,0.8f,0.5f,1.0f }, "%d", e.call_count);
            else
                ImGui::Text("%d", e.call_count);

            // Inferred name
            ImGui::TableSetColumnIndex(3);
            if (!e.inferred_name.empty())
                ImGui::TextColored({ 0.8f,0.9f,0.6f,1.0f }, "%s", e.inferred_name.c_str());
            else
                ImGui::TextDisabled("[unknown]");

            // Calling nodes
            ImGui::TableSetColumnIndex(4);
            std::string ns;
            for (size_t i = 0; i < e.calling_nodes.size() && i < 6; i++) {
                if (i) ns += " ";
                ns += "N" + std::to_string(e.calling_nodes[i]);
            }
            if (e.calling_nodes.size() > 6) ns += "…";
            ImGui::TextDisabled("%s", ns.c_str());
        }
        ImGui::EndTable();
    }
}

// CONTROL FLOW TAB
void AIScriptViewer::RenderControlFlow() {
    if (m_Script.nodes.empty()) { ImGui::TextDisabled("No nodes."); return; }

    ImGui::TextColored({ 0.7f,0.9f,1.0f,1.0f },
        "Node call graph  (%u nodes)", m_Script.node_count);
    ImGui::TextDisabled("Shows JMPNODE and CALLAPI references per node.");

    // Summary
    int n_caller = 0, n_api = 0, n_special = 0, n_stub = 0;
    for (auto& n : m_Script.nodes) {
        if (!n.calls_nodes.empty()) n_caller++;
        if (!n.api_calls.empty())   n_api++;
        if (n.has_special_ability)  n_special++;
        if (n.size <= 8)            n_stub++;
    }
    ImGui::Text("  Callers: %d  |  API users: %d  |  Special ability nodes: %d  |  Stubs: %d",
        n_caller, n_api, n_special, n_stub);
    ImGui::Separator();

    if (ImGui::BeginTable("CFG", 4,
        ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
        ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {

        ImGui::TableSetupScrollFreeze(0, 1);
        ImGui::TableSetupColumn("Node", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 55);
        ImGui::TableSetupColumn("Calls nodes", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("CALLAPI", ImGuiTableColumnFlags_WidthFixed, 240);
        ImGui::TableHeadersRow();

        for (auto& node : m_Script.nodes) {
            if (node.calls_nodes.empty() && node.api_calls.empty()) continue;

            ImGui::TableNextRow();

            // Row background for main loop / special
            if (node.is_main_loop)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                    ImGui::GetColorU32(ImVec4{ 0.05f,0.18f,0.20f,1.0f }));
            else if (node.has_special_ability)
                ImGui::TableSetBgColor(ImGuiTableBgTarget_RowBg0,
                    ImGui::GetColorU32(ImVec4{ 0.20f,0.12f,0.03f,1.0f }));

            // Node index
            ImGui::TableSetColumnIndex(0);
            ImVec4 ncol = node.is_main_loop ? ImVec4{ 0.4f,1.0f,1.0f,1.0f } :
                node.has_special_ability ? ImVec4{ 1.0f,0.65f,0.2f,1.0f } :
                ImVec4{ 0.55f,0.85f,1.0f,1.0f };
            ImGui::TextColored(ncol, "N%u%s", node.index,
                node.is_main_loop ? "*" :
                node.has_special_ability ? "!" : "");

            // Size
            ImGui::TableSetColumnIndex(1);
            ImGui::TextDisabled("%uB", node.size);

            // JMPNODE calls
            ImGui::TableSetColumnIndex(2);
            if (node.calls_nodes.empty()) {
                ImGui::TextDisabled("—");
            }
            else {
                std::string s;
                for (size_t i = 0; i < node.calls_nodes.size() && i < 8; i++) {
                    if (i) s += " ";
                    s += "N" + std::to_string(node.calls_nodes[i]);
                }
                if (node.calls_nodes.size() > 8) s += "…";
                ImGui::TextColored({ 0.4f,1.0f,1.0f,1.0f }, "%s", s.c_str());
            }

            // CALLAPI
            ImGui::TableSetColumnIndex(3);
            if (!node.api_calls.empty()) {
                for (uint32_t a : node.api_calls) {
                    if (a == 0)
                        ImGui::TextColored({ 1.0f,0.85f,0.3f,1.0f }, "exec_action() ");
                    else {
                        ImGui::TextColored({ 1.0f,0.55f,0.15f,1.0f }, "0x%06X ", a);
                    }
                    ImGui::SameLine();
                }
                ImGui::NewLine();
            }
        }
        ImGui::EndTable();
    }

    ImGui::Spacing();
    ImGui::TextDisabled("* = main AI loop    ! = special ability node");
}

// STRINGS TAB (complex files only)
void AIScriptViewer::RenderStrings() {
    if (m_Script.strings.empty()) {
        ImGui::TextDisabled("No debug strings embedded in this file.");
        return;
    }

    ImGui::Text("%zu debug/trace strings", m_Script.strings.size());
    ImGui::TextDisabled("printf-style debug strings embedded in the compiled behaviour script.");
    ImGui::Separator();

    ImGui::BeginChild("StrList", ImVec2(0, 0), false);
    for (size_t i = 0; i < m_Script.strings.size(); i++) {
        const auto& s = m_Script.strings[i];

        ImVec4 col = { 0.7f,0.7f,0.7f,1.0f };
        if (s[0] == '%')                                               col = { 0.6f,0.6f,1.0f,1.0f };
        else if (s.find("Check") != std::string::npos ||
            s.find("Decide") != std::string::npos ||
            s.find("Action") != std::string::npos ||
            s.find("Turn") != std::string::npos ||
            s.find("Flank") != std::string::npos ||
            s.find("A-F-") != std::string::npos)                    col = { 1.0f,0.85f,0.3f,1.0f };
        else if (s.find("ERROR") != std::string::npos ||
            s.find("failed") != std::string::npos ||
            s.find("Jammed") != std::string::npos)                  col = { 1.0f,0.4f,0.4f,1.0f };
        else if (s.find("OK") != std::string::npos ||
            s.find("done") != std::string::npos ||
            s.find("rdy") != std::string::npos ||
            s.find("true") != std::string::npos)                    col = { 0.4f,1.0f,0.5f,1.0f };
        else if (s.find("ver ") != std::string::npos ||
            s.find("200") != std::string::npos)                     col = { 0.5f,0.5f,0.5f,1.0f };
        else if (s.find("special") != std::string::npos ||
            s.find("Special") != std::string::npos)                 col = { 1.0f,0.6f,0.3f,1.0f };

        ImGui::TextColored(col, "[%2zu] %s", i, s.c_str());
    }
    ImGui::EndChild();
}