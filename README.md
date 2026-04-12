# Eternal Sonata Studio

A reverse-engineering toolkit for viewing and extracting assets from Eternal Sonata (PlayStation 3). Xbox 360 support may follow later.

## Overview

Eternal Sonata Studio provides low-level access to game files, enabling inspection of textures, 3D models, skeletons, dialogue text, audio, AI behaviour scripts, and internal file structures. This tool was developed through reverse engineering of the game's proprietary formats. The goal is to develop it enough so pretty much anything in the game can be viewed and exported.

## Features

### File Parsing

- Parse `.e` container files (maps, cutscenes, models)
- Parse `.e` AI behaviour scripts (magic `0x00000181`) - separate format, see below
- Parse `.e` tutorial files (magic `0x00000181`) - CSF audio + subtitle containers, detected automatically
- Parse `.bop` container files
- Parse `.bmd` and `.camp` container files with full section table support
- Parse `.tex` texture atlas per language
- NOBJ sub-container parsing - finds all nested chunks (NTX3, NSHP, NMTN, NBN2, etc.) at any depth
- P3OBJ object data parsing
- Chunk-based file structure inspection (NSHP, NTX3, NOBJ, NMDL, NMTN, NBN2, NCAM, NLIT, NMTR, NFOG, NMTB, NDYN, NLC2, NCLS, CSF, FONT)
- Hex viewer for raw binary data analysis

### Texture Viewing

- NTX3 texture format support (DXT1/DXT5/DXT1+mip/DXT5+mip/RGBA8 compression)
- Full PS3 DXT5 block-swap decoding (Namco stores colour and alpha sub-blocks in reverse order)
- `.tex` file support
- P3TEX texture archive browsing with grid and detail viewing modes
- Texture dimensions: 16x16 to 4096x4096
- PNG export with proper transparency (0xEE fill regions exported as alpha=0)
- Works across `.e`, `.bop`, `.bmd`, and `.camp` files

### Audio

- CSF audio container parsing - loads from file or directly from memory buffers inside containers
- Export individual tracks or all tracks at once to `.at3` (ATRAC3)
- Playback-ready with foobar2000 + ATRAC plugin or compatible PS3 audio tools

### 3D Model Viewing

- NSHP mesh format parser - complete rewrite with all five vertex strides confirmed
- Restart-aware tristrip decoding (`0xFFFF` primitive restart tokens handled correctly)
- Correct geometry rendering for map meshes (strides 16/20/24/28) and character meshes (stride 32)
- 10:10:10:2 packed normal decoding for static map geometry
- Stride-48 particle meshes rendered as GL_POINTS (billboard particle systems)
- OpenGL 4.5 rendering with camera controls (orbit, pan, zoom) and auto-fit on load
- Vertex and triangle count statistics

### NMDL Model Viewer (textured)

- Select any NMDL chunk and click **Load Textured Model in Viewport** to load the full model
- All meshes, materials and textures are loaded in one step from the NMDL envelope
- Per-section draw calls: each face section binds the correct diffuse texture before drawing
- Texture chain: `FaceSection.mat_id -> NMTR[mat_id].diffuse_img_id -> NTX3[img_id]`
- Sections without a texture fall back to flat shaded colour
- NMDL and single NSHP modes are mutually exclusive and share the same viewport

**Map texture support** -- map NMDL chunks reference an external `.p3tex` archive rather than embedding NTX3 textures directly. A **Load .p3tex for this map** button in the Model Info tab loads the archive and uploads all textures. The material-to-texture chain resolves identically to the embedded path.

**DXT5 blend pass** -- materials with DXT5 diffuse textures (full 8-bit alpha) are drawn in a second render pass with `GL_BLEND` enabled and depth writes suppressed. Transparent sprites, leaf overlays, and ground decals composite correctly over opaque geometry.

**Terrain UV** -- stride-24 map meshes use a two-field UV: `UV = i16/32767 + f16`. The f16 field alone collapses to zero after `GL_REPEAT` for integer offset values. The i16 component provides the fractional base for correct texture coverage. Sprite/prop meshes on the same stride use f16 only, detected per-vertex by `|f16| > 1.0`.

