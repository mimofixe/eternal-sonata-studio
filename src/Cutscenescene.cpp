#include "Cutscenescene.h"
#include "P3TexParser.h"
#include "FileDialog.h"
#include <imgui.h>
#include <glm/gtc/matrix_transform.hpp>
#include <fstream>
#include <cstring>
#include <cstdio>
#include <cctype>
#include <iostream>

// Shaders: same model as Viewport3D's (texture * vertex-alpha * NLIT lighting),
// with the per-instance "model" matrix doing the actual scene placement.
static const char* CS_VERT = R"(
#version 450 core
layout(location=0) in vec3  aPos;
layout(location=1) in vec3  aNormal;
layout(location=2) in vec2  aUV;
layout(location=3) in float aBoneAlpha;
out vec3  FragPos;
out vec3  Normal;
out vec2  TexCoord;
out float v_Alpha;
uniform mat4 model;
uniform mat4 view;
uniform mat4 projection;
uniform int  u_UseVertexAlpha;
void main(){
    FragPos    = vec3(model * vec4(aPos, 1.0));
    Normal     = mat3(transpose(inverse(model))) * aNormal;
    TexCoord   = aUV;
    v_Alpha    = (u_UseVertexAlpha != 0) ? aBoneAlpha : 1.0;
    gl_Position = projection * view * vec4(FragPos, 1.0);
})";

static const char* CS_FRAG = R"(
#version 450 core
in  vec3  FragPos;
in  vec3  Normal;
in  vec2  TexCoord;
in  float v_Alpha;
out vec4 FragColor;
uniform vec3      objectColor;
uniform sampler2D u_Diffuse;
uniform sampler2D u_AlphaMask;
uniform int       u_UseTexture;
uniform int       u_HasAlphaMask;
uniform float     u_AlphaThreshold;
uniform int       u_EnableLighting;
uniform vec3      u_AmbientColor;
uniform vec3      u_DirLightColor;
uniform vec3      u_DirLightDir;
void main(){
    vec4 texSample = vec4(1.0);
    if (u_UseTexture != 0) {
        texSample = texture(u_Diffuse, TexCoord);
        if (texSample.a < u_AlphaThreshold) discard;
    }
    if (u_HasAlphaMask != 0) {
        float mask = texture(u_AlphaMask, TexCoord).r;
        if (mask < 0.5) discard;
    }
    vec3 baseColor = (u_UseTexture != 0) ? texSample.rgb : objectColor;
    float outAlpha = ((u_UseTexture != 0) ? texSample.a : 1.0) * v_Alpha;
    if (u_EnableLighting != 0) {
        vec3 N = normalize(Normal);
        if (!gl_FrontFacing) N = -N;
        float ndotl = max(dot(N, u_DirLightDir), 0.0);
        vec3 lit = u_AmbientColor * baseColor + u_DirLightColor * baseColor * ndotl;
        FragColor = vec4(lit, outAlpha);
    } else {
        FragColor = vec4(baseColor, outAlpha);
    }
})";

CutsceneScene::CutsceneScene() {}

CutsceneScene::~CutsceneScene() {
    Clear();
    if (m_FBO)    glDeleteFramebuffers(1, &m_FBO);
    if (m_FBOTex) glDeleteTextures(1, &m_FBOTex);
    if (m_FBORbo) glDeleteRenderbuffers(1, &m_FBORbo);
    delete m_Shader;
}

void CutsceneScene::Clear() {
    for (auto& inst : m_Instances) DestroyInstance(inst);
    m_Instances.clear();
    for (GLuint t : m_BankTextures) if (t) glDeleteTextures(1, &t);
    m_BankTextures.clear();
    m_BankDXT5.clear();
    m_HasSceneLighting = false;
    m_Status[0] = 0;
}

void CutsceneScene::DestroyInstance(CSSceneInstance& inst) {
    for (auto& g : inst.meshes) {
        if (g.VAO) glDeleteVertexArrays(1, &g.VAO);
        if (g.VBO) glDeleteBuffers(1, &g.VBO);
        if (g.EBO) glDeleteBuffers(1, &g.EBO);
    }
    inst.meshes.clear();
    for (GLuint t : inst.owned_textures) if (t) glDeleteTextures(1, &t);
    inst.owned_textures.clear();
}

