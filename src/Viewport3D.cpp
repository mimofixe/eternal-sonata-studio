#include "Viewport3D.h"
#include <imgui.h>
#include <iostream>

const char* VERTEX_SHADER = R"(
#version 450 core
layout (location = 0) in vec3 aPos;
layout (location = 1) in vec3 aNormal;
layout (location = 2) in vec2 aTexCoord;

out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoord;

uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;

void main() {
    FragPos = vec3(model * vec4(aPos, 1.0));
    Normal = mat3(transpose(inverse(model))) * aNormal;
    TexCoord = aTexCoord;
    gl_Position = projection * view * vec4(FragPos, 1.0);
}
)";

const char* FRAGMENT_SHADER = R"(
#version 450 core
out vec4 FragColor;

in vec3 FragPos;
in vec3 Normal;
in vec2 TexCoord;

uniform vec3 lightPos;
uniform vec3 viewPos;
uniform vec3 objectColor;

void main() {
    float ambientStrength = 0.3;
    vec3 ambient = ambientStrength * vec3(1.0);
    
    vec3 norm = normalize(Normal);
    vec3 lightDir = normalize(lightPos - FragPos);
    float diff = max(dot(norm, lightDir), 0.0);
    vec3 diffuse = diff * vec3(1.0);
    
    float specularStrength = 0.5;
    vec3 viewDir = normalize(viewPos - FragPos);
    vec3 reflectDir = reflect(-lightDir, norm);
    float spec = pow(max(dot(viewDir, reflectDir), 0.0), 32);
    vec3 specular = specularStrength * spec * vec3(1.0);
    
    vec3 result = (ambient + diffuse + specular) * objectColor;
    FragColor = vec4(result, 1.0);
}
)";

Viewport3D::Viewport3D()
    : m_VAO(0), m_VBO(0), m_EBO(0),
    m_VertexCount(0), m_IndexCount(0),
    m_HasMesh(false), m_MouseDown(false),
    m_LastMousePos(0.0f, 0.0f),
    m_ViewportWidth(800), m_ViewportHeight(600) {

    m_Camera.SetDistance(100.0f);
    m_Camera.SetFOV(50.0f);

    m_Shader = new Shader();
    if (!m_Shader->CompileFromSource(VERTEX_SHADER, FRAGMENT_SHADER)) {
        std::cerr << "Failed to compile shaders" << std::endl;
    }
}

Viewport3D::~Viewport3D() {
    CleanupGL();
    delete m_Shader;
}

void Viewport3D::LoadMesh(const NSHPMesh& mesh) {
    CleanupGL();

    if (mesh.vertices.empty()) {
        std::cout << "No vertices to load" << std::endl;
        return;
    }

    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);

    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);

    glBufferData(GL_ARRAY_BUFFER, mesh.vertices.size() * sizeof(Vertex),
        mesh.vertices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(0);

    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, normal));
    glEnableVertexAttribArray(1);

    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));
    glEnableVertexAttribArray(2);

    if (!mesh.indices.empty()) {
        glGenBuffers(1, &m_EBO);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh.indices.size() * sizeof(uint16_t),
            mesh.indices.data(), GL_STATIC_DRAW);
        m_IndexCount = mesh.indices.size();
    }
    else {
        m_IndexCount = 0;
    }

    glBindVertexArray(0);

    m_VertexCount = mesh.vertices.size();
    m_HasMesh = true;

    std::cout << "Mesh loaded: " << mesh.name
        << " (" << m_VertexCount << " vertices, "
        << m_IndexCount << " indices)" << std::endl;
}

void Viewport3D::ClearMesh() {
    CleanupGL();
    m_HasMesh = false;
}

