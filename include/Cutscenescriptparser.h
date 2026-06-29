#pragma once
#include <string>
#include <vector>
#include <map>
#include <cstdint>

// Eternal Sonata PS3 — Cutscene Script (.e in evdata, magic 0x00000181)
//
// Same stack-machine VM as the AI scripts. Scene semantics:
//   0x7d (5 bytes) = CALLAPI. On disk the operand is 0 and gets RELOCATED at load
//     with the address of a command descriptor (0x52XXXX). The relocation source is
//     the FOOTER table (see below), which pairs each operand site with a numeric
//     API ID. ID -> function via category tables in the eboot (bases 0/200/500/
//     1000/2000/5000/20000). Known IDs: LOAD_MAPA=1002, SETTER=1010, CAMERA=1121,
//     TRANSFORM_ATOR=1037, ACTOR_TABLE=5006, INIT_SLOT=5007, ...
//   0x7a (5 bytes) = CALLNODE. Inline operand = bytecode offset of the target node,
//     or 0 with the target supplied by the footer relocation table.
//   0x07 (5 bytes) = PUSHOFF. File offset of data; some sites are relocated too.
//   Arguments come from the stack (0x03 float / 0x07 offset / 0x02 id / 0x01 i8).
//
// FOOTER (header field +0x14 points into it): [Shift-JIS string table] ...
// [RELOCATION TABLE][00000000 00000000]. The relocation table is a list of u32
// pairs read from the END of the file backwards. Each pair links an operand SITE
// (FILE-relative offset; the byte before it is the opcode 0x7d/0x7a/0x07) with a
// VALUE (API id for 0x7d, node bytecode offset for 0x7a, data ref for 0x07).
// Pair orientation varies, so each side is classified by ground truth (opcode at
// site-1, zeroed operand for 0x7d/0x7a).
//
// IDs pushed by 0x02 (u16): 0x0c01..0x0c09 actor slots, 0x0c40 = LookAt TARGET,
// 300/500/.../1500 = transition durations.
//
// Facts proven by analysis: cutscene files do NOT load the map or the character
// models (no LOAD_MAPA, no ACTOR_TABLE/INIT_SLOT in any cutscene). The map comes
// from the game context (field/system script; eboot map table: 355 entries,
// agn01 = id 16, texture bank AGS). Actor slots are runtime state initialized by
// the FIELD script; the cutscene only drives the slots. Which character fills a
// slot in a given scene is revealed by the embedded NMTN names (plk, cpn, ...).

static const uint32_t CUTSCENE_MAGIC = 0x00000181;

// Kind of argument pushed onto the symbolic stack
enum class CSArgKind {
    Float,      // 0x03 — float value
    Offset,     // 0x07 — offset within the file (pointer to data)
    ActorId,    // 0x02 with 0x0cXX — actor/object id
    Target,     // 0x02 with 0x0c40 — LookAt target
    Duration,   // 0x02 with a time value (300/500/1000/1500...)
    Imm,        // 0x01/0x7d — generic immediate
    Acc,        // 0x81 — accumulator push
    Raw,        // other
};

struct CSArg {
    CSArgKind   kind = CSArgKind::Raw;
    float       f = 0.0f;       // for Float
    uint32_t    u = 0;          // for Offset/ActorId/Imm/Duration
    std::string text;           // readable representation
};

// One disassembled instruction
struct CSInstr {
    uint32_t    offset = 0;     // offset in the bytecode
    uint8_t     op = 0;
    int         length = 1;
    std::string mnemonic;
    std::string operand;
    bool        is_callapi = false;   // opcode 0x7d
    bool        is_call = false;      // 0x7a CALLNODE
    uint32_t    raw_u32 = 0;          // u32 operand (if width >= 5)
    uint32_t    api_id = 0;           // resolved API id (CALLAPI, from footer)
    uint32_t    call_target = 0;      // resolved node target (CALLNODE)
};

// One reconstructed scene command (CALLAPI + args from the stack)
struct CSCommand {
    uint32_t              offset = 0;     // offset of the CALLAPI in the bytecode
    uint32_t              api_addr = 0;   // 0x7d operand (descriptor addr if relocated dump)
    uint32_t              api_id = 0;     // numeric API id (resolved from footer)
    bool                  is_lib = false; // relocated CALLNODE: call into the master
    // script's function LIBRARY (api_id = offset
    // in the master script; proven by debugger:
    // operands patch to master_base + offset)
    std::string           name;           // resolved name, "API_<id>" or "CALLAPI<reloc>"
    std::vector<CSArg>    args;           // arguments read from the stack (push order)
    std::string           summary;        // ready-to-display readable line
};