bool CutsceneScene::ReadFileBytes(const std::string& path, std::vector<uint8_t>& out) {
    std::ifstream f(path, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    size_t sz = (size_t)f.tellg();
    f.seekg(0, std::ios::beg);
    out.resize(sz);
    f.read((char*)out.data(), sz);
    return true;
}

// Find an NMDL chunk by case-insensitive name substring (name at chunk+0x10).
// Empty filter = first NMDL in the file (for maps: the main map model).
size_t CutsceneScene::FindNMDL(const std::vector<uint8_t>& fd, const std::string& name_filter) {
    std::string flt;
    for (char c : name_filter) flt += (char)tolower((unsigned char)c);
    for (size_t i = 0; i + 0x20 < fd.size(); i++) {
        if (fd[i] == 'N' && fd[i + 1] == 'M' && fd[i + 2] == 'D' && fd[i + 3] == 'L') {
            if (flt.empty()) return i;
            std::string nm;
            for (size_t j = i + 0x10; j < i + 0x20 && fd[j]; j++)
                nm += (char)tolower((unsigned char)fd[j]);
            if (nm.find(flt) != std::string::npos) return i;
        }
    }
    return (size_t)-1;
}

// NLIT scan (same logic as ChunkInspector): ambient (type 0) + directional (type 1).
void CutsceneScene::ScanNLIT(const std::vector<uint8_t>& fd) {
    size_t sp = 0;
    while (sp + 8 <= fd.size()) {
        if (memcmp(fd.data() + sp, "NLIT", 4) == 0) {
            uint32_t nsz = ((uint32_t)fd[sp + 4] << 24) | ((uint32_t)fd[sp + 5] << 16) |
                ((uint32_t)fd[sp + 6] << 8) | fd[sp + 7];
            size_t ep = sp + 8;
            bool fa = false, fdir = false;
            uint8_t ar = 180, ag = 180, ab = 180, dr = 220, dg = 220, db = 220;
            while (ep + 48 <= sp + nsz && ep + 48 <= fd.size()) {
                const uint8_t* e = fd.data() + ep;
                uint8_t type_b = e[16];
                if (type_b == 0x00 && !fa) { ar = e[20]; ag = e[21]; ab = e[22]; fa = true; }
                else if (type_b == 0x01 && !fdir) { dr = e[20]; dg = e[21]; db = e[22]; fdir = true; }
                ep += 48;
            }
            if (fa || fdir) {
                m_AmbientColor = glm::vec3(ar / 255.0f, ag / 255.0f, ab / 255.0f);
                m_DirLightColor = glm::vec3(dr / 255.0f, dg / 255.0f, db / 255.0f);
                m_HasSceneLighting = true;
            }
            return;
        }
        sp += 4;
    }
}

void CutsceneScene::UploadMesh(const NSHPMesh& mesh, CSMeshGPU& out) {
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
    // Attrib 3 = bone_weights[0], repurposed as per-vertex alpha (see NSHPParser).
    glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex),
        (void*)offsetof(Vertex, bone_weights));
    glEnableVertexAttribArray(3);

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
    out.needs_depth_offset = mesh.needs_depth_offset;
    out.has_vertex_alpha = mesh.has_vertex_alpha;
}

bool CutsceneScene::LoadModelInstance(const std::vector<uint8_t>& fd, size_t nmdl_off,
    bool is_map, CSSceneInstance& out) {
    NMDLModel mdl;
    const std::vector<GLuint>* ext = nullptr;
    const std::vector<bool>* extD = nullptr;
    if (is_map && !m_BankTextures.empty()) {
        ext = &m_BankTextures;
        if (!m_BankDXT5.empty()) extD = &m_BankDXT5;
    }
    if (!NMDLLoader::Load(fd.data(), fd.size(), nmdl_off, mdl, ext, extD))
        return false;

    out.name = mdl.name;
    out.is_map = is_map;
    out.bbox_min = mdl.bbox_min;
    out.bbox_max = mdl.bbox_max;
    for (auto& mesh : mdl.meshes) {
        if (mesh.vertices.empty()) continue;
        CSMeshGPU gpu;
        UploadMesh(mesh, gpu);
        out.meshes.push_back(gpu);
    }
    out.mat_to_tex = std::move(mdl.mat_to_tex);
    out.mat_to_alpha = std::move(mdl.mat_to_alpha);
    out.mat_clamp_ids = std::move(mdl.mat_clamp_ids);
    out.mat_blend_ids = std::move(mdl.mat_blend_ids);
    out.owned_textures = std::move(mdl.owned_textures);
    return !out.meshes.empty();
}

