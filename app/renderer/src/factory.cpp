// Carbon native renderer: direct construction (see factory.h).

#include "factory.h"

#include "gpu_system.h"

namespace carbon::gpu {

std::unique_ptr<rex::system::IGraphicsSystem> CreateGpuSystem() {
  return std::make_unique<GpuSystem>();
}

}  // namespace carbon::gpu
