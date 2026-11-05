#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/quaternion.hpp>
#include <map>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "EventScript.h"
#include "NSHPParser.h"
#include "NBN2Parser.h"
#include "NMTNParser.h"
#include "Camera.h"
#include "Shader.h"

// Static event player: rebuilds a cutscene from its files alone and plays it.
//
//   event .e   -> script timeline (EventScript): objects, transforms, motions, cameras,
//                 text, fades; plus every NMTN / bulk model the script names
//   lib.e      -> only used to check the exports the timeline relies on (VerifyLib)
//   actors     -> native 1141 ids: -10/-11/-12 = pcalg/pcplk/pcbet_v1.p3obj,
//                 -20..-28 = appkeep2.bmd entries 0..8 (CRS SRN CPN VOL SLS JRB FST MCH CLV)
//   host map   -> map .e + its .p3tex: the map script's branch for this event
//                 ((*state == EV), e.g. tnt03 for E0005) plus the NOBJ named like the map
//
// Coordinates are map world space (agn01 ground sits at y=0 under e1180's actors), so
// there is no scene anchor. Skinning is done on the CPU (a few 10k vertices per frame).
// Not reproduced: head look-at (1108 cmd 9), Mefc effects, per-actor lights, DOF,
// cloth (NDYN), the idle an actor holds before its first motion.
class EventPlayer {
public:
    struct Assets {
        std::string event_path;
        std::string lib_path;        // optional, for VerifyLib
        std::string appkeep2_path;   // appkeep2.bmd
        std::string party_dir;       // folder with pcalg/pcplk/pcbet_v1.p3obj
        std::string map_path;        // optional host map .e
        std::string p3tex_path;      // optional texture bank for the map
        std::string lang = "GBR";
    };

    EventPlayer();
    ~EventPlayer();

    bool Load(const Assets& a);      // needs a current GL context
    void Clear();
    bool Loaded() const { return m_Loaded; }

    double Duration() const { return m_TL.duration; }
    double Time() const { return m_T; }
    void   SetTime(double t);

    // Renders the frame at Time() into the player's FBO (w x h). free_cam uses m_FreeCam.
    void   Render(int w, int h, bool free_cam);
    GLuint ColorTexture() const { return m_FBOTex; }
    // Read back the last frame as RGBA8 (bottom-up rows)
    void   ReadPixels(std::vector<uint8_t>& out) const;

    // Overlay state at Time()
    std::string Subtitle() const;    // tags stripped, "" when none
    float       FadeAlpha() const;   // 0 = clear, 1 = black
    std::string CameraInfo() const;

    const es::EventTimeline& Timeline() const { return m_TL; }
    const std::string& Log() const { return m_Log; }
    const std::vector<std::string>& Report() const { return m_Report; }

    // ImGui panel (EventPlayerPanel.cpp). event_hint pre-fills the event path.
    void RenderPanel(const std::string& event_hint);
    // Called when an event file is opened in the Studio: fills the asset paths from the
    // event's folder, indexes the folder's map scripts (EventScript ScanMapEvents),
    // picks the map that plays this event and loads it. Returns false if not an event.
    bool OpenEvent(const std::string& path);

    struct MapChoice { std::string stem, path, how; };
    const std::vector<MapChoice>& MapChoices() const { return m_MapChoices; }

    Camera m_FreeCam;

private:
    struct GpuMesh {
        GLuint vao = 0, vbo = 0, ebo = 0;
        std::vector<FaceSection> sections;
        NSHPMesh cpu;                 // bind-pose vertices (skinned meshes)
        std::vector<Vertex> skinned;  // scratch
        bool skin = false;
        bool vertex_alpha = false;
    };
    struct Model {
        std::string name;
        std::vector<GpuMesh> meshes;
        std::unordered_map<uint16_t, GLuint> mat_to_tex, mat_to_alpha;
        std::unordered_set<uint16_t> mat_clamp, mat_blend;
        std::vector<GLuint> owned;
        std::vector<Bone> bones;              // bind (NBN2)
        std::vector<glm::vec3> pos_scale;     // accumulated parent scale per bone
        std::vector<glm::mat3> rot1;          // E(rot1) per bone
        std::vector<glm::mat4> inv_bind;
        std::unordered_map<std::string, std::vector<int>> by_name;
    };
    struct Anim {
        NMTNAnimation a;
        int mult = 5;                         // ticks per frame (5 when +0x29 bit 0)
        std::unordered_map<const Model*, std::vector<int>> track_to_bone;
    };
    // 2D screen layer from a Mefc (slideshow photo, background, localized title strip)
    struct Slide {
        uint16_t id = 0;
        std::vector<GLuint> tex;              // TEX0.. (title strips: one per PAL language)
        std::vector<glm::ivec2> size;
        float uv[4] = { 0, 0, 1, 1 };         // TRC0: u, v, width, height
        bool fullscreen = false;              // 1280-wide picture
        std::vector<glm::vec2> alpha;         // etbl (frame @60 fps, alpha 0..1)
    };
    struct Instance {
        uint16_t id = 0;
        Model* model = nullptr;
        bool is_static = false;               // map statics
        glm::mat4 static_xf{ 1.0f };
        std::vector<glm::mat4> pose;          // world matrix per bone (model space)
    };

