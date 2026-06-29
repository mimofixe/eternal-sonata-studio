#include "Cutscenescriptparser.h"
#include <fstream>
#include <cstring>
#include <cstdio>
#include <cstdlib>
#include <cctype>
#include <cmath>
#include <algorithm>

// VM width table (extracted from the eboot, jump table 0x3af380).
// 0x7a CALLNODE carries a 4-byte operand: its handler sets the PC directly, so the
// addi-based width extraction missed it. Verified against real bytecode + footer.
static const uint8_t CS_OP_W[256] = {
    1, 2, 3, 5, 9, 2, 3, 5, 5, 2, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 5, 2, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2,
    2, 2, 2, 2, 5, 2, 2, 2, 2, 2, 2, 2, 5, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 2, 1, 5, 1, 1, 5, 1, 5,
    2, 1, 1, 5, 2, 1, 1, 5, 2, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

// Known time values (transition frames)
static bool IsDurationValue(uint16_t v) {
    switch (v) {
    case 300: case 500: case 600: case 800:
    case 900: case 1000: case 1200: case 1500:
        return true;
    }
    return false;
}

uint32_t CutsceneScriptParser::ReadU32BE(const uint8_t* d) {
    return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) |
        ((uint32_t)d[2] << 8) | d[3];
}

float CutsceneScriptParser::ReadF32BE(const uint8_t* d) {
    uint32_t v = ReadU32BE(d);
    float f; memcpy(&f, &v, 4);
    return f;
}

std::string CutsceneScriptParser::CommandName(uint32_t a) {
    switch (a) {
    case 0x526af0: return "CAMERA";
    case 0x5260a0: return "TRANSFORM_ATOR";
    case 0x525d60: return "SETTER";
    case 0x526130: return "LOAD_MAPA";
    case 0x526100: return "CMD_526100";
    case 0x526098: return "FLAG";
    case 0x525c08: return "INIT_SLOT";
    case 0x525c10: return "ACTOR_TABLE";
    case 0x525b90: return "STATE_MACHINE";
    case 0x5261c8: return "CMD_5261c8";
    case 0x527558: return "ALLOC";
    case 0x526a88: return "CMD_526a88";
    default:       return "";
    }
}

// Master-library function names (relocated CALLNODE targets; empirically validated)
static std::string LibName(uint32_t off) {
    switch (off) {
    case 0x0F: return "SET_POSITION";
    case 0x10: return "SET_ANGLES";
    case 0x14: return "SET_TARGET";
    default: {
        char b[24];
        snprintf(b, sizeof(b), "LIB_0x%X", off);
        return b;
    }
    }
}

std::string CutsceneScriptParser::CommandNameById(uint32_t id) {
    // IDs resolved from the eboot category tables (api_id_table.json has all 642).
    switch (id) {
    case 1002: return "LOAD_MAPA";
    case 1010: return "SETTER";
    case 1011: return "CMD_526100";
    case 1031: return "FLAG";
    case 1032: return "FLAG2";
    case 1037: return "TRANSFORM_ATOR";
    case 1120: return "CMD_526a88";
    case 1121: return "CAMERA";
    case 1135: return "CMD_5261c8";
    case 5006: return "ACTOR_TABLE";
    case 5007: return "INIT_SLOT";
    case 5027: return "STATE_MACHINE";
    default: {
        char b[24];
        snprintf(b, sizeof(b), "API_%u", id);
        return b;
    }
    }
}

bool CutsceneScriptParser::IsCutscene(const uint8_t* data, size_t size) {
    if (size < 0x18) return false;
    return ReadU32BE(data) == CUTSCENE_MAGIC;
}

// FOOTER relocation table. Read u32 pairs from the END of the file backwards
// (after skipping trailing zero u32s). Each pair links an operand SITE
// (FILE-relative; classified by the opcode byte right before it) with a VALUE.
// Orientation varies per pair, so classify each side by ground truth.
void CutsceneScriptParser::ParseRelocations(CutsceneScript& s, const std::vector<uint8_t>& d) {
    const size_t code_end = s.code_offset + s.bytecode.size();

    // Classify a u32 as an operand site: returns the opcode (0x7a/0x7d/0x07) or 0.
    auto classify = [&](uint32_t off) -> uint8_t {
        if (off < 1 || off + 4 > d.size()) return 0;
        if (off > code_end) return 0;                 // sites live in the bytecode
        uint8_t op = d[off - 1];
        bool zeroed = (off + 4 <= d.size()) &&
            d[off] == 0 && d[off + 1] == 0 && d[off + 2] == 0 && d[off + 3] == 0;
        if (op == 0x7d && zeroed) return 0x7d;
        if (op == 0x7a && zeroed) return 0x7a;
        if (op == 0x07) return 0x07;
        return 0;
        };

    // Skip trailing zero u32s
    size_t end = d.size() & ~3u;
    while (end >= 4 && ReadU32BE(&d[end - 4]) == 0) end -= 4;

    // Walk back in 8-byte steps; tolerate a few unclassifiable pairs, stop after 4.
    // The reloc table stores (site, value) pairs sorted by value, BUT stray odd
    // words between value-groups flip the apparent pairing phase mid-table (the
    // root cause of both the phantom-id incident and the unresolved camera-param
    // APIs). Robust extraction: slide through the footer region in 4-byte steps,
    // test BOTH orientations of each candidate pair, and accept the one whose
    // "site" classifies (strong 0x7a/0x7d beats weak 0x07); advance 8 on accept,
    // 4 otherwise. The region starts at field_14 (the footer pointer) when valid.
    {
        auto strength = [](uint8_t k) -> int {
            if (k == 0x7d || k == 0x7a) return 2;
            if (k == 0x07) return 1;
            return 0;
            };
        size_t lo = code_end;
        if (s.field_14 > code_end && s.field_14 + 8 < d.size()) lo = s.field_14;
        size_t pos = lo & ~3u;
        while (pos + 8 <= end) {
            uint32_t a = ReadU32BE(&d[pos]);
            uint32_t b = ReadU32BE(&d[pos + 4]);
            uint8_t ca = classify(a), cb = classify(b);
            int sa = strength(ca), sb = strength(cb);
            uint32_t site = 0, value = 0; uint8_t kind = 0;
            if (sa > sb) { site = a; value = b; kind = ca; }
            else if (sb > sa) { site = b; value = a; kind = cb; }
            if (kind && site > s.code_offset) {
                uint32_t rel = site - (uint32_t)s.code_offset;
                if (kind == 0x7d)      s.api_by_site[rel] = value;
                else if (kind == 0x7a) s.node_by_site[rel] = value;
                pos += 8;
            }
            else {
                pos += 4;
            }
        }
    }
}

int CutsceneScriptParser::DisasmOne(const CutsceneScript& s, uint32_t pos, CSInstr& out) {
    const std::vector<uint8_t>& bc = s.bytecode;
    const int N = (int)bc.size();
    auto safe = [&](int off) -> uint8_t {
        int idx = (int)pos + off;
        return (idx >= 0 && idx < N) ? bc[idx] : 0;
        };
    uint8_t op = safe(0);
    out.offset = pos;
    out.op = op;
    out.is_callapi = false;
    out.is_call = false;
    out.raw_u32 = 0;
    out.api_id = 0;
    out.call_target = 0;

    if (op > 0x89) {
        char b[16]; snprintf(b, sizeof(b), "BAD_%02X", op);
        out.mnemonic = b; out.operand = ""; out.length = 1;
        return 1;
    }

    int len = CS_OP_W[op];
    out.length = len;

    char ob[64];
    switch (op) {
    case 0x00: out.mnemonic = "NOP";   out.operand = ""; break;
    case 0x01: out.mnemonic = "LDI8";  snprintf(ob, sizeof(ob), "#%u", safe(1)); out.operand = ob; break;
    case 0x02: {
        uint16_t v = ((uint16_t)safe(1) << 8) | safe(2);
        out.mnemonic = "LDID";
        if (v == 0x0c40)               snprintf(ob, sizeof(ob), "TARGET");
        else if ((v & 0xff00) == 0x0c00) snprintf(ob, sizeof(ob), "actor 0x%04X", v);
        else if (IsDurationValue(v))   snprintf(ob, sizeof(ob), "dur %u", v);
        else                           snprintf(ob, sizeof(ob), "#0x%04X", v);
        out.operand = ob;
        break;
    }
    case 0x03: {
        float f = (pos + 5 <= (uint32_t)N) ? ReadF32BE(&bc[pos + 1]) : 0.0f;
        out.mnemonic = "PUSHF";
        snprintf(ob, sizeof(ob), "%g", f); out.operand = ob;
        out.raw_u32 = (pos + 5 <= (uint32_t)N) ? ReadU32BE(&bc[pos + 1]) : 0;
        break;
    }
    case 0x07: {
        uint32_t v = (pos + 5 <= (uint32_t)N) ? ReadU32BE(&bc[pos + 1]) : 0;
        out.mnemonic = "PUSHOFF";
        out.raw_u32 = v;
        auto it = s.data_by_site.find(pos + 1);
        if (it != s.data_by_site.end())
            snprintf(ob, sizeof(ob), "0x%X (ref 0x%X)", v, it->second);
        else
            snprintf(ob, sizeof(ob), "0x%X", v);
        out.operand = ob;
        break;
    }
    case 0x18: {
        int8_t sb = (int8_t)safe(1);
        out.mnemonic = "LDLOCAL";
        snprintf(ob, sizeof(ob), "local[%+d]", (int)sb); out.operand = ob;
        break;
    }
    case 0x28: out.mnemonic = "I2F"; out.operand = ""; break;
    case 0x3d: out.mnemonic = "DIV"; out.operand = ""; break;
    case 0x7a: {
        // CALLNODE has two flavours, distinguished by the on-disk operand:
        //   inline (non-zero)  = LOCAL node call (bytecode offset in this file)
        //   relocated (zeroed) = call into the MASTER script's function LIBRARY
        //     (footer value = offset in the master script; the loader patches the
        //     operand with master_base + offset - seen live as 0x307aXXXX)
        uint32_t v = (pos + 5 <= (uint32_t)N) ? ReadU32BE(&bc[pos + 1]) : 0;
        out.mnemonic = "CALLNODE";
        out.is_call = true;
        out.raw_u32 = v;
        if (v) {
            out.call_target = v;
            snprintf(ob, sizeof(ob), "-> 0x%X", v);
        }
        else {
            auto it = s.node_by_site.find(pos + 1);
            if (it != s.node_by_site.end()) {
                out.api_id = it->second;          // library offset
                out.mnemonic = "CALLLIB";
                std::string nm = LibName(it->second);
                snprintf(ob, sizeof(ob), "%s", nm.c_str());
            }
            else {
                snprintf(ob, sizeof(ob), "<unresolved>");
            }
        }
        out.operand = ob;
        break;
    }
    case 0x7c: out.mnemonic = "RET"; out.operand = ""; break;
    case 0x7d: {
        uint32_t v = (pos + 5 <= (uint32_t)N) ? ReadU32BE(&bc[pos + 1]) : 0;
        out.mnemonic = "CALLAPI";
        out.is_callapi = true; out.raw_u32 = v;
        std::string nm;
        if (v) {
            // relocated memory dump: operand holds the descriptor address
            nm = CommandName(v);
            if (nm.empty()) snprintf(ob, sizeof(ob), "0x%X", v);
            else            snprintf(ob, sizeof(ob), "%s", nm.c_str());
        }
        else {
            auto it = s.api_by_site.find(pos + 1);
            if (it != s.api_by_site.end()) {
                out.api_id = it->second;
                nm = CommandNameById(out.api_id);
                snprintf(ob, sizeof(ob), "%s", nm.c_str());
            }
            else {
                snprintf(ob, sizeof(ob), "<reloc>");
            }
        }
        out.operand = ob;
        break;
    }
    case 0x81: out.mnemonic = "PUSH"; out.operand = ""; break;
    case 0x88: out.mnemonic = "POPN"; snprintf(ob, sizeof(ob), "#%u", safe(1)); out.operand = ob; break;
    default: {
        char b[16];
        const char* fam = (op <= 0x1b) ? "LD" : (op >= 0x4b && op <= 0x73) ? "ST" : "OP";
        snprintf(b, sizeof(b), "%s%02X", fam, op);
        out.mnemonic = b;
        if (len == 5) { uint32_t v = (pos + 5 <= (uint32_t)N) ? ReadU32BE(&bc[pos + 1]) : 0; snprintf(ob, sizeof(ob), "0x%X", v); out.operand = ob; out.raw_u32 = v; }
        else if (len == 3) { uint16_t v = ((uint16_t)safe(1) << 8) | safe(2); snprintf(ob, sizeof(ob), "#0x%04X", v); out.operand = ob; }
        else if (len == 2) { snprintf(ob, sizeof(ob), "#%u", safe(1)); out.operand = ob; }
        else out.operand = "";
        break;
    }
    }
    return len;
}

