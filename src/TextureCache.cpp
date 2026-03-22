#include "TextureCache.h"
#include <iostream>

TextureCache::TextureCache() {
}

TextureCache::~TextureCache() {
    Clear();
}

GLuint TextureCache::GetOrCreateTexture(uint8_t textureId, const uint8_t* rgba, int width, int height) {
    // Check if already exists
    auto it = m_Textures.find(textureId);
    if (it != m_Textures.end()) {
        return it->second;
    }

    // Return 0 if no data provided (just checking cache)
    if (rgba == nullptr || width == 0 || height == 0) {
        return 0;
    }

    // Create new texture
    GLuint texture;
    glGenTextures(1, &texture);
    glBindTexture(GL_TEXTURE_2D, texture);

    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba);

    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);

    glBindTexture(GL_TEXTURE_2D, 0);

    m_Textures[textureId] = texture;

    std::cout << "Created GPU texture " << (int)textureId << " (" << width << "x" << height << ")" << std::endl;

    return texture;
}

void TextureCache::Clear() {
    for (auto& pair : m_Textures) {
        glDeleteTextures(1, &pair.second);
    }
    m_Textures.clear();
}