    bool  LoadModel(const std::vector<uint8_t>& fd, size_t nobj_or_nmdl, bool use_bank, Model& out);
    Model* GetModel(const std::string& key, const std::vector<uint8_t>* fd, size_t off, bool use_bank);
    bool  LoadBank(const std::string& p3tex);
    Anim* GetAnim(uint32_t nmtn_off);
    void  ScanNLIT(const std::vector<uint8_t>& fd);

    void  PoseInstance(Instance& inst, double t);
    glm::mat4 ObjectMatrix(uint16_t id, double t, bool& alive) const;
    struct View { glm::vec3 eye{ 0 }, tgt{ 0, 0, -1 }; float fov_deg = 37.8f; int act = -1; float blend = -1; uint16_t cam = 0; };
    View  ViewAt(double t) const;
    View  ViewAtAct(int act, double t) const;
    View  CamStateView(uint16_t cam, double t) const;
    void  DrawModel(const Model& m, const glm::mat4& xf);
    bool  LoadSlide(uint16_t id, uint32_t mefc, Slide& out);
    void  DrawSlides();
    void  DrawQuad(float x0, float y0, float x1, float y1, const float* uv, GLuint tex, glm::vec4 color);
    void  EnsureGL(int w, int h);

    bool m_Loaded = false;
    double m_T = 0.0;
    Assets m_A;
    std::vector<uint8_t> m_EvData, m_MapData, m_AkData;
    es::Image m_Ev;
    es::EventTimeline m_TL;
    std::map<std::string, std::unique_ptr<Model>> m_Models;
    std::map<std::string, std::vector<uint8_t>> m_Files;
    std::map<uint32_t, std::unique_ptr<Anim>> m_Anims;
    std::vector<Instance> m_Inst;
    std::vector<Slide> m_Slides;
    Shader* m_Shader2D = nullptr;
    GLuint m_QuadVAO = 0, m_QuadVBO = 0;
    std::vector<GLuint> m_Bank;
    std::vector<bool> m_BankBlend;
    std::vector<size_t> m_CamActs;            // indices into m_TL.records
    mutable std::map<size_t, View> m_BlendFrom;
    std::string m_Log;
    std::vector<std::string> m_Report;

    glm::vec3 m_Ambient{ 0.62f }, m_DirColor{ 0.75f };
    glm::vec3 m_DirLight{ 0.35f, 0.85f, 0.4f };

    Shader* m_Shader = nullptr;
    GLuint m_FBO = 0, m_FBOTex = 0, m_FBODepth = 0;
    int m_W = 0, m_H = 0;

    // event -> map index of one folder
    std::string m_IndexDir;
    std::vector<es::EventRef> m_Index;
    std::map<std::string, std::string> m_MapFiles;   // map stem -> path
    std::vector<MapChoice> m_MapChoices;
    int m_MapPick = -1;
    void IndexFolder(const std::string& dir);
    void ApplyMapPick(int i);
    // Paths the user set once (maps folder, lib.e, appkeep2, party models, language),
    // kept in event_player.ini in the working directory.
    void LoadSettings();
    void SaveSettings() const;
    bool m_SettingsLoaded = false;

    // panel state (EventPlayerPanel.cpp)
    char m_PathMaps[512] = {};   // folder with the map scripts (cfdata_extracted\\cfdata)
    char m_PathEvent[512] = {}, m_PathLib[512] = {}, m_PathAk2[512] = {}, m_PathParty[512] = {},
        m_PathMap[512] = {}, m_PathTex[512] = {};
    bool m_Playing = false, m_UseFree = false;
    float m_Speed = 1.0f;
    int m_LangIdx = 1;
    std::string m_LastHint;
};
