# Eternal Sonata Studio
A reverse-engineering toolkit for viewing and extracting assets from Eternal Sonata (PlayStation 3 + Xbox 360). The toolkit covers PS3 demo, Xbox 360 demo, and Xbox 360 retail builds, including transparent decompression of the proprietary Xbox 360 retail container format.
## Overview
Eternal Sonata Studio provides low-level access to game files, enabling inspection of textures, 3D models, skeletons, dialogue text, audio, AI behaviour scripts, cutscene scripts, bitmap fonts, and internal file structures. This tool was developed through reverse engineering of the game's proprietary formats. The goal is to develop it enough so pretty much anything in the game can be viewed and exported.
## Features
### File Parsing
- Parse `.e` container files (maps, cutscenes, models)
- Parse `.e` AI behaviour scripts (magic `0x00000181`) - separate format, see below
- Parse `.e` cutscene scripts (magic `0x00000181`) - scene orchestration on the same VM, see below
- Parse `.e` tutorial files (magic `0x00000181`) - CSF audio + subtitle containers, detected automatically
- Parse `.bop` container files
- Parse `.bmd` and `.camp` container files with full section table support
- Parse `.tex` texture atlas per language
- Parse `.fnt` bitmap fonts, and FONT blocks embedded in other containers
- Parse `.cxs` streaming BGM tracks (Xbox 360)
- Parse `.ep3` UI animation containers (PS3 demo, Mefc-based frames)
- NOBJ sub-container parsing - finds all nested chunks (NTX3, NSHP, NMTN, NBN2, etc.) at any depth
- P3OBJ object data parsing
- Chunk-based file structure inspection (NSHP, NTX3, NOBJ, NMDL, NMTN, NBN2, NCAM, NLIT, NMTR, NFOG, NMTB, NDYN, NLC2, NCLS, CSF, FONT)
- Hex viewer for raw binary data analysis
### Xbox 360 Retail Support
Xbox 360 retail files use a proprietary tri-Crescendo (Tcx) compression format. Demo Xbox 360 builds and PS3 builds are uncompressed and pass straight through.
- **Automatic detection** -- the loader checks the first four bytes against the known magic table (BMD, BOP, NOBJ, NTX3, NTX2, Mefc, CSF, CXS, AI-script `0x00000181`, etc.). If the file does not match any known magic and has a plausible 256-byte frequency header, it is treated as a Tcx stream. Registering new formats in that table matters: the frequency-header fallback is a heuristic, and an uncompressed file with a sparse header will otherwise be "decompressed" into garbage -- which is exactly what happened to every `.cxs` before `CXS ` was added.
- **Transparent decompression** -- compressed files are decoded once and routed through the existing parsers via a temporary working file. The original game file is never modified. A `[Xbox 360 retail - auto-decompressed]` badge appears in the Chunks panel when a decompressed file is being viewed.
- **Supported extensions** -- `.e`, `.bmd`, `.bop`, and `.x3tex` (Xbox 360 texture archives). The decompressor is also invoked from the dedicated **Load P3TEX / X3TEX** button.
### Texture Viewing
- NTX3 texture format support (DXT1/DXT5/DXT1+mip/DXT5+mip/RGBA8 compression)
- Full PS3 DXT5 block-swap decoding (Namco stores colour and alpha sub-blocks in reverse order)
- NTX2 texture format support (Xbox 360 retail) - parses Namco's wrapper around XPR2 tiled textures, untiles to linear DXT1, then decompresses to RGBA8
- NTEX texture format support (Xbox 360) - a stock little-endian PC DDS payload; DXT1 and DXT5 decode directly with no untiling or byte swapping
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
- **CSF inside `evdata` containers** -- battle and cutscene event archives keep their voice clips as CSF sub-blocks. The parser correctly enumerates them, the Chunks panel groups them by source container, and the CSF Audio Export panel offers per-track or full-archive export with stable suffixes (`<base>_csf<i>_<clip>.<ext>`).
- **Both platforms' codecs** -- PS3 containers hold ATRAC3 and export as `.at3`; Xbox 360 containers hold XMA2 and export as `.xma`. The codec is detected per container from the payload, and the export buttons relabel themselves to match.
- Export individual tracks or all tracks at once
- **CXS streaming BGM** (`sound/cxs`) -- the Xbox 360 music tracks. One track per file, stereo 48 kHz XMA2, with loop points preserved on export.
- **Big-endian WAV** (`sound/cxs/*.wav`) -- Xbox 360 PCM with byte-swapped samples, up to 250 MB each. Play directly, or convert to a standard little-endian `.wav`; both stream, so file size doesn't matter.
- **In-app preview** -- select a clip and hit Play, or double-click it in the list. Works for CXS tracks and big-endian WAVs too. CXS previews **loop between the track's loop points**, the way they do in game. All decoding and conversion runs on a worker thread, so the UI never blocks and Play doubles as Cancel.
- CSF and CXS preview requires `ffmpeg` on `PATH` (see Audio Playback below); export works without it, and WAVs need it for nothing.
- ATRAC3 plays in foobar2000 + ATRAC plugin; XMA2 decodes with `ffmpeg -i clip.xma clip.wav` or vgmstream
### 3D Model Viewing
- NSHP mesh format parser - complete rewrite with all five vertex strides confirmed
- Restart-aware tristrip decoding (`0xFFFF` primitive restart tokens handled correctly)
- Correct geometry rendering for map meshes (strides 16/20/24/28) and character meshes (stride 32)
- Additional map strides covered by the detector: static terrain at stride 32 (e.g. `zimen019`), skinned rope and cord meshes at stride 28, and sequential-draw billboards at stride 24
- 10:10:10:2 packed normal decoding for static map geometry
- Stride-48 particle meshes rendered as GL_POINTS (billboard particle systems)
- OpenGL 4.5 rendering with camera controls (orbit, pan, zoom) and auto-fit on load
- Vertex and triangle count statistics
### NMDL Model Viewer (textured)
- Select any NMDL chunk and click **Load Textured Model in Viewport** to load the full model
- All meshes, materials and textures are loaded in one step from the NMDL envelope
- Per-section draw calls: each face section binds the correct diffuse texture before drawing
- Texture chain: `FaceSection.mat_id -> NMTR[mat_id].diffuse_img_id -> texture_chunk[img_id]`, where `texture_chunk` is every NTX3/NTX2/NTEX in the NMDL in file order (see the NTEX section)
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
### Animation Viewport
`.e` character files and `.bmd` party archives contain NBN2 skeletons, NSHP skinned meshes, and NMTN animation tracks. The Animation Viewport ties them together: it locates the skinned NSHP inside the NMDL envelope, builds the inverse-bind matrices, and applies the NMTN keyframe channels on top of the NBN2 bind pose every frame.
UI: animation selector, play/pause, frame slider with frame count display, FPS slider, skeleton overlay toggle. The viewport is offscreen-rendered to an ImGui-embedded texture so it composes inside the docking layout.
The header frame count at `+0x20` over-reports the cycle for some clips (`pcPLK_cf_run` says 200 while every key ends at 40), so the cycle is taken from the keyframe extent when that is shorter. The NMTN format is now decoded from the Xbox 360 executable, so the motion plays correctly. Each track carries nine channel slots (position XYZ, rotation XYZ, scale XYZ); each slot is inactive, a single static value, or a keyframed channel. The static values and the keyframed channels are interleaved in slot order, not grouped. Keyframes are interpolated with cubic Hermite using their stored tangents, position channels override the bone local position, and rotation channels compose as `Euler(bind) * Euler(anim)` in the bone-local frame. Three rotation correctness passes run at parse time: Euler antipodal flip detection, per-axis wraparound unwrapping, and a tangent rebuild that removes Hermite overshoot. See the NMTN Chunks and Animation Pipeline sections for detail.
`.bmd` party archives such as `appkeep2.bmd` expose each NOBJ as a selectable model with its own skeleton and animation set. Files that ship NMTN tracks with no NBN2 (shared field-event banks for door and chest interactions) are parsed as a skeleton-less animation list, so the motions are surfaced even though there is no rig in the file to pose.
### Cloth Physics (Dynabone)
Dynamic bone chains (skirts, capes, ribbons, hair: NBN2 flag word `>= 0x100`) are simulated with a re-implementation of the game's own Dynabone solver, reverse-engineered from the Xbox 360 executable and checked against the original code run in an interpreter (positions agree to within 2 mm over 2 s). A chain starts at a bone with flag `0x2`, runs down the first dynamic child and ends at a bone with flag `0x8`. Its root is kinematic; the other bones are point masses on inextensible rods driven by gravity, a PD spring toward the animated pose (pre-sagged so the animated pose is the static equilibrium) and quadratic air drag, with rod tensions from a linear solve every substep. After the solve the nodes are kept inside their keep-in spheres (NCLS group 29, attached to the thighs, which is what makes the skirt follow the legs), linked skirt panels are pulled together by the sheet solver, and finally the nodes are pushed out of the keep-out spheres of their collision groups. Bone rotations are rebuilt from the solved positions, so the skin always describes a rigid FK chain and cannot tear. A **Cloth** toggle and a **Gravity** slider are in the viewport toolbar; the cloth runs at `1/60` of animation time, so the speed slider slows it down with the motion. See the NDYN / NCLS section for the data layout.
### AI Behaviour Scripts
Full analysis of the compiled AI scripts that drive enemy and boss behaviour in battle. These are `.e` files with magic `0x00000181` - a different format from the regular `.e` containers.
**Disassembler** - decodes the custom stack-based VM bytecode instruction by instruction, with named operands, register names, and float constant annotations.
**Two memory spaces** - the VM has a local frame (per-entity registers) and a shared world space (cross-entity coordination for formations and attack synchronisation). Both are decoded and colour-coded in the disassembly view.
**Action table** - the viewer decodes the action registration table embedded in every file, showing the 8 available combat actions and the C-engine handler each one maps to: Walk, Turn, Guard, Flank Back, Flank Left (A-F-L), Flank Right (A-F-R), Forward Attack, Special Attack.
**API catalog** - maps all 208 known `CALLAPI` addresses to named functions in two groups: special ability functions (one per boss/enemy) and per-enemy function vtables (`init`, `get_speed`, `get_range`, `seek_target`, `combat_state`, `special_seq`).
**File tiers** - simple files (114 nodes, no debug strings) share a common combat template with one unique special ability each; complex files (370-600+ nodes) have a fully unique behaviour tree per enemy.
Viewer tabs: **Overview - Disassembly - Actions - API Calls - Ctrl Flow - Strings** (complex only)
### Cutscene Scripts
Cutscene `.e` files share the AI-script magic `0x00000181` and the same stack-based VM, but they orchestrate a scene rather than an enemy. The parser disassembles the bytecode, walks the node graph from the `CALLNODE` targets, and rebuilds each scene command as a `CALLAPI` plus the arguments read off the symbolic stack. `CALLAPI` operands are zero on disk and are relocated at load from a footer table that pairs each operand site with a numeric API id; the footer is read from the end of the file backwards, and each pair's orientation is classified by ground truth (the side that points into the bytecode with the `0x7d` opcode before it is the site).
Three event kinds are detected by content:
- **Cutscene3D** -- an in-engine real-time scene with a `CSL ` voice table, models, animations, and cameras (for example `e1180`). These are parsed and classified but **not** rendered correctly yet (see Known Limitations).
- **TitleCard** -- a tiny bytecode with text sprites and a fixed timer (for example `e1000`).
- **Narrative** -- a full-screen image slideshow with a faded subtitle track and a synced music track (the chapter-end "Revolution" and "Grande Valse Brillante" events). This event type is decoded end to end.
**Narrative slideshow extraction** -- for a Narrative event the parser produces the slide images (every NTX3 chunk, story photos and localized title strips), the subtitle timeline parsed per language (USA / GRB / FRA / ITA / DEU / ESP), the per-step master-clock timing from the `0x012C` timer (seconds = ticks / 300), the image-switch timeline that binds each photo to its display slot, and the music cue table. Each caption is stamped with the real bytecode time of its id-219 site so the captions line up with the photos. The reconstructed timeline matches the in-game rundown of both known events.
### Bitmap Fonts (.fnt)
The `.fnt` bitmap font format (the in-game message and gothic fonts) is decoded, and a dedicated viewer composes text with the font's own glyphs.
- A FONT block loads from a standalone `.fnt` file or is found inside any other container (the loader scans for an embedded `FONT` block when the file does not start with the magic), so a font carried inside a BMD opens the same way. Reads are bounded to the block size declared in the header.
- Only the BC sub-font (Latin letters, digits, symbols) is decoded; the optional MBC Japanese section is skipped.
- Each glyph is a 2-bit per pixel coverage bitmap expanded to 8-bit alpha. The glyph slot is the character code minus one (slot `0x2F` is `'0'`, `0x40` is `'A'`, `0x60` is `'a'`).
- The viewer has a text box and an on-screen QWERTY keyboard (uppercase toggle, space, punctuation, backspace) that compose a line rendered live from the bitmap glyphs, on a baseline with a zoom control.
- The composed line exports to a PNG at native glyph resolution, black text on a transparent or white background.
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
+0x0C  xpr2_hdr_size   size of the XPR2 header block, from the XPR2 magic (u32 BE)
+0x10  data_size       exact size of the pixel-data block (u32 BE)
+0x14  1               num_textures, always 1 (u32 BE)
+0x18  "TX2D"          descriptor magic
+0x1C  tx2d_name_sz    size of the name-area block (0x20 or 0x30)
+0x2C  <name>          null-terminated ASCII filename
       <GPUTEXTURE_FETCH_CONSTANT>  6 x u32 BE  (Xenos texture descriptor)
       width  = (W2 & 0x1FFF) + 1
       height = ((W2 >> 13) & 0x1FFF) + 1
       format =  W1 & 0x3F          (18 = DXT1, 20 = DXT5)