void CutsceneScriptParser::DisasmNode(CutsceneScript& s, CSNode& node) {
    uint32_t pos = node.bc_offset;
    uint32_t end = node.bc_offset + node.size;
    if (end > s.bytecode.size()) end = (uint32_t)s.bytecode.size();
    while (pos < end) {
        CSInstr ins;
        int len = DisasmOne(s, pos, ins);
        if (ins.is_call && ins.call_target) node.calls.push_back(ins.call_target);
        node.instrs.push_back(ins);
        pos += (len > 0 ? len : 1);
    }
}

// Reconstructs scene commands by walking the instructions with a symbolic stack.
void CutsceneScriptParser::BuildCommands(CutsceneScript& s, CSNode& node) {
    std::vector<CSArg> stack;
    for (const CSInstr& ins : node.instrs) {
        switch (ins.op) {
        case 0x03: {
            CSArg a; a.kind = CSArgKind::Float;
            uint32_t v = ins.raw_u32; memcpy(&a.f, &v, 4);
            char b[24]; snprintf(b, sizeof(b), "%g", a.f); a.text = b; stack.push_back(a); break;
        }
        case 0x07: { CSArg a; a.kind = CSArgKind::Offset; a.u = ins.raw_u32; char b[24]; snprintf(b, sizeof(b), "@0x%X", a.u); a.text = b; stack.push_back(a); break; }
        case 0x01: { CSArg a; a.kind = CSArgKind::Imm; a.u = (uint8_t)atoi(ins.operand.c_str() + 1); a.text = ins.operand; stack.push_back(a); break; }
        case 0x02: {
            uint16_t v = 0;
            if (ins.offset + 3 <= s.bytecode.size())
                v = ((uint16_t)s.bytecode[ins.offset + 1] << 8) | s.bytecode[ins.offset + 2];
            CSArg a; a.u = v;
            if (v == 0x0c40) { a.kind = CSArgKind::Target; a.text = "TARGET"; }
            else if ((v & 0xff00) == 0x0c00) { a.kind = CSArgKind::ActorId; char b[16]; snprintf(b, sizeof(b), "actor_0x%04X", v); a.text = b; }
            else if (IsDurationValue(v)) { a.kind = CSArgKind::Duration; char b[16]; snprintf(b, sizeof(b), "dur=%u", v); a.text = b; }
            else { a.kind = CSArgKind::Imm; char b[16]; snprintf(b, sizeof(b), "#0x%04X", v); a.text = b; }
            stack.push_back(a);
            break;
        }
        case 0x81:
            // PUSH confirms the value pushed by the previous opcode; not a new arg.
            break;
        case 0x7d: {
            CSCommand cmd;
            cmd.offset = ins.offset;
            cmd.api_addr = ins.raw_u32;
            cmd.api_id = ins.api_id;
            if (ins.raw_u32) {
                std::string nm = CommandName(ins.raw_u32);
                if (nm.empty()) { char b[24]; snprintf(b, sizeof(b), "CALLAPI@0x%X", ins.raw_u32); cmd.name = b; }
                else cmd.name = nm;
            }
            else if (ins.api_id) {
                cmd.name = CommandNameById(ins.api_id);
            }
            else {
                cmd.name = "CALLAPI<reloc>";
            }
            cmd.args = stack;
            std::string sum = cmd.name + "(";
            for (size_t i = 0; i < cmd.args.size(); i++) { if (i)sum += ", "; sum += cmd.args[i].text; }
            sum += ")";
            cmd.summary = sum;
            node.commands.push_back(cmd);
            s.all_commands.push_back(cmd);
            stack.clear();
            break;
        }
        case 0x7a: {
            if (ins.mnemonic == "CALLLIB") {
                // Master-library call: a scene command like CALLAPI
                CSCommand cmd;
                cmd.offset = ins.offset;
                cmd.is_lib = true;
                cmd.api_id = ins.api_id;
                cmd.name = ins.operand;
                cmd.args = stack;
                std::string sum = cmd.name + "(";
                for (size_t i = 0; i < cmd.args.size(); i++) { if (i)sum += ", "; sum += cmd.args[i].text; }
                sum += ")";
                cmd.summary = sum;
                node.commands.push_back(cmd);
                s.all_commands.push_back(cmd);
            }
            stack.clear();  // local node calls also consume their args
            break;
        }
        case 0x88: // POPN: clears the stack
            stack.clear();
            break;
        case 0x7c: // RET
            stack.clear();
            break;
        default:
            break;
        }
    }
}

// Node boundaries from CALLNODE targets (inline + footer-relocated) and offset 0.
void CutsceneScriptParser::BuildNodes(CutsceneScript& s) {
    std::vector<uint32_t> bounds;
    bounds.push_back(0);
    // ONLY inline CALLNODE targets are local node boundaries; relocated CALLNODEs
    // are master-library calls (their footer values are master-script offsets).
    {
        uint32_t pos = 0;
        const std::vector<uint8_t>& bc = s.bytecode;
        while (pos < bc.size()) {
            uint8_t op = bc[pos];
            if (op > 0x89) { pos++; continue; }
            int len = CS_OP_W[op];
            if (op == 0x7a && pos + 5 <= bc.size()) {
                uint32_t v = ReadU32BE(&bc[pos + 1]);
                if (v && v < bc.size()) bounds.push_back(v);
            }
            pos += (len > 0 ? len : 1);
        }
    }
    std::sort(bounds.begin(), bounds.end());
    bounds.erase(std::unique(bounds.begin(), bounds.end()), bounds.end());

    for (size_t i = 0; i < bounds.size(); i++) {
        CSNode node;
        node.index = (uint32_t)i;
        node.bc_offset = bounds[i];
        uint32_t next = (i + 1 < bounds.size()) ? bounds[i + 1] : (uint32_t)s.bytecode.size();
        node.size = next - bounds[i];
        DisasmNode(s, node);
        BuildCommands(s, node);
        s.nodes.push_back(node);
    }
}

void CutsceneScriptParser::ScanActorsAndData(CutsceneScript& s) {
    uint32_t pos = 0;
    const std::vector<uint8_t>& bc = s.bytecode;
    while (pos < bc.size()) {
        uint8_t op = bc[pos];
        if (op > 0x89) { pos++; continue; }
        int len = CS_OP_W[op];
        if (op == 0x02 && pos + 3 <= bc.size()) {
            uint16_t v = ((uint16_t)bc[pos + 1] << 8) | bc[pos + 2];
            if ((v & 0xff00) == 0x0c00) s.actor_ids[v]++;
        }
        else if (op == 0x07 && pos + 5 <= bc.size()) {
            uint32_t v = ReadU32BE(&bc[pos + 1]);
            if (v > 0x100 && v < s.file_size) s.data_offsets.push_back(v);
        }
        pos += (len > 0 ? len : 1);
    }
    std::sort(s.data_offsets.begin(), s.data_offsets.end());
    s.data_offsets.erase(std::unique(s.data_offsets.begin(), s.data_offsets.end()), s.data_offsets.end());
}

void CutsceneScriptParser::ScanEmbeddedAnims(CutsceneScript& s, const std::vector<uint8_t>& fd) {
    // Look for NMTN chunks and read the name at +0x10 (reveals the scene's actors)
    const uint8_t mn[4] = { 'N','M','T','N' };
    for (size_t i = 0; i + 0x20 < fd.size(); i++) {
        if (fd[i] == mn[0] && fd[i + 1] == mn[1] && fd[i + 2] == mn[2] && fd[i + 3] == mn[3]) {
            std::string nm;
            for (size_t j = i + 0x10; j < i + 0x20 && fd[j]; j++) {
                char c = (char)fd[j];
                if (c >= 32 && c < 127) nm += c; else break;
            }
            if (nm.size() >= 3) s.embedded_anims.push_back(nm);
        }
    }
}

// Self-contained cutscenes (e.g. e0005, the opening) carry their own scene set and
// actor models as NMDL chunks (etc999_v1, npcSLF_v1, ...). List them; nested NMDLs
// (sub-models within a parent's size range) are flagged so the top-level ones stand out.
void CutsceneScriptParser::ScanEmbeddedModels(CutsceneScript& s, const std::vector<uint8_t>& fd) {
    const uint8_t mn[4] = { 'N','M','D','L' };
    for (size_t i = 0; i + 0x20 < fd.size(); i++) {
        if (fd[i] == mn[0] && fd[i + 1] == mn[1] && fd[i + 2] == mn[2] && fd[i + 3] == mn[3]) {
            uint32_t sz = ReadU32BE(&fd[i + 4]);
            if (sz < 0x20 || i + sz > fd.size()) continue;
            std::string nm;
            for (size_t j = i + 0x10; j < i + 0x20 && fd[j]; j++) {
                char c = (char)fd[j];
                if (c >= 32 && c < 127) nm += c; else break;
            }
            if (nm.empty()) continue;
            CutsceneScript::CSEmbeddedModel m;
            m.name = nm;
            m.offset = (uint32_t)i;
            m.size = sz;
            m.top_level = true;
            for (const auto& prev : s.embedded_models)
                if (i > prev.offset && i < (size_t)prev.offset + prev.size) { m.top_level = false; break; }
            s.embedded_models.push_back(m);
        }
    }
}

// ------------------------------------------------------------------------- //
// Narrative slideshow (Chopin-type) support. Additive: these only populate the
// narrative fields and never touch the 3D-cutscene data.
// ------------------------------------------------------------------------- //

void CutsceneScriptParser::ClassifyEventKind(CutsceneScript& s,
    const std::vector<uint8_t>& fd) {
    // Heuristics from the RE: a Narrative slideshow has Mefc containers holding NTX3
    // images, id-1028 image-switches, 0x012C timers, and no 3D geometry/CSL. A title
    // card has a tiny bytecode plus text sprites and no Mefc/CSL. Otherwise it is a
    // 3D cutscene.
    auto count_tag = [&](const char* tag) {
        int n = 0;
        for (size_t i = 0; i + 4 <= fd.size(); i++)
            if (memcmp(&fd[i], tag, 4) == 0) n++;
        return n;
        };
    int mefc = count_tag("Mefc");
    int ntx3 = count_tag("NTX3");
    int csl = count_tag("CSL ");
    int nmtn = count_tag("NMTN");
    int nmdl = count_tag("NMDL");
    int nshp = count_tag("NSHP");
    bool has_3d_geometry = (nmtn > 0 || nmdl > 0 || nshp > 0);

    int id1028 = 0, timer012c = 0;
    const std::vector<uint8_t>& bc = s.bytecode;
    for (size_t i = 0; i + 4 <= bc.size(); i++) {
        // PUSH.W 0x012C (timer slot): 81 02 01 2c
        if (bc[i] == 0x81 && bc[i + 1] == 0x02 && bc[i + 2] == 0x01 && bc[i + 3] == 0x2C)
            timer012c++;
    }
    for (auto& kv : s.api_usage) if (kv.first == 1028) id1028 = kv.second;

    // Order matters: a title card has a tiny bytecode and no real slideshow logic
    // (no image-switch hand-offs), even though it carries Mefc text sprites. A
    // narrative slideshow has full-screen images (Mefc + NTX3) AND no embedded 3D
    // geometry (NMDL/NSHP/NMTN). If the file embeds geometry, it is a 3D cutscene
    // even when it also has Mefc/NTX3 (e.g. e9031: NMDL+NSHP present, no animations).
    if (s.bytecode.size() < 0x200 && csl == 0 && !has_3d_geometry && id1028 == 0) {
        s.kind = CutsceneScript::EventKind::TitleCard;
    }
    else if (mefc > 0 && ntx3 > 0 && !has_3d_geometry &&
        (id1028 > 0 || timer012c > 0)) {
        s.kind = CutsceneScript::EventKind::Narrative;
    }
    else {
        s.kind = CutsceneScript::EventKind::Cutscene3D;
    }
}

