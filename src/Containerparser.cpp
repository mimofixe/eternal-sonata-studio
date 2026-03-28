#include "ContainerParser.h"
#include <fstream>
#include <cstring>
#include <algorithm>


// Public entry point


ContainerFile ContainerParser::Parse(const std::string& filepath) {
    ContainerFile result;
    result.filename = filepath;

    std::ifstream file(filepath, std::ios::binary);
    if (!file) {
        return result;
    }

    file.seekg(0, std::ios::end);
    size_t file_size = file.tellg();
    file.seekg(0);

    std::vector<uint8_t> data(file_size);
    file.read(reinterpret_cast<char*>(data.data()), file_size);
    file.close();

    if (!ParseHeader(data.data(), file_size, result)) {
        return result;
    }
    if (result.has_leading_section) {
        ContainerSection section;
        if (ParseSection(data.data(), result.leading_section_offset, file_size, section)) {
            result.sections.push_back(section);
            CollectChunks(section, result);
        }
    }

    for (uint32_t i = 0; i < result.table_count; i++) {
        size_t offset_pos = result.offset_table_start + i * 4;
        if (offset_pos + 4 > file_size) {
            break;
        }

        uint32_t section_offset = ReadU32BE(data.data() + offset_pos);
        if (section_offset == 0 || section_offset >= file_size) {
            continue;
        }

        ContainerSection section;
        if (ParseSection(data.data(), section_offset, file_size, section)) {
            result.sections.push_back(section);
            CollectChunks(section, result);
        }
    }

    return result;
}

// ParseHeader
// Header layout (shared by BMD, CAMP, SCP):
//
//   0x00-0x03  magic ("BMD ", "CAMP", "SCP ")
//   0x04-0x07  file_size (BE u32)
//   0x08-0x0B  num_sections (BE u32)
//   0x0C-0x0F  either 0x00000000 (Tipo B) OR offset of section[0] (Tipo A)
//   0x10+      offset table: 4 bytes per entry (BE u32 absolute offsets)


bool ContainerParser::ParseHeader(const uint8_t* data, size_t size, ContainerFile& out) {
    if (size < 16) {
        return false;
    }

    char magic[5] = { 0 };
    memcpy(magic, data, 4);

    out.type = DetectType(magic);
    if (out.type == ContainerType::Unknown) {
        return false;
    }

    out.file_size = ReadU32BE(data + 4);
    out.num_sections = ReadU32BE(data + 8);

    if (out.file_size != static_cast<uint32_t>(size)) {
        return false;
    }

    // Sanity: reject clearly bogus section counts
    if (out.num_sections > 10000) {
        return false;
    }

    uint32_t maybe_leading = ReadU32BE(data + 12);
    bool is_tipo_a = (maybe_leading >= 0x10 && maybe_leading < static_cast<uint32_t>(size));

    out.offset_table_start = 0x10;              
    out.has_leading_section = is_tipo_a;
    out.leading_section_offset = is_tipo_a ? maybe_leading : 0;
    out.table_count = is_tipo_a ? out.num_sections - 1 : out.num_sections;

    return true;
}

// ParseSection

bool ContainerParser::ParseSection(const uint8_t* data, size_t offset,
    size_t file_size, ContainerSection& out) {
    if (offset + 8 > file_size) {
        return false;
    }

    out.offset = offset;

    char magic[5] = { 0 };
    memcpy(magic, data + offset, 4);
    out.magic = magic;

    uint32_t sec_size = ReadU32BE(data + offset + 4);
    out.size = sec_size;

    if (sec_size == 0 || offset + sec_size > file_size) {
        return false;
    }

    if (strcmp(magic, "Mefc") == 0) {
        out.type = "mefc";
        return ParseMefcContainer(data, offset, sec_size, out);
    }

    if (strcmp(magic, "NOBJ") == 0) {
        out.type = "nobj";

        // Always initialize out.chunk so Application.cpp can display this
        // section without reading uninitialized memory (0xCC in MSVC debug).
        out.chunk.offset = offset;
        out.chunk.size = sec_size;
        out.chunk.magic = *reinterpret_cast<const uint32_t*>(data + offset);
        out.chunk.type = ChunkType::NOBJ;
        strncpy(out.chunk.name, "NOBJ", 63);
        out.chunk.name[63] = '\0';

        ParseNOBJContainer(data, offset, sec_size, file_size, out);
        return true;
    }

    // Generic chunk (NTX3, NMTN, CSF, FONT, …)
    out.type = "chunk";

    Chunk chunk;
    chunk.offset = offset;
    chunk.size = sec_size;
    chunk.magic = *reinterpret_cast<const uint32_t*>(data + offset);
    chunk.type = GetChunkTypeFromMagic(chunk.magic);
    strncpy(chunk.name, magic, 63);
    chunk.name[63] = '\0';

    out.chunk = chunk;
    return true;
}

