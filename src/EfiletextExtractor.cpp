#include "EFileTextExtractor.h"
#include <imgui.h>
#include <iostream>
#include <algorithm>

EFileTextExtractor::EFileTextExtractor()
    : m_HasData(false) {
}

EFileTextExtractor::~EFileTextExtractor() {
    Clear();
}

void EFileTextExtractor::Clear() {
    m_AllStrings.clear();
    m_CurrentFilename.clear();
    m_HasData = false;
}

void EFileTextExtractor::LoadFromFile(const std::vector<uint8_t>& file_data) {
    Clear();

    if (file_data.empty()) {
        return;
    }

    // Extrair TODO o texto do ficheiro completo
    std::vector<std::string> all_text = ExtractAllText(file_data.data(), file_data.size(), 10);

    // Filtrar para obter só diálogo
    m_AllStrings = FilterDialogue(all_text);

    m_HasData = true;

    std::cout << "Extracted " << m_AllStrings.size() << " dialogue strings from .e file" << std::endl;
}

void EFileTextExtractor::Render() {
    if (!m_HasData) {
        ImGui::TextDisabled("No file loaded");
        ImGui::Separator();
        ImGui::TextWrapped("Select a .e file in the File Browser to extract all text.");
        return;
    }

    if (m_AllStrings.empty()) {
        ImGui::TextDisabled("No dialogue text found in this file");
        ImGui::Separator();
        ImGui::TextWrapped("This .e file contains no readable dialogue text. "
            "Try a different file or check if it's a map/cutscene file.");
        return;
    }

    static int min_length_filter = 10;
    static bool show_all = false;

    ImGui::Text("Text extracted from entire .e file");
    ImGui::Separator();

    ImGui::SetNextItemWidth(200);
    ImGui::SliderInt("Min Length Filter", &min_length_filter, 5, 50);
    ImGui::SameLine();
    ImGui::Checkbox("Show All (no filter)", &show_all);

    ImGui::Separator();

    // Aplicar filtro de comprimento se necessário
    std::vector<std::string> display_strings;
    if (show_all) {
        display_strings = m_AllStrings;
    }
    else {
        for (const auto& str : m_AllStrings) {
            if (str.length() >= (size_t)min_length_filter) {
                display_strings.push_back(str);
            }
        }
    }

    ImGui::Text("Found %zu strings:", display_strings.size());
    ImGui::Separator();

    ImGui::BeginChild("FileTextScrollRegion", ImVec2(0, 0), true, ImGuiWindowFlags_HorizontalScrollbar);

    for (size_t i = 0; i < display_strings.size(); i++) {
        const std::string& str = display_strings[i];

        // Colorir baseado no conteúdo
        if (str.find("Polka") != std::string::npos ||
            str.find("Allegretto") != std::string::npos ||
            str.find("Beat") != std::string::npos ||
            str.find("Viola") != std::string::npos ||
            str.find("Frederic") != std::string::npos ||
            str.find("Chopin") != std::string::npos ||
            str.find("Salsa") != std::string::npos ||
            str.find("March") != std::string::npos ||
            str.find("Jazz") != std::string::npos) {
            // Nomes de personagens - Amarelo
            ImGui::TextColored(ImVec4(1.0f, 0.8f, 0.3f, 1.0f), "[%zu] %s", i, str.c_str());
        }
        else if (str.find("<w>") != std::string::npos) {
            // Diálogo - Verde
            ImGui::TextColored(ImVec4(0.5f, 1.0f, 0.5f, 1.0f), "[%zu] %s", i, str.c_str());
        }
        else if (str.find("\\n") != std::string::npos) {
            // Opções de menu - Ciano
            ImGui::TextColored(ImVec4(0.5f, 0.8f, 1.0f, 1.0f), "[%zu] %s", i, str.c_str());
        }
        else if (str.find("?") != std::string::npos || str.find("!") != std::string::npos) {
            // Perguntas/exclamações - Branco
            ImGui::TextColored(ImVec4(1.0f, 1.0f, 1.0f, 1.0f), "[%zu] %s", i, str.c_str());
        }
        else {
            // Resto - Cinzento claro
            ImGui::TextColored(ImVec4(0.8f, 0.8f, 0.8f, 1.0f), "[%zu] %s", i, str.c_str());
        }
    }

    ImGui::EndChild();
}

