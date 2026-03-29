#include "AIScriptParser.h"
#include <fstream>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <sstream>

// Primitives
uint32_t AIScriptParser::ReadU32BE(const uint8_t* d) {
    return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | (uint32_t)d[3];
}
float AIScriptParser::ReadF32BE(const uint8_t* d) {
    uint32_t u = ReadU32BE(d); float f; memcpy(&f, &u, 4); return f;
}
bool AIScriptParser::IsAIScript(const uint8_t* data, size_t size) {
    if (size < 4) return false;
    return ReadU32BE(data) == AI_SCRIPT_MAGIC;
}

// Named globals
std::string AIScriptParser::GlobalName(uint8_t reg) {
    switch (reg) {
    case 0x08: return "r_target";
    case 0x0C: return "r_state";
    case 0x10: return "r_res1";
    case 0x14: return "r_res2";
    case 0x18: return "r_res3";
    case 0x1C: return "r_res4";
    case 0xC0: return "g_party_data";
    case 0xC8: return "g_C8";
    case 0xCC: return "g_CC";
    case 0xD0: return "g_frame_ctr";
    case 0xD4: return "g_D4";
    case 0xD8: return "g_D8";
    case 0xDC: return "g_DC";
    case 0xE0: return "g_timer_global";
    case 0xE4: return "g_combat_flags";
    case 0xE8: return "g_dist_timer";
    case 0xEC: return "g_result_timer";
    case 0xF0: return "g_handler_id";
    case 0xF4: return "g_action_id";
    case 0xF8: return "g_prev_action";
    case 0xFC: return "g_special_slot";
    default: { char buf[8]; snprintf(buf, 8, "g[%02X]", reg); return buf; }
    }
}

std::string AIScriptParser::SharedName(uint8_t addr) {
    switch (addr) {
    case 0x54: return "gs_timer54";
    case 0x58: return "gs_atk_slot";
    case 0x5B: return "gs_5B";
    case 0x5C: return "gs_5C";
    case 0x60: return "gs_60";
    case 0x67: return "gs_atk_coord";
    case 0x68: return "gs_pos_state";
    case 0x70: return "gs_70";
    case 0x74: return "gs_approach_thr";
    case 0x78: return "gs_formation_bm";
    case 0x7C: return "gs_form_slot";
    default: { char buf[12]; snprintf(buf, 12, "gs[%02X]", addr); return buf; }
    }
}

std::string AIScriptParser::ActionName(uint8_t action_id) {
    switch (action_id) {
    case 0x0A: return "Walk / Approach Target";
    case 0x0C: return "Turn to Target";
    case 0x1E: return "Guard / Block";
    case 0x58: return "Flank Back";
    case 0x5A: return "Flank Left (A-F-L)";
    case 0x5C: return "Flank Right (A-F-R)";
    case 0x5E: return "Forward Attack";
    case 0x60: return "Special Attack";
    default: { char buf[12]; snprintf(buf, 12, "Action_%02X", action_id); return buf; }
    }
}

std::string AIScriptParser::HandlerName(uint8_t handler_id) {
    switch (handler_id) {
    case 0x22: return "handler_walk";
    case 0x23: return "handler_turn";
    case 0x24: return "handler_guard";
    case 0x25: return "handler_flank_back";
    case 0x26: return "handler_flank_left";
    case 0x27: return "handler_flank_right";
    case 0x28: return "handler_fwd_attack";
    case 0x29: return "handler_special";
    default: { char buf[12]; snprintf(buf, 12, "handler_%02X", handler_id); return buf; }
    }
}

std::string AIScriptParser::CondStatus(uint8_t v) {
    switch (v) {
    case 0x0C: return "RUNNING";
    case 0x10: return "SUCCESS";
    case 0x14: return "FAILURE";
    case 0x18: return "BLOCKED";
    case 0x20: return "SPECIAL";
    default: { char buf[8]; snprintf(buf, 8, "0x%02X", v); return buf; }
    }
}

// Float constant meanings
std::string AIScriptParser::FloatMeaning(float v) {
    auto near = [&](float r) { return fabsf(v - r) < r * 0.003f + 0.0001f; };
    if (near(0.01745f))  return "PI/180 (deg→rad)";
    if (near(0.5236f))   return "PI/6 = 30° FOV";
    if (near(0.7854f))   return "PI/4 = 45° FOV";
    if (near(0.8727f))   return "~50° flank angle";
    if (near(3.1416f))   return "PI (180°)";
    if (near(6.2832f))   return "2*PI (360°)";
    if (near(3.1241f))   return "~179° (behind target)";
    if (near(3.1590f))   return "~181° (behind target)";
    if (near(600.0f))    return "dist: detection range";
    if (near(300.0f))    return "dist: long range";
    if (near(100.0f))    return "dist: medium range";
    if (near(30.0f))     return "dist: short range";
    if (near(10.0f))     return "dist: attack range";
    if (near(8.0f))      return "dist: melee range";
    if (near(6.0f))      return "dist: close melee";
    if (near(5.0f))      return "dist: very close / formation";
    if (near(0.8f))      return "prob: 80%";
    if (near(0.65f))     return "prob: 65%";
    if (near(0.5f))      return "prob: 50%";
    if (near(0.4f))      return "prob: 40%";
    if (near(0.3f))      return "prob: 30%";
    if (near(0.2f))      return "prob: 20%";
    if (near(1.0f))      return "1.0 (unit)";
    if (near(0.01f))     return "epsilon";
    return "";
}

