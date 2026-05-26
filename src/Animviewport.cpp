#include "AnimViewport.h"
#include "NMDLLoader.h"
#include "NBN2Parser.h"
#include "NSHPParser.h"
#include "NMTNParser.h"
#include <imgui.h>
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/type_ptr.hpp>
#include <iostream>
#include <cstring>
#include <algorithm>
#include <cfloat>

static const char* SKIN_VERT = R"(
#version 450 core
layout(location=0) in vec3  aPos;
layout(location=1) in vec3  aNormal;
layout(location=2) in vec2  aUV;
layout(location=3) in vec4  aBoneWeights;
layout(location=4) in uvec4 aBoneIds;
out vec3 FragPos;
out vec3 Normal;
uniform mat4 u_View;
uniform mat4 u_Proj;
uniform mat4 u_SkinMats[64];
void main() {
    vec4 w = aBoneWeights;
    float ws = w.x + w.y + w.z + w.w;
    if (ws > 0.001) w /= ws;
    vec4 skinPos = vec4(0.0);
    skinPos += w.x * u_SkinMats[aBoneIds.x] * vec4(aPos, 1.0);
    skinPos += w.y * u_SkinMats[aBoneIds.y] * vec4(aPos, 1.0);
    skinPos += w.z * u_SkinMats[aBoneIds.z] * vec4(aPos, 1.0);
    skinPos += w.w * u_SkinMats[aBoneIds.w] * vec4(aPos, 1.0);
    mat3 nm = mat3(u_SkinMats[aBoneIds.x]) * w.x
            + mat3(u_SkinMats[aBoneIds.y]) * w.y
            + mat3(u_SkinMats[aBoneIds.z]) * w.z
            + mat3(u_SkinMats[aBoneIds.w]) * w.w;
    FragPos     = skinPos.xyz;
    Normal      = normalize(nm * aNormal);
    gl_Position = u_Proj * u_View * skinPos;
}
)";

static const char* SKIN_FRAG = R"(
#version 450 core
in  vec3 FragPos;
in  vec3 Normal;
out vec4 FragColor;
uniform vec3 u_LightDir;
uniform vec3 u_MeshColor;
void main() {
    vec3 N = normalize(Normal);
    if (!gl_FrontFacing) N = -N;
    float diff = max(dot(N, u_LightDir), 0.0);
    vec3 col   = (0.25 + 0.75 * diff) * u_MeshColor;
    FragColor  = vec4(col, 1.0);
}
)";

static const char* BONE_VERT = R"(
#version 450 core
layout(location=0) in vec3 aPos;
uniform mat4 u_View;
uniform mat4 u_Proj;
void main() {
    gl_Position  = u_Proj * u_View * vec4(aPos, 1.0);
    gl_PointSize = 6.0;
}
)";

static const char* BONE_FRAG = R"(
#version 450 core
out vec4 FragColor;
uniform vec4 u_Color;
void main() { FragColor = u_Color; }
)";

// Constructor / Destructor
AnimViewport::AnimViewport() {
    m_Camera.SetDistance(3.0f);
    m_Camera.SetFOV(50.0f);
    m_SkinShader = new Shader();
    if (!m_SkinShader->CompileFromSource(SKIN_VERT, SKIN_FRAG))
        std::cerr << "[AnimViewport] Skin shader compile failed\n";
    m_BoneShader = new Shader();
    if (!m_BoneShader->CompileFromSource(BONE_VERT, BONE_FRAG))
        std::cerr << "[AnimViewport] Bone shader compile failed\n";
}

AnimViewport::~AnimViewport() {
    Clear();
    delete m_SkinShader;
    delete m_BoneShader;
    if (m_FBO)    glDeleteFramebuffers(1, &m_FBO);
    if (m_FBOTex) glDeleteTextures(1, &m_FBOTex);
    if (m_FBORbo) glDeleteRenderbuffers(1, &m_FBORbo);
}

