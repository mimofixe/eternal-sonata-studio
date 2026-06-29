#include "Fntviewer.h"
#include <imgui.h>
#include "FileDialog.h"
#include "../libs/stb/stb_image_write.h"
#include <cstring>
#include <cstdio>
#include <vector>

FntViewer::FntViewer() {}

FntViewer::~FntViewer() {
    FreeTextures();
}

void FntViewer::Clear() {
    FreeTextures();
    m_Font = FntFont();
    m_Loaded = false;
    m_Filename.clear();
    m_Text[0] = 0;
}

bool FntViewer::LoadFile(const std::string& filepath) {
    Clear();
    if (!FntParser::ParseFile(filepath, m_Font)) return false;
    m_Loaded = true;
    size_t slash = filepath.find_last_of("/\\");
    m_Filename = (slash == std::string::npos) ? filepath : filepath.substr(slash + 1);
    // start with a friendly sample so the preview isn't empty
    std::snprintf(m_Text, sizeof(m_Text), "Hello World 123");
    return true;
}

void FntViewer::FreeTextures() {
    for (auto& kv : m_Textures)
        if (kv.second) glDeleteTextures(1, &kv.second);
    m_Textures.clear();
}

GLuint FntViewer::GetGlyphTexture(const FntGlyph& glyph) {
    auto it = m_Textures.find(glyph.code);
    if (it != m_Textures.end()) return it->second;

    if (glyph.width == 0 || glyph.height == 0) {
        m_Textures[glyph.code] = 0;
        return 0;
    }

    std::vector<uint8_t> alpha;
    FntParser::GlyphToAlpha(glyph, alpha);
    std::vector<uint8_t> rgba((size_t)glyph.width * glyph.height * 4);
    for (size_t i = 0; i < alpha.size(); i++) {
        rgba[i * 4 + 0] = 255;
        rgba[i * 4 + 1] = 255;
        rgba[i * 4 + 2] = 255;
        rgba[i * 4 + 3] = alpha[i];
    }

    GLint prevAlign = 4;
    glGetIntegerv(GL_UNPACK_ALIGNMENT, &prevAlign);
    glPixelStorei(GL_UNPACK_ALIGNMENT, 1);

    GLuint tex = 0;
    glGenTextures(1, &tex);
    glBindTexture(GL_TEXTURE_2D, tex);
    glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA, glyph.width, glyph.height, 0,
        GL_RGBA, GL_UNSIGNED_BYTE, rgba.data());
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_LINEAR);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_CLAMP_TO_EDGE);
    glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_CLAMP_TO_EDGE);
    glBindTexture(GL_TEXTURE_2D, 0);

    glPixelStorei(GL_UNPACK_ALIGNMENT, prevAlign);

    m_Textures[glyph.code] = tex;
    return tex;
}

void FntViewer::TypeChar(char c) {
    size_t len = std::strlen(m_Text);
    if (len + 1 < sizeof(m_Text)) {
        m_Text[len] = c;
        m_Text[len + 1] = 0;
    }
}

void FntViewer::Backspace() {
    size_t len = std::strlen(m_Text);
    if (len > 0) m_Text[len - 1] = 0;
}

void FntViewer::Render() {
    if (!m_Loaded) {
        ImGui::TextDisabled("No font loaded.");
        ImGui::TextWrapped("Load a .fnt file, or any file that contains a FONT "
            "block (the loader scans for it), then type with the on-screen "
            "keyboard or the text box.");
        return;
    }

    RenderInfo();
    ImGui::Separator();
    RenderTextPreview();
    ImGui::Separator();
    RenderKeyboard();

    RenderExportPopup();
}

void FntViewer::RenderInfo() {
    ImGui::Text("File: %s", m_Filename.c_str());
    ImGui::SameLine();
    ImGui::TextDisabled("| %s | cell %dx%d | %zu glyphs",
        m_Font.name.c_str(), m_Font.cell_w, m_Font.cell_h,
        m_Font.glyphs.size());

    ImGui::SetNextItemWidth(-1.0f);
    ImGui::InputText("##fnttext", m_Text, sizeof(m_Text));

    ImGui::SetNextItemWidth(160.0f);
    ImGui::SliderFloat("scale", &m_Scale, 0.5f, 4.0f, "%.1fx");
    ImGui::SameLine();
    ImGui::Checkbox("baseline", &m_ShowBaseline);
    ImGui::SameLine();
    if (ImGui::Button("Clear text")) m_Text[0] = 0;

    // export row: PNG of the composed text (at native glyph resolution)
    if (ImGui::Button("Export PNG...")) ExportTextPNG();
    ImGui::SameLine();
    ImGui::Checkbox("white background", &m_ExportWhiteBg);
    ImGui::SameLine();
    ImGui::TextDisabled("(exported at 1x, black text)");
}

