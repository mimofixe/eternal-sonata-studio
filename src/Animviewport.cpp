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
#include <cstdio>
#include <algorithm>
#include <cfloat>
#include <set>
#include <map>
#include <functional>

// Skinning vertex shader that also forwards the UV so the mesh can be textured.
static const char* SKIN_VERT = R"(
#version 450 core
layout(location=0) in vec3  aPos;
layout(location=1) in vec3  aNormal;
layout(location=2) in vec2  aUV;
layout(location=3) in vec4  aBoneWeights;
layout(location=4) in uvec4 aBoneIds;
out vec3 Normal;
out vec2 TexCoord;
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
    Normal      = normalize(nm * aNormal);
    TexCoord    = aUV;
    // Cloth-vs-leg vertex clamp (skirt only): ABSOLUTE per-capsule rule. A cloth
    // vertex may never sit inside a leg capsule (per-segment radius: thigh/shin).
    // The authored run animation itself places skirt vertices inside the leg
    // (measured 0.001 from the axis); the real game visibly pushes the hem out
    // past the authored pose, so the rule must be absolute, not relative to the
    // animation. Covers the mesh BETWEEN cloth bones, which bone-level collision
    // cannot reach. Two iterations resolve most capsule-vs-capsule interplay;
    // vertices pinched between crossing thighs end on the nearest leg's surface
    // (same as the engine: the cloth cannot be outside both legs at once).
    gl_Position = u_Proj * u_View * skinPos;
}
)";

// Textured fragment shader. Falls back to flat colour when the section has no
// texture or when texturing is toggled off.
static const char* SKIN_FRAG = R"(
#version 450 core
in  vec3 Normal;
in  vec2 TexCoord;
out vec4 FragColor;
uniform sampler2D u_Diffuse;
uniform int       u_UseTexture;
uniform float     u_AlphaThreshold;
uniform vec3      u_LightDir;
uniform vec3      u_MeshColor;
uniform int       u_EnableLighting;
void main() {
    vec4 tex = vec4(1.0);
    if (u_UseTexture != 0) {
        tex = texture(u_Diffuse, TexCoord);
        if (tex.a < u_AlphaThreshold) discard;
    }
    vec3  base  = (u_UseTexture != 0) ? tex.rgb : u_MeshColor;
    float alpha = (u_UseTexture != 0) ? tex.a   : 1.0;
    // Default is unlit: show the texture flat, like the static Viewport3D.
    // Lighting is opt-in (the N.L term is what darkens faces turned from the light).
    if (u_EnableLighting != 0) {
        vec3 N = normalize(Normal);
        if (!gl_FrontFacing) N = -N;
        float diff = max(dot(N, u_LightDir), 0.0);
        base *= (0.30 + 0.70 * diff);
    }
    FragColor = vec4(base, alpha);
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

// Euler XYZ -> rotation matrix (Rz*Ry*Rx), same convention as NBN2Parser.
static glm::mat3 AVEulerXYZ(float rx, float ry, float rz) {
    float cx = std::cos(rx), sx = std::sin(rx);
    float cy = std::cos(ry), sy = std::sin(ry);
    float cz = std::cos(rz), sz = std::sin(rz);
    glm::mat3 Rx(1.f, 0.f, 0.f, 0.f, cx, sx, 0.f, -sx, cx);
    glm::mat3 Ry(cy, 0.f, -sy, 0.f, 1.f, 0.f, sy, 0.f, cy);
    glm::mat3 Rz(cz, sz, 0.f, -sz, cz, 0.f, 0.f, 0.f, 1.f);
    return Rz * Ry * Rx;
}

// big-endian readers for the cloth chunks (these formats are stored BE)
static inline uint16_t AVbeU16(const uint8_t* d) { return (uint16_t)((d[0] << 8) | d[1]); }
static inline int16_t  AVbeI16(const uint8_t* d) { return (int16_t)AVbeU16(d); }
static inline uint32_t AVbeU32(const uint8_t* d) {
    return ((uint32_t)d[0] << 24) | ((uint32_t)d[1] << 16) | ((uint32_t)d[2] << 8) | d[3];
}
static inline float AVbeF32(const uint8_t* d) {
    uint32_t u = AVbeU32(d); float f; std::memcpy(&f, &u, 4); return f;
}

// minimal rotation taking direction a onto direction b
static glm::mat3 AVrotBetween(glm::vec3 a, glm::vec3 b) {
    float la = glm::length(a), lb = glm::length(b);
    if (la < 1e-6f || lb < 1e-6f) return glm::mat3(1.f);
    a /= la; b /= lb;
    const glm::vec3 ax = glm::cross(a, b);
    const float s = glm::length(ax), d = glm::dot(a, b);
    if (s < 1e-7f) {
        if (d > 0.f) return glm::mat3(1.f);
        glm::vec3 p = glm::cross(a, glm::vec3(1, 0, 0));
        if (glm::length(p) < 1e-4f) p = glm::cross(a, glm::vec3(0, 1, 0));
        return glm::mat3(glm::rotate(glm::mat4(1.f), 3.14159265f, glm::normalize(p)));
    }
    return glm::mat3(glm::rotate(glm::mat4(1.f), std::atan2(s, d), ax / s));
}

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

SkeletalModel* AnimViewport::Active() {
    return (m_ModelIdx >= 0 && m_ModelIdx < (int)m_Models.size())
        ? &m_Models[m_ModelIdx] : nullptr;
}
const SkeletalModel* AnimViewport::Active() const {
    return (m_ModelIdx >= 0 && m_ModelIdx < (int)m_Models.size())
        ? &m_Models[m_ModelIdx] : nullptr;
}

void AnimViewport::CleanupModel(SkeletalModel& m) {
    for (auto& msh : m.meshes) {
        if (msh.EBO) glDeleteBuffers(1, &msh.EBO);
        if (msh.VBO) glDeleteBuffers(1, &msh.VBO);
        if (msh.VAO) glDeleteVertexArrays(1, &msh.VAO);
    }
    m.meshes.clear();
    for (GLuint t : m.ownedTextures) if (t) glDeleteTextures(1, &t);
    m.ownedTextures.clear();
    m.matToTex.clear();
    m.matToAlpha.clear();
    m.matBlend.clear();
    m.matClamp.clear();
    if (m.lineVBO) { glDeleteBuffers(1, &m.lineVBO); m.lineVBO = 0; }
    if (m.lineVAO) { glDeleteVertexArrays(1, &m.lineVAO); m.lineVAO = 0; }
    m.lineVerts = 0;
}

void AnimViewport::Clear() {
    for (auto& m : m_Models) CleanupModel(m);
    m_Models.clear();
    m_ModelIdx = 0;
    m_HasContent = false;
}

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
    out.sections = mesh.faceSections;

}

