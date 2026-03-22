#include "FileSystemBrowser.h"
#include <imgui.h>
#include <algorithm>

FileSystemBrowser::FileSystemBrowser() {
}

void FileSystemBrowser::SetRootPath(const std::string& path) {
    m_RootPath = path;
    m_Root.path = path;
    m_Root.name = fs::path(path).filename().string();
    m_Root.is_directory = true;
    m_Root.children.clear();
    ScanDirectory(path, m_Root);
}

void FileSystemBrowser::ScanDirectory(const std::string& path, FileEntry& entry) {
    try {
        for (const auto& dir_entry : fs::directory_iterator(path)) {
            FileEntry child;
            child.path = dir_entry.path().string();
            child.name = dir_entry.path().filename().string();
            child.is_directory = dir_entry.is_directory();

            if (!child.is_directory) {
                child.size = fs::file_size(dir_entry.path());
            }
            else {
                child.size = 0;
            }

            if (child.is_directory) {
                ScanDirectory(child.path, child);
            }

            entry.children.push_back(child);
        }

        std::sort(entry.children.begin(), entry.children.end(),
            [](const FileEntry& a, const FileEntry& b) {
                if (a.is_directory != b.is_directory) return a.is_directory;
                return a.name < b.name;
            });
    }
    catch (...) {
    }
}

void FileSystemBrowser::RenderNode(const FileEntry& entry) {
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow | ImGuiTreeNodeFlags_SpanAvailWidth;

    if (!entry.is_directory) {
        flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    }

    if (entry.path == m_SelectedFile) {
        flags |= ImGuiTreeNodeFlags_Selected;
    }

    const char* icon = entry.is_directory ? "" : "";
    bool node_open = ImGui::TreeNodeEx(entry.path.c_str(), flags, "%s %s", icon, entry.name.c_str());

    if (ImGui::IsItemClicked() && !entry.is_directory) {
        m_SelectedFile = entry.path;
    }

    if (!entry.is_directory) {
        ImGui::SameLine(ImGui::GetWindowWidth() - 100);
        if (entry.size < 1024) {
            ImGui::TextDisabled("%zu B", entry.size);
        }
        else if (entry.size < 1024 * 1024) {
            ImGui::TextDisabled("%.1f KB", entry.size / 1024.0f);
        }
        else {
            ImGui::TextDisabled("%.2f MB", entry.size / (1024.0f * 1024.0f));
        }
    }

    if (node_open && entry.is_directory) {
        for (const auto& child : entry.children) {
            RenderNode(child);
        }
        ImGui::TreePop();
    }
}

void FileSystemBrowser::Render() {
    if (m_Root.path.empty()) {
        ImGui::TextDisabled("No folder loaded");
        if (ImGui::Button("Select Folder...")) {
        }
        return;
    }

    RenderNode(m_Root);
}