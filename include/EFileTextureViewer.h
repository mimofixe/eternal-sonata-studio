#pragma once
#include "EFileParser.h"
#include "TextureCache.h"
#include <glad/glad.h>
#include <vector>

struct NTX3Texture {
    uint8_t id;
    size_t offset;
    int width;
    int height;
    uint8_t format;
    std::vector<uint8_t> data;
};

class EFileTextureViewer {
public:
    EFileTextureViewer();
    ~EFileTextureViewer();

    void LoadFromFile(const std::vector<Chunk>& chunks, const std::vector<uint8_t>& file_data);
    void Clear();
    void Render();

private:
    void ExtractTextures(const std::vector<Chunk>& chunks, const std::vector<uint8_t>& file_data);
    void RenderGrid();
    void RenderDetail();
    void GeneratePreview(size_t textureIdx);

    std::vector<NTX3Texture> m_Textures;
    TextureCache m_TextureCache;

    int m_SelectedTextureIdx;
    bool m_ShowDetail;
    int m_ThumbnailSize;
    int m_GridColumns;
};