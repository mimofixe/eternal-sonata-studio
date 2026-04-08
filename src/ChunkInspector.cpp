#include "ChunkInspector.h"
#include "NSHPParser.h"
#include "NMDLLoader.h"
#include <imgui.h>
#include <cstring>
#include <iostream>
#include "Viewport3D.h"
#include "FileDialog.h"
#include "Ntx3parser.h"

ChunkInspector::ChunkInspector()
    : m_HasChunk(false), m_Viewport(nullptr), m_P3TexParser(nullptr),
    m_MeshCached(false), m_LastChunkOffset(0) {
}

ChunkInspector::~ChunkInspector() {
    m_TextureCache.Clear();
    for (GLuint t : m_MapTextures) if (t) glDeleteTextures(1, &t);
}

void ChunkInspector::SetViewport(Viewport3D* viewport) {
    m_Viewport = viewport;
}

void ChunkInspector::SetP3TexParser(P3TexParser* parser) {
    m_P3TexParser = parser;
}

void ChunkInspector::SetChunk(const Chunk& chunk, const std::vector<uint8_t>& file_data) {
    m_CurrentChunk = chunk;
    m_HasChunk = true;

    // Invalidar cache se mudou de chunk
    if (m_LastChunkOffset != chunk.offset) {
        m_MeshCached = false;
        m_LastChunkOffset = chunk.offset;
    }

    size_t chunk_start = chunk.offset;
    size_t chunk_end = chunk_start + 24 + chunk.size;

    if (chunk_end <= file_data.size()) {
        m_ChunkData.clear();
        m_ChunkData.insert(m_ChunkData.end(),
            file_data.begin() + chunk_start,
            file_data.begin() + chunk_end);

        // Keep a reference to the full file data for NMDL loading
        m_FileData = &file_data;

        memset(&m_CameraData, 0, sizeof(m_CameraData));
        memset(&m_LightData, 0, sizeof(m_LightData));
        memset(&m_FogData, 0, sizeof(m_FogData));
        memset(&m_MaterialData, 0, sizeof(m_MaterialData));

        switch (chunk.type) {
        case ChunkType::NCAM:
            EFileParser::ParseCamera(m_ChunkData.data(), m_ChunkData.size(), m_CameraData);
            break;
        case ChunkType::NLIT:
            EFileParser::ParseLight(m_ChunkData.data(), m_ChunkData.size(), m_LightData);
            break;
        case ChunkType::NFOG:
            EFileParser::ParseFog(m_ChunkData.data(), m_ChunkData.size(), m_FogData);
            break;
        case ChunkType::NMTR:
            EFileParser::ParseMaterial(m_ChunkData.data(), m_ChunkData.size(), m_MaterialData);
            break;
        default:
            break;
        }
    }
}

void ChunkInspector::Render() {
    if (!m_HasChunk) {
        ImGui::TextDisabled("Select a chunk to inspect");
        return;
    }

    if (ImGui::BeginTabBar("InspectorTabs")) {
        if (ImGui::BeginTabItem("Properties")) {
            RenderProperties();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Hex View")) {
            RenderHexView();
            ImGui::EndTabItem();
        }

        if (m_CurrentChunk.type == ChunkType::NSHP && ImGui::BeginTabItem("Mesh Info")) {
            RenderNSHPInfo();
            ImGui::EndTabItem();
        }

        if (m_CurrentChunk.type == ChunkType::NTX3 && ImGui::BeginTabItem("Texture Info")) {
            RenderNTX3Info();
            ImGui::EndTabItem();
        }

        if (m_CurrentChunk.type == ChunkType::NBN2 && ImGui::BeginTabItem("Skeleton Info")) {
            RenderNBN2Info();
            ImGui::EndTabItem();
        }

        if (m_CurrentChunk.type == ChunkType::NMDL && ImGui::BeginTabItem("Model Info")) {
            RenderNMDLInfo();
            ImGui::EndTabItem();
        }

        if (m_CurrentChunk.type == ChunkType::NCAM && ImGui::BeginTabItem("Camera Info")) {
            RenderCameraInfo();
            ImGui::EndTabItem();
        }

        if (m_CurrentChunk.type == ChunkType::NLIT && ImGui::BeginTabItem("Light Info")) {
            RenderLightInfo();
            ImGui::EndTabItem();
        }

        if (m_CurrentChunk.type == ChunkType::NFOG && ImGui::BeginTabItem("Fog Info")) {
            RenderFogInfo();
            ImGui::EndTabItem();
        }

        if (m_CurrentChunk.type == ChunkType::NMTR && ImGui::BeginTabItem("Material Info")) {
            RenderMaterialInfo();
            ImGui::EndTabItem();
        }

        if (m_CurrentChunk.type == ChunkType::NMTN && ImGui::BeginTabItem("Animation Info")) {
            RenderAnimationInfo();
            ImGui::EndTabItem();
        }

        if (ImGui::BeginTabItem("Text View")) {
            RenderTextView();
            ImGui::EndTabItem();
        }

        ImGui::EndTabBar();
    }
}

