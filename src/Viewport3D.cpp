#include "Viewport3D.h"
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <iostream>
#include <algorithm>
#include <cfloat>

// Shaders 

static const char* MESH_VERT = R"(
#version 450 core
layout(location=0) in vec3 aPos;
layout(location=1) in vec3 aNormal;
layout(location=2) in vec2 aUV;
out vec3 FragPos;
out vec3 Normal;
out vec2 TexCoord;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
void main(){
    FragPos    = vec3(model * vec4(aPos, 1.0));
    Normal     = mat3(transpose(inverse(model))) * aNormal;
    TexCoord   = aUV;
    gl_Position = projection * view * vec4(FragPos, 1.0);
})";

static const char* MESH_FRAG = R"(
#version 450 core
in  vec3 FragPos;
in  vec3 Normal;
in  vec2 TexCoord;
out vec4 FragColor;
uniform vec3      lightPos;
uniform vec3      viewPos;
uniform vec3      objectColor;
uniform sampler2D u_Diffuse;
uniform sampler2D u_AlphaMask;
uniform int       u_UseTexture;
uniform int       u_HasAlphaMask;
uniform float     u_AlphaThreshold;
void main(){
    vec4 texSample = vec4(1.0);
    if (u_UseTexture != 0) {
        texSample = texture(u_Diffuse, TexCoord);
        // Alpha test — threshold depends on texture format:
        //   DXT1 punch-through: 0.5 (binary: 0 or 1)
        //   DXT5 full alpha:    0.01 (discard only near-fully-transparent pixels)
        if (texSample.a < u_AlphaThreshold) discard;
    }
    // Separate greyscale alpha mask texture (e.g. flowers on coloured background)
    if (u_HasAlphaMask != 0) {
        float mask = texture(u_AlphaMask, TexCoord).r;
        if (mask < 0.5) discard;
    }

    // Unlit: output texture colour directly, no lighting math.
    // Vertex normals in PS3 meshes are often imprecise enough that any lighting
    // model introduces visible artefacts (graininess, shadow patches).
    vec3 baseColor = (u_UseTexture != 0) ? texSample.rgb : objectColor;
    float outAlpha = (u_UseTexture != 0) ? texSample.a : 1.0;
    FragColor = vec4(baseColor, outAlpha);
})";

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

// Constructor / Destructor 

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
    CleanupModelGL();
    CleanupSkeletonGL();
    if (m_FBO)    glDeleteFramebuffers(1, &m_FBO);
    if (m_FBOTex) glDeleteTextures(1, &m_FBOTex);
    if (m_FBORbo) glDeleteRenderbuffers(1, &m_FBORbo);
    delete m_MeshShader;
    delete m_BoneShader;
}

//  GPU helpers 

void Viewport3D::UploadMeshToGPU(const NSHPMesh& mesh, MeshGPUData& out) {
    glGenVertexArrays(1, &out.VAO);
    glGenBuffers(1, &out.VBO);
    glBindVertexArray(out.VAO);

    glBindBuffer(GL_ARRAY_BUFFER, out.VBO);
    glBufferData(GL_ARRAY_BUFFER,
        mesh.vertices.size() * sizeof(Vertex),
        mesh.vertices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
        (void*)offsetof(Vertex, normal));
    glEnableVertexAttribArray(1);
    glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex),
        (void*)offsetof(Vertex, uv));
    glEnableVertexAttribArray(2);

    if (!mesh.indices.empty()) {
        glGenBuffers(1, &out.EBO);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, out.EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
            mesh.indices.size() * sizeof(uint16_t),
            mesh.indices.data(), GL_STATIC_DRAW);
    }

    glBindVertexArray(0);
    out.vertex_count = mesh.vertices.size();
    out.sections = mesh.faceSections;
    out.draw_sequential = mesh.draw_sequential;
}

//  Single mesh 

