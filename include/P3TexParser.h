#pragma once
#include <vector>
#include <string>
#include <cstdint>

struct P3Texture {
    size_t offset;
    size_t size;
    std::vector<uint8_t> data;

    int width;
    int height;
    uint8_t format;

    P3Texture() : offset(0), size(0), width(0), height(0), format(0) {}
};

class P3TexParser {
public:
    P3TexParser();
    ~P3TexParser();

    bool Load(const std::string& filename);
    void Clear();

    const P3Texture* GetTexture(uint8_t id) const;
    size_t GetTextureCount() const { return m_Textures.size(); }

    const std::string& GetFilename() const { return m_Filename; }
    bool IsLoaded() const { return m_Loaded; }

    static bool DeduceDimensions(size_t dataSize, int& width, int& height);
    static std::vector<uint8_t> DecompressDXT1(const uint8_t* data, int width, int height);  // NOVO
    static std::vector<uint8_t> DecompressDXT5(const uint8_t* data, int width, int height);
    // Raw RGBA8 uncompressed (format 0xA5)
    static std::vector<uint8_t> DecompressRGBA8(const uint8_t* data, int width, int height);

private:
    std::vector<P3Texture> m_Textures;
    std::string m_Filename;
    bool m_Loaded;
};