// API address catalog  (all 208 known addresses)
std::string AIScriptParser::ApiMeaning(uint32_t addr, int call_count, bool is_complex) {
    if (addr == 0)
        return "execute_action()  [reads g_special_slot → dispatch]";

    //Group A: boss/simple-enemy special abilities
    if (addr == 0x0021F0) return "default_special_ability()";
    if (addr == 0x0022B8) return "bos13_special_ability()";
    if (addr == 0x002358) return "bos09_special_ability()";
    if (addr == 0x00240C) return "em15_special_ability()";
    if (addr == 0x002438) return "bos01_special_ability()";
    if (addr == 0x00246C) return "em16v4_special_ability()";
    if (addr == 0x002494) return "bos12_special_ability()";
    if (addr == 0x002498) return "em08_special_ability()";
    if (addr == 0x0024D4) return "em12v3_special_ability()";
    if (addr == 0x0024F0) return "bos03_special_ability()";
    if (addr == 0x00253C) return "bos11_special_ability()";
    if (addr == 0x002588) return "em07v1_special_ability()";
    if (addr == 0x00258C) return "em27_special_ability()";
    if (addr == 0x0025BC) return "bosfga_special_ability()";
    if (addr == 0x0025C8) return "bos05_special_ability()";
    if (addr == 0x0025D0) return "em12v1_special_ability()";
    if (addr == 0x002618) return "bos02_special_ability()";
    if (addr == 0x002634) return "bos04_special_ability()";
    if (addr == 0x0026FC) return "bostub_special_ability()";
    if (addr == 0x0027A8) return "bos07_special_ability()";
    if (addr == 0x0027E0) return "bos10_special_ability()";
    if (addr == 0x002854) return "bos06_special_ability()";
    if (addr == 0x002980) return "bos08_special_ability()";
    if (addr == 0x002A80) return "bosrnd_special_ability()";
    if (addr == 0x002AB4) return "boswlz_special_ability()";
    if (addr == 0x002B28) return "boscpn_special_ability()";
    if (addr == 0x002B70) return "bosfga2_special_ability()";

    // complex-enemy function tables
    // em12 (some unique maths functions)
    if (addr == 0x002FB0) return "em12_math_fn_A()";
    if (addr == 0x002FB4) return "em12_math_fn_B()";
    if (addr == 0x003044) return "em12_init_flag()";
    if (addr == 0x0030B8) return "em12_special_ability()";
    if (addr == 0x00314C) return "em12_timer_event()";

    // Helper macro for vtable patterns
    struct EnemyTable { uint32_t base; const char* name; };
    static const EnemyTable tables[] = {
        {0x36F4, "em09"},
        {0x3734, "em17"},
        {0x37A0, "em11"},
        {0x37C0, "em22"},
        {0x37C8, "em13"},
        {0x3810, "em26"},
        {0x381C, "em25"},
        {0x3948, "em19"},
        {0x3A4C, "em10"},
        {0x3AD4, "em07v2"},
        {0x3B44, "em24"},
        {0x3BD0, "em20"},
        {0x3BEC, "em23"},
        {0x3C38, "em14"},
        {0x3C94, "bos16"},
        {0x3CD4, "em16"},
        {0x3E9C, "em21"},
        {0,      nullptr}
    };

    for (int i = 0; tables[i].name; i++) {
        uint32_t base = tables[i].base;
        if (addr < base || addr > base + 0x80) continue;
        uint32_t off = addr - base;
        char buf[64];
        switch (off) {
        case 0x00: snprintf(buf, sizeof(buf), "%s_init_flag()", tables[i].name); return buf;
        case 0x04: snprintf(buf, sizeof(buf), "%s_get_speed()", tables[i].name); return buf;
        case 0x08: snprintf(buf, sizeof(buf), "%s_get_range()", tables[i].name); return buf;
        case 0x0C: snprintf(buf, sizeof(buf), "%s_fn_0C()", tables[i].name); return buf;
        case 0x10: snprintf(buf, sizeof(buf), "%s_seek_target()", tables[i].name); return buf;
        case 0x14: snprintf(buf, sizeof(buf), "%s_fn_14()", tables[i].name); return buf;
        case 0x18: snprintf(buf, sizeof(buf), "%s_combat_state()", tables[i].name); return buf;
        case 0x1C: snprintf(buf, sizeof(buf), "%s_combat_state2()", tables[i].name); return buf;
        case 0x20: snprintf(buf, sizeof(buf), "%s_fn_20()", tables[i].name); return buf;
        case 0x28: snprintf(buf, sizeof(buf), "%s_attack_range()", tables[i].name); return buf;
        case 0x2C: snprintf(buf, sizeof(buf), "%s_fn_2C()", tables[i].name); return buf;
        case 0x30: snprintf(buf, sizeof(buf), "%s_approach_seq()", tables[i].name); return buf;
        case 0x3C: snprintf(buf, sizeof(buf), "%s_special_seq()", tables[i].name); return buf;
        case 0x40: snprintf(buf, sizeof(buf), "%s_special_seq2()", tables[i].name); return buf;
        case 0x44: snprintf(buf, sizeof(buf), "%s_timer_event()", tables[i].name); return buf;
        case 0x48: snprintf(buf, sizeof(buf), "%s_fn_48()", tables[i].name); return buf;
        case 0x60: snprintf(buf, sizeof(buf), "%s_special_ability()", tables[i].name); return buf;
        case 0x6C: snprintf(buf, sizeof(buf), "%s_special_ability()", tables[i].name); return buf;
        default:   snprintf(buf, sizeof(buf), "%s_fn_%02X()", tables[i].name, (unsigned)off); return buf;
        }
    }

    // Secondary sub-tables for em09 (has two blocks)
    if (addr >= 0x371C && addr <= 0x3734) {
        char buf[48];
        snprintf(buf, sizeof(buf), "em09_combat_fn_%02X()", (unsigned)(addr - 0x371C));
        return buf;
    }

    // Generic fallback by range
    if (addr >= 0x2200 && addr <= 0x2C00) return "boss_special_ability [game binary]";
    if (addr >= 0x2C00 && addr <= 0x3600) return "game_fn [unknown]";
    if (addr >= 0x3600 && addr <= 0x4200) {
        if (call_count >= 20) return "combat_math_fn (high-freq)";
        if (call_count >= 8)  return "combat_state_fn (med-freq)";
        return "enemy_fn [unknown]";
    }
    return "[unknown]";
}