void Viewport3D::LoadMesh(const NSHPMesh& mesh) {
    ClearModel();
    CleanupMeshGL();
    if (mesh.vertices.empty()) return;

    glGenVertexArrays(1, &m_VAO);
    glGenBuffers(1, &m_VBO);
    glBindVertexArray(m_VAO);

    glBindBuffer(GL_ARRAY_BUFFER, m_VBO);
    glBufferData(GL_ARRAY_BUFFER,
        mesh.vertices.size() * sizeof(Vertex),
        mesh.vertices.data(), GL_STATIC_DRAW);

    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)0);
    glEnableVertexAttribArray(0);
    glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex),
        (void*)offsetof(Vertex, normal));
    glEnableVertexAttribArray(1);
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
    m_DrawSequential = mesh.draw_sequential;
    m_HasMesh = true;

    glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
    for (const auto& v : mesh.vertices) {
        glm::vec3 p(v.position[0], v.position[1], v.position[2]);
        mn = glm::min(mn, p); mx = glm::max(mx, p);
    }
    FitCamera((mn + mx) * 0.5f, glm::length(mx - mn));

    std::cout << "[Viewport3D] Mesh: " << mesh.name
        << " (" << m_VertexCount << " verts, "
        << m_IndexCount / 3 << " tris)\n";
}

void Viewport3D::ClearMesh() {
    CleanupMeshGL();
    m_HasMesh = false;
}

// NMDL model 

void Viewport3D::LoadModel(NMDLModel& model) {
    ClearMesh();
    ClearModel();

    for (auto& mesh : model.meshes) {
        if (mesh.vertices.empty()) continue;
        MeshGPUData gpu;
        UploadMeshToGPU(mesh, gpu);
        m_ModelMeshes.push_back(std::move(gpu));
    }

    m_MatToTex = std::move(model.mat_to_tex);
    m_MatToAlpha = std::move(model.mat_to_alpha);
    m_MatClampIds = std::move(model.mat_clamp_ids);
    m_MatBlendIds = std::move(model.mat_blend_ids);
    m_ModelTextures = std::move(model.owned_textures);
    m_HasModel = !m_ModelMeshes.empty();

    if (m_HasModel) {
        FitCamera((model.bbox_min + model.bbox_max) * 0.5f,
            glm::length(model.bbox_max - model.bbox_min));
        std::cout << "[Viewport3D] Model '" << model.name << "': "
            << m_ModelMeshes.size() << " meshes, "
            << m_MatToTex.size() << " textured materials\n";
    }
}

void Viewport3D::ClearModel() {
    CleanupModelGL();
    m_HasModel = false;
}

//  Skeleton 

void Viewport3D::LoadSkeleton(const std::vector<Bone>& bones) {
    CleanupSkeletonGL();
    if (bones.empty()) return;

    const float ySign = m_FlipY ? -1.0f : 1.0f;
    std::vector<glm::vec3> lines, dots;
    glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);

    for (size_t i = 0; i < bones.size(); i++) {
        const Bone& b = bones[i];
        glm::vec3 wp(b.world_pos.x, b.world_pos.y * ySign, b.world_pos.z);
        dots.push_back(wp);
        mn = glm::min(mn, wp); mx = glm::max(mx, wp);
        if (b.parent_idx >= 0 && b.parent_idx < (int)bones.size()) {
            const Bone& p = bones[b.parent_idx];
            glm::vec3 pp(p.world_pos.x, p.world_pos.y * ySign, p.world_pos.z);
            lines.push_back(pp);
            lines.push_back(wp);
        }
    }

    auto upload = [](GLuint& vao, GLuint& vbo,
        const std::vector<glm::vec3>& pts) {
            glGenVertexArrays(1, &vao);
            glGenBuffers(1, &vbo);
            glBindVertexArray(vao);
            glBindBuffer(GL_ARRAY_BUFFER, vbo);
            glBufferData(GL_ARRAY_BUFFER, pts.size() * sizeof(glm::vec3),
                pts.data(), GL_STATIC_DRAW);
            glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
            glEnableVertexAttribArray(0);
            glBindVertexArray(0);
        };

    if (!lines.empty()) { upload(m_LineVAO, m_LineVBO, lines); m_LineVertexCount = (int)lines.size(); }
    if (!dots.empty()) { upload(m_DotVAO, m_DotVBO, dots);  m_DotCount = (int)dots.size(); }

    m_HasSkeleton = true;
    FitCamera((mn + mx) * 0.5f, glm::length(mx - mn));
    std::cout << "[Viewport3D] Skeleton: " << bones.size() << " bones\n";
}

