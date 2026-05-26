# Eternal Sonata Studio

A reverse-engineering toolkit for viewing and extracting assets from Eternal Sonata (PlayStation 3 + Xbox 360). The toolkit covers PS3 demo, Xbox 360 demo, and Xbox 360 retail builds, including transparent decompression of the proprietary Xbox 360 retail container format.

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
- Parse `.ep3` UI animation containers (PS3 demo, Mefc-based frames)
- NOBJ sub-container parsing - finds all nested chunks (NTX3, NSHP, NMTN, NBN2, etc.) at any depth
- P3OBJ object data parsing
- Chunk-based file structure inspection (NSHP, NTX3, NOBJ, NMDL, NMTN, NBN2, NCAM, NLIT, NMTR, NFOG, NMTB, NDYN, NLC2, NCLS, CSF, FONT)
- Hex viewer for raw binary data analysis

### Xbox 360 Retail Support

Xbox 360 retail files use a proprietary tri-Crescendo (Tcx) compression format. Demo Xbox 360 builds and PS3 builds are uncompressed and pass straight through.

- **Automatic detection** -- the loader checks the first four bytes against the known magic table (BMD, BOP, NOBJ, NTX3, NTX2, Mefc, CSF, AI-script `0x00000181`, etc.). If the file does not match any known magic and has a plausible 256-byte frequency header, it is treated as a Tcx stream.
- **Transparent decompression** -- compressed files are decoded once and routed through the existing parsers via a temporary working file. The original game file is never modified. A `[Xbox 360 retail - auto-decompressed]` badge appears in the Chunks panel when a decompressed file is being viewed.
- **Supported extensions** -- `.e`, `.bmd`, `.bop`, and `.x3tex` (Xbox 360 texture archives). The decompressor is also invoked from the dedicated **Load P3TEX / X3TEX** button.

### Texture Viewing

- NTX3 texture format support (DXT1/DXT5/DXT1+mip/DXT5+mip/RGBA8 compression)
- Full PS3 DXT5 block-swap decoding (Namco stores colour and alpha sub-blocks in reverse order)
- NTX2 texture format support (Xbox 360 retail) - parses Namco's wrapper around XPR2 tiled textures, untiles to linear DXT1, then decompresses to RGBA8
- `.tex` file support
- P3TEX (PS3) and X3TEX (Xbox 360) texture archive browsing with grid and detail viewing modes
- Texture dimensions: 16x16 to 4096x4096
- PNG export with proper transparency (0xEE fill regions exported as alpha=0)
- Works across `.e`, `.bop`, `.bmd`, `.camp`, `.p3tex`, `.x3tex`, and `.ep3` files

### EP3 UI Animations

`.ep3` files are Mefc-wrapped containers holding sequences of NTX3 frames used for PS3 demo UI screens (controller diagrams, pause-trial atlases, splash screens). The parser walks the Mefc chunk-key table, identifies every embedded NTX3 frame, and builds a chunk list so each frame appears in the Chunks panel.

- Frame-by-frame inspection with width/height per frame
- Swizzled DXT5 detection (format byte bit `0x20`) and Morton-order block deswizzling
- Each frame exported individually via the standard texture viewer paths

### Audio

- CSF audio container parsing - loads from file or directly from memory buffers inside containers
- **CSF inside `evdata` containers** -- battle and cutscene event archives keep their voice clips as CSF sub-blocks. The parser correctly enumerates them, the Chunks panel groups them by source container, and the CSF Audio Export panel offers per-track or full-archive export with stable suffixes (`<base>_csf<i>_<clip>.at3`).
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

**Terrain UV** -- stride-24 map meshes use a two-field UV: `UV = i16/32767 + f16`. The f16 field alone collapses to zero after `GL_REPEAT` for integer offset values. The i16 component provides the fractional base for correct texture coverage. Sprite/prop meshes on the same stride use f16 only, detected per-vertex by `|f16| > 1.0`. Stride-28 meshes use a separate f16 UV pair at `+0x18/+0x1A`.

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

### Animation Viewport (work in progress, output is wrong)

`.e` character files contain NBN2 skeletons, NSHP skinned meshes, and NMTN animation tracks. The Animation Viewport ties them together: it locates the skinned NSHP inside the NMDL envelope, builds the inverse-bind matrices, and applies NMTN keyframe deltas on top of the NBN2 bind pose every frame.

UI: animation selector, play/pause, frame slider with frame count display, FPS slider, skeleton overlay toggle. The viewport is offscreen-rendered to an ImGui-embedded texture so it composes inside the docking layout.

**The animation result is currently incorrect.** Two known issues:

1. **Channel routing is a guess.** NMTN tracks store three channels (`c0/c1/c2`). For translation bones the parser maps `c0 -> dx (lateral)`, `c2 -> dy (vertical)` and skips `c1` (root motion forward). For rotation bones it maps `c0 -> ry`, `c1 -> rx`, `c2 -> rz`. These mappings were inferred from a few test files and are wrong for many bones — limbs end up rotating around the wrong axis.
2. **No bezier tangent handling.** NMTN Format-A tracks include tangent data after each keyframe; the parser drops the tangents and treats keys as linear-interpolated. Smooth ease-in/ease-out motions come out as straight ramps.

