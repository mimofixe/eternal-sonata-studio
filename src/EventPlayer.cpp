#include "EventPlayer.h"
#include "NMDLLoader.h"
#include "P3TexParser.h"
#include "../libs/bcdec/bcdec.h"   // implementation lives in P3TexParser.cpp
#include <glm/gtc/matrix_transform.hpp>
#include <algorithm>
#include <cmath>
#include <cstring>
#include <fstream>
#include <iterator>

// Same shading model as CutsceneScene (texture * vertex alpha * NLIT ambient + directional)
static const char* EP_VERT = R"(
#version 450 core
layout(location=0) in vec3  aPos;
layout(location=1) in vec3  aNormal;
layout(location=2) in vec2  aUV;
layout(location=3) in float aAlpha;
out vec3 Normal; out vec2 TexCoord; out float v_Alpha;
uniform mat4 model, view, projection;
uniform int u_UseVertexAlpha;
void main(){
    Normal = mat3(transpose(inverse(model))) * aNormal;
    TexCoord = aUV;
    v_Alpha = (u_UseVertexAlpha != 0) ? aAlpha : 1.0;
    gl_Position = projection * view * model * vec4(aPos, 1.0);
})";
static const char* EP_FRAG = R"(
#version 450 core
in vec3 Normal; in vec2 TexCoord; in float v_Alpha;
out vec4 FragColor;
uniform sampler2D u_Diffuse; uniform sampler2D u_AlphaMask;
uniform int u_UseTexture, u_HasAlphaMask;
uniform float u_AlphaThreshold;
uniform vec3 u_Ambient, u_DirColor, u_DirLight, objectColor;
void main(){
    vec4 tex = vec4(1.0);
    if (u_UseTexture != 0) { tex = texture(u_Diffuse, TexCoord); if (tex.a < u_AlphaThreshold) discard; }
    if (u_HasAlphaMask != 0 && texture(u_AlphaMask, TexCoord).r < 0.5) discard;
    vec3 base = (u_UseTexture != 0) ? tex.rgb : objectColor;
    vec3 N = normalize(Normal); if (!gl_FrontFacing) N = -N;
    float ndl = max(dot(N, normalize(u_DirLight)), 0.0);
    FragColor = vec4(base * (u_Ambient + u_DirColor * ndl), tex.a * v_Alpha);
})";

static const char* EP_VERT2D = R"(
#version 450 core
layout(location=0) in vec2 aPos; layout(location=1) in vec2 aUV;
out vec2 uv;
void main(){ uv = aUV; gl_Position = vec4(aPos, 0.0, 1.0); })";
static const char* EP_FRAG2D = R"(
#version 450 core
in vec2 uv; out vec4 FragColor;
uniform sampler2D u_Tex; uniform int u_UseTex; uniform vec4 u_Color;
void main(){ vec4 c = (u_UseTex != 0) ? texture(u_Tex, uv) : vec4(1.0); FragColor = c * u_Color; })";

static bool ReadAll(const std::string& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    out.assign(std::istreambuf_iterator<char>(f), {});
    return !out.empty();
}
static uint32_t BE32(const std::vector<uint8_t>& d, size_t o) {
    return o + 4 > d.size() ? 0 : ((uint32_t)d[o] << 24) | ((uint32_t)d[o + 1] << 16) | ((uint32_t)d[o + 2] << 8) | d[o + 3];
}
static std::string Stem(const std::string& p) {
    size_t s = p.find_last_of("/\\"); std::string f = s == std::string::npos ? p : p.substr(s + 1);
    size_t d = f.find('.'); return d == std::string::npos ? f : f.substr(0, d);
}
// Engine Euler: R = Rz * Ry * Rx (same as NBN2Parser::EulerXYZ)
static glm::mat3 Euler(float rx, float ry, float rz) {
    float cx = std::cos(rx), sx = std::sin(rx), cy = std::cos(ry), sy = std::sin(ry), cz = std::cos(rz), sz = std::sin(rz);
    glm::mat3 Rx(1, 0, 0, 0, cx, sx, 0, -sx, cx), Ry(cy, 0, -sy, 0, 1, 0, sy, 0, cy), Rz(cz, sz, 0, -sz, cz, 0, 0, 0, 1);
    return Rz * Ry * Rx;
}
static glm::mat4 TRS(const float* p, const float* r, const float* s) {
    glm::mat4 m(Euler(r[0], r[1], r[2]));
    m[0] *= s[0]; m[1] *= s[1]; m[2] *= s[2];
    m[3] = glm::vec4(p[0], p[1], p[2], 1.0f);
    return m;
}

EventPlayer::EventPlayer() {}
EventPlayer::~EventPlayer() {
    Clear();
    if (m_FBO) glDeleteFramebuffers(1, &m_FBO);
    if (m_FBOTex) glDeleteTextures(1, &m_FBOTex);
    if (m_FBODepth) glDeleteRenderbuffers(1, &m_FBODepth);
    delete m_Shader;
    delete m_Shader2D;
    if (m_QuadVAO) glDeleteVertexArrays(1, &m_QuadVAO);
    if (m_QuadVBO) glDeleteBuffers(1, &m_QuadVBO);
}

void EventPlayer::Clear() {
    for (auto& kv : m_Models) {
        for (auto& g : kv.second->meshes) {
            if (g.vao) glDeleteVertexArrays(1, &g.vao);
            if (g.vbo) glDeleteBuffers(1, &g.vbo);
            if (g.ebo) glDeleteBuffers(1, &g.ebo);
        }
        for (GLuint t : kv.second->owned) if (t) glDeleteTextures(1, &t);
    }
    for (auto& sl : m_Slides) for (GLuint t : sl.tex) if (t) glDeleteTextures(1, &t);
    m_Slides.clear();
    m_Models.clear(); m_Anims.clear(); m_Inst.clear(); m_Files.clear();
    for (GLuint t : m_Bank) if (t) glDeleteTextures(1, &t);
    m_Bank.clear(); m_BankBlend.clear(); m_CamActs.clear(); m_BlendFrom.clear();
    m_TL = es::EventTimeline(); m_Loaded = false; m_T = 0; m_Report.clear(); m_Log.clear();
}