void CutsceneScriptParser::ScanSlideImages(CutsceneScript& s,
    const std::vector<uint8_t>& fd) {
    // Every NTX3 chunk is one slide image. Header (project format): format @0x18,
    // mips @0x19, width @0x20 (BE u16), height @0x22 (BE u16), size @0x04.
    for (size_t i = 0; i + 0x24 <= fd.size(); i++) {
        if (memcmp(&fd[i], "NTX3", 4) != 0) continue;
        CutsceneScript::CSSlideImage img;
        img.offset = (uint32_t)i;
        img.data_size = ReadU32BE(&fd[i + 4]);
        img.format = fd[i + 0x18];
        img.mips = fd[i + 0x19];
        img.width = (uint16_t)((fd[i + 0x20] << 8) | fd[i + 0x21]);
        img.height = (uint16_t)((fd[i + 0x22] << 8) | fd[i + 0x23]);
        // the small localized title words are ~256x64 strips
        img.is_title_strip = (img.width <= 512 && img.height <= 128);
        s.slide_images.push_back(img);
    }
}

// Parse one language block [block_start, block_end) of tagged subtitle text into
// lines. Grammar: <dNN><vNN><u>[<f1>]<g> TEXT [<f0>]<wNNNN>. Glyph refs <#NNN> are
// converted to their character; the block stores each line once (the USA block
// repeats its lines for GRB, so we stop at the first repeat).
static void ParseLanguageBlock(const std::string& all, size_t block_start,
    size_t block_end,
    std::vector<CutsceneScript::CSSubtitleLine>& out) {
    size_t p = block_start;
    while (p < block_end) {
        size_t d = all.find("<d", p);
        if (d == std::string::npos || d >= block_end) break;
        size_t close = all.find('>', d);
        if (close == std::string::npos) break;
        CutsceneScript::CSSubtitleLine line;
        line.line_type = atoi(all.substr(d + 2, close - d - 2).c_str());
        size_t q = close + 1;
        if (all.compare(q, 2, "<v") == 0) {
            size_t vc = all.find('>', q);
            if (vc != std::string::npos) {
                line.voice_index = atoi(all.substr(q + 2, vc - q - 2).c_str());
                q = vc + 1;
            }
        }
        size_t g = all.find("<g>", q);
        if (g == std::string::npos || g >= block_end) { p = close + 1; continue; }
        if (all.find("<f1>", q) < g) line.fade = true;
        size_t tstart = g + 3;
        size_t wtag = all.find("<w", tstart);
        if (wtag == std::string::npos || wtag >= block_end) { p = close + 1; continue; }
        size_t tend = all.find("<f0>", tstart);
        if (tend == std::string::npos || tend > wtag) tend = wtag;
        std::string text = all.substr(tstart, tend - tstart);
        std::string clean;
        for (size_t k = 0; k < text.size(); k++) {
            if (text[k] == '<') {
                size_t e = text.find('>', k);
                if (e != std::string::npos) {
                    if (text[k + 1] == '#') {
                        int code = atoi(text.substr(k + 2, e - k - 2).c_str());
                        if (code >= 32 && code < 256) clean += (char)code;
                    }
                    k = e;
                    continue;
                }
            }
            if (text[k] == '\\' && k + 1 < text.size() && text[k + 1] == 'n') { clean += ' '; k++; continue; }
            clean += text[k];
        }
        line.text = clean;
        size_t wc = all.find('>', wtag);
        if (wc != std::string::npos)
            line.duration_s = atoi(all.substr(wtag + 2, wc - wtag - 2).c_str()) / 1000.0f;
        if (!line.text.empty() || line.line_type == 48) {
            if (!out.empty() && line.text == out.front().text && out.size() > 1)
                break;  // start of a repeated copy (USA block repeats for GRB)
            out.push_back(line);
        }
        p = (wc != std::string::npos) ? wc + 1 : close + 1;
        if (out.size() > 256) break;
    }
}

void CutsceneScriptParser::ScanSubtitleTimeline(CutsceneScript& s,
    const std::vector<uint8_t>& fd) {
    // The subtitle section lives near the end of the file, split into per-language
    // blocks, each preceded by a 3-letter marker. We parse EVERY available language
    // so the UI can switch between them, then select the default (USA / first).
    std::string all((const char*)fd.data(), fd.size());

    // Language codes the game uses, in MLNG order.
    static const char* LANGS[] = { "JPN", "USA", "GRB", "FRA", "ITA", "DEU", "ESP" };

    // Restrict the search to the tail region (near the GBR index table) so we do not
    // match these 3-letter sequences inside binary data earlier in the file.
    size_t gbr = all.rfind("GBR ");
    size_t zone = (gbr != std::string::npos && gbr > 0x8000) ? gbr - 0x8000 : 0;

    // Find each language marker in the tail.
    std::vector<std::pair<size_t, std::string>> markers;
    for (const char* code : LANGS) {
        size_t pos = all.find(code, zone);
        // a real marker is followed by a space or NUL (then tag data)
        while (pos != std::string::npos) {
            char after = (pos + 3 < all.size()) ? all[pos + 3] : 0;
            if (after == ' ' || after == 0) { markers.push_back({ pos, code }); break; }
            pos = all.find(code, pos + 1);
        }
    }
    std::sort(markers.begin(), markers.end());

    // Parse each block [marker, next marker).
    for (size_t i = 0; i < markers.size(); i++) {
        size_t bstart = markers[i].first + 3;
        size_t bend = (i + 1 < markers.size()) ? markers[i + 1].first : all.size();
        CutsceneScript::CSLanguageBlock block;
        block.code = markers[i].second;
        block.offset = (uint32_t)markers[i].first;
        ParseLanguageBlock(all, bstart, bend, block.lines);
        if (!block.lines.empty()) s.languages.push_back(block);
    }

    // Default selection: USA if present, else the first language.
    int def = -1;
    for (size_t i = 0; i < s.languages.size(); i++)
        if (s.languages[i].code == "USA") { def = (int)i; break; }
    if (def < 0 && !s.languages.empty()) def = 0;
    SelectLanguage(s, def);
}

bool CutsceneScriptParser::SelectLanguage(CutsceneScript& s, int language_index) {
    if (language_index < 0 || language_index >= (int)s.languages.size()) return false;
    s.selected_language = language_index;
    s.subtitle_lines = s.languages[language_index].lines;
    return true;
}

std::vector<uint32_t> CutsceneScriptParser::FindCallApiSites(const CutsceneScript& s,
    const std::vector<uint8_t>& fd, uint32_t api_id) {
    std::vector<uint32_t> sites;
    const std::vector<uint8_t>& bc = s.bytecode;
    uint32_t code_off = (uint32_t)s.code_offset;
    uint32_t code_end = code_off + (uint32_t)bc.size();

    // A file offset is a valid CALLAPI site if it points inside the bytecode and the
    // byte right before it (in the bytecode) is the 0x7d opcode.
    auto isSite = [&](uint32_t off) -> bool {
        if (off <= code_off || off >= code_end) return false;
        uint32_t rel = off - code_off;
        return rel >= 1 && bc[rel - 1] == 0x7d;
        };

    // Walk the footer id/site table in aligned 8-byte records. Start at field_14 (the
    // footer pointer) when valid, else right after the bytecode.
    size_t end = fd.size() & ~7u;
    size_t lo = code_end;
    if (s.field_14 > code_end && s.field_14 + 8 < fd.size()) lo = s.field_14;
    for (size_t p = lo & ~7u; p + 8 <= end; p += 8) {
        uint32_t a = ReadU32BE(&fd[p]);
        uint32_t b = ReadU32BE(&fd[p + 4]);
        // test both orientations; accept the one whose other word is a valid site
        if (a == api_id && isSite(b))      sites.push_back(b - code_off);
        else if (b == api_id && isSite(a)) sites.push_back(a - code_off);
    }
    std::sort(sites.begin(), sites.end());
    sites.erase(std::unique(sites.begin(), sites.end()), sites.end());
    return sites;
}

void CutsceneScriptParser::ScanTimerSteps(CutsceneScript& s,
    const std::vector<uint8_t>& filedata) {
    // The master clock advances at each WAIT (opcode 0x7E), which blocks for the
    // current value of timer slot 0x012C. A timer SET is "03 <float ticks> 81 02 01 2c"
    // (seconds = ticks/300). CRUCIAL: a WAIT can REUSE the previous timer value without
    // a new SET, so we walk the bytecode in order, track the last value set, and add it
    // at EVERY wait (reuses included). Counting unique SETs alone undercounts the real
    // runtime badly (e.g. 153 s instead of ~176 s for the Chopin event). timer_steps_s
    // holds the per-WAIT durations in playback order.
    //
    // NOTE on the opening: the first two waits are a setup/fade-in of the background
    // painting (the castle appears, then the title is composited) and do NOT advance the
    // visible story timeline. The game's visible clock starts at the third wait. We
    // record how many seconds those opening setup waits consume (opening_setup_s) so the
    // timeline builder can subtract them and align with the in-game timing (e.g. the
    // second story photo enters at 39 s, not 43 s).
    const std::vector<uint8_t>& bc = s.bytecode;
    float lastTimer = 0.0f;
    int waitIndex = 0;
    s.opening_setup_s = 0.0f;
    for (size_t i = 0; i < bc.size(); i++) {
        // timer SET: 03 <float> 81 02 01 2c
        if (bc[i] == 0x03 && i + 9 <= bc.size() &&
            bc[i + 5] == 0x81 && bc[i + 6] == 0x02 && bc[i + 7] == 0x01 && bc[i + 8] == 0x2C) {
            float ticks = ReadF32BE(&bc[i + 1]);
            if (ticks > 0.0f && ticks < 100000.0f) lastTimer = ticks / 300.0f;
            i += 4;  // skip past the float bytes
            continue;
        }
        // WAIT: consume the current timer (reuses the last value if no new SET)
        if (bc[i] == 0x7E && lastTimer > 0.0f) {
            s.timer_steps_s.push_back(lastTimer);
            s.narrative_runtime_s += lastTimer;
            if (waitIndex < 2) s.opening_setup_s += lastTimer;  // setup fade-in
            waitIndex++;
        }
    }
    // image-switch sites: API id 1028 is the image hand-off command. The footer
    // id/site table stores the id in the first word in some files and the second in
    // others, so a fixed-orientation read missed every switch in half the files
    // (collapsing the slideshow to one image). FindCallApiSites tests both orientations.
    {
        s.image_switch_offsets = FindCallApiSites(s, filedata, 1028);
        // for each switch, read the image slot it targets: a PUSH.W 0x0C0X
        // (81 02 0c XX) near the site.
        const std::vector<uint8_t>& b = s.bytecode;
        for (uint32_t off : s.image_switch_offsets) {
            uint16_t slot = 0;
            uint32_t lo = (off >= 12) ? off - 12 : 0;
            for (uint32_t k = lo; k + 4 <= b.size() && k <= off + 6; k++) {
                if (b[k] == 0x81 && b[k + 1] == 0x02 && b[k + 2] == 0x0C) {
                    slot = (uint16_t)(0x0C00 | b[k + 3]);
                    break;
                }
            }
            s.image_switch_slots.push_back(slot);
        }

        // Explicit slot -> image bindings from the bytecode:
        //   PUSH.B 2 (81 01 02) ; PUSH.dw addr (81 07 AAAAAAAA) ; PUSH.W slot (81 02 0c XX)
        // addr points at the image's Mefc container; resolve to a slide_images index
        // (the first NTX3 chunk at or after addr). Keep the FIRST binding per slot.
        for (size_t k = 0; k + 13 <= b.size(); k++) {
            if (b[k] == 0x81 && b[k + 1] == 0x01 && b[k + 2] == 0x02 &&
                b[k + 3] == 0x81 && b[k + 4] == 0x07 &&
                b[k + 9] == 0x81 && b[k + 10] == 0x02 && b[k + 11] == 0x0C) {
                uint32_t addr = ReadU32BE(&b[k + 5]);
                uint16_t slot = (uint16_t)(0x0C00 | b[k + 12]);
                if (s.slot_to_image.count(slot)) continue;  // keep first
                int imgIdx = -1;
                if (addr == 0) {
                    imgIdx = 0;  // opening background = image #0
                }
                else {
                    // first slide image whose file offset is >= addr
                    for (size_t i = 0; i < s.slide_images.size(); i++) {
                        if (s.slide_images[i].offset >= addr) { imgIdx = (int)i; break; }
                    }
                }
                if (imgIdx >= 0) s.slot_to_image[slot] = imgIdx;
            }
        }
    }
}

