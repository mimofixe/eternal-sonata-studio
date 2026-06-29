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
    if (size < 0x18) return false;
    if (ReadU32BE(data) != AI_SCRIPT_MAGIC) return false;
    // node_count at 0x14 must be in the range seen across all 120 known AI files (114–600)
    // Container .e files with the same magic have node_count in the millions
    uint32_t node_count = ReadU32BE(data + 0x14);
    return node_count > 0 && node_count <= 2000;
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

    // ── Group A: boss/simple-enemy special abilities ─────────
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

    // ── Group B: complex-enemy function tables ───────────────
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
// === VM de AI: tabela de opcodes (RE do EBOOT runtime, jump table @ 0x3af380) ===
// Stack machine. Fetch 1 byte; opcodes validos 0x00-0x89; > 0x89 ignorados pela VM.
// Largura total = 1 (opcode) + operandos. Derivada por desmontagem dos 138 handlers.
// Familias confirmadas em runtime: 0x81 PUSH, 0x85 POP, 0x18 LOADL, 0x7d PUSHI32, 0x00 NOP.
static const uint8_t AI_OP_W[256] = {
    1, 2, 3, 5, 9, 2, 3, 5, 5, 2, 5, 5, 5, 5, 5, 5,
    5, 5, 5, 5, 5, 5, 2, 2, 2, 2, 2, 2, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 5, 2, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2,
    2, 2, 2, 2, 5, 2, 2, 2, 2, 2, 2, 2, 5, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 5, 1, 5,
    2, 1, 1, 5, 2, 1, 1, 5, 2, 2, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1,
};

