#include "EFileTextureViewer.h"
#include "P3TexParser.h"
#include <imgui.h>
#include <iostream>
#include <cstring>

EFileTextureViewer::EFileTextureViewer()
    : m_SelectedTextureIdx(-1),
    m_ShowDetail(false),
    m_ThumbnailSize(128),
    m_GridColumns(6) {
}

EFileTextureViewer::~EFileTextureViewer() {
    Clear();
}

void EFileTextureViewer::Clear() {
    m_Textures.clear();
    m_TextureCache.Clear();
    m_SelectedTextureIdx = -1;
    m_ShowDetail = false;
}

void EFileTextureViewer::LoadFromFile(const std::vector<Chunk>& chunks, const std::vector<uint8_t>& file_data) {
    Clear();
    ExtractTextures(chunks, file_data);
}

void EFileTextureViewer::ExtractTextures(const std::vector<Chunk>& chunks, const std::vector<uint8_t>& file_data) {
    uint8_t texture_id = 0;

    for (const auto& chunk : chunks) {
        if (chunk.type != ChunkType::NTX3) {
            continue;
        }

        size_t chunk_start = chunk.offset;
        if (chunk_start + 128 > file_data.size()) {
            continue;
        }

        const uint8_t* header = &file_data[chunk_start];

        // Verify magic
        if (memcmp(header, "NTX3", 4) != 0) {
            continue;
        }

        // Parse dimensions (16-bit big-endian)
        uint16_t width = (header[0x20] << 8) | header[0x21];
        uint16_t height = (header[0x22] << 8) | header[0x23];
        uint8_t format = header[0x18];

        // Extract texture data (skip 128-byte header)
        size_t data_start = chunk_start + 128;
        size_t data_size = chunk.size - 128;

        if (data_start + data_size > file_data.size()) {
            continue;
        }

        NTX3Texture tex;
        tex.id = texture_id++;
        tex.offset = chunk_start;
        tex.width = width;
        tex.height = height;
        tex.format = format;
        tex.data.resize(data_size);
        memcpy(tex.data.data(), &file_data[data_start], data_size);

        m_Textures.push_back(tex);
    }

    std::cout << "Extracted " << m_Textures.size() << " textures from .e file" << std::endl;
}

void EFileTextureViewer::Render() {
    if (m_Textures.empty()) {
        ImGui::TextDisabled("No textures in this file");
        ImGui::TextWrapped("This .e file contains no NTX3 texture chunks.");
        return;
    }

    ImGui::Text("Textures: %zu", m_Textures.size());
    ImGui::Separator();

    // Controls
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Thumbnail Size", &m_ThumbnailSize, 64, 256);
    ImGui::SameLine();
    ImGui::SetNextItemWidth(150);
    ImGui::SliderInt("Columns", &m_GridColumns, 2, 10);

    ImGui::Separator();

    if (m_ShowDetail) {
        if (ImGui::Button("< Back to Grid")) {
            m_ShowDetail = false;
        }
        ImGui::SameLine();
        ImGui::Text("Texture #%d", m_SelectedTextureIdx);
        ImGui::Separator();
        RenderDetail();
    }
    else {
        RenderGrid();
    }
}

void EFileTextureViewer::RenderGrid() {
    ImGui::BeginChild("EFileTextureGrid", ImVec2(0, 0), false);

    int columns = m_GridColumns;

    for (size_t i = 0; i < m_Textures.size(); i++) {
        const NTX3Texture& tex = m_Textures[i];

        if (tex.width == 0 || tex.height == 0) {
            continue;
        }

        // Get or generate texture
        GLuint gpuTexture = m_TextureCache.GetOrCreateTexture(tex.id, nullptr, 0, 0);
        if (gpuTexture == 0) {
            GeneratePreview(i);
            gpuTexture = m_TextureCache.GetOrCreateTexture(tex.id, nullptr, 0, 0);
        }

        // Start column
        if (i % columns != 0) {
            ImGui::SameLine();
        }

        ImGui::BeginGroup();

        ImGui::PushID((int)i);

        if (gpuTexture != 0) {
            // Calculate aspect-fit size
            float aspect = (float)tex.width / tex.height;
            float displayW = m_ThumbnailSize;
            float displayH = m_ThumbnailSize;

            if (aspect > 1.0f) {
                displayH = m_ThumbnailSize / aspect;
            }
            else {
                displayW = m_ThumbnailSize * aspect;
            }

            // Center thumbnail
            float offsetX = (m_ThumbnailSize - displayW) / 2.0f;
            float offsetY = (m_ThumbnailSize - displayH) / 2.0f;

            ImGui::Dummy(ImVec2(offsetX, offsetY));
            ImGui::SameLine(0, 0);
            ImGui::BeginGroup();
            ImGui::Dummy(ImVec2(0, offsetY));

            char buttonId[32];
            snprintf(buttonId, sizeof(buttonId), "etex_%zu", i);

            if (ImGui::ImageButton(
                buttonId,
                (ImTextureID)(intptr_t)gpuTexture,
                ImVec2(displayW, displayH),
                ImVec2(0, 0), ImVec2(1, 1),
                ImVec4(0, 0, 0, 1),
                ImVec4(1, 1, 1, 1))) {
                m_SelectedTextureIdx = (int)i;
                m_ShowDetail = true;
            }

            ImGui::EndGroup();
        }
        else {
            // Error placeholder
            ImGui::Button("...", ImVec2(m_ThumbnailSize, m_ThumbnailSize));
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Failed to load");
            }
        }

        ImGui::PopID();

        // Label
        ImGui::Text("#%zu", i);
        ImGui::SameLine();
        ImGui::TextDisabled("%dx%d", tex.width, tex.height);

        ImGui::EndGroup();
    }

    ImGui::EndChild();
}