// INSTRUCTION DECODER
//
// ISA summary:
//   0x81 XX [...]  — extended prefix opcodes (2–6 bytes)
//   0x09 XX        — PUSH_ADDR g[XX]  (write-mode, local frame)
//   0x08 FF FF FF XX — PUSH_ADDR g_shared[XX] (write-mode, shared world)
//   0x07 [3-byte]  — JMP (absolute 24-bit)
//   0x7A [3-byte]  — JMPNODE (24-bit node index, short form)
//   0x74/75/76 [3b]— JFALSE / JTRUE / JCOND
//   0x78/79 [1b]   — BRT/BRF (forward branch)
//   0x77/7E [1b]   — JMP.BACK / LOOP.BACK (backward branch)
//   0x7C [1b]      — MASK #XX
//   0x7D [4b]      — RETURN u32
//   0x85/86/87     — IFTRUE / IFFALSE / IFEITH
//   0x88 [1b]      — COND #status
//   0x89 00 00 00 XX — SWITCH #XX (dispatch table, XX cases)
//   0x89 (else)    — GETRES_89 (checkpoint / getres variant after JMPNODE)
//   0x98           — GETRES  (get result of last JMPNODE)
//   0x9A           — GETRES_ALT (same as GETRES, used after JMPNODE N24)
//   0x24           — STORE
//   0x32/33        — MOV / MOVC
//   0x22/1E/1C/28/2C — EQ / NE / LT / GT / LE
//   0x3A/35/36/30  — ADD / SUB / ABS / MUL
//   0x3E           — CMP
//   0x47/49        — SETBIT / CLRBIT
//   0x0C           — YIELD
//   0x57 [1b]      — CMP_TIMER (compare timer/counter)
//   0x5D/60/64/67  — STFLAG / GETFL / SETFG / GETST (flag & state ops, 1-byte operand)
//   0x6B/6C/6E/6F  — LOOPN / MODF / SETST / SETST2
//   0x73 [1b]      — TEST #XX
//   0x4F [1b]      — POPN #N  (pop N values from stack into globals)
int AIScriptParser::DisasmOne(const std::vector<uint8_t>& bc, uint32_t pos, AIInstr& out) {
    out.offset = pos;
    out.is_jump = false;
    out.is_call = false;
    out.target = 0;
    out.uses_shared_mem = false;
    out.uses_global = false;
    out.is_action_write = false;
    memset(out.raw, 0, sizeof(out.raw));

    const int N = (int)bc.size();
    auto safe = [&](int off) -> uint8_t {
        int idx = (int)pos + off;
        return (idx >= 0 && idx < N) ? bc[idx] : 0;
        };
    auto u24 = [&](int off) -> uint32_t {
        return ((uint32_t)safe(off) << 16) | ((uint32_t)safe(off + 1) << 8) | safe(off + 2);
        };
    auto u32 = [&](int off) -> uint32_t {
        return ((uint32_t)safe(off) << 24) | ((uint32_t)safe(off + 1) << 16) |
            ((uint32_t)safe(off + 2) << 8) | safe(off + 3);
        };
    auto f32 = [&](int off) -> float {
        uint32_t v = u32(off); float f; memcpy(&f, &v, 4); return f;
        };
    auto hex4 = [](uint32_t v) -> std::string {
        char buf[16]; snprintf(buf, sizeof(buf), "0x%04X", (unsigned)v); return buf;
        };
    auto hex6 = [](uint32_t v) -> std::string {
        char buf[16]; snprintf(buf, sizeof(buf), "0x%06X", (unsigned)v); return buf;
        };

    uint8_t b = safe(0);
    int len = 1;

    //0x81 extended prefix
    if (b == 0x81) {
        uint8_t sub = safe(1);
        switch (sub) {

        case 0x18: {           // PUSH local frame register (read)
            uint8_t reg = safe(2);
            std::string nm = GlobalName(reg);
            out.mnemonic = "PUSH";
            out.operand = nm;
            out.uses_global = true;
            len = 3; break;
        }
        case 0x01:             // PUSH immediate byte
            out.mnemonic = "PUSH";
            out.operand = "#" + std::to_string(safe(2));
            len = 3; break;

        case 0x02:
            out.mnemonic = "PUSH";
            out.operand = "#" + std::to_string(safe(2)) + "b";
            len = 3; break;

        case 0x03: {           // PUSH.F IEEE 754 float BE
            float fv = f32(2);
            char buf[32]; snprintf(buf, sizeof(buf), "%g", fv);
            std::string meaning = FloatMeaning(fv);
            out.mnemonic = "PUSH.F";
            out.operand = std::string(buf) + (meaning.empty() ? "" : "  ; " + meaning);
            len = 6; break;
        }
        case 0x09: {           // LDVAL g[XX]  (read value from local frame)
            uint8_t reg = safe(2);
            out.mnemonic = "LDVAL";
            out.operand = GlobalName(reg);
            out.uses_global = true;
            len = 3; break;
        }
        case 0x0C: {           // CALLAPI <24-bit address>
            uint32_t addr = u32(2);
            out.mnemonic = "CALLAPI";
            out.operand = hex6(addr);
            out.is_call = true;
            out.target = addr;
            len = 6; break;
        }
        case 0x07: {           // JMP (long form)
            uint32_t addr = u24(2);
            out.mnemonic = "JMP";
            out.operand = hex4(addr);
            out.is_jump = true;
            out.target = addr;
            len = 5; break;
        }
        case 0x7A: {           // JMPNODE (long form)
            uint32_t n = u24(2);
            out.mnemonic = "JMPNODE";
            out.operand = "N" + std::to_string(n);
            out.is_call = true;
            out.target = n;
            len = 5; break;
        }
        case 0x7D: {           // PUSH.I int32
            uint32_t v = u32(2);
            char buf[32]; snprintf(buf, sizeof(buf), "%u", v);
            out.mnemonic = "PUSH.I";
            out.operand = buf;
            len = 6; break;
        }
        case 0x12: {           // PUSH.SI signed int32
            int32_t sv; uint32_t uv = u32(2); memcpy(&sv, &uv, 4);
            char buf[32]; snprintf(buf, sizeof(buf), "%d", sv);
            out.mnemonic = "PUSH.SI";
            out.operand = buf;
            len = 6; break;
        }
        case 0x7B: {           // PUSH.FLAGS 32-bit bitmask
            char buf[16]; snprintf(buf, sizeof(buf), "0x%08X", u32(2));
            out.mnemonic = "PUSH.FLAGS";
            out.operand = buf;
            len = 6; break;
        }
        case 0x16: {           // PUSH word immediate
            out.mnemonic = "PUSH.W";
            out.operand = "#" + std::to_string(safe(2));
            len = 3; break;
        }
        case 0x08: {           // PUSH #byte (another immediate form)
            out.mnemonic = "PUSH";
            out.operand = "#" + std::to_string(safe(2)) + "x";
            len = 3; break;
        }
        default: {
            char buf[16]; snprintf(buf, sizeof(buf), "81_%02X", sub);
            out.mnemonic = buf;
            out.operand = "";
            len = 2; break;
        }
        }
    }

    //Standalone 0x09 XX = PUSH_ADDR &g[XX] (write mode)
    else if (b == 0x09) {
        uint8_t reg = safe(1);
        out.mnemonic = "PUSH_ADDR";
        out.operand = GlobalName(reg);
        out.uses_global = true;
        // Mark if writing to key globals
        if (reg == 0xF4 || reg == 0xF0)
            out.is_action_write = true;
        len = 2;
    }

    //0x08 FF FF FF XX = PUSH_ADDR &g_shared[XX]
    else if (b == 0x08 &&
        safe(1) == 0xFF && safe(2) == 0xFF && safe(3) == 0xFF) {
        uint8_t addr = safe(4);
        out.mnemonic = "PUSH_ADDR";
        out.operand = SharedName(addr);
        out.uses_shared_mem = true;
        len = 5;
    }

    //Standard control flow
    else if (b == 0x07) {
        uint32_t addr = u24(1);
        out.mnemonic = "JMP";
        out.operand = hex4(addr);
        out.is_jump = true;
        out.target = addr;
        len = 4;
    }
    else if (b == 0x7A) {
        uint32_t n = u24(1);
        out.mnemonic = "JMPNODE";
        out.operand = "N" + std::to_string(n);
        out.is_call = true;
        out.target = n;
        len = 4;
    }
    else if (b == 0x74) {
        uint32_t a = u24(1);
        out.mnemonic = "JFALSE"; out.operand = hex4(a);
        out.is_jump = true; out.target = a; len = 4;
    }
    else if (b == 0x75) {
        uint32_t a = u24(1);
        out.mnemonic = "JTRUE"; out.operand = hex4(a);
        out.is_jump = true; out.target = a; len = 4;
    }
    else if (b == 0x76) {
        uint32_t a = u24(1);
        out.mnemonic = "JCOND"; out.operand = hex4(a);
        out.is_jump = true; out.target = a; len = 4;
    }
    else if (b == 0x78) {
        uint32_t skip = safe(1);
        char buf[32]; snprintf(buf, sizeof(buf), "-> 0x%04X", pos + 2 + skip);
        out.mnemonic = "BRT";  out.operand = buf;
        out.is_jump = true; out.target = pos + 2 + skip; len = 2;
    }
    else if (b == 0x79) {
        uint32_t skip = safe(1);
        char buf[32]; snprintf(buf, sizeof(buf), "-> 0x%04X", pos + 2 + skip);
        out.mnemonic = "BRF";  out.operand = buf;
        out.is_jump = true; out.target = pos + 2 + skip; len = 2;
    }
    else if (b == 0x77) {
        uint32_t back = safe(1);
        char buf[32]; snprintf(buf, sizeof(buf), "-> 0x%04X", (pos + 2 > back) ? (pos + 2 - back) : 0);
        out.mnemonic = "JMP.B"; out.operand = buf;
        out.is_jump = true; out.target = (pos + 2 > back) ? (pos + 2 - back) : 0; len = 2;
    }
    else if (b == 0x7E) {
        uint32_t back = safe(1);
        char buf[32]; snprintf(buf, sizeof(buf), "-> 0x%04X", (pos + 2 > back) ? (pos + 2 - back) : 0);
        out.mnemonic = "LOOP.B"; out.operand = buf;
        out.is_jump = true; out.target = (pos + 2 > back) ? (pos + 2 - back) : 0; len = 2;
    }

    //0x7D = RETURN
    else if (b == 0x7D) {
        uint32_t v = u32(1);
        out.mnemonic = "RETURN";
        out.operand = (v == 0) ? "FAIL" : (v == 1 ? "SUCCESS" : std::to_string(v));
        len = 5;
    }
    //0x7C = MASK
    else if (b == 0x7C) {
        char buf[8]; snprintf(buf, 8, "#%02X", safe(1));
        out.mnemonic = "MASK"; out.operand = buf; len = 2;
    }

    //Branch-tree conditionals
    else if (b == 0x85) { out.mnemonic = "IFTRUE";  out.operand = ""; len = 1; }
    else if (b == 0x86) { out.mnemonic = "IFFALSE"; out.operand = ""; len = 1; }
    else if (b == 0x87) { out.mnemonic = "IFEITH";  out.operand = ""; len = 1; }

    //Result retrieval
    else if (b == 0x98) { out.mnemonic = "GETRES";     out.operand = ""; len = 1; }
    else if (b == 0x9A) { out.mnemonic = "GETRES_ALT"; out.operand = ""; len = 1; } 

    //0x88 = COND #status
    else if (b == 0x88) {
        uint8_t v = safe(1);
        std::string st = CondStatus(v);
        char buf[32]; snprintf(buf, sizeof(buf), "#%02X (%s)", v, st.c_str());
        out.mnemonic = "COND"; out.operand = buf; len = 2;
    }

    //0x89 = SWITCH #XX or GETRES_89
    else if (b == 0x89) {
        if (safe(1) == 0x00 && safe(2) == 0x00 && safe(3) == 0x00) {
            uint8_t n = safe(4);
            char buf[32]; snprintf(buf, sizeof(buf), "#%u (%u cases)", n, n);
            out.mnemonic = "SWITCH"; out.operand = buf; len = 5;
        }
        else {
            out.mnemonic = "GETRES_89";  // 1-byte checkpoint/getres variant
            out.operand = "";
            len = 1;
        }
    }

    //Stack / memory ops
    else if (b == 0x24) { out.mnemonic = "STORE";  out.operand = ""; len = 1; }
    else if (b == 0x32) { out.mnemonic = "MOV";    out.operand = ""; len = 1; }
    else if (b == 0x33) { out.mnemonic = "MOVC";   out.operand = ""; len = 1; }

    //Comparisons
    else if (b == 0x22) { out.mnemonic = "EQ";  out.operand = ""; len = 1; }
    else if (b == 0x1E) { out.mnemonic = "NE";  out.operand = ""; len = 1; }
    else if (b == 0x1C) { out.mnemonic = "LT";  out.operand = ""; len = 1; }
    else if (b == 0x28) { out.mnemonic = "GT";  out.operand = ""; len = 1; }
    else if (b == 0x2C) { out.mnemonic = "LE";  out.operand = ""; len = 1; }
    else if (b == 0x3E) { out.mnemonic = "CMP"; out.operand = ""; len = 1; }

    //Arithmetic
    else if (b == 0x3A) { out.mnemonic = "ADD"; out.operand = ""; len = 1; }
    else if (b == 0x35) { out.mnemonic = "SUB"; out.operand = ""; len = 1; }
    else if (b == 0x36) { out.mnemonic = "ABS"; out.operand = ""; len = 1; }
    else if (b == 0x30) { out.mnemonic = "MUL"; out.operand = ""; len = 1; }

    //Bit manipulation
    else if (b == 0x47) { out.mnemonic = "SETBIT"; out.operand = ""; len = 1; }
    else if (b == 0x49) { out.mnemonic = "CLRBIT"; out.operand = ""; len = 1; }

    //Flow control
    else if (b == 0x0C) { out.mnemonic = "YIELD"; out.operand = ""; len = 1; }

    //Timer compare (0x57 XX)
    else if (b == 0x57) {
        char buf[16]; snprintf(buf, sizeof(buf), "#%u", safe(1));
        out.mnemonic = "CMP_TIMER"; out.operand = buf; len = 2;
    }

    //Flag and state operations (each takes 1-byte operand)
    else if (b == 0x5D) {
        char buf[8]; snprintf(buf, 8, "#%u", safe(1));
        out.mnemonic = "STFLAG"; out.operand = buf; len = 2;
    }
    else if (b == 0x60) {
        char buf[8]; snprintf(buf, 8, "#%u", safe(1));
        out.mnemonic = "GETFL"; out.operand = buf; len = 2;
    }
    else if (b == 0x64) {
        char buf[8]; snprintf(buf, 8, "#%u", safe(1));
        out.mnemonic = "SETFG"; out.operand = buf; len = 2;
    }
    else if (b == 0x67) {
        char buf[8]; snprintf(buf, 8, "#%u", safe(1));
        out.mnemonic = "GETST"; out.operand = buf; len = 2;
    }
    else if (b == 0x6B) {
        char buf[8]; snprintf(buf, 8, "#%u", safe(1));
        out.mnemonic = "LOOPN"; out.operand = buf; len = 2;
    }
    else if (b == 0x6C) {
        char buf[8]; snprintf(buf, 8, "#%u", safe(1));
        out.mnemonic = "MODF"; out.operand = buf; len = 2;
    }
    else if (b == 0x6E) {
        char buf[8]; snprintf(buf, 8, "#%u", safe(1));
        out.mnemonic = "SETST"; out.operand = buf; len = 2;
    }
    else if (b == 0x6F) {
        char buf[8]; snprintf(buf, 8, "#%u", safe(1));
        out.mnemonic = "SETST2"; out.operand = buf; len = 2;
    }
    else if (b == 0x73) {
        char buf[8]; snprintf(buf, 8, "#%u", safe(1));
        out.mnemonic = "TEST"; out.operand = buf; len = 2;
    }

    //0x4F = POPN (pop N values from stack into sequential globals)
    else if (b == 0x4F) {
        char buf[8]; snprintf(buf, 8, "#%u", safe(1));
        out.mnemonic = "POPN"; out.operand = buf; len = 2;
    }

    //Fallback 
    else {
        char buf[8]; snprintf(buf, 8, "???_%02X", b);
        out.mnemonic = buf; out.operand = ""; len = 1;
    }

    // Store raw bytes
    for (int i = 0; i < len && i < 8; i++)
        out.raw[i] = safe(i);
    out.length = len;
    return len;
}

