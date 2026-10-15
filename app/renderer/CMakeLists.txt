# rexgpu-carbon: native Vulkan renderer for NFS Carbon (no Xenos emulation).
#
# From nfs-carbon-recomp/renderer (Sampreeth004). The sources are shared between
# platforms; only the way they are linked changes:
#
#   PC / Linux   rexgpu-carbon is a shared library loaded through the SDK GPU
#                plugin seam (gpu_plugin = "carbon"), as in nfs-carbon-recomp.
#   Switch       an NRO cannot load code at run time. The renderer is an OBJECT
#                library linked straight into the title (so every REXCVAR in it
#                registers itself, which an archive member nobody references
#                would not), and the app creates carbon::gpu::GpuSystem itself in
#                OnPreSetup (config.graphics). plugin_main.cpp is left out: it
#                defines rex_gpu_create, which the statically linked Xenos plugin
#                (the fallback renderer) already defines.

set(CARBON_GPU_COMMON_SOURCES
    src/gpu_system.cpp
    src/command_processor.cpp
    src/shader_translator.cpp
    src/shader_compiler.cpp
    src/renderer.cpp
    src/render_targets.cpp
    src/textures.cpp
    src/texture_decode.cpp
    src/factory.cpp
)

if(REXGLUE_PLATFORM_SWITCH)
    add_library(rexgpu-carbon OBJECT ${CARBON_GPU_COMMON_SOURCES})
    target_compile_definitions(rexgpu-carbon PUBLIC NFSCARBON_STATIC_RENDERER=1)
else()
    add_library(rexgpu-carbon SHARED ${CARBON_GPU_COMMON_SOURCES} src/plugin_main.cpp)
    set_target_properties(rexgpu-carbon PROPERTIES
        CXX_VISIBILITY_PRESET hidden
        VISIBILITY_INLINES_HIDDEN ON
        OUTPUT_NAME "rexgpu-carbon")
endif()

target_compile_features(rexgpu-carbon PRIVATE cxx_std_23)

# glslang's public headers (<glslang/Public/ShaderLang.h>, <SPIRV/GlslangToSpv.h>)
# live at the root of the SDK's glslang checkout.
target_include_directories(rexgpu-carbon PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}/src
    ${REXSDK_DIR}/thirdparty/glslang
    ${REXSDK_DIR}/thirdparty/xxHash
)

target_link_libraries(rexgpu-carbon PRIVATE rex::runtime xxhash glslang glslang::SPIRV)

if(NOT REXGLUE_PLATFORM_SWITCH AND TARGET renderdoc)
    target_link_libraries(rexgpu-carbon PRIVATE renderdoc)
endif()

if(WIN32)
    # Next to nfscarbon.exe, where LoadGpuPlugin looks.
    set_target_properties(rexgpu-carbon PROPERTIES
        RUNTIME_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}"
        LIBRARY_OUTPUT_DIRECTORY "${CMAKE_BINARY_DIR}")
    target_compile_definitions(rexgpu-carbon PRIVATE NOMINMAX WIN32_LEAN_AND_MEAN)
endif()

if(ANDROID)
    target_link_libraries(rexgpu-carbon PRIVATE log)
endif()