// One script node (boundaries from CALLNODE targets)
struct CSNode {
    uint32_t                index = 0;
    uint32_t                bc_offset = 0;
    uint32_t                size = 0;
    std::vector<CSInstr>    instrs;
    std::vector<CSCommand>  commands;     // scene commands (CALLAPI) of this node
    std::vector<uint32_t>   calls;        // resolved CALLNODE targets
};

// Full cutscene script
struct CutsceneScript {
    std::string  filename;
    bool         valid = false;

    uint32_t hash = 0;
    uint32_t secondary_id = 0;
    uint32_t file_size = 0;
    uint32_t code_size = 0;
    uint32_t field_14 = 0;          // footer offset

    size_t                  code_offset = 0x18;
    std::vector<uint8_t>    bytecode;

    std::vector<CSNode>     nodes;

    // Footer relocation maps (keys are BYTECODE-relative operand offsets)
    std::map<uint32_t, uint32_t> api_by_site;    // 0x7d site -> API id
    std::map<uint32_t, uint32_t> node_by_site;   // 0x7a site -> node bc offset
    std::map<uint32_t, uint32_t> data_by_site;   // 0x07 site -> data ref
    int reloc_pairs = 0;                        // total pairs parsed
    int reloc_unclassified = 0;                 // pairs that did not classify

    // API usage histogram: id -> count
    std::map<uint32_t, int>  api_usage;

    // Events hosted by this script. A field script tests "if (current_event == EV)"
    // (load game-state slot 0, compare against the constant) before its event setup:
    // agn01 -> 1180 (e1180). Cutscene scripts have none. This is THE event->map
    // association: the map of a cutscene eXXXX is the cfdata map testing event XXXX.
    std::vector<uint32_t>   hosted_events;
    // bytecode offset right after each hosted-event comparison (the setup block)
    std::vector<uint32_t>   hosted_event_setup_offsets;

    // Scene actors derived from the embedded NMTN names. Cutscenes carry NO explicit
    // "load model X" command (slots are filled at runtime by game state); the per-scene
    // character identity is authored into the animation names (e1180_010_plk -> plk).
    // model_hint maps known codes to their model source (root .p3obj / appkeep2.bmd).
    struct CSSceneActor {
        std::string code;        // 3-letter character code (plk, cpn, ...)
        std::string model_hint;  // where the model lives, or "unknown"
    };
    std::vector<CSSceneActor> scene_actors;

    // NMDL models embedded in the file itself. Self-contained cutscenes (e.g. e0005,
    // the opening) carry their whole scene here (etc999_v1 = the set, npcSLF/npcLRG =
    // their own actors) and use NO cfdata map. top_level marks models not nested
    // inside another NMDL's range.
    struct CSEmbeddedModel {
        std::string name;
        uint32_t    offset = 0;   // file offset of the NMDL chunk
        uint32_t    size = 0;
        bool        top_level = true;
    };
    std::vector<CSEmbeddedModel> embedded_models;

    // Initial actor transforms from the scene animations. Every scene NMTN
    // (e1180_010_plk_t, ...) carries a "move" track: the actor's root motion in the
    // LOCAL scene frame (pos XYZ + rot XYZ, Hermite keyframes). Sampling it at t=0
    // gives the actor's starting pose; the whole trajectory is there for later stages.
    // Validated live in RPCS3: at cutscene start the engine calls
    // TRANSFORM_ATOR(slot, params) with exactly these values.
    struct CSScenePlacement {
        std::string anim;        // e1180_010_plk_t
        std::string code;        // character code (plk)
        uint32_t    part = 0;    // scene part number (10 from "_010_")
        float       pos[3] = { 0, 0, 0 };
        float       rot[3] = { 0, 0, 0 };  // radians (X, Y=yaw, Z)
    };
    std::vector<CSScenePlacement> scene_placements;

    // Actors referenced (0x0cXX -> count)
    std::map<uint16_t, int>  actor_ids;
    // Data offsets referenced (0x07)
    std::vector<uint32_t>   data_offsets;
    // Embedded animations (NMTN names) -> reveal the scene's actors
    std::vector<std::string> embedded_anims;