// ParseMefcContainer

bool ContainerParser::ParseMefcContainer(const uint8_t* data, size_t offset,
    size_t mefc_size, ContainerSection& out) {
    const uint8_t* mefc_data = data + offset;

    char name_buffer[64];
    snprintf(name_buffer, sizeof(name_buffer), "Mefc Container @ 0x%zX", offset);
    out.mefc_name = name_buffer;

    size_t i = 0;
    while (i + 8 <= mefc_size) {
        uint32_t chunk_magic = *reinterpret_cast<const uint32_t*>(mefc_data + i);
        ChunkType type = GetChunkTypeFromMagic(chunk_magic);

        if (type != ChunkType::Unknown) {
            uint32_t chunk_size = ReadU32BE(mefc_data + i + 4);

            if (chunk_size > 0 && i + chunk_size <= mefc_size) {
                Chunk chunk;
                chunk.offset = offset + i;
                chunk.size = chunk_size;
                chunk.magic = chunk_magic;
                chunk.type = type;

                char chunk_name[5] = { 0 };
                memcpy(chunk_name, &chunk_magic, 4);
                strncpy(chunk.name, chunk_name, 63);
                chunk.name[63] = '\0';

                out.chunks.push_back(chunk);
                i += chunk_size;
                continue;
            }
        }

        i++;
    }

    return !out.chunks.empty();
}

// ParseNOBJContainer
// NOBJ is a sub-container found in BMD files (e.g. appkeep2.bmd).
// Structure: NOBJ header (8) → NPAD → NMDL → [NTX3, NSHP, NMTN, ...]
// Chunks are nested: NTX3/NSHP are INSIDE NMDL, which is INSIDE NOBJ.

void ContainerParser::ParseNOBJContainer(const uint8_t* data, size_t offset,
    size_t nobj_size, size_t file_size,
    ContainerSection& out) {
    size_t nobj_end = offset + nobj_size;
    if (nobj_end > file_size) nobj_end = file_size;

    // Start after the 8-byte NOBJ header.
    for (size_t i = offset + 8; i + 8 <= nobj_end; i++) {
        uint32_t magic = *reinterpret_cast<const uint32_t*>(data + i);
        ChunkType type = GetChunkTypeFromMagic(magic);

        if (type == ChunkType::Unknown) continue;

        uint32_t chunk_size = ReadU32BE(data + i + 4);
        if (chunk_size == 0 || i + chunk_size > nobj_end) continue;

        Chunk chunk;
        chunk.offset = i;
        chunk.size = chunk_size;
        chunk.magic = magic;
        chunk.type = type;
        memset(chunk.name, 0, sizeof(chunk.name));

        // Assign name using the same convention as EFileParser
        switch (type) {
        case ChunkType::NOBJ:
            strncpy(chunk.name, "[container]", sizeof(chunk.name) - 1);
            break;

        case ChunkType::NMDL:
            // Model name string starts 16 bytes into the chunk
            if (i + 16 < nobj_end) {
                size_t j = 0;
                for (; j < sizeof(chunk.name) - 1 && i + 16 + j < nobj_end; j++) {
                    uint8_t c = data[i + 16 + j];
                    if (c >= 32 && c < 127) chunk.name[j] = c;
                    else break;
                }
            }
            if (chunk.name[0] == '\0')
                strncpy(chunk.name, "[model]", sizeof(chunk.name) - 1);
            break;

        case ChunkType::NSHP:
        case ChunkType::NMTN: {
            // Name string starts immediately after the 8-byte header
            size_t j = 0;
            for (; j < sizeof(chunk.name) - 1 && i + 8 + j < nobj_end; j++) {
                uint8_t c = data[i + 8 + j];
                if (c != 0) chunk.name[j] = c;
                else break;
            }
            if (chunk.name[0] == '\0') {
                strncpy(chunk.name,
                    type == ChunkType::NSHP ? "[mesh]" : "[anim]",
                    sizeof(chunk.name) - 1);
            }
            break;
        }

        case ChunkType::NTX3: {
            int ntx3_n = 0;
            for (const auto& ch : out.chunks)
                if (ch.type == ChunkType::NTX3) ntx3_n++;
            snprintf(chunk.name, sizeof(chunk.name), "texture_%d", ntx3_n);
            break;
        }

        case ChunkType::NMTR:
            snprintf(chunk.name, sizeof(chunk.name), "material_%zu", out.chunks.size());
            break;

        case ChunkType::NCAM:
            snprintf(chunk.name, sizeof(chunk.name), "camera_%zu", out.chunks.size());
            break;

        default: {
            char magic_str[5] = { 0 };
            memcpy(magic_str, &magic, 4);
            strncpy(chunk.name, magic_str, sizeof(chunk.name) - 1);
            break;
        }
        }

        out.chunks.push_back(chunk);

        // After finding a chunk, skip to its end so we don't pick up the
        // same magic again from within its header. We still scan INSIDE
        // containers (NOBJ, NMDL) by NOT advancing here for those types —
        // the byte scan naturally enters them on the next iterations.
        // For leaf chunks, advance past the entire chunk to avoid false
        // positives from pixel/vertex data that might contain magic bytes.
        if (type != ChunkType::NOBJ && type != ChunkType::NMDL) {
            i += chunk_size - 1;  // -1 because loop does i++
        }
    }
}