bool CutsceneScene::LoadMap(const std::string& map_path, const std::string& p3tex_path) {
    // 1. Texture bank: decode every texture of the .p3tex and upload to GL
    //    (mirror of the proven ChunkInspector flow; NO mipmaps: atlas textures).
    P3TexParser bank;
    if (!bank.Load(p3tex_path)) {
        snprintf(m_Status, sizeof(m_Status), "Failed to load %s", p3tex_path.c_str());
        return false;
    }
    for (GLuint t : m_BankTextures) if (t) glDeleteTextures(1, &t);
    m_BankTextures.clear();
    m_BankDXT5.clear();
    for (size_t i = 0; i < bank.GetTextureCount(); i++) {
        const P3Texture* tex = bank.GetTexture((uint8_t)i);
        if (!tex || tex->width == 0 || tex->height == 0) {
            m_BankTextures.push_back(0);
            m_BankDXT5.push_back(false);
            continue;
        }
        std::vector<uint8_t> rgba;
        if ((tex->format & 0xDF) == 0x86)
            rgba = P3TexParser::DecompressDXT1(tex->data.data(), tex->width, tex->height);
        else
            rgba = P3TexParser::DecompressDXT5(tex->data.data(), tex->width, tex->height);
        GLuint glTex = 0;
        if (!rgba.empty()) {
            glGenTextures(1, &glTex);
            glBindTexture(GL_TEXTURE_2D, glTex);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex->width, tex->height, 0,
                GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            glBindTexture(GL_TEXTURE_2D, 0);
        }
        m_BankTextures.push_back(glTex);
        m_BankDXT5.push_back(tex->format != 0x86);
    }

    // 2. Map file: main NMDL (the first one) with external textures + NLIT lighting
    std::vector<uint8_t> fd;
    if (!ReadFileBytes(map_path, fd)) {
        snprintf(m_Status, sizeof(m_Status), "Failed to read %s", map_path.c_str());
        return false;
    }
    size_t off = FindNMDL(fd, "");
    if (off == (size_t)-1) {
        snprintf(m_Status, sizeof(m_Status), "No NMDL in %s", map_path.c_str());
        return false;
    }
    // Replace any previous map instance
    for (size_t i = 0; i < m_Instances.size(); ) {
        if (m_Instances[i].is_map) {
            DestroyInstance(m_Instances[i]);
            m_Instances.erase(m_Instances.begin() + i);
        }
        else i++;
    }
    CSSceneInstance inst;
    if (!LoadModelInstance(fd, off, true, inst)) {
        snprintf(m_Status, sizeof(m_Status), "NMDLLoader failed on the map");
        return false;
    }
    ScanNLIT(fd);
    m_Instances.insert(m_Instances.begin(), std::move(inst));
    snprintf(m_Status, sizeof(m_Status), "Map '%s' loaded (%zu bank textures)",
        m_Instances.front().name.c_str(), m_BankTextures.size());
    FitCamera();
    return true;
}

bool CutsceneScene::AddActor(const std::string& path, const std::string& name_filter) {
    std::vector<uint8_t> fd;
    if (!ReadFileBytes(path, fd)) {
        snprintf(m_Status, sizeof(m_Status), "Failed to read %s", path.c_str());
        return false;
    }
    size_t off = FindNMDL(fd, name_filter);
    if (off == (size_t)-1) {
        snprintf(m_Status, sizeof(m_Status), "No NMDL matching '%s'", name_filter.c_str());
        return false;
    }
    CSSceneInstance inst;
    if (!LoadModelInstance(fd, off, false, inst)) {
        snprintf(m_Status, sizeof(m_Status), "NMDLLoader failed on the actor");
        return false;
    }
    m_Instances.push_back(std::move(inst));
    snprintf(m_Status, sizeof(m_Status), "Actor '%s' added", m_Instances.back().name.c_str());
    if (m_Instances.size() == 1) FitCamera();
    return true;
}