bool EventPlayer::LoadBank(const std::string& path) {
    P3TexParser bank;
    if (!bank.Load(path)) return false;
    for (size_t i = 0; i < bank.GetTextureCount(); i++) {
        const P3Texture* tex = bank.GetTexture((uint8_t)i);
        GLuint id = 0; std::vector<uint8_t> rgba;
        if (tex && tex->width && tex->height) {
            rgba = ((tex->format & 0xDF) == 0x86) ? P3TexParser::DecompressDXT1(tex->data.data(), tex->width, tex->height)
                : P3TexParser::DecompressDXT5(tex->data.data(), tex->width, tex->height);
            if (!rgba.empty()) {
                glGenTextures(1, &id); glBindTexture(GL_TEXTURE_2D, id);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, tex->width, tex->height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
                glGenerateMipmap(GL_TEXTURE_2D);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR_MIPMAP_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);
            }
        }
        m_Bank.push_back(id);
        m_BankBlend.push_back(!rgba.empty() && NMDLLoader::NeedsAlphaBlend(rgba));
    }
    return true;
}

void EventPlayer::ScanNLIT(const std::vector<uint8_t>& fd) {
    // Ambient (type 0) + directional (type 1) colours, as CutsceneScene::ScanNLIT
    for (size_t sp = 0; sp + 8 <= fd.size(); sp += 4) {
        if (memcmp(fd.data() + sp, "NLIT", 4) != 0) continue;
        uint32_t nsz = BE32(fd, sp + 4); bool fa = false, fdir = false;
        for (size_t ep = sp + 8; ep + 48 <= sp + nsz && ep + 48 <= fd.size(); ep += 48) {
            const uint8_t* e = fd.data() + ep;
            if (e[16] == 0 && !fa) { m_Ambient = glm::vec3(e[20], e[21], e[22]) / 255.0f; fa = true; }
            else if (e[16] == 1 && !fdir) { m_DirColor = glm::vec3(e[20], e[21], e[22]) / 255.0f; fdir = true; }
        }
        return;
    }
}

bool EventPlayer::LoadModel(const std::vector<uint8_t>& fd, size_t off, bool use_bank, Model& out) {
    size_t nmdl = off;
    if (off + 4 <= fd.size() && memcmp(&fd[off], "NOBJ", 4) == 0) {
        uint32_t sz = BE32(fd, off + 4);
        nmdl = (size_t)-1;
        for (size_t p = off + 8; p + 8 <= off + sz && p + 8 <= fd.size(); ) {
            if (memcmp(&fd[p], "NMDL", 4) == 0) { nmdl = p; break; }
            uint32_t cs = BE32(fd, p + 4); if (cs < 8) break;
            p += cs; while (p & 3) p++;
        }
        if (nmdl == (size_t)-1) return false;
    }
    // Map NMDLs have no NTX3 of their own: they index the external .p3tex bank.
    uint32_t msz = BE32(fd, nmdl + 4);
    bool has_tex = false; size_t nbn2 = 0;
    for (size_t p = nmdl + 0x40; p + 8 <= nmdl + msz && p + 8 <= fd.size(); ) {
        if (!memcmp(&fd[p], "NTX3", 4) || !memcmp(&fd[p], "NTEX", 4) || !memcmp(&fd[p], "NTX2", 4)) has_tex = true;
        if (!memcmp(&fd[p], "NBN2", 4)) nbn2 = p;
        uint32_t cs = BE32(fd, p + 4); if (cs < 8) break;
        p += cs; while (p & 3) p++;
    }
    NMDLModel mdl;
    bool bank = use_bank && !has_tex && !m_Bank.empty();
    if (!NMDLLoader::Load(fd.data(), fd.size(), nmdl, mdl, bank ? &m_Bank : nullptr, bank ? &m_BankBlend : nullptr))
        return false;
    out.name = mdl.name;
    if (nbn2) NBN2Parser::Parse(fd.data() + nbn2, BE32(fd, nbn2 + 4), out.bones);
    for (auto& mesh : mdl.meshes) {
        if (mesh.vertices.empty() || mesh.draw_sequential) continue;
        GpuMesh g; g.sections = mesh.faceSections; g.vertex_alpha = mesh.has_vertex_alpha;
        g.skin = mesh.has_bones && !mesh.boneIDList.empty() && !out.bones.empty();
        glGenVertexArrays(1, &g.vao); glGenBuffers(1, &g.vbo); glBindVertexArray(g.vao);
        glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
        glBufferData(GL_ARRAY_BUFFER, mesh.vertices.size() * sizeof(Vertex), mesh.vertices.data(), g.skin ? GL_DYNAMIC_DRAW : GL_STATIC_DRAW);
        glVertexAttribPointer(0, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, position)); glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 3, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, normal));   glEnableVertexAttribArray(1);
        glVertexAttribPointer(2, 2, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, uv));       glEnableVertexAttribArray(2);
        glVertexAttribPointer(3, 1, GL_FLOAT, GL_FALSE, sizeof(Vertex), (void*)offsetof(Vertex, bone_weights)); glEnableVertexAttribArray(3);
        glGenBuffers(1, &g.ebo); glBindBuffer(GL_ELEMENT_ARRAY_BUFFER, g.ebo);
        glBufferData(GL_ELEMENT_ARRAY_BUFFER, mesh.indices.size() * sizeof(uint16_t), mesh.indices.data(), GL_STATIC_DRAW);
        glBindVertexArray(0);
        if (g.skin) { g.cpu = mesh; g.skinned = mesh.vertices; }
        out.meshes.push_back(std::move(g));
    }
    out.mat_to_tex = mdl.mat_to_tex; out.mat_to_alpha = mdl.mat_to_alpha;
    out.mat_clamp = mdl.mat_clamp_ids; out.mat_blend = mdl.mat_blend_ids; out.owned = mdl.owned_textures;
    // Bind pose helpers. NBN2Parser already gives world_pos / world_rot (rot pure, scale
    // folded into child offsets), which is what the inverse bind needs.
    size_t n = out.bones.size();
    out.pos_scale.resize(n); out.rot1.resize(n); out.inv_bind.resize(n);
    for (size_t i = 0; i < n; i++) {
        const Bone& b = out.bones[i];
        out.pos_scale[i] = (b.parent_idx >= 0 && b.parent_idx < (int)n) ? out.bones[b.parent_idx].world_scale : glm::vec3(1.0f);
        out.rot1[i] = Euler(b.rot_x, b.rot_y, b.rot_z);
        glm::mat4 w(b.world_rot); w[3] = glm::vec4(b.world_pos, 1.0f);
        out.inv_bind[i] = glm::inverse(w);
        out.by_name[b.name].push_back((int)i);
    }
    return !out.meshes.empty();
}

