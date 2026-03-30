#include "Viewport3D.h"
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <algorithm>
#include <cfloat>

//Shaders

// Mesh shader — Phong, uses model/view/projection matrices.
static const char* MESH_VERT = R"(
#version 450 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
out vec3 FragPos;
out vec3 Normal;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
void main(){
    FragPos   = vec3(model * vec4(aPos, 1.0));
    Normal    = mat3(transpose(inverse(model))) * aNormal;
    gl_Position = projection * view * vec4(FragPos, 1.0);
})";

static const char* MESH_FRAG = R"(
#version 450 core
in  vec3 FragPos;
in  vec3 Normal;
out vec4 FragColor;
uniform vec3 lightPos;
uniform vec3 viewPos;
uniform vec3 objectColor;
void main(){
    vec3 ambient  = 0.3 * vec3(1.0);
    vec3 norm     = length(Normal) > 0.01 ? normalize(Normal) : vec3(0,1,0);
    vec3 lightDir = normalize(lightPos - FragPos);
    vec3 diffuse  = max(dot(norm, lightDir), 0.0) * vec3(1.0);
    vec3 viewDir  = normalize(viewPos - FragPos);
    vec3 half_v   = normalize(lightDir + viewDir);
    float spec    = pow(max(dot(norm, half_v), 0.0), 32.0) * 0.5;
    FragColor = vec4((ambient + diffuse + spec) * objectColor, 1.0);
})";

// Bone shader — unlit flat colour; no model matrix (bones already in world space).
// gl_PointSize controls joint dot size.
static const char* BONE_VERT = R"(
#version 450 core
layout(location=0) in vec3 aPos;
uniform mat4 view;
uniform mat4 projection;
void main(){
    gl_Position  = projection * view * vec4(aPos, 1.0);
    gl_PointSize = 8.0;
})";

static const char* BONE_FRAG = R"(
#version 450 core
out vec4 FragColor;
uniform vec4 color;
void main(){ FragColor = color; })";

//Constructor / Destructor

Viewport3D::Viewport3D() {
    m_Camera.SetDistance(5.0f);
    m_Camera.SetFOV(50.0f);

    m_MeshShader = new Shader();
    if (!m_MeshShader->CompileFromSource(MESH_VERT, MESH_FRAG))
        std::cerr << "[Viewport3D] Mesh shader compile failed\n";

    m_BoneShader = new Shader();
    if (!m_BoneShader->CompileFromSource(BONE_VERT, BONE_FRAG))
        std::cerr << "[Viewport3D] Bone shader compile failed\n";
}

Viewport3D::~Viewport3D() {
    CleanupMeshGL();
    CleanupSkeletonGL();
    if (m_FBO)    glDeleteFramebuffers(1, &m_FBO);
    if (m_FBOTex) glDeleteTextures(1, &m_FBOTex);
    if (m_FBORbo) glDeleteRenderbuffers(1, &m_FBORbo);
    delete m_MeshShader;
    delete m_BoneShader;
}

//Mesh loading

void Viewport3D::LoadMesh(const NSHPMesh& mesh) {
    CleanupMeshGL();
    if (mesh.vertices.empty()) return;

    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glBindVertexArray(m_VAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER,
        mesh.vertices.size() * sizeof(Vertex),
        mesh.vertices.data(), GL_STATIC_DRAW);

    // position   loc=0
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(0);
    // normal     loc=1
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
        (void*)offsetof(Vertex, normal));
    glEnableVertexAttribArray(1);
    // uv         loc=2
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
        (void*)offsetof(Vertex, uv));
    glEnableVertexAttribArray(2);

    if (!mesh.indices.empty()) {
        glGenBuffers(1, &m_EBO);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, m_EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
            mesh.indices.size() * sizeof(uint16_t),
            mesh.indices.data(), GL_STATIC_DRAW);
        m_IndexCount = mesh.indices.size();
    }

    glBindVertexArray(0);
    m_VertexCount = mesh.vertices.size();
    m_HasMesh = true;

    // Auto-fit camera to mesh bounding box
    glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
    for (const auto& v : mesh.vertices) {
        glm::vec3 p(v.position[0], v.position[1], v.position[2]);
        mn = glm::min(mn, p);
        mx = glm::max(mx, p);
    }
    FitCamera((mn + mx) * 0.5f, glm::length(mx - mn));

    std::cout << "[Viewport3D] Mesh loaded: " << mesh.name
        << " (" << m_VertexCount << " verts, "
        << m_IndexCount / 3 << " tris)\n";
}

void Viewport3D::ClearMesh() {
    CleanupMeshGL();
    m_HasMesh = false;
}

//Skeleton loading