void ChunkInspector::RenderProperties() {
    ImGui::Text("Type: %s", m_CurrentChunk.GetTypeString().c_str());
    ImGui::Text("Description: %s", m_CurrentChunk.GetDescription().c_str());
    ImGui::Text("Name: %s", m_CurrentChunk.name);
    ImGui::Text("Offset: 0x%zX (%zu)", m_CurrentChunk.offset, m_CurrentChunk.offset);
    ImGui::Text("Size: %u bytes", m_CurrentChunk.size);

    if (m_CurrentChunk.size < 1024) {
        ImGui::Text("Size (formatted): %u B", m_CurrentChunk.size);
    }
    else if (m_CurrentChunk.size < 1024 * 1024) {
        ImGui::Text("Size (formatted): %.2f KB", m_CurrentChunk.size / 1024.0f);
    }
    else {
        ImGui::Text("Size (formatted): %.2f MB", m_CurrentChunk.size / (1024.0f * 1024.0f));
    }
}

void ChunkInspector::RenderHexView() {
    if (m_ChunkData.empty()) {
        ImGui::Text("No data");
        return;
    }

    ImGui::BeginChild("ChunkHex", ImVec2(0, 0), true);

    size_t display_size = m_ChunkData.size() < 4096 ? m_ChunkData.size() : 4096;

    for (size_t i = 0; i < display_size; i += 16) {
        ImGui::Text("%04zX:", i);
        ImGui::SameLine();

        for (size_t j = 0; j < 16 && i + j < display_size; j++) {
            ImGui::SameLine();
            ImGui::Text("%02X", m_ChunkData[i + j]);
        }

        ImGui::SameLine(350);
        ImGui::Text("|");
        ImGui::SameLine();

        for (size_t j = 0; j < 16 && i + j < display_size; j++) {
            char c = m_ChunkData[i + j];
            ImGui::SameLine();
            ImGui::Text("%c", (c >= 32 && c < 127) ? c : '.');
        }
    }

    if (display_size < m_ChunkData.size()) {
        ImGui::TextDisabled("... (%zu more bytes)", m_ChunkData.size() - display_size);
    }

    ImGui::EndChild();
}