void CutsceneScriptParser::ScanSubtitleTiming(CutsceneScript& s,
    const std::vector<uint8_t>& fd) {
    // Assign each subtitle line its real appearance time by walking the bytecode and
    // tracking the master clock (WAIT consumes timer 0x012C, reusing the last value),
    // then stamping the clock onto the line whose index matches the PUSH.B argument of
    // each id-219 site. This replaces the wrong "sum <w> from t=0" model in the viewer,
    // so captions line up with the images (the story text starts only after the opening
    // holds, ~19 s into the Chopin event, not at t=0).
    if (s.languages.empty() && s.subtitle_lines.empty()) return;

    const std::vector<uint8_t>& bc = s.bytecode;

    // collect id-219 sites (offset -> subtitle index from PUSH.B). Id 219 is a CALLAPI
    // whose footer record orientation varies per file, so a fixed-orientation read left
    // subSites empty in half the files and aborted the whole timeline (one image shown).
    // FindCallApiSites tests both orientations and returns the real bytecode sites.
    std::map<uint32_t, int> subSites;  // bytecode offset -> line index
    {
        std::vector<uint32_t> sites = FindCallApiSites(s, fd, 219);
        for (uint32_t off : sites) {
            if (off >= bc.size()) continue;
            // PUSH.B index before the site
            int idx = -1;
            uint32_t lo = (off >= 12) ? off - 12 : 0;
            for (uint32_t k = lo; k + 3 <= bc.size() && k < off; k++)
                if (bc[k] == 0x81 && bc[k + 1] == 0x01) idx = bc[k + 2];
            if (idx >= 0) subSites[off] = idx;
        }
    }
    if (subSites.empty()) return;

    // ---- unified timeline: captions stretch by <w>, images follow, opening setup
    //      removed ---- //
    // The game's visible clock does NOT count the first two opening waits (background
    // fade-in), so we subtract opening_setup_s. Each caption is shown for its <w>
    // duration and the next caption only appears once the previous ends; when a caption
    // has to wait, that delay (shift) is propagated to the following image switches too,
    // so the photo behind a long caption holds for the right length (e.g. photo #9 stays
    // with the "unusually talented" caption instead of flashing). Validated against the
    // in-game pointers: 2nd story photo enters at 39 s; caption 7 lands on photo #9.

    // bytecode clock at each offset (absolute: every WAIT advances the clock). The two
    // opening waits ARE visible time (the castle fades in and the Revolution title is
    // composited over it), so they are NOT skipped. This makes the opening match the
    // game: the Revolution appears ~4 s in, the first story photo (#6) appears with
    // caption 1 at ~19 s, and the total runtime lands near the real 185 s.
    std::map<uint32_t, float> clockAtOffset;
    {
        float clock = 0.0f, lastTimer = 0.0f;
        for (size_t i = 0; i < bc.size(); i++) {
            clockAtOffset[(uint32_t)i] = clock;
            if (bc[i] == 0x03 && i + 9 <= bc.size() &&
                bc[i + 5] == 0x81 && bc[i + 6] == 0x02 && bc[i + 7] == 0x01 && bc[i + 8] == 0x2C) {
                float ticks = ReadF32BE(&bc[i + 1]);
                if (ticks > 0.0f && ticks < 100000.0f) lastTimer = ticks / 300.0f;
                continue;
            }
            if (bc[i] == 0x7E && lastTimer > 0.0f) clock += lastTimer;
        }
    }

    // image transitions via the PRELOAD pattern. Each story transition is encoded as a
    // texture preload of the NEXT photo followed by a command that shows the CURRENT
    // photo. The preload is "PUSH.B 2 ; PUSH.dw <texAddr> ; PUSH.W 0x0C0<slot>" and it
    // loads slot N (the upcoming photo) while the photo that becomes visible is slot N-1
    // (the one already loaded). Confirmed against the in-game rundown: the preload of #7
    // (slot 0x0C06) is the moment photo #6 becomes visible, etc. The opening preloads
    // (castle 0x0C04, first photo 0x0C05, Revolution 0x0C03) are setup, not transitions.
    // The very last photo has no following preload; it is shown by a trailing PUSH.W of
    // the top slot (0x0C0D) with no new texture load.
    // Image transitions come from the id-1028 image-switch command. At each switch
    // the slot pushed right before the 0x7d opcode (PUSH.W 0x0C0X ; 81 7d) is the slot
    // being activated. A transition fires two switches at the same clock: one preloads
    // the NEXT photo's slot and one activates the photo that becomes visible; the
    // visible one is the LOWER slot of the pair. Reading the activated slot directly is
    // correct whether the first story photo lives in slot 0x0C05 (e2010) or 0x0C06
    // (e4090); the old "preload reveals slot N-1" heuristic only matched the former and
    // shifted the latter by one image. Validated against the in-game rundowns of both.
    std::vector<std::pair<uint32_t, uint16_t>> imgPairs;  // (offset, VISIBLE slot)
    {
        const std::vector<uint8_t>& b = s.bytecode;
        auto slotAtSite = [&](uint32_t site) -> uint16_t {
            // pattern right before a CALLAPI site: 81 02 0c XX 81 7d
            if (site >= 6 && b[site - 6] == 0x81 && b[site - 5] == 0x02 &&
                b[site - 4] == 0x0C && b[site - 2] == 0x81 && b[site - 1] == 0x7d)
                return (uint16_t)(0x0C00 | b[site - 3]);
            return 0;
            };
        // group switches by clock time; the visible photo is the lowest slot fired then
        std::map<int, uint16_t> visibleAt;   // (clock*10) -> lowest slot
        std::map<int, uint32_t> offAt;        // (clock*10) -> earliest offset
        uint16_t topSlot = 0;
        for (uint32_t off : s.image_switch_offsets) {
            uint16_t slot = slotAtSite(off);
            if (slot < 0x0C05) continue;
            if (slot > topSlot) topSlot = slot;
            float c = clockAtOffset.count(off) ? clockAtOffset[off] : 0.0f;
            int key = (int)(c * 10.0f + 0.5f);
            auto it = visibleAt.find(key);
            if (it == visibleAt.end() || slot < it->second) {
                visibleAt[key] = slot;
                if (offAt.find(key) == offAt.end() || off < offAt[key]) offAt[key] = off;
            }
        }
        for (auto& kv : visibleAt)
            imgPairs.push_back({ offAt[kv.first], kv.second });
        // the final photo is the top slot; its activation is the last switch group, but
        // some files only preload it (its activate switch is the trailing PUSH.W). If the
        // highest visible slot is below topSlot, add topSlot at its trailing push.
        uint16_t maxVisible = 0;
        for (auto& pr : imgPairs) if (pr.second > maxVisible) maxVisible = pr.second;
        if (topSlot >= 0x0C05 && maxVisible < topSlot) {
            uint32_t lastOff = 0; bool found = false;
            for (size_t i = 0; i + 4 <= b.size(); i++) {
                if (b[i] == 0x81 && b[i + 1] == 0x02 && b[i + 2] == 0x0C &&
                    (uint16_t)(0x0C00 | b[i + 3]) == topSlot) {
                    bool isPreload = (i >= 6 && b[i - 6] == 0x81 && b[i - 5] == 0x07);
                    if (!isPreload) { lastOff = (uint32_t)i; found = true; }
                }
            }
            if (found) imgPairs.push_back({ lastOff, topSlot });
        }
        std::sort(imgPairs.begin(), imgPairs.end());
    }

    auto imgForSlot = [&](uint16_t slot) -> int {
        auto it = s.slot_to_image.find(slot);
        return (it != s.slot_to_image.end()) ? it->second : -1;
        };

    // merged event list in bytecode order: captions and image switches
    struct TLEvent { uint32_t off; int kind; int sub; uint16_t slot; };  // kind 0=sub 1=img
    std::vector<TLEvent> tl;
    for (auto& kv : subSites) tl.push_back({ kv.first, 0, kv.second, 0 });
    for (auto& sw : imgPairs)  tl.push_back({ sw.first, 1, -1, sw.second });
    std::sort(tl.begin(), tl.end(), [](const TLEvent& a, const TLEvent& b) { return a.off < b.off; });

    // stamp captions (per language) and build image_segments (selected language pacing)
    auto wOf = [&](std::vector<CutsceneScript::CSSubtitleLine>& lines, int li) -> float {
        return (li >= 0 && li < (int)lines.size() && lines[li].duration_s > 0.0f)
            ? lines[li].duration_s : 0.0f;
        };

    auto stampLang = [&](std::vector<CutsceneScript::CSSubtitleLine>& lines,
        bool buildImages) {
            float prevEnd = 0.0f, shift = 0.0f;
            uint16_t maxSlot = 0;
            float firstCap = -1.0f;
            // first caption start time (after setup removal). The opening Revolution title
            // is composited over the castle until this moment; the castle then stays alone
            // (with captions 1..) until the first story photo switch fires.
            for (auto& e : tl) {
                if (e.kind == 0) {
                    float raw = clockAtOffset.count(e.off) ? clockAtOffset[e.off] : 0.0f;
                    firstCap = raw;
                    break;
                }
            }
            if (buildImages) {
                s.image_segments.clear();
                int bg = imgForSlot(0x0C04);     // castle #0
                int title = imgForSlot(0x0C03);  // Revolution title strip
                // castle from t=0
                if (bg >= 0) s.image_segments.push_back({ 0.0f, bg, false });
                // the Revolution title is composited over the castle a few seconds in (it
                // is activated by a PUSH.W 0x0C03 with id-1028 after the opening fade-in,
                // ~4 s, not at t=0). Find that activation offset and use its clock.
                if (title >= 0) {
                    float titleTime = 0.0f;
                    for (size_t k = 0; k + 4 <= bc.size(); k++) {
                        if (bc[k] == 0x81 && bc[k + 1] == 0x02 &&
                            bc[k + 2] == 0x0C && bc[k + 3] == 0x03) {
                            titleTime = clockAtOffset.count((uint32_t)k) ? clockAtOffset[(uint32_t)k] : 0.0f;
                            break;
                        }
                    }
                    s.image_segments.push_back({ titleTime, title, true });
                }
                // castle alone once the title is gone (captions 1.. play here). We re-emit
                // the castle as a non-overlay segment at firstCap so the viewer drops the
                // title strip but keeps the castle until the first story photo enters.
                if (bg >= 0 && firstCap > 0.0f)
                    s.image_segments.push_back({ firstCap, bg, false });
            }

            for (auto& e : tl) {
                float raw = clockAtOffset.count(e.off) ? clockAtOffset[e.off] : 0.0f;
                float real = raw + shift;
                if (e.kind == 0) {
                    int li = e.sub;
                    float start = real;
                    if (start < prevEnd) { shift += (prevEnd - real); start = prevEnd; }
                    if (li >= 0 && li < (int)lines.size()) lines[li].start_s = start;
                    prevEnd = start + wOf(lines, li);
                }
                else if (e.kind == 1 && buildImages) {
                    // skip switches that fire during the opening setup (before the
                    // first caption): those preload the first story photo's slot but
                    // are not yet visible. The opening shows the background/title,
                    // seeded above; the first visible story photo is the first switch
                    // at or after the first caption.
                    if (firstCap >= 0.0f && raw + 0.5f < firstCap) continue;
                    if (e.slot >= 0x0C05 && e.slot > maxSlot) {
                        maxSlot = e.slot;
                        int img = imgForSlot(e.slot);
                        if (img >= 0) {
                            // story photos enter at their real switch time; if a switch
                            // fires while a caption is still up, hold it to the caption end
                            // so the photo behind a long caption does not flash by.
                            float at = real;
                            if (at < prevEnd) at = prevEnd;
                            s.image_segments.push_back({ at, img, false });
                        }
                    }
                }
            }
        };

    // stamp every language; build images only once using the selected language
    int sel = (s.selected_language >= 0 && s.selected_language < (int)s.languages.size())
        ? s.selected_language : 0;
    for (size_t L = 0; L < s.languages.size(); L++)
        stampLang(s.languages[L].lines, (int)L == sel);
    if (sel >= 0 && sel < (int)s.languages.size())
        s.subtitle_lines = s.languages[sel].lines;

    // tidy the image timeline: stable-sort by start time (keep overlays right after
    // their base), then drop zero-length non-overlay segments left by the opening seed.
    std::stable_sort(s.image_segments.begin(), s.image_segments.end(),
        [](const CutsceneScript::CSImageSegment& a,
            const CutsceneScript::CSImageSegment& b) { return a.start_s < b.start_s; });
    {
        std::vector<CutsceneScript::CSImageSegment> cleaned;
        for (size_t i = 0; i < s.image_segments.size(); i++) {
            const auto& sg = s.image_segments[i];
            // a non-overlay segment with the same start as the next non-overlay one is
            // a zero-length leftover; skip it
            if (!sg.title_overlay && i + 1 < s.image_segments.size()) {
                const auto& nx = s.image_segments[i + 1];
                if (!nx.title_overlay && nx.start_s <= sg.start_s + 1e-3f) continue;
            }
            cleaned.push_back(sg);
        }
        s.image_segments.swap(cleaned);
    }

    // The runtime is the end of the last caption (captions stretch the timeline by their
    // <w> durations, so this is later than the raw timer sum and matches the real ~185 s
    // much better). Fall back to the timer sum if no caption timing was found.
    float lastEnd = 0.0f;
    for (const auto& ln : s.subtitle_lines)
        if (ln.start_s >= 0.0f)
            lastEnd = std::max(lastEnd, ln.start_s + ln.duration_s);
    if (lastEnd > s.narrative_runtime_s) s.narrative_runtime_s = lastEnd;
}