// CollectChunks — add a section's chunks into the ContainerFile totals

void ContainerParser::CollectChunks(const ContainerSection& sec, ContainerFile& out) {
    if (sec.type == "chunk") {
        out.all_chunks.push_back(sec.chunk);
        out.chunk_count++;

        if (sec.chunk.type == ChunkType::NTX3) {
            out.ntx3_count++;
        }
        else if (sec.chunk.type == ChunkType::CSF) {
            out.has_csf = true;
        }
        else if (sec.chunk.type == ChunkType::FONT) {
            out.has_font = true;
        }
    }
    else if (sec.type == "mefc" || sec.type == "nobj") {
        for (const auto& chunk : sec.chunks) {
            out.all_chunks.push_back(chunk);
            out.chunk_count++;

            if (chunk.type == ChunkType::NTX3) {
                out.ntx3_count++;
            }
            else if (chunk.type == ChunkType::CSF) {
                out.has_csf = true;
            }
            else if (chunk.type == ChunkType::FONT) {
                out.has_font = true;
            }
        }
    }
}

// Helpers

ContainerType ContainerParser::DetectType(const char magic[4]) {
    if (strncmp(magic, "BMD ", 4) == 0) return ContainerType::BMD;
    if (strncmp(magic, "SCP ", 4) == 0) return ContainerType::SCP;
    if (strncmp(magic, "CAMP", 4) == 0) return ContainerType::CAMP;
    return ContainerType::Unknown;
}

uint32_t ContainerParser::ReadU32BE(const uint8_t* data) {
    return (static_cast<uint32_t>(data[0]) << 24) |
        (static_cast<uint32_t>(data[1]) << 16) |
        (static_cast<uint32_t>(data[2]) << 8) |
        (static_cast<uint32_t>(data[3]));
}

ChunkType ContainerParser::GetChunkTypeFromMagic(uint32_t magic) {
    if (magic == MAGIC_NTX3) return ChunkType::NTX3;
    if (magic == MAGIC_NSHP) return ChunkType::NSHP;
    if (magic == MAGIC_NOBJ) return ChunkType::NOBJ;
    if (magic == MAGIC_NMDL) return ChunkType::NMDL;
    if (magic == MAGIC_NMTN) return ChunkType::NMTN;
    if (magic == MAGIC_NCAM) return ChunkType::NCAM;
    if (magic == MAGIC_NMTR) return ChunkType::NMTR;
    if (magic == MAGIC_NLIT) return ChunkType::NLIT;
    if (magic == MAGIC_NFOG) return ChunkType::NFOG;
    if (magic == MAGIC_BOOK) return ChunkType::BOOK;
    if (magic == MAGIC_SONG) return ChunkType::SONG;
    if (magic == MAGIC_PGHD) return ChunkType::PGHD;
    if (magic == MAGIC_TIM)  return ChunkType::TIM;
    if (magic == MAGIC_PROG) return ChunkType::PROG;
    if (magic == MAGIC_CSF)  return ChunkType::CSF;
    if (magic == MAGIC_FONT) return ChunkType::FONT;
    return ChunkType::Unknown;
}