void Viewport3D::ClearSkeleton() {
    CleanupSkeletonGL();
    m_HasSkeleton = false;
}

//  FitCamera 

void Viewport3D::FitCamera(glm::vec3 center, float extent) {
    if (extent < 0.001f) extent = 1.0f;
    m_Camera.SetTarget(center);
    m_Camera.SetDistance(extent * 1.4f);
}

//  Render 

void Viewport3D::Render() {
    ImGui::Begin("3D Viewport", nullptr,
        ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoScrollWithMouse);

    const bool hasSomething = m_HasMesh || m_HasModel || m_HasSkeleton;
    if (m_HasMesh || m_HasModel) {
        ImGui::Checkbox("Mesh", &m_ShowMesh);
        ImGui::SameLine();
    }
    if (m_HasSkeleton) {
        ImGui::Checkbox("Skeleton", &m_ShowSkeleton);
        ImGui::SameLine();
    }
    if (hasSomething) {
        bool flipChanged = ImGui::Checkbox("Flip Y", &m_FlipY);
        (void)flipChanged;
        ImGui::SameLine();
        if (ImGui::SmallButton("Reset View")) {
            m_Camera.SetTarget(glm::vec3(0));
            m_Camera.SetDistance(5.0f);
        }
    }

    if (!hasSomething) {
        ImGui::TextDisabled("Load a NSHP / NMDL chunk or NBN2 skeleton to view.");
        ImGui::End();
        return;
    }

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

    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glViewport(0, 0, m_ViewW, m_ViewH);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.18f, 0.18f, 0.22f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float      aspect = float(m_ViewW) / float(m_ViewH);
    const glm::mat4  view = m_Camera.GetViewMatrix();
    const glm::mat4  proj = m_Camera.GetProjectionMatrix(aspect);
    const glm::vec3  camPos = m_Camera.GetPosition();

    glm::mat4 model = glm::mat4(1.0f);
    if (m_FlipY) model = glm::scale(model, glm::vec3(1.0f, -1.0f, 1.0f));

    // draw single mesh
    if (m_HasMesh && m_ShowMesh && m_VAO) {
        m_MeshShader->Use();
        m_MeshShader->SetMat4("model", model);
        m_MeshShader->SetMat4("view", view);
        m_MeshShader->SetMat4("projection", proj);
        m_MeshShader->SetVec3("lightPos", camPos + glm::vec3(1, 2, 0));
        m_MeshShader->SetVec3("viewPos", camPos);
        m_MeshShader->SetVec3("objectColor", glm::vec3(0.72f, 0.72f, 0.88f));
        m_MeshShader->SetInt("u_UseTexture", 0);
        m_MeshShader->SetInt("u_Diffuse", 0);

        glBindVertexArray(m_VAO);
        if (m_DrawSequential) {
            glEnable(GL_PROGRAM_POINT_SIZE);
            glPointSize(4.0f);
            glDrawArrays(GL_POINTS, 0, (GLsizei)m_VertexCount);
        }
        else if (m_IndexCount > 0) {
            glDrawElements(GL_TRIANGLES, (GLsizei)m_IndexCount,
                GL_UNSIGNED_SHORT, nullptr);
        }
        else {
            glDrawArrays(GL_TRIANGLES, 0, (GLsizei)m_VertexCount);
        }
        glBindVertexArray(0);
    }

    //Draw NMDL model (multi-mesh, per-section textures)
    if (m_HasModel && m_ShowMesh) {
        m_MeshShader->Use();
        m_MeshShader->SetMat4("model", model);
        m_MeshShader->SetMat4("view", view);
        m_MeshShader->SetMat4("projection", proj);
        m_MeshShader->SetVec3("lightPos", camPos + glm::vec3(1, 2, 0));
        m_MeshShader->SetVec3("viewPos", camPos);
        m_MeshShader->SetVec3("objectColor", glm::vec3(0.72f, 0.72f, 0.88f));
        m_MeshShader->SetInt("u_Diffuse", 0);
        m_MeshShader->SetInt("u_AlphaMask", 1);
        m_MeshShader->SetInt("u_HasAlphaMask", 0);
        m_MeshShader->SetFloat("u_AlphaThreshold", 0.5f);

        for (const auto& gpu : m_ModelMeshes) {
            if (!gpu.VAO) continue;
            glBindVertexArray(gpu.VAO);

            if (gpu.draw_sequential) {
                m_MeshShader->SetInt("u_UseTexture", 0);
                glEnable(GL_PROGRAM_POINT_SIZE);
                glPointSize(4.0f);
                glDrawArrays(GL_POINTS, 0, (GLsizei)gpu.vertex_count);

            }
            else if (gpu.sections.empty()) {
                m_MeshShader->SetInt("u_UseTexture", 0);
                glDrawElements(GL_TRIANGLES, 0, GL_UNSIGNED_SHORT, nullptr);

            }
            else {
                for (const auto& sec : gpu.sections) {
                    if (sec.index_count == 0) continue;
                    auto it = m_MatToTex.find(sec.mat_id);
                    if (it != m_MatToTex.end() && it->second != 0) {
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, it->second);
                        GLenum wrap = m_MatClampIds.count(sec.mat_id)
                            ? GL_CLAMP_TO_EDGE : GL_REPEAT;
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
                        m_MeshShader->SetInt("u_UseTexture", 1);
                        // DXT5: full 8-bit alpha → GL_BLEND + low discard threshold.
                        // DXT1: binary punch-through → no blend, threshold=0.5.
                        if (m_MatBlendIds.count(sec.mat_id)) {
                            glEnable(GL_BLEND);
                            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                            glDepthMask(GL_FALSE);  // transparent: don't occlude geometry behind
                            m_MeshShader->SetFloat("u_AlphaThreshold", 0.01f);
                        }
                        else {
                            glDisable(GL_BLEND);
                            glDepthMask(GL_TRUE);
                            m_MeshShader->SetFloat("u_AlphaThreshold", 0.5f);
                        }
                    }
                    else {
                        m_MeshShader->SetInt("u_UseTexture", 0);
                        glDisable(GL_BLEND);
                        m_MeshShader->SetFloat("u_AlphaThreshold", 0.5f);
                    }
                    auto ita = m_MatToAlpha.find(sec.mat_id);
                    if (ita != m_MatToAlpha.end() && ita->second != 0) {
                        glActiveTexture(GL_TEXTURE1);
                        glBindTexture(GL_TEXTURE_2D, ita->second);
                        m_MeshShader->SetInt("u_AlphaMask", 1);
                        m_MeshShader->SetInt("u_HasAlphaMask", 1);
                    }
                    else {
                        m_MeshShader->SetInt("u_HasAlphaMask", 0);
                    }
                    glDrawElements(GL_TRIANGLES,
                        (GLsizei)sec.index_count,
                        GL_UNSIGNED_SHORT,
                        (void*)(uintptr_t)(sec.index_start * sizeof(uint16_t)));
                }
            }
            glBindVertexArray(0);
        }
        glActiveTexture(GL_TEXTURE1);
        glBindTexture(GL_TEXTURE_2D, 0);
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);  // always restore depth writes after model draw
    }

    //Draw skeleton
    if (m_HasSkeleton && m_ShowSkeleton) {
        m_BoneShader->Use();
        m_BoneShader->SetMat4("view", view);
        m_BoneShader->SetMat4("projection", proj);

        if (m_LineVAO && m_LineVertexCount > 0) {
            m_BoneShader->SetVec4("color", glm::vec4(1.0f, 0.85f, 0.0f, 1.0f));
            glLineWidth(2.0f);
            glBindVertexArray(m_LineVAO);
            glDrawArrays(GL_LINES, 0, m_LineVertexCount);
        }
        if (m_DotVAO && m_DotCount > 0) {
            glDisable(GL_DEPTH_TEST);
            glEnable(GL_PROGRAM_POINT_SIZE);
            m_BoneShader->SetVec4("color", glm::vec4(1.0f, 1.0f, 1.0f, 1.0f));
            glBindVertexArray(m_DotVAO);
            glDrawArrays(GL_POINTS, 0, m_DotCount);
            glDisable(GL_PROGRAM_POINT_SIZE);
            glEnable(GL_DEPTH_TEST);
        }
        glBindVertexArray(0);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    ImGui::SetCursorScreenPos(vp_pos);
    ImGui::InvisibleButton("##vp", vp_size,
        ImGuiButtonFlags_MouseButtonLeft |
        ImGuiButtonFlags_MouseButtonRight |
        ImGuiButtonFlags_MouseButtonMiddle);

    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();

    ImGui::GetWindowDrawList()->AddImage(
        (ImTextureID)(intptr_t)m_FBOTex,
        vp_pos,
        ImVec2(vp_pos.x + vp_size.x, vp_pos.y + vp_size.y),
        ImVec2(0, 1), ImVec2(1, 0));

    if (hovered || active) {
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.0f)) {
            ImVec2 d = io.MouseDelta;
            m_Camera.Rotate(d.x * 0.005f, d.y * 0.005f);
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.0f)) {
            ImVec2 d = io.MouseDelta;
            m_Camera.Pan(-d.x, d.y);
        }
        if (io.MouseWheel != 0.0f)
            m_Camera.Zoom(-io.MouseWheel * (m_Camera.GetDistance() * 0.1f));
    }

    ImGui::End();
}