void CutsceneScriptParser::ScanMusicCues(CutsceneScript& s,
    const std::vector<uint8_t>& fd) {
    // Find a "MPxxx.wav" string; after it, a table: pad + 0x02 + count + total +
    // [cue offsets...]. The cues are bytecode offsets where the music re-syncs.
    size_t pos = std::string::npos;
    for (size_t i = 0; i + 4 < fd.size(); i++) {
        if (fd[i] == 'M' && fd[i + 1] == 'P' &&
            i + 8 < fd.size() && memcmp(&fd[i + 5], ".wav", 4) == 0) {
            pos = i; break;
        }
        // also accept lowercase "mp" + ".wav"
        if ((fd[i] == 'm') && fd[i + 1] == 'p' &&
            i + 8 < fd.size() && memcmp(&fd[i + 5], ".wav", 4) == 0) {
            pos = i; break;
        }
    }
    if (pos == std::string::npos) return;
    // read the name
    size_t e = pos;
    while (e < fd.size() && fd[e] != 0) e++;
    s.music_track.assign((const char*)&fd[pos], e - pos);
    // skip name + padding to next non-zero, find the count marker (0x02) then count
    size_t t = e;
    while (t < fd.size() && fd[t] == 0) t++;
    // expect a 0x00000002 type tag word; search a small window
    size_t tbl = std::string::npos;
    for (size_t k = e; k + 8 < fd.size() && k < e + 0x20; k++) {
        if (ReadU32BE(&fd[k]) == 0x00000002) { tbl = k; break; }
    }
    if (tbl == std::string::npos) return;
    uint32_t count = ReadU32BE(&fd[tbl + 4]);
    if (count == 0 || count > 1024) return;
    size_t base = tbl + 8;
    // first value is a total/avg; the following `count` are cue offsets
    for (uint32_t c = 0; c < count && base + 4 + c * 4 + 4 <= fd.size(); c++)
        s.music_cues.push_back(ReadU32BE(&fd[base + 4 + c * 4]));
}

CutsceneScript CutsceneScriptParser::Parse(const std::string& filepath) {
    std::ifstream f(filepath, std::ios::binary);
    if (!f) return CutsceneScript();
    f.seekg(0, std::ios::end);
    size_t sz = (size_t)f.tellg();
    f.seekg(0, std::ios::beg);
    if (sz < 0x18) return CutsceneScript();
    std::vector<uint8_t> data(sz);
    f.read((char*)data.data(), sz);
    f.close();
    CutsceneScript s = ParseData(data);
    s.filename = filepath;
    return s;
}

CutsceneScript CutsceneScriptParser::ParseData(const std::vector<uint8_t>& data) {
    CutsceneScript s;
    size_t sz = data.size();
    if (sz < 0x18) return s;

    if (ReadU32BE(&data[0]) != CUTSCENE_MAGIC) return s;

    s.hash = ReadU32BE(&data[0x04]);
    s.secondary_id = ReadU32BE(&data[0x08]);
    s.file_size = ReadU32BE(&data[0x0C]);
    s.code_size = ReadU32BE(&data[0x10]);
    s.field_14 = ReadU32BE(&data[0x14]);

    uint32_t cs = s.code_size;
    if (s.code_offset + cs > data.size()) cs = (uint32_t)(data.size() - s.code_offset);
    s.bytecode.assign(data.begin() + s.code_offset, data.begin() + s.code_offset + cs);

    ParseRelocations(s, data);
    BuildNodes(s);
    ScanActorsAndData(s);
    ScanEmbeddedAnims(s, data);
    ScanEmbeddedModels(s, data);
    ScanScenePlacements(s, data);

    // Narrative-slideshow support (additive): classify the event kind, and when it is
    // a Narrative slideshow (Chopin-type), extract its images, subtitle timeline,
    // timer steps, image-switch sites and music cue table. 3D cutscenes are unaffected.
    ClassifyEventKind(s, data);
    if (s.kind == CutsceneScript::EventKind::Narrative ||
        s.kind == CutsceneScript::EventKind::TitleCard) {
        ScanSlideImages(s, data);
        ScanSubtitleTimeline(s, data);
        ScanTimerSteps(s, data);
        ScanMusicCues(s, data);
        ScanSubtitleTiming(s, data);
    }

    // PUSHOFF (0x07) operands are relative to the DATA SEGMENT (0x18 + code_size),
    // not to the file. Annotate every Offset argument with the chunk found at the
    // real target: BTX (subtitles), CSL (voices), Mefc (screen effects), NMTN
    // (scene/facial animations), or an argument string ("weapon", "MP158.cps").
    {
        const size_t dbase = s.code_offset + s.bytecode.size();
        auto label_at = [&](uint32_t off) -> std::string {
            size_t t = dbase + off;
            if (t + 0x20 > data.size()) return "";
            const uint8_t* p = data.data() + t;
            auto is_tag = [&](const char* m) { return memcmp(p, m, 4) == 0; };
            if (is_tag("NMTN") || is_tag("NMDL") || is_tag("NTX3")) {
                std::string nm((const char*)p, 4);
                nm += " '";
                for (size_t j = 0x10; j < 0x20 && p[j]; j++) {
                    char ch = (char)p[j];
                    if (ch < 32 || ch > 126) break;
                    nm += ch;
                }
                nm += "'";
                return nm;
            }
            if (is_tag("BTX ")) return "BTX subtitles";
            if (is_tag("CSL ")) return "CSL voices";
            if (is_tag("Mefc")) return "Mefc effects";
            // argument string?
            std::string txt;
            for (size_t j = 0; j < 24 && p[j]; j++) {
                char ch = (char)p[j];
                if (ch < 32 || ch > 126) { txt.clear(); break; }
                txt += ch;
            }
            if (txt.size() >= 3) return "'" + txt + "'";
            return "";
            };
        auto annotate = [&](std::vector<CSCommand>& cmds) {
            for (CSCommand& c : cmds) {
                bool changed = false;
                for (CSArg& a : c.args) {
                    if (a.kind != CSArgKind::Offset) continue;
                    std::string lbl = label_at(a.u);
                    if (!lbl.empty()) {
                        char b[96];
                        snprintf(b, sizeof(b), "@0x%X=%s", a.u, lbl.c_str());
                        a.text = b;
                        changed = true;
                    }
                }
                if (changed) {
                    std::string sum = c.name + "(";
                    for (size_t i = 0; i < c.args.size(); i++) {
                        if (i) sum += ", ";
                        sum += c.args[i].text;
                    }
                    sum += ")";
                    c.summary = sum;
                }
            }
            };
        annotate(s.all_commands);
        for (CSNode& nd : s.nodes) annotate(nd.commands);
    }

    // Hosted-event detection (corrected). A field script checks which events it hosts
    // with the VM sequence: load game-state slot 0 (opcode 0x0c, operand 0 = the
    // CURRENT EVENT number), PUSH, LDID <event>, CMP (0x5d = pop+compare with ACC),
    // i.e. "if (current_event == EV) { event setup }". Byte signature:
    //   0c 00 00 00 00 81 02 <ev_hi> <ev_lo> 5d
    // Menu scripts (zzz01) compare LOCALS instead (18 xx 81 ...) and never match.
    // NOTE: API 1021 (fn 0x385e70) is an ANGLE-CHECK utility (floats, deg/rad), not
    // event registration; earlier versions misattributed the event arg to it because
    // the comparison value leaked into the next command's stack snapshot.
    {
        static const uint8_t sig[6] = { 0x0c, 0, 0, 0, 0, 0x81 };
        const std::vector<uint8_t>& bc = s.bytecode;
        for (size_t i = 0; i + 10 <= bc.size(); i++) {
            if (memcmp(&bc[i], sig, 6) == 0 && bc[i + 6] == 0x02 && bc[i + 9] == 0x5d) {
                uint16_t ev = (uint16_t)((bc[i + 7] << 8) | bc[i + 8]);
                if (ev) {
                    s.hosted_events.push_back(ev);
                    s.hosted_event_setup_offsets.push_back((uint32_t)i);
                }
            }
        }
    }

    // Scene actors from the embedded NMTN names. Tokens of 3 alpha chars are
    // character codes (e1180_010_plk -> plk). Known codes map to their model source;
    // prop/scenery prefixes (tos/shi/eff = hana/kusa plants, effects) are excluded.
    {
        std::map<std::string, std::string> known = {
            {"plk", "root: pcplk_v*.p3obj (Polka)"},
            {"alg", "root: pcalg_v*.p3obj (Allegretto)"},
            {"bet", "root: pcbet_v*.p3obj (Beat)"},
            {"frd", "root: pcfrd_v*.p3obj (Frederic)"},
            {"cpn", "appkeep2.bmd: pcCPN_v1"},
            {"vol", "appkeep2.bmd: pcVOL_v1"},
            {"sls", "appkeep2.bmd: pcSLS_v1"},
            {"jrb", "appkeep2.bmd: pcJRB_v1"},
            {"fst", "appkeep2.bmd: pcFST_v1"},
            {"mch", "appkeep2.bmd: pcMCH_v1"},
            {"clv", "appkeep2.bmd: pcCLV_v1"},
            {"srn", "appkeep2.bmd: npcSRN_v3"},
        };
        std::map<std::string, bool> seen;
        for (const std::string& anim : s.embedded_anims) {
            std::string tok;
            std::string a = anim + "_";
            for (char ch : a) {
                if (ch == '_' || ch == '-') {
                    if (tok.size() == 3 &&
                        isalpha((unsigned char)tok[0]) && isalpha((unsigned char)tok[1]) &&
                        isalpha((unsigned char)tok[2]) &&
                        tok != "tos" && tok != "shi" && tok != "eff" && !seen[tok]) {
                        seen[tok] = true;
                        CutsceneScript::CSSceneActor act;
                        act.code = tok;
                        // 1st: a model embedded in THIS file whose name contains the code
                        // (self-contained cutscenes: slf -> npcSLF_v1). Several may match
                        // (npcLRG_v1 + the npcLRG_tsue cane prop): prefer the LARGEST.
                        std::string up = tok;
                        for (char& c2 : up) c2 = (char)toupper((unsigned char)c2);
                        uint32_t best_size = 0;
                        for (const auto& m : s.embedded_models) {
                            if (m.top_level && m.size > best_size &&
                                (m.name.find(up) != std::string::npos ||
                                    m.name.find(tok) != std::string::npos)) {
                                act.model_hint = "embedded: " + m.name;
                                best_size = m.size;
                            }
                        }
                        if (act.model_hint.empty()) {
                            auto it = known.find(tok);
                            act.model_hint = (it != known.end()) ? it->second : "unknown";
                        }
                        s.scene_actors.push_back(act);
                    }
                    tok.clear();
                }
                else tok += (char)tolower((unsigned char)ch);
            }
        }
    }

    // Initial scene camera: first SET_POSITION + SET_TARGET library calls on the
    // camera slot 0x0c03. Floats are pushed Z, Y, X (stack order), so args reverse.
    {
        // Gate: slot 0x0c03 is the camera only when the event drives it (has a
        // SET_TARGET on that slot) and the slot is not used as a character
        // (accessories via API_1074, PLAY_ANIM via LIB_0x4E5C with an NMTN). Events
        // with NCAM chunks use keyframed cameras, not this scripted path.
        bool slot_is_camera = false, slot_is_character = false;
        for (const CSCommand& c : s.all_commands) {
            bool slot03 = false;
            for (const CSArg& a : c.args)
                if (a.kind == CSArgKind::ActorId && a.u == 0x0c03) slot03 = true;
            if (!slot03) continue;
            if (c.is_lib && c.api_id == 0x14) slot_is_camera = true;     // SET_TARGET
            if (!c.is_lib && c.api_id == 1074) slot_is_character = true; // accessory
            if (c.is_lib && c.api_id == 0x4E5C) {
                for (const CSArg& a : c.args)
                    if (a.kind == CSArgKind::Offset) slot_is_character = true; // NMTN play
            }
        }
        bool has_ncam = false;
        for (size_t i = 0; i + 4 <= data.size(); i++)
            if (data[i] == 'N' && data[i + 1] == 'C' && data[i + 2] == 'A' && data[i + 3] == 'M') {
                has_ncam = true; break;
            }
        CutsceneScript::CSCameraShot cur;
        bool have_pos = false;
        if (!slot_is_camera || slot_is_character || has_ncam) {
            // not a scripted-camera event; leave camera_shots empty
        }
        else
            for (const CSCommand& c : s.all_commands) {
                if (!c.is_lib) continue;
                bool cam = false;
                for (const CSArg& a : c.args)
                    if (a.kind == CSArgKind::ActorId && a.u == 0x0c03) cam = true;
                if (!cam) continue;
                float f[3]; int nf = 0;
                for (const CSArg& a : c.args)
                    if (a.kind == CSArgKind::Float && nf < 3) f[nf++] = a.f;
                if (nf < 3) continue;
                if (c.api_id == 0x0F) {                  // SET_POSITION starts a shot
                    cur = CutsceneScript::CSCameraShot();
                    cur.offset = c.offset;
                    cur.eye[0] = f[2]; cur.eye[1] = f[1]; cur.eye[2] = f[0];
                    have_pos = true;
                }
                else if (c.api_id == 0x14 && have_pos) { // SET_TARGET completes it
                    cur.target[0] = f[2]; cur.target[1] = f[1]; cur.target[2] = f[0];
                    s.camera_shots.push_back(cur);
                    have_pos = false;
                }
            }
        // Attach the API_1025 params (#2 curve, #4 duration) and the API_1023 commit
        // to the shot they follow.
        auto shot_before = [&](uint32_t off) -> CutsceneScript::CSCameraShot* {
            CutsceneScript::CSCameraShot* best = nullptr;
            for (auto& sh : s.camera_shots)
                if (sh.offset < off && (!best || sh.offset > best->offset)) best = &sh;
            return best;
            };
        for (const CSCommand& c : s.all_commands) {
            bool cam = false;
            for (const CSArg& a : c.args)
                if (a.kind == CSArgKind::ActorId && a.u == 0x0c03) cam = true;
            if (!cam) continue;
            if (!c.is_lib && c.api_id == 1025) {
                int idx = -1; float fv = 0; bool hf = false; int iv = 0; bool hi = false;
                for (const CSArg& a : c.args) {
                    if (a.kind == CSArgKind::Float && !hf) { fv = a.f; hf = true; }
                    if (a.kind == CSArgKind::Imm) {
                        if (!hi) { iv = (int)a.u; hi = true; }
                        idx = (int)a.u;  // last Imm is the param index
                    }
                }
                CutsceneScript::CSCameraShot* sh = shot_before(c.offset);
                if (sh) {
                    if (idx == 4 && hf) sh->duration = fv;
                    else if (idx == 2) sh->curve = iv;
                }
            }
            else if (!c.is_lib && c.api_id == 1023) {
                CutsceneScript::CSCameraShot* sh = shot_before(c.offset);
                if (sh) sh->committed = true;
            }
        }
        if (!s.camera_shots.empty()) {
            s.has_camera = true;
            for (int i = 0; i < 3; i++) {
                s.cam_eye[i] = s.camera_shots[0].eye[i];
                s.cam_target[i] = s.camera_shots[0].target[i];
            }
        }

        // Pair the slot 0x0c04 cameras (confirmed in runtime to run with 0x0c03 on the
        // opening, with pitch). Match each 0x0c04 SET_POSITION/SET_TARGET to the 0x0c03
        // shot it immediately follows in bytecode order.
        if (!s.camera_shots.empty()) {
            CutsceneScript::CSCameraShot* curShot = nullptr;
            bool havePairPos = false;
            float peye[3] = { 0, 0, 0 };
            for (const CSCommand& c : s.all_commands) {
                // advance curShot to the last 0x0c03 shot at/just before this offset
                for (auto& sh : s.camera_shots)
                    if (sh.offset <= c.offset &&
                        (!curShot || sh.offset > curShot->offset)) curShot = &sh;
                bool c04 = false;
                for (const CSArg& a : c.args)
                    if (a.kind == CSArgKind::ActorId && a.u == 0x0c04) c04 = true;
                if (!c04 || !c.is_lib) continue;
                float f[3]; int nf = 0;
                for (const CSArg& a : c.args)
                    if (a.kind == CSArgKind::Float && nf < 3) f[nf++] = a.f;
                if (nf < 3) continue;
                if (c.api_id == 0x0F) {            // SET_POSITION
                    peye[0] = f[2]; peye[1] = f[1]; peye[2] = f[0];
                    havePairPos = true;
                }
                else if (c.api_id == 0x14 && havePairPos && curShot && !curShot->has_pair) {
                    curShot->has_pair = true;
                    for (int i = 0; i < 3; i++) curShot->pair_eye[i] = peye[i];
                    curShot->pair_target[0] = f[2];
                    curShot->pair_target[1] = f[1];
                    curShot->pair_target[2] = f[0];
                    havePairPos = false;
                }
            }
        }
    }

    s.valid = true;
    // Field conversational camera cuts: SET_POSITION (world) + SET_ANGLES (yaw),
    // paired. Only meaningful for MAP scripts (hosted_events present). World coords,
    // large |Z|, so no anchor needed when rendering these.
    if (!s.hosted_events.empty()) {
        for (size_t i = 0; i + 1 < s.all_commands.size(); i++) {
            const CSCommand& cp = s.all_commands[i];
            if (!(cp.is_lib && cp.api_id == 0x0F)) continue;  // SET_POSITION
            float fx[3] = { 0, 0, 0 }; int nf = 0;
            for (const CSArg& a : cp.args)
                if (a.kind == CSArgKind::Float && nf < 3) fx[nf++] = a.f;
            if (nf < 3) continue;
            // ignore tiny/degenerate ones (setup calls, not real positions)
            if (fx[0] == 0.0f && fx[1] == 0.0f && fx[2] == 0.0f) continue;
            CutsceneScript::CSFieldShot fs;
            fs.offset = cp.offset;
            // SET_POSITION pushes Z,Y,X -> args come as (X,Y,Z) after parser invert
            fs.eye[0] = fx[2]; fs.eye[1] = fx[1]; fs.eye[2] = fx[0];
            for (size_t j = i + 1; j < s.all_commands.size() && j < i + 8; j++) {
                const CSCommand& ca = s.all_commands[j];
                if (ca.is_lib && ca.api_id == 0x10) { // SET_ANGLES
                    int na = 0; float ay[3] = { 0, 0, 0 };
                    for (const CSArg& a : ca.args)
                        if (a.kind == CSArgKind::Float && na < 3) ay[na++] = a.f;
                    fs.pitch = ay[0]; fs.yaw = ay[1];
                    break;
                }
            }
            s.field_shots.push_back(fs);
        }
    }

    return s;
}

