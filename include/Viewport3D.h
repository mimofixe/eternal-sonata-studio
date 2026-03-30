#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <cfloat>
#include "NSHPParser.h"
#include "NBN2Parser.h"
#include "Camera.h"
#include "Shader.h"

class Viewport3D {
public:
    Viewport3D();
    ~Viewport3D();

    // Mesh
    void LoadMesh(const NSHPMesh& mesh);
    void ClearMesh();
    bool HasMesh() const { return m_HasMesh; }

    //Skeleton
    // Uploads bone world positions as GL_LINES (parent→child) and GL_POINTS.
    // Adjusts the camera to frame the skeleton automatically.
    void LoadSkeleton(const std::vector<Bone>& bones);
    void ClearSkeleton();
    bool HasSkeleton() const { return m_HasSkeleton; }

    //Main render call (call every frame inside an ImGui window)
    void Render();

private:
    void CleanupMeshGL();
    void CleanupSkeletonGL();
    void FitCamera(glm::vec3 center, float extent);

    //Mesh GL resource
    Camera  m_Camera;
    Shader* m_MeshShader = nullptr;
    GLuint  m_VAO = 0, m_VBO = 0, m_EBO = 0;
    size_t  m_VertexCount = 0;
    size_t  m_IndexCount = 0;
    bool    m_HasMesh = false;

    //Skeleton GL resources
    Shader* m_BoneShader = nullptr;
    GLuint  m_LineVAO = 0, m_LineVBO = 0;  // GL_LINES  parent→child
    GLuint  m_DotVAO = 0, m_DotVBO = 0;  // GL_POINTS at each joint
    int     m_LineVertexCount = 0;
    int     m_DotCount = 0;
    bool    m_HasSkeleton = false;

    //UI toggles
    bool m_ShowMesh = true;
    bool m_ShowSkeleton = true;
    bool m_FlipY = false;   // negate Y for PS3 → GL-Y-up conversion

    // framebuffer
    GLuint m_FBO = 0;
    GLuint m_FBOTex = 0;
    GLuint m_FBORbo = 0;
    int    m_FBOw = 0, m_FBOh = 0;
    int    m_ViewW = 800, m_ViewH = 600;

    glm::vec2 m_LastMouse{ 0.f };
    bool      m_MouseDown = false;
};