void ChunkInspector::RenderNSHPInfo() {
    ImGui::Text("3D Mesh Data");
    ImGui::Separator();

    if (m_ChunkData.size() < 0x38) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Chunk too small");
        return;
    }

    ImGui::Text("Vertex data starts at: 0x38");

    if (ImGui::Button("Parse Mesh")) {
        NSHPMesh mesh;
        if (NSHPParser::Parse(m_ChunkData.data(), m_ChunkData.size(), mesh)) {
            std::cout << "Successfully parsed mesh!" << std::endl;
            std::cout << "  Name: " << mesh.name << std::endl;
            std::cout << "  Vertices: " << mesh.vertices.size() << std::endl;
            std::cout << "  Has skinning: " << (mesh.has_skinning ? "Yes" : "No") << std::endl;

            if (mesh.has_texture) {
                std::cout << "  Texture ID: " << (int)mesh.texture_id << std::endl;
            }

            if (!mesh.vertices.empty()) {
                std::cout << "  First vertex:" << std::endl;
                std::cout << "    Pos: " << mesh.vertices[0].position[0] << ", "
                    << mesh.vertices[0].position[1] << ", "
                    << mesh.vertices[0].position[2] << std::endl;
            }
        }
    }

    ImGui::SameLine();

    if (ImGui::Button("Load in Viewport")) {
        if (m_Viewport) {
            NSHPMesh mesh;
            if (NSHPParser::Parse(m_ChunkData.data(), m_ChunkData.size(), mesh)) {
                m_Viewport->LoadMesh(mesh);
                std::cout << "Mesh loaded into viewport!" << std::endl;
            }
            else {
                std::cout << "Failed to parse mesh for viewport" << std::endl;
            }
        }
        else {
            std::cout << "No viewport available" << std::endl;
        }
    }

    // TEXTURE INFORMATION COM CACHE
    ImGui::Separator();
    ImGui::Text("Texture Information:");

    // Parse mesh apenas uma vez (usar cache)
    if (!m_MeshCached) {
        m_MeshCached = NSHPParser::Parse(m_ChunkData.data(), m_ChunkData.size(), m_CachedMesh);
    }

    if (m_MeshCached) {
        if (m_CachedMesh.has_texture) {
            ImGui::Text("Texture ID: %d", m_CachedMesh.texture_id);
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[OK]");

            if (m_P3TexParser && m_P3TexParser->IsLoaded()) {
                const P3Texture* tex = m_P3TexParser->GetTexture(m_CachedMesh.texture_id);

                if (tex && tex->width > 0 && tex->height > 0) {
                    ImGui::Indent();
                    ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[OK] Texture found in archive");
                    ImGui::Text("Dimensions: %dx%d", tex->width, tex->height);
                    ImGui::Text("Data size: %zu bytes (%.2f KB)", tex->size, tex->size / 1024.0f);

                    if ((tex->format & 0xDF) == 0x86) {
                        ImGui::Text("Format: DXT1 compressed");
                    }
                    else {
                        ImGui::Text("Format: DXT5 compressed");
                    }

                    ImGui::Separator();

                    static bool previewGenerated = false;
                    static uint8_t lastTextureId = 255;

                    if (lastTextureId != m_CachedMesh.texture_id) {
                        previewGenerated = false;
                        lastTextureId = m_CachedMesh.texture_id;
                    }

                    if (!previewGenerated) {
                        if (ImGui::Button("Generate Preview", ImVec2(150, 0))) {
                            std::cout << "Decompressing texture " << (int)m_CachedMesh.texture_id << "..." << std::endl;

                            std::vector<uint8_t> rgba;
                            if ((tex->format & 0xDF) == 0x86) {
                                std::cout << "  Format: DXT1" << std::endl;
                                rgba = P3TexParser::DecompressDXT1(tex->data.data(), tex->width, tex->height);
                            }
                            else {
                                std::cout << "  Format: DXT5" << std::endl;
                                rgba = P3TexParser::DecompressDXT5(tex->data.data(), tex->width, tex->height);
                            }

                            m_TextureCache.GetOrCreateTexture(
                                m_CachedMesh.texture_id, rgba.data(), tex->width, tex->height);

                            previewGenerated = true;
                            std::cout << "Preview generated!" << std::endl;
                        }

                        ImGui::SameLine();
                        ImGui::TextDisabled("(Click to decompress)");
                    }
                    else {
                        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[OK] Preview ready");
                    }

                    if (previewGenerated) {
                        GLuint gpuTexture = m_TextureCache.GetOrCreateTexture(
                            m_CachedMesh.texture_id, nullptr, 0, 0);

                        if (gpuTexture != 0) {
                            ImGui::Separator();
                            ImGui::Text("Texture Preview:");

                            float aspect = (float)tex->width / tex->height;
                            float preview_width = 256.0f;
                            float preview_height = preview_width / aspect;

                            if (preview_height > 256.0f) {
                                preview_height = 256.0f;
                                preview_width = preview_height * aspect;
                            }

                            ImGui::Image((void*)(intptr_t)gpuTexture,
                                ImVec2(preview_width, preview_height),
                                ImVec2(0, 0), ImVec2(1, 1),
                                ImVec4(1, 1, 1, 1),
                                ImVec4(0.3f, 0.3f, 0.3f, 1.0f));

                            ImGui::TextDisabled("(Actual size: %dx%d)", tex->width, tex->height);
                        }
                    }

                    ImGui::Unindent();

                }
                else if (tex) {
                    ImGui::Indent();
                    ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "[WARN] Could not determine dimensions");
                    ImGui::Text("Data size: %zu bytes", tex->size);
                    ImGui::Text("Cannot generate preview without dimensions");
                    ImGui::Unindent();

                }
                else {
                    ImGui::Indent();
                    ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "[ERROR] Texture ID out of range");
                    ImGui::Text("  Archive has %zu textures", m_P3TexParser->GetTextureCount());
                    ImGui::Unindent();
                }
            }
            else {
                ImGui::Indent();
                ImGui::TextDisabled("(Load P3TEX to verify)");
                ImGui::Unindent();
            }
        }
        else {
            ImGui::TextDisabled("No texture assigned (ID = 0)");
        }
    }
    else {
        ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Failed to parse mesh");
    }

    ImGui::Separator();
    ImGui::TextDisabled("Vertex format: stride 24-48 bytes");
    ImGui::TextDisabled("  - Position: 3x float32 (big-endian)");
    ImGui::TextDisabled("  - Normal: 3x int8");
    ImGui::TextDisabled("  - UV: 2x uint16 (big-endian)");
    ImGui::TextDisabled("  - [Optional] Bone weights + IDs (skinned meshes)");
}