// ---------------------------------------------------------------------------
// Event -> map index and helpers
// ---------------------------------------------------------------------------
#include <filesystem>
#include <cctype>

// Map name -> texture bank, extracted from the eboot map table (0x506e2c, 355 entries).
// The map's main NMDL has no internal NTX3; its textures come from <bank>.p3tex.
struct CSMapTex { const char* name; const char* bank; };
static const CSMapTex CS_MAP_TEXBANK[] = {
    {"adg01","ADG"}, {"adg02","ADG"}, {"adg40","ADG"}, {"adt01","ADT_A"},
    {"adt02","ADT_A"}, {"adt03","ADT_A"}, {"adt40","ADT_B"}, {"adt41","ADT_B"},
    {"adt42","ADT_B"}, {"adt43","ADT_B"}, {"adt44","ADT_B"}, {"agg01","AGG_C"},
    {"agg02","AGG_A"}, {"agg40","AGG_B"}, {"agg41","AGG_B"}, {"agg60","AGG_B"},
    {"agn01","AGS"}, {"agn02","AGS"}, {"agn03","AGS"}, {"agn04","AGS"},
    {"ags01","AGS"}, {"ags02","AGS"}, {"ags03","AGS"}, {"ags04","AGS"},
    {"ara01","ARA_A"}, {"ara02","ARA_B"}, {"ara03","ARA_B"}, {"ara04","ARA_B"},
    {"ara05","ARA_B"}, {"ara06","ARA_B"}, {"ara07","ARA_B"}, {"atn01","ATN"},
    {"atn02","ATN"}, {"atn03","ATN"}, {"atn04","ATN"}, {"atn05","ATN"},
    {"atn06","ATN"}, {"bel01","BEL_A"}, {"bel02","BEL_A"}, {"bel03","BEL_A"},
    {"bel04","BEL_A"}, {"bel05","BEL_A"}, {"bel40","BEL_B"}, {"bqf01","BQF"},
    {"bqf40","BQF"}, {"bqf41","BQF"}, {"bqf42","BQF"}, {"bqf43","BQF"},
    {"bqf44","BQF"}, {"bqf45","BQF"}, {"bqf47","BQF"}, {"bqf48","BQF"},
    {"bqf60","BQF"}, {"bqj01","BQJ_A"}, {"bqj02","BQJ_C"}, {"bqj03","BQJ_C"},
    {"bqj04","BQJ_C"}, {"bqj05","BQJ_C"}, {"bqj06","BQJ_C"}, {"bqj07","BQJ_C"},
    {"bqj08","BQJ_C"}, {"bqj09","BQJ_C"}, {"bqj10","BQJ_C"}, {"bqj11","BQJ_C"},
    {"bqj12","BQJ_C"}, {"bqj13","BQJ_C"}, {"bqj14","BQJ_C"}, {"bqj15","BQJ_D"},
    {"bqj16","BQJ_C"}, {"bqj17","BQJ_C"}, {"bqj18","BQJ_C"}, {"bqj60","BQJ_C"},
    {"bqm01","BQM_A"}, {"bqm02","BQM_B"}, {"bqm03","BQM_C"}, {"bqm40","BQM_D"},
    {"bqm41","BQM_E"}, {"bqm42","BQM_E"}, {"bqm43","BQM_F"}, {"bqm44","BQM_F"},
    {"bqm45","BQM_E"}, {"bqm46","BQM_D"}, {"bqm49","BQM_G"}, {"cbl01","CBL_A"},
    {"cbl02","ROC_A"}, {"cbs01","CBS"}, {"cbs60","CBS_B"}, {"cnt01","CNT_A"},
    {"cnt02","CNT_A"}, {"cnt03","CNT_A"}, {"cnt40","CNT_B"}, {"cnt41","CNT_B"},
    {"cnt42","CNT_B"}, {"cst01","CST_A"}, {"cst02","CST_A"}, {"cst03","CST_A"},
    {"cst04","ARA_A"}, {"dld01","DLD_A"}, {"dld02","DLD_A"}, {"dld03","DLD_A"},
    {"dld04","DLD_A"}, {"dld05","DLD_A"}, {"dld06","DLD_A"}, {"dld07","DLD_A"},
    {"dld08","DLD_A"}, {"dld09","DLD_A"}, {"dld10","DLD_A"}, {"dld11","DLD_A"},
    {"dld12","DLD_A"}, {"dld13","DLD_A"}, {"dld14","DLD_A"}, {"dld15","DLD_B"},
    {"dld16","DLD_C"}, {"dld17","DLD_D"}, {"dld18","DLD_E"}, {"elg01","ELG_A"},
    {"elg02","ELG_B"}, {"elg03","ELG_C"}, {"eve01","EVE"}, {"fmt01","FMT_A"},
    {"fmt02","FMT_B"}, {"fmt03","FMT_B"}, {"fmt04","FMT_B"}, {"fmt05","FMT_B"},
    {"fmt06","FMT_B"}, {"fmt07","FMT_B"}, {"fmt08","FMT_B"}, {"fmt09","FMT_B"},
    {"fmt10","FMT_B"}, {"fmt11","FMT_B"}, {"fmt12","FMT_B"}, {"fmt13","FMT_B"},
    {"fmt14","FMT_B"}, {"fmt15","FMT_B"}, {"ftj01","FTJ_A"}, {"ftj02","FTJ_B"},
    {"ftj40","FTJ_C"}, {"ftm01","FTM_A"}, {"ftm02","FTM_B"}, {"ftm03","FTM_A"},
    {"ftm04","FTM_A"}, {"ftm05","FTM_B"}, {"ftm06","FTM_A"}, {"ftm40","FTM_C"},
    {"ftm41","FTM_D"}, {"ftm42","FTM_E"}, {"ftm43","FTM_C"}, {"ftm44","FTM_D"},
    {"ftm45","FTM_E"}, {"ftm64","FTM_D"}, {"gls01","GLS"}, {"gls02","GLS"},
    {"gls03","GLS"}, {"gls04","GLS"}, {"gls05","GLS"}, {"gls06","GLS"},
    {"gls07","GLS"}, {"gls08","GLS"}, {"hno01","HNO"}, {"hno02","HNO"},
    {"hno03","HNO"}, {"ktm01","KTM"}, {"ktm02","KTM"}, {"ktm03","KTM"},
    {"ktm04","KTM"}, {"ktm05","KTM"}, {"ktm06","KTM"}, {"ktm07","KTM"},
    {"ktm08","KTM"}, {"ktm09","KTM"}, {"ktm10","KTM"}, {"kts01","KTS"},
    {"kts02","KTS"}, {"kzf01","KZF_A"}, {"kzf40","KZF_B"}, {"kzf41","KZF_B"},
    {"kzf42","KZF_B"}, {"kzf43","KZF_B"}, {"kzf44","KZF_B"}, {"kzf45","KZF_B"},
    {"kzf46","KZF_B"}, {"kzf47","KZF_B"}, {"kzf48","KZF_B"}, {"kzf49","KZF_B"},
    {"kzf50","KZF_B"}, {"kzf51","KZF_B"}, {"kzf52","KZF_B"}, {"kzf53","KZF_B"},
    {"kzf54","KZF_B"}, {"kzf55","KZF_B"}, {"kzf56","KZF_B"}, {"kzf57","KZF_B"},
    {"kzf58","KZF_B"}, {"kzf59","KZF_B"}, {"kzf60","KZF_B"}, {"kzf61","KZF_B"},
    {"kzf62","KZF_B"}, {"kzf63","KZF_B"}, {"kzf64","KZF_B"}, {"kzf65","KZF_B"},
    {"kzf66","KZF_B"}, {"kzf67","KZF_B"}, {"kzf68","KZF_B"}, {"kzf69","KZF_B"},
    {"kzf70","KZF_B"}, {"kzf71","KZF_B"}, {"lam01","LAM_A"}, {"lam02","LAM_B"},
    {"lam03","LAM_C"}, {"lam04","LAM_A"}, {"lam05","LAM_B"}, {"lam06","LAM_D"},
    {"lam07","LAM_D"}, {"lam08","LAM_D"}, {"lam09","LAM_D"}, {"lam10","LAM_D"},
    {"lam14","LAM_A"}, {"lnt01","LNT"}, {"lnt02","LNT"}, {"lnt03","LNT"},
    {"lnt04","LNT"}, {"lnt05","LNT"}, {"lnt06","LNT"}, {"lnt07","LNT"},
    {"lnt08","LNT"}, {"nis01","NIS_A"}, {"nis02","NIS_A"}, {"nis03","NIS_A"},
    {"nis04","NIS_B"}, {"prs01","PRS"}, {"roc01","ROC_A"}, {"roc02","ROC_B"},
    {"roc03","ROC_B"}, {"roc04","ROC_C"}, {"roc05","ROC_B"}, {"roc06","ROC_C"},
    {"roc07","ROC_B"}, {"roc08","ROC_C"}, {"roc09","ROC_B"}, {"roc10","ROC_A"},
    {"roc11","ROC_D"}, {"rtd01","RTD_A"}, {"rtd02","RTD_A"}, {"rtd03","RTD_B"},
    {"rtd04","RTD_C"}, {"rtd41","RTD_C"}, {"rtd42","RTD_D"}, {"rtd43","RTD_E"},
    {"rtd44","RTD_F"}, {"rtd45","RTD_G"}, {"rtd46","RTD_H"}, {"rtk01","RTK_A"},
    {"rtk02","RTK_B"}, {"rtk03","RTK_C"}, {"rtk04","RTK_B"}, {"rtk05","RTK_B"},
    {"rtk06","RTK_B"}, {"rtk07","RTK_B"}, {"rty01","RTY_A"}, {"rty02","RTY_A"},
    {"rty03","RTY_A"}, {"rty04","RTY_B"}, {"rvb01","RVB_A"}, {"rvb02","RVB_A"},
    {"rvb40","RVB_B"}, {"rvb41","RVB_B"}, {"sbi01","SBI_A"}, {"sbi02","SBI_B"},
    {"sbi03","SBI_C"}, {"sbi04","SBI_C"}, {"sbi05","SBI_D"}, {"sbi06","SBI_E"},
    {"sbi07","SBI_E"}, {"svh01","SVH_A"}, {"svh02","SVH_A"}, {"svh03","SVH_A"},
    {"svh04","SVH_A"}, {"svh40","SVH_B"}, {"svh60","SVH_B"}, {"tcd01","TCD"},
    {"tcd02","TCD"}, {"tcd03","TCD"}, {"tcd04","TCD"}, {"tcd05","TCD"},
    {"tcd06","TCD"}, {"tcd07","TCD"}, {"tcd08","TCD"}, {"tnk01","TNK"},
    {"tnk02","TNK"}, {"tnt01","TNT_A"}, {"tnt02","TNT_B"}, {"tnt03","TNT_B"},
    {"tnt04","TNT_C"}, {"tnt05","TNT_C"}, {"tnt06","TNT_B"}, {"tnt40","TNT_D"},
    {"tnt41","TNT_D"}, {"tnt42","TNT_D"}, {"tnt43","TNT_D"}, {"tnt44","TNT_D"},
    {"tnt45","TNT_D"}, {"tnt60","TNT_D"}, {"tnt61","TNT_D"}, {"uns01","UNS"},
    {"uns02","UNS"}, {"uns03","UNS"}, {"uns04","UNS"}, {"uns05","UNS"},
    {"uns06","UNS"}, {"uns07","UNS"}, {"uns08","UNS"}, {"uns09","UNS"},
    {"uns10","UNS"}, {"uns11","UNS"}, {"uns12","UNS"}, {"uns13","UNS"},
    {"uns14","UNS"}, {"uns15","UNS"}, {"uns16","UNS"}, {"uns17","UNS"},
    {"uns18","UNS"}, {"uns19","UNS"}, {"uns20","UNS"}, {"uns21","UNS"},
    {"uns22","UNS"}, {"uns23","UNS"}, {"wau01","WAU"}, {"wau02","WAU"},
    {"wau03","WAU"}, {"wau04","WAU"}, {"wau05","WAU"}, {"wau06","WAU"},
    {"wau07","WAU"}, {"wau08","WAU"}, {"wau09","WAU"}, {"wbk01","WBK"},
    {"wbk02","WBK"}, {"wbk03","WBK"}, {"wbk04","WBK"}, {"wbk05","WBK"},
    {"xlp01","XLP_A"}, {"xlp02","XLP_B"}, {"xlp03","XLP_C"}, {"xlp04","XLP_A"},
    {"xlp05","XLP_B"}, {"xlp06","XLP_C"}, {"xlp07","XLP_A"}, {"xlp08","XLP_B"},
    {"xlp09","XLP_C"}, {"xlp10","XLP_A"}, {"xlp11","XLP_A"}, {"JPN ","USA "},
    {"FRA ","ITA "}, {"ESP ","tnt03.e"}, {"cbs60.e","agg02.e"},
};