void Viewport3D::LoadSkeleton(const std::vector<Bone>& bones) {
    CleanupSkeletonGL();
    if (bones.empty()) return;

    // Y-flip sign: game uses Y-down, OpenGL Y-up
    const float ySign = m_FlipY ? -1.0f : 1.0f;

    // Build line vertex pairs (parent → child) and dot positions (every bone)
    std::vector<glm::vec3> lines;
    std::vector<glm::vec3> dots;
    lines.reserve(bones.size() * 2);
    dots.reserve(bones.size());

    glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);

    for (size_t i = 0; i < bones.size(); i++) {
        const Bone& b = bones[i];
        glm::vec3 wp(b.world_pos.x, b.world_pos.y * ySign, b.world_pos.z);
        dots.push_back(wp);
        mn = glm::min(mn, wp);
        mx = glm::max(mx, wp);

        if (b.parent_idx >= 0 && b.parent_idx < (int)bones.size()) {
            const Bone& p = bones[b.parent_idx];
            glm::vec3 pp(p.world_pos.x, p.world_pos.y * ySign, p.world_pos.z);
            lines.push_back(pp);
            lines.push_back(wp);
        }
    }

    // Upload lines
    if (!lines.empty()) {
        glGenVertexArrays(1, &m_LineVAO);
        glGenBuffers(1, &m_LineVBO);
        glBindVertexArray(m_LineVAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_LineVBO);
        glBufferData(GL_ARRAY_BUFFER,
            lines.size() * sizeof(glm::vec3), lines.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
        m_LineVertexCount = (int)lines.size();
    }

    // Upload dots
    if (!dots.empty()) {
        glGenVertexArrays(1, &m_DotVAO);
        glGenBuffers(1, &m_DotVBO);
        glBindVertexArray(m_DotVAO);
        glBindBuffer(GL_ARRAY_BUFFER, m_DotVBO);
        glBufferData(GL_ARRAY_BUFFER,
            dots.size() * sizeof(glm::vec3), dots.data(), GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
        glEnableVertexAttribArray(0);
        glBindVertexArray(0);
        m_DotCount = (int)dots.size();
    }

    m_HasSkeleton = true;
    FitCamera((mn + mx) * 0.5f, glm::length(mx - mn));

    std::cout << "[Viewport3D] Skeleton loaded: " << bones.size() << " bones, "
        << lines.size() / 2 << " segments\n";
}

void Viewport3D::ClearSkeleton() {
    CleanupSkeletonGL();
    m_HasSkeleton = false;
}

//FitCamera 

void Viewport3D::FitCamera(glm::vec3 center, float extent) {
    if (extent < 0.001f) extent = 1.0f;
    m_Camera.SetTarget(center);
    m_Camera.SetDistance(extent * 1.4f);
}

//Render

void Viewport3D::Render() {
    ImGui::Begin("3D Viewport", nullptr,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    //UI controls
    if (m_HasMesh) {
        ImGui::Checkbox("Mesh", &m_ShowMesh);
        ImGui::SameLine();
    }
    if (m_HasSkeleton) {
        ImGui::Checkbox("Skeleton", &m_ShowSkeleton);
        ImGui::SameLine();
    }
    if (m_HasMesh || m_HasSkeleton) {
        bool flipChanged = ImGui::Checkbox("Flip Y", &m_FlipY);
        (void)flipChanged;  // re-upload would be needed; for now just toggle flag
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset View")) {
            m_Camera.SetTarget(glm::vec3(0));
            m_Camera.SetDistance(5.0f);
        }
    }

    if (!m_HasMesh && !m_HasSkeleton) {
        ImGui::TextDisabled("Load a NSHP chunk or NBN2 skeleton to view.");
        ImGui::End();
        return;
    }

    //Framebuffer setup
    ImVec2 vp_pos = ImGui::GetCursorScreenPos();
    ImVec2 vp_size = ImGui::GetContentRegionAvail();
    m_ViewW = (int)vp_size.x;
    m_ViewH = (int)vp_size.y;
    if (m_ViewW <= 0 || m_ViewH <= 0) { ImGui::End(); return; }

    if (m_FBO == 0 || m_FBOw != m_ViewW || m_FBOh != m_ViewH) {
        if (m_FBO) {
            glDeleteFramebuffers(1, &m_FBO);
            glDeleteTextures(1, &m_FBOTex);
            glDeleteRenderbuffers(1, &m_FBORbo);
        }
        glGenFramebuffers(1, &m_FBO);
        glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);

        glGenTextures(1, &m_FBOTex);
        glBindTexture(GL_TEXTURE_2D, m_FBOTex);
        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, m_ViewW, m_ViewH, 0,
            GL_RGB, GL_UNSIGNED_BYTE, nullptr);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
        glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0,
            GL_TEXTURE_2D, m_FBOTex, 0);

        glGenRenderbuffers(1, &m_FBORbo);
        glBindRenderbuffer(GL_RENDERBUFFER, m_FBORbo);
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, m_ViewW, m_ViewH);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
            GL_RENDERBUFFER, m_FBORbo);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        m_FBOw = m_ViewW; m_FBOh = m_ViewH;
    }

    //Render to framebuffer
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glViewport(0, 0, m_ViewW, m_ViewH);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.18f, 0.18f, 0.22f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float aspect = float(m_ViewW) / float(m_ViewH);
    const glm::mat4 view = m_Camera.GetViewMatrix();
    const glm::mat4 proj = m_Camera.GetProjectionMatrix(aspect);
    const glm::vec3 camPos = m_Camera.GetPosition();

    // Y-flip applied via model matrix so mesh and skeleton share the same flip
    glm::mat4 model = glm::mat4(1.0f);
    if (m_FlipY)
        model = glm::scale(model, glm::vec3(1.0f, -1.0f, 1.0f));

    //Draw mesh
    if (m_HasMesh && m_ShowMesh && m_VAO) {
        m_MeshShader->Use();
        m_MeshShader->SetMat4("model", model);
        m_MeshShader->SetMat4("view", view);
        m_MeshShader->SetMat4("projection", proj);
        m_MeshShader->SetVec3("lightPos", camPos + glm::vec3(1, 2, 0));
        m_MeshShader->SetVec3("viewPos", camPos);
        m_MeshShader->SetVec3("objectColor", glm::vec3(0.72f, 0.72f, 0.88f));

        glBindVertexArray(m_VAO);
        if (m_IndexCount > 0)
            glDrawElements(GL_TRIANGLES, (GLsizei)m_IndexCount, GL_UNSIGNED_SHORT, nullptr);
        else
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)m_VertexCount);
        glBindVertexArray(0);
    }

    //Draw skeleton
    if (m_HasSkeleton && m_ShowSkeleton) {
        m_BoneShader->Use();
        // Bone positions are already in world space (no model matrix)
        // but we apply the Y-flip via the same model if needed —
        // actually for bones we built flipped positions in LoadSkeleton,
        // so we pass identity here.
        m_BoneShader->SetMat4("view", view);
        m_BoneShader->SetMat4("projection", proj);

        // Render after mesh so we can use depth test for depth-correct overlay
        // and disable it for "always on top" joints.

        // Lines (with depth test — shows correctly through mesh)
        if (m_LineVAO && m_LineVertexCount > 0) {
            m_BoneShader->SetVec4("color", glm::vec4(1.0f, 0.85f, 0.0f, 1.0f)); // yellow
            glLineWidth(2.0f);
            glBindVertexArray(m_LineVAO);
            glDrawArrays(GL_LINES, 0, m_LineVertexCount);
        }

        // Dots (depth test disabled so they're always visible on top)
        if (m_DotVAO && m_DotCount > 0) {
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_PROGRAM_POINT_SIZE);
            m_BoneShader->SetVec4("color", glm::vec4(1.0f, 1.0f, 1.0f, 1.0f)); // white
            glBindVertexArray(m_DotVAO);
            glDrawArrays(GL_POINTS, 0, m_DotCount);
            glDisable(GL_PROGRAM_POINT_SIZE);
            glEnable(GL_DEPTH_TEST);
        }

        glBindVertexArray(0);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // blit framebuffer texture into ImGui window
    ImGui::SetCursorScreenPos(vp_pos);
    ImGui::InvisibleButton("##vp",
        vp_size,
        ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight |
        ImGuiButtonFlags_MouseButtonMiddle);

    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    ImGui::GetWindowDrawList()->AddImage(
        (ImTextureID)(intptr_t)m_FBOTex,
        vp_pos,
        ImVec2(vp_pos.x + vp_size.x, vp_pos.y + vp_size.y),
        ImVec2(0, 1), ImVec2(1, 0));

    //Mouse input
    if (hovered || active) {
        ImGuiIO& io = ImGui::GetIO();

        // Left drag: orbit
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            ImVec2 d = io.MouseDelta;
            m_Camera.Rotate(d.x * 0.005f, d.y * 0.005f);
        }
        // Right drag: pan
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)) {
            ImVec2 d = io.MouseDelta;
            m_Camera.Pan(-d.x, d.y);
        }
        // Scroll: zoom
        if (io.MouseWheel != 0.0f)
            m_Camera.Zoom(-io.MouseWheel * (m_Camera.GetDistance() * 0.1f));
    }

    ImGui::End();
}

//GL cleanup helpers

void Viewport3D::CleanupMeshGL() {
    if (m_EBO) { glDeleteBuffers(1, &m_EBO); m_EBO = 0; }
    if (m_VBO) { glDeleteBuffers(1, &m_VBO); m_VBO = 0; }
    if (m_VAO) { glDeleteVertexArrays(1, &m_VAO); m_VAO = 0; }
    m_VertexCount = 0;
    m_IndexCount = 0;
}

void Viewport3D::CleanupSkeletonGL() {
    if (m_LineVBO) { glDeleteBuffers(1, &m_LineVBO); m_LineVBO = 0; }
    if (m_LineVAO) { glDeleteVertexArrays(1, &m_LineVAO); m_LineVAO = 0; }
    if (m_DotVBO) { glDeleteBuffers(1, &m_DotVBO);  m_DotVBO = 0; }
    if (m_DotVAO) { glDeleteVertexArrays(1, &m_DotVAO);  m_DotVAO = 0; }
    m_LineVertexCount = 0;
    m_DotCount = 0;
}