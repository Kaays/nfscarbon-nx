// SPDX-License-Identifier: GPL-3.0-or-later
// nfscarbon-nx: optional scene detail trim (quality for speed), ported from zofo007's NFS-Carbon-NX
// (src/lod_bias_hook.cpp, GPL-3.0, same USA default.xex).
//
// sub_82586908, the render thread's per-instance test (called once per scene object per view), asks
// sub_82586288 for the object's projected size in pixels (focal * radius / (distance - radius), 0 when
// it is behind the view plane). It drops objects under ~18 px and picks the model LOD with the same
// size thresholds. Scaling that size by nfscarbon_lod_bias < 1 makes objects look smaller to the test:
// small far props leave the scene sooner and lower LODs start sooner, so a crowded scene (the start of
// Race Wars: 3500-5700 draws per frame) submits fewer draws, which saves CPU in the game's render
// thread and in the renderer alike. 1.0 = the game's own choice. sub_82586288 has a single caller.

#include <cstdint>

#include <rex/cvar.h>

#include "../generated/default/nfscarbon_pch.h"

REXCVAR_DEFINE_DOUBLE(nfscarbon_lod_bias, 1.0, "Carbon",
                      "Scale of the projected object size the game's visibility/LOD test sees "
                      "(1.0 = game default; 0.8 = objects drop out or switch to a lower LOD sooner, "
                      "fewer draws)")
    .range(0.3, 1.0);

extern "C" void __imp__sub_82586288(PPCContext& __restrict ctx, uint8_t* base);

extern "C" REX_FUNC(sub_82586288) {
  __imp__sub_82586288(ctx, base);
  const double bias = REXCVAR_GET(nfscarbon_lod_bias);
  if (bias < 1.0) {
    ctx.r3.u64 = uint32_t(int32_t(double(ctx.r3.s32) * bias));
  }
}