// Mnemonica por opcode. Nomes confirmados explicitos; restantes por familia (OPxx/LDxx/STxx)
// ate' a semantica de cada handler ser confirmada individualmente.
static const char* AI_OP_MNEM(uint8_t op) {
    switch (op) {
    case 0x00: return "END";
    case 0x01: return "LDI8";
    case 0x02: return "LDX";
    case 0x03: return "LDX";
    case 0x04: return "OP";
    case 0x05: return "LDX";
    case 0x06: return "LDX";
    case 0x07: return "LDX";
    case 0x08: return "LDLOCX";
    case 0x09: return "LDLOCX";
    case 0x0a: return "LDX";
    case 0x0b: return "LDX";
    case 0x0c: return "LDX";
    case 0x0d: return "OP";
    case 0x0e: return "LDX";
    case 0x0f: return "LDX";
    case 0x10: return "LDLOCX";
    case 0x11: return "LDLOCX";
    case 0x12: return "LDLOCX";
    case 0x13: return "OP";
    case 0x14: return "LDLOCX";
    case 0x15: return "LDLOCX";
    case 0x16: return "LDLOCX";
    case 0x17: return "LDLOCX";
    case 0x18: return "LDLOC";
    case 0x19: return "OP";
    case 0x1a: return "LDLOCX";
    case 0x1b: return "LDLOCX";
    case 0x1c: return "LDX";
    case 0x1d: return "ACCOP";
    case 0x1e: return "ACCOP";
    case 0x1f: return "OP";
    case 0x20: return "LDX";
    case 0x21: return "ACCOP";
    case 0x22: return "STK";
    case 0x23: return "STK";
    case 0x24: return "STK";
    case 0x25: return "STK";
    case 0x26: return "CALLFX";
    case 0x27: return "CALLFX";
    case 0x28: return "I2F";
    case 0x29: return "I2F";
    case 0x2a: return "I2F";
    case 0x2b: return "I2F";
    case 0x2c: return "F2I";
    case 0x2d: return "OP";
    case 0x2e: return "F2I";
    case 0x2f: return "OP";
    case 0x30: return "CMPI";
    case 0x31: return "FCMP";
    case 0x32: return "ACCOP";
    case 0x33: return "FADD";
    case 0x34: return "FADD";
    case 0x35: return "ISUB";
    case 0x36: return "FSUB";
    case 0x37: return "FSUB";
    case 0x38: return "IMUL";
    case 0x39: return "IMUL";
    case 0x3a: return "STK";
    case 0x3b: return "STK";
    case 0x3c: return "ACCOP";
    case 0x3d: return "ACCOP";
    case 0x3e: return "FDIV";
    case 0x3f: return "FDIV";
    case 0x40: return "IMUL";
    case 0x41: return "IMUL";
    case 0x42: return "ACCOP";
    case 0x43: return "ACCOP";
    case 0x44: return "ACCOP";
    case 0x45: return "AND";
    case 0x46: return "XOR";
    case 0x47: return "OR";
    case 0x48: return "ACCOP";
    case 0x49: return "OP";
    case 0x4a: return "OP";
    case 0x4b: return "ACCOP";
    case 0x4c: return "AND";
    case 0x4d: return "LDX";
    case 0x4e: return "LDX";
    case 0x4f: return "LDX";
    case 0x50: return "FADD";
    case 0x51: return "FADD";
    case 0x52: return "LDX";
    case 0x53: return "LDX";
    case 0x54: return "LDX";
    case 0x55: return "LDX";
    case 0x56: return "LDX";
    case 0x57: return "LDX";
    case 0x58: return "FADD";
    case 0x59: return "FADD";
    case 0x5a: return "LDX";
    case 0x5b: return "LDX";
    case 0x5c: return "LDX";
    case 0x5d: return "ISUB";
    case 0x5e: return "FCMP";
    case 0x5f: return "FCMP";
    case 0x60: return "ISUB";
    case 0x61: return "FCMP";
    case 0x62: return "FCMP";
    case 0x63: return "ICMP";
    case 0x64: return "FCMP";
    case 0x65: return "FCMP";
    case 0x66: return "ICMP";
    case 0x67: return "FCMP";
    case 0x68: return "FCMP";
    case 0x69: return "UCMP";
    case 0x6a: return "UCMP";
    case 0x6b: return "ICMP";
    case 0x6c: return "FCMP";
    case 0x6d: return "FCMP";
    case 0x6e: return "ICMP";
    case 0x6f: return "FCMP";
    case 0x70: return "FCMP";
    case 0x71: return "UCMP";
    case 0x72: return "UCMP";
    case 0x73: return "CMPI";
    case 0x74: return "OP";
    case 0x75: return "JCMP";
    case 0x76: return "CMPI";
    case 0x77: return "OP";
    case 0x78: return "CMPI";
    case 0x79: return "CMPI";
    case 0x7a: return "CALLNODE";
    case 0x7b: return "STBASE";
    case 0x7c: return "RET";
    case 0x7d: return "LDI32";
    case 0x7e: return "CMPI";
    case 0x7f: return "CALLFX";
    case 0x80: return "CALLFX";
    case 0x81: return "PUSH";
    case 0x82: return "STK";
    case 0x83: return "ISUB";
    case 0x84: return "ISUB";
    case 0x85: return "DROP";
    case 0x86: return "STK";
    case 0x87: return "STK";
    case 0x88: return "STK";
    case 0x89: return "JNODEC";
    default: return nullptr;
    }
}

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
    auto u32 = [&](int off) -> uint32_t {
        return ((uint32_t)safe(off) << 24) | ((uint32_t)safe(off + 1) << 16) |
            ((uint32_t)safe(off + 2) << 8) | safe(off + 3);
        };
    auto f32 = [&](int off) -> float {
        uint32_t v = u32(off); float f; memcpy(&f, &v, 4); return f;
        };

    uint8_t op = safe(0);

    // Opcode invalido (a VM ignora tudo > 0x89): consome 1 byte como padding.
    if (op > 0x89) {
        char buf[16]; snprintf(buf, sizeof(buf), "BAD_%02X", op);
        out.mnemonic = buf; out.operand = ""; out.length = 1;
        out.raw[0] = op;
        return 1;
    }

    int len = AI_OP_W[op];

    // Mnemonica
    const char* mn = AI_OP_MNEM(op);
    if (mn) {
        out.mnemonic = mn;
    }
    else {
        char buf[16];
        const char* fam = (op <= 0x1b) ? "LD" : (op >= 0x4b && op <= 0x73) ? "ST" : "OP";
        snprintf(buf, sizeof(buf), "%s%02X", fam, op);
        out.mnemonic = buf;
    }

    // Operando conforme largura
    char ob[48];
    if (len == 1) {
        out.operand = "";
    }
    else if (len == 2) {
        // operando de 1 byte
        if (op == 0x18) {            // LOADL: offset local com sinal
            int8_t sb = (int8_t)safe(1);
            snprintf(ob, sizeof(ob), "local[%+d]", (int)sb);
            out.uses_global = true;
        }
        else {
            snprintf(ob, sizeof(ob), "#%u", safe(1));
        }
        out.operand = ob;
    }
    else if (len == 3) {
        uint32_t v = ((uint32_t)safe(1) << 8) | safe(2);
        snprintf(ob, sizeof(ob), "#0x%04X", v);
        out.operand = ob;
    }
    else if (len == 5) {
        // operando de 4 bytes: mostra hex e interpretacao float (heuristica)
        uint32_t v = u32(1);
        float fv = f32(1);
        if (fv > -1e6f && fv < 1e6f && fv != 0.0f) {
            char fb[24]; snprintf(fb, sizeof(fb), "%g", fv);
            snprintf(ob, sizeof(ob), "0x%08X (~%s)", v, fb);
        }
        else {
            snprintf(ob, sizeof(ob), "0x%08X", v);
        }
        out.operand = ob;
    }
    else if (len == 9) {
        // operando de 8 bytes (op 0x04)
        uint32_t hi = u32(1), lo = u32(5);
        snprintf(ob, sizeof(ob), "0x%08X%08X", hi, lo);
        out.operand = ob;
    }

    // Marcar fluxo (saltos de no' e chamadas)
    if (op == 0x7a || op == 0x89) { out.is_jump = true; }   // CALLNODE / JNODEC
    if (op == 0x26 || op == 0x27 || op == 0x7f || op == 0x80) { out.is_call = true; }  // CALLFX
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