void FntViewer::RenderTextPreview() {
    ImGui::TextDisabled("Preview");

    ImGui::BeginChild("FntPreview", ImVec2(0, m_Font.cell_h * m_Scale + 24.0f),
        true, ImGuiWindowFlags_HorizontalScrollbar);

    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImVec2 origin = ImGui::GetCursorScreenPos();
    float baseline = m_Font.cell_h * m_Scale;  // place glyphs on a common baseline
    float pen_x = origin.x + 4.0f;
    float top = origin.y + 4.0f;

    if (m_ShowBaseline) {
        float by = top + baseline;
        draw->AddLine(ImVec2(origin.x, by),
            ImVec2(origin.x + ImGui::GetContentRegionAvail().x, by),
            IM_COL32(80, 80, 80, 255));
    }

    for (const char* p = m_Text; *p; p++) {
        unsigned char ch = (unsigned char)*p;
        const FntGlyph* g = GlyphForChar(ch);
        if (!g) {
            // space or unknown char: advance by a fixed fraction of the cell
            pen_x += (m_Font.cell_w * 0.25f) * m_Scale;
            continue;
        }

        GLuint tex = GetGlyphTexture(*g);
        float w = g->width * m_Scale;
        float h = g->height * m_Scale;
        // align glyph bottoms to the baseline (these bitmaps are top-aligned per
        // record, so anchor by height for a stable text line)
        float gy = top + baseline - h;
        if (tex) {
            draw->AddImage((ImTextureID)(intptr_t)tex,
                ImVec2(pen_x, gy),
                ImVec2(pen_x + w, gy + h),
                ImVec2(0, 0), ImVec2(1, 1),
                IM_COL32_WHITE);
        }
        // advance by the visible width plus a small gap. The record's metric[0]
        // is a bearing, not a tracking width, so it isn't used for spacing.
        pen_x += (g->width + 2) * m_Scale;
    }

    // reserve the area we drew into so the child scrolls correctly
    float used_w = (pen_x - origin.x) + 8.0f;
    ImGui::Dummy(ImVec2(used_w, baseline + 8.0f));
    ImGui::EndChild();
}

const FntGlyph* FntViewer::GlyphForChar(unsigned char ch) const {
    if (ch == ' ') return nullptr;  // space has no glyph
    // In this font the glyph slot is the character code minus one
    // (slot 0x2F = '0', 0x40 = 'A', 0x60 = 'a', ...).
    return m_Font.FindGlyph((uint16_t)ch - 1);
}

bool FntViewer::ComposeTextRGBA(std::vector<uint8_t>& out_rgba,
    int& out_w, int& out_h, bool white_background) const {
    if (!m_Loaded || m_Text[0] == 0) return false;

    int space_adv = (int)(m_Font.cell_w * 0.25f);
    int pad = 4;

    // First pass: measure total width and the tallest glyph.
    int total_w = pad * 2;
    int max_h = m_Font.cell_h;  // baseline box height
    for (const char* p = m_Text; *p; p++) {
        const FntGlyph* g = GlyphForChar((unsigned char)*p);
        if (!g) { total_w += space_adv; continue; }
        total_w += g->width + 2;
        if (g->height > max_h) max_h = g->height;
    }
    if (total_w <= pad * 2) return false;

    out_w = total_w;
    out_h = max_h + pad * 2;

    // Background: opaque white or fully transparent.
    out_rgba.assign((size_t)out_w * out_h * 4, 0);
    if (white_background) {
        for (size_t i = 0; i < out_rgba.size(); i += 4) {
            out_rgba[i + 0] = 255;
            out_rgba[i + 1] = 255;
            out_rgba[i + 2] = 255;
            out_rgba[i + 3] = 255;
        }
    }

    // Second pass: blit each glyph in black, bottom-aligned to a common baseline.
    int baseline = out_h - pad;  // glyph bottoms sit here
    int pen_x = pad;
    std::vector<uint8_t> alpha;
    for (const char* p = m_Text; *p; p++) {
        const FntGlyph* g = GlyphForChar((unsigned char)*p);
        if (!g) { pen_x += space_adv; continue; }

        FntParser::GlyphToAlpha(*g, alpha);
        int gy = baseline - g->height;  // top of this glyph
        for (int row = 0; row < g->height; row++) {
            int dy = gy + row;
            if (dy < 0 || dy >= out_h) continue;
            for (int col = 0; col < g->width; col++) {
                int dx = pen_x + col;
                if (dx < 0 || dx >= out_w) continue;
                uint8_t a = alpha[(size_t)row * g->width + col];
                if (a == 0) continue;
                size_t di = ((size_t)dy * out_w + dx) * 4;
                // black text; alpha-over the background
                if (white_background) {
                    // composite black over white: result = bg * (1 - a)
                    uint8_t inv = (uint8_t)(255 - a);
                    out_rgba[di + 0] = inv;
                    out_rgba[di + 1] = inv;
                    out_rgba[di + 2] = inv;
                    out_rgba[di + 3] = 255;
                }
                else {
                    out_rgba[di + 0] = 0;
                    out_rgba[di + 1] = 0;
                    out_rgba[di + 2] = 0;
                    out_rgba[di + 3] = a;
                }
            }
        }
        pen_x += g->width + 2;
    }
    return true;
}

