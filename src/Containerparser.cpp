#include "ContainerParser.h"
#include <fstream>
#include <cstring>
#include <algorithm>

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

    // SCP has a unique header layout, handled separately
    if (result.type == ContainerType::SCP) {
        ParseSCPContent(data.data(), file_size, result);
        return result;
    }

    // Parse leading section if bytes 12-15 contained a valid section offset
    if (result.has_leading_section) {
        ContainerSection section;
        if (ParseSection(data.data(), result.leading_section_offset, file_size, section)) {
            result.sections.push_back(section);
            CollectChunks(section, result);
        }
    }

    // Parse offset table at 0x10
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

    // SCP stores the CSL offset at bytes 8-11, not num_sections
    if (out.type == ContainerType::SCP) {
        out.num_sections = 0;
        return true;
    }

    if (out.num_sections > 10000) {
        return false;
    }

    // If bytes 12-15 hold a valid offset, it points to a leading section
    // that comes before the offset table (e.g. characterphoto.bmd, camp_char.bmd)
    uint32_t maybe_leading = ReadU32BE(data + 12);
    bool is_tipo_a = (maybe_leading >= 0x10 && maybe_leading < static_cast<uint32_t>(size));

    out.offset_table_start = 0x10;
    out.has_leading_section = is_tipo_a;
    out.leading_section_offset = is_tipo_a ? maybe_leading : 0;
    out.table_count = is_tipo_a ? out.num_sections - 1 : out.num_sections;

    return true;
}

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
        // FIX: pass file_size so ParseMefcContainer can recurse into NOBJ
        return ParseMefcContainer(data, offset, sec_size, file_size, out);
    }

    if (strcmp(magic, "NOBJ") == 0) {
        out.type = "nobj";

        // Initialize out.chunk so the chunks panel can display this section
        out.chunk.offset = offset;
        out.chunk.size = sec_size;
        out.chunk.magic = *reinterpret_cast<const uint32_t*>(data + offset);
        out.chunk.type = ChunkType::NOBJ;
        strncpy(out.chunk.name, "NOBJ", 63);
        out.chunk.name[63] = '\0';

        ParseNOBJContainer(data, offset, sec_size, file_size, out);
        return true;
    }

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

// FIX: added file_size parameter so NOBJ chunks inside Mefc can be recursed into,
// exposing NMDL and NSHP just like EFileParser does for .e files.
bool ContainerParser::ParseMefcContainer(const uint8_t* data, size_t offset,
    size_t mefc_size, size_t file_size, ContainerSection& out) {
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
                // FIX: NOBJ containers are recursed into so that nested NMDL and
                // NSHP chunks appear in the list, matching .e / .p3obj behaviour.
                if (type == ChunkType::NOBJ) {
                    ParseNOBJContainer(data, offset + i, chunk_size, file_size, out);
                    i += chunk_size;
                    continue;
                }

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

// Scans every byte inside the NOBJ and collects all known chunks at any depth,
// matching EFileParser behaviour for .e files. Leaf chunks skip past their data
// to avoid false positives; container types let the scan enter them naturally.
void ContainerParser::ParseNOBJContainer(const uint8_t* data, size_t offset,
    size_t nobj_size, size_t file_size,
    ContainerSection& out) {
    size_t nobj_end = offset + nobj_size;
    if (nobj_end > file_size) nobj_end = file_size;

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

        switch (type) {
        case ChunkType::NOBJ:
            strncpy(chunk.name, "[container]", sizeof(chunk.name) - 1);
            break;

        case ChunkType::NMDL:
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

        if (type != ChunkType::NOBJ && type != ChunkType::NMDL) {
            i += chunk_size - 1;
        }
    }
}

// Adds all chunks from a parsed section into the ContainerFile totals
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

// SCP header: "SCP " + file_size + CSL_offset + NTX3_count + NTX3 offset table
// CSL section: "CSL " + CSF_count + CSF relative offset table
void ContainerParser::ParseSCPContent(const uint8_t* data, size_t file_size,
    ContainerFile& out) {
    uint32_t ntx3_count = ReadU32BE(data + 12);

    for (uint32_t i = 0; i < ntx3_count; i++) {
        size_t tbl_pos = 0x10 + static_cast<size_t>(i) * 4;
        if (tbl_pos + 4 > file_size) break;

        uint32_t off = ReadU32BE(data + tbl_pos);
        if (off + 8 > file_size) continue;
        if (memcmp(data + off, "NTX3", 4) != 0) continue;

        uint32_t chunk_size = ReadU32BE(data + off + 4);
        if (chunk_size == 0 || off + chunk_size > file_size) continue;

        ContainerSection sec;
        sec.offset = off;
        sec.type = "chunk";
        sec.magic = "NTX3";
        sec.size = chunk_size;

        sec.chunk.offset = off;
        sec.chunk.size = chunk_size;
        sec.chunk.magic = *reinterpret_cast<const uint32_t*>(data + off);
        sec.chunk.type = ChunkType::NTX3;
        snprintf(sec.chunk.name, sizeof(sec.chunk.name), "texture_%u", i);

        out.sections.push_back(sec);
        CollectChunks(sec, out);
    }

    // CSL section holds all CSF chunks with offsets relative to the CSL start
    uint32_t csl_off = ReadU32BE(data + 8);
    if (csl_off + 8 > file_size) return;
    if (memcmp(data + csl_off, "CSL ", 4) != 0) return;

    uint32_t csf_count = ReadU32BE(data + csl_off + 4);

    for (uint32_t i = 0; i < csf_count; i++) {
        size_t tbl_pos = csl_off + 8 + static_cast<size_t>(i) * 4;
        if (tbl_pos + 4 > file_size) break;

        uint32_t abs_off = csl_off + ReadU32BE(data + tbl_pos);
        if (abs_off + 8 > file_size) continue;
        if (memcmp(data + abs_off, "CSF ", 4) != 0) continue;

        uint32_t chunk_size = ReadU32BE(data + abs_off + 4);
        if (chunk_size == 0 || abs_off + chunk_size > file_size) continue;

        ContainerSection sec;
        sec.offset = abs_off;
        sec.type = "chunk";
        sec.magic = "CSF ";
        sec.size = chunk_size;

        sec.chunk.offset = abs_off;
        sec.chunk.size = chunk_size;
        sec.chunk.magic = *reinterpret_cast<const uint32_t*>(data + abs_off);
        sec.chunk.type = ChunkType::CSF;
        snprintf(sec.chunk.name, sizeof(sec.chunk.name), "audio_%u", i);

        out.sections.push_back(sec);
        CollectChunks(sec, out);
    }
}

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