void EFileTextureViewer::RenderDetail() {
    if (m_SelectedTextureIdx < 0 || m_SelectedTextureIdx >= (int)m_Textures.size()) {
        ImGui::Text("Invalid texture");
        return;
    }

    const NTX3Texture& tex = m_Textures[m_SelectedTextureIdx];
    GLuint gpuTexture = m_TextureCache.GetOrCreateTexture(tex.id, nullptr, 0, 0);

    // Info panel
    ImGui::BeginChild("EFileTextureInfo", ImVec2(300, 0), true);

    ImGui::Text("Texture #%d", m_SelectedTextureIdx);
    ImGui::Separator();

    ImGui::Text("Dimensions: %dx%d", tex.width, tex.height);
    ImGui::Text("Format: %s", tex.format == 0x86 ? "DXT1" : "DXT5");
    ImGui::Text("Data Size: %zu bytes", tex.data.size());
    ImGui::Text("Offset: 0x%zX", tex.offset);

    float aspect = (float)tex.width / tex.height;
    ImGui::Text("Aspect: %.2f:1", aspect);

    ImGui::Separator();

    // Navigation
    if (ImGui::Button("< Previous", ImVec2(-1, 0)) && m_SelectedTextureIdx > 0) {
        m_SelectedTextureIdx--;
    }
    if (ImGui::Button("Next >", ImVec2(-1, 0)) && m_SelectedTextureIdx < (int)m_Textures.size() - 1) {
        m_SelectedTextureIdx++;
    }

    ImGui::EndChild();

    // Preview
    ImGui::SameLine();

    ImGui::BeginChild("EFileTexturePreview", ImVec2(0, 0), true);

    if (gpuTexture != 0) {
        ImVec2 available = ImGui::GetContentRegionAvail();

        float aspect = (float)tex.width / tex.height;
        float displayW = available.x - 20;
        float displayH = displayW / aspect;

        if (displayH > available.y - 20) {
            displayH = available.y - 20;
            displayW = displayH * aspect;
        }

        float offsetX = (available.x - displayW) / 2.0f;
        float offsetY = (available.y - displayH) / 2.0f;

        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + offsetX);
        ImGui::SetCursorPosY(ImGui::GetCursorPosY() + offsetY);

        ImGui::Image(
            (ImTextureID)(intptr_t)gpuTexture,
            ImVec2(displayW, displayH),
            ImVec2(0, 0), ImVec2(1, 1),
            ImVec4(1, 1, 1, 1),
            ImVec4(0.5f, 0.5f, 0.5f, 1.0f));

        if (ImGui::IsItemHovered()) {
            ImGui::SetTooltip("%dx%d", tex.width, tex.height);
        }
    }
    else {
        ImGui::TextDisabled("Failed to load texture");
    }

    ImGui::EndChild();
}

void EFileTextureViewer::GeneratePreview(size_t textureIdx) {
    if (textureIdx >= m_Textures.size()) {
        return;
    }

    const NTX3Texture& tex = m_Textures[textureIdx];

    if (tex.width == 0 || tex.height == 0) {
        return;
    }

    std::vector<uint8_t> rgba;
    if (tex.format == 0x86) {
        rgba = P3TexParser::DecompressDXT1(tex.data.data(), tex.width, tex.height);
    }
    else {
        rgba = P3TexParser::DecompressDXT5(tex.data.data(), tex.width, tex.height);
    }

    m_TextureCache.GetOrCreateTexture(tex.id, rgba.data(), tex.width, tex.height);
}