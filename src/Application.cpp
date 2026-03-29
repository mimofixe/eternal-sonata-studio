#include "Application.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <iostream>
#include <fstream>
#include <set>
#include "EFileParser.h"
#include "BOPParser.h"
#include "ContainerParser.h"
#include "TEXParser.h"
#include "AIScriptParser.h"
#include "AIScriptViewer.h"
#include "TutorialParser.h"
#include "TutorialViewer.h"
#include "FileDialog.h"
#include "FileSystemBrowser.h"
#include "ChunkInspector.h"
#include "Viewport3D.h"
#include "P3TexViewer.h"
#include "EFileTextureViewer.h"
#include "EFileTextExtractor.h" 
#include "CSFViewer.h"

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
    EFileTextExtractor text_extractor;
    CSFViewer csf_viewer;
    AIScriptViewer ai_script_viewer;
    TutorialViewer tutorial_viewer;

    chunk_inspector.SetViewport(&viewport);
    chunk_inspector.SetP3TexParser(&m_P3TexParser);
    tex_viewer.SetParser(&m_P3TexParser);

    std::vector<Chunk> loaded_chunks;
    std::vector<ContainerSection> container_sections;
    std::vector<uint8_t> current_file_data;
    std::string current_file_path;
    int selected_chunk_idx = -1;
    bool is_container_file = false;
    std::set<int> csf_checked;
    bool show_csf_batch_result = false;
    int csf_batch_exported = 0;

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

        ImGui::Begin("Texture Viewer (P3TEX)");
        tex_viewer.Render();
        ImGui::End();

        ImGui::Begin("File Textures (.e / .bop / .bmd / .p3obj)");
        efile_tex_viewer.Render();
        ImGui::End();

        ImGui::Begin("File Text (.e / .bop / .bmd / .p3obj)");
        text_extractor.Render();
        ImGui::End();

        ImGui::Begin("CSF Audio Viewer");
        csf_viewer.Render();
        ImGui::End();

        ImGui::Begin("AI Script Viewer");
        ai_script_viewer.Render();
        ImGui::End();

        ImGui::Begin("Tutorial Viewer");
        tutorial_viewer.Render();
        ImGui::End();

        ImGui::Begin("File Browser");
        file_browser.Render();

        if (file_browser.HasSelection()) {
            std::string new_file = file_browser.GetSelectedFile();
            if (new_file != current_file_path) {
                current_file_path = new_file;

                std::string extension;
                size_t dot_pos = current_file_path.find_last_of('.');
                if (dot_pos != std::string::npos) {
                    extension = current_file_path.substr(dot_pos);
                    for (char& c : extension) {
                        c = std::tolower(c);
                    }
                }

                loaded_chunks.clear();
                container_sections.clear();
                is_container_file = false;
                csf_checked.clear();

                if (extension == ".csf") {
                    csf_viewer.LoadFile(current_file_path);
                }
                else if (extension == ".bmd") {
                    auto container = ContainerParser::Parse(current_file_path);

                    if (container.type != ContainerType::Unknown) {
                        container_sections = container.sections;
                        loaded_chunks = container.all_chunks;
                        is_container_file = true;

                        std::ifstream file(current_file_path, std::ios::binary);
                        if (file) {
                            file.seekg(0, std::ios::end);
                            size_t size = file.tellg();
                            file.seekg(0);
                            current_file_data.resize(size);
                            file.read((char*)current_file_data.data(), size);
                        }

                        selected_chunk_idx = -1;

                        efile_tex_viewer.LoadFromFile(loaded_chunks, current_file_data);
                        text_extractor.LoadFromFile(current_file_data);

                        if (container.has_csf) {
                            csf_viewer.LoadFile(current_file_path);
                        }
                    }
                }
                else if (extension == ".e" || extension == ".bop" || extension == ".p3obj") {
                    // Read enough header bytes to distinguish tutorial vs AI script vs container
                    uint8_t hdr[0x18] = {};
                    {
                        std::ifstream probe(current_file_path, std::ios::binary);
                        if (probe) probe.read(reinterpret_cast<char*>(hdr), sizeof(hdr));
                    }

                    if (TutorialParser::IsTutorialFile(hdr, sizeof(hdr))) {
                        tutorial_viewer.Load(current_file_path);
                        ai_script_viewer.Clear();
                        selected_chunk_idx = -1;
                    }
                    else if (AIScriptParser::IsAIScript(hdr, sizeof(hdr))) {
                        ai_script_viewer.Load(current_file_path);
                        tutorial_viewer.Clear();
                        selected_chunk_idx = -1;
                    }
                    else {
                        ai_script_viewer.Clear();
                        tutorial_viewer.Clear();

                        if (extension == ".bop") {
                            loaded_chunks = BOPParser::Parse(current_file_path);
                        }
                        else {
                            loaded_chunks = EFileParser::Parse(current_file_path);
                        }

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

                        efile_tex_viewer.LoadFromFile(loaded_chunks, current_file_data);
                        text_extractor.LoadFromFile(current_file_data);
                    }
                }
                else if (extension == ".tex") {
                    auto tex = TEXParser::Parse(current_file_path);

                    if (tex.valid) {
                        loaded_chunks = tex.chunks;

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
                        efile_tex_viewer.LoadFromFile(loaded_chunks, current_file_data);
                    }
                }
            }
        }
        ImGui::End();

        ImGui::Begin("Chunks");
        if (loaded_chunks.empty()) {
            ImGui::TextDisabled("No file loaded");
        }
        else {
            ImGui::Text("File: %s", current_file_path.c_str());
            ImGui::Text("Total Chunks: %d", (int)loaded_chunks.size());

            if (is_container_file) {
                ImGui::SameLine();
                ImGui::TextColored(ImVec4(0.4f, 1.0f, 1.0f, 1.0f),
                    "(BMD Container: %d sections)", (int)container_sections.size());
            }

            ImGui::Separator();

            if (ImGui::BeginTable("ChunkList", 4, ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
                ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
                ImGui::TableSetupColumn("Offset", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableSetupColumn("Size", ImGuiTableColumnFlags_WidthFixed, 80);
                ImGui::TableHeadersRow();

                if (is_container_file && !container_sections.empty()) {
                    int chunk_global_idx = 0;

                    for (size_t sec_idx = 0; sec_idx < container_sections.size(); sec_idx++) {
                        const auto& section = container_sections[sec_idx];

                        if (section.type == "mefc") {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);

                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 1.0f, 1.0f, 1.0f));
                            bool node_open = ImGui::TreeNodeEx(
                                (void*)(intptr_t)sec_idx,
                                ImGuiTreeNodeFlags_SpanFullWidth,
                                "Mefc"
                            );
                            ImGui::PopStyleColor();

                            ImGui::TableSetColumnIndex(1);
                            ImGui::Text("%s (%d chunks)", section.mefc_name.c_str(), (int)section.chunks.size());

                            ImGui::TableSetColumnIndex(2);
                            ImGui::Text("0x%zX", section.offset);

                            ImGui::TableSetColumnIndex(3);
                            FormatSize(section.size);

                            if (node_open) {
                                for (const auto& chunk : section.chunks) {
                                    ImGui::TableNextRow();

                                    ImGuiSelectableFlags flags = ImGuiSelectableFlags_SpanAllColumns;
                                    ImGui::TableSetColumnIndex(0);

                                    if (ImGui::Selectable(("##chunk" + std::to_string(chunk_global_idx)).c_str(),
                                        selected_chunk_idx == chunk_global_idx, flags)) {
                                        selected_chunk_idx = chunk_global_idx;
                                        chunk_inspector.SetChunk(chunk, current_file_data);
                                        if (chunk.type == ChunkType::CSF && !current_file_data.empty()) {
                                            csf_viewer.LoadFromMemory(
                                                current_file_data.data() + chunk.offset,
                                                chunk.size, chunk.name);
                                        }
                                    }

                                    ImGui::SameLine();
                                    ImGui::Indent();

                                    ImVec4 color = GetChunkColor(chunk.type);
                                    ImGui::TextColored(color, "%s", chunk.GetTypeString().c_str());
                                    ImGui::Unindent();

                                    ImGui::TableSetColumnIndex(1);
                                    ImGui::Text("%s", chunk.name);

                                    ImGui::TableSetColumnIndex(2);
                                    ImGui::Text("0x%zX", chunk.offset);

                                    ImGui::TableSetColumnIndex(3);
                                    FormatSize(chunk.size);

                                    chunk_global_idx++;
                                }
                                ImGui::TreePop();
                            }
                            else {
                                chunk_global_idx += section.chunks.size();
                            }
                        }
                        else if (section.type == "nobj") {
                            ImGui::TableNextRow();
                            ImGui::TableSetColumnIndex(0);

                            ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.4f, 0.4f, 1.0f, 1.0f));
                            bool node_open = ImGui::TreeNodeEx(
                                (void*)(intptr_t)sec_idx,
                                ImGuiTreeNodeFlags_SpanFullWidth,
                                "NOBJ"
                            );
                            ImGui::PopStyleColor();

                            ImGui::TableSetColumnIndex(1);
                            ImGui::Text("Object Container (%d chunks)", (int)section.chunks.size());

                            ImGui::TableSetColumnIndex(2);
                            ImGui::Text("0x%zX", section.offset);

                            ImGui::TableSetColumnIndex(3);
                            FormatSize(section.size);

                            if (node_open) {
                                for (const auto& chunk : section.chunks) {
                                    ImGui::TableNextRow();

                                    ImGuiSelectableFlags flags = ImGuiSelectableFlags_SpanAllColumns;
                                    ImGui::TableSetColumnIndex(0);

                                    if (ImGui::Selectable(("##chunk" + std::to_string(chunk_global_idx)).c_str(),
                                        selected_chunk_idx == chunk_global_idx, flags)) {
                                        selected_chunk_idx = chunk_global_idx;
                                        chunk_inspector.SetChunk(chunk, current_file_data);
                                        if (chunk.type == ChunkType::CSF && !current_file_data.empty()) {
                                            csf_viewer.LoadFromMemory(
                                                current_file_data.data() + chunk.offset,
                                                chunk.size, chunk.name);
                                        }
                                    }

                                    ImGui::SameLine();
                                    ImGui::Indent();

                                    ImVec4 color = GetChunkColor(chunk.type);
                                    ImGui::TextColored(color, "%s", chunk.GetTypeString().c_str());
                                    ImGui::Unindent();

                                    ImGui::TableSetColumnIndex(1);
                                    ImGui::Text("%s", chunk.name);

                                    ImGui::TableSetColumnIndex(2);
                                    ImGui::Text("0x%zX", chunk.offset);

                                    ImGui::TableSetColumnIndex(3);
                                    FormatSize(chunk.size);

                                    chunk_global_idx++;
                                }
                                ImGui::TreePop();
                            }
                            else {
                                chunk_global_idx += section.chunks.size();
                            }
                        }
                        else {
                            const auto& chunk = section.chunk;

                            ImGui::TableNextRow();

                            ImGuiSelectableFlags flags = ImGuiSelectableFlags_SpanAllColumns;
                            ImGui::TableSetColumnIndex(0);

                            if (ImGui::Selectable(("##chunk" + std::to_string(chunk_global_idx)).c_str(),
                                selected_chunk_idx == chunk_global_idx, flags)) {
                                selected_chunk_idx = chunk_global_idx;
                                chunk_inspector.SetChunk(chunk, current_file_data);
                                if (chunk.type == ChunkType::CSF && !current_file_data.empty()) {
                                    csf_viewer.LoadFromMemory(
                                        current_file_data.data() + chunk.offset,
                                        chunk.size, chunk.name);
                                }
                            }

                            ImGui::SameLine();
                            ImVec4 color = GetChunkColor(chunk.type);
                            ImGui::TextColored(color, "%s", chunk.GetTypeString().c_str());

                            ImGui::TableSetColumnIndex(1);
                            ImGui::Text("%s", chunk.name);

                            ImGui::TableSetColumnIndex(2);
                            ImGui::Text("0x%zX", chunk.offset);

                            ImGui::TableSetColumnIndex(3);
                            FormatSize(chunk.size);

                            chunk_global_idx++;
                        }
                    }
                }
                else {
                    for (int i = 0; i < (int)loaded_chunks.size(); i++) {
                        const Chunk& chunk = loaded_chunks[i];
                        ImGui::TableNextRow();

                        ImGuiSelectableFlags flags = ImGuiSelectableFlags_SpanAllColumns;
                        ImGui::TableSetColumnIndex(0);

                        if (ImGui::Selectable(("##chunk" + std::to_string(i)).c_str(), selected_chunk_idx == i, flags)) {
                            selected_chunk_idx = i;
                            chunk_inspector.SetChunk(chunk, current_file_data);
                            if (chunk.type == ChunkType::CSF && !current_file_data.empty()) {
                                csf_viewer.LoadFromMemory(
                                    current_file_data.data() + chunk.offset,
                                    chunk.size, chunk.name);
                            }
                        }

                        ImGui::SameLine();
                        ImVec4 color = GetChunkColor(chunk.type);
                        ImGui::TextColored(color, "%s", chunk.GetTypeString().c_str());

                        ImGui::TableSetColumnIndex(1);
                        ImGui::Text("%s", chunk.name);

                        ImGui::TableSetColumnIndex(2);
                        ImGui::Text("0x%zX", chunk.offset);

                        ImGui::TableSetColumnIndex(3);
                        FormatSize(chunk.size);
                    }
                }

                ImGui::EndTable();

                // CSF batch export panel — shown whenever the loaded file has CSF chunks
                std::vector<int> csf_indices;
                for (int i = 0; i < (int)loaded_chunks.size(); i++) {
                    if (loaded_chunks[i].type == ChunkType::CSF) csf_indices.push_back(i);
                }

                if (!csf_indices.empty()) {
                    ImGui::Separator();
                    ImGui::Text("CSF Audio Export (%d tracks)", (int)csf_indices.size());
                    ImGui::Separator();

                    // Scrollable checkbox list
                    ImGui::BeginChild("CSFCheckList", ImVec2(0, 120), true);
                    for (int j = 0; j < (int)csf_indices.size(); j++) {
                        int chunk_idx = csf_indices[j];
                        const Chunk& c = loaded_chunks[chunk_idx];
                        bool checked = csf_checked.count(chunk_idx) > 0;
                        if (ImGui::Checkbox(("##csf" + std::to_string(j)).c_str(), &checked)) {
                            if (checked) csf_checked.insert(chunk_idx);
                            else csf_checked.erase(chunk_idx);
                        }
                        ImGui::SameLine();
                        ImGui::TextDisabled("%s  (%.1f KB)", c.name, c.size / 1024.0f);
                    }
                    ImGui::EndChild();

                    if (ImGui::SmallButton("Select All")) {
                        for (int idx : csf_indices) csf_checked.insert(idx);
                    }
                    ImGui::SameLine();
                    if (ImGui::SmallButton("Clear")) {
                        csf_checked.clear();
                    }

                    ImGui::Spacing();

                    // Export selected
                    ImGui::BeginDisabled(csf_checked.empty());
                    char sel_label[48];
                    snprintf(sel_label, sizeof(sel_label), "Export Selected (%d)", (int)csf_checked.size());
                    if (ImGui::Button(sel_label, ImVec2(-1, 0))) {
                        std::string base = FileDialog::SaveFile("ATRAC3 Audio\0*.at3\0All Files\0*.*\0");
                        if (!base.empty()) {
                            size_t ext = base.find_last_of('.');
                            if (ext != std::string::npos) base = base.substr(0, ext);
                            int n = 0, ci_n = 0;
                            for (int idx : csf_checked) {
                                const Chunk& c = loaded_chunks[idx];
                                if (c.offset + c.size > current_file_data.size()) { ci_n++; continue; }
                                CSFFile csf;
                                if (CSFParser::LoadFromMemory(current_file_data.data() + c.offset, c.size, csf)) {
                                    for (size_t ci = 0; ci < csf.clips.size(); ci++) {
                                        char suf[32];
                                        snprintf(suf, sizeof(suf), "_%03d_%03d.at3", ci_n, (int)ci);
                                        if (CSFParser::ExportClipAT3(csf, (uint32_t)ci, base + suf)) n++;
                                    }
                                }
                                ci_n++;
                            }
                            csf_batch_exported = n;
                            show_csf_batch_result = true;
                        }
                    }
                    ImGui::EndDisabled();

                    // Export all
                    if (ImGui::Button("Export All CSF", ImVec2(-1, 0))) {
                        std::string base = FileDialog::SaveFile("ATRAC3 Audio\0*.at3\0All Files\0*.*\0");
                        if (!base.empty()) {
                            size_t ext = base.find_last_of('.');
                            if (ext != std::string::npos) base = base.substr(0, ext);
                            int n = 0;
                            for (int j = 0; j < (int)csf_indices.size(); j++) {
                                const Chunk& c = loaded_chunks[csf_indices[j]];
                                if (c.offset + c.size > current_file_data.size()) continue;
                                CSFFile csf;
                                if (CSFParser::LoadFromMemory(current_file_data.data() + c.offset, c.size, csf)) {
                                    for (size_t ci = 0; ci < csf.clips.size(); ci++) {
                                        char suf[32];
                                        snprintf(suf, sizeof(suf), "_%03d_%03d.at3", j, (int)ci);
                                        if (CSFParser::ExportClipAT3(csf, (uint32_t)ci, base + suf)) n++;
                                    }
                                }
                            }
                            csf_batch_exported = n;
                            show_csf_batch_result = true;
                        }
                    }

                    // Result popup
                    if (show_csf_batch_result) {
                        ImGui::OpenPopup("CSF Export Result");
                        show_csf_batch_result = false;
                    }
                    if (ImGui::BeginPopupModal("CSF Export Result", NULL, ImGuiWindowFlags_AlwaysAutoResize)) {
                        if (csf_batch_exported > 0) {
                            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "Export successful");
                            ImGui::Text("Exported %d .at3 file(s).", csf_batch_exported);
                        }
                        else {
                            ImGui::TextColored(ImVec4(1.0f, 0.4f, 0.4f, 1.0f), "Export failed");
                        }
                        ImGui::Separator();
                        if (ImGui::Button("OK", ImVec2(120, 0))) ImGui::CloseCurrentPopup();
                        ImGui::EndPopup();
                    }
                }

            }
        }
        ImGui::End();

        ImGui::Begin("Inspector");
        chunk_inspector.Render();
        ImGui::End();

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