    // Linear list of all scene commands (all nodes), offset order
    std::vector<CSCommand>  all_commands;

    // Initial scene camera: slot 0x0c03 IS the camera, set via master-library calls
    // LIB_0xF = SET_POSITION(z, y, x, slot), LIB_0x14 = SET_TARGET(z, y, x, slot)
    // (and LIB_0x10 = SET_ANGLES). Validated live: e1180's first camera at
    // (1.892, 2.379, -3.171) with FOV 35, near 0.1, far 4000 (debugger capture).
    bool  has_camera = false;
    float cam_eye[3] = { 0, 0, 0 };
    float cam_target[3] = { 0, 0, 0 };

    // Full camera score: every SET_POSITION starts a shot, the following SET_TARGET
    // completes it (e1180: 3 shots - the opening crane start high on the path, the
    // descent, and the low close-up of the dialogue).
    // A camera shot = one SET_POSITION/SET_ANGLES/SET_TARGET destination plus the
    // API_1025 params that configure HOW the camera reaches it. Mechanics (from the
    // eboot, fn 0x3871a8): API_1025(value, #idx, cam) sets one transition property:
    //   #1 float (approach speed?), #2 int (curve/mode; e1180 uses 63), #3 float
    //   (ease, gated by a flag), #4 float = DURATION in seconds. API_1023(1, .., cam)
    //   then COMMITS: the camera SLIDES from its current pose to this destination
    //   over #4 seconds. So a "shot" is a cut to a new destination that the camera
    //   eases into - not a keyframe linearly blended with the next shot.
    struct CSCameraShot {
        uint32_t offset = 0;        // bytecode offset of the SET_POSITION
        float eye[3] = { 0, 0, 0 };
        float target[3] = { 0, 0, 0 };
        float duration = 0.0f;     // param #4: seconds the camera takes to slide in
        int   curve = 0;        // param #2: movement curve/mode
        bool  committed = false;    // an API_1023(1,..) commit follows this shot
        // Paired slot 0x0c04 camera, confirmed in runtime (RPCS3) to run alongside
        // 0x0c03 on the black-screen opening, with real pitch. When present, this is
        // the second framing of the same beat. eye/target are scene-local like above.
        bool  has_pair = false;
        float pair_eye[3] = { 0, 0, 0 };
        float pair_target[3] = { 0, 0, 0 };
    };

    // Field-driven conversational camera cuts (from the MAP script, e.g. agn01),
    // one per dialogue line, in WORLD coordinates. Each is SET_POSITION (world eye)
    // + SET_ANGLES (yaw). Verified against live SET_TARGET dumps: the world targets
    // (-33.4, -45.7, -84.8, ...) match exactly. These are the cuts the player sees
    // during the conversation, distinct from the event's opening cameras.
    struct CSFieldShot {
        uint32_t offset = 0;
        float eye[3] = { 0, 0, 0 };  // world position
        float yaw = 0.0f;         // SET_ANGLES Y
        float pitch = 0.0f;
    };
    std::vector<CSFieldShot> field_shots;
    std::vector<CSCameraShot> camera_shots;

    // ---------------------------------------------------------------------- //
    // Narrative slideshow events (chapter-end type, e.g. e2010_010 Chopin).
    // These are NOT 3D scenes: they are a sequence of full-screen NTX3 images
    // (inside Mefc containers) shown one at a time, with a faded subtitle track,
    // paced by the fixed-timer slot 0x012C (value/300 = seconds), and synced to a
    // named music track via a cue table. Detected by content (Mefc + NTX3 + id 1028
    // image-switches + 0x012C timers + no 3D geometry/CSL). Additive: only filled
    // when the file is this type; 3D cutscenes leave these empty.
    // ---------------------------------------------------------------------- //
    enum class EventKind {
        Cutscene3D,    // e1180-class: map + actors + camera + CSL voice
        TitleCard,     // e1000-class: tiny bytecode, fixed timer, text sprites
        Narrative,     // e2010-class: image slideshow + faded subtitles + music
        Unknown
    };
    EventKind kind = EventKind::Unknown;

    // One full-screen (or title-strip) image carried in a Mefc container.
    struct CSSlideImage {
        uint32_t offset = 0;     // file offset of the NTX3 chunk
        uint16_t width = 0;
        uint16_t height = 0;
        uint8_t  format = 0;     // NTX3 format byte (DXT1/DXT5/raw)
        uint8_t  mips = 0;
        uint32_t data_size = 0;
        bool     is_title_strip = false;  // the small 256x64 localized title words
    };
    std::vector<CSSlideImage> slide_images;

