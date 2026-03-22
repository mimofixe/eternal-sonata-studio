#include "Application.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <iostream>
#include <fstream>
#include "EFileParser.h"
#include "FileDialog.h"
#include "FileSystemBrowser.h"
#include "ChunkInspector.h"
#include "Viewport3D.h"
#include "P3TexViewer.h"
#include "EFileTextureViewer.h"
#include "EFileTextExtractor.h"  // NOVO

Application::Application(const AppConfig& config)
    : m_Config(config), m_Running(false) {
}

Application::~Application() {
    Shutdown();
}

void Application::Run() {
    if (!glfwInit()) {
        std::cerr << "Failed to initialize GLFW" << std::endl;
        return;
    }

    glfwWindowHint(GLFW_CONTEXT_VERSION_MAJOR, 4);
    glfwWindowHint(GLFW_CONTEXT_VERSION_MINOR, 5);
    glfwWindowHint(GLFW_OPENGL_PROFILE, GLFW_OPENGL_CORE_PROFILE);

    GLFWwindow* window = glfwCreateWindow(
        m_Config.width, m_Config.height,
        m_Config.title.c_str(), nullptr, nullptr);

    if (!window) {
        std::cerr << "Failed to create window" << std::endl;
        glfwTerminate();
        return;
    }

    glfwMakeContextCurrent(window);
    glfwSwapInterval(m_Config.vsync ? 1 : 0);

    if (!gladLoadGLLoader((GLADloadproc)glfwGetProcAddress)) {
        std::cerr << "Failed to initialize GLAD" << std::endl;
        return;
    }

    std::cout << "OpenGL " << glGetString(GL_VERSION) << std::endl;

    IMGUI_CHECKVERSION();
    ImGui::CreateContext();
    ImGuiIO& io = ImGui::GetIO();
    io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
    io.ConfigFlags |= ImGuiConfigFlags_DockingEnable;

    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 450");

    m_Running = true;

    FileSystemBrowser file_browser;
    ChunkInspector chunk_inspector;
    Viewport3D viewport;
    P3TexViewer tex_viewer;
    EFileTextureViewer efile_tex_viewer;
    EFileTextExtractor text_extractor;  // NOVO

    chunk_inspector.SetViewport(&viewport);
    chunk_inspector.SetP3TexParser(&m_P3TexParser);
    tex_viewer.SetParser(&m_P3TexParser);

    std::vector<Chunk> loaded_chunks;
    std::vector<uint8_t> current_file_data;
    std::string current_file_path;
    int selected_chunk_idx = -1;

    while (!glfwWindowShouldClose(window) && m_Running) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        ImGuiID dockspace_id = ImGui::DockSpaceOverViewport();

        if (ImGui::BeginMainMenuBar()) {
            if (ImGui::BeginMenu("File")) {
                if (ImGui::MenuItem("Open Folder...")) {
                    std::string folder = FileDialog::OpenFolder();
                    if (!folder.empty()) {
                        file_browser.SetRootPath(folder);
                    }
                }
                ImGui::Separator();
                if (ImGui::MenuItem("Exit")) {
                    m_Running = false;
                }
                ImGui::EndMenu();
            }
            ImGui::EndMainMenuBar();
        }

        // Texture Archive (P3TEX)
        ImGui::Begin("Texture Archive (P3TEX)");

        if (ImGui::Button("Load P3TEX...", ImVec2(200, 0))) {
            std::string path = FileDialog::OpenFile("P3TEX Files\0*.p3tex\0All Files\0*.*\0");
            if (!path.empty()) {
                m_P3TexParser.Load(path);
                tex_viewer.SetParser(&m_P3TexParser);
            }
        }

        if (m_P3TexParser.IsLoaded()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "✓ Loaded");

            ImGui::Separator();
            ImGui::Text("File: %s", m_P3TexParser.GetFilename().c_str());
            ImGui::Text("Textures: %zu", m_P3TexParser.GetTextureCount());

            ImGui::Separator();
            ImGui::TextWrapped("View all textures in the 'Texture Viewer' window.");

            if (ImGui::Button("Clear", ImVec2(100, 0))) {
                m_P3TexParser.Clear();
                tex_viewer.SetParser(&m_P3TexParser);
            }
        }
        else {
            ImGui::SameLine();
            ImGui::TextDisabled("Not loaded");

            ImGui::Separator();
            ImGui::TextWrapped("Load a .p3tex texture archive.");
        }

        ImGui::End();

        // P3TEX Texture Viewer
        ImGui::Begin("Texture Viewer (P3TEX)");
        tex_viewer.Render();
        ImGui::End();

        // .E FILE Texture Viewer
        ImGui::Begin("File Textures (.e)");
        efile_tex_viewer.Render();
        ImGui::End();

        // .E FILE Text Viewer (NOVO)
        ImGui::Begin("File Text (.e)");
        text_extractor.Render();
        ImGui::End();

        // File Browser
        ImGui::Begin("File Browser");
        file_browser.Render();

        if (file_browser.HasSelection()) {
            std::string new_file = file_browser.GetSelectedFile();
            if (new_file != current_file_path) {
                current_file_path = new_file;
                loaded_chunks = EFileParser::Parse(current_file_path);

                std::ifstream file(current_file_path, std::ios::binary);
                if (file) {
                    file.seekg(0, std::ios::end);
                    size_t size = file.tellg();
                    file.seekg(0, std::ios::beg);
                    current_file_data.resize(size);
                    file.read((char*)current_file_data.data(), size);
                    file.close();
                }

                selected_chunk_idx = -1;

                // Atualizar visualizadores
                efile_tex_viewer.LoadFromFile(loaded_chunks, current_file_data);
                text_extractor.LoadFromFile(current_file_data);  // NOVO
            }
        }
        ImGui::End();

        // Chunks
        ImGui::Begin("Chunks");
        if (loaded_chunks.empty()) {
            ImGui::TextDisabled("No file loaded");
        }
        else {
            ImGui::Text("File: %s", current_file_path.c_str());
            ImGui::Text("Total Chunks: %d", (int)loaded_chunks.size());
            ImGui::Separator();

            if (ImGui::BeginTable("ChunkList", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 60);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableHeadersRow();

                for (int i = 0; i < (int)loaded_chunks.size(); i++) {
                    const Chunk& chunk = loaded_chunks[i];
                    ImGui::TableNextRow();

                    ImGuiSelectableFlags flags = ImGuiSelectableFlags_SpanAllColumns;
                    ImGui::TableSetColumnIndex(0);

                    if (ImGui::Selectable(("##chunk" + std::to_string(i)).c_str(), selected_chunk_idx == i, flags)) {
                        selected_chunk_idx = i;
                        chunk_inspector.SetChunk(chunk, current_file_data);
                    }

                    ImGui::SameLine();
                    ImVec4 color;
                    switch (chunk.type) {
                    case ChunkType::NSHP:
                        color = ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
                        break;
                    case ChunkType::NOBJ:
                        color = ImVec4(0.4f, 0.4f, 1.0f, 1.0f);
                        break;
                    case ChunkType::NMDL:
                        color = ImVec4(1.0f, 0.8f, 0.4f, 1.0f);
                        break;
                    case ChunkType::NTX3:
                        color = ImVec4(1.0f, 0.4f, 1.0f, 1.0f);
                        break;
                    case ChunkType::NMTN:
                        color = ImVec4(0.4f, 1.0f, 1.0f, 1.0f);
                        break;
                    case ChunkType::NCAM:
                        color = ImVec4(1.0f, 1.0f, 0.4f, 1.0f);
                        break;
                    case ChunkType::NLIT:
                        color = ImVec4(1.0f, 1.0f, 0.8f, 1.0f);
                        break;
                    case ChunkType::NFOG:
                        color = ImVec4(0.7f, 0.7f, 0.9f, 1.0f);
                        break;
                    case ChunkType::NMTR:
                        color = ImVec4(1.0f, 0.6f, 0.2f, 1.0f);
                        break;
                    case ChunkType::SONG:
                        color = ImVec4(0.8f, 0.4f, 0.8f, 1.0f);
                        break;
                    default:
                        color = ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
                        break;
                    }
                    ImGui::TextColored(color, "%s", chunk.GetTypeString().c_str());

                    ImGui::TableSetColumnIndex(1);
                    ImGui::Text("%s", chunk.name);

                    ImGui::TableSetColumnIndex(2);
                    ImGui::Text("0x%zX", chunk.offset);

                    ImGui::TableSetColumnIndex(3);
                    if (chunk.size < 1024) {
                        ImGui::Text("%u B", chunk.size);
                    }
                    else if (chunk.size < 1024 * 1024) {
                        ImGui::Text("%.1f KB", chunk.size / 1024.0f);
                    }
                    else {
                        ImGui::Text("%.2f MB", chunk.size / (1024.0f * 1024.0f));
                    }
                }

                ImGui::EndTable();
            }
        }
        ImGui::End();

        // Inspector
        ImGui::Begin("Inspector");
        chunk_inspector.Render();
        ImGui::End();

        // 3D Viewport
        viewport.Render();

        ImGui::Render();
        int display_w, display_h;
        glfwGetFramebufferSize(window, &display_w, &display_h);
        glViewport(0, 0, display_w, display_h);
        glClearColor(0.1f, 0.1f, 0.1f, 1.0f);
        glClear(GL_COLOR_BUFFER_BIT);
        ImGui_ImplOpenGL3_RenderDrawData(ImGui::GetDrawData());

        glfwSwapBuffers(window);
    }

    ImGui_ImplOpenGL3_Shutdown();
    ImGui_ImplGlfw_Shutdown();
    ImGui::DestroyContext();
    glfwDestroyWindow(window);
    glfwTerminate();
}

void Application::Shutdown() {
    m_Running = false;
}