void AnimViewport::Clear() {
    CleanupMeshes();
    CleanupSkeleton();
    m_BindBones.clear();
    m_AnimBones.clear();
    m_InvBind.clear();
    m_BoneIndexMap.clear();
    m_Anims.clear();
    m_AnimIdx = 0;
    m_Frame = 0.f;
    m_Playing = false;
    m_HasContent = false;
}

void AnimViewport::CleanupMeshes() {
    for (auto& m : m_Meshes) {
        if (m.EBO) glDeleteBuffers(1, &m.EBO);
        if (m.VBO) glDeleteBuffers(1, &m.VBO);
        if (m.VAO) glDeleteVertexArrays(1, &m.VAO);
    }
    m_Meshes.clear();
}

void AnimViewport::CleanupSkeleton() {
    if (m_LineVBO) { glDeleteBuffers(1, &m_LineVBO); m_LineVBO = 0; }
    if (m_LineVAO) { glDeleteVertexArrays(1, &m_LineVAO); m_LineVAO = 0; }
    m_LineVerts = 0;
}

// GPU upload
void AnimViewport::UploadSkinMesh(const NSHPMesh& mesh, SkinMeshGPU& out) {
    if (mesh.vertices.empty()) return;
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
    glVertexAttribPointer(3, 4, GL_FLOAT, GL_FALSE, sizeof(Vertex),
        (void*)offsetof(Vertex, bone_weights));
    glEnableVertexAttribArray(3);
    glVertexAttribIPointer(4, 4, GL_UNSIGNED_BYTE, sizeof(Vertex),
        (void*)offsetof(Vertex, bone_ids));
    glEnableVertexAttribArray(4);
    if (!mesh.indices.empty()) {
        glGenBuffers(1, &out.EBO);
        glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, out.EBO);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER,
            mesh.indices.size() * sizeof(uint16_t),
            mesh.indices.data(), GL_STATIC_DRAW);
        out.index_count = mesh.indices.size();
    }
    glBindVertexArray(0);
    out.vertex_count = mesh.vertices.size();
    out.draw_sequential = mesh.draw_sequential;
    out.bone_id_list = mesh.boneIDList;
}

// LoadFromChunks
void AnimViewport::LoadFromChunks(const std::vector<uint8_t>& file_data,
    const std::vector<Chunk>& chunks) {
    Clear();
    if (file_data.empty()) return;

    const uint8_t* fd = file_data.data();
    size_t         fsz = file_data.size();

    bool has_nbn2 = false;

    // Pass 1: skeleton
    for (const auto& c : chunks) {
        if (c.type == ChunkType::NBN2 && !has_nbn2) {
            if (c.offset + c.size <= fsz) {
                if (NBN2Parser::Parse(fd + c.offset, c.size, m_BindBones)) {
                    has_nbn2 = true;
                    for (int i = 0; i < (int)m_BindBones.size(); i++)
                        m_BoneIndexMap[m_BindBones[i].name] = i;
                    BuildInvBind();
                }
            }
        }
    }
    if (!has_nbn2) return;
    m_AnimBones = m_BindBones;

    // Pass 2: skinned meshes
    for (const auto& c : chunks) {
        if (c.type == ChunkType::NSHP && c.offset + c.size <= fsz) {
            NSHPMesh mesh;
            if (NSHPParser::Parse(fd + c.offset, c.size, mesh)
                && mesh.has_bones
                && !mesh.vertices.empty()
                && !mesh.boneIDList.empty()) {
                SkinMeshGPU gpu;
                UploadSkinMesh(mesh, gpu);
                if (gpu.VAO) m_Meshes.push_back(std::move(gpu));
            }
        }
    }

    // Pass 3: NMTN animation clips
    for (const auto& c : chunks) {
        if (c.type == ChunkType::NMTN && c.offset + c.size <= fsz) {
            NMTNAnimation anim;
            if (NMTNParser::Parse(fd + c.offset, c.size, anim)
                && anim.frame_count > 0) {
                // Ensure unique display name
                if (anim.name.empty())
                    anim.name = "anim_" + std::to_string(m_Anims.size());
                m_Anims.push_back(std::move(anim));
            }
        }
    }

    if (m_BindBones.empty()) return;
    m_HasContent = true;

    // Fit camera to bind pose bbox
    glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
    for (const auto& b : m_BindBones) {
        mn = glm::min(mn, b.world_pos);
        mx = glm::max(mx, b.world_pos);
    }
    glm::vec3 center = (mn + mx) * 0.5f;
    float extent = glm::length(mx - mn);
    if (extent < 0.1f) extent = 2.f;
    m_Camera.SetTarget(center);
    m_Camera.SetDistance(extent * 0.8f);

    RebuildSkeletonLines();
    std::cout << "[AnimViewport] Loaded: "
        << m_BindBones.size() << " bones, "
        << m_Meshes.size() << " skinned meshes, "
        << m_Anims.size() << " animations\n";
}

