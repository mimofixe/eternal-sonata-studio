#pragma once
#include "EFileParser.h"
#include <vector>
#include <string>

// BOP files use the same chunk-based format as .E files
// They contain: NTX3 (textures), NSHP (shapes), NOBJ (objects), NMDL (models)
// Plus BOP-specific chunks: BOOK, PGHD, TIM, PROG
//
// This parser simply reuses EFileParser since the format is identical
class BOPParser {
public:
    // Parse BOP file - returns same chunk types as .E files
    static std::vector<Chunk> Parse(const std::string& filepath) {
        // BOP files are chunk-based like .E files
        // EFileParser now recognizes BOP chunk types (BOOK, PGHD, TIM, PROG)
        return EFileParser::Parse(filepath);
    }
};