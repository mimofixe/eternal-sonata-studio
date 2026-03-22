# Eternal Sonata Studio

A reverse-engineering toolkit for viewing and extracting assets from Eternal Sonata (PlayStation 3 only, Xbox 360 maybe later).

## Overview

Eternal Sonata Studio provides low-level access to game files, enabling inspection of textures, 3D models, dialogue text, and internal file structures. This tool was developed through reverse engineering of the game's proprietary formats. The goal is to develop it enough so pretty much anything in the game can be viewed and exported.

## Features

### File Parsing
- Parse .e container files (maps, cutscenes, models)
- Chunk-based file structure inspection (NSHP, NTX3, NOBJ, NCAM, NLIT, NMTR)
- Hex viewer for raw binary data analysis

### Texture Viewing
- NTX3 texture format support (DXT1/DXT5 compression)
- P3TEX texture archive browsing
- Grid and detail viewing modes
- Texture dimensions: 16x16 to 4096x4096

### 3D Model Viewing
- NSHP mesh format parser
- OpenGL 4.5 rendering with wireframe/solid modes
- Camera controls (orbit, pan, zoom)
- Vertex and triangle count statistics

### Text Extraction
- Dialogue and in-game text extraction
- Automatic filtering of technical strings
- Support for text markers (<w>, \n)
- Character name detection

## Build Requirements

### Compiler
- C++17 or later
- CMake 3.15+
- OpenGL 4.5+

### Platforms
- Windows: Visual Studio 2019+ or MinGW
- Linux: GCC 9+ or Clang 10+
- macOS: Xcode 11+ or Clang 10+

### Dependencies
- GLFW 3.3+ (windowing)
- GLM 0.9.9+ (mathematics)
- Dear ImGui 1.89+ (UI)
- glad (OpenGL loader)
- bcdec (DXT decompression)

## Compilation

### Windows (Visual Studio)
```bash
cmake -B build -G "Visual Studio 16 2019"
cmake --build build --config Release
```

### Linux / macOS
```bash
cmake -B build -DCMAKE_BUILD_TYPE=Release
make -C build
```

## Usage

```bash
./build/es_studio
```

## File Format Documentation

### .e Files
Container format holding multiple chunks. Each chunk has:
- 4-byte magic identifier (e.g., "NSHP", "NTX3")
- 4-byte size field
- Variable-length data payload

### NTX3 Chunks
Texture data with DXT1 or DXT5 compression.
- Header: 128 bytes
- Dimensions: 16-bit big-endian at offset 0x20-0x23
- Format byte at offset 0x18 (0x86 = DXT1, other = DXT5)
- Compressed data follows header

### NSHP Chunks
3D mesh geometry.
- Vertex data (position, normal, UV)
- Index buffer for triangles
- Format varies by model complexity

### P3TEX Files
Archive containing multiple NTX3 textures.
- Index of texture offsets
- Sequential NTX3 chunks

## Project Structure

```
eternal-sonata-studio/
├── include/            # Header files
│   ├── Application.h
│   ├── EFileParser.h
│   ├── NTX3Parser.h
│   └── ...
├── src/                # Source files
│   ├── main.cpp
│   ├── Application.cpp
│   └── ...
├── external/           # Third-party dependencies
└── CMakeLists.txt      # Build configuration
```

## Known Limitations

- Animation data (NMTN) is not yet fully parsed
- Audio files (CSF) are not supported
- Xbox 360 specific compression not implemented
- Material export not available

## Technical Notes

### Texture Decompression
DXT1 and DXT5 textures are decompressed on-the-fly using the bcdec library. Decompressed RGBA data is cached in GPU memory for performance.

### Mesh Rendering
NSHP meshes are parsed into vertex/index buffers and rendered using OpenGL 4.5 core profile. Camera uses arcball rotation for intuitive 3D navigation.

### Text Encoding
Game text uses ASCII encoding with custom markup tags (<w> for wait, \n for line breaks). Character names are hard-coded for detection.

## License

MIT License - see LICENSE file for details.

This project is for educational and preservation purposes. All game assets remain property of their respective copyright holders (Bandai Namco, tri-Crescendo).

## Contributing

Pull requests are welcome. For major changes, please open an issue first to discuss proposed modifications.


## Contact

For questions or bug reports, please open an issue on GitHub.