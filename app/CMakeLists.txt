# sf4 - ReXGlue Recompiled Project (Street Fighter IV, Xbox 360)
#
# SDK boilerplate lives in generated/rexglue.cmake.

cmake_minimum_required(VERSION 3.25)
project(sf4 LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(generated/rexglue.cmake)

set(SF4_SOURCES
    src/main.cpp
    # Automated tests: virtual pad driven by a script (sf4_test_buttons).
    src/sf4_test_input.cpp
    # PNG captures of the game image for tests (sf4_capture_every_s).
    src/sf4_capture.cpp
    # The game's command stream: opcodes, walker, and the hook on its player sub_82287DF0 (docs/d3d-map.md).
    src/sf4_commands.cpp
    # Controller vibration on although SF4's default button configuration has it off (sf4_vibration_always).
    src/sf4_rumble.cpp
    # Idle job workers give the CPU back (sf4_job_wait_yield): 5 pollers on 3 cores starved the main thread.
    src/sf4_jobs.cpp
    # 30 frames per second on screen with the game at full speed (sf4_frame_rate).
    src/sf4_framerate.cpp
    # 3D scene at 480p, menus, movies and HUD at 720p (sf4_scene_480p).
    src/sf4_resolution.cpp
    # Readable dumps of whole frames of that stream (sf4_frame_dump; off by default).
    src/sf4_frame_decode.cpp
)

if(WIN32)
    add_executable(sf4 WIN32 ${SF4_SOURCES})
else()
    add_executable(sf4 ${SF4_SOURCES})
endif()

# Phase A of the native renderer: wraps all 540 functions of the game's Direct3D and writes rex_d3d.log
# (cvar sf4_d3d_trace, off by default). PC only: on the Switch tools/direct_calls.py makes most calls into the
# library direct, so the wrappers would miss them.
if(REXGLUE_PLATFORM_SWITCH)
    set(SF4_D3D_TRACE_DEFAULT OFF)
else()
    set(SF4_D3D_TRACE_DEFAULT ON)
endif()
option(SF4_D3D_TRACE "Build src/sf4_d3d_trace.cpp (Direct3D call map, rex_d3d.log)" ${SF4_D3D_TRACE_DEFAULT})
if(SF4_D3D_TRACE)
    target_sources(sf4 PRIVATE src/sf4_d3d_trace.cpp)
endif()

# The SDK's Xenos emulation, the fallback renderer (masseffect_renderer_native = false).
rexglue_setup_target(sf4 GPU_PLUGINS xenos)

# The native renderer, from masseffect-nx (src/native, docs/native-renderer.md): reads the Direct3D command ring and
# draws with Vulkan, shaders from a library built beforehand (tools/build_shader_library.py, sf4_shaders.mesp next
# to the executable). Needs the Vulkan backend of the SDK (REXGLUE_USE_VULKAN, always on the Switch).
option(SF4_NATIVE_RENDERER "Build the native renderer (src/native)" ON)
if(SF4_NATIVE_RENDERER)
    target_sources(sf4 PRIVATE
        src/native/me_native_system.cpp
        src/native/me_heap_report.cpp
        src/native/sf4_native_glue.cpp
        # The game's Direct3D GPU wait sleeps instead of spinning (sub_82216850).
        src/native/sf4_ring_wait.cpp
        src/native/masseffect/masseffect_native_draws.cpp
        src/native/masseffect/masseffect_deferred_recording.cpp
        src/native/masseffect/masseffect_native_targets.cpp
        src/native/masseffect/masseffect_native_shaders.cpp
        src/native/masseffect/masseffect_native_textures_pool.cpp
        src/native/masseffect/masseffect_shader_library.cpp
        src/native/masseffect/me_masseffect_glue.cpp
        # CPU-only SDK sources the renderer uses (draw extents, shader microcode); the GPU plugin's copies are
        # private to it.
        "${REXSDK_DIR}/src/graphics/register_file.cpp"
        "${REXSDK_DIR}/src/graphics/format/ucode.cpp"
        "${REXSDK_DIR}/src/graphics/pipeline/shader/interpreter.cpp")
    target_compile_definitions(sf4 PRIVATE SF4_NATIVE_RENDERER=1)
    # SPIR-V initializers of the renderer's EDRAM and resolve shaders: tools/build_native_shaders.sh (glslang).
    set(SF4_NATIVE_SHADER_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../out/native-shaders" CACHE PATH
        "Output of tools/build_native_shaders.sh")
    if(NOT EXISTS "${SF4_NATIVE_SHADER_DIR}/me_edram_import.inc")
        message(FATAL_ERROR "Run tools/build_native_shaders.sh first (${SF4_NATIVE_SHADER_DIR})")
    endif()
    target_include_directories(sf4 PRIVATE
        "${SF4_NATIVE_SHADER_DIR}"
        src
        src/native
        src/native/masseffect
        src/native/masseffect/shaders
        "${REXSDK_DIR}/src/core"
        "${REXSDK_DIR}/src/ui/shaders"
        "${REXSDK_DIR}/thirdparty/xxHash")
    if(NOT CMAKE_SYSTEM_PROCESSOR MATCHES "^(aarch64|arm64|ARM64)$" AND NOT REXGLUE_PLATFORM_SWITCH)
        target_include_directories(sf4 PRIVATE src/native/x86 "${REXSDK_DIR}/thirdparty/simde")
    endif()
endif()

# As in masseffect-nx: the SDK compiles itself with these (directory scope), but they do not reach the generated
# game code, which is built in this directory. GCC's default for GNU C++ is -ffp-contract=fast: it fuses separate
# PPC fmul/fadd into aarch64 FMA and results drift from the Xbox 360 (a fighting game's simulation must not).
if(CMAKE_CXX_COMPILER_ID STREQUAL "GNU")
    foreach(target sf4 sf4_recomp)
        if(TARGET ${target})
            target_compile_options(${target} PRIVATE -ffp-contract=off -fno-strict-aliasing)
        endif()
    endforeach()
endif()

# PC build with the SDK as a subdirectory: rex_app.cpp includes imgui.h (and the Vulkan headers
# renderdoc_app.h) but rexruntime links them privately. On Switch rexglue_switch.cmake handles it.
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET imgui)
    target_include_directories(sf4 PRIVATE $<TARGET_PROPERTY:imgui,INTERFACE_INCLUDE_DIRECTORIES>)
