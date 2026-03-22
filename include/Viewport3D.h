#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include "NSHPParser.h"
#include "Camera.h"
#include "Shader.h"

class Viewport3D {
public:
    Viewport3D();
    ~Viewport3D();

    void Render();
    void LoadMesh(const NSHPMesh& mesh);
    void ClearMesh();

    bool HasMesh() const { return m_HasMesh; }

private:
    void CleanupGL();
    void HandleInput();

    Camera m_Camera;
    Shader* m_Shader;

    GLuint m_VAO;
    GLuint m_VBO;
    GLuint m_EBO;

    size_t m_VertexCount;
    size_t m_IndexCount;

    bool m_HasMesh;

    glm::vec2 m_LastMousePos;
    bool m_MouseDown;

    int m_ViewportWidth;
    int m_ViewportHeight;
};