#pragma once
#include <glad/glad.h>
#include <glm/glm.hpp>
#include <vector>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <cstdint>

#include "NBN2Parser.h"
#include "NSHPParser.h"
#include "NMTNParser.h"
#include "Camera.h"
#include "Shader.h"
#include "EFileParser.h"

// GPU data for one skinned mesh. Now carries the face sections and per-vertex
// UVs needed to draw the mesh textured (one draw call per material section),
// in addition to the skinning attributes.
struct SkinMeshGPU {
    GLuint VAO = 0, VBO = 0, EBO = 0;
    std::string name;                        // NSHP mesh name ("skirt", "body", ...)
    bool   visible = true;                   // unchecked in the Meshes popup -> skipped
    size_t vertex_count = 0;
    size_t index_count = 0;
    bool   draw_sequential = false;
    std::vector<uint16_t>    bone_id_list;   // local bone index -> global NBN2 index
    std::vector<FaceSection> sections;       // material id + index range per submesh
};

// Cloth dynamics (Dynabone), reverse-engineered from the Xbox 360 executable
// (chain solver 0x8215e280..0x82160540, builder 0x82118628, collisions
// 0x82137510 / 0x82136b08 / response 0x82137cf0). The solver was checked against
// the game's own code run in an interpreter (positions agree to < 2 mm over 2 s).
//
// A chain starts at a bone with flag 0x2 (kinematic root, carried by its parent)
// and follows the first dynamic child until a bone with flag 0x8 (end); an end
// bone that also has 0x2 starts the next chain. The other bodies are point masses
// on inextensible rods: gravity, a PD spring toward the animated pose (pre-sagged
// so the animated pose is the static equilibrium) and quadratic air drag; rod
// tensions come from a linear solve every substep. dt = 1/60, 10 substeps when
// the root record's type bit 0 is set, 8 otherwise. After the solve each node is
// pushed out of the keep-out spheres of its masked groups (+1 mm) and kept inside
// its keep-in spheres (-2 mm) by rotating its segment about the parent node;
// the nodes below follow.
//
// NDYN record (48 bytes, at chunk+8 + ndyn_index*48):
//   +0x00 u16 type (bit 0: 10 substeps)        +0x02 u16 bone (cross-check)
//   +0x04 u16 mask pass 2, +0x06 u16 mask pass 1: bit b -> NCLS group b+1
//   +0x08 f32 mass   +0x0C f32 air drag (chain value, from the root's record)
//   +0x10 f32 spring k   +0x14 f32 damping   (segment parent->child: parent's record)
//   +0x18 u32 (count<<24)|first: links {u32 bone, f32 weight, f32 tol} (32 bytes)
//             after the records; +0x28 u16 sheet threshold /64
//   +0x20 u8 keep-in id: NCLS group 29 records with that id
// Frame order (0x8213e420): every chain solve + keep-in, then the sheet solver
// (0x8213f448: when a chain of a linked group strays more than the threshold
// from the animation, its neighbours are pulled to weight * rest distance of it
// and the three chains flare outward by tol radians), then the keep-out spheres.
// NCLS record (48 bytes): +0x00 u8 type (0 sphere), +0x01 u8 id, +0x02 u16 bone,
// +0x04 i16x3 offset /64 (bone frame), +0x0A i16x3 Euler /4096, +0x10 f32 radius.
struct ClothBone {
    bool      isDyn = false;
    bool      follower = false;      // rigid bone under a dynamic ancestor (JHribon)
    int       chain = -1;            // chain whose root is this bone
    float     mass = 0.003f;         // NDYN +0x08
    float     ks = 0.f, kd = 0.f;    // NDYN +0x10 / +0x14 (applied to the child body)
    float     drag = 0.f;            // NDYN +0x0C
    uint16_t  type = 0;              // NDYN +0x00
    uint16_t  mask1 = 0, mask2 = 0;  // NDYN +0x06 / +0x04 (pass 1 / pass 2)
    std::vector<int> keepIn;         // colliders of NCLS group 29 with id == NDYN +0x20
    struct Link { int bone; float weight, tol; };
    std::vector<Link> links;         // NDYN +0x18 table (32-byte entries)
    float     sheetThr = 0.f;        // NDYN +0x28 / 64
};

struct ClothSphere {                 // NCLS record ("collider")
    int       bone = -1;
    uint8_t   type = 0;              // 0 sphere (1 box, 2 capsule: not simulated)
    uint8_t   id = 0;
    glm::vec3 loff{ 0.f };           // offset in the bone frame
    float     r = 0.1f;
};

// Link from a chain node to a node of a neighbouring chain (sheet solver).
struct ClothLink {
    int   chain = -1, node = -1;
    float weight = 1.f, tol = 0.f;
    float rest = -1.f;               // node distance on the animated pose at reset
};

// One simulated chain: bodies[0] is the kinematic root.
struct ClothChain {
    std::vector<int>       bones;    // bone index per body
    std::vector<glm::vec3> x, v;     // body state (world)
    std::vector<glm::vec3> tgt;      // animated pose this frame
    std::vector<glm::vec3> xPrev;    // state at the start of this frame
    std::vector<float>     m, ks, kd;// per body (ks/kd of body j from bone j-1)
    std::vector<std::vector<ClothLink>> links;   // per body
    float drag = 0.f;
    float sheetThr = 0.f;
    int   substeps = 8;
    bool  spring = false;
    bool  init = false;
};

// Chains joined side by side by links (a skirt), in neighbour order.
struct ClothSheet {
    std::vector<int> chains;
    int first = 1, last = 1;         // node range carrying links
};

