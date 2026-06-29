#pragma once
#include "Cutscenescriptparser.h"
#include "Cutscenescene.h"
#include <string>
#include <map>

// Stage 0 of the "maestro": an isolated viewport that shows the disassembled cutscene
// script. Does not touch the rest of the app. Shows: overview (with footer relocation
// stats, API usage, hosted events and event->map resolution), scene commands (CALLAPI
// with resolved ids and stack args), actors (slots + derived characters with model
// sources), and the full per-node disassembly.
class CutsceneViewport {
public:
    CutsceneViewport();
    void Load(const std::string& filepath);
    void Clear();
    void Render();
    bool HasScript() const { return m_Script.valid; }

private:
    void RenderOverview();
    void RenderCommands();
    void RenderActors();
    void RenderDisassembly();
    // Narrative slideshow / title-card preview: shows the slide images, the subtitle
    // timeline, the music cue table and a scrubbable playhead. Only meaningful when the
    // event kind is Narrative or TitleCard.
    void RenderSlideshow();

    CutsceneScript m_Script;
    int  m_ActiveTab = 0;
    char m_Filter[64] = {};
    bool m_OnlyCommands = false;

    // Event -> map index, grown automatically as the user opens cfdata map files
    // in the Studio (each field script tests "if (current_event == EV)").
    std::map<uint32_t, std::string> m_EventMapIndex;
    std::string m_IndexStatus;

    // Stage 1: isolated scene renderer (map + actors)
    CutsceneScene m_Scene;
    // Folder with the character assets (pcplk_v1.p3obj, appkeep2.bmd, ...)
    char m_AssetsRoot[260] = {};

    void AssembleScene();

    // Slideshow preview state
    float m_SlideTime = 0.0f;     // current playhead in seconds
    bool  m_SlidePlaying = false; // auto-advance the playhead

    // Lazily-loaded GL textures for the slide images (one per slide_images entry, in
    // order). 0 means not loaded / unsupported. Built on first Slideshow render and
    // freed on Clear/Load.
    std::vector<unsigned int> m_SlideTextures;
    bool m_SlideTexturesBuilt = false;
    void BuildSlideTextures();
    void FreeSlideTextures();
    // Map a playhead time to the index of the full-screen image visible then.
    int  ActiveImageAt(float t) const;
    int  ActiveOverlayAt(float t) const;
};