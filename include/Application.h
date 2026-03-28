#pragma once
#include <string>
#include <imgui.h>
#include "P3TexParser.h"
#include "EFileParser.h"

struct AppConfig {
    int width = 1920;
    int height = 1080;
    std::string title = "Eternal Sonata Studio";
    bool vsync = true;
};

class Application {
public:
    Application(const AppConfig& config);
    ~Application();

    void Run();
    void Shutdown();

private:
    AppConfig m_Config;
    bool m_Running;
    P3TexParser m_P3TexParser;

    ImVec4 GetChunkColor(ChunkType type);
    void FormatSize(uint32_t size);
};