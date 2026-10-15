// SPDX-License-Identifier: GPL-3.0-or-later
// nfscarbon-nx: platform settings of the Switch port.
//
// The Mesa/NVK environment and the nvdrv transfer memory size are adapted from
// nfsmw-nx/app/src/nfsmw_entorno_mesa.cpp, and the clock/overlay calls from
// nfsmw-nx/app/src/nfsmw_ajustes_graficos.cpp (StevensND, GPL-3.0). The SDK
// functions they call (RexSwitch*) live in sdk/src/ui/switch_*.cpp.

#include "nfscarbon_switch.h"

#include <cstdint>
#include <cstdlib>
#include <string>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

#if defined(NFSCARBON_STATIC_RENDERER)
#include "factory.h"
#endif

REXCVAR_DEFINE_STRING(nfscarbon_renderer, "carbon", "Carbon",
                      "Renderer: \"carbon\" (native Vulkan renderer) or \"xenos\" (Xenos emulation, "
                      "much slower; only to compare)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

REXCVAR_DEFINE_STRING(nfscarbon_mesa_env, "", "Carbon",
                      "Environment variables for Mesa/NVK set before Vulkan is created, as "
                      "\"VARIABLE=value;VARIABLE=value\" (empty = none)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// The renderer keeps its own VkPipelineCache (cache/carbon_gpu): Mesa's disk
// cache (sdmc:/.mesa) would be a second copy with writer threads of its own.
REXCVAR_DEFINE_BOOL(nfscarbon_mesa_disk_cache, false, "Carbon",
                    "Mesa's on-disk shader cache (sdmc:/.mesa). false = not created nor used")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

#if REX_PLATFORM_SWITCH
// libnx gives nvdrv 8 MB of transfer memory, where the bookkeeping of every GPU
// allocation lives. NFSMW died with error 0x235C (SharedMemoryTooSmall) at about
// 4,000 live allocations; the released MW build uses 32.
REXCVAR_DEFINE_INT32(nfscarbon_switch_nvmap_mb, 32, "Carbon",
                     "Switch: MB of the area libnx shares with nvdrv for GPU allocation bookkeeping "
                     "(libnx default 8; 0 = leave it)")
    .range(0, 256)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfscarbon_switch_gpu_mhz, 460, "Carbon",
                     "Switch: GPU MHz asked from the system in handheld mode (0 = leave it, 384, 460). "
                     "460.8 MHz is an official Horizon profile, not an overclock")
    .range(0, 1000);
REXCVAR_DEFINE_BOOL(nfscarbon_switch_ram_1600, false, "Carbon",
                    "Switch: allow the 1600 MHz memory profile in handheld mode");
REXCVAR_DEFINE_BOOL(nfscarbon_switch_saltynx, true, "Carbon",
                    "Switch: publish FPS and resolution for the console overlays (SaltyNX)");
REXCVAR_DEFINE_BOOL(nfscarbon_switch_reverse_clocks, false, "Carbon",
                    "Switch: let the clocks follow Reverse-NX when it fakes docked/handheld");

extern "C" uint32_t __nx_nv_transfermem_size;  // libnx, weak, 8 MB by default
extern "C" void RexSwitchPerfResolution(unsigned width, unsigned height);
extern "C" void RexSwitchSaltyNxHabilitar(int enabled);
extern "C" void RexSwitchRelojesReverseHabilitar(int enabled);
extern "C" void RexSwitchApmPedirGpuMhz(int mhz);
extern "C" void RexSwitchApmPermitirRam1600(int allow);
extern "C" void RexSwitchApmAplicar(void);
#endif

namespace nfscarbon::switch_port {
namespace {

std::string Trim(const std::string& text) {
  const size_t first = text.find_first_not_of(" \t\r\n");
  if (first == std::string::npos) {
    return {};
  }
  const size_t last = text.find_last_not_of(" \t\r\n");
  return text.substr(first, last - first + 1);
}

int SetEnv(const std::string& name, const std::string& value) {
#if defined(_WIN32)
  return _putenv_s(name.c_str(), value.c_str());
#else
  return setenv(name.c_str(), value.c_str(), 1);
#endif
}

void ApplyMesaEnvironment() {
  if (!REXCVAR_GET(nfscarbon_mesa_disk_cache)) {
    SetEnv("MESA_SHADER_CACHE_DISABLE", "true");
  }
  const std::string list = REXCVAR_GET(nfscarbon_mesa_env);
  size_t start = 0;
  while (start <= list.size()) {
    size_t end = list.find(';', start);
    if (end == std::string::npos) {
      end = list.size();
    }
    const std::string pair = Trim(list.substr(start, end - start));
    start = end + 1;
    if (pair.empty()) {
      continue;
    }
    const size_t eq = pair.find('=');
    if (eq == std::string::npos || eq == 0) {
      REXLOG_WARN("[mesa] '{}' is not VARIABLE=value, ignored", pair);
      continue;
    }
    const std::string name = Trim(pair.substr(0, eq));
    const std::string value = Trim(pair.substr(eq + 1));
    const int error = SetEnv(name, value);
    REXLOG_INFO("[mesa] env {}={}{}", name, value, error ? " (could not set it)" : "");
  }
}

#if REX_PLATFORM_SWITCH
void ApplySwitchSystemSettings() {
  const int32_t nvmap_mb = REXCVAR_GET(nfscarbon_switch_nvmap_mb);
  if (nvmap_mb > 0) {
    const uint32_t before = __nx_nv_transfermem_size;
    __nx_nv_transfermem_size = uint32_t(nvmap_mb) << 20;
    REXLOG_INFO("[switch] nvdrv transfer memory {} MB -> {} MB", before >> 20, nvmap_mb);
  }
  RexSwitchSaltyNxHabilitar(REXCVAR_GET(nfscarbon_switch_saltynx) ? 1 : 0);
  RexSwitchRelojesReverseHabilitar(REXCVAR_GET(nfscarbon_switch_reverse_clocks) ? 1 : 0);
  RexSwitchApmPedirGpuMhz(REXCVAR_GET(nfscarbon_switch_gpu_mhz));
  RexSwitchApmPermitirRam1600(REXCVAR_GET(nfscarbon_switch_ram_1600) ? 1 : 0);
  RexSwitchApmAplicar();

  // Resolution shown by the overlays: the game's video mode times the
  // renderer's scale.
  const unsigned width = unsigned(std::strtoul(rex::cvar::GetFlagByName("video_mode_width").c_str(), nullptr, 10));
  const unsigned height = unsigned(std::strtoul(rex::cvar::GetFlagByName("video_mode_height").c_str(), nullptr, 10));
  const unsigned scale =
      unsigned(std::strtoul(rex::cvar::GetFlagByName("carbon_gpu_render_scale").c_str(), nullptr, 10));
  if (width && height && scale) {
    RexSwitchPerfResolution(width * scale / 100, height * scale / 100);
  }
}
#endif

}  // namespace

bool NativeRendererActive() {
  return REXCVAR_GET(nfscarbon_renderer) != "xenos";
}

std::unique_ptr<rex::system::IGraphicsSystem> CreateNativeRenderer() {
#if defined(NFSCARBON_STATIC_RENDERER)
  return carbon::gpu::CreateGpuSystem();
#else
  return nullptr;  // PC: the renderer is the rexgpu-carbon plugin (gpu_plugin = "carbon")
#endif
}

void ApplyEarlySettings() {
  ApplyMesaEnvironment();
#if REX_PLATFORM_SWITCH
  ApplySwitchSystemSettings();
#endif
}

}  // namespace nfscarbon::switch_port
