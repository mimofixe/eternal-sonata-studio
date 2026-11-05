#pragma once
#include <string>
#include <vector>
#include <cstdint>
#include "NMDLLoader.h"
#include "NBN2Parser.h"

// Export an NMDL model to glTF 2.0, the one interchange format Blender imports
// natively with meshes, UVs, textures AND a skinned armature in a single file
// set. OBJ carries no bones, and FBX/Collada would both mean shipping a writer
// far larger than this one.
//
// Writes three kinds of file into the chosen directory:
//   <name>.gltf        JSON scene: nodes, armature, materials
//   <name>.bin         vertex/index/inverse-bind data
//   <name>_texN.png    one per distinct texture the materials reference
//
// Cloth bones are exported as ordinary joints (so the armature is complete and
// the skinning is correct) and additionally tagged with a `cloth` custom
// property, which Blender surfaces in the bone's Custom Properties panel. The
// simulation itself is not exported: NDYN parameters have no glTF equivalent.
struct GltfExportResult {
    bool        ok = false;
    std::string message;      // one-line summary, or the reason it failed
    int         meshes = 0;
    int         primitives = 0;
    int         bones = 0;
    int         cloth_bones = 0;
    int         textures = 0;
    int         skipped_meshes = 0;   // point-cloud (sequential) meshes, see below
    std::string gltf_path;            // the .gltf actually written
};

// Default export scale.
constexpr float kGltfDefaultScale = 1.0f;

class GltfExporter {
public:
    // `out_dir` receives all the files; `base_name` names them (it is sanitised
    // for the filesystem). `bones` may be empty, in which case the model exports
    // unskinned. Texture pixels are read back from the GL handles in `model`, so
    // this must be called with the same GL context current that uploaded them.
    static GltfExportResult Export(const std::string& out_dir,
        const std::string& base_name,
        const NMDLModel& model,
        const std::vector<Bone>& bones,
        float scale = kGltfDefaultScale);
};