**Scene lighting** -- when a map model is loaded, a **Scene Lighting** toggle appears in the viewport toolbar if NLIT data is present. Enabling it switches to an ambient + directional lighting model using colours read from the NLIT chunk. Pitch and Yaw sliders let you adjust the light direction interactively. Lighting is disabled by default; unlit output matches the raw texture colours exactly.

### Skeleton Viewing

- NBN2 bone parser - confirmed layout from cross-referencing a 2015 Blender importer
- Bind-pose world positions computed via BONESPACE algorithm (Euler XYZ, rotation encoding i16/4096)
- Select any NBN2 chunk in the file browser -> "Skeleton Info" tab -> **Load Skeleton in Viewport**
- Mesh and skeleton can be loaded simultaneously and toggled independently
- Viewport rendering: yellow GL_LINES (parent->child, depth-tested) and white GL_POINTS (joints, always on top of mesh)
- Camera auto-fits to skeleton bounding box on load
- "Flip Y" toggle for PS3 Y-down to OpenGL Y-up conversion
- Bone list in ChunkInspector shows all bones inline with Euler angles, local positions, and type colour-coding (dynamic chains in blue, effectors in green)

### AI Behaviour Scripts

Full analysis of the compiled AI scripts that drive enemy and boss behaviour in battle. These are `.e` files with magic `0x00000181` - a different format from the regular `.e` containers.

**Disassembler** - decodes the custom stack-based VM bytecode instruction by instruction, with named operands, register names, and float constant annotations.

**Two memory spaces** - the VM has a local frame (per-entity registers) and a shared world space (cross-entity coordination for formations and attack synchronisation). Both are decoded and colour-coded in the disassembly view.

**Action table** - the viewer decodes the action registration table embedded in every file, showing the 8 available combat actions and the C-engine handler each one maps to: Walk, Turn, Guard, Flank Back, Flank Left (A-F-L), Flank Right (A-F-R), Forward Attack, Special Attack.

**API catalog** - maps all 208 known `CALLAPI` addresses to named functions in two groups: special ability functions (one per boss/enemy) and per-enemy function vtables (`init`, `get_speed`, `get_range`, `seek_target`, `combat_state`, `special_seq`).

**File tiers** - simple files (114 nodes, no debug strings) share a common combat template with one unique special ability each; complex files (370-600+ nodes) have a fully unique behaviour tree per enemy.

Viewer tabs: **Overview - Disassembly - Actions - API Calls - Ctrl Flow - Strings** (complex only)

### Text Extraction

- Dialogue and in-game text extraction
- Automatic filtering of technical strings
- Support for text markers (`<WAIT>`, `\n`)
- Character name detection

### Scene Data

- Camera parser (position, rotation, FOV)
- Light parser (type, colour, intensity)
- Fog and material parsers
- NMTR material parser (diffuse and alpha NTX3 index per material)

---

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

**Windows (Visual Studio)**
```
cmake -B build -G "Visual Studio 16 2019"
cmake --build build --config Release
```

**Linux / macOS**
```
cmake -B build -DCMAKE_BUILD_TYPE=Release
make -C build
```

## Usage

```
./build/es_studio
```

---

## File Format Documentation

### `.e` Container Files

Container format holding multiple chunks. Each chunk has a 4-byte magic identifier (e.g. `NSHP`, `NTX3`), a 4-byte size field, and a variable-length data payload.

### `.e` AI Behaviour Scripts

Compiled behaviour tree scripts with magic `0x00000181`. Distinct from the container format above. Each file is a custom stack-based VM bytecode with up to 600+ nodes. The VM has two memory spaces: a local per-entity frame and a shared world space for cross-entity coordination. The game binary is called via `CALLAPI(address)` with absolute PS3 binary addresses.

### `.e` Tutorial Files

Tutorial voiceover files also use magic `0x00000181` but contain a CSF audio stream and subtitle text chunks rather than VM bytecode. Detected by checking `code_size > 0x100000` at offset `0x14`.

### `.bop` Files

Same chunk structure as `.e` container files. Used for character and object data.

### `.bmd` / `.camp` Files

