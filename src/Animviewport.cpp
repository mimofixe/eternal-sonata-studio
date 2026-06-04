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
    float d = glm::dot(a, b);
    if (d > 0.9999f) return glm::mat3(1.f);
    if (d < -0.9999f) {
        glm::vec3 ax = glm::cross(a, glm::vec3(1, 0, 0));
        if (glm::length(ax) < 1e-4f) ax = glm::cross(a, glm::vec3(0, 1, 0));
        return glm::mat3(glm::rotate(glm::mat4(1.f), 3.14159265f, glm::normalize(ax)));
    }
    return glm::mat3(glm::rotate(glm::mat4(1.f), std::acos(d),
        glm::normalize(glm::cross(a, b))));
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

static glm::vec3 AVclosestOnSeg(glm::vec3 q, glm::vec3 a, glm::vec3 b) {
    glm::vec3 ab = b - a;
    float dd = glm::dot(ab, ab);
    float t = (dd < 1e-9f) ? 0.f : glm::clamp(glm::dot(q - a, ab) / dd, 0.f, 1.f);
    return a + t * ab;
}

// Parse NDYN (per-bone params), NCLS (collider radii) and NLC2 (collision proxy
// points) from this model's region and build the cloth simulation data.
void AnimViewport::ParseDynamics(SkeletalModel& m, const std::vector<uint8_t>& fd,
    size_t begin, size_t end) {
    const size_t N = m.bind.size();
    m.cloth.assign(N, ClothBone{});
    m.pCur.assign(N, glm::vec3(0.f));
    m.pPrev.assign(N, glm::vec3(0.f));
    m.colliders.clear();
    m.hasCloth = false;
    m.clothInit = false;
    if (N == 0 || end > fd.size() || begin + 8 > end) return;
    const uint8_t* d = fd.data();

    // per-bone chain data from the bind pose
    bool anyDyn = false;
    for (size_t i = 0; i < N; ++i) {
        ClothBone& cb = m.cloth[i];
        cb.isDyn = (m.bind[i].flags != 96 && m.bind[i].flags != 224);
        int par = m.bind[i].parent_idx;
        cb.isRoot = cb.isDyn && (par < 0 || par >= (int)N || !(m.bind[par].flags != 96 && m.bind[par].flags != 224));
        if (par >= 0 && par < (int)N) {
            glm::vec3 rel = m.bind[i].world_pos - m.bind[par].world_pos;
            glm::mat3 pInv = glm::transpose(m.bind[par].world_rot);
            cb.localOff = pInv * rel;
            cb.bindLocalRot = pInv * m.bind[i].world_rot;
            cb.restLen = glm::length(rel);
        }
        if (cb.isDyn) anyDyn = true;
    }
    if (!anyDyn) return;

    // NDYN: 48-byte records {u16 chainlen, u16 bone, ... f32 A@0x08, f32 B@0x10, f32 D@0x1C}
    for (size_t p = begin; p + 8 <= end; ) {
        if (!std::memcmp(d + p, "NDYN", 4)) {
            uint32_t sz = AVbeU32(d + p + 4);
            size_t cend = std::min(p + sz, end);
            for (size_t r = p + 8; r + 48 <= cend; r += 48) {
                uint16_t bone = AVbeU16(d + r + 2);
                if (bone >= 3 && bone < N) {
                    float A = AVbeF32(d + r + 0x08);
                    float B = AVbeF32(d + r + 0x10);
                    float C = AVbeF32(d + r + 0x14);
                    float D = AVbeF32(d + r + 0x1C);
                    ClothBone& cb = m.cloth[bone];
                    // Engine model: B drives how strongly the bone returns to the
                    // parent-frame rest direction; C is velocity damping; A is mass.
                    // D is NOT a hard angle clamp (the projection is Gram-Schmidt
                    // frame maintenance), so it is not used as a limit here.
                    // Calibrated against the PS3 retail Dynabone solver: the
                    // restoring pull toward the rest frame is very weak (~0.01 base),
                    // so gravity dominates and the skirt hangs instead of snapping
                    // back to its flared bind pose. NDYN B (3..30) scales it gently.
                    cb.stiffness = glm::clamp(B * 0.004f, 0.002f, 0.08f);
                    cb.damping = glm::clamp(C * 0.012f, 0.04f, 0.85f);
                    cb.mass = glm::clamp(A * 60.f, 0.05f, 0.6f);   // inertia weight (not used to scale gravity)
                    (void)D;
                }
            }
            p = (sz >= 8) ? p + sz : p + 4;
        }
        else ++p;
    }

    // NCLS: 48-byte records = body collision SPHERES. Real file-record layout
    // (verified against the chunk): f32 radius @ +0x04, u16 bone @ +0x26, sphere
    // centered on the bone (the center field is zero). A small header precedes the
    // records; we skip it implicitly by keeping only records with a valid bone
    // index and a limb-scale radius. The engine collides cloth particles against
    // these spheres (CSphere; the CEllipseCylinder variant is a no-op stub). The
    // big body spheres (hip/torso) would inflate the skirt into a disk, so keep
    // only the limb-hugging spheres.
    {
        std::unordered_map<uint64_t, bool> seen;
        for (size_t p = begin; p + 8 <= end; ) {
            if (!std::memcmp(d + p, "NCLS", 4)) {
                uint32_t sz = AVbeU32(d + p + 4);
                size_t cend = std::min(p + sz, end);
                for (size_t r = p + 8; r + 48 <= cend; r += 48) {
                    uint16_t bone = AVbeU16(d + r + 0x26);
                    float rad = AVbeF32(d + r + 0x04);
                    if (bone < 1 || bone >= N) continue;
                    if (!(rad > 0.03f && rad <= 0.16f)) continue;   // limb spheres; cap validated so the
                    // skirt clears the leg without ballooning (leg-vs-skirt penetration ~0, skirt radius
                    // matches the authored rigid pose). The i16 offset at +0x28/+0x2a/+0x2c (/64,
                    // bone-local) spreads the spheres ALONG the limb so the whole leg is covered.
                    glm::vec3 off(AVbeI16(d + r + 0x28), AVbeI16(d + r + 0x2a), AVbeI16(d + r + 0x2c));
                    off /= 64.f;
                    uint64_t key = ((uint64_t)bone << 20) ^ (uint64_t)(int)(rad * 1000);
                    if (seen[key]) continue; seen[key] = true;
                    ClothSphere s; s.bone = (int)bone; s.loff = off; s.r = rad;
                    m.colliders.push_back(s);
                }
                p = (sz >= 8) ? p + sz : p + 4;
            }
            else ++p;
        }
    }

    m.hasCloth = anyDyn;   // simulation runs on gravity+constraints even with no spheres
}