The viewport is shipped as-is for debugging the format. Don't use it for asset extraction yet.

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

---

## File Format Reference

### NTX3 Chunks (PS3)

Texture chunks. Pixel data starts at `chunk_offset + 0x88`. Format byte at `+0x18`:
- DXT1 (`0x86`): direct decode with `bcdec_bc1`
- DXT5 (`0x88` / variants): colour sub-block first (bytes 0-7), alpha sub-block second (bytes 8-15) -- halves are swapped before calling `bcdec_bc3`
- RGBA8 (`0xA5`): header height is `actual_height / 2`; real height = `data_size / (width x 4)`
- Mipmap flag is bit `0x20`; strip it with `(fmt & 0xDF)` before identifying the base format

### NTX2 Chunks (Xbox 360)

Namco's wrapper around an XPR2 (Xbox Packed Resource v2) tiled texture. Layout:

```
+0x00  "NTX2"          magic
+0x04  chunk_size      total size of this NTX2 entry (u32 BE)
+0x08  "XPR2"          sub-magic
+0x0C  xpr2_tail_size  size of the trailing pad region (u32 BE)
+0x10  hdr_sz          offset where pixel data ends / tail begins (u32 BE)
+0x14  1               num_textures, always 1 (u32 BE)
+0x18  "TX2D"          descriptor magic
+0x1C  tx2d_name_sz    size of the name-area block (0x20 or 0x30)
+0x2C  <name>          null-terminated ASCII filename
       <GPUTEXTURE_FETCH_CONSTANT>  6 x u32 BE  (Xenos texture descriptor)
       width  = (W2 & 0x1FFF) + 1
       height = ((W2 >> 13) & 0x1FFF) + 1
       format =  W1 & 0x3F          (18 = DXT1, 20 = DXT5)
```

Pixel data begins at the first 4 KB-aligned absolute address that is at least `0x1000` past the chunk start. Tiled layout uses the Xenia `XenosTextureTiledAddress2D` mapping, which interleaves a 32x32 macro-tile grid with a bank/pipe/oib hash; the parser implements this exactly so block `(bx, by)` is read from the correct offset.

DXT1 is fully supported. DXT5 and RGBA8 paths exist but were not exhaustively tested -- if a texture renders garbled, log the format value reported in the parser output.

### P3TEX / X3TEX Files

Archives containing multiple texture chunks. `.p3tex` (PS3) holds NTX3 chunks; `.x3tex` (Xbox 360, optionally Tcx-compressed) holds NTX2 chunks. The same parser handles both transparently and the same viewer renders the result.

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

### NMTN Chunks

Animation tracks. Each animation has a name, a frame count, and a list of per-bone tracks. Two storage formats (A and BC) coexist; both decode into the same `NMTNTrack { is_translation, is_static, rx/ry/rz channels }` runtime structure. The channel routing into Euler XYZ deltas is the part still being verified -- see the Animation Viewport caveats.

### NLIT Chunks

Scene lighting data. Contains ambient colour (RGB bytes) and a directional light colour and direction. Parsed into `LightData` and forwarded to the viewport via `SetSceneLighting`. The viewer reconstructs the light direction from interactive pitch/yaw sliders rather than the raw direction field.

### CSF Chunks

Audio container holding ATRAC3 streams. Header is `CSF ` + `BOOK` + a sequence of `TIM ` entries describing each clip's sample rate, offset, and length. Exportable to `.at3`. Loads from file or directly from a memory buffer inside any container format, including the `evdata` event-archive `.e` files used during battle dialogue.

### P3OBJ Files

Object data container, parsed and listed in the file browser.

### EP3 Files

Mefc-wrapped UI animation containers. The Mefc header carries a chunk-key block at `+0x28` describing each component (TEX0 / TRC0 / EFCT). Frames are stored as a sequence of NTX3 blocks, each with its own 128-byte header and a swizzled DXT5 payload. Frame count = number of NTX3 blocks found sequentially in the file.

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
│   ├── NTX2Parser.h
│   ├── TcxDecompressor.h
│   ├── AnimViewport.h
│   ├── Ep3Parser.h
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

- Animation playback is implemented but produces incorrect motion -- channel-to-axis routing and bezier tangent handling still need work
- Skinned mesh vertex normals are not yet decoded correctly; geometric normals are used as a workaround in the viewer
- NTX2 DXT5 and RGBA8 paths are present but not exhaustively tested
- Material export not available

---

## Technical Notes

### Xbox 360 Decompression

Xbox 360 retail builds wrap most `.e`, `.bmd`, `.bop`, and `.x3tex` files in a proprietary tri-Crescendo (Tcx) format. The algorithm was reverse-engineered from the Xbox 360 executable -- the relevant functions sit at virtual addresses `0x821189B8` (initialiser), `0x82118AF8` (per-symbol arithmetic decode) and `0x82118C70` (main LZSS decode loop). It is a classic two-layer entropy coder:

1. **Static arithmetic coding.** The first 256 bytes of the file are a frequency table -- one byte per symbol 0..255. The decoder builds cumulative and inverse-lookup tables, initialises `low = 0`, `range = 0xFFFFFFFF`, and reads the next four bytes into `code`. Each symbol is recovered as `inv[(code - low) / (range / total)]`, followed by a low/range update and a double normalisation pass (the main pass shifts when the high byte of `low` matches the high byte of `low + range`; an underflow pass kicks in when `range` drops below `0x2000`).
2. **LZSS layer.** Output bytes from the arithmetic stream feed a tiny state machine with four modes. Mode 0 captures a control byte (eight 1-bit literal/back-ref flags); mode 1 emits a literal and writes it into a 4 KB sliding dictionary (initial position `0xFEE`); modes 2 and 3 capture the low and high+length bytes of a 12-bit-offset / 4-bit-length-plus-three back-reference and copy from the dictionary.

The decompressor is invoked automatically when a file fails the known-magic check and looks like a Tcx stream (non-trivial frequency sum, at least eight distinct values in the first 256 bytes). Decompressed contents are written to a temp file and the rest of the toolchain operates on that path. For BMD/BOP/CAMP/SCP containers the decompressed size is trimmed to the value at file offset `+0x04` so that container parsers don't trip on a few trailing LZSS overflow bytes.

### NTX2 Texture Decoding (Xbox 360)

Xbox 360 textures live inside NTX2 chunks that wrap an XPR2 descriptor. The parser reads the Xenos `GPUTEXTURE_FETCH_CONSTANT` to recover width, height, format, and pitch -- pitch comes from the W0 pitch field rather than `width_blocks >> 5` so that narrow non-power-of-two textures (whose pitch is padded to 32 blocks) decode correctly.

Pixel data sits at the first 4 KB-aligned address at least `0x1000` past the chunk start. Block addressing follows Xenia's `XenosTextureTiledAddress2D`: each output block `(bx, by)` is read from a tiled offset computed by a bank/pipe/oib hash. The untiler reorders the 8-byte DXT1 blocks into a linear buffer, which is then handed to `bcdec_bc1` for RGBA8 decode. The result is stored in `P3Texture::data` with `format = P3TEX_FORMAT_RGBA8_DECODED (0xFF)` so the viewer skips re-decompression and uploads it straight to GL.

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

### Animation Pipeline

The AnimViewport ties NBN2, the skinned NSHP inside the NMDL envelope, and NMTN tracks together. Each frame it copies the bind pose, walks every track's `NMTNChannel::Sample(t)` (linear interpolation between keyframes), adds the resulting deltas into the Euler / position fields of the corresponding bone, then re-runs `NBN2Parser::ComputeWorldPositions` to refresh world matrices. Skinning matrices `world * inv_bind` are uploaded per draw as a `mat4` array uniform; the vertex shader does the standard four-weight blend.

The pose application is correct in *shape*; the part that's wrong is which NMTN channel feeds which bone axis. Format-A tracks also carry bezier tangents that are currently dropped, so smooth easing comes out as a piecewise-linear ramp. Both issues are isolated to `NMTNParser::ParseAnimation` and `AnimViewport::ApplyPose`.

### Scene Lighting

NLIT chunk data is forwarded to `Viewport3D::SetSceneLighting` when a map is loaded. The viewport stores ambient and directional colours as `glm::vec3` in [0,1] range. The directional light vector is rebuilt each frame from the pitch and yaw slider values, allowing interactive adjustment independent of what the game stored. Lighting is applied in the fragment shader as `ambient * colour + directional * colour * max(dot(N, L), 0)`. Unlit mode bypasses the lighting term entirely and outputs texture colour directly.

### Text Encoding

Game text uses ASCII encoding with custom markup tags (`<WAIT>` for pauses, `\n` for line breaks). Character names are hard-coded for detection.

### Audio Export

CSF containers hold ATRAC3 audio data. The exporter strips the CSF framing and writes a standard `.at3` file compatible with PS3 audio tools and foobar2000 with the appropriate plugin. The parser also loads directly from memory buffers, making in-container audio accessible without extraction -- the same path handles standalone `.csf`, CSF blocks inside `.bmd`/`.bop`/`.e` containers, and the per-clip CSF entries inside event archives (`evdata`).

### EP3 Parsing

EP3 files open with a `Mefc` header and a chunk-key block at offset `0x28` that lists the embedded components. The parser scans the file for NTX3 sub-blocks, reads each one's width/height/format from the standard NTX3 fields, and produces an `Ep3Frame` with the absolute pixel offset and size. Frames flagged with format bit `0x20` are swizzled in Morton (Z-order) block order; the deswizzler interleaves the bits of the block coordinates to recover the linear order. Each frame is surfaced as a synthetic NTX3 chunk in the file browser so the existing texture viewer can render and export it.

---

## License

MIT License -- see LICENSE file for details.

This project is for educational and preservation purposes. All game assets remain property of their respective copyright holders (Bandai Namco, tri-Crescendo).

## Contributing

Pull requests are welcome. For major changes, please open an issue first to discuss proposed modifications.

## Contact

For questions or bug reports, please open an issue on GitHub.