EventPlayer::Model* EventPlayer::GetModel(const std::string& key, const std::vector<uint8_t>* fd, size_t off, bool use_bank) {
    auto it = m_Models.find(key);
    if (it != m_Models.end()) return it->second.get();
    if (!fd) return nullptr;
    auto m = std::make_unique<Model>();
    if (!LoadModel(*fd, off, use_bank, *m)) { m_Report.push_back("model " + key + ": load failed"); return nullptr; }
    Model* raw = m.get(); m_Models[key] = std::move(m);
    return raw;
}

EventPlayer::Anim* EventPlayer::GetAnim(uint32_t off) {
    auto it = m_Anims.find(off);
    if (it != m_Anims.end()) return it->second.get();
    if (off + 0x30 > m_EvData.size() || memcmp(&m_EvData[off], "NMTN", 4) != 0) return nullptr;
    auto a = std::make_unique<Anim>();
    uint32_t sz = BE32(m_EvData, off + 4);
    if (!NMTNParser::Parse(m_EvData.data() + off, std::min<size_t>(sz, m_EvData.size() - off), a->a)) return nullptr;
    a->mult = (m_EvData[off + 0x29] & 1) ? 5 : 1;   // duration helpers multiply by 5 when bit 0 of +0x29
    Anim* raw = a.get(); m_Anims[off] = std::move(a);
    return raw;
}

bool EventPlayer::Load(const Assets& as) {
    Clear();
    m_A = as;
    if (!ReadAll(as.event_path, m_EvData) || !m_Ev.Load(m_EvData)) { m_Log = "cannot read event " + as.event_path; return false; }
    m_TL = es::BuildTimeline(m_Ev, as.lang);
    if (!as.lib_path.empty()) {
        std::vector<uint8_t> lib; es::Image L;
        if (ReadAll(as.lib_path, lib) && L.Load(lib)) for (auto& s : es::VerifyLib(L)) m_Report.push_back(s);
        else m_Report.push_back("lib.e not readable; export check skipped");
    }
    // Host map
    if (!as.map_path.empty() && ReadAll(as.map_path, m_MapData)) {
        es::Image M;
        if (M.Load(m_MapData)) {
            if (!as.p3tex_path.empty() && !LoadBank(as.p3tex_path)) m_Report.push_back("p3tex bank failed: " + as.p3tex_path);
            std::string stem = es::MapStemFromPath(as.map_path);
            uint32_t evno = 0, part = 0;
            es::EventNumberFromPath(as.event_path, evno, part);
            es::MapSetup ms = es::ExtractMapSetup(M, evno, stem);
            std::vector<es::MapObject> objs;
            for (auto& o : ms.objects) if (o.tag == "NOBJ" || o.tag == "NMDL") objs.push_back(o);
            bool has_default = false;
            for (auto& o : objs) if (o.ref == ms.default_map) has_default = true;
            if (!has_default && ms.default_map) { es::MapObject d; d.ref = ms.default_map; d.tag = "NOBJ"; d.name = stem; objs.insert(objs.begin(), d); }
            for (auto& o : objs) {
                Model* m = GetModel("map:" + std::to_string(o.ref), &m_MapData, o.ref, true);
                if (!m) continue;
                Instance in; in.model = m; in.is_static = true; in.static_xf = TRS(o.pos, o.rot, o.scale);
                m_Inst.push_back(in);
            }
            ScanNLIT(m_MapData);
            char b[160]; snprintf(b, sizeof(b), "map %s: %s branch for event %u, %zu static models",
                stem.c_str(), ms.found_branch ? "script" : "no", evno, objs.size());
            m_Report.push_back(b);
        }
    }
    // Event objects
    std::vector<uint8_t>* ak = nullptr;
    if (!as.appkeep2_path.empty() && ReadAll(as.appkeep2_path, m_AkData)) ak = &m_AkData;
    for (auto& kv : m_TL.objects) {
        const es::EvObject& o = kv.second;
        Model* m = nullptr;
        if (o.kind == es::ObjKind::Model) m = GetModel("ev:" + std::to_string(o.ref), &m_EvData, o.ref, false);
        else if (o.kind == es::ObjKind::Actor) {
            static const char* party[] = { "pcalg_v1.p3obj", "pcplk_v1.p3obj", "pcbet_v1.p3obj" };
            if (o.char_id <= -10 && o.char_id >= -12) {
                std::string p = as.party_dir + "/" + party[-10 - o.char_id];
                auto& fd = m_Files[p];
                if (fd.empty()) ReadAll(p, fd);
                if (!fd.empty()) m = GetModel(p, &fd, 0, false);
                if (!m) m_Report.push_back(std::string("actor ") + std::to_string(o.id) + ": missing " + party[-10 - o.char_id]);
            }
            else if (o.char_id <= -20 && o.char_id >= -28 && ak) {
                int e = -20 - o.char_id;
                uint32_t off = BE32(*ak, 12 + 4 * e);
                m = GetModel("ak2:" + std::to_string(e), ak, off, false);
            }
            else m_Report.push_back("actor " + std::to_string(o.id) + ": id " + std::to_string(o.char_id) + " not served (AppKeep slot or missing appkeep2)");
        }
        if (o.kind == es::ObjKind::Effect) {
            Slide sl;
            if (LoadSlide(o.id, o.ref, sl)) m_Slides.push_back(std::move(sl));
            continue;
        }
        if (!m) continue;
        Instance in; in.id = o.id; in.model = m; m_Inst.push_back(in);
    }
    if (!m_Slides.empty()) m_Report.push_back(std::to_string(m_Slides.size()) + " screen layers (Mefc pictures)");
    for (size_t i = 0; i < m_TL.records.size(); i++)
        if (m_TL.records[i].type == es::EvRecord::CamActivate) m_CamActs.push_back(i);
    for (auto& kv : m_TL.unresolved) m_Report.push_back("unmapped call " + kv.first + " x" + std::to_string(kv.second));
    m_Loaded = true;
    m_Log = "loaded " + Stem(as.event_path);
    SetTime(0.0);
    // Free camera starts where the first shot looks
    View v = ViewAt(0.0);
    glm::vec3 dir = v.tgt - v.eye;
    if (glm::length(dir) > 1e-4f) {
        dir = glm::normalize(dir);
        m_FreeCam.SetFromMapShot(v.eye, std::atan2(dir.x, dir.z), std::asin(glm::clamp(dir.y, -1.0f, 1.0f)));
    }
    return true;
}