void ChunkInspector::RenderNMDLInfo() {
    ImGui::Text("NMDL Model");
    ImGui::Separator();

    // Header info: NMDL w[2]=mat_count at +0x22, w[4]=bone_count at +0x26
    if (m_ChunkData.size() >= 0x30) {
        auto u16be = [](const uint8_t* d) { return uint16_t((d[0] << 8) | d[1]); };
        const uint8_t* hdr = m_ChunkData.data();
        uint16_t mat_count = u16be(hdr + 0x22);
        uint16_t bone_count = u16be(hdr + 0x26);
        ImGui::Text("Materials: %d", (int)mat_count);
        ImGui::Text("Bones:     %d", (int)bone_count);
        ImGui::Text("Size:      %.2f MB", m_CurrentChunk.size / (1024.0f * 1024.0f));
    }

    ImGui::Separator();

    if (!m_Viewport) {
        ImGui::TextDisabled("(no viewport connected)");
        return;
    }

    // ── Map texture loading ───────────────────────────────────────────────────
    int expected_tex_count = 0;
    if (m_FileData && m_ChunkData.size() >= 8) {
        auto u16be_fn = [](const uint8_t* d) { return uint16_t((d[0] << 8) | d[1]); };
        auto u32be_fn = [](const uint8_t* d) {
            return (uint32_t(d[0]) << 24) | (uint32_t(d[1]) << 16) | (uint32_t(d[2]) << 8) | d[3]; };
        const uint8_t* fd = m_FileData->data();
        size_t fs = m_FileData->size();
        size_t nmdl_end = std::min(
            m_CurrentChunk.offset + size_t(m_CurrentChunk.size), fs);
        size_t p2 = m_CurrentChunk.offset + 8;
        while (p2 + 8 <= nmdl_end) {
            const uint8_t* pp = fd + p2;
            uint32_t s2 = u32be_fn(pp + 4);
            if (s2 >= 8 && p2 + s2 <= nmdl_end) {
                if (memcmp(pp, "NMTR", 4) == 0) {
                    int mc = (int)((s2 - 8) / 96);
                    int mx = -1;
                    for (int m = 0; m < mc; m++) {
                        const uint8_t* entry = pp + 8 + m * 96;
                        if (u16be_fn(entry + 0x0A) == 1) {
                            int idx = (int)u16be_fn(entry + 0x04);
                            if (idx > mx) mx = idx;
                        }
                    }
                    if (mx >= 0) expected_tex_count = mx + 1;
                    break;
                }
                p2 += s2;
            }
            else { p2 += 4; }
        }
    }

    ImGui::Text("Map textures (.p3tex):");
    if (expected_tex_count > 0 && m_MapTextures.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(1.0f, 0.85f, 0.2f, 1.0f),
            "(needs p3tex with >= %d textures)", expected_tex_count);
    }

    if (!m_MapTextures.empty()) {
        ImGui::SameLine();
        ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f),
            "%d textures loaded", (int)m_MapTextures.size());
        if (ImGui::SmallButton("Clear##maptex")) {
            for (GLuint t : m_MapTextures) if (t) glDeleteTextures(1, &t);
            m_MapTextures.clear();
            m_MapTexDXT5.clear();
        }
    }
    else {
        ImGui::SameLine();
        ImGui::TextDisabled("none");
    }

    if (ImGui::Button("Load .p3tex for this map...", ImVec2(-1, 0))) {
        std::string path = FileDialog::OpenFile(
            "P3TEX Files\0*.p3tex\0All Files\0*.*\0");
        if (!path.empty()) {
            P3TexParser tempParser;
            if (tempParser.Load(path)) {
                m_MapTextures.clear();
                m_MapTexDXT5.clear();
                // Upload all textures to GPU and collect their handles
                for (size_t i = 0; i < tempParser.GetTextureCount(); i++) {
                    const P3Texture* tex = tempParser.GetTexture((uint8_t)i);
                    if (!tex || tex->width == 0 || tex->height == 0) {
                        m_MapTextures.push_back(0);
                        m_MapTexDXT5.push_back(false);
                        continue;
                    }
                    std::vector<uint8_t> rgba;
                    if ((tex->format & 0xDF) == 0x86)
                        rgba = P3TexParser::DecompressDXT1(
                            tex->data.data(), tex->width, tex->height);
                    else
                        rgba = P3TexParser::DecompressDXT5(
                            tex->data.data(), tex->width, tex->height);

                    GLuint glTex = 0;
                    if (!rgba.empty()) {
                        glGenTextures(1, &glTex);
                        glBindTexture(GL_TEXTURE_2D, glTex);
                        glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA,
                            tex->width, tex->height, 0,
                            GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
                        // No mipmaps — atlas textures must not be mip-filtered.
                        // glGenerateMipmap on an atlas produces garbage mip levels
                        // which GL_LINEAR_MIPMAP_LINEAR then samples → rainbow artefacts.
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S,
                            GL_REPEAT);
                        glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T,
                            GL_REPEAT);
                        glBindTexture(GL_TEXTURE_2D, 0);
                    }
                    m_MapTextures.push_back(glTex);
                    m_MapTexDXT5.push_back(tex->format != 0x86);
                }
                std::cout << "[ChunkInspector] Loaded " << m_MapTextures.size()
                    << " map textures from " << path << "\n";
            }
        }
    }

    ImGui::Separator();

    // ── Load model button ─────────────────────────────────────────────────────
    if (ImGui::Button("Load Textured Model in Viewport", ImVec2(-1, 0))) {
        if (m_FileData && !m_FileData->empty()) {

            // Scan file for NLIT chunk and extract scene lighting
            if (m_Viewport) {
                m_Viewport->ClearSceneLighting();
                uint8_t amb_r = 180, amb_g = 180, amb_b = 180;
                uint8_t dir_r = 220, dir_g = 220, dir_b = 220;
                bool found_amb = false, found_dir = false;
                const uint8_t* fd = m_FileData->data();
                size_t fs = m_FileData->size();
                size_t sp = 0;
                while (sp + 8 <= fs) {
                    if (memcmp(fd + sp, "NLIT", 4) == 0) {
                        uint32_t nsz = (uint32_t(fd[sp + 4]) << 24) | (uint32_t(fd[sp + 5]) << 16) | (uint32_t(fd[sp + 6]) << 8) | fd[sp + 7];
                        size_t ep = sp + 8;
                        while (ep + 48 <= sp + nsz) {
                            const uint8_t* entry = fd + ep;
                            char ename[17]{}; memcpy(ename, entry, 16);
                            uint8_t type_b = entry[16];
                            uint8_t r = entry[20], g = entry[21], b = entry[22];
                            if (type_b == 0x00 && !found_amb) { // ambient
                                amb_r = r; amb_g = g; amb_b = b; found_amb = true;
                            }
                            else if (type_b == 0x01 && !found_dir) { // directional
                                dir_r = r; dir_g = g; dir_b = b; found_dir = true;
                            }
                            ep += 48;
                        }
                        break;
                    }
                    sp += 4;
                }
                if (found_amb || found_dir)
                    m_Viewport->SetSceneLighting(amb_r, amb_g, amb_b, dir_r, dir_g, dir_b);
            }

            NMDLModel mdl;
            const std::vector<GLuint>* extTex =
                m_MapTextures.empty() ? nullptr : &m_MapTextures;

            // Pass the DXT5 flag vector so NMDLLoader can enable GL_BLEND
            // for materials whose diffuse texture uses full 8-bit alpha (DXT5).
            const std::vector<bool>* extDXT5ptr = nullptr;
            if (extTex && !m_MapTexDXT5.empty())
                extDXT5ptr = &m_MapTexDXT5;

            bool ok = NMDLLoader::Load(
                m_FileData->data(),
                m_FileData->size(),
                m_CurrentChunk.offset,
                mdl,
                extTex,
                extDXT5ptr);

            if (ok) {
                m_Viewport->LoadModel(mdl);
                std::cout << "[ChunkInspector] NMDL model loaded into viewport\n";
            }
            else {
                std::cerr << "[ChunkInspector] NMDLLoader::Load failed — check console\n";
                ImGui::OpenPopup("NMDL Load Error");
            }
        }
        else {
            std::cerr << "[ChunkInspector] File data not available\n";
        }
    }

    if (ImGui::BeginPopupModal("NMDL Load Error", nullptr,
        ImGuiWindowFlags_AlwaysAutoResize)) {
        ImGui::Text("Failed to load NMDL model.\nCheck the console for details.");
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }

    if (m_Viewport->HasModel()) {
        ImGui::Spacing();
        if (ImGui::Button("Clear Model", ImVec2(-1, 0)))
            m_Viewport->ClearModel();
    }
}

