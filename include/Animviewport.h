#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

#include "NBN2Parser.h"
#include "NSHPParser.h"
#include "NMTNParser.h"
#include "Camera.h"
#include "Shader.h"
#include "EFileParser.h"

// GPU data for one skinned mesh. Now carries the face sections and per-vertex
// UVs needed to draw the mesh textured (one draw call per material section),
// in addition to the skinning attributes.
struct SkinMeshGPU {
    GLuint VAO = 0, VBO = 0, EBO = 0;
    size_t vertex_count = 0;
    size_t index_count = 0;
    bool   draw_sequential = false;
    std::vector<uint16_t>    bone_id_list;   // local bone index -> global NBN2 index
    std::vector<FaceSection> sections;       // material id + index range per submesh
};

// Per-bone cloth dynamics parameters, parsed from the NDYN chunks. Dynamic bones
// (the "JD*"/"eff*" chains: skirt, ribbons, sleeves, hair) are not posed by the
// animation; the engine simulates them at runtime. We reproduce that here.
struct ClothBone {
    bool      isDyn = false;     // participates in the simulation
    bool      isRoot = false;    // first dynamic bone of a chain -> pinned to body
    glm::vec3 localOff{ 0.f };   // bind offset from parent, in parent bind space
    glm::mat3 bindLocalRot{ 1.f };
    float     restLen = 0.f;     // bone length (distance to parent)
    float     stiffness = 0.15f; // NDYN B: return strength toward the parent-frame rest dir
    float     damping = 0.06f;   // NDYN C: velocity damping (shoes high, hair low)
    float     mass = 1.f;        // NDYN A: gravity weight
};

// A body collision SPHERE: a bone plus a bone-local offset (NCLS i16 offset /64)
// and a radius (NCLS f32 @ +0x2c). This matches the engine: the Dynabone collides
// the cloth particles against sphere colliders (the CEllipseCylinder variant is a
// no-op stub in the demo; CSphere is what actually runs). World center is recomputed
// from the rigid pose each step.
struct ClothSphere {
    int       bone = -1;
    glm::vec3 loff{ 0.f };
    float     r = 0.1f;
};

// One self-contained animatable model: skeleton + skinned meshes + animations,
// plus the textures decoded from the model's own NMDL so the meshes can be drawn
// textured instead of flat-shaded.
struct SkeletalModel {
    std::string name;

    std::vector<Bone>      bind;
    std::vector<Bone>      anim;
    std::vector<glm::mat4> invBind;
    std::unordered_map<std::string, int> boneIndex;

    std::vector<SkinMeshGPU> meshes;
    std::vector<NMTNAnimation> anims;

    // material -> GL texture, decoded from the NMDL's internal NTX3 chunks.
    std::unordered_map<uint16_t, GLuint> matToTex;
    std::unordered_map<uint16_t, GLuint> matToAlpha;
    std::unordered_set<uint16_t>         matBlend;   // DXT5: full alpha -> GL_BLEND
    std::unordered_set<uint16_t>         matClamp;   // GL_CLAMP_TO_EDGE
    std::vector<GLuint>                  ownedTextures;

    int   animIdx = 0;
    float frame = 0.f;
    bool  playing = false;

    GLuint lineVAO = 0, lineVBO = 0;
    int    lineVerts = 0;

    // cloth dynamics
    std::vector<ClothBone>    cloth;       // per bone (size == bind.size())
    std::vector<ClothSphere>  colliders;   // body collision spheres (from NCLS)
    std::vector<glm::vec3>    pCur, pPrev;  // Verlet particle state (per bone)
    bool  hasCloth = false;
    bool  clothInit = false;

    glm::vec3 fitCenter{ 0.f };
    float     fitDist = 3.f;
};

class AnimViewport {
public:
    AnimViewport();
    ~AnimViewport();

    void LoadFromChunks(const std::vector<uint8_t>& file_data,
        const std::vector<Chunk>& chunks);

    void Tick(float dt);
    void Render();

    bool HasContent() const { return m_HasContent; }
    void Clear();
    int  ModelCount() const { return (int)m_Models.size(); }

private:
    std::vector<SkeletalModel> m_Models;
    int  m_ModelIdx = 0;

    SkeletalModel* Active();
    const SkeletalModel* Active() const;

    void BuildGroups(const std::vector<uint8_t>& file_data,
        const std::vector<Chunk>& chunks);
    void SelectModel(int idx);

    Camera  m_Camera;
    Shader* m_SkinShader = nullptr;   // skinning + texture
    Shader* m_BoneShader = nullptr;

    GLuint m_FBO = 0, m_FBOTex = 0, m_FBORbo = 0;
    int    m_FBOw = 0, m_FBOh = 0;
    int    m_ViewW = 800, m_ViewH = 600;

    float m_PlaySpeed = 30.f;
    bool  m_ShowSkel = false;
    bool  m_Textured = true;          // draw with NMDL textures vs flat shading
    bool  m_Lighting = false;         // off = flat texture like the static Viewport3D
    bool  m_Cloth = true;             // run NDYN/NCLS cloth dynamics
    float m_Gravity = 9.80665f;       // proven from the Dynabone binary (real g)
    bool  m_HasContent = false;
    float m_ClothAccum = 0.f;         // fixed-timestep accumulator (engine runs 1/60)

    void BuildInvBind(SkeletalModel& m);
    void ApplyPose(SkeletalModel& m, float frame);
    void RebuildSkeletonLines(SkeletalModel& m);
    void UploadSkinMesh(const NSHPMesh& mesh, SkinMeshGPU& out);
    void CleanupModel(SkeletalModel& m);
    glm::mat4 WorldMat(const Bone& b) const;

    // cloth dynamics
    void ParseDynamics(SkeletalModel& m, const std::vector<uint8_t>& fd,
        size_t region_begin, size_t region_end);
    void StepDynamics(SkeletalModel& m, float dt);   // one temporal Verlet step
    void ApplyClothPose(SkeletalModel& m);           // write cloth state to m.anim (no advance)
    void SettleDynamics(SkeletalModel& m, int steps); // settle for static preview
    void ResetDynamics(SkeletalModel& m);

    static void SetMat4Array(GLuint prog, const char* name,
        const glm::mat4* mats, int count);
};