// Build one SkeletalModel per NBN2-bearing NMDL. The model's meshes and textures
// come from NMDLLoader (so they arrive textured); animations are the trailing
// NMTN block. Falls back to a single whole-file group when no NBN2 sits inside
// an NMDL (plain single-character .e).
void AnimViewport::BuildGroups(const std::vector<uint8_t>& fdv,
    const std::vector<Chunk>& chunks) {
    const uint8_t* fd = fdv.data();
    const size_t   fsz = fdv.size();

    struct Range { size_t start, end; std::string name; };
    std::vector<Range>        nmdl;
    std::vector<size_t>       nobj;
    std::vector<const Chunk*> nbn2, nmtn;
    for (const auto& c : chunks) {
        switch (c.type) {
        case ChunkType::NMDL: nmdl.push_back({ c.offset, c.offset + c.size, c.name }); break;
        case ChunkType::NOBJ: nobj.push_back(c.offset); break;
        case ChunkType::NBN2: nbn2.push_back(&c); break;
        case ChunkType::NMTN: nmtn.push_back(&c); break;
        default: break;
        }
    }
    std::sort(nobj.begin(), nobj.end());

    auto add_anim = [&](SkeletalModel& m, const Chunk* a) {
        if (a->offset + a->size > fsz) return;
        NMTNAnimation an;
        if (NMTNParser::Parse(fd + a->offset, a->size, an) && an.frame_count > 0) {
            if (an.name.empty()) an.name = "anim_" + std::to_string(m.anims.size());
            m.anims.push_back(std::move(an));
        }
        };

    // load the textured meshes of the NMDL at nmdl_offset into the model
    auto load_meshes = [&](SkeletalModel& m, size_t nmdl_offset) {
        NMDLModel model;
        if (!NMDLLoader::Load(fd, fsz, nmdl_offset, model)) return;
        for (const NSHPMesh& mesh : model.meshes) {
            if (!mesh.has_bones || mesh.vertices.empty() || mesh.boneIDList.empty())
                continue;
            SkinMeshGPU gpu;
            gpu.name = mesh.name;
            UploadSkinMesh(mesh, gpu);
            if (gpu.VAO) m.meshes.push_back(std::move(gpu));
        }
        // take ownership of the decoded textures and the material maps
        m.matToTex = std::move(model.mat_to_tex);
        m.matToAlpha = std::move(model.mat_to_alpha);
        m.matBlend = std::move(model.mat_blend_ids);
        m.matClamp = std::move(model.mat_clamp_ids);
        m.ownedTextures = std::move(model.owned_textures);
        model.owned_textures.clear();   // prevent NMDLModel::Destroy double-free
        };

    auto finalize = [&](SkeletalModel& m) {
        if (m.bind.empty()) {   // animation-only model (NMTN bank, no NBN2): no rig to fit
            m.fitCenter = glm::vec3(0.f); m.fitDist = 2.f; m.anim = m.bind;
            RebuildSkeletonLines(m); return;
        }
        glm::vec3 mn(FLT_MAX), mx(-FLT_MAX);
        for (const auto& b : m.bind) { mn = glm::min(mn, b.world_pos); mx = glm::max(mx, b.world_pos); }
        m.fitCenter = (mn + mx) * 0.5f;
        float ext = glm::length(mx - mn);
        if (ext < 0.1f) ext = 2.f;
        m.fitDist = ext * 0.8f;
        m.anim = m.bind;
        if (!m.anims.empty()) ApplyPose(m, 0.f);
        if (m_Cloth && m.hasCloth) { ResetDynamics(m); SettleDynamics(m, 24); }
        RebuildSkeletonLines(m);
        };

    for (const Chunk* sk : nbn2) {
        const Range* host = nullptr;
        for (const auto& r : nmdl)
            if (r.start <= sk->offset && sk->offset < r.end) { host = &r; break; }
        if (!host || sk->offset + sk->size > fsz) continue;

        SkeletalModel m;
        m.name = host->name.empty() ? "model" : host->name;
        if (!NBN2Parser::Parse(fd + sk->offset, sk->size, m.bind) || m.bind.empty())
            continue;
        for (int i = 0; i < (int)m.bind.size(); ++i) m.boneIndex[m.bind[i].name] = i;
        BuildInvBind(m);

        load_meshes(m, host->start);

        size_t nxt = fsz;
        for (size_t o : nobj) if (o >= host->end) { nxt = o; break; }
        for (const Chunk* a : nmtn)
            if (a->offset >= host->end && a->offset < nxt) add_anim(m, a);

        ParseDynamics(m, fdv, host->start, host->end);
        finalize(m);
        m_Models.push_back(std::move(m));
    }

    // fallback: NBN2 not inside any NMDL -> single whole-file model
    if (m_Models.empty() && !nbn2.empty()) {
        const Chunk* sk = nbn2.front();
        if (sk->offset + sk->size <= fsz) {
            SkeletalModel m;
            m.name = "model";
            if (NBN2Parser::Parse(fd + sk->offset, sk->size, m.bind) && !m.bind.empty()) {
                for (int i = 0; i < (int)m.bind.size(); ++i) m.boneIndex[m.bind[i].name] = i;
                BuildInvBind(m);
                if (!nmdl.empty()) load_meshes(m, nmdl.front().start);
                for (const Chunk* a : nmtn) add_anim(m, a);
                ParseDynamics(m, fdv, 0, fsz);
                finalize(m);
                m_Models.push_back(std::move(m));
            }
        }
    }

    // fallback: an NMTN animation bank with no NBN2 (e.g. appkeep.bmd field-event
    // motions for ALG/BET/PLK). No skeleton ships in the file, so there is no rig to
    // pose, but parse the motions anyway so they are listed instead of silently
    // dropped. They can later be applied to a separately-loaded character skeleton.
    if (m_Models.empty() && !nmtn.empty()) {
        SkeletalModel m;
        m.name = "animations (no skeleton)";
        for (const Chunk* a : nmtn) add_anim(m, a);
        if (!m.anims.empty()) { finalize(m); m_Models.push_back(std::move(m)); }
    }
}

void AnimViewport::LoadFromChunks(const std::vector<uint8_t>& file_data,
    const std::vector<Chunk>& chunks) {
    Clear();
    if (file_data.empty()) return;
    BuildGroups(file_data, chunks);
    if (m_Models.empty()) return;
    m_HasContent = true;
    SelectModel(0);
    std::cout << "[AnimViewport] " << m_Models.size() << " skinned model(s):\n";
    for (const auto& m : m_Models)
        std::cout << "   - " << m.name << ": " << m.bind.size() << " bones, "
        << m.meshes.size() << " meshes, " << m.anims.size() << " anims, "
        << m.matToTex.size() << " textured mats\n";
}

void AnimViewport::SelectModel(int idx) {
    if (idx < 0 || idx >= (int)m_Models.size()) return;
    m_ModelIdx = idx;
    SkeletalModel& m = m_Models[idx];
    m_Camera.SetTarget(m.fitCenter);
    m_Camera.SetDistance(m.fitDist);
    if (!m.anims.empty()) ApplyPose(m, m.frame);
    if (m_Cloth && m.hasCloth) { ResetDynamics(m); SettleDynamics(m, 24); }
    RebuildSkeletonLines(m);
}

glm::mat4 AnimViewport::WorldMat(const Bone& b) const {
    glm::mat4 m(b.world_rot);
    m[3] = glm::vec4(b.world_pos, 1.f);
    return m;
}

void AnimViewport::BuildInvBind(SkeletalModel& m) {
    m.invBind.resize(m.bind.size());
    for (size_t i = 0; i < m.bind.size(); i++)
        m.invBind[i] = glm::inverse(WorldMat(m.bind[i]));
}

void AnimViewport::ApplyPose(SkeletalModel& m, float frame) {
    if (m.anims.empty()) return;
    const NMTNAnimation& anim = m.anims[m.animIdx];
    m.anim = m.bind;
    const size_t N = m.anim.size();

    std::vector<glm::vec3> local_pos(N);
    std::vector<glm::mat3> local_rot(N);
    for (size_t i = 0; i < N; i++) {
        const Bone& b = m.anim[i];
        local_pos[i] = glm::vec3(b.local_x, b.local_y, b.local_z);
        glm::mat3 r1 = AVEulerXYZ(b.rot_x, b.rot_y, b.rot_z);
        glm::mat3 r2 = AVEulerXYZ(b.rot2_x, b.rot2_y, b.rot2_z);
        local_rot[i] = r1 * r2;
    }

    for (const auto& track : anim.tracks) {
        auto it = m.boneIndex.find(track.bone_name);
        if (it == m.boneIndex.end()) continue;
        const int idx = it->second;
        const Bone& b = m.anim[idx];

        const int pos_slot[3] = { 0, 1, 2 };
        float P[3] = { b.local_x, b.local_y, b.local_z };
        bool pos_touched = false;
        for (int c = 0; c < 3; ++c) {
            int s = pos_slot[c];
            if (track.HasChannel(s)) { P[c] = track.channel[s].Sample(frame); pos_touched = true; }
            else if (track.HasStatic(s)) { P[c] = track.static_val[s]; pos_touched = true; }
        }
        if (pos_touched) local_pos[idx] = glm::vec3(P[0], P[1], P[2]);

        float R[3] = { b.rot2_x, b.rot2_y, b.rot2_z };
        const int rot_slot[3] = { 3, 4, 5 };
        for (int c = 0; c < 3; ++c) {
            int s = rot_slot[c];
            if (track.HasChannel(s))      R[c] = track.channel[s].Sample(frame);
            else if (track.HasStatic(s))  R[c] = track.static_val[s];
        }
        glm::mat3 bindR = AVEulerXYZ(b.rot_x, b.rot_y, b.rot_z);
        glm::mat3 animR = AVEulerXYZ(R[0], R[1], R[2]);
        local_rot[idx] = bindR * animR;
    }

    for (size_t i = 0; i < N; i++) {
        Bone& b = m.anim[i];
        const int par = b.parent_idx;
        if (par < 0 || par >= (int)N) {
            b.world_pos = local_pos[i];
            b.world_rot = local_rot[i];
        }
        else {
            const Bone& p = m.anim[par];
            b.world_pos = p.world_rot * local_pos[i] + p.world_pos;
            b.world_rot = p.world_rot * local_rot[i];
        }
    }

    // Snapshot the rigid (animation-only) pose. Taken HERE (not in
    // ApplyClothPose) so it refreshes on EVERY path that re-poses the model:
    // playback ticks, the frame-slider scrub while paused, and load. The cloth
    // overlay and the shader leg clamp both rely on this being current; a stale
    // snapshot leaves the leg-clamp capsules at an old pose (invisible failure).
    m.rigidAnim = m.anim;
}

