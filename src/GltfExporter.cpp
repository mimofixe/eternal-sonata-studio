#include "GltfExporter.h"

#include <glad/glad.h>
#include <glm/glm.hpp>
#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

#include <algorithm>
#include <cfloat>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <unordered_map>

// STB_IMAGE_WRITE_IMPLEMENTATION lives in P3texviewer.cpp; defining it again
// here would duplicate every symbol at link time.
#include "../libs/stb/stb_image_write.h"

namespace {

namespace fs = std::filesystem;

// glTF component types
constexpr int GL_CT_UNSIGNED_SHORT = 5123;
constexpr int GL_CT_FLOAT = 5126;
// glTF buffer view targets
constexpr int TARGET_ARRAY_BUFFER = 34962;
constexpr int TARGET_ELEMENT_ARRAY = 34963;

// One accessor's worth of bookkeeping while the buffer is being assembled.
struct Accessor {
    int         buffer_view = 0;
    int         component_type = GL_CT_FLOAT;
    size_t      count = 0;
    std::string type;                 // "SCALAR" / "VEC2" / "VEC3" / "VEC4" / "MAT4"
    bool        has_bounds = false;
    float       min_v[3]{}, max_v[3]{};
};

struct BufferView {
    size_t offset = 0;
    size_t length = 0;
    int    target = 0;                // 0 = omit (glTF forbids a target on IBM data)
    int    byte_stride = 0;           // 0 = omit
};

// Builds the .bin blob and the parallel bufferView/accessor tables.
class BufferBuilder {
public:
    std::vector<uint8_t>    bin;
    std::vector<BufferView> views;
    std::vector<Accessor>   accessors;

    // Accessor offsets must be 4-byte aligned; pad between views rather than
    // relying on every payload happening to land on a multiple of four (u16
    // index runs of odd length do not).
    void Align() { while (bin.size() % 4) bin.push_back(0); }

    int AddView(const void* data, size_t bytes, int target, int stride) {
        Align();
        BufferView v;
        v.offset = bin.size();
        v.length = bytes;
        v.target = target;
        v.byte_stride = stride;
        const uint8_t* p = static_cast<const uint8_t*>(data);
        bin.insert(bin.end(), p, p + bytes);
        views.push_back(v);
        return static_cast<int>(views.size() - 1);
    }

