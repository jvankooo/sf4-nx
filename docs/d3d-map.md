# The game's Direct3D: phase A map

## In short

Phase A of a native renderer (the order of nfsmw-nx and masseffect-nx: first confirm which function is which, then
replace anything) for Street Fighter IV. Everything here was measured on the PC build with `sf4_d3d_trace`, in a
scripted run that goes from the logos through the menus, character select and Abel's prologue into a full round
against Ken (Volcanic Rim), a K.O., continue, game over and back to the main menu.

Every opcode with a handler now has a name, from its handlers' code and the commands of a real fight: see
[The opcodes](#the-opcodes). The few details still open are listed at the end.

The main finding: **SF4 does not draw by calling Direct3D directly.** The game records its rendering into a
command buffer, and a player, `sub_82287DF0`, walks the commands once per frame and calls one handler per opcode.
The handlers set the GPU state and build the PM4 packets. A native renderer can read that command stream, which is
a far smaller and better-structured thing to replace than the PM4 ring nfsmw-nx and masseffect-nx had to decode.

## How it was found

| Step | Tool | Result |
|---|---|---|
| Where the XDK's Direct3D is | `tools/import_map.py`, `tools/callgraph.py` | the users of `Vd*`, `Mm*Physical*` and `KeGetCurrentProcessType` sit in 0x8220E000-0x8222BFFF; with the functions around them, 0x8220C000-0x8222BFFF |
| Which functions build GPU commands | `tools/pm4_scan.py` | the four functions that build `DRAW_INDX` packets are not in that range but at 0x8228A8D0-0x8228B648 |
| The second part | call graph | 0x82287000-0x8228B7FF calls into the core and is called by the game |
| Who calls what, how often | `src/sf4_d3d_trace.cpp` (675 functions wrapped), `tools/d3d_report.py` | `rex_d3d.log`: calls per frame, callers, first arguments |
| Reaching a fight unattended | `src/sf4_test_input.cpp` (`sf4_test_buttons`), `src/sf4_capture.cpp` (`sf4_capture_every_s`), `tools/contact_sheet.ps1` | the script in `docs/fight_script.txt` |
| The command player | `tools/listing.py 0x82287E70`, `tools/command_tables.py` | the opcode tables below |
| What each opcode carries | the command sampler of `src/sf4_d3d_trace.cpp`, `tools/d3d_report.py --commands` | counts per opcode, payload samples, the resource behind each pointer |
| What each opcode does | `tools/listing.py` on both handlers of every opcode, and on the device functions they call | the names in [The opcodes](#the-opcodes) |

To repeat it (PC build, `tools/build_pc.bat`):

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_pc.ps1 -Seconds 170 -Extra "--sf4_test_buttons=$(Get-Content docs\fight_script.txt) --sf4_capture_every_s=10 --sf4_d3d_trace=true"
python tools\d3d_report.py app\out\pc\rex_d3d.log --report 10 --api --min 0.5 --detail 1
python tools\command_tables.py <SF4_DUMP_IMAGE dump> app\out\pc\rex_d3d.log --report 10
python tools\d3d_report.py app\out\pc\rex_d3d.log --report 8 --commands --op 29 --op 43
```

The opponent and the stage are random, so the fight's reports move by a few: the commands per buffer of each
report (a few hundred in menus, over 10,000 in a fight) and the captures show where it is.

In the first traced run (against Ken) report 10, 90-100 s, was the middle of the round: 595 frames in 10 s. The
function tables below come from it; the opcode tables from report 8 of a later run (against Ryu).

## The frame

- `sub_8221A058` is the only caller of `VdSwap` (and of `VdSetDisplayMode` and `VdGetSystemCommandBuffer`): one
  call per frame, 60 per second in menus and fights. The trace uses it as its frame clock.
- It is called from `sub_822887A0` (from 0x82333A58), once per frame.
- `sub_82212D18` initializes the ring buffer (`VdInitializeRingBuffer`); `sub_82212AB0` is the graphics interrupt
  callback (`SetInterruptCallback(82212AB0, 40002180)` in the log), 4 per frame.

## Three objects

| Address | Seen as | What it is (evidence) |
|---|---|---|
| 0x40002180 | `r3` of the core functions (0x8221xxxx), `r4` of the interrupt callback | the XDK Direct3D device, in physical memory |
| 0x8270A6C8 | `r3` of the recorders (0x822875xx-0x82287Bxx); they return pointers into 0x401000xx | the object the game records commands into |
| 0x826330A0 | `r3` of `sub_82287DF0` and of every handler | the object that plays the commands |

## The command player: sub_82287DF0(player, buffer)

```text
buffer +8   base of the command words
buffer +16  first segment, +20 end; 8 bytes per segment, +4 = word offset of its first command from the base
command     header word = opcode << 16 | length in words (header included), then its payload w1, w2...
            0x00010000 ends the segment
for each command: if (tableA[opcode](player, cmd) != 0) tableB[opcode](player, cmd)
tableA = 0x82500980, tableB = 0x82500790, 61 opcodes
```

For the state opcodes, handler A compares the new value with a copy the player keeps (from player+0xE4 on) and
returns 1 only when it changed; handler B then writes it to the device. Most other opcodes have the same function
in both tables and return 0. The player and the recorders ran on the same thread in the trace (thread tag t1).

The game records one buffer per frame: about 530 commands in the menus, 12,000-20,000 in a fight depending on the
stage (the commands per buffer of the reports of `sf4_d3d_trace`, which walks one buffer every 30 frames before
the player runs: `WalkCommands` in `src/sf4_d3d_trace.cpp`).

## The opcodes

How each one was named: **code** = from what its handlers write (a device function, or the field of a GPU register
copy, see [The device](#the-device-0x40002180)); **payload** = from the values in the fight (`tools/d3d_report.py
--commands`), including the type of the resource a pointer leads to (the low nibble of the first word of an XDK
resource: 3 texture, 4 surface, 5 vertex declaration, 6 vertex shader, 7 pixel shader). Calls per frame are from a
fight (report 8, 70-80 s, of a run against Ryu on the Old Temple stage; a run on the Diner stage had about 1.6 times
as many commands). The names are the
Direct3D call each opcode stands for; payload words are w1, w2... after the header.

| Op | Name | Payload | Per frame | Evidence |
|---|---|---|---|---|
| 0, 1, 6, 46-49, 59, 60 | unused | | 0 | `sub_82213930` (`li r3,0; blr`) in both tables |
| 2 | **inline data** | vertices, indices, rects... | 58 | same empty handler; floats; ops 30, 31, 50, 52 point into it with word offsets |
| 3 | callback | w1 = index in the table at 0x82619480 | 0 | code: calls the table entry with the command |
| 4 | **SetRenderTarget** | w1 = index, w2 = surface (type 4) or 0 | 45 | code: `sub_82215FC8(device, index, surface)`; payload |
| 5 | **SetDepthStencilSurface** | w1 = surface (type 4) | 9 | code: `sub_82216310(device, surface)`; payload |
| 7 | restore the default targets | | 2 | 2
| 8 | **Clear** (ClearF) | w1 = flags (0x3F: 4 targets, Z, stencil), w2-w5 = color RGBA float, w6 = Z, w7 = stencil | 7 | code: `sub_82221A68(device, flags, NULL, &color, Z, stencil)`; payload (0, 0, 0, 1), 1.0, 0 |
| 9 | **SetViewport** | w1 = X << 16 \| Y, w2 = width << 16 \| height, w3 = MinZ, w4 = MaxZ | 28 | code: builds a D3DVIEWPORT for `sub_82215D10`; payload 1280x720, 0-1 |
| 10 | **color write mask** of a render target | w1 = target 0-3, w2 = RGBA mask | 58 | code: 4 bits per target in RB_COLOR_MASK; payload 0xF / 0x7 |
| 11 | **Z enable** | w1 | 357 | code: RB_DEPTHCONTROL bit 1 (off without a depth buffer) |
| 12 | **Z write enable** | w1 | 357 | code: RB_DEPTHCONTROL bit 2 |
| 13 | **Z function** | w1 = compare function | 46 | code: RB_DEPTHCONTROL bits 4-6; payload 3 (less-equal), 6 (greater-equal) |
| 14 | **alpha test enable** | w1 | 213 | code: RB_COLORCONTROL bit 3 |
| 15 | **alpha function and reference** | w1 = function, w2 = reference 0-255 | 191 | code: RB_COLORCONTROL bits 0-2, reference as float in RB_ALPHA_REF; payload (7, 0), (6, 100) |
| 16 | **stencil enable** | w1 | 61 | code: RB_DEPTHCONTROL bit 0 (off without a depth buffer) |
| 17 | **stencil function, reference and mask** | w1 = function, w2 = reference, w3 = mask | 61 | code: RB_DEPTHCONTROL bits 8-10, RB_STENCILREFMASK bytes 0 and 1; payload (6, 0xFE, 0xFFFFFFFF) |
| 18 | **stencil write mask** | w1 | 61 | code: RB_STENCILREFMASK byte 2 |
| 19 | **stencil operations** | w1 = fail, w2 = Z fail, w3 = pass | 61 | code: RB_DEPTHCONTROL bits 11-13, 17-19, 14-16; payload (keep, keep, replace) |
| 20 | **fill mode** | byte 7 of the command | 0 | code: PA_SU_SC_MODE_CNTL bits 3-10 |
| 21 | **cull mode** | w1 | 260 | code: PA_SU_SC_MODE_CNTL bits 0-2; payload 0 / 6 |
| 22 | **depth bias** | w1 = slope scale, w2 = constant bias (floats) | 0 | code: `sub_82214510`, `sub_82214448` (polygon offset scale/offset, enable bits 11-12 of PA_SU_SC_MODE_CNTL) |
| 23 | **blend control** of a render target | w1 = target 0-3, w2 = the whole register | 362 | code: RB_BLENDCONTROL0 (target 0) or the copies of targets 1-3; payload 0x01860186, 0x00010001 (one, zero) |
| 24 | **other sampler states** | byte 6 = state, byte 7 = sampler, w2 = value | 0 | code: state 0 border color (fetch word 5), 1 LOD bias (word 4 bits 12-21), 2 max mip level (`sub_82215878`), 3 min mip level (`sub_822157F8`) |
| 25 | **sampler address modes** | w1 = sampler, w2 = bytes U, V, W | 447 | code: fetch constant word 0 clamp_x/y/z; payload (12, 2/2/0) |
| 26 | **sampler filters** | w1 = sampler, w2 = bytes mag, min, mip, anisotropic | 1121 | code: `sub_822151F0` (mag), `sub_82215398` (min), fetch word 3 mip filter; payload 1/1/2/0 |
| 27 | **sampler max anisotropy** | w1 = sampler, w2 = level | 1121 | code: `sub_82215638` (fetch word 3 aniso field through a table); payload 1; always right after 26 |
| 28 | **DrawPrimitive** | w1 = type, w2 = primitive count, w3 = start vertex | 0 | code: count through the table at 0x8206F660, `sub_8228B258` (DrawVertices, `DRAW_INDX`) |
| 29 | **DrawIndexedPrimitive** | w1 = type, w2 = primitive count, w3 = base vertex, w4 = number of vertices (not passed on), w5 = start index | **208** | code: `sub_8228B648(device, type, w3, w5, index count)` (DrawIndexedVertices, `DRAW_INDX` with 4 words): D3D9's DrawIndexedPrimitive order; payload type 4 (triangle list); checked draw by draw ([Step 2](#step-2-the-frame-decoder)) |
| 30 | **DrawPrimitiveUP** | w1 = type, w2 = primitive count, w3 = stride, w4 = word offset of the vertices (op 2 before it) | **54** | code: `sub_8228A8D0` (BeginVertices) then memcpy (`sub_8237EA10`); recorded by `sub_82287790`; payload types 5/6 (fan/strip), 2 primitives, stride 0x1C |
| 31 | **DrawIndexedPrimitiveUP** | type, count, ..., offsets of vertices and 16-bit indices | 0 | code: `sub_8228AD68` (BeginIndexedVertices) and two memcpy |
| 32 | **SetStreamSource** (stream 0) | w1 = offset, w2 = stride, w3 = vertex buffer | 206 | code: `sub_82215D90(device, 0, buffer, offset, stride, 1)`; payload strides 0x14-0x40 |
| 33 | **SetIndices** | w1 = index buffer | 206 | code: `sub_82215F38(device, buffer)`; payload: the object right after the vertex buffer of op 32 |
| 34 | **SetVertexDeclaration** | w1 = declaration (type 5) | 250 | code: stored at device+11820; payload |
| 35 | **SetVertexShader** | w1 = shader (type 6) | 242 | code: `sub_8221EDE0`; payload |
| 36 | **SetPixelShader** | w1 = shader (type 7) | 242 | code: `sub_8221EA38`; payload |
| 37 | **SetVertexShaderConstantF** | w1 = start register << 16 \| count, w2 = word offset of the data | 3185 | code: `sub_8221E750` writes device + 16 * (120 + register); recorded by `sub_822877E8` |
| 38 | **SetVertexShaderConstantI** | same | 0 | code: packs bytes 0, 4, 8 of each vector into device + 4 * (2536 + register) |
| 39 | **SetVertexShaderConstantB** | same | 347 | code: bits of device + 4 * (2528 + register / 32); recorded by `sub_82287878` |
| 40 | **SetPixelShaderConstantF** | same | 1321 | code: `sub_8221E830` writes 256 registers higher than op 37; recorded by `sub_822878E8` |
| 41 | **SetPixelShaderConstantI** | same | 0 | code: like 38 at 2552 |
| 42 | **SetPixelShaderConstantB** | same | 344 | code: like 39 at 2532; recorded by `sub_82287978` |
| 43 | **SetTexture** | w1 = sampler, w2 = texture (type 3) or 0 | 421 | code: `sub_8220F9C0` copies the texture's fetch constant into the slot and keeps the texture at device + 4 * (3136 + sampler) |
| 44 | **user clip planes enable** | w1 = mask | 2 | code: PA_CL_CLIP_CNTL bits 0-5 |
| 45 | **SetClipPlane** | w1 = index, w2-w5 = plane | 1 | code: device + 16 * (642 + index) |
| 50 | **Resolve** | w1 = flags, w2 = source rect offset, w3 = destination texture (type 3), w4 = destination point offset, w5 = level, w6 = slice, w7 = clear color offset, w8 = Z, w9 = stencil, w10 = parameters offset | 37 | code: `sub_8221F918` with the XDK Resolve argument order; payload; recorded by `sub_82287A08` |
| 51 | **BeginTiling** | w1 = flags, w2 = tile count, w3 = rects offset, w4 = color offset, w5 = Z, w6 = stencil | 1 | code: `sub_82218FD0`; payload: 3 tiles |
| 52 | **EndTiling** | w1 = flags, w2 = resolve rects offset, w3 = destination, w4 = color offset, w5 = Z, w6 = stencil, w7 = parameters offset | 1 | code: `sub_82219368` |
| 53 | **SetPredication** | w1 = tile mask | 44 | code: `sub_82218E60` sets device+12716, the predicate bits ops 57-58 put in their packets; payload 0, 3, 0xC, 0x30 |
| 54 | **SetShaderGPRAllocation** | w1 = flags, w2 = vertex, w3 = pixel | 2 | code: `sub_8221F140`; payload (0, 24, 104): 24 + 104 = 128 registers |
| 55 / 56 | clear / set a range of bits in a device state mask | w1 = mask word << 16, w2, w3 = range | 1 | code: `andc` / `or` on device + 8 * (w1 >> 16); once per frame each, payload (0x00014000, 2, 2) |
| 57 | **load pixel shader constants from GPU memory** | w1 = start << 16 \| count, w2 = physical address | 3 | code: writes a `LOAD_ALU_CONSTANT` packet (0xC0022F00) into the ring with register start + 256; recorded by `sub_82287B98` |
| 58 | **load vertex shader constants from GPU memory** | same | 0 | code: same packet without the +256 |

## The device (0x40002180)

What the handlers taught about the XDK device, as offsets from its start:

| Offset | What |
|---|---|
| +16, +24, +32 | 64-bit dirty masks: render state groups, fetch constants (bit 32 + sampler), shader constants |
| +48, +56 | the write pointer of the command ring and its limit (ops 57-58 write packets there; `sub_82212698` makes room) |
| +1152 + 24 * n | texture fetch constant of sampler n (6 words, the Xenos fetch layout) |
| +1920 + 16 * r, +6016 + 16 * r | vertex and pixel shader float constants |
| +10112, +10128 | vertex and pixel shader bool constants |
| +10144, +10208 | vertex and pixel shader integer constants |
| +10272 + 16 * i | user clip planes |
| +10460 | RB_COLOR_MASK |
| +10496, +10500 | RB_STENCILREFMASK, alpha reference |
| +10548, +10552, +10556 | RB_DEPTHCONTROL, RB_BLENDCONTROL0, RB_COLORCONTROL |
| +10564, +10568 | PA_CL_CLIP_CNTL, PA_SU_SC_MODE_CNTL |
| +10832-10844 | depth bias values |
| +11820 | current vertex declaration |
| +12716 | predication bits for the packets |
| +12440-12452, +12456 | whether render target 0-3 and a depth buffer are bound (the color masks, Z and stencil enables are forced off when not) |
| +4 * (3136 + n) | texture bound to sampler n |

## Direct3D functions named on the way

| Function | Name | From |
|---|---|---|
| sub_8221A058 | the swap (only caller of VdSwap) | imports |
| sub_82215FC8 | SetRenderTarget | op 4 |
| sub_82216310 | SetDepthStencilSurface | op 5 |
| sub_82221A68 | ClearF | op 8 |
| sub_82215D10 | SetViewport | op 9 |
| sub_8228B258, sub_8228B648 | DrawVertices, DrawIndexedVertices | ops 28, 29 |
| sub_8228A8D0, sub_8228AD68 | BeginVertices, BeginIndexedVertices | ops 30, 31 |
| sub_82215D90, sub_82215F38 | SetStreamSource, SetIndices | ops 32, 33 |
| sub_8221EDE0, sub_8221EA38 | SetVertexShader, SetPixelShader | ops 35, 36 |
| sub_8221E750, sub_8221E830 | SetVertexShaderConstantF, SetPixelShaderConstantF | ops 37, 40 |
| sub_8220F9C0 | SetTexture | op 43 |
| sub_822151F0, sub_82215398, sub_82215638 | sampler mag filter, min filter, max anisotropy | ops 26, 27 |
| sub_82215878, sub_822157F8 | sampler max / min mip level | op 24 |
| sub_82214510, sub_82214448 | slope-scale depth bias, depth bias | op 22 |
| sub_8221F918 | Resolve | op 50 |
| sub_82218FD0, sub_82219368 | BeginTiling, EndTiling | ops 51, 52 |
| sub_82218E60 | SetPredication | op 53 |
| sub_8221F140 | SetShaderGPRAllocation | op 54 |
| sub_8237EA10 | memcpy | ops 30, 31 |

## Step 2: the frame decoder

`src/sf4_commands.cpp` hooks the player `sub_82287DF0` (and the four draw functions the draw opcodes call) and lets
diagnostics look at each buffer before the original plays it. `src/sf4_frame_decode.cpp` uses it to write whole
frames as text. Both are in the PC and the Switch builds; off by default, the hook costs one flag test per frame.

```powershell
powershell -ExecutionPolicy Bypass -File tools\run_pc.ps1 -Seconds 115 -Extra "--sf4_test_buttons=$(Get-Content docs\fight_script.txt) --sf4_frame_dump=20,90,100 --sf4_frame_dump_count=2"
```

`sf4_frame_dump` lists seconds after the first frame; at each, `sf4_frame_dump_count` consecutive buffers are
decoded into `frames/frame_<s>s_<n>.txt` next to the executable (about 18 ms and 160 KB for a fight frame). A frame
reads like this (a fight, 90 s; lines cut):

```text
== pass 3: render target 1 = surf@4218DC70 [05000140 00000030 00000000 04FC0598]
depth buffer = surf@4218DCB0 [05000140 00010060 00000011 04FC0598]
viewport 0,0 320x180 z 1-0
clear flags 3F color (0 0 0 1) z 0 stencil 0
clip plane 0 (-1.23214e-13 -0.363273 20.9995 -21.0641)
draw 0  DrawIndexedPrimitive   triangles prims 480, indices 1440, base vertex 0, start index 0, 274 vertices
        alpha=off  blend0=00010001  clip=1  cull=2  decl=decl@40009EC0  depth=surf@4218DCB0  ib=IB@443CCCB0
        ps=PS@409B60A0  rt0=surf@4218DC30  rt1=surf@4218DC70  stencil=off  t0=tex@4439F980 1024x1024 fmt12 ...
        constants: vs b0,b3,c46,b2,c57,... ps b2,c49,c50,...
draw 1  DrawIndexedPrimitive   triangles prims 64, indices 192, base vertex 0, start index 0, 6220 vertices
        decl=decl@4000A520  ib=IB@443A39F0  ps=PS@409B85A0  t0=tex@443AECC0 256x256 fmt13 ... vs=VS@409BFC60
```

- **Events** in order: render targets (each starts a new "pass"), depth buffer, viewport, clears, resolves, tiling,
  clip planes, constant loads.
- **Each draw** on one line, then the state that changed since the previous draw (all of it at the first draw of a
  pass), then the shader constant ranges written since the previous draw.
- **Resources** by type and guest address (`tex@4439F980`), so an object can be followed through the frame and
  between frames; textures with the size and format of their fetch constant.
- **A summary**: commands per opcode, draws per pass.
- **A verification**: while the frame is played, the hooks record what the handlers really pass to the draw
  functions (type, base vertex or start, start index or stride, count) and compare it with what the decoder
  predicted from the commands, draw by draw.

Results of the run above: the menu frames (33 draws) and four fight frames (342, 342, 357 and 359 draws) were all
decoded with **0 mismatches**. The check corrected op 29's layout on the way: w4 is D3D9's number of vertices, not
the start index.

What the fight frame shows: 48 passes for about 340 draws. A first pass at 320x180 with reversed depth (Z cleared to
0, "gequal") and a clip plane, then the main scene (one pass with 240-260 draws), then many one-draw passes
(post-processing, each followed by a resolve), and the HUD.

Not decoded yet: the surface header words after the type (printed raw in brackets: they look like the surface's
EDRAM layout, `05000140` = 1280 pixels wide), the vertex declarations' elements, and the shader microcode behind
each shader object.

## What this means for the native renderer

nfsmw-nx and masseffect-nx read the PM4 ring: the packets the XDK's Direct3D writes for the GPU, after all the
game's state has been flattened into registers. SF4 offers a level above that: its own command stream, with one
opcode per state change and per draw, played by one function on one thread. The plan that follows from the map:

1. **Name the opcodes** that matter (draws, shaders, constants, textures, render states, render targets, resolves,
   clears) from their handlers' code and arguments: done, [The opcodes](#the-opcodes). Not established yet: the
   exact meaning of op 55/56's mask and the callback table of op 3.
2. **Hook `sub_82287DF0`** and walk the same buffer, still calling the original: done, see [Step 2](#step-2-the-frame-decoder), verified
   against the draw functions' arguments rather than the PM4 they write. Before step 3: decode the surfaces and the vertex declarations, and identify each shader by its microcode (the library of translated
   shaders is keyed by it, as in the two other ports).
3. **Replace the handlers** opcode by opcode with Vulkan recording, as the native renderers of the two other ports
   do, with the shaders translated beforehand (XenosRecomp, as in their `shaders/`).

Measured on the console (user report, 2026-10-22): the character select screen ran at about 1 fps with the SDK's
Xenos emulation, the figure masseffect-nx measured for its menus (1.5-2.5 fps) before its native renderer. The plan
for the native renderer, from what masseffect-nx's can give:

- **Its core is reusable.** `masseffect-nx/app/src/native/me_native_system.cpp` is an IGraphicsSystem that replaces
  the Xenos plugin and reads the PM4 ring the XDK's Direct3D writes (SF4's too), and `native/masseffect/` records
  the draws in Vulkan (EDRAM, render targets, textures, pipelines). Of the 125 guest code addresses in `native/`,
  none is in that core: they are in the parts made for Mass Effect (native replacements of its game functions,
  resolution, audio, a scripted tour), which stay behind. Its 345 settings are named `masseffect_*`.
- **The shaders are easier than in Mass Effect.** SF4 has all of them in four EMB archives in `Shader/`: 379 XDK
  shader containers (228 pixel, 151 vertex; 366 different), extracted by `tools/extract_shaders.py`. The library
  is built from them with XenosRecomp and DXC (`tools/dxc`, v1.9.2609) as in the two other ports.
- **Drawing one frame in two** (`sf4_draw_every_n_frames`, default 2) already halves the work of whatever draws:
  the player hook does not play the buffers of the other frames (only their callbacks, op 3). Measured on the PC:
  the swap stays at 60 per second, the round timer runs at its normal speed, and the indexed draws per frame go
  from about 280 to 137.

Second console run (logs_run_2, after removing `readback_resolve = fast` and asking for the 460.8 MHz GPU): menus
about 35 fps, character select still 1.4 fps with the GPU at 99 %. The decoded frame shows why: the character
select's post-processing is a chain of small render targets that all start at the same EDRAM base, with 32
resolves per frame. Each time one of them takes the range over, the emulation's render target cache copies the
previous owner's contents into it (an ownership transfer: on the console about 41 render target changes and 141
copy draws per frame), although SF4 clears or fully overwrites each target before reading it. The SDK now has
`render_target_color_transfers` (`sdk/src/graphics/pipeline/render_target/cache.cpp`), which SF4 set to false.

Third console run (logs_run_3): character select 1.6-1.8 fps, still 17.5 render targets and 119 transfer draws per
frame. Those were the depth/stencil transfers: without `VK_EXT_shader_stencil_export` (NVK on the Switch) the
Vulkan backend copies depth with one draw and stencil with eight, one per bit
(`sdk/src/graphics/vulkan/render_target_cache.cpp`, `transfer_is_stencil_bit`). New cvar
`render_target_depth_transfers`; SF4 now sets it to false and keeps the color transfers on.

Lesson from the A/B: the PC build only has the D3D12 backend, whose default path on this machine is ROV, where
there are no transfers at all, so the first color A/B measured nothing. Test transfers on the PC with
`--render_target_path_d3d12=rtv` (the same common ownership code as the Switch's host render targets) and
`--render_target_transfer_stats=true` (a log line every 5 s with the copies made and skipped). With it, a fight
makes about 40 copies per drawn frame (6000 color + 5700 depth per 5 s). Results: skipping depth is clean (the
character select is pixel-identical, a whole round against Ken looks right); skipping color draws the fight HUD's
opponent portrait (top right) over leftovers, and skipping both smears a band on the right of the screen.
Fourth console run (logs_run_4, depth transfers off): character select 4.0-4.4 fps, fights 4.6 fps, fences waited
about 935 ms/s. Per frame: char select 89 draws, 50.7 screens of scissor area, 17 resolves, 26 color transfers;
fight 250 draws, 90 screens, 21 resolves. masseffect-nx measured the emulation's per-pixel paths at about
0.3 Gpixel/s on this GPU (`masseffect-nx/docs/gpu-cost-analysis.md`), so the cost follows the pixel count. SF4
sizes its buffers from the reported video mode: with `video_mode_width/height = 854/480` the frame decoder shows
854x480 viewports for the scene, 640x480 and 320x480 resolve targets for its full-size and half-size buffers (the
game's own SD sizes), and the fixed ones unchanged (1024x1024 and 512x512 shadow maps, 640x360, 256x180, 213x120
blur chain). Character select and a whole fight look right on the PC at 854x480 and at 640x360. The Switch build
defaults to 854x480 (`sf4_app.h`, `sf4.toml`).

Also: `build_pc.bat` relinks `sdk/out/win-amd64/rexgpu-xenos.dll` but does not always copy it next to `sf4.exe`;
copy it by hand after SDK graphics changes.

Until step 3 the game keeps drawing through the SDK's Xenos emulation, which works on the PC (this run held 55-60
frames per second) but not well on the Switch (see the README of the SD folder).