void CutsceneScene::SetStatus(const std::string& msg) {
    snprintf(m_Status, sizeof(m_Status), "%s", msg.c_str());
}

void CutsceneScene::SetLastInstancePose(const glm::vec3& pos, float yaw_deg) {
    for (size_t i = m_Instances.size(); i > 0; i--) {
        CSSceneInstance& inst = m_Instances[i - 1];
        if (inst.is_map) continue;
        inst.position = pos;
        inst.yaw_deg = yaw_deg;
        return;
    }
}

void CutsceneScene::FitCamera() {
    if (m_Instances.empty()) return;
    // Prefer the actors' area; fall back to the map's bbox.
    const CSSceneInstance* ref = nullptr;
    for (auto& i : m_Instances) if (!i.is_map) { ref = &i; break; }
    if (!ref) ref = &m_Instances.front();
    glm::vec3 c = (ref->bbox_min + ref->bbox_max) * 0.5f;
    float ext = glm::length(ref->bbox_max - ref->bbox_min);
    if (ext < 0.01f) ext = 5.0f;
    m_Camera.SetTarget(c);
    m_Camera.SetDistance(ext * 1.2f);
}

void CutsceneScene::EnsureShader() {
    if (m_Shader) return;
    m_Shader = new Shader();
    if (!m_Shader->CompileFromSource(CS_VERT, CS_FRAG))
        std::cerr << "[CutsceneScene] shader compile failed\n";
}

void CutsceneScene::EnsureFBO(int w, int h) {
    if (m_FBO != 0 && m_FBOw == w && m_FBOh == h) return;
    if (m_FBO) {
        glDeleteFramebuffers(1, &m_FBO);
        glDeleteTextures(1, &m_FBOTex);
        glDeleteRenderbuffers(1, &m_FBORbo);
    }
    glGenFramebuffers(1, &m_FBO);
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glGenTextures(1, &m_FBOTex);
    glBindTexture(GL_TEXTURE_2D, m_FBOTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGB, w, h, 0, GL_RGB, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_FBOTex, 0);
    glGenRenderbuffers(1, &m_FBORbo);
    glBindRenderbuffer(GL_RENDERBUFFER, m_FBORbo);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
        GL_RENDERBUFFER, m_FBORbo);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_FBOw = w; m_FBOh = h;
}