ImVec4 Application::GetChunkColor(ChunkType type) {
    switch (type) {
    case ChunkType::NSHP:
        return ImVec4(0.4f, 1.0f, 0.4f, 1.0f);
    case ChunkType::NOBJ:
        return ImVec4(0.4f, 0.4f, 1.0f, 1.0f);
    case ChunkType::NMDL:
        return ImVec4(1.0f, 0.8f, 0.4f, 1.0f);
    case ChunkType::NTX3:
        return ImVec4(1.0f, 0.4f, 1.0f, 1.0f);
    case ChunkType::NMTN:
        return ImVec4(0.4f, 1.0f, 1.0f, 1.0f);
    case ChunkType::NMTB:
        return ImVec4(0.2f, 0.8f, 0.8f, 1.0f);
    case ChunkType::NBN2:
        return ImVec4(1.0f, 0.7f, 0.2f, 1.0f);
    case ChunkType::NDYN:
        return ImVec4(0.3f, 1.0f, 0.6f, 1.0f);
    case ChunkType::NLC2:
        return ImVec4(0.8f, 0.5f, 1.0f, 1.0f);
    case ChunkType::NCLS:
        return ImVec4(0.6f, 0.8f, 0.4f, 1.0f);
    case ChunkType::NCAM:
        return ImVec4(1.0f, 1.0f, 0.4f, 1.0f);
    case ChunkType::NLIT:
        return ImVec4(1.0f, 1.0f, 0.8f, 1.0f);
    case ChunkType::NFOG:
        return ImVec4(0.7f, 0.7f, 0.9f, 1.0f);
    case ChunkType::NMTR:
        return ImVec4(1.0f, 0.6f, 0.2f, 1.0f);
    case ChunkType::SONG:
        return ImVec4(0.8f, 0.4f, 0.8f, 1.0f);
    case ChunkType::BOOK:
        return ImVec4(0.6f, 0.8f, 0.6f, 1.0f);
    case ChunkType::PGHD:
        return ImVec4(0.8f, 0.6f, 0.4f, 1.0f);
    case ChunkType::TIM:
        return ImVec4(0.9f, 0.5f, 0.9f, 1.0f);
    case ChunkType::PROG:
        return ImVec4(0.5f, 0.9f, 0.9f, 1.0f);
    case ChunkType::CSF:
        return ImVec4(1.0f, 0.7f, 0.3f, 1.0f);
    case ChunkType::FONT:
        return ImVec4(0.9f, 0.9f, 0.5f, 1.0f);
    default:
        return ImVec4(0.5f, 0.5f, 0.5f, 1.0f);
    }
}

void Application::FormatSize(uint32_t size) {
    if (size < 1024) {
        ImGui::Text("%u B", size);
    }
    else if (size < 1024 * 1024) {
        ImGui::Text("%.1f KB", size / 1024.0f);
    }
    else {
        ImGui::Text("%.2f MB", size / (1024.0f * 1024.0f));
    }
}