// Inverse bind matrices
glm::mat4 AnimViewport::WorldMat(const Bone& b) const {
    glm::mat4 m(b.world_rot);
    m[3] = glm::vec4(b.world_pos, 1.f);
    return m;
}

void AnimViewport::BuildInvBind() {
    m_InvBind.resize(m_BindBones.size());
    for (size_t i = 0; i < m_BindBones.size(); i++)
        m_InvBind[i] = glm::inverse(WorldMat(m_BindBones[i]));
}

// Pose application
void AnimViewport::ApplyPose(float frame) {
    if (m_Anims.empty()) return;
    const NMTNAnimation& anim = m_Anims[m_AnimIdx];

    // Start from bind pose every frame
    m_AnimBones = m_BindBones;

    for (const auto& track : anim.tracks) {
        if (track.is_static) continue;

        auto it = m_BoneIndexMap.find(track.bone_name);
        if (it == m_BoneIndexMap.end()) continue;
        Bone& b = m_AnimBones[it->second];

        if (track.is_translation) {
            // move/pos/Top1: c0→dx(lateral)  c2→dy(vertical)  c1→dz(skip root-motion)
            b.local_x += track.ry.Sample(frame, 0.f);  // c0 stored in ry → dx lateral
            b.local_y += track.rz.Sample(frame, 0.f);  // c2 stored in rz → dy vertical
            // track.rx = c1 (root-motion forward) — intentionally skipped
        }
        else {
            // Rotation bones: YXZ mapping (c0→ry, c1→rx, c2→rz)
            // All values are deltas in radians, added to NBN2 bind-pose angles
            b.rot_y += track.ry.Sample(frame, 0.f);  // c0: primary sagittal
            b.rot_x += track.rx.Sample(frame, 0.f);  // c1
            b.rot_z += track.rz.Sample(frame, 0.f);  // c2
        }
    }

    NBN2Parser::ComputeWorldPositions(m_AnimBones);
}