std::string CutsceneScriptParser::MapTextureBank(const std::string& map_name) {
    for (const CSMapTex& e : CS_MAP_TEXBANK)
        if (map_name == e.name) return e.bank;
    return "";
}

uint32_t CutsceneScriptParser::EventNumberFromFilename(const std::string& path) {
    // basename
    size_t slash = path.find_last_of("/\\");
    std::string base = (slash == std::string::npos) ? path : path.substr(slash + 1);
    // find 'e' (or 'E') followed by digits
    for (size_t i = 0; i + 1 < base.size(); i++) {
        if ((base[i] == 'e' || base[i] == 'E') && isdigit((unsigned char)base[i + 1])) {
            uint32_t v = 0;
            size_t j = i + 1;
            while (j < base.size() && isdigit((unsigned char)base[j])) {
                v = v * 10 + (base[j] - '0');
                j++;
            }
            return v;
        }
    }
    return 0;
}

std::map<uint32_t, std::string> CutsceneScriptParser::ScanCfdataFolder(const std::string& folder) {
    std::map<uint32_t, std::string> index;
    std::error_code ec;
    for (const auto& entry : std::filesystem::directory_iterator(folder, ec)) {
        if (ec) break;
        if (!entry.is_regular_file()) continue;
        std::string ext = entry.path().extension().string();
        if (ext != ".e" && ext != ".E") continue;
        CutsceneScript s = Parse(entry.path().string());
        if (!s.valid) continue;
        std::string stem = entry.path().stem().string();
        for (uint32_t ev : s.hosted_events)
            index[ev] = stem;
    }
    return index;
}

// ---------------------------------------------------------------------------
// Event titles + scene placements
// ---------------------------------------------------------------------------

