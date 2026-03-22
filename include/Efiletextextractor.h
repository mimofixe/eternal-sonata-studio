#pragma once
#include <vector>
#include <string>
#include <cstdint>

class EFileTextExtractor {
public:
    EFileTextExtractor();
    ~EFileTextExtractor();

    void LoadFromFile(const std::vector<uint8_t>& file_data);
    void Clear();
    void Render();

private:
    std::vector<std::string> ExtractAllText(const uint8_t* data, size_t size, size_t min_length);
    std::vector<std::string> FilterDialogue(const std::vector<std::string>& strings);

    std::vector<std::string> m_AllStrings;
    std::string m_CurrentFilename;
    bool m_HasData;
};