endif()
if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_include_directories(sf4 PRIVATE $<TARGET_PROPERTY:renderdoc,INTERFACE_INCLUDE_DIRECTORIES>)
endif()

# Nintendo Switch: sf4.nro next to the ELF. On the SD card it goes in sdmc:/switch/sf4-nx/
# with the game_root/ folder extracted beside it (src/sf4_app.h looks for it there).
if(REXGLUE_PLATFORM_SWITCH)
    # LTO on the game code and ours, as in nfsmw-nx: -flto=2 keeps the whole-program phase within 16 GB.
    option(SF4_LTO "LTO en el codigo del juego y de la app (Switch)" OFF)
    if(SF4_LTO)
        foreach(objetivo sf4_recomp sf4)
            if(TARGET ${objetivo})
                target_compile_options(${objetivo} PRIVATE -flto -fno-fat-lto-objects)
            endif()
        endforeach()
        target_link_options(sf4 PRIVATE -flto=2 -flto-partition=balanced)
    endif()
    # Homebrew menu icon: a 256x256 JPEG. Without it the NRO gets the default libnx icon.
    set(SF4_ICONO "${CMAKE_CURRENT_SOURCE_DIR}/../tools/switch/icono/sf4_icono.jpg")
    set(SF4_ICONO_ARG)
    if(EXISTS "${SF4_ICONO}")
        set(SF4_ICONO_ARG ICON "${SF4_ICONO}")
    endif()
    rexglue_switch_add_nro(sf4
        NAME "Street Fighter IV"
        AUTHOR "sf4-nx"
        VERSION "0.1.0"
        ${SF4_ICONO_ARG})
endif()