void CutsceneScene::DrawInstance(const CSSceneInstance& inst, const glm::mat4& model) {
    m_Shader->SetMat4("model", model);
    // Two passes: opaque meshes first, vertex-alpha overlays after (mirrors Viewport3D).
    for (int pass = 0; pass < 2; ++pass) {
        for (const auto& gpu : inst.meshes) {
            if (!gpu.VAO) continue;
            if (pass == 0 && gpu.has_vertex_alpha) continue;
            if (pass == 1 && !gpu.has_vertex_alpha) continue;
            glBindVertexArray(gpu.VAO);
            if (gpu.has_vertex_alpha) {
                glEnable(GL_BLEND);
                glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                glDepthMask(GL_FALSE);
                m_Shader->SetInt("u_UseVertexAlpha", 1);
            }
            else {
                m_Shader->SetInt("u_UseVertexAlpha", 0);
            }
            if (gpu.draw_sequential) {
                // Point-cloud debug meshes: skip in the scene view.
                continue;
            }
            for (const auto& sec : gpu.sections) {
                if (sec.index_count == 0) continue;
                auto it = inst.mat_to_tex.find(sec.mat_id);
                if (it != inst.mat_to_tex.end() && it->second != 0) {
                    glActiveTexture(GL_TEXTURE0);
                    glBindTexture(GL_TEXTURE_2D, it->second);
                    GLenum wrap = inst.mat_clamp_ids.count(sec.mat_id)
                        ? GL_CLAMP_TO_EDGE : GL_REPEAT;
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
                    m_Shader->SetInt("u_UseTexture", 1);
                    // DXT5 (full alpha) -> blend + low threshold; DXT1 -> punch-through.
                    if (inst.mat_blend_ids.count(sec.mat_id)) {
                        glEnable(GL_BLEND);
                        glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                        glDepthMask(GL_FALSE);
                        m_Shader->SetFloat("u_AlphaThreshold", 0.01f);
                    }
                    else {
                        if (!gpu.has_vertex_alpha) {
                            glDisable(GL_BLEND);
                            glDepthMask(GL_TRUE);
                        }
                        m_Shader->SetFloat("u_AlphaThreshold", 0.5f);
                    }
                }
                else {
                    m_Shader->SetInt("u_UseTexture", 0);
                    if (!gpu.has_vertex_alpha) glDisable(GL_BLEND);
                    m_Shader->SetFloat("u_AlphaThreshold", 0.5f);
                }
                auto ita = inst.mat_to_alpha.find(sec.mat_id);
                if (ita != inst.mat_to_alpha.end() && ita->second != 0) {
                    glActiveTexture(GL_TEXTURE1);
                    glBindTexture(GL_TEXTURE_2D, ita->second);
                    m_Shader->SetInt("u_HasAlphaMask", 1);
                }
                else {
                    m_Shader->SetInt("u_HasAlphaMask", 0);
                }
                glDrawElements(GL_TRIANGLES, (GLsizei)sec.index_count,
                    GL_UNSIGNED_SHORT,
                    (void*)(uintptr_t)(sec.index_start * sizeof(uint16_t)));
            }
        }
    }
    glBindVertexArray(0);
    glDisable(GL_BLEND);
    glDepthMask(GL_TRUE);
}