void AnimViewport::ResetDynamics(SkeletalModel& m) { m.clothInit = false; }

// One Verlet/PBD step, faithful to the decoded Dynabone solver:
//   vel    = (pCur - pPrev) * (1 - C)             // Verlet velocity, damped by C
//   pred   = pCur + vel + gravity*mass*dt^2       // gravity (9.80665) as acceleration
//   target = parentPos + parentFrame * localOff   // rest dir follows the simulated parent
//   np     = mix(pred, target, B)                 // B = return strength to rest dir
//   np     = parentPos + dir(np)*restLen          // HARD length constraint (rescale)
//   sphere collision push against the NCLS colliders
// The parent's *simulated* rotation cascades down the chain (srot), reproducing the
// engine's per-bone frame. dt is a fixed 1/60 substep (see Tick / SettleDynamics).
void AnimViewport::StepDynamics(SkeletalModel& m, float dt) {
    const size_t N = m.bind.size();
    if (!m.hasCloth || m.cloth.size() != N || m.anim.size() != N) return;

    std::vector<glm::vec3> rpos(N);
    std::vector<glm::mat3> srot(N);
    for (size_t i = 0; i < N; ++i) { rpos[i] = m.anim[i].world_pos; srot[i] = m.anim[i].world_rot; }

    if (!m.clothInit) {
        for (size_t i = 0; i < N; ++i) if (m.cloth[i].isDyn) { m.pCur[i] = rpos[i]; m.pPrev[i] = rpos[i]; }
        m.clothInit = true;
    }

    const float dt2 = dt * dt;
    const float G = m_Gravity;

    // precompute world sphere centers from the (rigid) pose once per step
    std::vector<glm::vec3> sc(m.colliders.size());
    for (size_t k = 0; k < m.colliders.size(); ++k) {
        const ClothSphere& s = m.colliders[k];
        sc[k] = (s.bone >= 0 && s.bone < (int)N) ? rpos[s.bone] + srot[s.bone] * s.loff : s.loff;
    }

    for (size_t i = 0; i < N; ++i) {
        const ClothBone& cb = m.cloth[i];
        if (!cb.isDyn) continue;
        int par = m.bind[i].parent_idx;
        if (par < 0 || par >= (int)N) continue;
        if (cb.isRoot) { m.pPrev[i] = m.pCur[i]; m.pCur[i] = rpos[i]; continue; } // pinned

        glm::vec3 parPos = m.cloth[par].isDyn ? m.pCur[par] : rpos[par];
        glm::mat3 parRot = srot[par];                 // simulated parent rotation (cascade)
        glm::vec3 restDir = parRot * cb.localOff;      // rest direction in the moving parent frame
        glm::vec3 target = parPos + restDir;

        // Verlet integrate with real gravity as an acceleration
        glm::vec3 vel = (m.pCur[i] - m.pPrev[i]) * (1.f - cb.damping);
        // Gravity is an ACCELERATION: mass-independent (all bones fall at g). The
        // engine stores a fixed (0,-9.80665,0) per bone; scaling by mass here was a
        // bug that made the light cloth bones barely fall and billow outward.
        glm::vec3 np = m.pCur[i] + vel + glm::vec3(0.f, -G, 0.f) * dt2;
        // return toward the parent-frame rest direction (strength = B)
        np = glm::mix(np, target, glm::clamp(cb.stiffness, 0.f, 1.f));

        glm::vec3 prev = m.pCur[i];
        // PBD iterations: hard length constraint + sphere collision
        for (int it = 0; it < 2; ++it) {
            glm::vec3 dir = np - parPos; float L = glm::length(dir);
            if (L > 1e-6f) np = parPos + dir * (cb.restLen / L);   // rescale to rest length
            // small margin so the cloth sits just off the leg surface (the collision
            // spheres sit inside the limb mesh); validated to remove leg-through-skirt
            // clipping while keeping the skirt at its authored width.
            const float margin = 0.015f;
            for (size_t k = 0; k < m.colliders.size(); ++k) {
                float rr = m.colliders[k].r + margin;
                glm::vec3 dvec = np - sc[k]; float dl = glm::length(dvec);
                if (dl < rr && dl > 1e-6f)
                    np = sc[k] + dvec * (rr / dl);                 // push to sphere surface
            }
        }
        m.pPrev[i] = prev; m.pCur[i] = np;

        // this bone's simulated rotation, so its children cascade off it
        glm::mat3 delta = AVrotBetween(restDir, np - parPos);
        srot[i] = delta * parRot * cb.bindLocalRot;
        m.anim[i].world_pos = np;
        m.anim[i].world_rot = srot[i];
    }
}