void AnimViewport::RebuildSkeletonLines(SkeletalModel& m) {
    if (m.lineVBO) { glDeleteBuffers(1, &m.lineVBO); m.lineVBO = 0; }
    if (m.lineVAO) { glDeleteVertexArrays(1, &m.lineVAO); m.lineVAO = 0; }
    m.lineVerts = 0;
    const auto& bones = m.anim.empty() ? m.bind : m.anim;
    if (bones.empty()) return;
    std::vector<glm::vec3> lines;
    lines.reserve(bones.size() * 2);
    for (const auto& b : bones)
        if (b.parent_idx >= 0 && b.parent_idx < (int)bones.size()) {
            lines.push_back(bones[b.parent_idx].world_pos);
            lines.push_back(b.world_pos);
        }
    if (lines.empty()) return;
    glGenVertexArrays(1, &m.lineVAO);
    glGenBuffers(1, &m.lineVBO);
    glBindVertexArray(m.lineVAO);
    glBindBuffer(GL_ARRAY_BUFFER, m.lineVBO);
    glBufferData(GL_ARRAY_BUFFER, lines.size() * sizeof(glm::vec3),
        lines.data(), GL_DYNAMIC_DRAW);
    glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(glm::vec3), nullptr);
    glEnableVertexAttribArray(0);
    glBindVertexArray(0);
    m.lineVerts = (int)lines.size();
}

// ---------------------------------------------------------------------------
// Cloth dynamics (Dynabone) - see the format notes in Animviewport.h.
//
// Every solved bone ends up with   pos = solved node position   and its rotation
// is rebuilt from where its chain child landed, so the skin matrices always
// describe a rigid FK chain: the mesh bends at the joints exactly like under
// animation and cannot tear.
// ---------------------------------------------------------------------------

namespace {
    constexpr float  kClothStep = 1.f / 60.f;   // one cloth frame (game: 1/60 or 1/fps)
    constexpr float  kKeepOut = 0.001f;         // response margin, mask groups (0x82136b08)
    constexpr float  kKeepIn = -0.002f;         // response margin, keep-in ids (0x82137510)
    constexpr int    kKeepInGroup = 29;         // NCLS group 0x1c (+1) holds the keep-in volumes
    constexpr int    kMaxBodies = 17;           // root + 16 point masses
    constexpr double kDragCoef = 0.0375;        // 1.2 * 3 / (400 * 0.03 * 8) per unit of NDYN +0x0C
}

