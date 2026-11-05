#pragma once
#include "EFileParser.h"
#include "NSHPParser.h"
#include "NBN2Parser.h"
#include "NMDLLoader.h"
#include "Viewport3D.h"
#include "P3TexParser.h"
#include "TextureCache.h"
#include "GltfExporter.h"
#include "BmdCreditsParser.h"

class ChunkInspector {
public:
    ChunkInspector();
    ~ChunkInspector();

    void SetChunk(const Chunk& chunk, const std::vector<uint8_t>& file_data);

    // File-scoped state (currently the credits table). Call ClearFileContext()
    // at the start of every file load and SetFileContext() once the file's
    // bytes are available, so the Inspector always reflects the open file.
    void SetFileContext(const std::vector<uint8_t>& file_data);
    void ClearFileContext();
    void Render();
    void SetViewport(Viewport3D* viewport);
    void SetP3TexParser(P3TexParser* parser);

private:
    // Prompts for a .p3tex/.x3tex archive and uploads its textures into
    // m_MapTextures/m_MapTexDXT5. Returns false if the user cancelled the
    // dialog or the archive failed to load.
    bool LoadMapTextureArchive();

    // Writes the current NMDL chunk out as glTF 2.0 (+ .bin + PNGs), leaving a
    // one-line result in m_ExportStatus.
    void ExportNMDLToGltf();

    void RenderProperties();
    void RenderHexView();
    void RenderNSHPInfo();
    void RenderNMDLInfo();
    void RenderCameraInfo();
    void RenderLightInfo();
    void RenderFogInfo();
    void RenderMaterialInfo();
    void RenderAnimationInfo();
    void RenderNTX3Info();
    void RenderNBN2Info();
    void RenderTextView();
    void RenderCreditsInfo();
    std::vector<std::string> ExtractStrings(const uint8_t* data, size_t size, size_t min_length = 4);

    Chunk m_CurrentChunk;
    std::vector<uint8_t> m_ChunkData;
    bool m_HasChunk;

    CameraData m_CameraData;
    LightData m_LightData;
    FogData m_FogData;
    MaterialData m_MaterialData;

    Viewport3D* m_Viewport;
    P3TexParser* m_P3TexParser;
    TextureCache m_TextureCache;

    // Pointer to the full file buffer — needed by NMDLLoader which scans
    // the entire NMDL envelope, not just the isolated chunk data.
    const std::vector<uint8_t>* m_FileData = nullptr;

    // GPU textures uploaded from a manually-loaded .p3tex file.
    // Used when the current file is a map NMDL that has no internal NTX3 chunks.
    // These are NOT owned here — they belong to the external P3TexParser instance
    // held by the application, so we store GLuints by value (copied, not deleted).
    std::vector<GLuint> m_MapTextures;
    std::vector<bool> m_MapTexDXT5;

    // Result line for the last glTF export, shown under the export button.
    std::string m_ExportStatus;
    bool        m_ExportOk = false;
    float       m_ExportScale = kGltfDefaultScale;

    // Credits BMD (op.bmd, ab.bmd, ed1.bmd, ed2.bmd). Describes the whole
    // file rather than the selected chunk, so it is driven by
    // Set/ClearFileContext rather than by SetChunk.
    CreditsFile m_Credits;
    size_t m_CreditsSection = 0;
    bool   m_CreditsShowTags = false;
    bool   m_CreditsHideSpacers = true;

    // Cache para evitar parsing repetido
    NSHPMesh m_CachedMesh;
    bool m_MeshCached;
    size_t m_LastChunkOffset;
};