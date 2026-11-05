#include "Application.h"
#include <glad/glad.h>
#include <GLFW/glfw3.h>
#include <imgui.h>
#include <imgui_impl_glfw.h>
#include <imgui_impl_opengl3.h>
#include <iostream>
#include <fstream>
#include <set>
#include <filesystem>
#include "EFileParser.h"
#include "BOPParser.h"
#include "ContainerParser.h"
#include "TEXParser.h"
#include "AIScriptParser.h"
#include "AIScriptViewer.h"
#include "Cutsceneviewport.h"
#include "TutorialParser.h"
#include "TutorialViewer.h"
#include "FileDialog.h"
#include "FileSystemBrowser.h"
#include "ChunkInspector.h"
#include "Viewport3D.h"
#include "P3TexViewer.h"
#include "EFileTextureViewer.h"
#include "Ep3Parser.h"
#include "EFileTextExtractor.h" 
#include "CSFViewer.h"
#include "AnimViewport.h"
#include "TcxDecompressor.h"
#include "Fntviewer.h"

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

    // The default atlas is ASCII-only, so Japanese text (the credits BMDs, and
    // the JPN blocks in .e files) would render as blank boxes. Pull in a system
    // CJK face if one is present; the default font stays as the fallback.
    {
        static const ImWchar* jp_ranges = io.Fonts->GetGlyphRangesJapanese();
        const char* candidates[] = {
            "C:\\Windows\\Fonts\\meiryo.ttc",
            "C:\\Windows\\Fonts\\msgothic.ttc",
            "C:\\Windows\\Fonts\\YuGothM.ttc",
        };
        for (const char* path : candidates) {
            if (std::filesystem::exists(path) &&
                io.Fonts->AddFontFromFileTTF(path, 16.0f, nullptr, jp_ranges)) {
                break;
            }
        }
    }

    ImGui::StyleColorsDark();
    ImGui_ImplGlfw_InitForOpenGL(window, true);
    ImGui_ImplOpenGL3_Init("#version 450");

    m_Running = true;

    FileSystemBrowser file_browser;
    // argv[1], already validated as an existing directory by main().
    if (!m_Config.start_folder.empty())
        file_browser.SetRootPath(m_Config.start_folder);
    ChunkInspector chunk_inspector;
    Viewport3D viewport;
    P3TexViewer tex_viewer;
    EFileTextureViewer efile_tex_viewer;
    EFileTextExtractor text_extractor;
    CSFViewer csf_viewer;
    AIScriptViewer ai_script_viewer;
    TutorialViewer tutorial_viewer;
    CutsceneViewport cutscene_viewport;
    AnimViewport   anim_viewport;
    FntViewer      fnt_viewer;

    chunk_inspector.SetViewport(&viewport);
    chunk_inspector.SetP3TexParser(&m_P3TexParser);
    tex_viewer.SetParser(&m_P3TexParser);

    std::vector<Chunk> loaded_chunks;
    std::vector<ContainerSection> container_sections;
    std::vector<uint8_t> current_file_data;
    std::string current_file_path;
    int selected_chunk_idx = -1;
    bool is_container_file = false;
    bool was_compressed = false;
    std::set<int> csf_checked;
    bool show_csf_batch_result = false;
    int csf_batch_exported = 0;

    // If `path` is a Tcx-compressed file, decompress it to a stable temp file
    // and return the temp path. Otherwise returns `path` unchanged.
    auto resolveDecompressedPath = [](const std::string& path, bool* out_was_compressed) -> std::string {
        if (out_was_compressed) *out_was_compressed = false;

        // Decide from the header alone. Without this, every click paid a full
        // read of the file just to discover it wasn't compressed - which on a
        // 250 MB WAV froze the window for seconds before anything happened.
        const TcxDecompressor::Codec codec = TcxDecompressor::DetectFileCodec(path);
        if (codec == TcxDecompressor::Codec::None) return path;

        // Damaged beyond recovery - say so once, instead of letting the file
        // fall through and be shown as undecodable noise.
        if (codec == TcxDecompressor::Codec::LzssNulEaten) {
            std::cerr << "[Tcx] " << path << ": "
                << TcxDecompressor::DescribeCodec(codec) << "\n";
            return path;
        }

        bool comp = false;
        std::vector<uint8_t> bytes = TcxDecompressor::LoadAndMaybeDecompress(path, &comp);
        if (!comp || bytes.empty()) return path;
        std::filesystem::path src(path);
        std::filesystem::path tmp =
            std::filesystem::temp_directory_path() /
            (src.stem().string() + ".tcx_decompressed" + src.extension().string());
        std::ofstream out(tmp, std::ios::binary | std::ios::trunc);
        if (!out) return path;
        out.write(reinterpret_cast<const char*>(bytes.data()), (std::streamsize)bytes.size());
        out.close();
        if (out_was_compressed) *out_was_compressed = true;
        return tmp.string();
        };

    while (!glfwWindowShouldClose(window) && m_Running) {
        glfwPollEvents();

        ImGui_ImplOpenGL3_NewFrame();
        ImGui_ImplGlfw_NewFrame();
        ImGui::NewFrame();

        anim_viewport.Tick(ImGui::GetIO().DeltaTime);

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

        ImGui::Begin("Texture Archive (P3TEX / X3TEX)");

        if (ImGui::Button("Load P3TEX / X3TEX...", ImVec2(220, 0))) {
            std::string path = FileDialog::OpenFile(
                "Texture Archives\0*.p3tex;*.x3tex\0P3TEX (PS3)\0*.p3tex\0X3TEX (Xbox 360)\0*.x3tex\0All Files\0*.*\0");
            if (!path.empty()) {
                // Xbox 360 retail .x3tex files use the proprietary Tcx
                // compression. The decompressor passes uncompressed files
                // (PS3 .p3tex and demo .x3tex) through unchanged.
                bool comp = false;
                std::string load_path = resolveDecompressedPath(path, &comp);
                m_P3TexParser.Load(load_path);
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
            ImGui::TextWrapped("Load a .p3tex (PS3) or .x3tex (Xbox 360) texture archive.");
        }

        ImGui::End();

        ImGui::Begin("Texture Viewer (P3TEX)");
        tex_viewer.Render();
        ImGui::End();

        ImGui::Begin("File Textures (.e / .bop / .bmd / .p3obj / .ep3)");
        efile_tex_viewer.Render();
        ImGui::End();

        ImGui::Begin("File Text (.e / .bop / .bmd / .p3obj)");
        text_extractor.Render();
        ImGui::End();

        ImGui::Begin("Audio Viewer (CSF / CXS)");
        csf_viewer.Render();
        ImGui::End();

        ImGui::Begin("AI Script Viewer");
        ai_script_viewer.Render();
        ImGui::End();

        ImGui::Begin("Tutorial Viewer");
        tutorial_viewer.Render();
        ImGui::End();

        ImGui::Begin("Cutscene Script (Stage 0)");
        cutscene_viewport.Render();
        ImGui::End();

        ImGui::Begin("Animation Viewport");
        anim_viewport.Render();
        ImGui::End();

        ImGui::Begin("Font Loader (.fnt / FONT block)");
        if (ImGui::Button("Load Font...", ImVec2(220, 0))) {
            std::string path = FileDialog::OpenFile(
                "Font / any file\0*.fnt;*.bmd;*.*\0FNT Fonts\0*.fnt\0All Files\0*.*\0");
            if (!path.empty()) {
                // A .fnt loads directly; for any other file the parser scans for
                // an embedded FONT block, so a BMD (or anything carrying a font)
                // can be opened here too.
                bool comp = false;
                std::string load_path = resolveDecompressedPath(path, &comp);
                if (!fnt_viewer.LoadFile(load_path))
                    fnt_viewer.Clear();
            }
        }
        if (fnt_viewer.IsLoaded()) {
            ImGui::SameLine();
            ImGui::TextColored(ImVec4(0.4f, 1.0f, 0.4f, 1.0f), "✓ Loaded");
            ImGui::Text("File: %s", fnt_viewer.GetFilename().c_str());
            ImGui::TextWrapped("Type text with the on-screen keyboard in the "
                "'Font Viewer' window.");
            if (ImGui::Button("Clear", ImVec2(100, 0)))
                fnt_viewer.Clear();
        }
        else {
            ImGui::SameLine();
            ImGui::TextDisabled("Not loaded");
            ImGui::TextWrapped("Load a .fnt font, or any file that contains a "
                "FONT block (it will be found automatically).");
        }
        ImGui::End();

        ImGui::Begin("Font Viewer (write with .fnt)");
        fnt_viewer.Render();
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
                was_compressed = false;
                csf_checked.clear();

                // Drop the previous file's file-scoped inspector state up front,
                // so a format that never repopulates current_file_data cannot
                // leave the last file's data on screen.
                chunk_inspector.ClearFileContext();

                // Xbox 360 retail auto-decompression. If this file uses the
                // proprietary Tcx arithmetic+LZSS format, decompress it once
                // into a temp file and route all downstream parsers through
                // that path. Demo / already-uncompressed files pass straight
                // through and `effective_path` stays equal to the original.
                std::string effective_path = resolveDecompressedPath(current_file_path, &was_compressed);

                if (extension == ".csf") {
                    csf_viewer.LoadFile(effective_path);
                }
                else if (extension == ".cxs") {
                    csf_viewer.LoadCXSFile(effective_path);
                }
                else if (extension == ".wav") {
                    // Header only. These are the biggest files in the game
                    // (up to 250 MB) and carry no chunks worth scanning, so
                    // they must not fall through to the generic byte scan.
                    csf_viewer.LoadWavFile(effective_path);
                }
                else if (extension == ".bmd") {
                    auto container = ContainerParser::Parse(effective_path);

                    if (container.type != ContainerType::Unknown) {
                        container_sections = container.sections;
                        loaded_chunks = container.all_chunks;
                        is_container_file = true;

                        std::ifstream file(effective_path, std::ios::binary);
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
                        csf_viewer.SetFileContext(current_file_data, loaded_chunks);
                        anim_viewport.LoadFromChunks(current_file_data, loaded_chunks);
                        chunk_inspector.SetFileContext(current_file_data);

                        if (container.has_csf) {
                            csf_viewer.LoadFile(effective_path);
                        }
                    }
                }
                else if (extension == ".e" || extension == ".bop" || extension == ".p3obj") {
                    // The whole file is needed to tell a tutorial from a cutscene:
                    // both open with the same magic, the same 0x1FE8 code size and a
                    // "CSL " voice table at 0x2000, and only differ in whether the
                    // rest of the file carries scene chunks. A header-sized probe
                    // cannot see that, so read once and reuse the buffer for the
                    // AI-script probe as well.
                    std::vector<uint8_t> probe_data;
                    {
                        std::ifstream probe(effective_path, std::ios::binary);
                        if (probe) {
                            probe.seekg(0, std::ios::end);
                            const std::streamoff plen = probe.tellg();
                            probe.seekg(0);
                            if (plen > 0) {
                                probe_data.resize(static_cast<size_t>(plen));
                                probe.read(reinterpret_cast<char*>(probe_data.data()), plen);
                            }
                        }
                    }

                    if (TutorialParser::IsTutorialFile(probe_data.data(), probe_data.size())) {
                        tutorial_viewer.Load(effective_path);
                        ai_script_viewer.Clear();
                        selected_chunk_idx = -1;
                    }
                    else if (AIScriptParser::IsAIScript(probe_data.data(), probe_data.size())) {
                        ai_script_viewer.Load(effective_path);
                        tutorial_viewer.Clear();
                        selected_chunk_idx = -1;
                    }
                    else {
                        ai_script_viewer.Clear();
                        tutorial_viewer.Clear();

                        if (extension == ".bop") {
                            loaded_chunks = BOPParser::Parse(effective_path);
                        }
                        else {
                            loaded_chunks = EFileParser::Parse(effective_path);
                        }

                        std::ifstream file(effective_path, std::ios::binary);
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
                        csf_viewer.SetFileContext(current_file_data, loaded_chunks);
                        anim_viewport.LoadFromChunks(current_file_data, loaded_chunks);

                        // Stage 0: if it's a .e with the script magic (evdata cutscene),
                        // disassemble the scene script. Additive: does not affect other viewers.
                        if (extension == ".e" &&
                            CutsceneScriptParser::IsCutscene(
                                current_file_data.data(), current_file_data.size()))
                            cutscene_viewport.Load(effective_path);
                        else
                            cutscene_viewport.Clear();
                    }
                }
                else if (extension == ".ep3") {
                    Ep3File ep3_tmp;
                    if (Ep3Parser::Load(effective_path, ep3_tmp)) {
                        current_file_data = ep3_tmp.raw;

                        loaded_chunks.clear();
                        for (size_t fi = 0; fi < ep3_tmp.frames.size(); fi++) {
                            const Ep3Frame& fr = ep3_tmp.frames[fi];
                            Chunk c;
                            c.magic = 0x3358544E;
                            c.size = static_cast<uint32_t>(fr.pixel_size + 0x80);
                            c.offset = fr.pixel_offset - 0x80;
                            c.type = ChunkType::NTX3;
                            snprintf(c.name, sizeof(c.name), "frame%zu_%dx%d%s",
                                fi, fr.width, fr.height,
                                fr.swizzled ? "_swz" : "");
                            loaded_chunks.push_back(c);
                        }
                        efile_tex_viewer.LoadFromEp3(effective_path);
                    }
                }
                else if (extension == ".tex") {
                    auto tex = TEXParser::Parse(effective_path);

                    if (tex.valid) {
                        loaded_chunks = tex.chunks;

                        std::ifstream file(effective_path, std::ios::binary);
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
                        csf_viewer.SetFileContext(current_file_data, loaded_chunks);
                    }
                }
                else if (extension == ".p3tex" || extension == ".x3tex") {
                    // Texture archive selected from the file browser: route
                    // through the same path as the dedicated Load button.
                    m_P3TexParser.Load(effective_path);
                    tex_viewer.SetParser(&m_P3TexParser);
                }
                else if (extension == ".fnt") {
                    // Bitmap font selected from the browser: route through the
                    // same path as the dedicated Load Font button.
                    if (!fnt_viewer.LoadFile(effective_path))
                        fnt_viewer.Clear();
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
            if (was_compressed) {
                ImGui::TextColored(ImVec4(1.0f, 0.7f, 0.2f, 1.0f),
                    "[Xbox 360 retail - auto-decompressed]");
            }
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

                std::vector<int> csf_indices;
                for (int i = 0; i < (int)loaded_chunks.size(); i++) {
                    if (loaded_chunks[i].type == ChunkType::CSF) csf_indices.push_back(i);
                }

                if (!csf_indices.empty()) {
                    ImGui::Separator();
                    ImGui::Text("CSF Audio Export (%d tracks)", (int)csf_indices.size());
                    ImGui::Separator();

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

                    ImGui::BeginDisabled(csf_checked.empty());
                    char sel_label[48];
                    snprintf(sel_label, sizeof(sel_label), "Export Selected (%d)", (int)csf_checked.size());
                    if (ImGui::Button(sel_label, ImVec2(-1, 0))) {
                        std::string base = FileDialog::SaveFile("Audio\0*.at3;*.xma\0All Files\0*.*\0");
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
                                        char suf[40];
                                        snprintf(suf, sizeof(suf), "_%03d_%03d%s", ci_n, (int)ci,
                                            CSFCodecExtension(csf.codec));
                                        if (CSFParser::ExportClip(csf, (uint32_t)ci, base + suf)) n++;
                                    }
                                }
                                ci_n++;
                            }
                            csf_batch_exported = n;
                            show_csf_batch_result = true;
                        }
                    }
                    ImGui::EndDisabled();

                    if (ImGui::Button("Export All CSF", ImVec2(-1, 0))) {
                        std::string base = FileDialog::SaveFile("Audio\0*.at3;*.xma\0All Files\0*.*\0");
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
                                        char suf[40];
                                        snprintf(suf, sizeof(suf), "_%03d_%03d%s", j, (int)ci,
                                            CSFCodecExtension(csf.codec));
                                        if (CSFParser::ExportClip(csf, (uint32_t)ci, base + suf)) n++;
                                    }
                                }
                            }
                            csf_batch_exported = n;
                            show_csf_batch_result = true;
                        }
                    }

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
    case ChunkType::NTX2:
        return ImVec4(1.0f, 0.5f, 0.8f, 1.0f);
    case ChunkType::NTEX:
        return ImVec4(1.0f, 0.6f, 0.9f, 1.0f);
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