// Parse NBN2 topology + NDYN + NCLS for this model's region.
void AnimViewport::ParseDynamics(SkeletalModel& m, const std::vector<uint8_t>& fd,
    size_t begin, size_t end) {
    const size_t N = m.bind.size();
    m.cloth.assign(N, ClothBone{});
    m.clothPos.assign(N, glm::vec3(0.f));
    m.clothDelta.assign(N, glm::mat3(1.f));
    m.colliders.clear();
    m.clothGroups.assign(64, { 0, 0 });
    m.chains.clear();
    m.sheets.clear();
    m.clothOrder.clear();
    m.hasCloth = false;
    m.clothInit = false;
    if (N == 0 || end > fd.size() || begin + 8 > end) return;
    const uint8_t* d = fd.data();

    // --- topology ---------------------------------------------------------
    auto dynFlag = [&](int i) { return m.bind[i].flags != 96 && m.bind[i].flags != 224; };
    std::vector<std::vector<int>> kids(N);
    std::vector<int> roots;
    size_t nDyn = 0;
    for (size_t i = 0; i < N; ++i) {
        const int par = m.bind[i].parent_idx;
        if (par >= 0 && par < (int)N) kids[par].push_back((int)i);
        else roots.push_back((int)i);
    }
    // topological order (parents first), children by index
    for (int r : roots) {
        std::vector<int> stack(1, r);
        while (!stack.empty()) {
            const int b = stack.back(); stack.pop_back();
            m.clothOrder.push_back(b);
            for (auto it = kids[b].rbegin(); it != kids[b].rend(); ++it) stack.push_back(*it);
        }
    }
    for (int i : m.clothOrder) {
        ClothBone& cb = m.cloth[i];
        const int par = m.bind[i].parent_idx;
        cb.isDyn = dynFlag(i);
        nDyn += cb.isDyn;
        if (!cb.isDyn && par >= 0 && par < (int)N)
            cb.follower = m.cloth[par].isDyn || m.cloth[par].follower;
    }
    if (nDyn == 0) return;

    // --- locate the first NDYN and NCLS of the region ----------------------
    size_t ndyn = 0, ndynEnd = 0, ncls = 0, nclsEnd = 0;
    for (size_t p = begin; p + 8 <= end; ++p) {
        if (!ndyn && !std::memcmp(d + p, "NDYN", 4)) {
            ndyn = p + 8; ndynEnd = std::min(p + (size_t)AVbeU32(d + p + 4), end);
        }
        else if (!ncls && !std::memcmp(d + p, "NCLS", 4)) {
            ncls = p + 8; nclsEnd = std::min(p + (size_t)AVbeU32(d + p + 4), end);
        }
        if (ndyn && ncls) break;
    }

    // --- NCLS: every group, flattened; group -> [first,last) ----------------
    if (ncls && ncls + 0x84 <= nclsEnd) {
        const uint16_t count = AVbeU16(d + ncls + 0x82);
        uint16_t prev = 0;
        for (int g = 0; g < 64; ++g) {
            const uint16_t cum = AVbeU16(d + ncls + 2 + 2 * g);
            if (cum <= prev || cum > count) continue;
            const uint16_t first = (uint16_t)m.colliders.size();
            for (uint16_t k = prev; k < cum; ++k) {
                const size_t r = ncls + 0x84 + (size_t)k * 48;
                if (r + 48 > nclsEnd) break;
                const uint16_t bone = AVbeU16(d + r + 0x02);
                if (bone >= N) continue;   // 0xFFFF entries are not body-attached
                ClothSphere s;
                s.type = d[r + 0x00];
                s.id = d[r + 0x01];
                s.bone = bone;
                s.loff = glm::vec3(AVbeI16(d + r + 0x04), AVbeI16(d + r + 0x06), AVbeI16(d + r + 0x08)) / 64.f;
                s.r = AVbeF32(d + r + 0x10);
                m.colliders.push_back(s);
            }
            m.clothGroups[g] = { first, (uint16_t)m.colliders.size() };
            prev = cum;
        }
    }

    // --- NDYN: per-bone records ---------------------------------------------
    int matched = 0;
    if (ndyn) {
        for (size_t i = 0; i < N; ++i) {
            ClothBone& cb = m.cloth[i];
            if (!cb.isDyn) continue;
            const size_t r = ndyn + (size_t)m.bind[i].ndyn_index * 48;
            if (r + 48 > ndynEnd || (AVbeU32(d + r) & 0xFFFF) != i) continue;
            ++matched;
            cb.type = AVbeU16(d + r + 0x00);
            cb.mask2 = AVbeU16(d + r + 0x04);
            cb.mask1 = AVbeU16(d + r + 0x06);
            const float mass = AVbeF32(d + r + 0x08);
            if (mass > 1e-6f && mass < 100.f) cb.mass = mass;
            cb.drag = glm::max(AVbeF32(d + r + 0x0C), 0.f);
            cb.ks = glm::max(AVbeF32(d + r + 0x10), 0.f);
            cb.kd = glm::max(AVbeF32(d + r + 0x14), 0.f);
            cb.sheetThr = AVbeU16(d + r + 0x28) / 64.f;
            const uint32_t lk = AVbeU32(d + r + 0x18);
            const size_t linkBase = ndyn + nDyn * 48;
            for (uint32_t k = 0; k < (lk >> 24); ++k) {
                const size_t e = linkBase + (size_t)((lk & 0xFFFFFF) + k) * 32;
                if (e + 32 > ndynEnd) break;          // variant chunks without the table
                const uint32_t o = AVbeU32(d + e);
                if (o < N) cb.links.push_back({ (int)o, AVbeF32(d + e + 4), AVbeF32(d + e + 8) });
            }
            const uint8_t cid = d[r + 0x20];
            if (cid) {
                const auto range = m.clothGroups[kKeepInGroup];
                for (uint16_t k = range.first; k < range.second; ++k)
                    if (m.colliders[k].id == cid && m.colliders[k].type == 0) cb.keepIn.push_back(k);
            }
        }
    }

    // --- chains --------------------------------------------------------------
    // root: dynamic bone with flag 0x2 (or a dynamic bone under a rigid parent);
    // next: first dynamic child without 0x2, or one that also ends (0x8) the chain.
    int keepIn = 0, bodies = 0;
    for (int i : m.clothOrder) {
        const ClothBone& cb = m.cloth[i];
        if (!cb.isDyn) continue;
        const int par = m.bind[i].parent_idx;
        const bool parDyn = par >= 0 && par < (int)N && m.cloth[par].isDyn;
        if (!(m.bind[i].flags & 0x2) && parDyn) continue;
        ClothChain c;
        c.bones.push_back(i);
        for (int b = i; (int)c.bones.size() < kMaxBodies; ) {
            if (b != i && (m.bind[b].flags & 0x8)) break;
            int next = -1;
            for (int k : kids[b]) {
                const uint16_t f = m.bind[k].flags;
                if (m.cloth[k].isDyn && (!(f & 0x2) || (f & 0x8))) { next = k; break; }
            }
            if (next < 0) break;
            c.bones.push_back(next);
            b = next;
        }
        if (c.bones.size() < 2) continue;
        const size_t n = c.bones.size();
        c.m.resize(n); c.ks.assign(n, 0.f); c.kd.assign(n, 0.f);
        for (size_t j = 0; j < n; ++j) {
            c.m[j] = m.cloth[c.bones[j]].mass;
            if (j) {
                c.ks[j] = m.cloth[c.bones[j - 1]].ks;
                c.kd[j] = m.cloth[c.bones[j - 1]].kd;
                c.spring |= c.ks[j] > 0.f;
                keepIn += !m.cloth[c.bones[j]].keepIn.empty();
            }
        }
        c.drag = cb.drag;
        c.substeps = (cb.type & 1) ? 10 : 8;
        m.cloth[i].chain = (int)m.chains.size();
        bodies += (int)n - 1;
        m.chains.push_back(std::move(c));
    }
    // --- links and sheets (0x8213c7f8 / 0x8213eee8) ----------------------------
    std::vector<std::pair<int, int>> where(N, { -1, -1 });   // bone -> (chain, node>0)
    for (int ci = 0; ci < (int)m.chains.size(); ++ci)
        for (int j = 1; j < (int)m.chains[ci].bones.size(); ++j)
            where[m.chains[ci].bones[j]] = { ci, j };
    std::vector<std::set<int>> adj(m.chains.size());
    for (int ci = 0; ci < (int)m.chains.size(); ++ci) {
        ClothChain& c = m.chains[ci];
        c.links.assign(c.bones.size(), {});
        for (int j = 1; j < (int)c.bones.size(); ++j) {
            const ClothBone& cb = m.cloth[c.bones[j]];
            if (!cb.links.empty()) c.sheetThr = cb.sheetThr;
            for (const auto& L : cb.links) {
                const auto t = where[L.bone];
                if (t.first < 0 || t.first == ci) continue;
                ClothLink l; l.chain = t.first; l.node = t.second; l.weight = L.weight; l.tol = L.tol;
                c.links[j].push_back(l);
                adj[ci].insert(t.first); adj[t.first].insert(ci);
            }
        }
    }
    std::vector<char> seen(m.chains.size(), 0);
    for (int pass = 0; pass < 2; ++pass)                     // open paths first, then rings
        for (int ci = 0; ci < (int)m.chains.size(); ++ci) {
            if (seen[ci] || adj[ci].empty() || (pass == 0 && adj[ci].size() != 1)) continue;
            ClothSheet sh;
            for (int cur = ci; cur >= 0 && sh.chains.size() < 12; ) {
                seen[cur] = 1; sh.chains.push_back(cur);
                int nx = -1;
                for (int k : adj[cur]) if (!seen[k]) { nx = k; break; }
                cur = nx;
            }
            sh.first = 1 << 30; sh.last = 0;
            for (int k : sh.chains)
                for (int j = 1; j < (int)m.chains[k].links.size(); ++j)
                    if (!m.chains[k].links[j].empty()) { sh.first = std::min(sh.first, j); sh.last = std::max(sh.last, j); }
            if (sh.chains.size() > 1 && sh.last >= sh.first && m.sheets.size() < 8) m.sheets.push_back(sh);
        }

    std::cout << "[Cloth] " << nDyn << " dynamic bones, " << matched << " NDYN records, "
        << m.chains.size() << " chains (" << bodies << " bodies), " << m.sheets.size() << " sheets, "
        << m.colliders.size() << " NCLS records, " << keepIn << " keep-in nodes\n";
    m.hasCloth = !m.chains.empty();
}

void AnimViewport::ResetDynamics(SkeletalModel& m) {
    m.clothInit = false;
    for (ClothChain& c : m.chains) {
        c.init = false;
        for (auto& node : c.links) for (ClothLink& l : node) l.rest = -1.f;
    }
}

namespace {
    using dvec3 = glm::dvec3;

    // Rod tensions + body accelerations (0x8215ead8/0x821605a0). Body 0 is
    // kinematic. Fsys is what the tension system sees: the game feeds the spring
    // and external forces into it with a flipped sign (verified in the XEX).
    void AVClothAccel(const std::vector<dvec3>& x, const std::vector<double>& m,
        const std::vector<dvec3>& F, const std::vector<dvec3>& Fsys, std::vector<dvec3>& a) {
        const int n = (int)x.size(), N = n - 1;
        double A[kMaxBodies][kMaxBodies + 1] = {};
        dvec3 u[kMaxBodies];
        for (int i = 0; i < N; ++i) {
            const dvec3 dd = x[i + 1] - x[i];
            const double L = glm::length(dd);
            u[i] = L > 1e-12 ? dd / L : dvec3(0, -1, 0);
        }
        for (int i = 0; i < N; ++i) {
            const int j1 = i + 1;
            A[i][i] += -1.0 / m[j1];
            if (j1 < N) A[i][j1] += glm::dot(u[i], u[j1]) / m[j1];
            double rhs = -glm::dot(u[i], Fsys[j1] / m[j1]);
            if (i >= 1) {
                A[i][i] += -1.0 / m[i];
                A[i][i - 1] += glm::dot(u[i], u[i - 1]) / m[i];
                rhs += glm::dot(u[i], Fsys[i] / m[i]);
            }
            A[i][N] = rhs;
        }
        // Gauss-Jordan with partial pivoting
        for (int c = 0; c < N; ++c) {
            int p = c;
            for (int r = c + 1; r < N; ++r) if (std::abs(A[r][c]) > std::abs(A[p][c])) p = r;
            if (std::abs(A[p][c]) < 1e-10) continue;
            if (p != c) for (int k = 0; k <= N; ++k) std::swap(A[p][k], A[c][k]);
            const double inv = 1.0 / A[c][c];
            for (int k = c; k <= N; ++k) A[c][k] *= inv;
            for (int r = 0; r < N; ++r) {
                if (r == c || A[r][c] == 0.0) continue;
                const double f = A[r][c];
                for (int k = c; k <= N; ++k) A[r][k] -= f * A[c][k];
            }
        }
        a.assign(n, dvec3(0.0));
        for (int j = 1; j < n; ++j) {
            a[j] = F[j] / m[j] - A[j - 1][N] * u[j - 1] / m[j];
            if (j < N) a[j] += A[j][N] * u[j] / m[j];
        }
    }

    // world center of an NCLS sphere on the current (solved) pose
    glm::vec3 AVColliderCenter(const SkeletalModel& m, const std::vector<Bone>& R, const ClothSphere& s) {
        const int b = s.bone;
        return m.clothPos[b] + m.clothDelta[b] * (R[b].world_rot * s.loff);
    }
}

