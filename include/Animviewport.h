#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <unordered_map>
#include <cstdint>

#include "NBN2Parser.h"
#include "NSHPParser.h"
#include "NMTNParser.h"
#include "Camera.h"
#include "Shader.h"
#include "EFileParser.h"

// GPU data for one skinned mesh.
struct SkinMeshGPU {
    GLuint VAO = 0, VBO = 0, EBO = 0;
    size_t vertex_count = 0;
    size_t index_count = 0;
    bool   draw_sequential = false;
    // local bone index → global NBN2 bone index
    std::vector<uint16_t> bone_id_list;
};

class AnimViewport {
public:
    AnimViewport();
    ~AnimViewport();

    // Load bones (NBN2), skinned meshes (NSHP inside NMDL), and
    // animations (NMTN) from the already-scanned chunk list + file buffer.
    void LoadFromChunks(const std::vector<uint8_t>& file_data,
        const std::vector<Chunk>& chunks);

    // Advance animation time; call once per frame before Render().
    void Tick(float dt);

    // Render the full viewport window including ImGui controls.
    // Must be called between ImGui::Begin("...") / ImGui::End() pairs
    // handled internally.
    void Render();

    bool HasContent() const { return m_HasContent; }
    void Clear();

private:
    // skeleton 
    std::vector<Bone>     m_BindBones;   // T-pose from NBN2
    std::vector<Bone>     m_AnimBones;   // current pose (modified each tick)
    std::vector<glm::mat4> m_InvBind;   // inverse world matrices of bind pose
    std::unordered_map<std::string, int> m_BoneIndexMap;

    // skinned meshes
    std::vector<SkinMeshGPU> m_Meshes;

    // animations
    std::vector<NMTNAnimation> m_Anims;
    int   m_AnimIdx = 0;
    float m_Frame = 0.f;
    bool  m_Playing = false;
    float m_PlaySpeed = 30.f;   // frames per second
    bool  m_ShowSkel = true;

    // skeleton lines (for overlay)
    GLuint m_LineVAO = 0, m_LineVBO = 0;
    int    m_LineVerts = 0;

    // rendering
    Camera  m_Camera;
    Shader* m_SkinShader = nullptr;
    Shader* m_BoneShader = nullptr;

    GLuint m_FBO = 0, m_FBOTex = 0, m_FBORbo = 0;
    int    m_FBOw = 0, m_FBOh = 0;
    int    m_ViewW = 800, m_ViewH = 600;
    glm::vec2 m_LastMouse{ 0.f };
    bool      m_MouseDown = false;

    bool m_HasContent = false;

    // helpers
    void BuildInvBind();
    void ApplyPose(float frame);
    void RebuildSkeletonLines();
    void UploadSkinMesh(const NSHPMesh& mesh, SkinMeshGPU& out);
    void CleanupMeshes();
    void CleanupSkeleton();
    glm::mat4 WorldMat(const Bone& b) const;

    static void SetMat4Array(GLuint prog, const char* name,
        const glm::mat4* mats, int count);
};