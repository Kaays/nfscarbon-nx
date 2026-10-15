// Carbon native renderer: direct construction, for builds that link the
// renderer into the executable instead of loading it as a GPU plugin (Switch).
#pragma once

#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace carbon::gpu {

// The same graphics system rex_gpu_create returns from the plugin.
std::unique_ptr<rex::system::IGraphicsSystem> CreateGpuSystem();

}  // namespace carbon::gpu
