# nfscarbon-nx - Need for Speed: Carbon (Xbox 360) on the Nintendo Switch
#
# The game is nfs-carbon-recomp (Sampreeth004): its manifest, its hooks and its
# native Vulkan renderer. The Switch side is nfsmw-nx (StevensND): the ReXGlue
# SDK with the Horizon layer (../sdk), the NVK driver (../mesa) and the
# devkitA64 toolchain (../tools/switch).
#
# SDK boilerplate lives in generated/rexglue.cmake (written by the code
# generator, see ../tools/codegen.sh).

cmake_minimum_required(VERSION 3.25)
project(nfscarbon LANGUAGES CXX)

set(CMAKE_CXX_STANDARD 23)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

if(NOT EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/generated/rexglue.cmake")
    message(FATAL_ERROR
        "app/generated/rexglue.cmake is missing: run the code generator first "
        "(REXGLUE=out/host/rexglue tools/codegen.sh app, see README.md).")
endif()
include(generated/rexglue.cmake)

set(NFSCARBON_SOURCES
    src/main.cpp
    # Unregistered indirect calls are logged and skipped instead of fatal.
    src/dispatch_tolerance.cpp
    # Retail render mode 2 for the scene: one tile, no MSAA (cvar carbon_single_pass).
    src/render_mode_hook.cpp
    # Renderer choice, Mesa/NVK environment, nvdrv memory, GPU clock and overlays.
    src/nfscarbon_switch.cpp
    # The game's two render threads sleep until the frame flag changes (no Sleep(0) loops).
    src/frame_handoff_hook.cpp
    # The game's audio mixer thread above the other game threads.
    src/audio_thread_hook.cpp
)

# Native Vulkan renderer (renderer/): an OBJECT library linked into the NRO on
# the Switch, the rexgpu-carbon plugin elsewhere.
add_subdirectory(renderer)

if(REXGLUE_PLATFORM_SWITCH)
    # The game's D3D sleeps instead of spinning while it waits for ring space.
    # It calls into the renderer directly, so only where the renderer is linked in.
    list(APPEND NFSCARBON_SOURCES src/ring_wait_hook.cpp)
endif()

if(WIN32)
    add_executable(nfscarbon WIN32 ${NFSCARBON_SOURCES})
    target_link_options(nfscarbon PRIVATE "LINKER:/force:multiple")
else()
    add_executable(nfscarbon ${NFSCARBON_SOURCES})
endif()

# The Xenos emulation stays available as a fallback (nfscarbon_renderer = "xenos").
rexglue_setup_target(nfscarbon GPU_PLUGINS xenos)

target_include_directories(nfscarbon PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/renderer/src)

if(REXGLUE_PLATFORM_SWITCH)
    target_link_libraries(nfscarbon PRIVATE rexgpu-carbon glslang glslang::SPIRV xxhash)
    target_compile_definitions(nfscarbon PRIVATE NFSCARBON_STATIC_RENDERER=1)
else()
    add_dependencies(nfscarbon rexgpu-carbon)
endif()

if(REXGLUE_PLATFORM_SWITCH)
    # LTO over the game code and the app, as nfsmw-nx does (MW: 144 MB of
    # generated C++ needs -flto=2 to fit in 16 GB of RAM). Run
    # tools/direct_calls.py after codegen so calls between unhooked game
    # functions can be inlined across files.
    option(NFSCARBON_LTO "LTO over the game code and the app (Switch)" ON)
    if(NFSCARBON_LTO)
        foreach(target nfscarbon_recomp nfscarbon rexgpu-carbon)
            if(TARGET ${target})
                target_compile_options(${target} PRIVATE -flto -fno-fat-lto-objects)
            endif()
        endforeach()
        target_link_options(nfscarbon PRIVATE -flto=2 -flto-partition=balanced)
    endif()

    # A/B: -O2 gives smaller code than the default -O3 (the A57 has a 48 KB
    # instruction cache). Off by default, as in nfsmw-nx.
    option(NFSCARBON_RECOMP_O2 "Compile the translated code with -O2 instead of -O3" OFF)
    if(NFSCARBON_RECOMP_O2 AND TARGET nfscarbon_recomp)
        target_compile_options(nfscarbon_recomp PRIVATE -O2)
    endif()

    # Homebrew Menu icon: a 256x256 JPEG. Not distributed; without it the NRO
    # gets libnx's default icon.
    set(NFSCARBON_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../tools/switch/icon/nfscarbon_icon.jpg")
    set(NFSCARBON_ICON_ARG)
    if(EXISTS "${NFSCARBON_ICON}")
        set(NFSCARBON_ICON_ARG ICON "${NFSCARBON_ICON}")
    endif()
    rexglue_switch_add_nro(nfscarbon
        NAME "Need for Speed Carbon"
        AUTHOR "nfscarbon-nx"
        VERSION "0.1.0"
        ${NFSCARBON_ICON_ARG})
endif()