void FntViewer::ExportTextPNG() {
    std::vector<uint8_t> rgba;
    int w = 0, h = 0;
    if (!ComposeTextRGBA(rgba, w, h, m_ExportWhiteBg)) {
        m_ExportSuccess = false;
        m_ShowExportPopup = true;
        return;
    }

    std::string savePath = FileDialog::SaveFile("PNG Image\0*.png\0All Files\0*.*\0");
    if (savePath.empty()) {
        m_ExportSuccess = false;
        m_ShowExportPopup = true;
        return;
    }
    if (savePath.find(".png") == std::string::npos)
        savePath += ".png";

    int result = stbi_write_png(savePath.c_str(), w, h, 4, rgba.data(), w * 4);
    m_ExportSuccess = (result != 0);
    if (m_ExportSuccess) m_LastExportPath = savePath;
    m_ShowExportPopup = true;
}

void FntViewer::RenderExportPopup() {
    if (m_ShowExportPopup) {
        ImGui::OpenPopup("Font Export Result");
        m_ShowExportPopup = false;
    }
    if (ImGui::BeginPopupModal("Font Export Result", NULL,
        ImGuiWindowFlags_AlwaysAutoResize)) {
        if (m_ExportSuccess) {
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Export Successful!");
            ImGui::Separator();
            ImGui::TextWrapped("Saved to:");
            ImGui::TextWrapped("%s", m_LastExportPath.c_str());
        }
        else {
            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f),
                "Export Cancelled or Failed");
            ImGui::Separator();
            ImGui::TextWrapped("Nothing was saved (empty text or no file chosen).");
        }
        ImGui::Separator();
        if (ImGui::Button("OK", ImVec2(120, 0)))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

void FntViewer::RenderKeyboard() {
    ImGui::TextDisabled("On-screen keyboard");

    // letter case toggle
    ImGui::Checkbox("UPPERCASE", &m_Uppercase);
    ImGui::SameLine();
    ImGui::TextDisabled("(click keys to type)");

    // QWERTY-style rows; digits on top. Buttons type into m_Text.
    const char* row_num = "1234567890";
    const char* row1 = m_Uppercase ? "QWERTYUIOP" : "qwertyuiop";
    const char* row2 = m_Uppercase ? "ASDFGHJKL" : "asdfghjkl";
    const char* row3 = m_Uppercase ? "ZXCVBNM" : "zxcvbnm";

    const ImVec2 key(34, 34);

    auto draw_row = [&](const char* row, float indent) {
        if (indent > 0.0f) ImGui::Dummy(ImVec2(indent, 0)), ImGui::SameLine();
        for (const char* p = row; *p; p++) {
            char label[2] = { *p, 0 };
            ImGui::PushID((int)(intptr_t)p);
            if (ImGui::Button(label, key)) TypeChar(*p);
            ImGui::PopID();
            if (*(p + 1)) ImGui::SameLine();
        }
        };

    draw_row(row_num, 0.0f);
    draw_row(row1, 0.0f);
    draw_row(row2, key.x * 0.5f);
    draw_row(row3, key.x * 1.0f);

    // bottom row: space, common punctuation, backspace
    if (ImGui::Button("Space", ImVec2(key.x * 4, key.y))) TypeChar(' ');
    ImGui::SameLine();
    const char* punct = ".,!?:;'\"-()";
    for (const char* p = punct; *p; p++) {
        char label[2] = { *p, 0 };
        ImGui::PushID(1000 + (int)(*p));
        if (ImGui::Button(label, key)) TypeChar(*p);
        ImGui::PopID();
        ImGui::SameLine();
    }
    if (ImGui::Button("Backspace", ImVec2(key.x * 3, key.y))) Backspace();
}