// English event titles extracted from zzz01's debug-menu data (168 events).
struct CSEventTitle { uint32_t ev; const char* title; };
static const CSEventTitle CS_EVENT_TITLES[] = {
    { 5, "A New Journey" },
    { 10, "Al abismo" },
    { 20, "Aldea Tenuto" },
    { 30, "Wellen im Meer" },
    { 40, "Antes de la muerte" },
    { 1000, "Chapter 1 Raindrops" },
    { 1005, "Going Home" },
    { 1010, "No One Wants to Touch Me" },
    { 1020, "Selling Floral Powder" },
    { 1030, "Using Magic" },
    { 1040, "Run Beat!" },
    { 1050, "After the Bread->BOSS BATTLE" },
    { 1055, "Why so expensive?" },
    { 1060, "Not Let Her Go" },
    { 1070, "Magic Girl" },
    { 1080, "Opening" },
    { 1090, "Star Gazing" },
    { 1100, "Logo" },
    { 1110, "Frederic's Dream" },
    { 1120, "Frederic and Polka" },
    { 1130, "BOSS BATTLE->Animals in the Forest" },
    { 1140, "What I Wanted to Show You" },
    { 1150, "First Time Out of Ritardando" },
    { 1160, "Different from Rats" },
    { 1170, "BOSS BATTLE->Rain->PIANO" },
    { 1180, "Heaven's Gift" },
    { 1190, "Lightning->BOSS BATTLE (LOSE)" },
    { 1195, "Relieve Stress" },
    { 1200, "Tonight is Crucial" },
    { 1210, "Stopped Raining" },
    { 1220, "Finding a House Now" },
    { 1230, "Polka!->BOSS BATTLE" },
    { 1235, "Are you hurt?" },
    { 1240, "Meeting the Count" },
    { 1250, "Are you listening?" },
    { 1260, "Long Name" },
    { 1270, "Becoming Heaven's Mirror" },
    { 1280, "Take Me With You" },
    { 2000, "Chapter 2 Revolution" },
    { 2010, "What can they do?->PIANO" },
    { 2020, "Someone is Backing Them" },
    { 2030, "Don't Touch My Goats->BOSS BATTLE" },
    { 2035, "Sorry About That" },
    { 2040, "I'm Viola" },
    { 2050, "Old Bridge" },
    { 2060, "Any information Fugue?" },
    { 2070, "Ideal Place" },
    { 2080, "BOSS BATTLE->Fort in Name Only" },
    { 2090, "Mission Brief" },
    { 2100, "Air from the Rock" },
    { 2102, "Forte City" },
    { 2103, "Where did he go?" },
    { 2104, "Something on the Ground" },
    { 2107, "Rescue Phil" },
    { 2108, "Thanks" },
    { 2110, "Suspicious Group->BOSS BATTLE" },
    { 2115, "Too Easy on Them" },
    { 2120, "Who're you calling a little girl?" },
    { 2130, "I'm Taller" },
    { 2140, "Encounter" },
    { 2150, "Stop Mining at Mt. Rock" },
    { 2160, "Mission Complete" },
    { 2170, "Last Chance" },
    { 2180, "Why are you on a journey?" },
    { 2190, "Journey to Confront Myself" },
    { 2200, "Not Being Nosy" },
    { 2210, "Thank Goodness You Talk->BOSS BATTLE" },
    { 2220, "Bridge Collapse" },
    { 2230, "Swept Away" },
    { 3000, "Chapter 3 Fantaisie-Impromptu" },
    { 3010, "Four Days" },
    { 3020, "Tired" },
    { 3030, "Cabasa Bridge Fell" },
    { 3040, "Mistook Us for Someone Else" },
    { 3050, "Flashback to Tuba" },
    { 3060, "Spy in the Midst" },
    { 3070, "Andante is Beyond the Woods" },
    { 3075, "BOSS BATTLE" },
    { 3080, "Underground City" },
    { 3090, "BOSS BATTLE->We Won" },
    { 3110, "Something Troubling" },
    { 3120, "Claves's Death->Piano" },
    { 4000, "Chapter 4 Grand Valse Brilliante" },
    { 4010, "There You Are" },
    { 4020, "Thank You for Rescuing Us" },
    { 4030, "Go Outside for Fresh Air" },
    { 4040, "Emilia" },
    { 4050, "Pirate Attack" },
    { 4060, "Strange Rock" },
    { 4070, "BOSS BATTLE->Pirates Treasure" },
    { 4080, "Remember Something Bad->PIANO" },
    { 4090, "Beautiful Dance" },
    { 4100, "Serenade" },
    { 4101, "A Mirror Called Lament" },
    { 4102, "Where am I?" },
    { 4103, "The Spirit of the Mirror" },
    { 4104, "Minuet!" },
    { 4105, "We made it back!" },
    { 4110, "Beyond These Mountains" },
    { 4120, "It's Cold" },
    { 4130, "Once a Spy Always a Spy" },
    { 4140, "Mountain Lodge" },
    { 4150, "Astra" },
    { 4160, "What's wrong Polka?" },
    { 4170, "Lava Cave" },
    { 4180, "Back to Agogo Village->BOSS BATTLE" },
    { 4190, "Fugue's Failure" },
    { 4200, "Dove on the Windowsill" },
    { 5000, "Chapter 5 Nocturne" },
    { 5010, "Why did you die?" },
    { 5020, "You're Late Beat" },
    { 5030, "Jazz Looking at Baroque Castle" },
    { 5040, "Poor Claves" },
    { 5050, "People by the Church" },
    { 5060, "Hey, Salsa" },
    { 5070, "Don't Have Time for Ghosts" },
    { 5080, "Something on the Ground" },
    { 5090, "March Joining" },
    { 5100, "Key Person" },
    { 5110, "Ghost in the Church->BOSS BATTLE" },
    { 5120, "More Beautiful Back Then->PIANO" },
    { 5130, "Maybe I'll Jump" },
    { 5135, "You two are awfully friendly" },
    { 5140, "Last Place Again" },
    { 5150, "It'll Be Too Late" },
    { 5160, "Fortune" },
    { 5170, "Being Third Place" },
    { 5180, "Need Time Alone" },
    { 5190, "Someone Going In" },
    { 5200, "It's a Dead End->BOSS BATTLE" },
    { 5210, "Falsetto is Back" },
    { 6000, "Chapter 6 Tristesse" },
    { 6010, "He Should Have an Answer" },
    { 6011, "Are you okay, Polka?" },
    { 6012, "The Agogo Queen Mother" },
    { 6013, "We're really here!" },
    { 6014, "I have no use for ordinary agogos" },
    { 6015, "I guess I underestimated you" },
    { 6016, "Thank you, Allegretto" },
    { 6020, "They're Not Here" },
    { 6030, "Farewell Homeland->PIANO" },
    { 6040, "Let's Go After Them" },
    { 6050, "Sacred Tree" },
    { 6052, "What are you doing?" },
    { 6054, "What is that EZI guy anyway?" },
    { 6060, "Confrontation with Waltz->BOSS BATTLE" },
    { 6070, "Legato's Transformation" },
    { 7000, "Chapter 7 Heroic" },
    { 7010, "What is that hole?" },
    { 7020, "Where are we?" },
    { 7040, "Are you okay, Jazz?" },
    { 7050, "Hold on a minute" },
    { 7060, "March! What are you doing?" },
    { 7070, "Come on, Beat!" },
    { 7080, "Everyone ready?->BOSS BATTLE" },
    { 7090, "After Battle Against Legato" },
    { 7100, "Frederic?" },
    { 8000, "Final Chapter Heaven's Mirror" },
    { 8005, "Final Battle->BOSS BATTLE" },
    { 8010, "Stay Back" },
    { 8021, "End Credits 1Beat" },
    { 8022, "End Credits 1 Frederic" },
    { 8023, "End Credits 1 Normal" },
    { 8040, "Frederic's Death" },
    { 8042, "Frederic. You've come back to us." },
    { 8050, "Heaven's Mirror" },
    { 8060, "End Credits 2" },
    { 8070, "Shape of Life" },
};

std::string CutsceneScriptParser::EventTitle(uint32_t ev) {
    for (const CSEventTitle& e : CS_EVENT_TITLES)
        if (e.ev == ev) return e.title;
    return "";
}

// Scene placements: decode the "move" track of every scene NMTN at t=0.
// NMTN layout (see Nmtnparser.h, validated against the eboot): chunk header 0x20,
// anim header 0x10, then tracks. Track = name[16] + total_size u32 + flags u32 +
// [preamble f32 per static slot] + [channel per keyframed slot]. Channel =
// count u16 + fmt u16 (low byte: scale = 1/(1<<fmt)) + count * {frame u16,
// value i16, tan_in i16, tan_out i16}. Slots 0-2 = pos XYZ, 3-5 = rot XYZ.
void CutsceneScriptParser::ScanScenePlacements(CutsceneScript& s, const std::vector<uint8_t>& fd) {
    auto rdU32 = [&](size_t o) { return ReadU32BE(&fd[o]); };
    auto rdU16 = [&](size_t o) { return (uint16_t)((fd[o] << 8) | fd[o + 1]); };
    auto rdI16 = [&](size_t o) { return (int16_t)rdU16(o); };

    for (size_t i = 0; i + 0x40 < fd.size(); i++) {
        if (!(fd[i] == 'N' && fd[i + 1] == 'M' && fd[i + 2] == 'T' && fd[i + 3] == 'N')) continue;
        uint32_t nsz = rdU32(i + 4);
        if (nsz < 0x40 || i + nsz > fd.size()) continue;
        std::string anim;
        for (size_t j = i + 0x10; j < i + 0x20 && fd[j]; j++) {
            char c = (char)fd[j];
            if (c < 32 || c > 126) { anim.clear(); break; }
            anim += c;
        }
        if (anim.empty()) continue;

        // Walk the tracks looking for "move" (usually track 1, after "Top1")
        size_t p = i + 0x30;
        size_t end = i + 8 + nsz;
        if (end > fd.size()) end = fd.size();
        bool found = false;
        for (int ti = 0; ti < 8 && p + 0x18 <= end; ti++) {
            std::string bname;
            for (size_t j = p; j < p + 16 && fd[j]; j++) bname += (char)fd[j];
            uint32_t tsize = rdU32(p + 0x10);
            uint32_t flags = rdU32(p + 0x14);
            if (tsize < 0x18 || p + tsize > end + 0x10) break;
            if (bname == "move") {
                uint8_t field[9];
                for (int sl = 0; sl < 9; sl++)
                    field[sl] = (uint8_t)((flags >> ((8 - sl) * 3)) & 7);
                size_t q = p + 0x18;
                float v[9] = { 0,0,0, 0,0,0, 1,1,1 };
                for (int sl = 0; sl < 9; sl++) {
                    if (field[sl] == 1 && q + 4 <= end) {
                        uint32_t raw = rdU32(q);
                        memcpy(&v[sl], &raw, 4);
                        q += 4;
                    }
                }
                for (int sl = 0; sl < 9; sl++) {
                    if (field[sl] != 2 || q + 4 > end) continue;
                    uint16_t cnt = rdU16(q);
                    uint8_t fmt = fd[q + 3];
                    float scale = 1.0f / (float)(1u << fmt);
                    if (cnt > 0 && q + 12 <= end)
                        v[sl] = rdI16(q + 6) * scale;  // first key's value (frame 0)
                    q += 4 + (size_t)cnt * 8;
                }
                CutsceneScript::CSScenePlacement pl;
                pl.anim = anim;
                pl.pos[0] = v[0]; pl.pos[1] = v[1]; pl.pos[2] = v[2];
                pl.rot[0] = v[3]; pl.rot[1] = v[4]; pl.rot[2] = v[5];
                // character code and part number from the name tokens
                std::string tok;
                std::string a = anim + "_";
                for (char ch : a) {
                    if (ch == '_' || ch == '-') {
                        if (tok.size() == 3 && isalpha((unsigned char)tok[0]) &&
                            isalpha((unsigned char)tok[1]) && isalpha((unsigned char)tok[2]) &&
                            pl.code.empty())
                            pl.code = tok;
                        if (!tok.empty() && tok.size() <= 4 && pl.part == 0) {
                            bool dig = true;
                            for (char c2 : tok) if (!isdigit((unsigned char)c2)) { dig = false; break; }
                            if (dig && tok != a.substr(1, tok.size()))  // skip the event number
                                pl.part = (uint32_t)atoi(tok.c_str());
                        }
                        tok.clear();
                    }
                    else tok += (char)tolower((unsigned char)ch);
                }
                s.scene_placements.push_back(pl);
                found = true;
                break;
            }
            p += tsize;
        }
        (void)found;
    }
}

// Approximate map-space anchor for an event: the field script surrounds the
// "if (current_event == EV)" comparison with SETTER/INIT_SLOT commands whose data
// tables carry map-space positions (validated on agn01/e1180: a trigger-point list
// and the event-zone rectangle, ground Y = terrain height). Decode the first
// plausible float triplet from those tables (any byte alignment; the records are
// not 4-aligned).
bool CutsceneScriptParser::FindEventAnchor(const std::string& map_path, uint32_t event,
    float out_pos[3]) {
    CutsceneScript m = Parse(map_path);
    if (!m.valid) return false;
    uint32_t setup = 0;
    bool found = false;
    for (size_t i = 0; i < m.hosted_events.size(); i++)
        if (m.hosted_events[i] == event && i < m.hosted_event_setup_offsets.size()) {
            setup = m.hosted_event_setup_offsets[i];
            found = true;
            break;
        }
    if (!found) return false;

    // Re-read the file for the data segment
    std::ifstream f(map_path, std::ios::binary);
    if (!f) return false;
    std::vector<uint8_t> d((std::istreambuf_iterator<char>(f)), {});
    const size_t dbase = m.code_offset + m.bytecode.size();

    auto rdF32 = [&](size_t o) -> float {
        uint32_t v = ((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) |
            ((uint32_t)d[o + 2] << 8) | d[o + 3];
        float fv; memcpy(&fv, &v, 4);
        return fv;
        };
    auto plausible = [&](float x, float y, float z) {
        if (!(x > -3000 && x < 3000)) return false;
        if (!(y > -500 && y < 3000)) return false;
        if (!(z > -3000 && z < 3000)) return false;
        float mag = fabsf(x) + fabsf(y) + fabsf(z);
        return mag > 1.0f && mag < 5000.0f;
        };

    // Commands around the setup block (the SETTER table precedes the comparison)
    for (const CSCommand& c : m.all_commands) {
        if (c.offset + 0x80 < setup || c.offset > setup + 0x180) continue;
        for (const CSArg& a : c.args) {
            if (a.kind != CSArgKind::Offset) continue;
            size_t t = dbase + a.u;
            if (t + 0x40 > d.size()) continue;
            size_t lim = t + 0x200;
            if (lim > d.size()) lim = d.size();
            for (size_t o = t; o + 12 <= lim; o++) {
                float x = rdF32(o), y = rdF32(o + 4), z = rdF32(o + 8);
                if (plausible(x, y, z)) {
                    out_pos[0] = x; out_pos[1] = y; out_pos[2] = z;
                    return true;
                }
            }
        }
    }
    return false;
}

std::vector<CSInstr> CutsceneScriptParser::DisasmAt(const CutsceneScript& s,
    uint32_t offset, int max_instrs) {
    std::vector<CSInstr> out;
    uint32_t pos = offset;
    for (int i = 0; i < max_instrs && pos < s.bytecode.size(); i++) {
        CSInstr ins;
        int sz = DisasmOne(s, pos, ins);
        out.push_back(ins);
        if (sz <= 0) break;
        pos += sz;
    }
    return out;
}