void EventPlayer::SetTime(double t) { m_T = std::max(0.0, std::min(t, m_TL.duration)); }

// ---------------------------------------------------------------------------------
// Evaluation

glm::mat4 EventPlayer::ObjectMatrix(uint16_t id, double t, bool& alive) const {
    float p[3] = { 0,0,0 }, r[3] = { 0,0,0 }, s[3] = { 1,1,1 };
    alive = true;
    auto oit = m_TL.objects.find(id);
    if (oit != m_TL.objects.end() && oit->second.t_create > t + 1e-6) alive = false;
    const es::EvRecord* mv = nullptr; float from[3] = { 0,0,0 };
    for (const auto& rc : m_TL.records) {
        if (rc.t > t + 1e-9) break;
        if (rc.obj != id) continue;
        switch (rc.type) {
        case es::EvRecord::Pos: memcpy(p, rc.v, 12); mv = nullptr; break;
        case es::EvRecord::Rot: memcpy(r, rc.v, 12); break;
        case es::EvRecord::Scale: memcpy(s, rc.v, 12); break;
        case es::EvRecord::Delete: alive = false; break;
        case es::EvRecord::MoveTo: memcpy(from, p, 12); mv = &rc; break;
        default: break;
        }
    }
    if (mv) {   // lib.e sym76 -> 1108 cmd 12: move to a point over a time (linear here)
        float k = mv->blend > 0 ? (float)std::min(1.0, (t - mv->t) / mv->blend) : 1.0f;
        for (int i = 0; i < 3; i++) p[i] = from[i] + (mv->v[i] - from[i]) * k;
    }
    return TRS(p, r, s);
}

static int LayerOf(const std::string& n) {
    if (n.rfind("f_", 0) == 0) return 3;
    if (n.rfind("hr_", 0) == 0) return 1;
    if (n.rfind("hl_", 0) == 0) return 2;
    return 0;
}

void EventPlayer::PoseInstance(Instance& in, double t) {
    Model& M = *in.model;
    size_t n = M.bones.size();
    if (!n) return;
    std::vector<glm::vec3> lp(n); std::vector<glm::quat> lr(n);
    for (size_t i = 0; i < n; i++) {
        const Bone& b = M.bones[i];
        lp[i] = glm::vec3(b.local_x, b.local_y, b.local_z) * M.pos_scale[i];
        lr[i] = glm::quat_cast(M.rot1[i] * Euler(b.rot2_x, b.rot2_y, b.rot2_z));
    }
    // Motion layers: body, right hand, left hand, face. A new play on a layer blends from
    // the previous one over its blend ticks (4th argument of sym20060).
    const es::EvRecord* cur[4] = {}, * prev[4] = {};
    for (const auto& rc : m_TL.records) {
        if (rc.t > t + 1e-9) break;
        if (rc.obj != in.id || rc.type != es::EvRecord::Motion || !rc.nmtn) continue;
        int L = LayerOf(rc.str); prev[L] = cur[L]; cur[L] = &rc;
    }
    auto sample = [&](const es::EvRecord* rc, std::vector<std::pair<int, std::pair<glm::vec3, glm::quat>>>& out) {
        out.clear(); if (!rc) return;
        Anim* A = GetAnim(rc->nmtn); if (!A) return;
        auto& map = A->track_to_bone[&M];
        if (map.empty()) {   // Nth track named X drives the Nth bone named X
            std::unordered_map<std::string, int> seen;
            for (auto& tr : A->a.tracks) {
                auto it = M.by_name.find(tr.bone_name); int bi = -1;
                if (it != M.by_name.end()) { int k = seen[tr.bone_name]++; if (k < (int)it->second.size()) bi = it->second[k]; }
                map.push_back(bi);
            }
        }
        double ticks = rc->start_ticks + (t - rc->t) * 300.0 * rc->speed;
        float f = (float)std::max(0.0, std::min<double>(A->a.frame_count, ticks / A->mult));
        for (size_t k = 0; k < A->a.tracks.size(); k++) {
            int bi = map[k]; if (bi < 0) continue;
            const NMTNTrack& tr = A->a.tracks[k]; const Bone& b = M.bones[bi];
            float P[3] = { b.local_x, b.local_y, b.local_z }, R[3] = { b.rot2_x, b.rot2_y, b.rot2_z };
            bool any = false;
            for (int c = 0; c < 3; c++) {
                if (tr.HasChannel(c)) { P[c] = tr.channel[c].Sample(f); any = true; }
                else if (tr.HasStatic(c)) { P[c] = tr.static_val[c]; any = true; }
                if (tr.HasChannel(3 + c)) { R[c] = tr.channel[3 + c].Sample(f); any = true; }
                else if (tr.HasStatic(3 + c)) { R[c] = tr.static_val[3 + c]; any = true; }
            }
            if (!any) continue;
            out.push_back({ bi, { glm::vec3(P[0], P[1], P[2]) * M.pos_scale[bi], glm::quat_cast(M.rot1[bi] * Euler(R[0], R[1], R[2])) } });
        }
        };
    std::vector<std::pair<int, std::pair<glm::vec3, glm::quat>>> A, B;
    for (int L = 0; L < 4; L++) {
        if (!cur[L]) continue;
        sample(cur[L], B);
        float w = cur[L]->blend_ticks > 0 ? (float)std::min(1.0, (t - cur[L]->t) * 300.0 / cur[L]->blend_ticks) : 1.0f;
        if (w < 1.0f) {
            sample(prev[L], A);
            std::unordered_map<int, size_t> ai; for (size_t i = 0; i < A.size(); i++) ai[A[i].first] = i;
            for (auto& e : B) {
                auto it = ai.find(e.first);
                glm::vec3 p0 = it != ai.end() ? A[it->second].second.first : lp[e.first];
                glm::quat q0 = it != ai.end() ? A[it->second].second.second : lr[e.first];
                lp[e.first] = glm::mix(p0, e.second.first, w);
                lr[e.first] = glm::slerp(q0, e.second.second, w);
            }
        }
        else for (auto& e : B) { lp[e.first] = e.second.first; lr[e.first] = e.second.second; }
    }
    in.pose.resize(n);
    for (size_t i = 0; i < n; i++) {
        glm::mat4 l = glm::mat4_cast(lr[i]); l[3] = glm::vec4(lp[i], 1.0f);
        int par = M.bones[i].parent_idx;
        in.pose[i] = (par >= 0 && par < (int)i) ? in.pose[par] * l : l;
    }
}

