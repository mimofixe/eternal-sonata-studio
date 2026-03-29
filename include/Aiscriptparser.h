#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

// Eternal Sonata PS3 — Compiled AI Behaviour Script (.e files)
// Magic: 0x00000181
//
// File layout:
//   [0x00] magic       = 0x00000181
//   [0x04] hash        — changes when bytecode changes
//   [0x08] secondary_id— increments per recompile (v1==v2 semantically identical)
//   [0x0C] file_size
//   [0x10] code_size
//   [0x14] node_count
//   [0x18] bytecode    — custom stack-based VM, variable-width instructions
//   [0x18+code_size]   string table (null/\n separated, complex files only)
//   [Dz sentinel (44 7A)] + [23 × u32BE fixed VM config] + [node_count × u32BE offsets]
//
// VM MEMORY SPACES:
//   Local frame   (09 XX / 81 09 XX)    — addresses 0x08..0xFF (per-entity)
//   Shared world  (08 FF FF FF XX)      — addresses 0x54..0x7C (cross-entity)
//
// LOCAL FRAME REGISTER MAP (key globals):
//   g[C0] = party_data           g[D0] = frame_counter / AI_state
//   g[E0] = timer_global         g[E4] = combat_flags (bitmask)
//   g[E8] = dist_or_timer        g[EC] = combat_result / timer
//   g[F0] = action_handler_id    g[F4] = current_action_id  (most used: 2818x)
//   g[F8] = prev_action / flags  g[FC] = special_ability_slot (1 per file)
//
// SHARED MEMORY MAP (cross-enemy coordination):
//   g_shared[67] = attack_coordination
//   g_shared[68] = position_state
//   g_shared[74] = approach_threshold (compared with 5.0f)
//   g_shared[78] = formation_bitmask  (SETBIT/CLRBIT)
//   g_shared[7C] = formation_slot_ctrl
//
// ACTION REGISTRATION TABLE (SWITCH #24 in node 113 / default.e):
//   g[F4]=0x0A → g[F0]=0x22  WALK / APPROACH TARGET
//   g[F4]=0x0C → g[F0]=0x23  TURN TO TARGET
//   g[F4]=0x1E → g[F0]=0x24  GUARD / BLOCK
//   g[F4]=0x58 → g[F0]=0x25  FLANK BACK
//   g[F4]=0x5A → g[F0]=0x26  FLANK LEFT  (A-F-L)
//   g[F4]=0x5C → g[F0]=0x27  FLANK RIGHT (A-F-R)
//   g[F4]=0x5E → g[F0]=0x28  FORWARD ATTACK
//   g[F4]=0x60 → g[F0]=0x29  SPECIAL ATTACK
//
// g[F0] and g[F4] are SET by the VM and READ by the C engine for dispatch.
// The VM scripts never compare g[F0] — it is purely a C-side handler index.

static const uint32_t AI_SCRIPT_MAGIC = 0x00000181;

// Complexity tier
enum class AIScriptTier {
    Simple,   // 114-115 nodes, no debug strings — shared combat template + 1 unique special
    Complex,  // 370-600+ nodes, debug strings   — full unique behaviour tree
};

// One decoded action registration entry from SWITCH #24 table
struct AIActionEntry {
    uint8_t     action_id;    // stored in g[F4] (e.g. 0x0A, 0x5A, 0x60)
    uint8_t     handler_id;   // stored in g[F0] (C-side function index, 0x22..0x29)
    std::string action_name;  // e.g. "Flank Left (A-F-L)"
    std::string handler_name; // e.g. "handler_flank_left"
};

// Decoded instruction
struct AIInstr {
    uint32_t    offset = 0;
    uint8_t     raw[8] = {};
    int         length = 1;
    std::string mnemonic;
    std::string operand;
    bool        is_jump = false;
    bool        is_call = false;
    uint32_t    target = 0;
    bool        uses_shared_mem = false;   // accesses g_shared[XX]
    bool        uses_global = false;   // accesses g[XX] local frame
    bool        is_action_write = false;   // writes to g[F4] or g[F0]
};

// One behaviour node
struct AINode {
    uint32_t              index = 0;
    uint32_t              bc_offset = 0;   // offset within bytecode
    uint32_t              file_offset = 0;   // absolute offset in file
    uint32_t              size = 0;
    std::vector<AIInstr>  instrs;
    std::vector<uint32_t> calls_nodes;       // JMPNODE targets
    std::vector<uint32_t> api_calls;         // all CALLAPI addresses (incl. 0)
    bool                  has_special_ability = false;  // non-zero CALLAPI present
    bool                  is_main_loop = false;  // the largest node
};

// API catalog entry
struct AIApiEntry {
    uint32_t               address = 0;
    int                    call_count = 0;
    std::string            inferred_name;
    std::string            role;           // "init", "speed", "range", "seek", "special"
    std::vector<uint32_t>  calling_nodes;
};

// Full parsed script
struct AIScriptFile {
    std::string  filename;
    bool         valid = false;

    uint32_t hash = 0;
    uint32_t secondary_id = 0;
    uint32_t file_size = 0;
    uint32_t code_size = 0;
    uint32_t node_count = 0;

    AIScriptTier tier = AIScriptTier::Simple;
    bool         has_debug_strings = false;

    std::vector<std::string>   strings;
    std::vector<uint32_t>      node_offsets;
    std::vector<AINode>        nodes;
    std::vector<AIApiEntry>    api_catalog;
    std::vector<AIActionEntry> action_table;   

    uint32_t special_ability_api = 0;  // Group A: special ability address (simple files)
    uint32_t main_loop_node = 0;  // index of the largest node (main AI loop ~9KB)

    size_t               code_offset = 0x18;
    std::vector<uint8_t> bytecode;

    // API addresses sorted by call count (descending)
    std::vector<uint32_t> unique_apis_sorted;
};

class AIScriptParser {
public:
    static bool         IsAIScript(const uint8_t* data, size_t size);
    static AIScriptFile Parse(const std::string& filepath);

    static std::string  FloatMeaning(float v);
    static std::string  ApiMeaning(uint32_t addr, int call_count, bool is_complex);
    static std::string  GlobalName(uint8_t reg);
    static std::string  SharedName(uint8_t addr);
    static std::string  ActionName(uint8_t action_id);
    static std::string  HandlerName(uint8_t handler_id);
    static std::string  CondStatus(uint8_t v);

private:
    static uint32_t ReadU32BE(const uint8_t* data);
    static float    ReadF32BE(const uint8_t* data);

    static std::vector<std::string> ParseStringTable(
        const std::vector<uint8_t>& data, size_t start);

    static std::vector<uint32_t> ParseNodeOffsets(
        const std::vector<uint8_t>& data, size_t post_code_start,
        uint32_t node_count, uint32_t code_size);

    // Disassemble one instruction; returns byte count consumed (≥1)
    static int DisasmOne(const std::vector<uint8_t>& bc, uint32_t pos, AIInstr& out);

    static void DisasmNodes(AIScriptFile& f);
    static void BuildApiCatalog(AIScriptFile& f);
    static void DecodeActionTable(AIScriptFile& f);
    static void FindKeyNodes(AIScriptFile& f);
};