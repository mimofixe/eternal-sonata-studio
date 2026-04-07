#pragma once
#include "EFileParser.h"
#include "NSHPParser.h"
#include "NBN2Parser.h"
#include "NMDLLoader.h"
#include "Viewport3D.h"
#include "P3TexParser.h"
#include "TextureCache.h"

class ChunkInspector {
public:
    ChunkInspector();
    ~ChunkInspector();

    void SetChunk(const Chunk& chunk, const std::vector<uint8_t>& file_data);
    void Render();
    void SetViewport(Viewport3D* viewport);
    void SetP3TexParser(P3TexParser* parser);

private:
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

    // Cache para evitar parsing repetido
    NSHPMesh m_CachedMesh;
    bool m_MeshCached;
    size_t m_LastChunkOffset;
};