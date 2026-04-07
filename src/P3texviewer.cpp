#include "P3TexViewer.h"
#include "FileDialog.h"
#include <imgui.h>
#include <iostream>
#include <cmath>
#include <filesystem>

#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../libs/stb/stb_image_write.h"

namespace fs = std::filesystem;

P3TexViewer::P3TexViewer()
    : m_Parser(nullptr),
    m_SelectedTextureId(-1),
    m_ShowDetail(false),
    m_ThumbnailSize(128),
    m_GridColumns(6),
    m_ShowExportPopup(false),
    m_ExportSuccess(false) {
}

P3TexViewer::~P3TexViewer() {
    m_TextureCache.Clear();
}

void P3TexViewer::SetParser(P3TexParser* parser) {
    m_Parser = parser;
    m_TextureCache.Clear();
    m_SelectedTextureId = -1;
    m_ShowDetail = false;
}

void P3TexViewer::Render() {
    if (!m_Parser || !m_Parser->IsLoaded()) {
        ImGui::TextDisabled("No P3TEX loaded");
        ImGui::TextWrapped("Load a .p3tex file from the 'Texture Archive' window to view textures here.");
        return;
    }

    ImGui::Text("File: %s", m_Parser->GetFilename().c_str());
    ImGui::Text("Textures: %zu", m_Parser->GetTextureCount());
    ImGui::Separator();

    // Thumbnail size slider
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
        ImGui::Text("Texture #%d", m_SelectedTextureId);
        ImGui::Separator();
        RenderDetail();
    }
    else {
        RenderGrid();
    }

    // Export popup
    if (m_ShowExportPopup) {
        ImGui::OpenPopup("Export Result");
        m_ShowExportPopup = false;
    }

    if (ImGui::BeginPopupModal("Export Result", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
        if (m_ExportSuccess) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Export Successful!");
            ImGui::Separator();
            ImGui::TextWrapped("Saved to:");
            ImGui::TextWrapped("%s", m_LastExportPath.c_str());
        }
        else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Export Cancelled or Failed");
            ImGui::Separator();
            ImGui::TextWrapped("No file was saved.");
        }

        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 0))) {
            ImGui::CloseCurrentPopup();
        }
        ImGui::EndPopup();
    }
}

void P3TexViewer::RenderGrid() {
    ImGui::BeginChild("TextureGrid", ImVec2(0, 0), false);

    size_t textureCount = m_Parser->GetTextureCount();
    int columns = m_GridColumns;

    for (size_t i = 0; i < textureCount; i++) {
        const P3Texture* tex = m_Parser->GetTexture(i);
        if (!tex || tex->width == 0 || tex->height == 0) {
            continue;
        }

        // Get or generate texture
        GLuint gpuTexture = m_TextureCache.GetOrCreateTexture(i, nullptr, 0, 0);
        if (gpuTexture == 0) {
            GeneratePreview(i);
            gpuTexture = m_TextureCache.GetOrCreateTexture(i, nullptr, 0, 0);
        }

        // Start column
        if (i % columns != 0) {
            ImGui::SameLine();
        }

        ImGui::BeginGroup();
        ImGui::PushID((int)i);

        if (gpuTexture != 0) {
            // Calculate aspect-fit size
            float aspect = (float)tex->width / tex->height;
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
            snprintf(buttonId, sizeof(buttonId), "tex_%zu", i);

            if (ImGui::ImageButton(
                buttonId,
                (ImTextureID)(intptr_t)gpuTexture,
                ImVec2(displayW, displayH),
                ImVec2(0, 0), ImVec2(1, 1),
                ImVec4(0, 0, 0, 1),
                ImVec4(1, 1, 1, 1))) {
                m_SelectedTextureId = (int)i;
                m_ShowDetail = true;
            }

            ImGui::EndGroup();
        }
        else {
            ImGui::Button("...", ImVec2(m_ThumbnailSize, m_ThumbnailSize));
            if (ImGui::IsItemHovered()) {
                ImGui::SetTooltip("Failed to load texture");
            }
        }

        ImGui::PopID();

        // Label
        ImGui::Text("#%zu", i);
        ImGui::SameLine();
        ImGui::TextDisabled("%dx%d", tex->width, tex->height);

        ImGui::EndGroup();
    }

    ImGui::EndChild();
}