// One cloth frame of one chain (0x821602b0 pre-pass + 0x8215e8a8 substeps).
// Targets and the root drive are already in c (set by StepDynamics).
void AnimViewport::SolveChain(SkeletalModel& m, ClothChain& c, float dtf) {
    (void)m;
    const int n = (int)c.bones.size();
    const double dt = dtf, h = dt / c.substeps;
    const dvec3 g(0.0, -(double)m_Gravity, 0.0);
    const double cdrag = kDragCoef * c.drag;
    std::vector<double> mass(n), ks(n), kd(n), L(n, 0.0);
    std::vector<dvec3> x(n), v(n), tgt(n), F(n), Fs(n), acc;
    for (int j = 0; j < n; ++j) {
        mass[j] = c.m[j]; ks[j] = c.ks[j]; kd[j] = c.kd[j];
        x[j] = dvec3(c.x[j]); v[j] = dvec3(c.v[j]); tgt[j] = dvec3(m.clothPos[c.bones[j]]);
    }
    // rest lengths from the animated pose (the rods are inextensible)
    for (int j = 1; j < n; ++j) L[j] = glm::length(tgt[j] - tgt[j - 1]);
    const dvec3 rootV = (tgt[0] - x[0]) / dt;   // root driven by velocity (flag +0xe)

    // pre-pass: the root carries the drive velocity; drop radial relative velocity
    {
        v[0] = rootV;
        std::vector<dvec3> w(n, dvec3(0.0));
        for (int j = 1; j < n; ++j) {
            const dvec3 dd = x[j] - x[j - 1];
            w[j] = v[j] - v[j - 1];
            const double l2 = glm::dot(dd, dd);
            if (l2 > 1e-20) w[j] -= (glm::dot(w[j], dd) / l2) * dd;
        }
        for (int j = 1; j < n; ++j) v[j] = v[j - 1] + w[j];
    }
    // pre-sag (0x8215fa38): the spring rest points are moved so that the animated
    // pose is the static equilibrium under gravity
    std::vector<dvec3> rel(n, dvec3(0.0));
    if (c.spring) {
        std::vector<dvec3> G0(n, dvec3(0.0));
        for (int j = 1; j < n; ++j) G0[j] = mass[j] * g;
        AVClothAccel(tgt, mass, G0, G0, acc);
        for (int j = 1; j < n; ++j) {
            const dvec3 off = ks[j] > 0.0 ? -(mass[j] * acc[j]) / ks[j] : dvec3(0.0);
            rel[j] = tgt[j] + off - tgt[0];
        }
    }
    std::vector<dvec3> d(n), w(n);
    for (int s = 0; s < c.substeps; ++s) {
        for (int j = 1; j < n; ++j) {
            dvec3 G = mass[j] * g, U(0.0);
            if (c.spring)
                U = ks[j] * ((x[0] + rel[j]) - x[j]) - 2.0 * mass[j] * kd[j] * v[j];
            if (cdrag > 0.0) {
                const dvec3 dd = x[j] - x[j - 1];
                const double ll = glm::length(dd);
                if (ll > 1e-12) {
                    const dvec3 uu = dd / ll;
                    const dvec3 perp = v[j] - glm::dot(v[j], uu) * uu;
                    G -= cdrag * glm::length(perp) * perp;
                }
            }
            F[j] = G + U; Fs[j] = G - U;
        }
        AVClothAccel(x, mass, F, Fs, acc);
        dvec3 x0 = x[0] + rootV * h;
        dvec3 px = x0, pv = rootV;
        for (int j = 1; j < n; ++j) {
            dvec3 wj = (v[j] - v[j - 1]) + (acc[j] - acc[j - 1]) * h;
            dvec3 dj = (x[j] - x[j - 1]) + wj * h;
            {   // length projection (game: flag +0xc, i.e. spring chains only; here
                // always, since without it a spring-less chain such as JDWRibon
                // stretches without bound: the game must restore it elsewhere)
                const double ld = glm::length(dj);
                if (ld > 1e-12) dj *= L[j] / ld;
                const double l2 = glm::dot(dj, dj);
                if (l2 > 1e-20) wj -= (glm::dot(wj, dj) / l2) * dj;
            }
            d[j] = dj; w[j] = wj;
        }
        x[0] = x0; v[0] = rootV;
        for (int j = 1; j < n; ++j) { px += d[j]; pv += w[j]; x[j] = px; v[j] = pv; }
    }
    for (int j = 0; j < n; ++j) { c.x[j] = glm::vec3(x[j]); c.v[j] = glm::vec3(v[j]); }
}

// Collision response 0x82137cf0 (mode 0): rotate the node's segment about its
// parent node until it sits at R + margin from the sphere center (law of
// cosines), then the nodes below follow the leader.
void AnimViewport::CollideChain(SkeletalModel& m, ClothChain& c, bool keepIn, float dt) {
    const std::vector<Bone>& R = m.rigidAnim.size() == m.bind.size() ? m.rigidAnim : m.anim;
    const int n = (int)c.bones.size();
    std::vector<float> L(n, 0.f);
    for (int j = 1; j < n; ++j) L[j] = glm::length(c.tgt[j] - c.tgt[j - 1]);

    auto respond = [&](int j, const glm::vec3& ctr, float rad, float margin) {
        const float a = rad + margin;
        const glm::vec3 P = c.x[j - 1];
        const glm::vec3 toC = ctr - P, toN = c.x[j] - P;
        const float dist = glm::length(c.x[j] - ctr);
        if (!((margin > 0.f && dist < a) || (margin < 0.f && dist > a))) return;
        const float b = L[j], cc = glm::length(toC), ln = glm::length(toN);
        if (b < 1e-6f || cc < 1e-6f || ln < 1e-6f) return;
        const float want = std::acos(glm::clamp((b * b + cc * cc - a * a) / (2.f * b * cc), -1.f, 1.f));
        const float cur = std::acos(glm::clamp(glm::dot(toC, toN) / (cc * ln), -1.f, 1.f));
        glm::vec3 axis = glm::cross(toC, toN);
        float la = glm::length(axis);
        if (la < 1e-9f) {                                  // node on the center line
            axis = glm::cross(toC, glm::vec3(0, 1, 0));
            if (glm::length(axis) < 1e-6f) axis = glm::cross(toC, glm::vec3(1, 0, 0));
            la = glm::length(axis);
        }
        axis /= la;
        const glm::mat3 rot(glm::rotate(glm::mat4(1.f), want - cur, axis));
        c.x[j] = P + rot * (toN * (b / ln));
        for (int k = j + 1; k < n; ++k) {                  // follow the leader
            const glm::vec3 dk = c.x[k] - c.x[k - 1];
            const float lk = glm::length(dk);
            if (lk > 1e-6f) c.x[k] = c.x[k - 1] + dk * (L[k] / lk);
        }
        if (margin > 0.f) {                                // keep-out: lose the inward normal speed
            const glm::vec3 nrm = glm::normalize(c.x[j] - ctr);
            const float vn = glm::dot(c.v[j], nrm);
            if (vn < 0.f) c.v[j] -= vn * nrm;
            for (int k = j + 1; k < n; ++k) c.v[k] = (c.x[k] - c.xPrev[k]) / dt;
        }
        else {                                             // keep-in: stop
            for (int k = j; k < n; ++k) c.v[k] = glm::vec3(0.f);
        }
        };
    if (keepIn) {                                          // 0x82137510
        for (int j = 1; j < n; ++j)
            for (int k : m.cloth[c.bones[j]].keepIn) {
                const ClothSphere& s = m.colliders[k];
                respond(j, AVColliderCenter(m, R, s), s.r, kKeepIn);
            }
        return;
    }
    for (int pass = 0; pass < 2; ++pass) {                 // 0x82136b08: two mask passes
        for (int j = 1; j < n; ++j) {
            const uint16_t mask = pass == 0 ? m.cloth[c.bones[j]].mask1 : m.cloth[c.bones[j]].mask2;
            for (int b = 0; b < 16; ++b) {
                if (!(mask >> b & 1u)) continue;
                const auto range = m.clothGroups[b + 1];
                for (uint16_t k = range.first; k < range.second; ++k) {
                    const ClothSphere& s = m.colliders[k];
                    if (s.type != 0) continue;             // boxes/capsules: not simulated yet
                    respond(j, AVColliderCenter(m, R, s), s.r, kKeepOut);
                }
            }
        }
    }
}