// Disassemble all nodes
void AIScriptParser::DisasmNodes(AIScriptFile& f) {
    uint32_t largest_size = 0;
    uint32_t largest_index = 0;

    for (uint32_t ni = 0; ni < (uint32_t)f.node_offsets.size(); ni++) {
        AINode node;
        node.index = ni;
        node.bc_offset = f.node_offsets[ni];
        node.file_offset = (uint32_t)(f.code_offset + node.bc_offset);

        uint32_t end_bc = (ni + 1 < (uint32_t)f.node_offsets.size())
            ? f.node_offsets[ni + 1]
            : (uint32_t)f.bytecode.size();
        node.size = end_bc - node.bc_offset;

        if (node.size > largest_size) {
            largest_size = node.size;
            largest_index = ni;
        }

        uint32_t pos = node.bc_offset;
        int      guard = 0;
        while (pos < end_bc && pos < (uint32_t)f.bytecode.size() && guard++ < 4000) {
            AIInstr instr;
            int len = DisasmOne(f.bytecode, pos, instr);
            if (len <= 0) { pos++; continue; }

            if (instr.is_call && instr.mnemonic == "JMPNODE")
                node.calls_nodes.push_back(instr.target);
            if (instr.is_call && instr.mnemonic == "CALLAPI") {
                node.api_calls.push_back(instr.target);
                if (instr.target != 0)
                    node.has_special_ability = true;
            }

            node.instrs.push_back(instr);
            pos += len;
        }

        f.nodes.push_back(node);
    }

    f.main_loop_node = largest_index;
    if (largest_index < f.nodes.size())
        f.nodes[largest_index].is_main_loop = true;
}