EventPlayer::View EventPlayer::CamStateView(uint16_t cam, double t) const {
    View v; v.cam = cam; float focal = 35.0f;
    for (const auto& rc : m_TL.records) {
        if (rc.t > t + 1e-9) break;
        if (rc.obj != cam) continue;
        if (rc.type == es::EvRecord::Pos) v.eye = glm::vec3(rc.v[0], rc.v[1], rc.v[2]);
        else if (rc.type == es::EvRecord::Target) v.tgt = glm::vec3(rc.v[0], rc.v[1], rc.v[2]);
        else if (rc.type == es::EvRecord::CamParam && rc.idx == 2 && rc.v[0] > 0) focal = rc.v[0];
    }
    // 1025 index 2 is a focal length on a 24 mm high frame: fovV = 2 atan(12 / f)
    v.fov_deg = glm::degrees(2.0f * std::atan(12.0f / focal));
    return v;
}

EventPlayer::View EventPlayer::ViewAt(double t) const {
    int ai = -1;
    for (size_t i = 0; i < m_CamActs.size(); i++) if (m_TL.records[m_CamActs[i]].t <= t + 1e-6) ai = (int)i;
    return ViewAtAct(ai, t);
}

// View of activation ai at time t. A blend starts from what activation ai-1 showed at
// that moment, so two activations in the same frame (cut, then blend away: the e1180
// opening crane) still blend from the first one.
EventPlayer::View EventPlayer::ViewAtAct(int ai, double t) const {
    if (ai < 0) return View();
    const es::EvRecord& a = m_TL.records[m_CamActs[ai]];
    View v = CamStateView(a.obj, t); v.act = ai;
    // 1023(cam, seconds, curve): blend from the view at activation (x300 + 0.1 into ticks
    // on the 360). The curve argument is not modelled: smoothstep.
    if (a.blend > 0 && t < a.t + a.blend) {
        auto it = m_BlendFrom.find(ai);
        if (it == m_BlendFrom.end()) it = m_BlendFrom.emplace(ai, ai > 0 ? ViewAtAct(ai - 1, a.t) : CamStateView(a.obj, a.t)).first;
        const View& f = it->second;
        float k = (float)((t - a.t) / a.blend); k = k * k * (3 - 2 * k);
        v.eye = glm::mix(f.eye, v.eye, k); v.tgt = glm::mix(f.tgt, v.tgt, k); v.fov_deg = glm::mix(f.fov_deg, v.fov_deg, k);
        v.blend = k;
    }
    return v;
}

float EventPlayer::FadeAlpha() const {
    float a = 0.0f; const es::EvRecord* cur = nullptr; float from = 0.0f;
    for (const auto& rc : m_TL.records) {
        if (rc.t > m_T + 1e-9) break;
        if (rc.type == es::EvRecord::FadeSet) { a = ((rc.color >> 24) & 0xFF) / 255.0f; cur = nullptr; }
        else if (rc.type == es::EvRecord::FadeIn || rc.type == es::EvRecord::FadeOut) { from = a; cur = &rc; }
    }
    if (cur) {
        float k = cur->blend > 0 ? (float)std::min(1.0, (m_T - cur->t) / cur->blend) : 1.0f;
        a = (cur->type == es::EvRecord::FadeIn) ? from * (1 - k) : from + (1 - from) * k;
    }
    return a;
}

std::string EventPlayer::Subtitle() const {
    for (const auto& rc : m_TL.records) {
        if (rc.type != es::EvRecord::Text) continue;
        double dur = 3.0; size_t w = rc.str.find("<w");
        if (w != std::string::npos && isdigit((unsigned char)rc.str[w + 2])) dur = atoi(rc.str.c_str() + w + 2) / 1000.0;
        if (m_T >= rc.t && m_T < rc.t + dur) {
            // Western text is single-byte (Latin-1 range); ImGui wants UTF-8. <#N> is a
            // literal character code (<#201> = É), \n a two-character line break.
            auto put = [](std::string& o, unsigned cp) {
                if (cp < 0x80) o += (char)cp;
                else { o += (char)(0xC0 | (cp >> 6)); o += (char)(0x80 | (cp & 0x3F)); }
                };
            std::string s, in = rc.str;
            for (size_t i = 0; i < in.size(); i++) {
                if (in[i] == '<') {
                    size_t e = in.find('>', i); if (e == std::string::npos) break;
                    if (in[i + 1] == '#') put(s, (unsigned)atoi(in.c_str() + i + 2) & 0xFF);
                    i = e; continue;
                }
                if (in[i] == '\\' && i + 1 < in.size() && in[i + 1] == 'n') { s += '\n'; i++; continue; }
                put(s, (unsigned char)in[i]);
            }
            return s;
        }
    }
    return "";
}

std::string EventPlayer::CameraInfo() const {
    View v = ViewAt(m_T); char b[128];
    if (v.act < 0) return "no active camera";
    snprintf(b, sizeof(b), "cam %u  f=%.0fmm  fov %.1f%s", v.cam, 12.0f / std::tan(glm::radians(v.fov_deg) / 2), v.fov_deg,
        v.blend >= 0 ? "  (blending)" : "");
    return b;
}

// ---------------------------------------------------------------------------------
// Rendering