void AnimViewport::RebuildSkeletonLines() {
    CleanupSkeleton();
    const auto& bones = m_AnimBones.empty() ? m_BindBones : m_AnimBones;
    if (bones.empty()) return;

    std::vector<glm::vec3> lines;
    lines.reserve(bones.size() * 2);
    for (const auto& b : bones) {
        if (b.parent_idx >= 0 && b.parent_idx < (int)bones.size()) {
            lines.push_back(bones[b.parent_idx].world_pos);
            lines.push_back(b.world_pos);
        }
    }
    if (lines.empty()) return;

    glGenVertexArrays(1, &m_LineVAO);
    glGenBuffers(1, &m_LineVBO);
    glBindVertexArray(m_LineVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m_LineVBO);
    glBufferData(GL_ARRAY_BUFFER,
        lines.size() * sizeof(glm::vec3), lines.data(), GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    m_LineVerts = (int)lines.size();
}

// Tick
void AnimViewport::Tick(float dt) {
    if (!m_HasContent || !m_Playing || m_Anims.empty()) return;
    const NMTNAnimation& anim = m_Anims[m_AnimIdx];
    if (anim.frame_count == 0) return;
    m_Frame = std::fmod(m_Frame + dt * m_PlaySpeed,
        (float)anim.frame_count);
    ApplyPose(m_Frame);
    RebuildSkeletonLines();
}

// Static helper
void AnimViewport::SetMat4Array(GLuint prog, const char* name,
    const glm::mat4* mats, int count) {
    GLint loc = glGetUniformLocation(prog, name);
    if (loc >= 0)
        glUniformMatrix4fv(loc, count, GL_FALSE, glm::value_ptr(mats[0]));
}

// Render
void AnimViewport::Render() {
    ImGui::Begin("Animation Viewport");

    if (!m_HasContent) {
        ImGui::TextDisabled("Load a .p3obj or .e file with NBN2 + NMTN chunks.");
        ImGui::End();
        return;
    }

    // Animation selector & controls
    if (!m_Anims.empty()) {
        ImGui::PushItemWidth(200);
        const char* preview = m_Anims[m_AnimIdx].name.empty()
            ? "(anim)" : m_Anims[m_AnimIdx].name.c_str();
        if (ImGui::BeginCombo("##anim", preview)) {
            for (int i = 0; i < (int)m_Anims.size(); i++) {
                bool sel = (i == m_AnimIdx);
                ImGui::PushID(i);   // guarantees unique ID even if names match
                const char* lbl = m_Anims[i].name.empty()
                    ? "(anim)" : m_Anims[i].name.c_str();
                if (ImGui::Selectable(lbl, sel)) {
                    m_AnimIdx = i;
                    m_Frame = 0.f;
                    ApplyPose(0.f);
                    RebuildSkeletonLines();
                }
                if (sel) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();

        ImGui::SameLine();
        if (ImGui::Button(m_Playing ? "Pause" : " Play ")) {
            m_Playing = !m_Playing;
            if (m_Playing && m_AnimBones.empty()) {
                ApplyPose(m_Frame);
                RebuildSkeletonLines();
            }
        }
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            m_Frame = 0.f;
            m_Playing = false;
            ApplyPose(0.f);
            RebuildSkeletonLines();
        }

        const NMTNAnimation& anim = m_Anims[m_AnimIdx];
        float maxF = (float)(anim.frame_count > 0 ? anim.frame_count - 1 : 1);
        ImGui::PushItemWidth(-1);
        if (ImGui::SliderFloat("##frame", &m_Frame, 0.f, maxF, "Frame %.1f")) {
            m_Playing = false;
            ApplyPose(m_Frame);
            RebuildSkeletonLines();
        }
        ImGui::PopItemWidth();
        ImGui::SameLine(0, 8);
        ImGui::Text("%d frames", anim.frame_count);

        ImGui::SliderFloat("Speed", &m_PlaySpeed, 1.f, 60.f, "%.0f fps");
        ImGui::SameLine();
        ImGui::Checkbox("Skeleton", &m_ShowSkel);
        ImGui::Text("Bones: %d   Meshes: %d",
            (int)m_BindBones.size(), (int)m_Meshes.size());
    }
    else {
        ImGui::TextDisabled("No NMTN animations found in this file.");
        ImGui::Text("Bones: %d   Meshes: %d",
            (int)m_BindBones.size(), (int)m_Meshes.size());
        ImGui::Checkbox("Skeleton", &m_ShowSkel);
    }

    ImGui::Separator();

    // Offscreen framebuffer
    ImVec2 vp_pos = ImGui::GetCursorScreenPos();
    ImVec2 avail = ImGui::GetContentRegionAvail();
    m_ViewW = std::max((int)avail.x, 64);
    m_ViewH = std::max((int)avail.y, 64);

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
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8,
            m_ViewW, m_ViewH);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
            GL_RENDERBUFFER, m_FBORbo);

        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        m_FBOw = m_ViewW;
        m_FBOh = m_ViewH;
    }

    // 3D render
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glViewport(0, 0, m_ViewW, m_ViewH);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.12f, 0.12f, 0.16f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float     aspect = (float)m_ViewW / (float)m_ViewH;
    const glm::mat4 view = m_Camera.GetViewMatrix();
    const glm::mat4 proj = m_Camera.GetProjectionMatrix(aspect);
    const glm::vec3 lightDir = glm::normalize(glm::vec3(0.5f, 1.0f, 0.7f));

    // Skinned meshes
    if (!m_Meshes.empty() && m_SkinShader) {
        m_SkinShader->Use();
        m_SkinShader->SetMat4("u_View", view);
        m_SkinShader->SetMat4("u_Proj", proj);
        m_SkinShader->SetVec3("u_LightDir", lightDir);
        m_SkinShader->SetVec3("u_MeshColor", glm::vec3(0.72f, 0.72f, 0.88f));

        GLuint prog = m_SkinShader->GetProgram();
        const auto& pose = m_AnimBones.empty() ? m_BindBones : m_AnimBones;

        for (const auto& mesh : m_Meshes) {
            if (!mesh.VAO || mesh.bone_id_list.empty()) continue;

            int n_local = std::min((int)mesh.bone_id_list.size(), 64);
            glm::mat4 skin_mats[64];
            for (int local = 0; local < n_local; ++local) {
                uint16_t global = mesh.bone_id_list[local];
                skin_mats[local] = (global < (uint16_t)pose.size())
                    ? WorldMat(pose[global]) * m_InvBind[global]
                    : glm::mat4(1.f);
            }
            SetMat4Array(prog, "u_SkinMats", skin_mats, n_local);

            glBindVertexArray(mesh.VAO);
            if (mesh.draw_sequential)
                glDrawArrays(GL_POINTS, 0, (GLsizei)mesh.vertex_count);
            else if (mesh.index_count > 0)
                glDrawElements(GL_TRIANGLES, (GLsizei)mesh.index_count,
                    GL_UNSIGNED_SHORT, nullptr);
            else
                glDrawArrays(GL_TRIANGLES, 0, (GLsizei)mesh.vertex_count);
            glBindVertexArray(0);
        }
    }

    // Skeleton lines
    if (m_ShowSkel && m_LineVAO && m_LineVerts > 0 && m_BoneShader) {
        m_BoneShader->Use();
        m_BoneShader->SetMat4("u_View", view);
        m_BoneShader->SetMat4("u_Proj", proj);
        m_BoneShader->SetVec4("u_Color", glm::vec4(1.f, 0.85f, 0.f, 1.f));
        glLineWidth(1.5f);
        glBindVertexArray(m_LineVAO);
        glDrawArrays(GL_LINES, 0, m_LineVerts);
        glBindVertexArray(0);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    // ImGui image & mouse input
    ImVec2 img_size((float)m_ViewW, (float)m_ViewH);
    ImGui::SetCursorScreenPos(vp_pos);
    ImGui::InvisibleButton("##av", img_size,
        ImGuiButtonFlags_MouseButtonLeft |
        ImGuiButtonFlags_MouseButtonRight |
        ImGuiButtonFlags_MouseButtonMiddle);
    ImGui::GetWindowDrawList()->AddImage(
        (ImTextureID)(intptr_t)m_FBOTex,
        vp_pos,
        ImVec2(vp_pos.x + img_size.x, vp_pos.y + img_size.y),
        ImVec2(0, 1), ImVec2(1, 0));

    if (ImGui::IsItemHovered() || ImGui::IsItemActive()) {
        ImGuiIO& io = ImGui::GetIO();
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 0.f)) {
            ImVec2 d = io.MouseDelta;
            m_Camera.Rotate(d.x * 0.005f, d.y * 0.005f);
        }
        if (ImGui::IsMouseDragging(ImGuiMouseButton_Right, 0.f)) {
            ImVec2 d = io.MouseDelta;
            m_Camera.Pan(-d.x, d.y);
        }
        if (io.MouseWheel != 0.f)
            m_Camera.Zoom(-io.MouseWheel * (m_Camera.GetDistance() * 0.1f));
    }

    ImGui::End();
}