```
Pixel data begins at the first 4 KB-aligned **absolute** address at or after the end of the XPR2 header (the XPR2 magic sits at chunk `+0x08`), and runs for exactly `data_size` bytes:

```
pix_abs    = align4K(chunk_abs_offset + 0x08 + xpr2_hdr_size)
pix_offset = pix_abs - chunk_abs_offset
pix_size   = data_size
```

The layout always satisfies `pix_offset + data_size == chunk_size`, which the parser asserts as a cross-check. Because the alignment is absolute, the chunk's own position in the file is part of the calculation.

Aligning up from `chunk_start + 0x1000` instead of from the header end is wrong, and wrong in a way that is easy to miss: the two agree whenever the XPR2 header runs past the chunk's first 4 KB page, which is the common case. When the header ends *inside* that first page the shortcut skips a whole page and mip 0 is read `0x1000` bytes too far in. The result does not look like noise -- the untiler maps each output block through a bijection, so a shifted base permutes the 32x32 macro-tiles and the texture renders as a checkerboard of coherent rectangles. Verified over 882 NTX2 chunks spanning maps, party models, scenes and UI archives: the rule above lands on a 4 KB boundary and satisfies the size invariant for every one of them, and the 33 chunks where it disagrees with the old shortcut are exactly those whose header ends inside the first page (`eye.tga` in `cfdata/e0030.e`, `enkei.tga` in `btldata/map/adg90.bop`, and 31 others).

Tiled layout uses the Xenia `XenosTextureTiledAddress2D` mapping, which interleaves a 32x32 macro-tile grid with a bank/pipe/oib hash; the parser implements this exactly so block `(bx, by)` is read from the correct offset.
DXT1 is fully supported. DXT5 and RGBA8 paths exist but were not exhaustively tested -- if a texture renders garbled, log the format value reported in the parser output.
### NTEX Chunks (Xbox 360)
The third texture chunk kind, used by the Xbox 360 build alongside NTX2. Layout is trivial: `"NTEX"` + `chunk_size` (u32 BE), followed by a **stock little-endian PC DDS file** — not tiled, not byte-swapped, mip level 0 first. DXT1 and DXT5 are decoded directly with bcdec. One further variant appears (DDS pixel-format flags `0x80000`, 16 bpp, no FourCC) used for light/shadow maps; it is not decoded and no material ever references it.

**NTEX shares the NMDL texture index space with NTX3 and NTX2.** A material's `diffuse_img_id` is a positional index over *every* texture chunk in the model, in file order, whichever kind. Skipping NTEX chunks — or dropping an NTX2 whose decode failed — shifts every later texture onto the wrong material, which is what made Xbox 360 maps render with textures on the wrong meshes. Any traversal that builds the material table must claim a slot for a texture chunk it cannot decode. Verified against the retail build: `xlp90.bop`'s 17 materials reference ids `{0-5, 7-10}` over 6 NTX2 + 1 NTEX + 4 NTX2, and the one id nothing references is exactly the NTEX slot; the same alignment holds across every map and character model in the game, with no id ever running past the end of the slot list.

Some sub-models are textured *entirely* by NTEX (whole map props and set pieces whose NMDL contains nothing but NTEX chunks), so decoding the format is what makes them show up textured at all.
### P3TEX / X3TEX Files
Archives containing multiple texture chunks. `.p3tex` (PS3) holds NTX3 chunks; `.x3tex` (Xbox 360, optionally Tcx-compressed) holds NTX2 chunks. The same parser handles both transparently and the same viewer renders the result.
### NSHP Chunks
3D mesh geometry. Five vertex formats exist depending on mesh type:
- Stride 16: position only (water, collision geometry)
- Stride 20: position + 10:10:10:2 packed normal + UV (i16/32767)
- Stride 24: position + normal + two-field UV (`i16/32767 + f16` for tiling terrain; `f16` only for sprites, detected per-vertex by `|f16| > 1.0`)
- Stride 28: position + normal + unknown fields + UV as f16 pair at `+0x18/+0x1A`. The i16 pair at `+0x10/+0x12` and f16 pair at `+0x14/+0x16` are not UV (blend weights / lightmap / sentinel data).
- Stride 32: skinned characters (PS3) - position + three bone weights as one RSX CMP dword + bone indices + CMP normal + UV (f16). See below.
- Stride 40: skinned characters, Xbox 360 party models (`AppKeep.bmd`) - position + three f32 weights + four reversed index bytes + normal + vertex colour + UV (f16). See below.
- Stride 48: particle / billboard positions, no index buffer, 12-byte footer
All indexed formats use restartable tristrips with `0xFFFF` as the primitive restart token.
**Stride 28/32 (PS3 skinned)** -- `+0x0C` is one CMP dword (signed normalized 11:11:10, x in the low 11 bits, z in the high 10) holding three weights for index bytes 3, 2 and 1; index byte 0 takes `1 - (x + y + z)`. The four bone indices are the bytes at `+0x10`, and `+0x14` is the normal in the same CMP packing. Verified on `pcplk` / `pcalg`: every vertex sums to exactly 1 and every normal is unit length. This is the same weight-to-index order as the Xbox 360 stride-40 layout below.

**Stride 40 (Xbox 360 skinned)** -- the party models in `AppKeep.bmd` (`pcALG_v1` and friends) use a skinned layout that exists on neither PS3 nor any Xbox 360 map file, so the two-candidate skinned detector (32 then 28) rejected every mesh in the model and the NMDL load failed with all textures already decoded. Layout:

| Offset | Field |
| --- | --- |
| `+0x00` | position, 3 x f32 BE |
| `+0x0C`, `+0x10`, `+0x14` | bone weights 0..2, f32 BE |
| `+0x18`..`+0x1B` | bone indices, **reverse order**: `+0x1B`/`+0x1A`/`+0x19`/`+0x18` are the indices for weights 0/1/2/3 |
| `+0x1C` | packed normal (encoding not identified) |
| `+0x20` | RGBA vertex colour (alpha `0xFF`, RGB grey) |
| `+0x24` | UV, f16 pair (`stride - 4`, same convention as strides 28/32) |

Two things make this format easy to get wrong. Only **three** weights are stored -- the fourth is derived as `1 - (w0+w1+w2)`, so a parser that reads four consecutive floats walks into the index bytes. And the indices are byte-reversed relative to the weights, so reading them forwards silently rigs every vertex to the wrong joint instead of failing outright. Both were confirmed against all seven meshes of `pcALG_v1`: the maximum index found is exactly `bone_count - 1` under the reversed reading, and the stored weights sum to exactly 1.0 wherever the fourth slot is unused. Bone index 0 is legal, so the fourth slot is always read rather than treated as padding -- `face` has 56 vertices with a real fourth influence and several of `tasuki_sp`'s land on bone 0.

Stride is not stored; the parser detects it by validating each candidate against the file layout. Beyond the canonical assignments above, some maps reuse the same byte layouts with a different stride: static terrain meshes can use stride 32 (UV is the f16 pair at `+0x14`), skinned rope and cord meshes can use stride 28 (UV is the f16 pair in the last four bytes), and sequential-draw billboards can use stride 24 as well as 48. The detector tries all of these.
### NMDL Chunks
Top-level model container. Contains all NSHP meshes, the texture chunks (NTX3 on PS3; NTX2 and NTEX on Xbox 360) and the NMTR material table for one model. Sub-chunks are found by byte-scanning within the NMDL size boundary.

PS3 map models carry no embedded textures — they come from a companion `.p3tex` archive loaded separately. Xbox 360 maps are the opposite: they embed all their NTX2/NTEX textures, and the retail build ships no `.x3tex` archives at all, so no archive should be mounted for them (an external texture list overrides the correctly-indexed embedded one). The Model Info tab reports the embedded texture count, and flags that an archive is wanted only for models carrying no texture chunks of their own. Mounting an archive is always an explicit action — **Load Textured Model in Viewport** never opens a file dialog of its own; it loads with whatever is already mounted.
### NBN2 Chunks
Skeleton bone table. Each entry is 64 bytes: name (16 bytes), flags (u16), parent/child/sibling indices (i16 each), Euler XYZ rotation as `i16 / 4096` radians, local scale as `i16 / 256` at `+0x2A`, the NDYN record index (u16) at `+0x30`, and local XYZ position as f32 at offsets `+0x34/+0x38/+0x3C`. Bind-pose world positions use the BONESPACE formula.
### NMTR Chunks
Material definitions, 96 bytes per entry, read as the game's material setup does (Xbox 360 `sub_82115F80`). `+0x00` u16 flags (`0x0001` specular, `0x0008` environment map), `+0x02` layer count, `+0x04` layer 0: a colour, or a u16 texture index when bit 0 of `+0x0B` is set (colours are RGBA on PS3, ARGB on Xbox 360). Alpha texture index is an i16 at `+0x30` (-1 = none); without one, `+0x34` i16 gives the opacity as `1 - v / 16384`. With flag `0x0008`, `+0x42` is the environment-map texture and `+0x3C` i16 `/ 16384` its strength. Water (`mizu_001` in `adg01.e`) is such a material: a teal colour, opacity 0.51, the map's forest backdrop as environment map at 0.65, plus a per-vertex alpha in the stride-28 vertex colour (`+0x14`, alpha `+0x17`) that fades it to 0 at the shore. The viewer draws it blended, with a sphere-mapped reflection standing in for the game's environment shader.
### NMTN Chunks
Animation tracks. Each animation has a name, a frame count, and a list of per-bone tracks. One track is one bone: a 16-byte name, the track size at `+0x10`, and a `u32` flag word at `+0x14` holding nine 3-bit fields (one per channel slot, read as `(flags >> ((8 - slot) * 3)) & 7`). Field 0 is inactive, field 1 is a single static value, field 2 is a keyframed channel. Slots 0 to 2 are position XYZ, slots 3 to 5 are rotation XYZ, slots 6 to 8 are scale XYZ. The slot data begins at `+0x18` and is interleaved in slot order: walking slots 0 to 8, a static slot contributes one `f32` and a keyframed slot contributes a full channel block, in whatever order the slots fall. Statics and channels are not grouped.
Each channel is a `u16` keyframe count, an exponent byte `fmt` in the range `0x0C` to `0x0F`, then `count` eight-byte keyframes: frame (`u16`), value (`i16`), incoming tangent (`i16`), outgoing tangent (`i16`). Value and tangents are scaled by `1.0 / (1 << fmt)`, giving radians for rotation and local units for position. Keyframes are interpolated with cubic Hermite from the stored tangents. The layout was reverse-engineered from the Xbox 360 executable (channel init `0x8211cc18`, value sampler `0x8211ccc8`, channel binding `0x8211b720`).
Rotation channels get three correctness passes at parse time: antipodal Euler flip detection (`E(a,b,c)` equals `E(a-pi, pi-b, c-pi)`), per-axis unwrap when a channel crosses plus or minus pi, and a tangent rebuild from the corrected values to stop cubic Hermite overshoot.
### NDYN / NCLS Chunks
Cloth (Dynabone) data for a character model, decoded from the Xbox 360 executable (chain builder `0x82118628`, solver `0x8215e280`-`0x82160540`, collisions `0x82137510` / `0x82136b08`, response `0x82137cf0`, sheet solver `0x8213f448`).

`NDYN` holds one 48-byte record per dynamic bone, at `chunk + 8 + ndyn_index * 48` (`ndyn_index` is the u16 at NBN2 `+0x30`):

| Offset | Field |
| --- | --- |
| `+0x00` | u16 type (bit 0: 10 substeps at 1/60, else 8) |
| `+0x02` | u16 bone index (cross-check) |
| `+0x04` / `+0x06` | u16 collision masks, pass 2 / pass 1: bit `b` selects NCLS group `b + 1` (keep-out) |
| `+0x08` | f32 mass |
| `+0x0C` | f32 air drag (chain value, read from the root record) |
| `+0x10` / `+0x14` | f32 spring stiffness / damping of the segment parent to child (read from the parent's record) |
| `+0x18` | u32 `(count << 24) \| first`: lateral links `{u32 bone, f32 weight, f32 angle}` (32 bytes each) stored after the records |
| `+0x20` | u8 keep-in id: the NCLS group 29 records with that id |
| `+0x28` | u16 sheet threshold, `/ 64` |

`NCLS` starts with a `0x84`-byte header (u16 reserved, u16[64] cumulative per-group counts, u16 record count), then 48-byte records: u8 type (`0` sphere, `1` oriented box, `2` capsule), u8 id, u16 bone, three i16 offsets `/ 64` in the bone frame, three i16 Euler angles `/ 4096`, f32 radius (or extents at `+0x10`/`+0x14`/`+0x18`). Keep-out pushes a node to `radius + 0.001`, keep-in pulls it to `radius - 0.002`, both by rotating its segment about the parent node (law of cosines) with the nodes below following.

The sheet solver handles chains linked side by side (Polka's skirt panels). When the chain that strays furthest from the animation exceeds the threshold, its neighbours are placed at `weight * rest distance` from it, outward along the group, and it and its two neighbours are rotated outward by the link angle.
### NLIT Chunks
Scene lighting data. Contains ambient colour (RGB bytes) and a directional light colour and direction. Parsed into `LightData` and forwarded to the viewport via `SetSceneLighting`. The viewer reconstructs the light direction from interactive pitch/yaw sliders rather than the raw direction field.
### CSF Chunks
Audio container. Header is `CSF ` + `BOOK` + a sequence of `TIM ` entries describing each clip's sample rate, offset, and length. Loads from file or directly from a memory buffer inside any container format, including the `evdata` event-archive `.e` files used during battle dialogue.

The framing is byte-identical on both platforms -- same big-endian fields, and the `TIM ` format word is `0` in both builds -- but the payload is not: the PS3 build stores ATRAC3 and the Xbox 360 build stores XMA2. Since no header field distinguishes them, the codec is inferred from the payload: XMA2 is a stream of fixed 2048-byte packets and the encoder pads every clip out to a whole packet, so a container whose clips are all packet-aligned *and* whose packet headers pass a sanity check (top 6 bits a frame count, bottom 8 a skip count) is XMA2; anything else is treated as ATRAC3. Verified against all 68 CSF chunks in the Xbox 360 `campdata/scp.bmd`: 3174/3174 packets pass, every clip is packet-aligned, and only 25 of 68 happen to be 192-byte ATRAC3-aligned -- which is why frame alignment alone is not enough to tell them apart.
### Cutscene Scripts (.e, magic 0x00000181)
Scene orchestration scripts on the same stack VM as the AI behaviour scripts. Header: code size at `+0x10`, footer offset at `+0x14`. The bytecode runs from `+0x18`; opcode `0x7d` is `CALLAPI` (operand relocated from the footer), `0x7a` is `CALLNODE`, `0x07` is `PUSHOFF`, `0x7E` is `WAIT`, `0x03` is `PUSH.F`. The footer is an id/site relocation table read from the end of the file backwards as 8-byte records; pair orientation varies per file, so each side is classified by ground truth (the side that points into the bytecode with `0x7d` before it is the site). Known API ids include 219 (subtitle), 255 (fade), 1025 (set-param, including the `0x012C` timer), 1028 (image-switch), 1002 (load map). A `FindCallApiSites` helper resolves every site for a given API id by testing both pair orientations.
For Narrative slideshow events the parser also reads: every NTX3 chunk as a slide image (DXT5; `0xA8` full-screen story photos, `0x88` localized title strips); a per-language subtitle block (`<dNN>` line type, `<vNN>` voice index, `<f1>`/`<f0>` fade, `<wNNNN>` on-screen ms); the master-clock timer steps from slot `0x012C` (seconds = ticks / 300, advanced at each `WAIT`); the image-switch timeline (id-1028, the visible photo is the lower slot of each activate/preload pair, read from the `PUSH.W 0x0C0X` before the site); and the music cue table (`MPxxx.wav` plus offsets).
### FONT Chunks (.fnt)
Bitmap font. A FONT block can stand alone in a `.fnt` file or be embedded in another container. Header: magic `FONT`, block size at `+0x04`, version at `+0x08`, cell size at `+0x0C` (two u16 halves), anti-alias levels at `+0x10`, the BC sub-font tag at `+0x14`, and the BC glyph offset table at `+0x20` (indexed by character code; the first non-zero entry marks the start of glyph data and gives the table length). The glyph slot is the character code minus one (slot `0x2F` is `'0'`). Each glyph record carries 4 metric bytes at `+0x14` (advance width, advance height, visible width, height in rows) inside the bitmap stream; the pixel data runs from `+0x18` of the glyph up to `+0x14` of the next non-empty glyph, 2 bits per pixel (4 anti-alias levels), packed MSB-first. The optional MBC (Japanese) sub-font following the BC data is not decoded by the viewer.
### P3OBJ Files
Object data container, parsed and listed in the file browser.
### EP3 Files
Mefc-wrapped UI animation containers. The Mefc header carries a chunk-key block at `+0x28` describing each component (TEX0 / TRC0 / EFCT). Frames are stored as a sequence of NTX3 blocks, each with its own 128-byte header and a swizzled DXT5 payload. Frame count = number of NTX3 blocks found sequentially in the file.

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
│   ├── Cutscenescriptparser.h
│   ├── Fntparser.h
│   ├── Fntviewer.h
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
- Full 3D cutscenes (the in-engine `e1180`-class scenes with a map, posed actors, and camera moves) are parsed and classified but not rendered correctly yet; only the narrative slideshow event type works end to end
- Dynabone cloth: NCLS oriented-box and capsule colliders are parsed but not simulated, and the animations play in place, so the drag from the character's forward motion is missing
- Xbox 360 stride-40 skinned normals (`+0x1C`) are not decoded yet; geometric normals are used there
- One sequential mesh variant is still unparsed: a multi-section sequential draw at stride 24 (the dandelion fluff `watage_ten`)
- The font viewer decodes the BC (Latin) sub-font only; the MBC Japanese glyphs are not surfaced
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
Pixel data sits at the first 4 KB-aligned absolute address at or after the end of the XPR2 header, and is exactly `data_size` bytes long (see the NTX2 Chunks section -- deriving it from the chunk start instead scrambled 33 textures into a macro-tile checkerboard). Block addressing follows Xenia's `XenosTextureTiledAddress2D`: each output block `(bx, by)` is read from a tiled offset computed by a bank/pipe/oib hash. The untiler reorders the 8-byte DXT1 blocks into a linear buffer, which is then handed to `bcdec_bc1` for RGBA8 decode. The result is stored in `P3Texture::data` with `format = P3TEX_FORMAT_RGBA8_DECODED (0xFF)` so the viewer skips re-decompression and uploads it straight to GL.
### Texture Decompression
DXT1 and DXT5 textures are decompressed on-the-fly using the bcdec library. All format detection uses `(fmt & 0xDF)` to strip the Cell GCM mipmap flag (`0x20`) before identifying the base format. DXT5 blocks in PS3 Namco files have their colour and alpha sub-blocks stored in the reverse of the PC convention; the two 8-byte halves are swapped before calling `bcdec_bc3`. RGBA8 (format `0xA5`) is copied directly; the NTX3 header stores half the real pixel height and is corrected from the data size. Decompressed RGBA data is cached in GPU memory.
### Container Parsing
BMD and CAMP files use two distinct header layouts depending on the file. The parser auto-detects which layout is in use based on the value at offset `0x0C`. NOBJ sub-containers are parsed with the same byte-scan approach as `.e` files, finding all chunks at any nesting depth. BMD files are identified at runtime by their `BMD ` magic header to select the correct DXT5 decoding path for inline NTX3 previews.
### AI Script Parsing
The AI VM uses a node-based execution model where each node returns one of four statuses (SUCCESS / FAILURE / RUNNING / BLOCKED). Instructions are variable-width, decoded with a 5-byte extended prefix (`0x81`) or standalone opcodes. The 23-word fixed header block after the `Dz` sentinel is identical across all 120 AI files -- it is VM configuration, not per-file data. The opcode mnemonics outside the confirmed set are best-effort; any finding derived from the executable takes priority over a guessed mnemonic.
### Cutscene Script Parsing
Cutscene scripts run on the same VM as the AI scripts but drive a scene. The parser disassembles the bytecode, builds nodes from `CALLNODE` targets, and reconstructs each `CALLAPI` with its stack arguments. The footer id/site relocation table is read from the end of the file as 8-byte records, with each pair's orientation classified by ground truth (the side that lands inside the bytecode with `0x7d` before it is the site). A single `FindCallApiSites` helper resolves all sites for any API id by testing both orientations, which is what makes the narrative slideshow timeline work across files whose footer pairs are laid out in opposite order. Event kind (3D / title-card / narrative) is classified by content; the slideshow passes (slide images, per-language subtitles, timer steps, image-switch timeline, music cues) run only for narrative events and leave 3D and title-card files untouched.
The narrative image timeline is reconstructed from the id-1028 image-switches rather than a preload heuristic: each switch activates a display slot (read from the `PUSH.W 0x0C0X` immediately before the `0x7d` site), a transition fires an activate/preload pair at the same clock, and the visible photo is the lower slot of the pair. Switches that fire during the opening setup (before the first caption) are setup, not transitions, and are skipped. Each caption is timestamped from the bytecode clock at its id-219 site so captions and photos stay in sync.
### NMTN Track Layout
The per-bone track stores nine 3-bit slot fields in a flag word at `+0x14`; field 0 is inactive, 1 is a single static value, 2 is a keyframed channel. The slot data at `+0x18` is interleaved in slot order: walking slots 0 to 8, a static slot reads one `f32` and a keyframed slot reads a channel block, at the running offset. The static values and channels are not grouped into separate runs. A track that mixes the two kinds, a static slot after a channel slot, would have every entry after the first such transition read from the wrong offset if the slots were assumed grouped, corrupting that bone's motion; the single-pass slot walk matches the engine's own per-slot binding (`fn 0x8211b720`).
### Mesh Rendering
NSHP meshes are parsed into vertex/index buffers and rendered using OpenGL 4.5 core profile. The tristrip decoder handles `0xFFFF` primitive restart tokens by ending the current strip and beginning a fresh one, resetting winding parity. Stride detection is automatic: the parser tries each candidate stride and validates that the vertex block ends where the face-section headers begin, with plausible strip counts and real vertex indices within range.
### NMDL Loading
The NMDLLoader scans all sub-chunks within the NMDL size boundary, uploads each texture chunk (NTX3 / NTX2 / NTEX) to the GPU via bcdec, and builds a `mat_id -> GLuint` map used at draw time. All three chunk kinds share one positional index space, and a chunk whose format the decoder doesn't handle still claims its slot so later materials stay aligned. For PS3 map models the textures come from a separately loaded `.p3tex` archive instead; the material chain resolves identically. Materials with DXT5 diffuse textures are placed in `mat_blend_ids` and drawn in a second pass with `GL_BLEND` and depth-write suppressed.
### Skeleton Rendering
NBN2 bones are rendered in two passes: yellow `GL_LINES` from parent to child (depth-tested against the mesh) and white `GL_POINTS` at each joint position (depth-test disabled, always visible on top). Bone world positions are uploaded once at load time. PS3 uses Y-down coordinates; the Flip Y toggle negates the Y component so the skeleton appears upright in the OpenGL viewport.
### Animation Pipeline
The AnimViewport ties NBN2, the skinned NSHP inside the NMDL envelope, and NMTN tracks together. Each frame it copies the bind pose, samples every track channel with `NMTNChannel::Sample(t)` (cubic Hermite from the stored tangents), writes position channels over the bone local position and composes rotation channels as `Euler(bind) * Euler(anim)`, then re-runs `NBN2Parser::ComputeWorldPositions` to refresh world matrices. Skinning matrices `world * inv_bind` are uploaded per draw as a `mat4` array uniform; the vertex shader does the standard four-weight blend.
Channel routing is no longer a guess: the nine 3-bit slot fields name each channel directly (position XYZ, rotation XYZ, scale XYZ), and the static values and keyframed channels are read interleaved in slot order in a single pass. The tangents are read and used, so smooth easing is reproduced rather than flattened to a ramp. Rotation channels are corrected for antipodal Euler flips and for wraparound past plus or minus pi, and their tangents are rebuilt from the corrected values, which removes the per-frame tweak and tremble artifacts. PS3 skinned normals are decoded from the CMP field; Xbox 360 stride-40 meshes still use geometric normals.
`.bmd` party archives feed the viewport through the same path as `.e` files, and each NOBJ becomes its own model. Archives that contain NMTN tracks but no NBN2 are parsed as a skeleton-less animation list so the motions are not dropped.
### Cloth Physics
Frame order follows the engine's update (`0x8213e420`): every chain is solved and kept inside its keep-in spheres (parent chains first, so a child chain starts from its solved parent), then the sheet solver runs, then the keep-out spheres. Per chain: the root is driven by velocity toward its new position; targets are the chain's bind shape carried by the root's current frame (the animation of the chain bones themselves is ignored, as in the game); the spring rest points are pre-sagged so the target pose is the static equilibrium under gravity; each substep solves the tridiagonal rod-tension system (the game feeds the spring and external forces into it with a flipped sign), integrates the segment velocities semi-implicitly and projects the rod lengths. Not yet simulated: NCLS type 1 boxes and type 2 capsules, and the sheet solver's fallback when one segment cannot reach (approximated).
### Scene Lighting
NLIT chunk data is forwarded to `Viewport3D::SetSceneLighting` when a map is loaded. The viewport stores ambient and directional colours as `glm::vec3` in [0,1] range. The directional light vector is rebuilt each frame from the pitch and yaw slider values, allowing interactive adjustment independent of what the game stored. Lighting is applied in the fragment shader as `ambient * colour + directional * colour * max(dot(N, L), 0)`. Unlit mode bypasses the lighting term entirely and outputs texture colour directly.
### Cutscene and Bitmap Font Parsing
Cutscene scripts are disassembled on the AI-script VM and classified into 3D, title-card, and narrative kinds; the narrative slideshow timeline (images, per-language captions, timer-paced clock, image-switch transitions, music cues) is reconstructed from the bytecode and footer relocation table, with all API-id site lookups going through a both-orientation helper so the timeline is correct regardless of the file's footer pair layout. The `.fnt` parser reads a FONT block (standalone or embedded), decodes the BC sub-font's 2-bit-per-pixel glyphs to 8-bit alpha, and the font viewer composes typed text from those glyphs and exports it to PNG.
### Text Encoding
Game text uses ASCII encoding with custom markup tags (`<WAIT>` for pauses, `\n` for line breaks). Character names are hard-coded for detection.
### Audio Export
The exporter strips the CSF framing and rewraps the clip's payload in the container its codec expects, without transcoding. PS3 ATRAC3 clips become a standard `.at3` (RIFF, format tag `0x0270`, 192-byte frames) compatible with PS3 audio tools and foobar2000 with the appropriate plugin. Xbox 360 XMA2 clips become a `.xma` (RIFF, format tag `0x0166`, `XMA2WAVEFORMATEX`) that ffmpeg and vgmstream decode directly. Because the `TIM ` descriptor carries no decoded-sample count, the XMA2 header's `SamplesEncoded`/`PlayLength` are over-estimated from the packet count; decoders treat those as a seeking upper bound and stop at the true end of the stream. The parser also loads directly from memory buffers, making in-container audio accessible without extraction -- the same path handles standalone `.csf`, CSF blocks inside `.bmd`/`.bop`/`.e` containers, and the per-clip CSF entries inside event archives (`evdata`).

### CXS Files (Xbox 360 streaming BGM)
`assets/sound/cxs` holds the music as `.cxs`: a flat `0x1000`-byte big-endian header followed by a single raw XMA2 packet stream. There is no clip table -- one file is one track.

| Offset | Field |
| --- | --- |
| `0x00` | `"CXS "` |
| `0x04` | packet size, always 2048 |
| `0x08` | sample rate |
| `0x0C` | channel count |
| `0x10` | total decoded samples per channel (**exact**) |
| `0x14` | loop start in samples (0 = no loop) |
| `0x18` | loop end in samples |
| `0x1C` | block count, `ceil(data_size / 0x10000)` |
| `0x20` | data offset, always `0x1000` |
| `0x24` | data size |
| `0x28`, `0x2C` | both 2048 -- encoder packet/alignment granularity |

Verified across all 62 retail `.cxs`: every one is 48 kHz stereo, `data_offset + data_size` equals the file size, `block_count` matches the formula, every packet header passes the XMA2 sanity check, and 47 carry loop points. All 62 rewrap and decode through ffmpeg with no errors, and decoded length matches the declared sample count to within a single frame.

Because CXS states the sample count exactly -- unlike CSF, which has to estimate it -- `XMAWriter` takes an optional `XMAStreamInfo` carrying the real count plus loop points, which become `LoopBegin`/`LoopLength` with `LoopCount = 0xFF` (loop forever). `nAvgBytesPerSec` is also derived from the true duration rather than left at a nominal PCM value; tools that estimate length from it instead of decoding otherwise report a track several times shorter than it is.

The 14 `.wav` files sitting alongside the `.cxs` are a different thing -- big-endian PCM, handled by `scripts/convert_wav.py` in the Reprise repo, not by this studio.

### Big-Endian WAV Files (Xbox 360)
Sitting alongside the `.cxs` tracks in `assets/sound/cxs` are 14 `.wav` files that are ordinary RIFF/WAVE except that every header field *and* every 16-bit sample is big-endian. Converting one is a byte-swap, not a decode. They are also by far the largest assets in the game -- up to 250 MB each -- which drives most of the design here.

Telling them from a normal WAV needs care, since the first 12 bytes are identical. The reliable discriminator is the `fmt ` chunk size: it is 16 (or 18/40) in every real file, so a big-endian 16 read little-endian comes out as `0x10000000`. Whichever way reads as a legal size tells you how the file is stored -- no heuristics, no guessing.

Nothing in this path ever reads a whole file. `BEWavParser::Load` reads only the header and walks to the `data` chunk; conversion streams through a 1 MiB window on a worker thread with a progress bar and a cancel button; and playback doesn't convert at all -- the samples are already PCM, so the player streams them straight off the source file and swaps each buffer as it fills it. That is what keeps a 250 MB track from costing 250 MB of RAM. This mirrors `scripts/convert_wav.py` in the Reprise repo.

Clicking one of these must also bypass the generic chunk scanner in `Application.cpp`, which walks a file byte by byte against ~18 magics -- on 250 MB that alone freezes the UI for a long time.

### Audio Playback
Preview playback is a convenience layer over the export path, not a second implementation of it. Neither codec in a CSF or CXS has a usable in-process decoder -- XMA2 is a WMA-Pro variant, and XAudio2 on Windows does not decode XMA at all (it's Xbox-only) -- so rather than vendor libavcodec for a preview button, `AudioPlayer` shells out to `ffmpeg` if it is on `PATH`, reusing `CSFParser::ExportClip` / `CXSParser::ExportXMA` so preview and export can never disagree about framing. Big-endian WAVs need no decoder and play whether or not ffmpeg is installed.

**Everything runs on a worker thread.** Decoding a full BGM track takes seconds and the WAVs are enormous, so the UI thread only ever reads atomics: `State()` (`Idle`/`Preparing`/`Playing`/`Failed`), position, duration. `Stop()` doubles as cancel, and it terminates a running ffmpeg rather than waiting out the decode -- otherwise cancelling would block for exactly as long as the thing you were trying to cancel.

**Output goes through `waveOut`, not `PlaySound`.** This is forced by looping: a CXS track's loop region is an interval in the middle of the stream, and `PlaySound`'s `SND_LOOP` can only repeat a whole file. The player instead keeps four ~125 ms buffers and a byte cursor, and when the cursor reaches `loop_end` it seeks back to `loop_start`, so a preview loops exactly the way the track does in game. Streaming also means the source stays on disk rather than resident in memory, and it gives a real position readout -- `PlaySound` offers no position query for an async sound.

One non-obvious requirement: the worker thread initializes COM for itself before touching `waveOut`. `waveOut` is backed by WASAPI on modern Windows and opening a device from a thread with no COM apartment can fail outright, while the UI thread happens to have COM initialized already (the shell folder picker does it) -- so a player that relied on that would work or not depending on what the user had clicked first. The open is also retried a few times, falling back from `WAVE_MAPPER` to device 0, and reports the raw `MMRESULT` on failure.

ffmpeg is invoked with `-bitexact` alongside `pcm_s16le` to suppress the `LIST`/`ISFT` encoder tag it otherwise writes between `fmt ` and `data`, keeping the intermediate to a plain `fmt`+`data` WAV that the streamer's chunk walk can rely on.

### EP3 Parsing
EP3 files open with a `Mefc` header and a chunk-key block at offset `0x28` that lists the embedded components. The parser scans the file for NTX3 sub-blocks, reads each one's width/height/format from the standard NTX3 fields, and produces an `Ep3Frame` with the absolute pixel offset and size. Frames flagged with format bit `0x20` are swizzled in Morton (Z-order) block order; the deswizzler interleaves the bits of the block coordinates to recover the linear order. Each frame is surfaced as a synthetic NTX3 chunk in the file browser so the existing texture viewer can render and export it.

## License
MIT License -- see LICENSE file for details.
This project is for educational and preservation purposes. All game assets remain property of their respective copyright holders (Bandai Namco, tri-Crescendo).
## Contributing
Pull requests are welcome. For major changes, please open an issue first to discuss proposed modifications.
## Contact
For questions or bug reports, please open an issue on GitHub.