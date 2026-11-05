#pragma once
#include <cstdint>
#include <map>
#include <string>
#include <vector>

// Event (.e) script reader, built from the container layout the game's loader uses
// (Xbox 360 sub_820FF9C8 / PS3 equivalent) instead of byte-pattern guesses:
//
//   +0x00 u32 magic 0x181   +0x0C u32 total size   +0x10 u32 image size - 0x18
//   +0x14 u32 reloc offset (from image end)
//   image  = file[0x18 .. 0x18 + hdr[0x10]]   bytecode, run from +0x18
//   bulk   = image end .. reloc base          NMTN / NOBJ / NMDL / Mefc / CSL / BTX
//   list A = u32 count + u32 sites            pointer operands patched with the image base
//   list B = u32 count + u32 sites            pointer operands patched with the BULK base
//   block2 = four {u32 count, {u32 id, u32 site}[count]} tables:
//            0 = native ids (opcode 7d), 1 = lib.e imports (7a/07), 2 = exports, 3 = ?
//
// A pointer operand is therefore always classified exactly, and every asset the script
// touches (motion, model, effect, voice directory, text) is a known bulk offset.
//
// The VM is a single-accumulator stack machine (opcode table: EternalSonataReprise
// docs/script-vm-notes.md §8). Event scripts are straight-line code: almost every
// action is a call into a lib.e export (see LibSym below), and time only advances
// in  sleep(ceil(T / (300 / fps)))  so T is in 300 Hz engine ticks.
namespace es {

enum class PtrKind { None, Native, Import, Bulk, Image, Raw };

struct Ptr {
    PtrKind  kind = PtrKind::None;
    uint32_t value = 0;     // native id / import id / raw operand
    uint32_t fileoff = 0;   // resolved file offset for Bulk / Image
};

// Symbolic accumulator / stack value
struct Val {
    enum Kind { None, Num, PtrV, Ret, Expr } kind = None;
    double   num = 0.0;     // Num
    uint32_t raw = 0;       // raw 32-bit operand (colours, flags)
    Ptr      ptr;           // PtrV, or the callee for Ret
    // Expression helpers
    bool     has_lead = false;  // left-most numeric leaf (sleep tick constant)
    double   lead = 0.0;
    bool     is_global_deref = false;   // *(500-series state slot)
    uint32_t global_id = 0;
    bool     is_cmp_eq_global = false;  // (*global == const)
    double   cmp_const = 0.0;
};

struct Call {
    enum Type { CallFn, Native, Sleep, Branch, Goto, Return } type = CallFn;
    uint32_t         site = 0;     // file offset of the opcode
    Ptr              fn;           // CallFn: Image or Import; Native: Native
    std::vector<Val> args;         // first argument first (reverse push order)
    double           sleep_ticks = -1.0;  // Sleep: ticks (300 Hz), <0 unknown
    double           sleep_frames = -1.0; // Sleep with a literal frame count
    uint32_t         target = 0;   // Branch/Goto target (file offset)
    Val              cond;         // Branch condition
};

class Image {
public:
    bool Load(const std::vector<uint8_t>& file);
    bool valid = false;

    const std::vector<uint8_t>& Data() const { return *m_File; }
    uint32_t ImageEnd() const { return m_ImageEnd; }
    uint32_t RelocBase() const { return m_RelocBase; }
    const std::vector<std::pair<uint32_t, uint32_t>>& Table(int i) const { return m_Tables[i]; }

    Ptr ResolvePtr(uint32_t site) const;
    // Linear decompile of [start, end): rebuilds calls from the push sequences.
    std::vector<Call> Decompile(uint32_t start, uint32_t end) const;
    // Offset of an exported symbol (block2 table 2), or 0
    uint32_t ExportOffset(uint32_t sym) const;