// Build API catalog
void AIScriptParser::BuildApiCatalog(AIScriptFile& f) {
    std::map<uint32_t, AIApiEntry> catalog;
    for (uint32_t ni = 0; ni < (uint32_t)f.nodes.size(); ni++) {
        for (uint32_t addr : f.nodes[ni].api_calls) {
            auto& e = catalog[addr];
            e.address = addr;
            e.call_count++;
            if (std::find(e.calling_nodes.begin(), e.calling_nodes.end(), ni)
                == e.calling_nodes.end())
                e.calling_nodes.push_back(ni);
        }
    }
    for (auto& [addr, entry] : catalog) {
        bool complex = (f.tier == AIScriptTier::Complex);
        entry.inferred_name = ApiMeaning(addr, entry.call_count, complex);

        // Infer role from call count and address
        if (addr == 0) {
            entry.role = "core";
        }
        else if (addr >= 0x2200 && addr <= 0x2C00) {
            entry.role = "special";
        }
        else if (addr >= 0x3600 && addr <= 0x4200) {
            if (entry.call_count == 1)       entry.role = "init";
            else if (entry.call_count <= 3)   entry.role = "special";
            else if (entry.call_count <= 8)   entry.role = "combat";
            else if (entry.call_count <= 16)  entry.role = "seek";
            else                              entry.role = "math";
        }

        f.api_catalog.push_back(entry);
        f.unique_apis_sorted.push_back(addr);
    }
    std::sort(f.api_catalog.begin(), f.api_catalog.end(),
        [](const AIApiEntry& a, const AIApiEntry& b) {
            return a.call_count > b.call_count;
        });
}