Higher-level container with a section table. Supported layouts:

- Direct section table at `0x10` (e.g. `appkeep.bmd`)
- Leading section offset at `0x0C` followed by table at `0x10` (e.g. `characterphoto.bmd`, `camp_char.bmd`)
- NOBJ sub-containers holding nested model data (e.g. `appkeep2.bmd`)

### NTX3 Chunks

Texture data. The format byte at offset `0x18` determines the compression type. Bit `0x20` indicates a mipmap chain and is masked off before comparing: `(fmt & 0xDF) == 0x86` -> DXT1, `(fmt & 0xDF) == 0x88` -> DXT5, `fmt == 0xA5` -> raw RGBA8.

- Header: 136 bytes (`0x88`). Pixel data starts at `+0x88`.
- Dimensions: 16-bit big-endian at offsets `0x20` (width) and `0x22` (height)
- DXT5: colour sub-block first (bytes 0-7), alpha sub-block second (bytes 8-15) -- halves are swapped before calling `bcdec_bc3`
- RGBA8 (0xA5): header height is `actual_height / 2`; real height = `data_size / (width x 4)`

### P3TEX Files

Archive containing multiple NTX3 textures. Pixel data for each chunk starts at `chunk_offset + 0x88`. DXT5 textures use the PS3 block-swap.

### NSHP Chunks

3D mesh geometry. Five vertex formats exist depending on mesh type:

- Stride 16: position only (water, collision geometry)
- Stride 20: position + 10:10:10:2 packed normal + UV (i16/32767)
- Stride 24: position + normal + two-field UV (`i16/32767 + f16` for tiling terrain; `f16` only for sprites, detected per-vertex by `|f16| > 1.0`)
- Stride 28: position + normal + unknown fields + UV as f16 pair at `+0x18/+0x1A`. The i16 pair at `+0x10/+0x12` and f16 pair at `+0x14/+0x16` are not UV (blend weights / lightmap / sentinel data).
- Stride 32: skinned characters - position + bone weights (u8x4/255) + bone indices + unknown + UV (f16)
- Stride 48: particle / billboard positions, no index buffer, 12-byte footer

All indexed formats use restartable tristrips with `0xFFFF` as the primitive restart token.

### NMDL Chunks

Top-level model container. Contains all NSHP meshes, NTX3 textures and the NMTR material table for one model. Map models carry no embedded NTX3; textures come from a companion `.p3tex` archive loaded separately. Sub-chunks are found by byte-scanning within the NMDL size boundary.

### NBN2 Chunks

Skeleton bone table. Each entry is 64 bytes: name (16 bytes), flags (u16), parent/child/sibling indices (i16 each), Euler XYZ rotation as `i16 / 4096` radians, and local XYZ position as f32 at offsets `+0x34/+0x38/+0x3C`. Bind-pose world positions use the BONESPACE formula.

### NMTR Chunks

Material definitions, 96 bytes per entry. `w[5]==1` at the first 8xu16 block indicates a diffuse texture; the NTX3 index is at `w[2]`. Alpha texture index is an i16 at `+0x30` (-1 = none).

### NLIT Chunks

Scene lighting data. Contains ambient colour (RGB bytes) and a directional light colour and direction. Parsed into `LightData` and forwarded to the viewport via `SetSceneLighting`. The viewer reconstructs the light direction from interactive pitch/yaw sliders rather than the raw direction field.

### CSF Chunks

Audio container holding ATRAC3 streams. Exportable to `.at3`. Loads from file or directly from a memory buffer inside any container format.

### P3OBJ Files

Object data container, parsed and listed in the file browser.

---

## Project Structure

```
eternal-sonata-studio/
├── include/            # Header files
│   ├── Application.h
│   ├── EFileParser.h
│   ├── NSHPParser.h
│   ├── NBN2Parser.h
│   ├── NMDLLoader.h
│   ├── AIScriptParser.h
│   └── ...
├── src/                # Source files
│   ├── main.cpp
│   ├── Application.cpp
│   └── ...
├── libs/               # Third-party dependencies
└── CMakeLists.txt      # Build configuration
```

---

## Known Limitations