void ChunkInspector::RenderNTX3Info() {
    ImGui::Text("NTX3 Texture Data");
    ImGui::Separator();

    if (m_ChunkData.size() < 128) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Chunk too small for NTX3");
        return;
    }

    const uint8_t* header = m_ChunkData.data();

    if (memcmp(header, "NTX3", 4) != 0) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Invalid NTX3 magic");
        return;
    }

    uint32_t chunk_size = (header[4] << 24) | (header[5] << 16) | (header[6] << 8) | header[7];

    uint16_t width = (header[0x20] << 8) | header[0x21];
    uint16_t height = (header[0x22] << 8) | header[0x23];
    uint8_t format_byte = header[0x18];

    size_t data_size = m_ChunkData.size() - 128;

    ImGui::Text("Chunk Size: %u bytes", chunk_size);
    ImGui::Text("Data Size: %zu bytes (%.2f KB)", data_size, data_size / 1024.0f);
    ImGui::Separator();

    if (width > 0 && height > 0) {
        ImGui::Text("Dimensions: %dx%d", width, height);

        if ((format_byte & 0xDF) == 0x86) {
            ImGui::Text("Format: DXT1 compressed");
        }
        else {
            ImGui::Text("Format: DXT5 compressed");
        }

        ImGui::Text("Dimensions (hex): 0x%04X x 0x%04X", width, height);
        ImGui::Text("Format byte: 0x%02X", format_byte);

        ImGui::Separator();

        static bool previewGenerated = false;
        static size_t lastChunkOffset = 0;
        static GLuint cachedTexture = 0;

        if (lastChunkOffset != m_CurrentChunk.offset) {
            previewGenerated = false;
            lastChunkOffset = m_CurrentChunk.offset;
            if (cachedTexture != 0) {
                glDeleteTextures(1, &cachedTexture);
                cachedTexture = 0;
            }
        }

        if (!previewGenerated) {
            if (ImGui::Button("Generate Preview", ImVec2(150, 0))) {
                std::cout << "Decompressing NTX3 texture (" << width << "x" << height << ")..." << std::endl;

                // NTX3 pixel data starts at +0x88 (136-byte header).
                // Use NTX3Parser (no block-swap) — P3TexParser::DecompressDXT5 has a
                // swap only valid for .p3tex external textures, not .bmd/.e inline NTX3.
                const uint8_t* dxt_data = m_ChunkData.data() + 0x88;
                std::vector<uint8_t> rgba;
                if ((format_byte & 0xDF) == 0x86) {
                    std::cout << "  Format: DXT1" << std::endl;
                    rgba.resize(size_t(width) * height * 4, 0);
                    NTX3Parser::DecompressDXT1(dxt_data, width, height, rgba);
                }
                else if (format_byte == 0xA5) {
                    // Raw RGBA8 — actual height = data_size / (width * 4)
                    const size_t actual_h = data_size / (width * 4);
                    height = static_cast<uint16_t>(actual_h);
                    std::cout << "  Format: RGBA8 raw (" << width << "x" << height << ")" << std::endl;
                    rgba.resize(size_t(width) * height * 4, 0);
                    std::memcpy(rgba.data(), dxt_data, rgba.size());
                }
                else {
                    // All PS3 Namco DXT5 use swapped block layout (colour first, alpha second).
                    std::cout << "  Format: DXT5" << std::endl;
                    rgba.resize(size_t(width) * height * 4, 0);
                    NTX3Parser::DecompressDXT5(dxt_data, width, height, rgba);
                }

                glGenTextures(1, &cachedTexture);
                glBindTexture(GL_TEXTURE_2D, cachedTexture);
                glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, width, height, 0, GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_BASE_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAX_LEVEL, 0);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_LINEAR);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
                glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
                glBindTexture(GL_TEXTURE_2D, 0);

                previewGenerated = true;
                std::cout << "Preview generated!" << std::endl;
            }

            ImGui::SameLine();
            ImGui::TextDisabled("(Click to decompress)");
        }
        else {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "[OK] Preview ready");
        }

        if (previewGenerated && cachedTexture != 0) {
            ImGui::Separator();
            ImGui::Text("Texture Preview:");

            float aspect = (float)width / height;
            float preview_width = 512.0f;
            float preview_height = preview_width / aspect;

            if (preview_height > 512.0f) {
                preview_height = 512.0f;
                preview_width = preview_height * aspect;
            }

            if (preview_width < 128.0f) {
                preview_width = 128.0f;
                preview_height = preview_width / aspect;
            }

            ImGui::Image((void*)(intptr_t)cachedTexture,
                ImVec2(preview_width, preview_height),
                ImVec2(0, 0), ImVec2(1, 1),
                ImVec4(1, 1, 1, 1),
                ImVec4(0.3f, 0.3f, 0.3f, 1.0f));

            ImGui::TextDisabled("(Actual size: %dx%d)", width, height);

            ImGui::Separator();
            if (ImGui::Button("Export as DDS", ImVec2(120, 0))) {
                ImGui::OpenPopup("Export DDS");
            }

            if (ImGui::BeginPopupModal("Export DDS", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                ImGui::Text("DDS export feature coming soon!");
                ImGui::Text("For now, you can screenshot the preview.");
                ImGui::Separator();
                if (ImGui::Button("OK", ImVec2(120, 0))) {
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

    }
    else {
        ImGui::TextColored(ImVec4(1.0f, 1.0f, 0.4f, 1.0f), "[WARN] Could not read dimensions from header");
        ImGui::Text("Dimensions (hex): 0x%04X x 0x%04X", width, height);
        ImGui::Separator();
        ImGui::TextDisabled("Cannot generate preview without valid dimensions");

        int deduced_width = 0, deduced_height = 0;
        if (P3TexParser::DeduceDimensions(data_size, deduced_width, deduced_height)) {
            ImGui::Separator();
            ImGui::Text("Deduced dimensions: %dx%d", deduced_width, deduced_height);
            ImGui::TextDisabled("(These dimensions were calculated from data size)");
        }
    }

    ImGui::Separator();
    ImGui::TextDisabled("NTX3 Structure:");
    ImGui::BulletText("Header: 128 bytes");
    ImGui::BulletText("  - Magic: NTX3 (0x00-0x03)");
    ImGui::BulletText("  - Size: big-endian uint32 (0x04-0x07)");
    ImGui::BulletText("  - Format: 0x86 = DXT1, other = DXT5 (0x18)");
    ImGui::BulletText("  - Dimensions: width/256 @ 0x20, height/256 @ 0x22");
    ImGui::BulletText("Texture data: 88-byte header + DXT compressed + mipmaps");
}

void ChunkInspector::RenderCameraInfo() {
    if (!m_CameraData.parsed) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Failed to parse camera data");
        return;
    }

    ImGui::Text("Camera Properties");
    ImGui::Separator();

    ImGui::Text("Position:");
    ImGui::BulletText("X: %.3f", m_CameraData.position[0]);
    ImGui::BulletText("Y: %.3f", m_CameraData.position[1]);
    ImGui::BulletText("Z: %.3f", m_CameraData.position[2]);

    ImGui::Spacing();
    ImGui::Text("Rotation:");
    ImGui::BulletText("X: %.3f", m_CameraData.rotation[0]);
    ImGui::BulletText("Y: %.3f", m_CameraData.rotation[1]);
    ImGui::BulletText("Z: %.3f", m_CameraData.rotation[2]);

    ImGui::Spacing();
    ImGui::Text("FOV: %.2f", m_CameraData.fov);
}

void ChunkInspector::RenderLightInfo() {
    if (!m_LightData.parsed) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Failed to parse light data");
        return;
    }

    ImGui::Text("Light Properties");
    ImGui::Separator();

    ImGui::Text("Name: %s", m_LightData.light_name);
    ImGui::Text("Type: %s", m_LightData.is_ambient ? "Ambient" : "Directional");

    ImGui::Spacing();
    ImGui::Text("Color:");
    ImGui::BulletText("R: %d", m_LightData.color[0]);
    ImGui::BulletText("G: %d", m_LightData.color[1]);
    ImGui::BulletText("B: %d", m_LightData.color[2]);

    ImVec4 color(m_LightData.color[0] / 255.0f,
        m_LightData.color[1] / 255.0f,
        m_LightData.color[2] / 255.0f, 1.0f);
    ImGui::ColorButton("Light Color", color, 0, ImVec2(50, 50));

    if (!m_LightData.is_ambient) {
        ImGui::Spacing();
        ImGui::Text("Direction:");
        ImGui::BulletText("X: %.3f", m_LightData.direction[0]);
        ImGui::BulletText("Y: %.3f", m_LightData.direction[1]);
        ImGui::BulletText("Z: %.3f", m_LightData.direction[2]);
    }
}

