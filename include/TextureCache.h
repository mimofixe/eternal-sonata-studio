#pragma once
#include <glad/glad.h>
#include <unordered_map>
#include <cstdint>

class TextureCache {
public:
    TextureCache();
    ~TextureCache();

    GLuint GetOrCreateTexture(uint8_t textureId, const uint8_t* rgba, int width, int height);
    void Clear();

private:
    std::unordered_map<uint8_t, GLuint> m_Textures;
};