// Decode the SWITCH #24 action registration table
//
// Pattern per entry (in bytecode after SWITCH instruction):
//   09 F4  — PUSH_ADDR &g[F4]
//   81 01 XX 24  — PUSH #XX, STORE  → g[F4] = action_id
//   09 F0  — PUSH_ADDR &g[F0]
//   81 01 YY 24  — PUSH #YY, STORE  → g[F0] = handler_id
//   77 NN  — JMP.BACK -NN  (back to switch loop)
void AIScriptParser::DecodeActionTable(AIScriptFile& f) {
    const auto& bc = f.bytecode;
    const int N = (int)bc.size();

    // Find SWITCH #24 = 89 00 00 00 18
    int sw_pos = -1;
    for (int i = 0; i + 5 <= N; i++) {
        if (bc[i] == 0x89 && bc[i + 1] == 0x00 && bc[i + 2] == 0x00 &&
            bc[i + 3] == 0x00 && bc[i + 4] == 0x18) {
            sw_pos = i; break;
        }
    }
    if (sw_pos < 0) return;

    int pos = sw_pos + 5;

    // Parse up to 32 entries
    while (pos + 11 <= N) {
        // 09 F4
        if (bc[pos] != 0x09 || bc[pos + 1] != 0xF4) break;
        pos += 2;
        // 81 01 XX 24
        if (bc[pos] != 0x81 || bc[pos + 1] != 0x01) break;
        uint8_t action_id = bc[pos + 2];
        pos += 3;
        if (bc[pos] != 0x24) break;
        pos++;
        // 09 F0
        if (bc[pos] != 0x09 || bc[pos + 1] != 0xF0) break;
        pos += 2;
        // 81 01 YY 24
        if (bc[pos] != 0x81 || bc[pos + 1] != 0x01) break;
        uint8_t handler_id = bc[pos + 2];
        pos += 3;
        if (bc[pos] != 0x24) break;
        pos++;
        // 77 NN
        if (bc[pos] != 0x77) break;
        pos += 2;  // skip 77 NN

        AIActionEntry entry;
        entry.action_id = action_id;
        entry.handler_id = handler_id;
        entry.action_name = ActionName(action_id);
        entry.handler_name = HandlerName(handler_id);
        f.action_table.push_back(entry);
    }
}