void EventPlayer::EnsureGL(int w, int h) {
    if (!m_Shader) { m_Shader = new Shader(); m_Shader->CompileFromSource(EP_VERT, EP_FRAG); }
    if (m_FBO && m_W == w && m_H == h) return;
    if (m_FBO) { glDeleteFramebuffers(1, &m_FBO); glDeleteTextures(1, &m_FBOTex); glDeleteRenderbuffers(1, &m_FBODepth); }
    glGenFramebuffers(1, &m_FBO); glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glGenTextures(1, &m_FBOTex); glBindTexture(GL_TEXTURE_2D, m_FBOTex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, m_FBOTex, 0);
    glGenRenderbuffers(1, &m_FBODepth); glBindRenderbuffer(GL_RENDERBUFFER, m_FBODepth);
    glRenderbufferStorage(GL_RENDERBUFFER, GL_DEPTH24_STENCIL8, w, h);
    glFramebufferRenderbuffer(GL_FRAMEBUFFER, GL_DEPTH_STENCIL_ATTACHMENT, GL_RENDERBUFFER, m_FBODepth);
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
    m_W = w; m_H = h;
}

void EventPlayer::DrawModel(const Model& m, const glm::mat4& xf) {
    m_Shader->SetMat4("model", xf);
    for (int pass = 0; pass < 2; pass++) {
        for (const auto& g : m.meshes) {
            if ((pass == 1) != g.vertex_alpha) continue;
            glBindVertexArray(g.vao);
            m_Shader->SetInt("u_UseVertexAlpha", g.vertex_alpha ? 1 : 0);
            if (g.vertex_alpha) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_FALSE); }
            for (const auto& s : g.sections) {
                if (!s.index_count) continue;
                auto it = m.mat_to_tex.find(s.mat_id);
                if (it != m.mat_to_tex.end() && it->second) {
                    glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, it->second);
                    GLenum wrap = m.mat_clamp.count(s.mat_id) ? GL_CLAMP_TO_EDGE : GL_REPEAT;
                    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, wrap); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, wrap);
                    m_Shader->SetInt("u_UseTexture", 1);
                    if (m.mat_blend.count(s.mat_id)) { glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA); glDepthMask(GL_FALSE); m_Shader->SetFloat("u_AlphaThreshold", 0.01f); }
                    else { if (!g.vertex_alpha) { glDisable(GL_BLEND); glDepthMask(GL_TRUE); } m_Shader->SetFloat("u_AlphaThreshold", 0.5f); }
                }
                else { m_Shader->SetInt("u_UseTexture", 0); m_Shader->SetFloat("u_AlphaThreshold", 0.5f); }
                auto ia = m.mat_to_alpha.find(s.mat_id);
                if (ia != m.mat_to_alpha.end() && ia->second) { glActiveTexture(GL_TEXTURE1); glBindTexture(GL_TEXTURE_2D, ia->second); m_Shader->SetInt("u_HasAlphaMask", 1); }
                else m_Shader->SetInt("u_HasAlphaMask", 0);
                glDrawElements(GL_TRIANGLES, s.index_count, GL_UNSIGNED_SHORT, (void*)(uintptr_t)(s.index_start * sizeof(uint16_t)));
            }
            glDisable(GL_BLEND); glDepthMask(GL_TRUE);
        }
    }
    glBindVertexArray(0);
}

void EventPlayer::Render(int w, int h, bool free_cam) {
    if (w <= 0 || h <= 0) return;
    EnsureGL(w, h);
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glViewport(0, 0, w, h);
    glEnable(GL_DEPTH_TEST);
    glClearColor(0, 0, 0, 1);
    glClear(GL_COLOR_BUFFER_BIT | GL_DEPTH_BUFFER_BIT);
    if (!m_Loaded) { glBindFramebuffer(GL_FRAMEBUFFER, 0); return; }
    m_Shader->Use();
    glm::mat4 view, proj;
    if (free_cam) { view = m_FreeCam.GetViewMatrix(); proj = m_FreeCam.GetProjectionMatrix((float)w / h); }
    else {
        View v = ViewAt(m_T);
        view = glm::lookAt(v.eye, v.tgt, glm::vec3(0, 1, 0));
        proj = glm::perspective(glm::radians(v.fov_deg), (float)w / h, 0.1f, 4000.0f);
    }
    m_Shader->SetMat4("view", view); m_Shader->SetMat4("projection", proj);
    m_Shader->SetInt("u_Diffuse", 0); m_Shader->SetInt("u_AlphaMask", 1);
    m_Shader->SetVec3("u_Ambient", m_Ambient); m_Shader->SetVec3("u_DirColor", m_DirColor);
    m_Shader->SetVec3("u_DirLight", m_DirLight); m_Shader->SetVec3("objectColor", glm::vec3(0.7f));

    // Object matrices first: bone attachments (sym20058) read their parent's pose.
    std::map<uint16_t, Instance*> byId;
    for (auto& in : m_Inst) if (!in.is_static) byId[in.id] = &in;
    // Map statics take the map's NLIT colours; actors get a neutral key light (the game
    // lights them with their own linked lights, sym20056/20057, which are not modelled).
    for (auto& in : m_Inst) {
        if (in.is_static) {
            m_Shader->SetVec3("u_Ambient", m_Ambient); m_Shader->SetVec3("u_DirColor", m_DirColor);
            DrawModel(*in.model, in.static_xf); continue;
        }
        m_Shader->SetVec3("u_Ambient", glm::vec3(0.62f)); m_Shader->SetVec3("u_DirColor", glm::vec3(0.45f));
        bool alive = true;
        glm::mat4 xf = ObjectMatrix(in.id, m_T, alive);
        if (!alive) continue;
        for (const auto& rc : m_TL.records) {
            if (rc.t > m_T + 1e-9) break;
            if (rc.type != es::EvRecord::AttachBone || rc.obj != in.id) continue;
            auto p = byId.find(rc.other);
            if (p == byId.end()) continue;
            bool pa = true; glm::mat4 pxf = ObjectMatrix(rc.other, m_T, pa);
            PoseInstance(*p->second, m_T);
            if (rc.idx >= 0 && rc.idx < (int)p->second->pose.size())
                xf = pxf * p->second->pose[rc.idx] * TRS(rc.v, rc.v + 3, std::vector<float>{ 1, 1, 1 }.data());
        }
        Model& M = *in.model;
        if (!M.bones.empty()) {
            PoseInstance(in, m_T);
            std::vector<glm::mat4> skin(M.bones.size());
            for (size_t i = 0; i < skin.size(); i++) skin[i] = in.pose[i] * M.inv_bind[i];
            for (auto& g : M.meshes) {
                if (!g.skin) continue;
                const auto& src = g.cpu.vertices; auto& dst = g.skinned;
                for (size_t v = 0; v < src.size(); v++) {
                    glm::mat4 S(0.0f); float ws = 0;
                    for (int k = 0; k < 4; k++) {
                        float wk = src[v].bone_weights[k]; if (wk <= 0) continue;
                        uint8_t li = src[v].bone_ids[k];
                        if (li >= g.cpu.boneIDList.size()) continue;
                        uint16_t gi = g.cpu.boneIDList[li];
                        if (gi >= skin.size()) continue;
                        S += skin[gi] * wk; ws += wk;
                    }
                    if (ws <= 0) S = glm::mat4(1.0f);
                    glm::vec4 p = S * glm::vec4(src[v].position[0], src[v].position[1], src[v].position[2], 1.0f);
                    glm::vec3 nn = glm::mat3(S) * glm::vec3(src[v].normal[0], src[v].normal[1], src[v].normal[2]);
                    dst[v].position[0] = p.x; dst[v].position[1] = p.y; dst[v].position[2] = p.z;
                    dst[v].normal[0] = nn.x; dst[v].normal[1] = nn.y; dst[v].normal[2] = nn.z;
                }
                glBindBuffer(GL_ARRAY_BUFFER, g.vbo);
                glBufferSubData(GL_ARRAY_BUFFER, 0, dst.size() * sizeof(Vertex), dst.data());
            }
        }
        DrawModel(M, xf);
    }
    DrawSlides();
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}