    // One narrated caption with its timing.
    struct CSSubtitleLine {
        int         line_type = 42;       // <dNN>: 42 narrative, 48 title
        int         voice_index = 0;      // <vNN>: the id-219 byte argument
        std::string text;                 // the text in the selected language
        float       duration_s = 0.0f;    // <wNNNN> on-screen seconds (ms/1000)
        bool        fade = false;         // has <f1>/<f0> text fade
        float       start_s = -1.0f;      // master-clock time this line appears, taken
        // from the id-219 site in the bytecode (NOT by
        // summing <w> from t=0). -1 = unknown.
    };
    std::vector<CSSubtitleLine> subtitle_lines;   // lines in the SELECTED language

    // Subtitles are stored per language, each block preceded by a 3-letter marker
    // (JPN USA GRB FRA ITA DEU ESP ...) near the end of the file. We parse every
    // available language so the UI can switch between them. This applies to any event
    // type that carries a subtitle section (narrative, dialogue, title card).
    struct CSLanguageBlock {
        std::string code;                         // "USA", "FRA", ...
        uint32_t    offset = 0;                   // file offset of the marker
        std::vector<CSSubtitleLine> lines;        // parsed lines for this language
    };
    std::vector<CSLanguageBlock> languages;
    int selected_language = -1;   // index into languages; -1 = none/auto

    // Per-step master-clock timer values (slot 0x012C), in bytecode order. Each is
    // seconds (ticks/300). Drives the slideshow pacing; the 0.1+0.9 pairs are fade
    // transitions, the large ones are reading holds.
    std::vector<float> timer_steps_s;

    // Image-switch sites (id 1028): the bytecode offsets where the slideshow hands
    // off to the next image slot (0x0C05+). Used to align images to the timeline.
    std::vector<uint32_t> image_switch_offsets;
    // The image slot (0x0C05+) activated at each image-switch site, parallel to
    // image_switch_offsets. The hand-offs come in "activate next / deactivate prev"
    // pairs; the visible image is the highest slot activated so far.
    std::vector<uint16_t> image_switch_slots;

    // Explicit slot -> slide-image binding read straight from the bytecode: each
    // image is bound to a display slot by a "PUSH.B 2 ; PUSH.dw addr ; PUSH.W slot"
    // sequence, where addr points at the image's Mefc container. We resolve addr to a
    // slide_images index. This removes all guesswork: 0x0C04 = opening background,
    // 0x0C03 = title overlay, 0x0C05.. = the story photos.
    std::map<uint16_t, int> slot_to_image;   // slot -> index into slide_images

    // Music sync: a named track plus cue points (bytecode offsets where the music
    // re-syncs the script). From the "MPxxx.wav" + table at the end of the file.
    std::string           music_track;     // e.g. "MP181.wav"
    std::vector<uint32_t> music_cues;       // bytecode offsets

    // Total reconstructed runtime of the slideshow (sum of timer_steps_s).
    float narrative_runtime_s = 0.0f;

    // Seconds consumed by the opening setup waits (background fade-in) that do not
    // advance the visible story timeline. The timeline builder subtracts this so the
    // images and captions match the in-game clock.
    float opening_setup_s = 0.0f;

    // The image timeline: each segment is one slide image visible from start_s until
    // the next segment. Built by ScanNarrativeTimeline, which anchors image changes to
    // the subtitle clock (captions carry the true pacing), so each photo holds for the
    // right length. The viewer just looks up the playhead in this list.
    struct CSImageSegment {
        float start_s = 0.0f;
        int   image = -1;          // index into slide_images
        bool  title_overlay = false;  // the title strip composited over the opening
    };
    std::vector<CSImageSegment> image_segments;
};

class CutsceneScriptParser {
public:
    static bool           IsCutscene(const uint8_t* data, size_t size);
    static CutsceneScript Parse(const std::string& filepath);
    // Parse from an in-memory buffer (e.g. a map .e already loaded for its NMDL).
    static CutsceneScript ParseData(const std::vector<uint8_t>& data);