// Find special ability address and mark key nodes
void AIScriptParser::FindKeyNodes(AIScriptFile& f) {
    // Special ability = any non-zero CALLAPI in a non-simple-loop node
    for (auto& node : f.nodes) {
        for (uint32_t addr : node.api_calls) {
            if (addr != 0) {
                f.special_ability_api = addr;
                break;
            }
        }
        if (f.special_ability_api) break;
    }
}

// String table
std::vector<std::string> AIScriptParser::ParseStringTable(
    const std::vector<uint8_t>& data, size_t start) {

    std::vector<std::string> result;
    size_t pos = start;
    while (pos < data.size()) {
        uint8_t b = data[pos];
        if (b == 0x00 || b == 0x0A) { pos++; continue; }
        if (b < 32 || b > 126) break;
        std::string s;
        while (pos < data.size() && data[pos] >= 32 && data[pos] <= 126)
            s += (char)data[pos++];
        if (s.size() >= 3) result.push_back(s);
    }
    return result;
}

// Node offset table
// (header = 23 fixed u32 VM config words, then node_count offsets)
std::vector<uint32_t> AIScriptParser::ParseNodeOffsets(
    const std::vector<uint8_t>& data, size_t post_code_start,
    uint32_t node_count, uint32_t code_size) {

    std::vector<uint32_t> offsets;

    // Find "Dz" sentinel (0x44 0x7A)
    size_t dz = std::string::npos;
    for (size_t i = post_code_start; i + 1 < data.size(); i++) {
        if (data[i] == 0x44 && data[i + 1] == 0x7A) { dz = i; break; }
    }
    if (dz == std::string::npos) return offsets;

    // 23 fixed header u32s follow, then the node offset table
    size_t tbl = dz + 2 + 23 * 4;

    if (tbl + node_count * 4 > data.size()) {
        // Fallback: scan for the first block of node_count valid offsets
        tbl = dz + 2;
        for (size_t scan = tbl; scan + 4 <= data.size(); scan += 4) {
            size_t run = 0;
            for (size_t j = 0; j < node_count; j++) {
                size_t p = scan + j * 4;
                if (p + 4 > data.size()) break;
                uint32_t v = ReadU32BE(data.data() + p);
                if (v < code_size) run++; else break;
            }
            if (run >= node_count) { tbl = scan; break; }
        }
    }

    for (uint32_t i = 0; i < node_count; i++) {
        size_t p = tbl + i * 4;
        if (p + 4 > data.size()) break;
        offsets.push_back(ReadU32BE(data.data() + p));
    }
    return offsets;
}