    int AddAccessor(const Accessor& a) {
        accessors.push_back(a);
        return static_cast<int>(accessors.size() - 1);
    }
};

// JSON string escaping. Chunk names come straight out of the file and are not
// guaranteed to be clean ASCII, so anything outside the printable range is
// emitted as \u00XX rather than written raw into the JSON.
std::string JStr(const std::string& s) {
    std::string o = "\"";
    for (unsigned char c : s) {
        switch (c) {
        case '"':  o += "\\\""; break;
        case '\\': o += "\\\\"; break;
        case '\n': o += "\\n";  break;
        case '\r': o += "\\r";  break;
        case '\t': o += "\\t";  break;
        default:
            if (c < 0x20 || c >= 0x7F) {
                char buf[8];
                snprintf(buf, sizeof(buf), "\\u%04x", c);
                o += buf;
            }
            else o += static_cast<char>(c);
        }
    }
    o += "\"";
    return o;
}

std::string F(float v) {
    // Non-finite values would produce invalid JSON; they should not occur after
    // the parser's sanity checks, but a NaN slipping through must not silently
    // corrupt the file.
    if (!std::isfinite(v)) v = 0.f;
    char buf[40];
    snprintf(buf, sizeof(buf), "%.9g", v);
    return buf;
}

// The bind-pose world transform of a bone, as computed by NBN2Parser, with the
// export scale applied to the translation only -- scaling the rotation basis
// too would compound the factor down every chain and warp the skeleton.
glm::mat4 WorldMat(const Bone& b, float scale) {
    glm::mat4 m(b.world_rot);
    m[3] = glm::vec4(b.world_pos * scale, 1.f);
    return m;
}

// Strip anything that cannot go in a filename. Chunk names come from the game
// data and are not guaranteed to be path-safe.
std::string SafeName(const std::string& in) {
    std::string o;
    for (char c : in) {
        const unsigned char u = static_cast<unsigned char>(c);
        if (u < 0x20 || u >= 0x7F) continue;
        if (std::strchr("\\/:*?\"<>|", c)) { o += '_'; continue; }
        o += c;
    }
    // Trim trailing dots/spaces, which Windows silently drops from filenames.
    while (!o.empty() && (o.back() == '.' || o.back() == ' ')) o.pop_back();
    if (o.empty()) o = "model";
    return o;
}

// Read an uploaded texture back out of GL as RGBA8.
bool ReadTexture(GLuint tex, std::vector<uint8_t>& rgba, int& w, int& h) {
    if (!tex) return false;
    glBindTexture(GL_TEXTURE_2D, tex);
    GLint gw = 0, gh = 0;
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_WIDTH, &gw);
    glGetTexLevelParameteriv(GL_TEXTURE_2D, 0, GL_TEXTURE_HEIGHT, &gh);
    if (gw <= 0 || gh <= 0) { glBindTexture(GL_TEXTURE_2D, 0); return false; }
    rgba.assign(size_t(gw) * gh * 4, 0);
    glPixelStorei(GL_PACK_ALIGNMENT, 1);
    glGetTexImage(GL_TEXTURE_2D, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glBindTexture(GL_TEXTURE_2D, 0);
    w = gw; h = gh;
    return true;
}

} // namespace