// GL cleanup

void Viewport3D::CleanupMeshGL() {
    if (m_EBO) { glDeleteBuffers(1, &m_EBO); m_EBO = 0; }
    if (m_VBO) { glDeleteBuffers(1, &m_VBO); m_VBO = 0; }
    if (m_VAO) { glDeleteVertexArrays(1, &m_VAO); m_VAO = 0; }
    m_VertexCount = 0;
    m_IndexCount = 0;
    m_DrawSequential = false;
}

void Viewport3D::CleanupModelGL() {
    for (auto& gpu : m_ModelMeshes) {
        if (gpu.EBO) glDeleteBuffers(1, &gpu.EBO);
        if (gpu.VBO) glDeleteBuffers(1, &gpu.VBO);
        if (gpu.VAO) glDeleteVertexArrays(1, &gpu.VAO);
    }
    m_ModelMeshes.clear();
    for (GLuint t : m_ModelTextures) if (t) glDeleteTextures(1, &t);
    m_ModelTextures.clear();
    m_MatToTex.clear();
    m_MatToAlpha.clear();
    m_MatClampIds.clear();
    m_MatBlendIds.clear();
}

void Viewport3D::CleanupSkeletonGL() {
    if (m_LineVBO) { glDeleteBuffers(1, &m_LineVBO); m_LineVBO = 0; }
    if (m_LineVAO) { glDeleteVertexArrays(1, &m_LineVAO); m_LineVAO = 0; }
    if (m_DotVBO) { glDeleteBuffers(1, &m_DotVBO);  m_DotVBO = 0; }
    if (m_DotVAO) { glDeleteVertexArrays(1, &m_DotVAO);  m_DotVAO = 0; }
    m_LineVertexCount = 0;
    m_DotCount = 0;
}