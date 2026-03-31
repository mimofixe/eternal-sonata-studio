#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <unordered_map>
#include <cfloat>
#include "NSHPParser.h"
#include "NMDLLoader.h"
#include "NBN2Parser.h"
#include "Camera.h"
#include "Shader.h"

// GPU resources for one mesh within an NMDL model.
struct MeshGPUData {
    GLuint VAO = 0, VBO = 0, EBO = 0;
    std::vector<FaceSection> sections;   // per-section draw calls
    size_t vertex_count = 0;
    bool   draw_sequential = false;
};

class Viewport3D {
public:
    Viewport3D();
    ~Viewport3D();

    // Single mesh (existing API, no textures)
    void LoadMesh(const NSHPMesh& mesh);
    void ClearMesh();
    bool HasMesh() const { return m_HasMesh; }

    // Full NMDL model with textures.
    // Takes ownership of NMDLModel textures. Clears any loaded single mesh.
    void LoadModel(NMDLModel& model);
    void ClearModel();
    bool HasModel() const { return m_HasModel; }

    // Skeleton
    void LoadSkeleton(const std::vector<Bone>& bones);
    void ClearSkeleton();
    bool HasSkeleton() const { return m_HasSkeleton; }

    void Render();

private:
    void CleanupMeshGL();
    void CleanupModelGL();
    void CleanupSkeletonGL();
    void FitCamera(glm::vec3 center, float extent);
    void UploadMeshToGPU(const NSHPMesh& mesh, MeshGPUData& out);

    Camera  m_Camera;
    Shader* m_MeshShader = nullptr;
    Shader* m_BoneShader = nullptr;

    // Single mesh
    GLuint m_VAO = 0, m_VBO = 0, m_EBO = 0;
    size_t m_VertexCount = 0;
    size_t m_IndexCount = 0;
    bool   m_HasMesh = false;
    bool   m_DrawSequential = false;

    // NMDL model
    std::vector<MeshGPUData>             m_ModelMeshes;
    std::unordered_map<uint16_t, GLuint> m_MatToTex;
    std::vector<GLuint>                  m_ModelTextures;
    bool m_HasModel = false;

    // Skeleton
    GLuint m_LineVAO = 0, m_LineVBO = 0;
    GLuint m_DotVAO = 0, m_DotVBO = 0;
    int    m_LineVertexCount = 0;
    int    m_DotCount = 0;
    bool   m_HasSkeleton = false;

    // UI toggles
    bool m_ShowMesh = true;
    bool m_ShowSkeleton = true;
    bool m_FlipY = false;

    // Offscreen framebuffer
    GLuint m_FBO = 0, m_FBOTex = 0, m_FBORbo = 0;
    int    m_FBOw = 0, m_FBOh = 0;
    int    m_ViewW = 800, m_ViewH = 600;

    glm::vec2 m_LastMouse{ 0.f };
    bool      m_MouseDown = false;
};