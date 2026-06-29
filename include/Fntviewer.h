#pragma once
#include "Fntparser.h"
#include <glad/glad.h>
#include <string>
#include <vector>
#include <unordered_map>

// FntViewer
// Loads a .fnt font (or finds a FONT block embedded in any other file) and lets
// the user type text with it. Text can be entered in an input box or with an
// on-screen keyboard, and is rendered live using the font's own bitmap glyphs.
// Only the BC sub-font (Latin letters, digits, symbols) is used.
//
// Follows the same pattern as the other Studio viewers: the Application owns the
// instance and calls Render() once per frame inside its own ImGui::Begin/End.
class FntViewer {
public:
    FntViewer();
    ~FntViewer();

    // Load a .fnt, or any file that contains a FONT block (e.g. a BMD).
    bool LoadFile(const std::string& filepath);

    // Drop the current font and free all GPU textures.
    void Clear();

    bool IsLoaded() const { return m_Loaded; }
    const std::string& GetFilename() const { return m_Filename; }

    // Draw the panel. Call between ImGui::Begin(...) / ImGui::End().
    void Render();

private:
    void RenderInfo();
    void RenderKeyboard();
    void RenderTextPreview();
    void RenderExportPopup();

    // Append a character / handle a key from the on-screen keyboard.
    void TypeChar(char c);
    void Backspace();

    // Look up the glyph for an ASCII character (slot = code - 1 in this font).
    // Returns nullptr for space / characters with no glyph.
    const FntGlyph* GlyphForChar(unsigned char ch) const;

    // Compose the current text into a flat RGBA8 buffer (black text on a
    // transparent or white background). Returns false if there is nothing to
    // draw. out_w / out_h receive the image size in pixels.
    bool ComposeTextRGBA(std::vector<uint8_t>& out_rgba, int& out_w, int& out_h,
        bool white_background) const;

    // Export the composed text as a PNG via a save dialog.
    void ExportTextPNG();

    // Lazily upload one glyph to a GL texture (white RGBA + per-pixel alpha),
    // cached by character code. Returns 0 if the glyph is empty/missing.
    GLuint GetGlyphTexture(const FntGlyph& glyph);
    void FreeTextures();

    FntFont     m_Font;
    bool        m_Loaded = false;
    std::string m_Filename;

    char        m_Text[256] = { 0 };  // the line the user is composing
    float       m_Scale = 1.0f;       // preview magnification
    bool        m_ShowBaseline = false;
    bool        m_Uppercase = false;  // on-screen keyboard letter case
    bool        m_ExportWhiteBg = false;  // PNG background: transparent or white

    // export feedback popup
    bool        m_ShowExportPopup = false;
    bool        m_ExportSuccess = false;
    std::string m_LastExportPath;

    std::unordered_map<uint16_t, GLuint> m_Textures;  // code -> GL texture
};