std::vector<std::string> EFileTextExtractor::ExtractAllText(const uint8_t* data, size_t size, size_t min_length) {
    std::vector<std::string> strings;
    std::string current;

    for (size_t i = 0; i < size; i++) {
        uint8_t byte = data[i];

        // Caracteres ASCII imprimíveis
        if ((byte >= 32 && byte <= 126) || byte == '\n' || byte == '\r' || byte == '\t') {
            current += (char)byte;
        }
        else {
            if (current.length() >= min_length) {
                // Limpar espaços
                size_t start = current.find_first_not_of(" \t\n\r");
                size_t end = current.find_last_not_of(" \t\n\r");

                if (start != std::string::npos) {
                    std::string trimmed = current.substr(start, end - start + 1);

                    // Contar letras
                    int alpha_count = 0;
                    for (char c : trimmed) {
                        if (isalpha(c)) alpha_count++;
                    }

                    // Aceitar se tem pelo menos 40% de letras
                    if (!trimmed.empty() && alpha_count > (int)(trimmed.length() * 0.4)) {
                        strings.push_back(trimmed);
                    }
                }
            }
            current.clear();
        }
    }

    // Última string
    if (current.length() >= min_length) {
        size_t start = current.find_first_not_of(" \t\n\r");
        size_t end = current.find_last_not_of(" \t\n\r");
        if (start != std::string::npos) {
            std::string trimmed = current.substr(start, end - start + 1);

            int alpha_count = 0;
            for (char c : trimmed) {
                if (isalpha(c)) alpha_count++;
            }

            if (!trimmed.empty() && alpha_count > (int)(trimmed.length() * 0.4)) {
                strings.push_back(trimmed);
            }
        }
    }

    return strings;
}

std::vector<std::string> EFileTextExtractor::FilterDialogue(const std::vector<std::string>& strings) {
    std::vector<std::string> filtered;

    for (const auto& str : strings) {
        // Ignorar strings técnicas de chunks
        if (str.find("NSHP") != std::string::npos ||
            str.find("NTX3") != std::string::npos ||
            str.find("NOBJ") != std::string::npos ||
            str.find("NMDL") != std::string::npos ||
            str.find("NCAM") != std::string::npos ||
            str.find("NLIT") != std::string::npos ||
            str.find("NFOG") != std::string::npos ||
            str.find("NMTR") != std::string::npos ||
            str.find("NMTN") != std::string::npos ||
            str.find("poly") != std::string::npos ||
            str.find("Surface") != std::string::npos ||
            str.find("texture_") != std::string::npos ||
            str.find("Sphere") != std::string::npos ||
            str.find("_v1") != std::string::npos ||
            str.find("_v2") != std::string::npos ||
            str.find("etc") == 0) {
            continue;
        }

        // Aceitar strings que parecem diálogo/texto do jogo
        bool has_space = str.find(' ') != std::string::npos;
        bool is_dialog = str.find("<w>") != std::string::npos ||
            str.find("\\n") != std::string::npos;
        bool has_punctuation = str.find('.') != std::string::npos ||
            str.find('?') != std::string::npos ||
            str.find('!') != std::string::npos;
        bool is_character_name = str.find("Polka") != std::string::npos ||
            str.find("Allegretto") != std::string::npos ||
            str.find("Beat") != std::string::npos ||
            str.find("Viola") != std::string::npos ||
            str.find("Frederic") != std::string::npos ||
            str.find("Chopin") != std::string::npos;

        if (has_space || is_dialog || has_punctuation || is_character_name) {
            filtered.push_back(str);
        }
    }

    return filtered;
}