void ChunkInspector::RenderFogInfo() {
    if (!m_FogData.parsed) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Failed to parse fog data");
        return;
    }

    ImGui::Text("Fog Properties");
    ImGui::Separator();

    ImGui::Text("Color:");
    ImGui::BulletText("R: %d", m_FogData.color[0]);
    ImGui::BulletText("G: %d", m_FogData.color[1]);
    ImGui::BulletText("B: %d", m_FogData.color[2]);

    ImVec4 color(m_FogData.color[0] / 255.0f,
        m_FogData.color[1] / 255.0f,
        m_FogData.color[2] / 255.0f, 1.0f);
    ImGui::ColorButton("Fog Color", color, 0, ImVec2(50, 50));

    ImGui::Spacing();
    ImGui::Text("Near Distance: %.2f", m_FogData.near_distance);
    ImGui::Text("Far Distance: %.2f", m_FogData.far_distance);
}

void ChunkInspector::RenderMaterialInfo() {
    if (!m_MaterialData.parsed) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Failed to parse material data");
        return;
    }

    ImGui::Text("Material Properties");
    ImGui::Separator();

    ImGui::Text("Flags: 0x%08X", m_MaterialData.flags);

    ImGui::Spacing();
    ImGui::Text("Diffuse Color:");
    ImVec4 diffuse(m_MaterialData.diffuse_color[0] / 255.0f,
        m_MaterialData.diffuse_color[1] / 255.0f,
        m_MaterialData.diffuse_color[2] / 255.0f, 1.0f);
    ImGui::ColorButton("Diffuse", diffuse, 0, ImVec2(50, 50));

    ImGui::Spacing();
    ImGui::Text("Specular Color:");
    ImVec4 specular(m_MaterialData.specular_color[0] / 255.0f,
        m_MaterialData.specular_color[1] / 255.0f,
        m_MaterialData.specular_color[2] / 255.0f, 1.0f);
    ImGui::ColorButton("Specular", specular, 0, ImVec2(50, 50));

    ImGui::Spacing();
    ImGui::Text("Ambient Color:");
    ImVec4 ambient(m_MaterialData.ambient_color[0] / 255.0f,
        m_MaterialData.ambient_color[1] / 255.0f,
        m_MaterialData.ambient_color[2] / 255.0f, 1.0f);
    ImGui::ColorButton("Ambient", ambient, 0, ImVec2(50, 50));
}

