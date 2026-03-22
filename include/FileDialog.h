#pragma once
#include <string>

class FileDialog {
public:
    static std::string OpenFile(const char* filter = "All Files (*.*)\0*.*\0");
    static std::string SaveFile(const char* filter = "All Files (*.*)\0*.*\0");
    static std::string OpenFolder();
};