// Main parse entry point
AIScriptFile AIScriptParser::Parse(const std::string& filepath) {
    AIScriptFile result;
    result.filename = filepath;

    std::ifstream file(filepath, std::ios::binary);
    if (!file) return result;

    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0);

    std::vector<uint8_t> data(file_size);
    file.read(reinterpret_cast<char*>(data.data()), file_size);
    file.close();

    if (!IsAIScript(data.data(), file_size)) return result;

    result.hash = ReadU32BE(data.data() + 0x04);
    result.secondary_id = ReadU32BE(data.data() + 0x08);
    result.file_size = ReadU32BE(data.data() + 0x0C);
    result.code_size = ReadU32BE(data.data() + 0x10);
    result.node_count = ReadU32BE(data.data() + 0x14);
    result.code_offset = 0x18;

    size_t code_end = result.code_offset + result.code_size;
    if (code_end <= file_size)
        result.bytecode.assign(data.begin() + result.code_offset,
            data.begin() + code_end);

    if (code_end < file_size) {
        result.strings = ParseStringTable(data, code_end);
        result.has_debug_strings = !result.strings.empty();
    }

    result.node_offsets = ParseNodeOffsets(data, code_end,
        result.node_count,
        result.code_size);

    result.tier = (result.node_count > 200 || result.has_debug_strings)
        ? AIScriptTier::Complex
        : AIScriptTier::Simple;

    DisasmNodes(result);
    BuildApiCatalog(result);
    DecodeActionTable(result);
    FindKeyNodes(result);

    result.valid = true;
    return result;
}