void CutsceneScene::RenderPanel(const std::string& map_hint, const std::string& texbank_hint) {
    // Guidance from the cutscene's resolved map (event->map index + eboot table)
    if (!map_hint.empty()) {
        if (!texbank_hint.empty())
            ImGui::TextColored({ 0.5f,1.0f,0.6f,1.0f },
                "Scene for this cutscene: map %s.e + %s.p3tex",
                map_hint.c_str(), texbank_hint.c_str());
        else
            ImGui::TextColored({ 0.5f,1.0f,0.6f,1.0f },
                "Scene for this cutscene: map %s.e", map_hint.c_str());
    }

    if (ImGui::Button("Load map (.e)...")) {
        std::string mp = FileDialog::OpenFile("Map E Files\0*.e\0All Files\0*.*\0");
        if (!mp.empty()) {
            std::string tp = FileDialog::OpenFile("P3TEX bank\0*.p3tex\0All Files\0*.*\0");
            if (!tp.empty()) LoadMap(mp, tp);
        }
    }
    ImGui::SameLine();
    if (ImGui::Button("Add actor (.p3obj/.bmd)...")) {
        std::string ap = FileDialog::OpenFile(
            "Actor Files\0*.p3obj;*.bmd\0All Files\0*.*\0");
        if (!ap.empty()) AddActor(ap, m_ActorFilter);
    }
    ImGui::SameLine();
    ImGui::SetNextItemWidth(120);
    ImGui::InputTextWithHint("##aflt", "NMDL filter (e.g. cpn)", m_ActorFilter, sizeof(m_ActorFilter));
    ImGui::SameLine();
    if (ImGui::Button("Clear scene")) Clear();

    if (m_Status[0]) ImGui::TextDisabled("%s", m_Status);

    // Instance list: visibility + transform controls
    for (size_t i = 0; i < m_Instances.size(); i++) {
        CSSceneInstance& inst = m_Instances[i];
        ImGui::PushID((int)i);
        ImGui::Checkbox("##vis", &inst.visible);
        ImGui::SameLine();
        ImGui::Text("%s%s", inst.name.c_str(), inst.is_map ? " (map)" : "");
        if (!inst.is_map) {
            ImGui::SameLine();
            ImGui::SetNextItemWidth(220);
            ImGui::DragFloat3("pos", &inst.position.x, 0.05f);
            ImGui::SameLine();
            ImGui::SetNextItemWidth(80);
            ImGui::DragFloat("yaw", &inst.yaw_deg, 1.0f);
        }
        ImGui::PopID();
    }

    if (m_HasSceneLighting) {
        ImGui::Checkbox("NLIT lighting", &m_EnableLighting);
        if (m_EnableLighting) {
            ImGui::SameLine(); ImGui::SetNextItemWidth(90);
            ImGui::SliderFloat("Pitch##l", &m_DirLightPitch, 0.0f, 90.0f, "%.0f");
            ImGui::SameLine(); ImGui::SetNextItemWidth(90);
            ImGui::SliderFloat("Yaw##l", &m_DirLightYaw, 0.0f, 360.0f, "%.0f");
        }
        ImGui::SameLine();
    }
    ImGui::Checkbox("Flip Y", &m_FlipY);
    if (m_HasSceneCam) {
        ImGui::SameLine();
        ImGui::Checkbox("Scene camera", &m_UseSceneCam);
        if (m_CamPath.size() > 1) {
            ImGui::SameLine();
            if (ImGui::SmallButton(m_CamPlaying ? "Stop##cam" : "Play##cam")) {
                m_CamPlaying = !m_CamPlaying;
                if (m_CamPlaying) {
                    m_CamSeg = 0;
                    m_CamT = 0.0f;
                    m_UseSceneCam = true;
                    m_SceneEye = m_CamPath[0].eye;
                    m_SceneTarget = m_CamPath[0].target;
                }
            }
        }
    }
    // Camera playback: a key flagged move_to is the END pose of a timed move (the
    // 0x0c04 pair); the eboot feeds its duration #4 to the tween scheduler, so the
    // camera SLIDES from the previous pose to it over `dur` seconds - the opening
    // descend. A key not flagged move_to is a hard cut, then a hold. This matches the
    // game: descend (interpolated) then cut between framings.
    if (m_CamPlaying && m_CamPath.size() > 0) {
        m_CamT += ImGui::GetIO().DeltaTime;
        const float hold = 2.0f;
        const CSCamKey& sh = m_CamPath[m_CamSeg];
        if (sh.move_to && m_CamSeg > 0) {
            // interpolate from the previous key to this one over dur seconds
            const CSCamKey& prev = m_CamPath[m_CamSeg - 1];
            float dur = sh.dur > 0.01f ? sh.dur : 1.0f;
            float t = m_CamT / dur;
            if (t > 1.0f) t = 1.0f;
            float s = t * t * (3.0f - 2.0f * t);   // smoothstep ease
            m_SceneEye = prev.eye + (sh.eye - prev.eye) * s;
            m_SceneTarget = prev.target + (sh.target - prev.target) * s;
            if (m_CamT > dur) {
                if (m_CamSeg + 1 < m_CamPath.size()) { m_CamSeg++; m_CamT = 0.0f; }
                else m_CamPlaying = false;
            }
        }
        else {
            // hard cut to this key, then hold
            m_SceneEye = sh.eye;
            m_SceneTarget = sh.target;
            if (m_CamT > hold) {
                if (m_CamSeg + 1 < m_CamPath.size()) { m_CamSeg++; m_CamT = 0.0f; }
                else m_CamPlaying = false;
            }
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("Scene anchor:");
    ImGui::SameLine();
    ImGui::SetNextItemWidth(200);
    ImGui::DragFloat3("##anch", &m_AnchorPos.x, 0.05f);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(70);
    ImGui::DragFloat("yaw##anch", &m_AnchorYaw, 1.0f);

    if (m_Instances.empty()) {
        ImGui::TextDisabled("Load the map and the actors to assemble the scene.");
        return;
    }

    // 3D view (FBO -> ImGui image; orbit camera)
    ImVec2 vp_pos = ImGui::GetCursorScreenPos();
    ImVec2 vp_size = ImGui::GetContentRegionAvail();
    int w = (int)vp_size.x, h = (int)vp_size.y;
    if (w <= 0 || h <= 0) return;
    EnsureShader();
    EnsureFBO(w, h);

    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glViewport(0, 0, w, h);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.12f, 0.13f, 0.16f, 1.0f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    m_Shader->Use();
    if (m_HasSceneCam && m_UseSceneCam) {
        // The eye/target are in the scene-LOCAL frame (same frame as the actors'
        // positions), exactly as captured from the game's camera struct. The non-map
        // actors are drawn as  flip * anchor * T(local).  For the camera to see them
        // correctly it must UNDO that same transform:
        //   view = lookAt(eye_local, target_local) * inv(anchor) * inv(flip)
        // (inv(flip) == flip since flip^2 == I). This keeps eye/target LOCAL (no
        // double anchor) and shares the exact flip+anchor the actors use - fixing
        // both the wrong aim and the mismatch.
        glm::mat4 flipM = m_FlipY
            ? glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, -1.0f, 1.0f))
            : glm::mat4(1.0f);
        glm::mat4 anchorM = glm::translate(glm::mat4(1.0f), m_AnchorPos);
        anchorM = glm::rotate(anchorM, glm::radians(m_AnchorYaw), glm::vec3(0, 1, 0));
        glm::mat4 view = glm::lookAt(m_SceneEye, m_SceneTarget, glm::vec3(0, 1, 0))
            * glm::inverse(flipM * anchorM);
        m_Shader->SetMat4("view", view);
        // FOV from the camera struct: +0x60 = tan(fovV/2) = 0.3764 -> ~41.3 deg
        // vertical. near 0.1, far 4000 confirmed against the struct (+0x80/+0x84).
        m_Shader->SetMat4("projection",
            glm::perspective(2.0f * atanf(0.3764f), (float)w / (float)h, 0.1f, 4000.0f));
    }
    else {
        m_Shader->SetMat4("view", m_Camera.GetViewMatrix());
        m_Shader->SetMat4("projection", m_Camera.GetProjectionMatrix((float)w / (float)h));
    }
    m_Shader->SetVec3("objectColor", glm::vec3(0.72f, 0.72f, 0.88f));
    m_Shader->SetInt("u_Diffuse", 0);
    m_Shader->SetInt("u_AlphaMask", 1);
    m_Shader->SetInt("u_HasAlphaMask", 0);
    m_Shader->SetFloat("u_AlphaThreshold", 0.5f);
    m_Shader->SetInt("u_EnableLighting", (m_EnableLighting && m_HasSceneLighting) ? 1 : 0);
    {
        float pitch = glm::radians(m_DirLightPitch);
        float yaw = glm::radians(m_DirLightYaw);
        glm::vec3 dir(std::cos(pitch) * std::sin(yaw), std::sin(pitch),
            std::cos(pitch) * std::cos(yaw));
        m_Shader->SetVec3("u_DirLightDir", glm::normalize(dir));
        m_Shader->SetVec3("u_AmbientColor", m_AmbientColor);
        m_Shader->SetVec3("u_DirLightColor", m_DirLightColor);
    }

    glm::mat4 flip = m_FlipY
        ? glm::scale(glm::mat4(1.0f), glm::vec3(1.0f, -1.0f, 1.0f))
        : glm::mat4(1.0f);
    glm::mat4 anchor = glm::translate(glm::mat4(1.0f), m_AnchorPos);
    anchor = glm::rotate(anchor, glm::radians(m_AnchorYaw), glm::vec3(0, 1, 0));
    for (const auto& inst : m_Instances) {
        if (!inst.visible) continue;
        glm::mat4 model = flip;
        if (!inst.is_map) model = model * anchor;  // scene-local frame -> map
        model = glm::translate(model, inst.position);
        model = glm::rotate(model, glm::radians(inst.yaw_deg), glm::vec3(0, 1, 0));
        model = glm::scale(model, glm::vec3(inst.scale));
        DrawInstance(inst, model);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    ImGui::SetCursorScreenPos(vp_pos);
    ImGui::InvisibleButton("##csvp", vp_size,
        ImGuiButtonFlags_MouseButtonLeft |
        ImGuiButtonFlags_MouseButtonRight |
        ImGuiButtonFlags_MouseButtonMiddle);
    const bool hovered = ImGui::IsItemHovered();
    const bool active = ImGui::IsItemActive();
    ImGui::GetWindowDrawList()->AddImage(
        (ImTextureID)(intptr_t)m_FBOTex, vp_pos,
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
}