    uint32_t U32(uint32_t o) const;
    uint16_t U16(uint32_t o) const;
    float    F32(uint32_t o) const;
    std::string CStr(uint32_t o, size_t max = 64) const;
    std::string Tag(uint32_t o) const;
    // NMDL name of a NOBJ/NMDL at o ("" if none)
    std::string ModelName(uint32_t o) const;

private:
    const std::vector<uint8_t>* m_File = nullptr;
    std::vector<uint8_t> m_Own;
    uint32_t m_ImageEnd = 0, m_RelocBase = 0;
    std::vector<uint32_t> m_RelA, m_RelB;
    std::vector<std::pair<uint32_t, uint32_t>> m_Tables[4];
    std::map<uint32_t, uint32_t> m_NativeAt, m_ImportAt;  // site -> id
    std::vector<bool> m_IsA, m_IsB;                        // per file offset
};

// ---------------------------------------------------------------------------------
// Timeline

enum class ObjKind { Unknown, Actor, Model, Effect, Camera, Light4, Light5, Sound };

struct EvObject {
    uint16_t    id = 0;
    ObjKind     kind = ObjKind::Unknown;
    int         char_id = 0;      // Actor: native 1141 id (-10..-12 party, -20..-28 appkeep2)
    uint32_t    ref = 0;          // Model/Effect: file offset of the bulk NOBJ/NMDL/Mefc
    std::string tag, name;        // chunk tag + NMDL name
    double      t_create = 0.0;
    float       light_pos[3] = { 0,0,0 }, light_rot[3] = { 0,0,0 };
    uint32_t    light_color = 0;  // ARGB
};

struct EvRecord {
    enum Type {
        Pos, Rot, Target, Scale, CamParam, CamActivate, Motion, LookAt, LookAtObj,
        LookAtRelease, MoveTo, AttachBone, Attach, LightClear, LightLink, Text, Say,
        EffectCmd, FadeSet, FadeIn, FadeOut, Music, Delete, NodeVisible, RenderFlag, Flash, Other
    } type = Other;
    double   t = 0.0;
    uint16_t obj = 0;
    float    v[6] = { 0,0,0,0,0,0 };
    // camera
    int      idx = 0;            // CamParam index (2 = focal mm, 1 = focus distance); RenderFlag: native id
    float    blend = 0.0f;       // CamActivate: blend seconds
    float    curve = 0.0f;
    // motion (lib.e sym20060 -> natives 1113 + 1108 cmd 19)
    uint32_t nmtn = 0;           // file offset of the NMTN chunk (0 = model's own table)
    uint32_t flags = 0;
    int      blend_ticks = 0;    // 4th argument
    int      start_ticks = 0;    // 5th argument
    float    speed = 1.0f;
    // misc
    uint16_t other = 0;          // parent / target / light object, bone index
    int      text_id = -1;
    uint32_t color = 0;
    std::string str;             // text, effect command, node name, music, native name
};

struct EventTimeline {
    bool        valid = false;
    double      duration = 0.0;
    uint32_t    main_fn = 0;
    std::map<uint16_t, EvObject> objects;
    std::vector<EvRecord> records;       // time order (stable)
    std::map<std::string, int> unresolved; // callee -> count
};

// lang: "USA", "GBR", "FRA", "ITA", "DEU", "ESP", "JPN"
EventTimeline BuildTimeline(const Image& img, const std::string& lang = "GBR");

// String from a BTX blob at file offset btx (language fourcc without the trailing space)
std::string BtxString(const Image& img, uint32_t btx, uint32_t id, const std::string& lang);

// Checks the lib.e exports this reader relies on still call the expected natives.
// Returns human-readable mismatches (empty = all good).
std::vector<std::string> VerifyLib(const Image& lib);

// ---------------------------------------------------------------------------------
// Host map setup: the map script branches on the current-event slot ((*state == EV))
// and spawns the scene for that event (tnt03 for E0005: map, sky set, props, effects).
struct MapObject {
    uint32_t    ref = 0;          // file offset in the MAP file
    std::string tag, name;
    float       pos[3] = { 0,0,0 }, rot[3] = { 0,0,0 }, scale[3] = { 1,1,1 };
};
struct MapSetup {
    bool        found_branch = false;
    uint32_t    branch_site = 0;
    uint32_t    default_map = 0;  // NOBJ whose NMDL is named like the map file
    std::vector<MapObject> objects;
};
MapSetup ExtractMapSetup(const Image& map, uint32_t event_no, const std::string& map_stem);

// ---------------------------------------------------------------------------------
// Which map plays an event. Event files never name their map; map scripts do:
//   Branch : the map tests (*state == EV) to set up its scene for EV (tnt03 for E0005)
//   Start  : lib.e sym20018(map, kind, EV, part, ...) starts EV (part) on "map.e", or on
//            the calling map when the first argument is 0 (it builds "E%04d.e" /
//            "E%04d_%03d.e" and changes map)
struct EventRef {
    enum Kind { Branch, Start } kind = Branch;
    uint32_t    event = 0, part = 0;
    std::string map;        // destination map stem ("tnt01")
    std::string from;       // map file that holds the reference
};
// Reads only the image and the relocation tail of a map file (plus the few strings it
// needs), so a whole cfdata folder scans quickly.
std::vector<EventRef> ScanMapEvents(const std::string& path);
// "e2010_010.e" / "cfdata_e2010_010.e" -> 2010 / 10; false if not an event file name
bool EventNumberFromPath(const std::string& path, uint32_t& event, uint32_t& part);
// "C:/x/cfdata_agn01.e" -> "agn01" (lower case, a flattened "cfdata_" prefix dropped)
std::string MapStemFromPath(const std::string& path);

const char* ObjKindName(ObjKind k);
const char* RecordName(EvRecord::Type t);

} // namespace es
