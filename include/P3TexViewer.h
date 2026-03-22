#pragma once
#include "P3TexParser.h"
#include "TextureCache.h"
#include <glad/glad.h>
#include <vector>

class P3TexViewer {
public:
    P3TexViewer();
    ~P3TexViewer();

    void SetParser(P3TexParser* parser);
    void Render();

private:
    void RenderGrid();
    void RenderDetail();
    void GeneratePreview(uint8_t textureId);

    P3TexParser* m_Parser;
    TextureCache m_TextureCache;

    int m_SelectedTextureId;
    bool m_ShowDetail;

    // Grid settings
    int m_ThumbnailSize;
    int m_GridColumns;
};