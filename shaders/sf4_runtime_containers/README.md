# Direct3D runtime shaders of SF4

Three vertex shader containers that SF4's Direct3D builds at run time (immediate-mode draws), not stored on the disc
or in `default.xex`: they were captured from the running game (`MASSEFFECT_SHADER_MISSING`, the `assets/shaders_runtime`
input of `tools/build_shader_library.py`). They are Microsoft's Direct3D shaders, like masseffect-nx's set in
`shaders/me/runtime_containers`, and both sets go into the shader library; the installer page gets them as
`wasm/runtime_containers.json` (installer/tools/stage_site.sh).