// ---------------------------------------------------------------------------------
// Slideshow layers. A Mefc is a directory of sections ("CK" entries: tag, type, offset
// from the Mefc): TEXn = NTX3 pictures, TRC0 = UV rect (u, v, w, h, pivot), EFCT = etbl,
// MLNG = per-language picture choice. Chapter-end slideshows (e2010_010) are a stack of
// such pictures: the script creates the next one, shows it (1028 b=0), hides the
// previous one (1028 b=1) right after a sym20038 screen-layer fade, and deletes the one
// before that.
// Only pictures 1280 texels wide (full screen) and localized strips (MLNG) are drawn;
// 3D effects are skipped.
bool EventPlayer::LoadSlide(uint16_t id, uint32_t mefc, Slide& out) {
    const auto& d = m_EvData;
    if (mefc + 0x10 > d.size() || memcmp(&d[mefc], "Mefc", 4) != 0) return false;
    uint32_t dir = mefc + ((d[mefc + 0x0C] << 8) | d[mefc + 0x0D]);
    if (dir + 4 > d.size() || d[dir] != 'C' || d[dir + 1] != 'K') return false;
    int esz = d[dir + 2], n = d[dir + 3];
    bool mlng = false; uint32_t etbl = 0, trc = 0;
    std::vector<uint32_t> texs;
    for (int i = 0; i < n; i++) {
        uint32_t e = dir + 4 + i * esz;
        std::string tag((const char*)&d[e], 4);
        uint32_t off = mefc + BE32(d, e + 8);
        if (tag.compare(0, 3, "TEX") == 0 && off + 0x80 <= d.size() && !memcmp(&d[off], "NTX3", 4)) texs.push_back(off);
        else if (tag == "TRC0") trc = off;
        else if (tag == "EFCT") etbl = off;
        else if (tag == "MLNG") mlng = true;
    }
    if (texs.empty()) return false;
    for (uint32_t t : texs) {
        uint8_t fmt = d[t + 0x18];
        int w = (d[t + 0x20] << 8) | d[t + 0x21], h = (d[t + 0x22] << 8) | d[t + 0x23];
        uint32_t data = t + BE32(d, t + 0x10);
        std::vector<uint8_t> rgba;
        // Standard BC1/BC3 block order (PIL/DDS decode these pictures correctly).
        // P3TexParser::DecompressDXT5 swaps the two block halves, which garbles them.
        if (((fmt & 0xDF) == 0x86 || (fmt & 0xDF) == 0x88) && w % 4 == 0 && h % 4 == 0) {
            bool bc3 = (fmt & 0xDF) == 0x88; size_t bs = bc3 ? 16 : 8;
            if (data + (size_t)(w / 4) * (h / 4) * bs <= d.size()) {
                rgba.resize((size_t)w * h * 4);
                for (int by = 0; by < h / 4; by++) for (int bx = 0; bx < w / 4; bx++) {
                    const uint8_t* src = &d[data + ((size_t)by * (w / 4) + bx) * bs];
                    uint8_t* dst = &rgba[((size_t)by * 4 * w + bx * 4) * 4];
                    if (bc3) bcdec_bc3(src, dst, w * 4); else bcdec_bc1(src, dst, w * 4);
                }
            }
        }
        else if (fmt == 0xA5 && data + (size_t)w * h * 4 <= d.size()) {   // A8R8G8B8, rows at the pitch
            uint32_t pitch = BE32(d, t + 0x28); if (!pitch) pitch = w * 4;
            rgba.resize((size_t)w * h * 4);
            for (int y = 0; y < h; y++) for (int x = 0; x < w; x++) {
                const uint8_t* p = &d[data + (size_t)y * pitch + x * 4];
                uint8_t* q = &rgba[((size_t)y * w + x) * 4]; q[0] = p[1]; q[1] = p[2]; q[2] = p[3]; q[3] = p[0];
            }
        }
        GLuint id2 = 0;
        if (!rgba.empty()) {
            glGenTextures(1, &id2); glBindTexture(GL_TEXTURE_2D, id2);
            glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
            glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE); glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
        }
        out.tex.push_back(id2); out.size.push_back({ w, h });
    }
    out.id = id;
    out.fullscreen = out.size[0].x >= 1024;
    if (!out.fullscreen && !mlng) { for (GLuint t : out.tex) if (t) glDeleteTextures(1, &t); return false; }
    if (trc) for (int k = 0; k < 4; k++) { uint32_t u = BE32(d, trc + 4 * k); float f; memcpy(&f, &u, 4); out.uv[k] = f; }
    // etbl alpha keys: (u16 frame, u16 alpha 0..255) pairs from +0xA0 while frames increase.
    // Read as frames at 60 fps from the object's creation (an inference, not traced).
    if (etbl && !memcmp(&d[etbl], "etbl", 4)) {
        int last = -1;
        for (uint32_t p = etbl + 0xA0; p + 4 <= d.size() && p < etbl + 0x200; p += 4) {
            int fr = (d[p] << 8) | d[p + 1], al = (d[p + 2] << 8) | d[p + 3];
            if (fr <= last || al > 255) break;
            out.alpha.push_back({ (float)fr, al / 255.0f }); last = fr;
        }
    }
    return true;
}