void ChunkInspector::RenderNBN2Info() {
    ImGui::Text("Skeleton Bone Data");
    ImGui::Separator();

    if (m_ChunkData.size() < 72) {
        ImGui::TextColored(ImVec4(1, 0, 0, 1), "Chunk too small for skeleton");
        return;
    }

    const int est_bones = (int)((m_ChunkData.size() - 8) / 64);
    ImGui::Text("Estimated bone count: %d", est_bones);
    ImGui::Spacing();

    if (ImGui::Button("Load Skeleton in Viewport")) {
        if (m_Viewport) {
            std::vector<Bone> bones;
            if (NBN2Parser::Parse(m_ChunkData.data(), m_ChunkData.size(), bones)) {
                m_Viewport->LoadSkeleton(bones);
                std::cout << "[ChunkInspector] Skeleton loaded: "
                    << bones.size() << " bones\n";
            }
            else {
                std::cerr << "[ChunkInspector] NBN2 parse failed\n";
            }
        }
        else {
            std::cerr << "[ChunkInspector] No viewport set\n";
        }
    }

    ImGui::SameLine();
    if (ImGui::Button("Clear Skeleton")) {
        if (m_Viewport) m_Viewport->ClearSkeleton();
    }

    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Text("Bone list (first 30):");

    std::vector<Bone> bones;
    NBN2Parser::Parse(m_ChunkData.data(), m_ChunkData.size(), bones);

    ImGui::BeginChild("BoneList", ImVec2(0, 300), true);
    const int show = (int)std::min(bones.size(), size_t(30));
    for (int i = 0; i < show; i++) {
        const Bone& b = bones[i];
        ImVec4 col = b.IsDynamic() ? ImVec4(0.6f, 0.8f, 1.0f, 1.0f)
            : b.IsEffector() ? ImVec4(0.8f, 1.0f, 0.6f, 1.0f)
            : ImVec4(1.0f, 1.0f, 1.0f, 1.0f);
        ImGui::TextColored(col,
            "[%3d] %-18s  par=%d  rot=(%.0f\xc2\xb0,%.0f\xc2\xb0,%.0f\xc2\xb0)  "
            "pos=(%.3f,%.3f,%.3f)",
            i, b.name.c_str(), (int)b.parent_idx,
            b.rot_x * (180.f / 3.14159f),
            b.rot_y * (180.f / 3.14159f),
            b.rot_z * (180.f / 3.14159f),
            b.local_x, b.local_y, b.local_z);
    }
    if (bones.size() > 30)
        ImGui::TextDisabled("... (%zu more bones)", bones.size() - 30);
    ImGui::EndChild();
}