// One self-contained animatable model: skeleton + skinned meshes + animations,
// plus the textures decoded from the model's own NMDL so the meshes can be drawn
// textured instead of flat-shaded.
struct SkeletalModel {
    std::string name;

    std::vector<Bone>      bind;
    std::vector<Bone>      anim;
    // Rigid (animation-only) pose snapshot, refreshed by ApplyPose: the cloth
    // targets and the collider spheres are taken from it.
    std::vector<Bone> rigidAnim;
    std::vector<glm::mat4> invBind;
    // Bone name -> every bone index carrying that name, in file order. Names are
    // NOT unique: the e3120_110 seagull has two 'Jude_L' bones (20 and 22) and two
    // 'Jude_R' (32 and 34), and its NMTN ships a matching pair of same-named
    // tracks. A plain name->index map kept only the last of each, so both tracks
    // landed on bone 22/34 and the wings' upper-arm bones were never animated at
    // all. ApplyPose walks tracks in order and consumes the duplicates in order.
    std::unordered_map<std::string, std::vector<int>> boneIndex;

    std::vector<SkinMeshGPU> meshes;
    std::vector<NMTNAnimation> anims;

    // material -> GL texture, decoded from the NMDL's internal NTX3 chunks.
    std::unordered_map<uint16_t, GLuint> matToTex;
    std::unordered_map<uint16_t, GLuint> matToAlpha;
    std::unordered_set<uint16_t>         matBlend;   // DXT5: full alpha -> GL_BLEND
    std::unordered_set<uint16_t>         matClamp;   // GL_CLAMP_TO_EDGE
    std::vector<GLuint>                  ownedTextures;

    int   animIdx = 0;
    float frame = 0.f;
    bool  playing = false;

    GLuint lineVAO = 0, lineVBO = 0;
    int    lineVerts = 0;

    // cloth dynamics
    std::vector<ClothBone>    cloth;       // per bone (size == bind.size())
    std::vector<ClothSphere>  colliders;   // NCLS records (all groups, flattened)
    std::vector<std::pair<uint16_t, uint16_t>> clothGroups; // NCLS group -> [first,last) in colliders
    std::vector<ClothChain>   chains;      // simulated chains, parents before children
    std::vector<ClothSheet>   sheets;      // sheet solver groups
    std::vector<int>          clothOrder;  // topological bone order (parents first)
    std::vector<glm::vec3>    clothPos;    // solved world positions (per bone)
    std::vector<glm::mat3>    clothDelta;  // solved rotation delta on top of the rigid pose
    bool  hasCloth = false;
    bool  clothInit = false;

    glm::vec3 fitCenter{ 0.f };
    float     fitDist = 3.f;
};

class AnimViewport {
public:
    AnimViewport();
    ~AnimViewport();

    void LoadFromChunks(const std::vector<uint8_t>& file_data,
        const std::vector<Chunk>& chunks);

    void Tick(float dt);
    void Render();

    bool HasContent() const { return m_HasContent; }
    void Clear();
    int  ModelCount() const { return (int)m_Models.size(); }

private:
    std::vector<SkeletalModel> m_Models;
    int  m_ModelIdx = 0;

    SkeletalModel* Active();
    const SkeletalModel* Active() const;

    void BuildGroups(const std::vector<uint8_t>& file_data,
        const std::vector<Chunk>& chunks);
    void SelectModel(int idx);

    Camera  m_Camera;
    Shader* m_SkinShader = nullptr;   // skinning + texture
    Shader* m_BoneShader = nullptr;

    GLuint m_FBO = 0, m_FBOTex = 0, m_FBORbo = 0;
    int    m_FBOw = 0, m_FBOh = 0;
    int    m_ViewW = 800, m_ViewH = 600;

    float m_PlaySpeed = 30.f;
    bool  m_ShowSkel = false;
    bool  m_Textured = true;          // draw with NMDL textures vs flat shading
    bool  m_Lighting = false;         // off = flat texture like the static Viewport3D
    bool  m_Cloth = true;             // run NDYN/NCLS cloth dynamics
    float m_Gravity = 9.80665f;       // proven from the Dynabone binary (real g)
    bool  m_HasContent = false;
    float m_ClothAccum = 0.f;         // fixed-timestep accumulator (engine runs 1/60)

    void BuildInvBind(SkeletalModel& m);
    void ApplyPose(SkeletalModel& m, float frame);
    void RebuildSkeletonLines(SkeletalModel& m);
    void UploadSkinMesh(const NSHPMesh& mesh, SkinMeshGPU& out);
    void CleanupModel(SkeletalModel& m);
    glm::mat4 WorldMat(const Bone& b) const;

    // cloth dynamics
    void ParseDynamics(SkeletalModel& m, const std::vector<uint8_t>& fd,
        size_t region_begin, size_t region_end);
    void StepDynamics(SkeletalModel& m, float dt);   // one 1/60 cloth frame
    void SolveChain(SkeletalModel& m, ClothChain& c, float dt);
    void CollideChain(SkeletalModel& m, ClothChain& c, bool keepIn, float dt);
    void SolveSheets(SkeletalModel& m);
    void WriteClothPose(SkeletalModel& m);
    void ApplyClothPose(SkeletalModel& m);           // write cloth state to m.anim (no advance)
    void SettleDynamics(SkeletalModel& m, int steps); // settle for static preview
    void ResetDynamics(SkeletalModel& m);

    // "Meshes (n/N)" popup. False when the model has no meshes and nothing was
    // drawn, so the caller can skip its SameLine.
    bool DrawMeshVisibilityUI(SkeletalModel& m);
    static void SetMat4Array(GLuint prog, const char* name,
        const glm::mat4* mats, int count);
};