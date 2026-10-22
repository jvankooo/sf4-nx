# How the port draws the game (the native renderer)

## In short

- The SDK's Xenos emulation drew SF4 correctly but ran fights at 4-5 fps on the Switch (console run 4, GPU busy
  93 %), the same wall nfsmw-nx and masseffect-nx hit before their native renderers.
- So the port uses **masseffect-nx's native renderer** (`app/src/native`). The game's Direct3D runs as it is and
  writes its GPU commands (the [PM4 ring](glossary.md#pm4-ring)); the renderer reads them and draws the same frame
  with Vulkan, with the shaders translated before the game runs (`sf4_shaders.mesp`, see [shaders.md](shaders.md)).
- It is on by default on the Switch (`masseffect_renderer_native = true`); the Xenos emulation stays as the fallback
  and is what the Direct3D 12 PC build uses. The Vulkan PC build (`tools/build_pcvk.bat`) runs the native renderer.
- On the PC it draws the logos, menus, character select, the story movies and whole fights (HUD, hit effects, K.O.)
  like the emulation, at 1280x720 and at 854x480.
- First console run (2026-10-22, `console-run-logs/logs_run_native_1`): menus at 60 fps, fights drawn at 30 but the
  game itself at 30 ticks per second instead of 60, so in slow motion. What was done about it is under
  [Speed](#speed); it needs the next console run to confirm.

Mass Effect's renderer was written for one game. Bringing it to SF4 took the fixes below; each one is general
enough to matter for the next game too.

## What SF4 needed

| Symptom on the PC | Cause | Fix |
|---|---|---|
| Black screen after the logos | SF4's Direct3D reorders the vertex fetches of a shader into **different** temporaries (library order r3 r0 r1 r2, loaded r3 r1 r2 r0). The renderer only accepted reorders into the same register, so the UI shaders were never identified and their draws were dropped | `me_shader_identity.h`: a fetch run is any block of back-to-back fetches with the same opcode and source in one exec clause; each library fetch must have one loaded partner writing the same register and components |
| A few fight draws without shaders | Direct3D removes the vertex shader exports the pixel shader does not read (the ALU instruction stays, with no write mask) | `VertexExportDropped` (cvar `sf4_native_vs_dropped_exports`) |
| Stale picture, shifted by one tile, from the character select on | Mass Effect's EDRAM mode 4 rejects SF4's tiled surfaces | `masseffect_native_edram_alias_mode = 0` (below) |
| 80x16 stair stripes over the fight HUD; the P2 portrait ghosted | Direct3D clears every surface a second time through a **4x view at half the pitch** (the back buffer only that way). With `sf4_native_msaa_off` both are 1x and the view covers other tiles | `sf4_native_msaa4_views`: such a clear is drawn on the 1x surface it stands for (pitch x2, the rectangle scaled through the viewport registers; the surface's MSAA is remembered from the game's draws at that base and pitch) |
| Black squares where hit sparks go | Blend op 4 (reverse subtract) was mapped to subtract | `VK_BLEND_OP_REVERSE_SUBTRACT` |
| At 854x480 the picture squeezed, a dark strip on the right | The back buffer is resolved at a pitch of 864 and sampled as 854 wide; the padded image was sampled whole | The logical-size crop of resolved textures runs in every EDRAM mode; Present outputs a wider front buffer texel for texel |
| Fights at 9-35 fps on the PC | Mass Effect's draw-extent proofs run the vertex shader on the CPU for every rectangle-like draw (60-160 us each); SF4 draws mostly quads | The proofs only run in EDRAM mode 4 |

### Predicated tiling and MSAA

SF4 draws its 3D scene in three predicated tiles (2x MSAA at 720p: three tiles of 544 pixels at a pitch of 560;
4x at 480p: tiles of 320). Every draw and resolve of the scene is in the ring three times, each predicated on its
tile's bin. `sf4_native_msaa_off` (default on) draws every surface at 1x: the tiles' window offsets, scissors and
resolve rectangles stay valid at 1x. The cost is that each scene draw is still recorded once per tile (see below).

### EDRAM mode 0

Mode 0 keeps one image per (EDRAM base, format, pitch) and never copies between views. SF4 clears or redraws each
target before reading it, and the only views it reads across are Direct3D's 4x clears, handled above. Modes 1 and
3 also draw correctly but leave a one-pixel seam at 720p; mode 4 rejects SF4's surfaces.

## Settings

Set by `src/sf4_app.h` on the Switch when nothing else does, and listed in `sf4.toml`:

| Setting | SF4 value | Why |
|---|---|---|
| `masseffect_renderer_native` | true | the native renderer |
| `masseffect_native_edram_alias_mode` | 0 | above |
| `sf4_native_msaa_off` | true | above |
| `masseffect_native_skip_prepass`, `masseffect_native_postponed_sky` | false | Mass Effect shortcuts keyed on its own passes (depth-only draws at the screen width, a 480-index sky dome) |
| `video_mode_width` / `height` | 854 / 480 (sf4.toml) | the first console run had the GPU at about 13 ms of work per Swap at 854x480 (26 per drawn frame, two thirds the 3D scene); 1280x720 is about 45-50 ms per drawn frame, over the 33 ms of 30 fps. With 720p the second console run had fights at about 1 fps (cause not measured yet) |
| `sf4_draw_every_n_frames` | 2 | the command player draws one frame in two; the game runs at 60 |
| `sf4_native_tile_collapse`, `sf4_native_skip_writeless`, `sf4_vblank_catchup` | true | [Speed](#speed) |
| Mass Effect's speed settings (`masseffect_native_deferred_recording`, `..._pm4_fast`, `..._fast_pair`, `..._load_memo`, `..._constants_dirty`, `..._vertex_arena`, `..._fast_copy`, `..._pipelines_prewarm`, ...) | as masseffect-nx ships them | listed in `sf4.toml`; all exact, each one checked against the slow path for its first uses (`masseffect_native_verify_n`) |

## Tools for the next problem

| Tool | What it shows |
|---|---|
| `sf4_native_draw_exits` | every 16384 draws, which return of the draw recorder they left by (most returns are silent) |
| `sf4_native_draw_trace=N` (`_skip`, `_at_s`) | the first N real draws after a count or a time: render state, targets, shaders, bound textures |
| `sf4_native_pm4_dump_swap=N` + `tools/pm4_frame.py` | every packet of the frames around Swap N in `pm4_dump.txt`; the script replays the registers and lists each draw and resolve with its targets, window, scissor, predication and textures (`--draws --fetch --clears`) |
| `sf4_capture_every_s` | PNG captures work with the native renderer (the window screenshot script does not from a non-interactive shell) |
| `MASSEFFECT_SHADER_DISCOVERY=<dir>` | the microcode of every shader the library cannot identify |

## Speed

**The console's first run** (2026-10-22, 720p not yet the default, 480p): fights at exactly 300 Swaps per 10 s, i.e.
the game ticked 30 times per second instead of 60. Main thread about 60 % of a core, ring thread 52 % (about 47 us per
draw, about 730 ring draws per drawn frame), audio mixer 35 %, GPU about 13 ms per Swap at 854x480. The game's Swap waits for
the previous frame's fence, so the ring thread's work on a drawn frame (about 34 ms) and the game's own frame are on
one path: more than the 33 ms two game frames have.

What changed after it, all checked on the PC through menus, the story movies and a whole fight:

- **The tiled scene is drawn once** (`sf4_native_tile_collapse`). SF4 draws its 3D scene three times, once per
  predicated tile. The first tile pass now draws on a surface as wide as all the tiles (window offset and scissor moved
  to the tile's place), the later passes' common draws are dropped, and each tile's resolve reads its area of the wide
  image (`RegistersCopy::dest_shift_x/y`). Draws a tile needs on its own (tile-specific bins, and draws that sample a
  resolve of the same block) still run per tile. The wide pitch is one tile further than the tiles need, so the wide
  surface never shares the back buffer's image. About 60 % fewer ring draws in fights.
  A block is decided at its first shared draw that writes on the tiles' surface, or at its first resolve. If that
  resolve goes to a texture the size of one tile, the block stays per tile: the VS and world-map screens resolve
  each tile into the same 544x720 texture and then draw over the tile reading it by pixel position, which on the
  wide surface lands past that texture (their right part was black; console report 2026-10-23). The tiles' own
  clears before the decision go to both surfaces. A packet predicated on some tiles of the block but not on one
  tile alone runs in the first pass, in stream order (`RunsInFirstPass`). `sf4_native_tile_collapse_debug=16` logs
  each decision.
- **Direct3D's one-point draws** that write no color, depth or stencil are dropped before the draw path
  (`sf4_native_skip_writeless`).
- **Mass Effect's speed settings** (sf4.toml): Vulkan recording on a worker thread, the fast PM4 parser, pairing and
  shader identity, constant and vertex caches; Mass Effect's ring went from about 47 to 8.5 us per draw with them.
- **VBlank catch-up** (`sf4_vblank_catchup`): when the game is behind a steady 60 Hz schedule of Swaps, the VBlank its
  Swap waits for fires at once, so a heavy frame followed by a light one still gives 60 game frames per second. It
  is triggered from the game thread (`sf4_ring_wait.cpp`, the Swap's fence wait); on the ring's WAIT_REG_MEM it fired
  on the interrupt handshakes too.
- **Faster generated code** (`app/perf_overrides.toml`, as masseffect-nx): guest registers as C++ locals, no MSR
  fences, no dead `lr` stores. Turning the non-volatile registers into locals needed: the guest setjmp/longjmp
  (Lua's error handling) mapped to the host's, the false `share_registers` mark on `sub_82269FE0` removed (it copied
  r29-r31 back into a Lua caller), and Bink's VMX code marked (its constant table is loaded into v64-v127 by
  `sub_824C80C0` and read by the IDCT leaves; without that every movie decoded to green blocks).

The busy wait was fixed earlier: SF4's Direct3D waits for the previous frame's fence in its Swap (`sub_82212798`
with reason 3) by calling `sub_82216850` in a loop, the XDK routine masseffect-nx's `me_ring_wait.cpp` hooks (SF4's
device offsets: fence word pointer at +10896, "GPU hung" flag 0x02 at +10941). On the PC it ran 15-26 million times
per second and kept the main thread at 97 % of a core. `src/native/sf4_ring_wait.cpp` (cvar `sf4_wait_blocking_ring`,
default on) sleeps until the ring thread writes progress back (at most `sf4_wait_ring_max_us`), and `[ring_wait]` in
the log reports it every 10 s. Its loads use the generated code's address rule (`REX_PHYS_HOST_OFFSET`: the
0xE0000000 view is 4 KB further on Windows only).

What to look at in the next console log: `[sf4] command player, last 10 s: N drawn frames` (300 = 30 drawn per second)
and `[native] 10.0 s: N Swaps` (600 in a fight = the game at full speed), the ring thread's share in the thread
report, and the tile collapse line of the native report.
