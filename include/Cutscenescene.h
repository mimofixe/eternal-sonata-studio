#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <string>
#include <vector>
#include <unordered_map>
#include <unordered_set>
#include "NSHPParser.h"
#include "Nmdlloader.h"
#include "Camera.h"
#include "Shader.h"

// Stage 1 of the "maestro": an ISOLATED multi-model scene renderer for cutscene
// recreation. Does not touch Viewport3D or any existing viewer. Mirrors the proven
// GL patterns of Viewport3D (mesh upload, per-section material draw, FBO + ImGui
// image, orbit camera) but holds N model instances with world transforms:
//   - the MAP, loaded with EXTERNAL textures from its .p3tex bank (the map's main
//     NMDL has no internal NTX3; e.g. agn01 + AGS.p3tex per the eboot map table);
//   - the ACTORS (characters), loaded with INTERNAL textures from .p3obj /
//     appkeep2.bmd, in bind pose.
// NLIT scene lighting is read from the map file.

// GPU data of one mesh (mirror of Viewport3D's MeshGPUData, kept local so this
// module stays fully decoupled from Viewport3D).
struct CSMeshGPU {
    GLuint VAO = 0, VBO = 0, EBO = 0;
    std::vector<FaceSection> sections;
    size_t vertex_count = 0;
    bool   draw_sequential = false;
    bool   needs_depth_offset = false;
    bool   has_vertex_alpha = false;
};

// One model placed in the scene (map or actor)
struct CSSceneInstance {
    std::string                          name;
    std::vector<CSMeshGPU>               meshes;
    std::unordered_map<uint16_t, GLuint> mat_to_tex;
    std::unordered_map<uint16_t, GLuint> mat_to_alpha;
    std::unordered_set<uint16_t>         mat_clamp_ids;
    std::unordered_set<uint16_t>         mat_blend_ids;
    std::vector<GLuint>                  owned_textures; // internal NTX3 (actors)
    glm::vec3                            position{ 0.0f };
    float                                yaw_deg = 0.0f;
    float                                scale = 1.0f;
    bool                                 visible = true;
    bool                                 is_map = false;
    glm::vec3                            bbox_min{ 0.0f }, bbox_max{ 0.0f };
};

class CutsceneScene {
public:
    CutsceneScene();
    ~CutsceneScene();

    // Map with external texture bank (.p3tex). Loads the FIRST NMDL of the map file
    // (the main map model) and reads NLIT lighting.
    bool LoadMap(const std::string& map_path, const std::string& p3tex_path);

    // Actor with internal textures (.p3obj or .bmd). name_filter selects the NMDL by
    // case-insensitive substring (e.g. "cpn" inside appkeep2.bmd); empty = first NMDL.
    bool AddActor(const std::string& path, const std::string& name_filter);

    void Clear();
    bool HasScene() const { return !m_Instances.empty(); }
    bool HasMap() const { for (auto& i : m_Instances) if (i.is_map) return true; return false; }

    // Pose the most recently added actor (scene-local frame, from the "move" track t=0)
    void SetLastInstancePose(const glm::vec3& pos, float yaw_deg);
    void SetStatus(const std::string& msg);
    void SetAnchor(const glm::vec3& pos, float yaw_deg) { m_AnchorPos = pos; m_AnchorYaw = yaw_deg; }
    // Director's camera from the script (slot 0x0c03): eye + target in the scene-local
    // frame; FOV 35, near 0.1, far 4000 (engine values, captured live).
    void SetSceneCamera(const glm::vec3& eye, const glm::vec3& target) {
        m_SceneEye = eye; m_SceneTarget = target; m_HasSceneCam = true; m_UseSceneCam = true;
    }
    // Director's camera path: shots in script order with per-move durations.
    struct CSCamKey {
        glm::vec3 eye, target;
        float dur = 0.0f;        // movement duration (API_1025 #4); 0 = cut
        bool  move_to = false;   // this key is the END of a timed move from the
        // previous key (the 0x0c04 pair), so interpolate
        // toward it over dur seconds instead of cutting
    };
    void SetCameraPath(const std::vector<CSCamKey>& path) { m_CamPath = path; }

    // Full ImGui panel: load controls, instance list, 3D view.
    // map_hint: resolved map name for this cutscene ("agn01"), shown as guidance.
    void RenderPanel(const std::string& map_hint, const std::string& texbank_hint);

private:
    bool LoadModelInstance(const std::vector<uint8_t>& fd, size_t nmdl_off,
        bool is_map, CSSceneInstance& out);
    void UploadMesh(const NSHPMesh& mesh, CSMeshGPU& out);
    void DrawInstance(const CSSceneInstance& inst, const glm::mat4& model);
    void DestroyInstance(CSSceneInstance& inst);
    void ScanNLIT(const std::vector<uint8_t>& fd);
    void EnsureShader();
    void EnsureFBO(int w, int h);
    void FitCamera();

    static bool   ReadFileBytes(const std::string& path, std::vector<uint8_t>& out);
    static size_t FindNMDL(const std::vector<uint8_t>& fd, const std::string& name_filter);

    std::vector<CSSceneInstance> m_Instances;

    // Shared texture bank from the map's .p3tex (scene-owned)
    std::vector<GLuint> m_BankTextures;
    std::vector<bool>   m_BankDXT5;

    Camera  m_Camera;
    Shader* m_Shader = nullptr;

    GLuint m_FBO = 0, m_FBOTex = 0, m_FBORbo = 0;
    int    m_FBOw = 0, m_FBOh = 0;

    // NLIT scene lighting
    bool      m_HasSceneLighting = false;
    bool      m_EnableLighting = true;
    glm::vec3 m_AmbientColor{ 0.7f };
    glm::vec3 m_DirLightColor{ 0.86f };
    float     m_DirLightPitch = 60.0f;
    float     m_DirLightYaw = 45.0f;
    bool      m_FlipY = false;

    // Scene anchor: places the cutscene's LOCAL frame in the map. Applied to all
    // non-map instances. Debug-room launches anchor near the map origin; story mode
    // anchors at the field's trigger zone. Freely adjustable.
    glm::vec3 m_AnchorPos{ 0.0f };
    float     m_AnchorYaw = 0.0f;

    // Scene camera (script slot 0x0c03)
    bool      m_HasSceneCam = false;
    bool      m_UseSceneCam = false;
    glm::vec3 m_SceneEye{ 0.0f }, m_SceneTarget{ 0.0f };
    std::vector<CSCamKey> m_CamPath;
    bool      m_CamPlaying = false;
    size_t    m_CamSeg = 0;       // current segment (moving toward shot m_CamSeg+1)
    float     m_CamT = 0.0f;      // seconds into the segment

    char m_Status[256] = {};
    char m_ActorFilter[32] = {};
};