void Viewport3D::Render() {
    ImGui::Begin("3D Viewport", nullptr, ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    if (!m_HasMesh) {
        ImGui::TextDisabled("No mesh loaded");
        ImGui::Text("Select a NSHP chunk and click 'Load in Viewport'");
        ImGui::End();
        return;
    }

    ImGui::Text("Mesh loaded! Vertices: %zu", m_VertexCount);
    if (m_IndexCount > 0) {
        ImGui::SameLine();
        ImGui::Text("Indices: %zu", m_IndexCount);
    }

    ImVec2 viewport_pos = ImGui::GetCursorScreenPos();
    ImVec2 viewport_size = ImGui::GetContentRegionAvail();
    m_ViewportWidth = (int)viewport_size.x;
    m_ViewportHeight = (int)viewport_size.y;

    if (m_ViewportWidth <= 0 || m_ViewportHeight <= 0) {
        ImGui::End();
        return;
    }

    static GLuint fbo = 0;
    static GLuint texture = 0;
    static GLuint rbo = 0;
    static int last_w = 0, last_h = 0;

    if (fbo == 0 || last_w != m_ViewportWidth || last_h != m_ViewportHeight) {
        if (fbo) {
            glDeleteFramebuffers(1, &fbo);
            glDeleteTextures(1, &texture);
            glDeleteRenderbuffers(1, &rbo);
        }

        glGenFramebuffers(1, &fbo);
        glBindFramebuffer(GL_FRAMEBUFFER, fbo);

        glGenTextures(1, &texture);
        glBindTexture(GL_TEXTURE_2D, texture);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, m_ViewportWidth, m_ViewportHeight, 0, GL_RGB, GL_UNSIGNED_BYTE, NULL);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, texture, 0);

        glGenRenderbuffers(1, &rbo);
        glBindRenderbuffer(GL_RENDERBUFFER, rbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, m_ViewportWidth, m_ViewportHeight);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, rbo);

        if (glCheckFramebufferStatus(GL_FRAMEBUFFER) != GL_FRAMEBUFFER_COMPLETE) {
            std::cerr << "Framebuffer not complete!" << std::endl;
        }

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        last_w = m_ViewportWidth;
        last_h = m_ViewportHeight;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, fbo);
    glViewport(0, 0, m_ViewportWidth, m_ViewportHeight);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.2f, 0.2f, 0.25f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_Shader->Use();

    glm::mat4 model = glm::mat4(1.0f);
    glm::mat4 view = m_Camera.GetViewMatrix();
    glm::mat4 projection = m_Camera.GetProjectionMatrix((float)m_ViewportWidth / m_ViewportHeight);

    m_Shader->SetMat4("model", model);
    m_Shader->SetMat4("view", view);
    m_Shader->SetMat4("projection", projection);

    m_Shader->SetVec3("lightPos", glm::vec3(10.0f, 10.0f, 10.0f));
    m_Shader->SetVec3("viewPos", m_Camera.GetPosition());
    m_Shader->SetVec3("objectColor", glm::vec3(0.7f, 0.7f, 0.9f));

    glBindVertexArray(m_VAO);

    if (m_IndexCount > 0) {
        glDrawElements(GL_TRIANGLES, m_IndexCount, GL_UNSIGNED_SHORT, 0);
    }
    else {
        glDrawArrays(GL_TRIANGLES, 0, m_VertexCount);
    }

    glBindVertexArray(0);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    ImGui::SetCursorScreenPos(viewport_pos);
    ImGui::InvisibleButton("viewport_canvas", viewport_size,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight);

    bool is_hovered = ImGui::IsItemHovered();
    bool is_active = ImGui::IsItemActive();

    ImDrawList* draw_list = ImGui::GetWindowDrawList();
    draw_list->AddImage(
        (void*)(intptr_t)texture,
        viewport_pos,
        ImVec2(viewport_pos.x + viewport_size.x, viewport_pos.y + viewport_size.y),
        ImVec2(0, 1),
        ImVec2(1, 0)
    );

    if (is_hovered || is_active) {
        ImGuiIO& io = ImGui::GetIO();

        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            ImVec2 delta = io.MouseDelta;
            m_Camera.Rotate(delta.x * 0.005f, delta.y * 0.005f);
        }

        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)) {
            ImVec2 delta = io.MouseDelta;
            m_Camera.Pan(-delta.x, delta.y);
        }

        if (io.MouseWheel != 0.0f) {
            m_Camera.Zoom(-io.MouseWheel * 2.0f);
        }
    }

    ImGui::End();
}

void Viewport3D::CleanupGL() {
    if (m_VAO) {
        glDeleteVertexArrays(1, &m_VAO);
        m_VAO = 0;
    }
    if (m_VBO) {
        glDeleteBuffers(1, &m_VBO);
        m_VBO = 0;
    }
    if (m_EBO) {
        glDeleteBuffers(1, &m_EBO);
        m_EBO = 0;
    }
}