void EventPlayer::DrawQuad(float x0, float y0, float x1, float y1, const float* uv, GLuint tex, glm::vec4 color) {
    float u0 = uv ? uv[0] : 0, v0 = uv ? uv[1] : 0, u1 = uv ? uv[0] + uv[2] : 1, v1 = uv ? uv[1] + uv[3] : 1;
    // NDC, y up; texture rows top-down
    float q[] = { x0, y0, u0, v1,  x1, y0, u1, v1,  x1, y1, u1, v0,  x0, y0, u0, v1,  x1, y1, u1, v0,  x0, y1, u0, v0 };
    glBindBuffer(GL_ARRAY_BUFFER, m_QuadVBO);
    glBufferData(GL_ARRAY_BUFFER, sizeof(q), q, GL_DYNAMIC_DRAW);
    m_Shader2D->SetInt("u_UseTex", tex ? 1 : 0);
    m_Shader2D->SetVec4("u_Color", color);
    if (tex) { glActiveTexture(GL_TEXTURE0); glBindTexture(GL_TEXTURE_2D, tex); }
    glBindVertexArray(m_QuadVAO);
    glDrawArrays(GL_TRIANGLES, 0, 6);
}

void EventPlayer::DrawSlides() {
    if (m_Slides.empty()) return;
    if (!m_Shader2D) {
        m_Shader2D = new Shader(); m_Shader2D->CompileFromSource(EP_VERT2D, EP_FRAG2D);
        glGenVertexArrays(1, &m_QuadVAO); glGenBuffers(1, &m_QuadVBO);
        glBindVertexArray(m_QuadVAO); glBindBuffer(GL_ARRAY_BUFFER, m_QuadVBO);
        glBufferData(GL_ARRAY_BUFFER, 24 * sizeof(float), nullptr, GL_DYNAMIC_DRAW);
        glVertexAttribPointer(0, 2, GL_FLOAT, GL_FALSE, 16, (void*)0); glEnableVertexAttribArray(0);
        glVertexAttribPointer(1, 2, GL_FLOAT, GL_FALSE, 16, (void*)8); glEnableVertexAttribArray(1);
        glBindVertexArray(0);
    }
    m_Shader2D->Use(); m_Shader2D->SetInt("u_Tex", 0);
    glDisable(GL_DEPTH_TEST); glEnable(GL_BLEND); glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    struct Vis { const Slide* s; float a; int prio; double born; };
    std::vector<Vis> vis;
    for (const Slide& sl : m_Slides) {
        auto o = m_TL.objects.find(sl.id);
        double born = o != m_TL.objects.end() ? o->second.t_create : 0.0;
        if (born > m_T + 1e-9) continue;
        bool alive = true, hidden = false; int prio = 0;
        for (auto& r : m_TL.records) {
            if (r.t > m_T + 1e-9) break;
            if (r.obj != sl.id) continue;
            if (r.type == es::EvRecord::Delete) alive = false;
            if (r.type == es::EvRecord::RenderFlag && r.idx == 1028) hidden = r.v[0] != 0;
            if (r.type == es::EvRecord::RenderFlag && r.idx == 1097) prio = (int)r.v[0];
        }
        if (!alive || hidden) continue;
        float a = 1.0f;
        if (!sl.alpha.empty()) {
            float f = (float)((m_T - born) * 60.0);
            const auto& k = sl.alpha;
            if (f <= k.front().x) a = k.front().y;
            else if (f >= k.back().x) a = k.back().y;
            else for (size_t i = 0; i + 1 < k.size(); i++)
                if (f >= k[i].x && f <= k[i + 1].x) { a = k[i].y + (k[i + 1].y - k[i].y) * (f - k[i].x) / std::max(1.0f, k[i + 1].x - k[i].x); break; }
        }
        if (a > 0.001f) vis.push_back({ &sl, a, prio, born });
    }
    // Higher 1097 priority on top; among equals the earlier picture covers the later one
    // (e2010_010 opens on a painting that dissolves into the photo created with it).
    std::sort(vis.begin(), vis.end(), [](const Vis& x, const Vis& y) {
        if (x.prio != y.prio) return x.prio < y.prio;
        if (x.born != y.born) return x.born > y.born;
        return x.s->id > y.s->id; });   // same frame: the one created first (lower id) on top
    static const char* pal[] = { "GBR", "FRA", "ITA", "DEU", "ESP" };
    int lang_tex = 0;
    for (int i = 0; i < 5; i++) if (m_A.lang == pal[i]) lang_tex = i;
    for (const Vis& v : vis) {
        const Slide& sl = *v.s;
        if (sl.fullscreen) DrawQuad(-1, -1, 1, 1, sl.uv, sl.tex[0], glm::vec4(1, 1, 1, v.a));
        else {
            // localized title strip, drawn at 2x texel size, centred, upper third (approximate)
            int ti = std::min<int>(lang_tex, (int)sl.tex.size() - 1);
            float w = sl.size[ti].x * 2.0f / 1280.0f * 2.0f, h = sl.size[ti].y * 2.0f / 720.0f * 2.0f;
            DrawQuad(-w / 2, 0.30f - h / 2, w / 2, 0.30f + h / 2, sl.uv, sl.tex[ti], glm::vec4(1, 1, 1, v.a));
        }
    }
    // sym20038 (Flash records) is not drawn: it fades screen layer 1 from its current
    // colour (-1) to 0x00FFFFFF, and nothing in the event sets layer 1 beforehand, so its
    // visible effect cannot be told from the script alone.
    glDisable(GL_BLEND); glEnable(GL_DEPTH_TEST);
}

void EventPlayer::ReadPixels(std::vector<uint8_t>& out) const {
    out.resize((size_t)m_W * m_H * 4);
    glBindFramebuffer(GL_FRAMEBUFFER, m_FBO);
    glReadPixels(0, 0, m_W, m_H, GL_RGBA, GL_UNSIGNED_BYTE, out.data());
    glBindFramebuffer(GL_FRAMEBUFFER, 0);
}