void P3TexViewer::RenderDetail() {
    if (m_SelectedTextureId < 0 || m_SelectedTextureId >= (int)m_Parser->GetTextureCount()) {
        ImGui::Text("Invalid texture ID");
        return;
    }

    const P3Texture* tex = m_Parser->GetTexture(m_SelectedTextureId);
    if (!tex) {
        ImGui::Text("Texture not found");
        return;
    }

    GLuint gpuTexture = m_TextureCache.GetOrCreateTexture(m_SelectedTextureId, nullptr, 0, 0);

    // Info panel
    ImGui::BeginChild("TextureInfo", ImVec2(300, 0), true);

    ImGui::Text("Texture #%d", m_SelectedTextureId);
    ImGui::Separator();

    ImGui::Text("Dimensions: %dx%d", tex->width, tex->height);
    ImGui::Text("Data Size: %zu bytes", tex->size);
    ImGui::Text("Format: %s", (tex->format & 0xDF) == 0x86 ? "DXT1" : (tex->format == 0xA5 ? "RGBA8" : "DXT5"));

    float aspect = (float)tex->width / tex->height;
    ImGui::Text("Aspect Ratio: %.2f:1", aspect);

    ImGui::Separator();

    // Export button
    if (ImGui::Button("Export as PNG", ImVec2(-1, 0))) {
        ExportTexturePNG(m_SelectedTextureId);
    }

    // Navigation
    ImGui::Separator();
    if (ImGui::Button("< Previous", ImVec2(-1, 0)) && m_SelectedTextureId > 0) {
        m_SelectedTextureId--;
    }
    if (ImGui::Button("Next >", ImVec2(-1, 0)) && m_SelectedTextureId < (int)m_Parser->GetTextureCount() - 1) {
        m_SelectedTextureId++;
    }

    ImGui::EndChild();

    // Preview
    ImGui::SameLine();

    ImGui::BeginChild("TexturePreview", ImVec2(0, 0), true);

    if (gpuTexture != 0) {
        ImVec2 available = ImGui::GetContentRegionAvail();

        // Calculate fit size
        float aspect = (float)tex->width / tex->height;
        float displayW = available.x - 20;
        float displayH = displayW / aspect;

        if (displayH > available.y - 20) {
            displayH = available.y - 20;
            displayW = displayH * aspect;
        }

        // Center preview
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
            ImGui::SetTooltip("Actual size: %dx%d", tex->width, tex->height);
        }
    }
    else {
        ImGui::TextDisabled("Failed to load texture");
    }

    ImGui::EndChild();
}

void P3TexViewer::GeneratePreview(uint8_t textureId) {
    const P3Texture* tex = m_Parser->GetTexture(textureId);
    if (!tex || tex->width == 0 || tex->height == 0) {
        return;
    }

    std::vector<uint8_t> rgba;
    if ((tex->format & 0xDF) == 0x86) {
        rgba = P3TexParser::DecompressDXT1(tex->data.data(), tex->width, tex->height);
    }
    else if (tex->format == 0xA5) {
        rgba = P3TexParser::DecompressRGBA8(tex->data.data(), tex->width, tex->height);
    }
    else {
        rgba = P3TexParser::DecompressDXT5(tex->data.data(), tex->width, tex->height);
    }

    m_TextureCache.GetOrCreateTexture(textureId, rgba.data(), tex->width, tex->height);
}

void P3TexViewer::ExportTexturePNG(uint8_t textureId) {
    const P3Texture* tex = m_Parser->GetTexture(textureId);
    if (!tex || tex->width == 0 || tex->height == 0) {
        std::cerr << "Invalid texture for export" << std::endl;
        m_ExportSuccess = false;
        m_ShowExportPopup = true;
        return;
    }

    // Generate default filename
    char defaultFilename[256];
    snprintf(defaultFilename, sizeof(defaultFilename), "texture_%03d_%dx%d.png",
        textureId, tex->width, tex->height);

    // Open save dialog
    std::string savePath = FileDialog::SaveFile("PNG Image\0*.png\0All Files\0*.*\0");

    // User cancelled
    if (savePath.empty()) {
        std::cout << "Export cancelled by user" << std::endl;
        m_ExportSuccess = false;
        m_ShowExportPopup = true;
        return;
    }

    // Ensure .png extension
    if (savePath.find(".png") == std::string::npos) {
        savePath += ".png";
    }

    std::cout << "Exporting texture to: " << savePath << std::endl;

    // Decompress texture
    std::vector<uint8_t> rgba;
    if ((tex->format & 0xDF) == 0x86) {
        rgba = P3TexParser::DecompressDXT1(tex->data.data(), tex->width, tex->height);
    }
    else if (tex->format == 0xA5) {
        rgba = P3TexParser::DecompressRGBA8(tex->data.data(), tex->width, tex->height);
    }
    else {
        rgba = P3TexParser::DecompressDXT5(tex->data.data(), tex->width, tex->height);
    }

    if (rgba.empty()) {
        std::cerr << "Failed to decompress texture" << std::endl;
        m_ExportSuccess = false;
        m_ShowExportPopup = true;
        return;
    }

    // Write PNG
    int result = stbi_write_png(savePath.c_str(), tex->width, tex->height, 4, rgba.data(), tex->width * 4);

    if (result) {
        std::cout << "Successfully exported: " << savePath << std::endl;
        m_ExportSuccess = true;
        m_LastExportPath = savePath;
    }
    else {
        std::cerr << "Failed to write PNG file" << std::endl;
        m_ExportSuccess = false;
    }

    m_ShowExportPopup = true;
}