#pragma once
#include <vector>
#include <string>
#include <cstdint>

// Special format sentinel values stored in P3Texture::format
// (in addition to the PS3 NTX3 format bytes)
constexpr uint8_t P3TEX_FORMAT_RGBA8_DECODED = 0xFF;  // already decoded RGBA8 (EP3, NTX2, …)

struct P3Texture {
    size_t offset;    // byte offset of the chunk in the original file
    size_t size;      // raw compressed size
    std::vector<uint8_t> data;  // raw OR pre-decoded RGBA8 (see format)

    int     width;
    int     height;
    uint8_t format;   // PS3: 0x86=DXT1, else=DXT5, 0xA5=RGBA8
    // Xbox: P3TEX_FORMAT_RGBA8_DECODED (already decoded)

    std::string name; // texture filename from NTX2/TX2D, empty for NTX3

    P3Texture() : offset(0), size(0), width(0), height(0), format(0) {}
};

class P3TexParser {
public:
    P3TexParser();
    ~P3TexParser();

    // Load a .p3tex / .x3tex / .bmd / .e file and extract all textures.
    // For .x3tex files every top-level NTX2 chunk is decoded.
    // For PS3 files the existing NTX3 path is used unchanged.
    bool Load(const std::string& filename);
    void Clear();

    const P3Texture* GetTexture(uint8_t id) const;
    size_t GetTextureCount() const { return m_Textures.size(); }

    const std::string& GetFilename() const { return m_Filename; }
    bool IsLoaded() const { return m_Loaded; }

    static bool DeduceDimensions(size_t dataSize, int& width, int& height);

    // PS3 DXT decode helpers (also used by EFileTextureViewer)
    static std::vector<uint8_t> DecompressDXT1(
        const uint8_t* data, int width, int height);
    static std::vector<uint8_t> DecompressDXT5(
        const uint8_t* data, int width, int height);
    static std::vector<uint8_t> DecompressRGBA8(
        const uint8_t* data, int width, int height);

private:
    std::vector<P3Texture> m_Textures;
    std::string m_Filename;
    bool m_Loaded;
};