namespace {
    // rotate v about the unit axis by angle
    inline glm::vec3 AVRot(const glm::vec3& v, const glm::vec3& axis, float ang) {
        return glm::mat3(glm::rotate(glm::mat4(1.f), ang, axis)) * v;
    }
    inline float AVAngle(const glm::vec3& a, const glm::vec3& b) {      // 0x82134d18
        const float la = glm::length(a), lb = glm::length(b);
        if (la < 1e-9f || lb < 1e-9f) return 0.f;
        return std::acos(glm::clamp(glm::dot(a, b) / (la * lb), -1.f, 1.f));
    }
    inline float AVLawCos(float a, float b, float c) {                 // 0x821354f0
        if (b * c == 0.f) return 0.f;
        return std::acos(glm::clamp((b * b + c * c - a * a) / (2.f * b * c), -1.f, 1.f));
    }
    // rotate segment k of chain x about x[k-1] so its end sits at distance a from T
    void AVSwing(std::vector<glm::vec3>& x, int k, const glm::vec3& T, float a, float Lk) {
        const glm::vec3 seg = x[k] - x[k - 1], to = T - x[k - 1];
        const float c = glm::length(to);
        glm::vec3 axis = glm::cross(seg, to);
        const float la = glm::length(axis);
        if (la < 1e-9f || c < 1e-9f) return;
        const float ang = AVAngle(seg, to) - AVLawCos(a, Lk, c);
        x[k] = x[k - 1] + AVRot(seg, axis / la, ang);
    }

    // 0x8213d640: put node n of chain A at exactly dist from point T, swinging
    // its segment, or bending the chain above it (backward reach) when one
    // segment is not enough. Velocities are kept.
    void AVSheetReach(ClothChain& A, int n, const glm::vec3& T, float dist) {
        std::vector<glm::vec3>& x = A.x;
        const int cnt = (int)x.size();
        std::vector<float> L(cnt, 0.f);
        for (int j = 1; j < cnt; ++j) L[j] = glm::length(A.tgt[j] - A.tgt[j - 1]);
        const float c = glm::length(T - x[n - 1]);
        if (L[n] + dist >= c) {                            // one segment reaches
            if (dist < c) AVSwing(x, n, T, dist, L[n]);
        }
        else {
            float total = 0.f;
            for (int k = 1; k <= n; ++k) total += L[k];
            const glm::vec3 to0 = T - x[0];
            const float d0 = glm::length(to0);
            if (total + dist <= d0) {                      // out of reach: stretch toward T
                float acc = 0.f;
                for (int k = 1; k <= n; ++k) { acc += L[k]; x[k] = x[0] + to0 * (acc / d0); }
            }
            else {
                x[n] = T - (T - x[n - 1]) * (dist / c);
                for (int k = n - 1; k >= 1; --k) {
                    const glm::vec3 vec = x[k + 1] - x[k - 1];
                    const float dv = glm::length(vec), S = L[k] + L[k + 1];
                    if (dv < 1e-9f) break;
                    if (S <= dv) {                         // straight: keep going up
                        x[k] = x[k + 1] - vec * (L[k + 1] / dv);
                        if (S == dv) break;
                        continue;
                    }
                    AVSwing(x, k, x[k + 1], L[k + 1], L[k]);
                    break;
                }
                // the top segment may now be long: restore lengths from the root down
                for (int k = 1; k <= n; ++k) {
                    const glm::vec3 dk = x[k] - x[k - 1];
                    const float lk = glm::length(dk);
                    if (lk > 1e-9f) x[k] = x[k - 1] + dk * (L[k] / lk);
                }
            }
        }
        for (int k = n + 1; k < cnt; ++k) {                // follow the leader
            const glm::vec3 dk = x[k] - x[k - 1];
            const float lk = glm::length(dk);
            if (lk > 1e-9f) x[k] = x[k - 1] + dk * (L[k] / lk);
        }
    }

    // 0x8213f870: flare a chain outward (away from the character's vertical
    // axis) by ang radians, segment by segment.
    void AVSheetFlare(ClothChain& A, const glm::vec3& origin, float ang) {
        std::vector<glm::vec3>& x = A.x;
        glm::vec3 r = x[0] - origin;
        if (glm::length(r) < 1e-9f) return;
        glm::vec3 axis = glm::cross(glm::normalize(r), glm::vec3(0, 1, 0));
        const float la = glm::length(axis);
        if (la < 1e-6f) return;
        axis /= la;
        for (int j = 1; j < (int)x.size(); ++j) {
            const float Lj = glm::length(A.tgt[j] - A.tgt[j - 1]);
            const glm::vec3 d = AVRot(x[j] - x[j - 1], axis, ang);
            const float ld = glm::length(d);
            if (ld > 1e-9f) x[j] = x[j - 1] + d * (Lj / ld);
        }
    }
}

// Sheet solver 0x8213f448.
void AnimViewport::SolveSheets(SkeletalModel& m) {
    const std::vector<Bone>& R = m.rigidAnim.size() == m.bind.size() ? m.rigidAnim : m.anim;
    const glm::vec3 origin = R.empty() ? glm::vec3(0.f) : R[0].world_pos;
    for (const ClothSheet& sh : m.sheets) {
        const int cnt = (int)sh.chains.size();
        // rest distances are taken on the animated pose at reset
        for (int ci : sh.chains) {
            ClothChain& c = m.chains[ci];
            for (int j = 1; j < (int)c.links.size(); ++j)
                for (ClothLink& l : c.links[j])
                    if (l.rest < 0.f)
                        l.rest = glm::length(m.chains[l.chain].tgt[l.node] - c.tgt[j]);
        }
        // the chain that strays most from the animation leads
        float best = 0.f; int lead = -1;
        for (int k = 0; k < cnt; ++k) {
            const ClothChain& c = m.chains[sh.chains[k]];
            float dev = 0.f;
            for (int j = sh.first; j <= sh.last && j < (int)c.x.size(); ++j) dev += glm::length(c.x[j] - c.tgt[j]);
            if (best - dev < 0.001f) { best = dev; lead = k; }
        }
        if (lead < 0 || best <= m.chains[sh.chains[lead]].sheetThr) continue;
        auto pull = [&](int cur, int nb) {   // neighbour chain nb follows chain cur
            ClothChain& C = m.chains[sh.chains[cur]];
            ClothChain& B = m.chains[sh.chains[nb]];
            for (int j = sh.first; j <= sh.last; ++j) {
                if (j >= (int)C.x.size() || j >= (int)B.x.size()) break;
                const ClothLink* lk = nullptr;
                for (const ClothLink& l : C.links[j]) if (l.chain == sh.chains[nb]) lk = &l;
                if (!lk || lk->rest <= 0.f) continue;
                AVSheetReach(B, j, C.x[lk->node], lk->weight * lk->rest);
            }
            };
        for (int k = lead; k > 0; --k) pull(k, k - 1);
        for (int k = lead; k < cnt - 1; ++k) pull(k, k + 1);
        const ClothChain& L = m.chains[sh.chains[lead]];
        const float tol = (sh.first < (int)L.links.size() && !L.links[sh.first].empty()) ? L.links[sh.first][0].tol : 0.f;
        if (tol > 0.f) {
            AVSheetFlare(m.chains[sh.chains[lead]], origin, tol);
            if (lead > 0) AVSheetFlare(m.chains[sh.chains[lead - 1]], origin, tol);
            if (lead < cnt - 1) AVSheetFlare(m.chains[sh.chains[lead + 1]], origin, tol);
        }
    }
}

// Write the solved positions/rotations over the rigid pose in m.anim.
void AnimViewport::WriteClothPose(SkeletalModel& m) {
    const size_t N = m.bind.size();
    const std::vector<Bone>& R = (m.rigidAnim.size() == N) ? m.rigidAnim : m.anim;
    for (int i : m.clothOrder) {
        if (!m.cloth[i].isDyn && !m.cloth[i].follower) continue;
        m.anim[i].world_pos = m.clothPos[i];
        m.anim[i].world_rot = m.clothDelta[i] * R[i].world_rot;
    }
}