GltfExportResult GltfExporter::Export(const std::string& out_dir,
    const std::string& base_name,
    const NMDLModel& model,
    const std::vector<Bone>& bones,
    float scale) {
    GltfExportResult R;

    if (model.meshes.empty()) {
        R.message = "Model has no meshes.";
        return R;
    }
    if (!(scale > 0.f) || !std::isfinite(scale)) scale = kGltfDefaultScale;

    std::error_code ec;
    const fs::path dir(out_dir);
    if (!dir.empty() && !fs::is_directory(dir, ec)) {
        fs::create_directories(dir, ec);
        if (ec) { R.message = "Cannot create " + dir.string(); return R; }
    }
    const std::string stem = SafeName(base_name);
    const fs::path    out = dir.empty() ? fs::path(stem + ".gltf") : (dir / (stem + ".gltf"));
    const std::string bin_name = stem + ".bin";
    R.gltf_path = out.string();

    BufferBuilder B;

    // ── Textures ──────────────────────────────────────────────────────────────
    // Materials address textures by GL handle; several materials commonly share
    // one. Deduplicate by handle so each image is written and listed once.
    std::unordered_map<GLuint, int> tex_to_image;   // GL handle -> glTF image idx
    std::vector<std::string>        image_uris;
    for (const auto& kv : model.mat_to_tex) {
        const GLuint h = kv.second;
        if (!h || tex_to_image.count(h)) continue;
        std::vector<uint8_t> rgba; int w = 0, hgt = 0;
        if (!ReadTexture(h, rgba, w, hgt)) continue;
        const int idx = static_cast<int>(image_uris.size());
        char fname[128];
        snprintf(fname, sizeof(fname), "%s_tex%d.png", stem.c_str(), idx);
        const fs::path png = dir.empty() ? fs::path(fname) : (dir / fname);
        if (!stbi_write_png(png.string().c_str(), w, hgt, 4, rgba.data(), w * 4))
            continue;
        tex_to_image[h] = idx;
        image_uris.push_back(fname);
    }
    R.textures = static_cast<int>(image_uris.size());

    // ── Materials ─────────────────────────────────────────────────────────────
    // One glTF material per NSHP mat_id actually referenced by a section.
    std::unordered_map<uint16_t, int> mat_to_gltf;
    struct MatDef { int image = -1; bool blend = false; };
    std::vector<MatDef> mats;
    for (const auto& mesh : model.meshes) {
        for (const auto& sec : mesh.faceSections) {
            if (mat_to_gltf.count(sec.mat_id)) continue;
            MatDef md;
            auto it = model.mat_to_tex.find(sec.mat_id);
            if (it != model.mat_to_tex.end()) {
                auto im = tex_to_image.find(it->second);
                if (im != tex_to_image.end()) md.image = im->second;
            }
            md.blend = model.mat_blend_ids.count(sec.mat_id) != 0;
            mat_to_gltf[sec.mat_id] = static_cast<int>(mats.size());
            mats.push_back(md);
        }
    }

    // ── Skeleton ──────────────────────────────────────────────────────────────
    // Bone nodes come first so their indices are stable and easy to reference
    // from the skin; mesh nodes follow.
    const size_t NB = bones.size();
    const bool   has_skin = NB > 0;
    int ibm_accessor = -1;

    if (has_skin) {
        std::vector<float> ibm;
        ibm.reserve(NB * 16);
        for (const auto& b : bones) {
            const glm::mat4 inv = glm::inverse(WorldMat(b, scale));
            for (int c = 0; c < 4; c++)
                for (int r = 0; r < 4; r++) ibm.push_back(inv[c][r]);
            if (b.flags >= 0x100) R.cloth_bones++;
        }
        // No target: the spec disallows ARRAY_BUFFER on inverse-bind data.
        const int view = B.AddView(ibm.data(), ibm.size() * 4, 0, 0);
        Accessor a; a.buffer_view = view; a.component_type = GL_CT_FLOAT;
        a.count = NB; a.type = "MAT4";
        ibm_accessor = B.AddAccessor(a);
        R.bones = static_cast<int>(NB);
    }

    // ── Meshes ────────────────────────────────────────────────────────────────
    struct Prim { int mat = -1; int idx_accessor = -1; };
    struct MeshDef {
        std::string name;
        int pos = -1, nrm = -1, uv = -1, joints = -1, weights = -1;
        std::vector<Prim> prims;
    };
    std::vector<MeshDef> meshdefs;

    for (const auto& mesh : model.meshes) {
        // Sequential meshes carry no index buffer and the viewer draws them as a
        // point cloud; there is no face data to hand Blender, so skip them
        // rather than emit a degenerate primitive.
        if (mesh.draw_sequential || mesh.indices.empty() || mesh.vertices.empty()) {
            R.skipped_meshes++;
            continue;
        }

        MeshDef md;
        md.name = mesh.name.empty() ? "mesh" : mesh.name;

        const size_t VC = mesh.vertices.size();
        std::vector<float>    pos(VC * 3), nrm(VC * 3), uv(VC * 2);
        std::vector<uint16_t> joints;
        std::vector<float>    weights;

        const bool skinned = has_skin && !mesh.boneIDList.empty();
        if (skinned) { joints.assign(VC * 4, 0); weights.assign(VC * 4, 0.f); }

        float mn[3] = { FLT_MAX, FLT_MAX, FLT_MAX };
        float mx[3] = { -FLT_MAX, -FLT_MAX, -FLT_MAX };

        for (size_t i = 0; i < VC; i++) {
            const Vertex& v = mesh.vertices[i];
            for (int c = 0; c < 3; c++) {
                const float p = v.position[c] * scale;
                pos[i * 3 + c] = p;
                mn[c] = std::min(mn[c], p);
                mx[c] = std::max(mx[c], p);
                nrm[i * 3 + c] = v.normal[c];
            }
            uv[i * 2 + 0] = v.uv[0];
            uv[i * 2 + 1] = v.uv[1];

            if (skinned) {
                // bone_ids index this mesh's local bone list; glTF joints index
                // the skin's joint array, which is the global bone order.
                float sum = 0.f;
                for (int k = 0; k < 4; k++) sum += v.bone_weights[k];
                for (int k = 0; k < 4; k++) {
                    const uint8_t local = v.bone_ids[k];
                    const uint16_t global = (local < mesh.boneIDList.size())
                        ? mesh.boneIDList[local] : 0;
                    joints[i * 4 + k] = (global < NB) ? global : 0;
                    // glTF requires the four weights to sum to 1. The viewer's
                    // shader normalises at draw time, so the stored values do
                    // not, and an unnormalised set shrinks the mesh toward the
                    // origin in Blender.
                    weights[i * 4 + k] = (sum > 1e-6f) ? (v.bone_weights[k] / sum)
                        : (k == 0 ? 1.f : 0.f);
                }
            }
        }

        auto add_vec = [&](const std::vector<float>& d, int comps,
            const char* type, bool bounds) {
                const int view = B.AddView(d.data(), d.size() * 4,
                    TARGET_ARRAY_BUFFER, comps * 4);
                Accessor a; a.buffer_view = view; a.component_type = GL_CT_FLOAT;
                a.count = VC; a.type = type;
                if (bounds) {
                    a.has_bounds = true;
                    for (int c = 0; c < 3; c++) { a.min_v[c] = mn[c]; a.max_v[c] = mx[c]; }
                }
                return B.AddAccessor(a);
            };

        md.pos = add_vec(pos, 3, "VEC3", true);
        md.nrm = add_vec(nrm, 3, "VEC3", false);
        md.uv = add_vec(uv, 2, "VEC2", false);
        if (skinned) {
            const int jv = B.AddView(joints.data(), joints.size() * 2,
                TARGET_ARRAY_BUFFER, 8);
            Accessor ja; ja.buffer_view = jv;
            ja.component_type = GL_CT_UNSIGNED_SHORT;
            ja.count = VC; ja.type = "VEC4";
            md.joints = B.AddAccessor(ja);
            md.weights = add_vec(weights, 4, "VEC4", false);
        }

        // One primitive per face section, so each keeps its own material.
        for (const auto& sec : mesh.faceSections) {
            if (sec.index_count == 0) continue;
            if (size_t(sec.index_start) + sec.index_count > mesh.indices.size()) continue;
            const int iv = B.AddView(mesh.indices.data() + sec.index_start,
                size_t(sec.index_count) * 2, TARGET_ELEMENT_ARRAY, 0);
            Accessor ia; ia.buffer_view = iv;
            ia.component_type = GL_CT_UNSIGNED_SHORT;
            ia.count = sec.index_count; ia.type = "SCALAR";
            Prim pr;
            pr.idx_accessor = B.AddAccessor(ia);
            auto mi = mat_to_gltf.find(sec.mat_id);
            pr.mat = (mi != mat_to_gltf.end()) ? mi->second : -1;
            md.prims.push_back(pr);
        }
        if (md.prims.empty()) { R.skipped_meshes++; continue; }

        R.primitives += static_cast<int>(md.prims.size());
        meshdefs.push_back(std::move(md));
    }

    if (meshdefs.empty()) {
        R.message = "No exportable geometry (all meshes were point clouds or empty).";
        return R;
    }
    R.meshes = static_cast<int>(meshdefs.size());

    // ── Write the .bin ────────────────────────────────────────────────────────
    B.Align();
    {
        std::ofstream f(dir.empty() ? fs::path(bin_name) : (dir / bin_name),
            std::ios::binary | std::ios::trunc);
        if (!f) { R.message = "Cannot write " + bin_name; return R; }
        f.write(reinterpret_cast<const char*>(B.bin.data()),
            static_cast<std::streamsize>(B.bin.size()));
        if (!f) { R.message = "Failed while writing " + bin_name; return R; }
    }

    // ── Write the .gltf ───────────────────────────────────────────────────────
    std::ostringstream j;
    j << "{\n\"asset\":{\"version\":\"2.0\",\"generator\":\"Eternal Sonata Studio\"},\n";

    // Nodes: bones first (indices 0..NB-1), then one node per mesh.
    j << "\"nodes\":[\n";
    bool first = true;
    for (size_t i = 0; i < NB; i++) {
        const Bone& b = bones[i];
        // glTF nodes are local to their parent; the parser gives world transforms.
        glm::mat4 local = WorldMat(b, scale);
        if (b.parent_idx >= 0 && size_t(b.parent_idx) < NB)
            local = glm::inverse(WorldMat(bones[b.parent_idx], scale)) * local;
        const glm::vec3 t(local[3]);
        const glm::quat q = glm::quat_cast(glm::mat3(local));

        if (!first) j << ",\n";
        first = false;
        j << "{\"name\":" << JStr(b.name.empty() ? ("bone" + std::to_string(i)) : b.name)
            << ",\"translation\":[" << F(t.x) << "," << F(t.y) << "," << F(t.z) << "]"
            << ",\"rotation\":[" << F(q.x) << "," << F(q.y) << "," << F(q.z) << "," << F(q.w) << "]";
        // Children, from the parent links (child/sibling indices are not needed).
        std::string kids;
        for (size_t k = 0; k < NB; k++) {
            if (bones[k].parent_idx == static_cast<int16_t>(i)) {
                if (!kids.empty()) kids += ",";
                kids += std::to_string(k);
            }
        }
        if (!kids.empty()) j << ",\"children\":[" << kids << "]";
        // Blender reads `extras` into the bone's Custom Properties panel.
        if (b.flags >= 0x100)
            j << ",\"extras\":{\"cloth\":true,\"nbn2_flags\":" << b.flags << "}";
        else
            j << ",\"extras\":{\"nbn2_flags\":" << b.flags << "}";
        j << "}";
    }
    for (size_t m = 0; m < meshdefs.size(); m++) {
        if (!first) j << ",\n";
        first = false;
        j << "{\"name\":" << JStr(meshdefs[m].name) << ",\"mesh\":" << m;
        if (has_skin && meshdefs[m].joints >= 0) j << ",\"skin\":0";
        j << "}";
    }
    j << "\n],\n";

    // Scene roots: every parentless bone, plus every mesh node. A skinned mesh
    // node must not sit under a joint -- the spec ignores its transform there.
    {
        std::string roots;
        for (size_t i = 0; i < NB; i++) {
            if (bones[i].parent_idx < 0 || size_t(bones[i].parent_idx) >= NB) {
                if (!roots.empty()) roots += ",";
                roots += std::to_string(i);
            }
        }
        for (size_t m = 0; m < meshdefs.size(); m++) {
            if (!roots.empty()) roots += ",";
            roots += std::to_string(NB + m);
        }
        j << "\"scene\":0,\n\"scenes\":[{\"nodes\":[" << roots << "]}],\n";
    }

    if (has_skin) {
        std::string joints;
        for (size_t i = 0; i < NB; i++) {
            if (!joints.empty()) joints += ",";
            joints += std::to_string(i);
        }
        j << "\"skins\":[{\"inverseBindMatrices\":" << ibm_accessor
            << ",\"joints\":[" << joints << "]}],\n";
    }

    j << "\"meshes\":[\n";
    for (size_t m = 0; m < meshdefs.size(); m++) {
        const MeshDef& md = meshdefs[m];
        if (m) j << ",\n";
        j << "{\"name\":" << JStr(md.name) << ",\"primitives\":[";
        for (size_t p = 0; p < md.prims.size(); p++) {
            if (p) j << ",";
            j << "{\"attributes\":{\"POSITION\":" << md.pos
                << ",\"NORMAL\":" << md.nrm
                << ",\"TEXCOORD_0\":" << md.uv;
            if (md.joints >= 0)
                j << ",\"JOINTS_0\":" << md.joints << ",\"WEIGHTS_0\":" << md.weights;
            j << "},\"indices\":" << md.prims[p].idx_accessor;
            if (md.prims[p].mat >= 0) j << ",\"material\":" << md.prims[p].mat;
            j << ",\"mode\":4}";
        }
        j << "]}";
    }
    j << "\n],\n";

    if (!mats.empty()) {
        j << "\"materials\":[\n";
        for (size_t m = 0; m < mats.size(); m++) {
            if (m) j << ",\n";
            j << "{\"name\":\"mat" << m << "\",\"doubleSided\":true"
                << ",\"pbrMetallicRoughness\":{\"metallicFactor\":0,\"roughnessFactor\":1";
            if (mats[m].image >= 0)
                j << ",\"baseColorTexture\":{\"index\":" << mats[m].image << "}";
            j << "}";
            // Matches how the viewport draws these: DXT-style cutouts discard at
            // 0.5, and only genuinely translucent textures get real blending.
            if (mats[m].image >= 0)
                j << (mats[m].blend ? ",\"alphaMode\":\"BLEND\""
                    : ",\"alphaMode\":\"MASK\",\"alphaCutoff\":0.5");
            j << "}";
        }
        j << "\n],\n";
    }

    if (!image_uris.empty()) {
        j << "\"images\":[";
        for (size_t i = 0; i < image_uris.size(); i++) {
            if (i) j << ",";
            j << "{\"uri\":" << JStr(image_uris[i]) << "}";
        }
        j << "],\n\"textures\":[";
        for (size_t i = 0; i < image_uris.size(); i++) {
            if (i) j << ",";
            j << "{\"source\":" << i << ",\"sampler\":0}";
        }
        // REPEAT wrapping with linear filtering, matching the viewport default.
        j << "],\n\"samplers\":[{\"magFilter\":9729,\"minFilter\":9729,"
            "\"wrapS\":10497,\"wrapT\":10497}],\n";
    }

    j << "\"bufferViews\":[\n";
    for (size_t i = 0; i < B.views.size(); i++) {
        if (i) j << ",\n";
        j << "{\"buffer\":0,\"byteOffset\":" << B.views[i].offset
            << ",\"byteLength\":" << B.views[i].length;
        if (B.views[i].byte_stride) j << ",\"byteStride\":" << B.views[i].byte_stride;
        if (B.views[i].target)      j << ",\"target\":" << B.views[i].target;
        j << "}";
    }
    j << "\n],\n\"accessors\":[\n";
    for (size_t i = 0; i < B.accessors.size(); i++) {
        const Accessor& a = B.accessors[i];
        if (i) j << ",\n";
        j << "{\"bufferView\":" << a.buffer_view
            << ",\"componentType\":" << a.component_type
            << ",\"count\":" << a.count
            << ",\"type\":" << JStr(a.type);
        if (a.has_bounds) {
            j << ",\"min\":[" << F(a.min_v[0]) << "," << F(a.min_v[1]) << "," << F(a.min_v[2]) << "]"
                << ",\"max\":[" << F(a.max_v[0]) << "," << F(a.max_v[1]) << "," << F(a.max_v[2]) << "]";
        }
        j << "}";
    }
    j << "\n],\n\"buffers\":[{\"uri\":" << JStr(bin_name)
        << ",\"byteLength\":" << B.bin.size() << "}]\n}\n";

    {
        std::ofstream f(out, std::ios::trunc);
        if (!f) { R.message = "Cannot write " + out.string(); return R; }
        const std::string s = j.str();
        f.write(s.data(), static_cast<std::streamsize>(s.size()));
        if (!f) { R.message = "Failed while writing " + out.string(); return R; }
    }

    char msg[256];
    snprintf(msg, sizeof(msg),
        "%d meshes (%d parts), %d bones (%d cloth), %d textures, scale x%g",
        R.meshes, R.primitives, R.bones, R.cloth_bones, R.textures, scale);
    R.message = msg;
    R.ok = true;
    return R;
}