- Animation data (NMTN) is parsed and listed but not yet visualised
- Skinned mesh vertex normals are not yet decoded correctly; geometric normals are used as a workaround in the viewer
- Xbox 360 specific compression not implemented
- Material export not available

---

## Technical Notes

### Texture Decompression

DXT1 and DXT5 textures are decompressed on-the-fly using the bcdec library. All format detection uses `(fmt & 0xDF)` to strip the Cell GCM mipmap flag (`0x20`) before identifying the base format. DXT5 blocks in PS3 Namco files have their colour and alpha sub-blocks stored in the reverse of the PC convention; the two 8-byte halves are swapped before calling `bcdec_bc3`. RGBA8 (format `0xA5`) is copied directly; the NTX3 header stores half the real pixel height and is corrected from the data size. Decompressed RGBA data is cached in GPU memory.

### Container Parsing

BMD and CAMP files use two distinct header layouts depending on the file. The parser auto-detects which layout is in use based on the value at offset `0x0C`. NOBJ sub-containers are parsed with the same byte-scan approach as `.e` files, finding all chunks at any nesting depth. BMD files are identified at runtime by their `BMD ` magic header to select the correct DXT5 decoding path for inline NTX3 previews.

### AI Script Parsing

The AI VM uses a node-based execution model where each node returns one of four statuses (SUCCESS / FAILURE / RUNNING / BLOCKED). Instructions are variable-width, decoded with a 5-byte extended prefix (`0x81`) or standalone opcodes. The 23-word fixed header block after the `Dz` sentinel is identical across all 120 AI files -- it is VM configuration, not per-file data.

### Mesh Rendering

NSHP meshes are parsed into vertex/index buffers and rendered using OpenGL 4.5 core profile. The tristrip decoder handles `0xFFFF` primitive restart tokens by ending the current strip and beginning a fresh one, resetting winding parity. Stride detection is automatic: the parser tries each candidate stride and validates that the vertex block ends where the face-section headers begin, with plausible strip counts and real vertex indices within range.

### NMDL Loading

The NMDLLoader scans all sub-chunks within the NMDL size boundary, uploads each NTX3 texture to the GPU via bcdec, and builds a `mat_id -> GLuint` map used at draw time. For map models, textures come from a separately loaded `.p3tex` archive; the material chain resolves identically. Materials with DXT5 diffuse textures are placed in `mat_blend_ids` and drawn in a second pass with `GL_BLEND` and depth-write suppressed.

### Skeleton Rendering

NBN2 bones are rendered in two passes: yellow `GL_LINES` from parent to child (depth-tested against the mesh) and white `GL_POINTS` at each joint position (depth-test disabled, always visible on top). Bone world positions are uploaded once at load time. PS3 uses Y-down coordinates; the Flip Y toggle negates the Y component so the skeleton appears upright in the OpenGL viewport.

### Scene Lighting

NLIT chunk data is forwarded to `Viewport3D::SetSceneLighting` when a map is loaded. The viewport stores ambient and directional colours as `glm::vec3` in [0,1] range. The directional light vector is rebuilt each frame from the pitch and yaw slider values, allowing interactive adjustment independent of what the game stored. Lighting is applied in the fragment shader as `ambient * colour + directional * colour * max(dot(N, L), 0)`. Unlit mode bypasses the lighting term entirely and outputs texture colour directly.

### Text Encoding

Game text uses ASCII encoding with custom markup tags (`<WAIT>` for pauses, `\n` for line breaks). Character names are hard-coded for detection.

### Audio Export

CSF containers hold ATRAC3 audio data. The exporter strips the CSF framing and writes a standard `.at3` file compatible with PS3 audio tools and foobar2000 with the appropriate plugin. The parser also loads directly from memory buffers, making in-container audio accessible without extraction.

---

## License

MIT License -- see LICENSE file for details.

This project is for educational and preservation purposes. All game assets remain property of their respective copyright holders (Bandai Namco, tri-Crescendo).

## Contributing

Pull requests are welcome. For major changes, please open an issue first to discuss proposed modifications.

## Contact

For questions or bug reports, please open an issue on GitHub.