// Pose pass in topological order. advance: run one cloth frame for every chain
// (else only re-place the stored chain state on the current rigid pose).
static void AVPoseCloth(SkeletalModel& m, bool advance,
    const std::function<void(ClothChain&)>& simulate) {
    const size_t N = m.bind.size();
    const std::vector<Bone>& R = (m.rigidAnim.size() == N) ? m.rigidAnim : m.anim;
    std::vector<glm::vec3>& P = m.clothPos;
    std::vector<glm::mat3>& D = m.clothDelta;
    // rigid bones first, so colliders follow this frame's animation
    for (size_t i = 0; i < N; ++i)
        if (!m.cloth[i].isDyn && !m.cloth[i].follower) { P[i] = R[i].world_pos; D[i] = glm::mat3(1.f); }
    std::vector<char> done(N, 0);
    for (int i : m.clothOrder) {
        const ClothBone& cb = m.cloth[i];
        const int par = m.bind[i].parent_idx;
        const bool parOk = par >= 0 && par < (int)N;
        const bool parMoved = parOk && (m.cloth[par].isDyn || m.cloth[par].follower);
        const glm::mat3 inh = parMoved ? D[par] : glm::mat3(1.f);
        if (cb.chain >= 0) {
            ClothChain& c = m.chains[cb.chain];
            const int n = (int)c.bones.size();
            // targets (0x8213d0c0): the chain's bind shape carried by the root's
            // current frame; the animation of the chain bones themselves is ignored
            const glm::vec3 root = parMoved ? P[par] + inh * (R[i].world_pos - R[par].world_pos)
                : R[i].world_pos;
            const glm::mat3 carry = inh * R[i].world_rot * glm::transpose(m.bind[i].world_rot);
            for (int j = 0; j < n; ++j)
                P[c.bones[j]] = root + carry * (m.bind[c.bones[j]].world_pos - m.bind[i].world_pos);
            c.tgt.resize(n);
            for (int j = 0; j < n; ++j) c.tgt[j] = P[c.bones[j]];
            if (!c.init) {
                c.x = c.tgt; c.v.assign(n, glm::vec3(0.f));
                c.init = true;
            }
            if (advance) {
                c.xPrev = c.x;
                simulate(c);
            }
            else {
                // keep the chain shape relative to the root, follow the root
                const glm::vec3 shift = root - c.x[0];
                for (glm::vec3& p : c.x) p += shift;
            }
            // positions, then rotations from where each chain child landed
            glm::mat3 frame = inh;
            for (int j = 0; j < n; ++j) {
                const int b = c.bones[j];
                P[b] = c.x[j];
                if (j + 1 < n) {
                    const int k = c.bones[j + 1];
                    D[b] = AVrotBetween(frame * (R[k].world_pos - R[b].world_pos), c.x[j + 1] - c.x[j]) * frame;
                }
                else D[b] = frame;
                frame = D[b];
                done[b] = 1;
            }
            continue;
        }
        if (done[i]) continue;
        if (cb.isDyn || cb.follower) {                     // carried by the parent
            P[i] = parOk ? P[par] + inh * (R[i].world_pos - R[par].world_pos) : R[i].world_pos;
            D[i] = inh;
        }
    }
}

// One cloth frame in the game's order (0x8213e420): every chain is solved and
// kept inside its keep-in volumes (parents first, so child chains start from
// the solved parent), then the sheet solver, then the keep-out spheres.
void AnimViewport::StepDynamics(SkeletalModel& m, float dt) {
    const size_t N = m.bind.size();
    if (!m.hasCloth || m.cloth.size() != N || m.anim.size() != N) return;
    if (!m.clothInit) { ResetDynamics(m); m.clothInit = true; }
    AVPoseCloth(m, true, [&](ClothChain& c) {
        SolveChain(m, c, dt);
        CollideChain(m, c, true, dt);
        });
    SolveSheets(m);
    for (ClothChain& c : m.chains) CollideChain(m, c, false, dt);
    AVPoseCloth(m, false, nullptr);   // final pose from the chain states
    WriteClothPose(m);
}

// Overlay the persistent cloth state on the current rigid pose without advancing
// it (frames where the accumulator runs no step, scrubbing, model switch).
void AnimViewport::ApplyClothPose(SkeletalModel& m) {
    const size_t N = m.bind.size();
    if (!m.hasCloth || !m.clothInit || m.cloth.size() != N || m.anim.size() != N) return;
    AVPoseCloth(m, false, nullptr);
    WriteClothPose(m);
}

void AnimViewport::SettleDynamics(SkeletalModel& m, int steps) {
    if (!m_Cloth || !m.hasCloth) return;
    for (int s = 0; s < steps; ++s) {
        if (!m.anims.empty()) ApplyPose(m, m.frame);
        else { m.anim = m.bind; m.rigidAnim = m.anim; }
        StepDynamics(m, kClothStep);
    }
}

void AnimViewport::Tick(float dt) {
    SkeletalModel* m = Active();
    if (!m_HasContent || !m || !m->playing || m->anims.empty()) return;
    const NMTNAnimation& anim = m->anims[m->animIdx];
    if (anim.frame_count == 0) return;
    const float adv = dt * m_PlaySpeed;   // animation frames this tick
    m->frame = std::fmod(m->frame + adv, (float)anim.frame_count);
    ApplyPose(*m, m->frame);
    if (m_Cloth && m->hasCloth) {
        // Physics runs in animation time (NMTN is authored at 30 fps) at a fixed
        // 1/60 substep, so the speed slider slows the cloth down with the motion.
        // The small tolerance stops float drift from skipping a substep.
        m_ClothAccum += adv / 30.f;
        int n = 0;
        while (m_ClothAccum >= kClothStep * 0.999f && n < 8) {
            m_ClothAccum -= kClothStep;
            StepDynamics(*m, kClothStep);
            ++n;
        }
        if (m_ClothAccum < 0.f || n == 8) m_ClothAccum = 0.f;
        ApplyClothPose(*m);
    }
    RebuildSkeletonLines(*m);
}

void AnimViewport::SetMat4Array(GLuint prog, const char* name,
    const glm::mat4* mats, int count) {
    GLint loc = glGetUniformLocation(prog, name);
    if (loc >= 0)
        glUniformMatrix4fv(loc, count, GL_FALSE, glm::value_ptr(mats[0]));
}