void ChunkInspector::RenderAnimationInfo() {
    ImGui::Text("Animation Data");
    ImGui::Separator();
    ImGui::TextDisabled("Model Name: %s", m_CurrentChunk.name);
    ImGui::TextDisabled("Contains skeleton bones and keyframe transforms");
    ImGui::TextDisabled("Full parser not yet implemented");
}

void ChunkInspector::RenderTextView() {
    if (m_ChunkData.empty()) {
        ImGui::TextDisabled("No data");
        return;
    }

    static int min_length = 10;
    static bool dialog_only = true;

    ImGui::Text("Extract text strings from chunk data");
    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Min Length", &min_length, 5, 30);
    ImGui::Checkbox("Dialogue Only (filter technical strings)", &dialog_only);
    ImGui::Separator();

    std::vector<std::string> strings = ExtractStrings(m_ChunkData.data(), m_ChunkData.size(), min_length);

    if (strings.empty()) {
        ImGui::TextDisabled("No readable text found");
        return;
    }

    std::vector<std::string> filtered;
    if (dialog_only) {
        for (const auto& str : strings) {
            if (str.find("NSHP") != std::string::npos ||
                str.find("NTX3") != std::string::npos ||
                str.find("NOBJ") != std::string::npos ||
                str.find("poly") != std::string::npos ||
                str.find("Surface") != std::string::npos ||
                str.find("texture_") != std::string::npos ||
                str.find("Sphere") != std::string::npos ||
                str.find("_v") != std::string::npos ||
                str.find("etc") == 0) {
                continue;
            }

            bool has_space = str.find(' ') != std::string::npos;
            bool is_dialog = str.find("<w>") != std::string::npos ||
                str.find("\\n") != std::string::npos;
            bool has_punctuation = str.find('.') != std::string::npos ||
                str.find('?') != std::string::npos ||
                str.find('!') != std::string::npos;

            if (has_space || is_dialog || has_punctuation) {
                filtered.push_back(str);
            }
        }
    }
    else {
        filtered = strings;
    }

    if (filtered.empty()) {
        ImGui::TextDisabled("No dialogue text found (try disabling filter)");
        return;
    }

    ImGui::Text("Found %zu strings:", filtered.size());
    ImGui::Separator();

    ImGui::BeginChild("TextScrollRegion", ImVec2(0, 0), false, ImGuiWindowFlags_HorizontalScrollbar);

    for (size_t i = 0; i < filtered.size(); i++) {
        const std::string& str = filtered[i];

        if (str.find("Polka") != std::string::npos ||
            str.find("Allegretto") != std::string::npos ||
            str.find("Beat") != std::string::npos ||
            str.find("Viola") != std::string::npos ||
            str.find("Frederic") != std::string::npos ||
            str.find("Chopin") != std::string::npos ||
            str.find("Salsa") != std::string::npos ||
            str.find("March") != std::string::npos) {
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "[%zu] %s", i, str.c_str());
        }
        else if (str.find("<w>") != std::string::npos) {
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "[%zu] %s", i, str.c_str());
        }
        else if (str.find("\\n") != std::string::npos) {
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "[%zu] %s", i, str.c_str());
        }
        else if (str.find("?") != std::string::npos || str.find("!") != std::string::npos) {
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "[%zu] %s", i, str.c_str());
        }
        else {
            ImGui::TextColored(ImVec4(0.7f, 0.7f, 0.7f, 1.0f), "[%zu] %s", i, str.c_str());
        }
    }

    ImGui::EndChild();
}

std::vector<std::string> ChunkInspector::ExtractStrings(const uint8_t* data, size_t size, size_t min_length) {
    std::vector<std::string> strings;
    std::string current;

    for (size_t i = 0; i < size; i++) {
        uint8_t byte = data[i];

        if ((byte >= 32 && byte <= 126) || byte == '\n' || byte == '\r' || byte == '\t') {
            current += (char)byte;
        }
        else {
            if (current.length() >= min_length) {
                size_t start = current.find_first_not_of(" \t\n\r");
                size_t end = current.find_last_not_of(" \t\n\r");

                if (start != std::string::npos) {
                    std::string trimmed = current.substr(start, end - start + 1);

                    int alpha_count = 0;
                    for (char c : trimmed) {
                        if (isalpha(c)) alpha_count++;
                    }

                    if (!trimmed.empty() && alpha_count > (int)(trimmed.length() * 0.4)) {
                        strings.push_back(trimmed);
                    }
                }
            }
            current.clear();
        }
    }

    if (current.length() >= min_length) {
        size_t start = current.find_first_not_of(" \t\n\r");
        size_t end = current.find_last_not_of(" \t\n\r");
        if (start != std::string::npos) {
            std::string trimmed = current.substr(start, end - start + 1);

            int alpha_count = 0;
            for (char c : trimmed) {
                if (isalpha(c)) alpha_count++;
            }

            if (!trimmed.empty() && alpha_count > (int)(trimmed.length() * 0.4)) {
                strings.push_back(trimmed);
            }
        }
    }

    return strings;
}