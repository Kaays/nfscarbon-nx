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
    # The game's audio server: wait for its packet before each audio frame, rescue it if it stalls.
    src/audio_server_hook.cpp
    # PGO: profile dump of the instrumented build (empty unless NFSCARBON_PGO=generate).
    src/nfscarbon_pgo.cpp
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
    # Profile-guided optimization in two builds from the same build folder (nfsmw-nx docs/toolchain.md):
    #   generate: instrumented, no LTO; the game writes the profile to sdmc:/switch/nfscarbon-nx/pgo every
    #             3 minutes (src/nfscarbon_pgo.cpp).
    #   use:      the .gcda files copied to pgo/ at the root of the repository; LTO on.
    # The generated code and the sources must not change between the two.
    set(NFSCARBON_PGO "" CACHE STRING "Profile-guided optimization: empty, generate or use")
    get_filename_component(NFSCARBON_PGO_DIR "${CMAKE_CURRENT_SOURCE_DIR}/../pgo" ABSOLUTE)
    if(NFSCARBON_PGO STREQUAL "generate")
        # -fno-profile-values: libgcov's indirect-call profiler reads TLS with mrs tpidr_el0, which is 0
        # on Horizon (libnx uses -mtp=soft): the instrumented build crashed before main in nfsmw-nx.
        set(NFSCARBON_PGO_OPTIONS -fprofile-generate=${NFSCARBON_PGO_DIR}
                                  "-fprofile-prefix-path=${CMAKE_BINARY_DIR}"
                                  -fprofile-update=single -fno-profile-values)
        target_link_options(nfscarbon PRIVATE -fprofile-generate -Wl,--wrap=fopen)
        target_compile_definitions(nfscarbon PRIVATE NFSCARBON_PGO_GENERATE=1
                                   "NFSCARBON_PGO_DIR=\"${NFSCARBON_PGO_DIR}\"")
        set(NFSCARBON_LTO OFF)
    elseif(NFSCARBON_PGO STREQUAL "use")
        # -fprofile-correction: counters updated by several threads without synchronization come out
        # negative, and GCC rejects them as a corrupt profile without it.
        set(NFSCARBON_PGO_OPTIONS -fprofile-use=${NFSCARBON_PGO_DIR}
                                  "-fprofile-prefix-path=${CMAKE_BINARY_DIR}"
                                  -fprofile-partial-training -fno-profile-values -fprofile-correction
                                  -Wno-missing-profile -Wno-error=coverage-mismatch)
        target_link_options(nfscarbon PRIVATE -fprofile-use -fprofile-partial-training -fprofile-correction)
    elseif(NOT NFSCARBON_PGO STREQUAL "")
        message(FATAL_ERROR "NFSCARBON_PGO must be empty, generate or use (it is '${NFSCARBON_PGO}')")
    endif()
    if(NFSCARBON_PGO_OPTIONS)
        foreach(target nfscarbon_recomp nfscarbon rexgpu-carbon)
            if(TARGET ${target})
                target_compile_options(${target} PRIVATE ${NFSCARBON_PGO_OPTIONS})
            endif()
        endforeach()
        message(STATUS "PGO: ${NFSCARBON_PGO} (profile folder ${NFSCARBON_PGO_DIR})")
    endif()
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

    # Hot functions grouped at the start of .text (function_order.ld, from console stack sampling in
    # Race Wars, Canyon and the menus; tools/hot_functions.py). Tens of MB of code against the A57's
    # 48 KB instruction cache and small TLB: scattered hot code walks the page tables on far jumps.
    # It only moves functions. Needs binutils 2.43+ (--section-ordering-file).
    option(NFSCARBON_FUNCTION_ORDER "Group the hot functions at the start of .text" ON)
    if(NFSCARBON_FUNCTION_ORDER AND EXISTS "${CMAKE_CURRENT_SOURCE_DIR}/function_order.ld")
        target_link_options(nfscarbon PRIVATE
            "-Wl,--section-ordering-file,${CMAKE_CURRENT_SOURCE_DIR}/function_order.ld")
        set_property(TARGET nfscarbon APPEND PROPERTY LINK_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/function_order.ld")
        # libnx's start-up code must stay at address 0 or the NRO does not boot (nfsmw-nx lost four
        # builds to this). Stop the build if the ordering moved it.
        add_custom_command(TARGET nfscarbon POST_BUILD
            COMMAND ${CMAKE_COMMAND} -DNM=${CMAKE_NM} -DELF=$<TARGET_FILE:nfscarbon>
                    -P ${CMAKE_CURRENT_SOURCE_DIR}/../tools/switch/cmake/check_start.cmake
            VERBATIM)
    endif()

    # Homebrew Menu icon: a 256x256 JPEG (original artwork, no game assets).
    # Without it the NRO gets libnx's default icon.
    # A personal icon in nfscarbon_icon_local.jpg (ignored by git, tools/make_icon.py) takes precedence.
    set(NFSCARBON_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../tools/switch/icon/nfscarbon_icon_local.jpg")
    if(NOT EXISTS "${NFSCARBON_ICON}")
        set(NFSCARBON_ICON "${CMAKE_CURRENT_SOURCE_DIR}/../tools/switch/icon/nfscarbon_icon.jpg")
    endif()
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