// Liga referencias a strings no bytecode (opcode 0x07 LDI32 -> tabela de strings).
// Padrao: 07 <u32 V> 81 ...  onde V indexa o inicio de um token na tabela de strings
// (zona entre fim-do-bytecode e marker 0x447A). Preenche f.string_refs (offset_bc -> texto).
// Regra: V < tamanho_tabela e V==0 ou tabela[V-1]==0 e tabela[V] e' ASCII imprimivel.
void AIScriptParser::AnnotateStringRefs(AIScriptFile& f) {
    f.string_refs.clear();
    const auto& bc = f.bytecode;
    const int N = (int)bc.size();
    // A tabela de strings comeca logo apos o bytecode no ficheiro original.
    // f.string_table_raw guarda esses bytes (offset 0 = primeira string).
    const std::vector<uint8_t>& tbl = f.string_table_raw;
    if (tbl.empty()) return;
    for (int i = 0; i + 6 <= N; i++) {
        if (bc[i] != 0x07 || bc[i + 5] != 0x81) continue;
        uint32_t V = ((uint32_t)bc[i + 1] << 24) | ((uint32_t)bc[i + 2] << 16) |
            ((uint32_t)bc[i + 3] << 8) | bc[i + 4];
        if (V >= tbl.size()) continue;
        bool tok_start = (V == 0) || (tbl[V - 1] == 0x00) || (tbl[V - 1] == 0x0A);
        if (!tok_start) continue;
        if (tbl[V] < 32 || tbl[V] > 126) continue;
        std::string s;
        size_t p = V;
        while (p < tbl.size() && tbl[p] >= 32 && tbl[p] <= 126) s += (char)tbl[p++];
        if (s.size() >= 2) f.string_refs[(uint32_t)i] = s;
    }
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

        // Guardar os bytes crus da tabela de strings (code_end ate' ao marker 0x447A)
        // para resolver as refs do opcode 0x07. Offset 0 = primeira string.
        size_t str_end = file_size;
        for (size_t p = code_end; p + 1 < file_size; p++) {
            if (data[p] == 0x44 && data[p + 1] == 0x7A) { str_end = p; break; }
        }
        if (str_end > code_end)
            result.string_table_raw.assign(data.begin() + code_end,
                data.begin() + str_end);
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
    AnnotateStringRefs(result);
    FindKeyNodes(result);

    result.valid = true;
    return result;
}