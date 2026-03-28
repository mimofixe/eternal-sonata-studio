#pragma once
#include "EFileParser.h"
#include <string>
#include <vector>
#include <cstdint>

enum class ContainerType {
    BMD,
    SCP,
    CAMP,
    Unknown
};

struct ContainerSection {
    size_t offset;
    std::string type;
    std::string magic;
    uint32_t size;

    std::vector<Chunk> chunks;
    std::string mefc_name;

    Chunk chunk;

    ContainerSection() : offset(0), size(0) {}
};

struct ContainerFile {
    std::string filename;
    ContainerType type;
    uint32_t file_size;
    uint32_t num_sections;

    // Offset table is always at 0x10.
    // Some containers also have a leading section stored in bytes 12-15.
    uint32_t offset_table_start;
    uint32_t table_count;
    bool has_leading_section;
    uint32_t leading_section_offset;

    std::vector<ContainerSection> sections;
    std::vector<Chunk> all_chunks;

    int ntx3_count;
    int mefc_count;
    int chunk_count;
    bool has_csf;
    bool has_font;

    ContainerFile()
        : type(ContainerType::Unknown), file_size(0),
        num_sections(0), offset_table_start(0x10),
        table_count(0), has_leading_section(false),
        leading_section_offset(0),
        ntx3_count(0), mefc_count(0), chunk_count(0),
        has_csf(false), has_font(false) {
    }
};

class ContainerParser {
public:
    static ContainerFile Parse(const std::string& filepath);

private:
    static bool ParseHeader(const uint8_t* data, size_t size, ContainerFile& out);

    static bool ParseSection(const uint8_t* data, size_t offset,
        size_t file_size, ContainerSection& out);

    static bool ParseMefcContainer(const uint8_t* data, size_t offset,
        size_t mefc_size, ContainerSection& out);

    static void ParseNOBJContainer(const uint8_t* data, size_t offset,
        size_t nobj_size, size_t file_size,
        ContainerSection& out);

    static void ParseSCPContent(const uint8_t* data, size_t file_size,
        ContainerFile& out);

    static void CollectChunks(const ContainerSection& sec, ContainerFile& out);

    static ContainerType DetectType(const char magic[4]);
    static uint32_t ReadU32BE(const uint8_t* data);
    static ChunkType GetChunkTypeFromMagic(uint32_t magic);
};