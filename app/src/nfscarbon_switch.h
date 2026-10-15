// SPDX-License-Identifier: GPL-3.0-or-later
// nfscarbon-nx: platform settings of the Switch port (nfscarbon_switch.cpp).
#pragma once

#include <memory>

#include <rex/system/interfaces/graphics.h>

namespace nfscarbon::switch_port {

// nfscarbon_renderer: "carbon" (the native renderer from nfs-carbon-recomp,
// default) or "xenos" (the SDK's Xenos emulation, slow, for comparison).
bool NativeRendererActive();

// The native renderer's graphics system, for RuntimeConfig::graphics.
std::unique_ptr<rex::system::IGraphicsSystem> CreateNativeRenderer();

// Mesa/NVK environment, nvdrv transfer memory, GPU clock profile and console
// overlays. Call from OnPostInitLogging: the toml has been read and Vulkan is
// created later, in SetupPresentation.
void ApplyEarlySettings();

}  // namespace nfscarbon::switch_port