void AnimViewport::Render() {
    ImGui::Begin("Animation Viewport");

    if (!m_HasContent || m_Models.empty()) {
        ImGui::TextDisabled("Load a .bop / .e / .p3obj file with NBN2 + NMTN chunks.");
        ImGui::End();
        return;
    }

    if (m_Models.size() > 1) {
        ImGui::TextUnformatted("Model");
        ImGui::SameLine();
        ImGui::PushItemWidth(220);
        const SkeletalModel& cur = m_Models[m_ModelIdx];
        char preview[96];
        snprintf(preview, sizeof(preview), "%s  (%d bones)",
            cur.name.c_str(), (int)cur.bind.size());
        if (ImGui::BeginCombo("##model", preview)) {
            for (int i = 0; i < (int)m_Models.size(); i++) {
                const SkeletalModel& mm = m_Models[i];
                bool sel = (i == m_ModelIdx);
                ImGui::PushID(i);
                char lbl[128];
                snprintf(lbl, sizeof(lbl), "%s  -  %d bones, %d meshes, %d anims",
                    mm.name.c_str(), (int)mm.bind.size(),
                    (int)mm.meshes.size(), (int)mm.anims.size());
                if (ImGui::Selectable(lbl, sel)) SelectModel(i);
                if (sel) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        ImGui::Separator();
    }

    SkeletalModel& M = m_Models[m_ModelIdx];

    if (!M.anims.empty()) {
        ImGui::PushItemWidth(200);
        const char* preview = M.anims[M.animIdx].name.empty()
            ? "(anim)" : M.anims[M.animIdx].name.c_str();
        if (ImGui::BeginCombo("##anim", preview)) {
            for (int i = 0; i < (int)M.anims.size(); i++) {
                bool sel = (i == M.animIdx);
                ImGui::PushID(i);
                const char* lbl = M.anims[i].name.empty()
                    ? "(anim)" : M.anims[i].name.c_str();
                if (ImGui::Selectable(lbl, sel)) {
                    M.animIdx = i;
                    M.frame = 0.f;
                    ApplyPose(M, 0.f);
                    if (m_Cloth && M.hasCloth) { ResetDynamics(M); SettleDynamics(M, 24); }
                    RebuildSkeletonLines(M);
                }
                if (sel) ImGui::SetItemDefaultFocus();
                ImGui::PopID();
            }
            ImGui::EndCombo();
        }
        ImGui::PopItemWidth();
        ImGui::SameLine();
        if (ImGui::Button(M.playing ? "Pause" : " Play ")) M.playing = !M.playing;
        ImGui::SameLine();
        if (ImGui::Button("Reset")) {
            M.frame = 0.f; M.playing = false;
            ApplyPose(M, 0.f);
            if (m_Cloth && M.hasCloth) { ResetDynamics(M); SettleDynamics(M, 24); }
            RebuildSkeletonLines(M);
        }
        const NMTNAnimation& anim = M.anims[M.animIdx];
        float maxF = (float)(anim.frame_count > 0 ? anim.frame_count - 1 : 1);
        ImGui::PushItemWidth(-1);
        if (ImGui::SliderFloat("##frame", &M.frame, 0.f, maxF, "Frame %.1f")) {
            M.playing = false;
            ApplyPose(M, M.frame);
            if (m_Cloth && M.hasCloth) SettleDynamics(M, 8);
            RebuildSkeletonLines(M);
        }
        ImGui::PopItemWidth();
        ImGui::SameLine(0, 8);
        ImGui::Text("%d frames", anim.frame_count);
        ImGui::SliderFloat("Speed", &m_PlaySpeed, 1.f, 60.f, "%.0f fps");
        ImGui::SameLine();        ImGui::Checkbox("Textured", &m_Textured);
        ImGui::SameLine();
        ImGui::Checkbox("Lighting", &m_Lighting);
        ImGui::SameLine();
        ImGui::Checkbox("Skeleton", &m_ShowSkel);
        if (M.hasCloth) {
            ImGui::SameLine();
            if (ImGui::Checkbox("Cloth", &m_Cloth)) {
                ApplyPose(M, M.frame);   // drop the cloth overlay when turning it off
                if (m_Cloth) { ResetDynamics(M); SettleDynamics(M, 24); }
                RebuildSkeletonLines(M);
            }
            ImGui::PushItemWidth(130);
            ImGui::SliderFloat("Gravity", &m_Gravity, 0.f, 15.f, "%.1f");
            ImGui::PopItemWidth();
        }
        int nclo = 0; for (const auto& cb : M.cloth) if (cb.isDyn) ++nclo;
        ImGui::Text("Bones: %d   Meshes: %d   Textures: %d   Cloth bones: %d",
            (int)M.bind.size(), (int)M.meshes.size(), (int)M.matToTex.size(), nclo);
    }
    else {
        ImGui::TextDisabled("No NMTN animations found for this model.");
        ImGui::Checkbox("Textured", &m_Textured);
        ImGui::SameLine();
        ImGui::Checkbox("Skeleton", &m_ShowSkel);
        ImGui::Text("Bones: %d   Meshes: %d", (int)M.bind.size(), (int)M.meshes.size());
    }

    ImGui::Separator();

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
        glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, m_ViewW, m_ViewH);
        glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT,
            GL_RENDERBUFFER, m_FBORbo);
        glBindFramebuffer(GL_FRAMEBUFFER, 0);
        m_FBOw = m_ViewW; m_FBOh = m_ViewH;
    }

    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glViewport(0, 0, m_ViewW, m_ViewH);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0.12f, 0.12f, 0.16f, 1.f);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);

    const float     aspect = (float)m_ViewW / (float)m_ViewH;
    const glm::mat4 view = m_Camera.GetViewMatrix();
    const glm::mat4 proj = m_Camera.GetProjectionMatrix(aspect);
    const glm::vec3 lightDir = glm::normalize(glm::vec3(0.5f, 1.0f, 0.7f));

    if (!M.meshes.empty() && m_SkinShader) {
        m_SkinShader->Use();
        m_SkinShader->SetMat4("u_View", view);
        m_SkinShader->SetMat4("u_Proj", proj);
        m_SkinShader->SetVec3("u_LightDir", lightDir);
        m_SkinShader->SetVec3("u_MeshColor", glm::vec3(0.72f, 0.72f, 0.88f));
        m_SkinShader->SetInt("u_EnableLighting", m_Lighting ? 1 : 0);
        m_SkinShader->SetInt("u_Diffuse", 0);
        GLuint prog = m_SkinShader->GetProgram();
        const auto& pose = M.anim.empty() ? M.bind : M.anim;

        for (const auto& mesh : M.meshes) {
            if (!mesh.VAO || mesh.bone_id_list.empty()) continue;

            int n_local = std::min((int)mesh.bone_id_list.size(), 64);
            glm::mat4 skin_mats[64];
            for (int local = 0; local < n_local; ++local) {
                uint16_t global = mesh.bone_id_list[local];
                skin_mats[local] = (global < (uint16_t)pose.size())
                    ? WorldMat(pose[global]) * M.invBind[global]
                    : glm::mat4(1.f);
            }
            SetMat4Array(prog, "u_SkinMats", skin_mats, n_local);


            glBindVertexArray(mesh.VAO);

            if (!m_Textured || mesh.sections.empty() || mesh.index_count == 0) {
                m_SkinShader->SetInt("u_UseTexture", 0);
                glDisable(GL_BLEND); glDepthMask(GL_TRUE);
                if (mesh.draw_sequential)
                    glDrawArrays(GL_POINTS, 0, (GLsizei)mesh.vertex_count);
                else if (mesh.index_count > 0)
                    glDrawElements(GL_TRIANGLES, (GLsizei)mesh.index_count,
                        GL_UNSIGNED_SHORT, nullptr);
                else
                    glDrawArrays(GL_TRIANGLES, 0, (GLsizei)mesh.vertex_count);
            }
            else {
                for (const auto& sec : mesh.sections) {
                    if (sec.index_count == 0) continue;
                    auto it = M.matToTex.find(sec.mat_id);
                    if (it != M.matToTex.end() && it->second) {
                        glActiveTexture(GL_TEXTURE0);
                        glBindTexture(GL_TEXTURE_2D, it->second);
                        GLenum wrap = M.matClamp.count(sec.mat_id)
                            ? GL_CLAMP_TO_EDGE : GL_REPEAT;
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
                        m_SkinShader->SetInt("u_UseTexture", 1);
                        if (M.matBlend.count(sec.mat_id)) {
                            glEnable(GL_BLEND);
                            glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
                            glDepthMask(GL_FALSE);
                            m_SkinShader->SetFloat("u_AlphaThreshold", 0.01f);
                        }
                        else {
                            glDisable(GL_BLEND);
                            glDepthMask(GL_TRUE);
                            m_SkinShader->SetFloat("u_AlphaThreshold", 0.5f);
                        }
                    }
                    else {
                        m_SkinShader->SetInt("u_UseTexture", 0);
                        glDisable(GL_BLEND); glDepthMask(GL_TRUE);
                    }
                    glDrawElements(GL_TRIANGLES, (GLsizei)sec.index_count,
                        GL_UNSIGNED_SHORT,
                        (void*)(uintptr_t)(sec.index_start * sizeof(uint16_t)));
                }
            }
            glBindVertexArray(0);
        }
        glActiveTexture(GL_TEXTURE0);
        glBindTexture(GL_TEXTURE_2D, 0);
        glDisable(GL_BLEND);
        glDepthMask(GL_TRUE);
    }

    if (m_ShowSkel && M.lineVAO && M.lineVerts > 0 && m_BoneShader) {
        glClear(GL_DEPTH_BUFFER_BIT);
        m_BoneShader->Use();
        m_BoneShader->SetMat4("u_View", view);
        m_BoneShader->SetMat4("u_Proj", proj);
        m_BoneShader->SetVec4("u_Color", glm::vec4(1.f, 0.85f, 0.f, 1.f));
        glLineWidth(1.5f);
        glBindVertexArray(M.lineVAO);
        glDrawArrays(GL_LINES, 0, M.lineVerts);
        glBindVertexArray(0);
    }

    glBindFramebuffer(GL_FRAMEBUFFER, 0);

    ImVec2 img_size((float)m_ViewW, (float)m_ViewH);
    ImGui::SetCursorScreenPos(vp_pos);
    ImGui::InvisibleButton("##av", img_size,
        ImGuiButtonFlags_MouseButtonLeft |
        ImGuiButtonFlags_MouseButtonRight |
        ImGuiButtonFlags_MouseButtonMiddle);
    ImGui::GetWindowDrawList()->AddImage(
        (ImTextureID)(intptr_t)m_FBOTex, vp_pos,
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