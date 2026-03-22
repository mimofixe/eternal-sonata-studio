#pragma once
#include <string>
#include <vector>
#include <filesystem>

namespace fs = std::filesystem;

struct FileEntry {
    std::string path;
    std::string name;
    bool is_directory;
    size_t size;
    std::vector<FileEntry> children;
};

class FileSystemBrowser {
public:
    FileSystemBrowser();

    void SetRootPath(const std::string& path);
    void Render();

    std::string GetSelectedFile() const { return m_SelectedFile; }
    bool HasSelection() const { return !m_SelectedFile.empty(); }

private:
    void RenderNode(const FileEntry& entry);
    void ScanDirectory(const std::string& path, FileEntry& entry);

    FileEntry m_Root;
    std::string m_RootPath;
    std::string m_SelectedFile;
};