// Write the current cloth state (m.pCur + the cascade rotations) into m.anim,
// WITHOUT advancing the simulation. Must be called every rendered frame after the
// rigid ApplyPose, so that on frames where the fixed-timestep accumulator runs zero
// substeps the cloth is still drawn from its persistent state instead of snapping
// back to the rigid pose (which caused a rigid<->simulated strobing/flicker).
void AnimViewport::ApplyClothPose(SkeletalModel& m) {
    const size_t N = m.bind.size();
    if (!m.hasCloth || !m.clothInit || m.cloth.size() != N || m.anim.size() != N) return;
    std::vector<glm::vec3> rpos(N);
    std::vector<glm::mat3> srot(N);
    for (size_t i = 0; i < N; ++i) { rpos[i] = m.anim[i].world_pos; srot[i] = m.anim[i].world_rot; }
    for (size_t i = 0; i < N; ++i) {
        const ClothBone& cb = m.cloth[i];
        if (!cb.isDyn) continue;
        int par = m.bind[i].parent_idx;
        if (par < 0 || par >= (int)N) continue;
        if (cb.isRoot) { m.anim[i].world_pos = m.pCur[i]; continue; }
        glm::vec3 parPos = m.cloth[par].isDyn ? m.pCur[par] : rpos[par];
        glm::mat3 parRot = srot[par];
        glm::vec3 restDir = parRot * cb.localOff;
        glm::vec3 np = m.pCur[i];
        srot[i] = AVrotBetween(restDir, np - parPos) * parRot * cb.bindLocalRot;
        m.anim[i].world_pos = np;
        m.anim[i].world_rot = srot[i];
    }
}

void AnimViewport::SettleDynamics(SkeletalModel& m, int steps) {
    if (!m_Cloth || !m.hasCloth) return;
    // Each step reads a fresh rigid pose as reference; the sim state lives in
    // pCur/pPrev. Use the engine's fixed 1/60 substep so settling matches runtime.
    for (int s = 0; s < steps; ++s) {
        if (!m.anims.empty()) ApplyPose(m, m.frame);
        else m.anim = m.bind;
        StepDynamics(m, 1.f / 60.f);
    }
}

void AnimViewport::Tick(float dt) {
    SkeletalModel* m = Active();
    if (!m_HasContent || !m || !m->playing || m->anims.empty()) return;
    const NMTNAnimation& anim = m->anims[m->animIdx];
    if (anim.frame_count == 0) return;
    m->frame = std::fmod(m->frame + dt * m_PlaySpeed, (float)anim.frame_count);
    ApplyPose(*m, m->frame);
    // advance the cloth with the engine's fixed 1/60 timestep, decoupled from the
    // render framerate; cap the substep count so a hitch can't stall the loop.
    if (m_Cloth && m->hasCloth) {
        const float fixed = 1.f / 60.f;
        m_ClothAccum += std::min(dt, 0.1f);
        int sub = 0;
        while (m_ClothAccum >= fixed && sub < 8) {
            ApplyPose(*m, m->frame);     // refresh rigid reference for this substep
            StepDynamics(*m, fixed);
            m_ClothAccum -= fixed; ++sub;
        }
        // Re-apply the rigid base for this frame, then overlay the cloth from its
        // persistent state. Done every frame (even when sub==0) so the cloth never
        // strobes back to the rigid pose between physics ticks.
        ApplyPose(*m, m->frame);
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
            if (ImGui::Checkbox("Cloth", &m_Cloth) && m_Cloth) {
                ResetDynamics(M); SettleDynamics(M, 24);
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