    // Resolve a command name by descriptor address (relocated memory dumps)
    static std::string    CommandName(uint32_t api_addr);
    // Resolve a command name by numeric API id (footer relocation)
    static std::string    CommandNameById(uint32_t api_id);

    // Event -> map index: scans a folder of cfdata map .e files and returns
    // {event_number -> map name (file stem)}, built from each map's hosted_events.
    static std::map<uint32_t, std::string> ScanCfdataFolder(const std::string& folder);
    // Texture bank of a map (eboot map table, 355 entries): agn01 -> "AGS" (AGS.p3tex).
    // Returns empty string if unknown.
    static std::string    MapTextureBank(const std::string& map_name);
    // Event number from a cutscene filename: "e1180.e" -> 1180, "e3090_010.e" -> 3090.
    // Returns 0 if no event number is found.
    static uint32_t       EventNumberFromFilename(const std::string& path);
    // English event title from the debug-room catalog (168 events extracted from
    // zzz01's menu data). Empty string if unknown (e.g. e90xx system events).
    static std::string    EventTitle(uint32_t event_number);
    // Approximate MAP-SPACE anchor of an event: parses the hosting map's script,
    // finds the event's setup block, and decodes the first plausible float triplet
    // from its data tables (agn01/e1180: the trigger point (-5.5, 11.6, -128.5)).
    // Returns false if nothing is found. Used to seed the scene anchor.
    static bool           FindEventAnchor(const std::string& map_path, uint32_t event,
        float out_pos[3]);
    // Disassemble a run of instructions starting at a bytecode offset (for probing
    // library function bodies in lib.e). Stops after max_instrs or at bytecode end.
    static std::vector<CSInstr> DisasmAt(const CutsceneScript& s, uint32_t offset,
        int max_instrs);

    // Select the subtitle language by index into s.languages, copying that block's
    // lines into s.subtitle_lines. Safe to call repeatedly (e.g. from a UI combo).
    // No-op if index is out of range. Returns true on success.
    static bool SelectLanguage(CutsceneScript& s, int language_index);

private:
    static uint32_t ReadU32BE(const uint8_t* d);
    static float    ReadF32BE(const uint8_t* d);

    static void ParseRelocations(CutsceneScript& s, const std::vector<uint8_t>& filedata);
    static int  DisasmOne(const CutsceneScript& s, uint32_t pos, CSInstr& out);
    static void DisasmNode(CutsceneScript& s, CSNode& node);
    static void BuildCommands(CutsceneScript& s, CSNode& node);
    static void BuildNodes(CutsceneScript& s);
    static void ScanActorsAndData(CutsceneScript& s);
    static void ScanEmbeddedAnims(CutsceneScript& s, const std::vector<uint8_t>& filedata);
    static void ScanEmbeddedModels(CutsceneScript& s, const std::vector<uint8_t>& filedata);
    static void ScanScenePlacements(CutsceneScript& s, const std::vector<uint8_t>& filedata);

    // Narrative slideshow (Chopin-type) detection and extraction. Additive: classifies
    // the event kind, and when Narrative, extracts the images, subtitle timeline,
    // timer steps, image-switch sites and music cue table. No effect on 3D cutscenes.
    static void ClassifyEventKind(CutsceneScript& s, const std::vector<uint8_t>& filedata);
    static void ScanSlideImages(CutsceneScript& s, const std::vector<uint8_t>& filedata);
    static void ScanSubtitleTimeline(CutsceneScript& s, const std::vector<uint8_t>& filedata);
    static void ScanTimerSteps(CutsceneScript& s, const std::vector<uint8_t>& filedata);
    static void ScanMusicCues(CutsceneScript& s, const std::vector<uint8_t>& filedata);
    // Stamp each subtitle line with its real appearance time from the id-219 sites.
    static void ScanSubtitleTiming(CutsceneScript& s, const std::vector<uint8_t>& filedata);

    // Find every CALLAPI (opcode 0x7d) site that resolves to a given API id, reading
    // the footer id/site table. Each 8-byte record pairs an id with a bytecode site,
    // but the pair orientation varies per file (the id is in the first word in some
    // files, the second in others). We test BOTH orientations of each record and
    // accept the side whose site lands inside the bytecode with a 0x7d opcode right
    // before it. Returns the sites as bytecode-relative offsets, sorted and unique.
    static std::vector<uint32_t> FindCallApiSites(const CutsceneScript& s,
        const std::vector<uint8_t>& filedata, uint32_t api_id);
};