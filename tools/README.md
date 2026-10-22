# Tools

Scripts to build, translate, test and analyse the port. Run them from the repository root. Several come from
masseffect-nx (gap tools, `direct_calls.py`, `read_before_write.py`, `callgraph.py`, `pm4_ops.py`) and nfsmw-nx
(`fase1_extraer.py`, `fetch_thirdparty.py`, the Switch toolchain), adapted to SF4.

## Build

| File | What it does |
|---|---|
| `fase1_extraer.py` | Extracts the Xbox 360 ISO into `assets/game_root` and prints the XEX header (`--info`, `--listar`). |
| `fetch_thirdparty.py` | Fetches the SDK's third-party sources (ReXGlue v0.10.0) into `sdk/thirdparty`, keeping the changed files. |
| `build_host.bat` | Builds the code generator `sdk/out/win-amd64/rexglue.exe` with clang in the VS 2022 environment. |
| `codegen.ps1` | `rexglue codegen app/sf4_manifest.toml`, then `direct_calls.py`; fails if the code has unresolved-branch stubs. |
| `build.ps1` | Builds `app/out/sw/sf4.nro` for the Switch and copies it with `sf4.toml` and the shader library (`assets/shaderlib/sf4_shaders.mesp`) to `build/sf4-nx/`. Run `codegen.ps1` first: the PC build regenerates without direct calls. |
| `build_pc.bat` | Builds the PC test version `app/out/pc/sf4.exe` with the Xenos emulation on Direct3D 12 (its CMake runs the codegen itself). |
| `build_pcvk.bat` | Builds `app/out/pcvk/sf4.exe` with the Vulkan backend and the native renderer (`--masseffect_renderer_native=true`; `sf4_shaders.mesp` next to it). |
| `run_pc.ps1` | Runs the PC build for N seconds with extra arguments and prints the end of the log. |
| `contact_sheet.ps1` | Puts the captures of a run (`sf4_capture_every_s`) in one image, `app/out/pc/captures/sheet.png`. |
| `english_only.py` | Removes the non-English text folders and the Japanese voices from the SD copy of the disc (`build/sf4-nx/game_root`, 370 MB); lists them without `--apply`. Never run it on `assets/game_root`. |

On this machine `cmd` and `ninja` on the PATH are MSYS2 shims: the scripts call `C:\Windows\System32\cmd.exe` and the
VS BuildTools ninja.

## Code the analysis misses (gaps)

| File | What it does |
|---|---|
| `find_gaps.py` | Lists the stretches between functions the analysis left without an owner. |
| `gap_classify.py`, `gap_fixpoint.py` | Declares only self-contained leaves and thunks, pass after pass until nothing new qualifies. Do not declare every gap: in SF4 some are data and broke the analysis. |
| `pointer_scan.py` | Code pointers stored in data that land in gaps, from an `SF4_DUMP_IMAGE` dump. |
| `runtime_gap_resolver.py` | From a log that stopped on "Call to invalid or unregistered function", proves the address lies in a gap. |
| `read_before_write.py` | Registers a function reads before writing, for when the register-locals options are turned on. |
| `direct_calls.py` | Calls between recompiled functions without a hook become direct calls (run by `codegen.ps1`). |

## The game's Direct3D (native renderer, phase A)

See [../docs/d3d-map.md](../docs/d3d-map.md).

| File | What it does |
|---|---|
| `callgraph.py` | Call graph of the generated code (`build`, `callers`, `callees`, `grep`) in `out/callgraph.json`. |
| `import_map.py` | Which kernel imports the functions of each address block call: where the XDK libraries are. |
| `pm4_scan.py`, `pm4_ops.py` | Functions that build PM4 packet headers (all at once, or the given ones). |
| `d3d_trace_list.py` | Writes `app/src/sf4_d3d_trace_all.inc`, the functions `sf4_d3d_trace.cpp` wraps. |
| `d3d_report.py` | One report of `rex_d3d.log` as a table, with the first calls of each function; `--commands` prints the report's command-stream section (counts and payload samples per opcode). |
| `command_tables.py` | The opcode tables of the command player `sub_82287DF0`, with calls per frame from a trace. |
| `listing.py` | The PowerPC listing of the function that contains an address. |
| `extract_shaders.py` | Extracts the 379 XDK shader containers (with their names) from `game_root/Shader/*.emb` into `assets/shaders`, the input of the shader library. |
| `pm4_frame.py` | Replays the packets `sf4_native_pm4_dump_swap` writes (`pm4_dump.txt`) and lists each draw and resolve with its render targets, window, scissor, predication and textures. See [../docs/native-renderer.md](../docs/native-renderer.md). |
| `dxc/` | Microsoft's DirectXShaderCompiler v1.9.2609 (official release zip, `bin/x64/dxc.exe`): HLSL to SPIR-V for the shader library. |
