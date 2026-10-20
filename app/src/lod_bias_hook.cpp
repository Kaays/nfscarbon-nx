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

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>

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
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("[lod] scene detail test active, nfscarbon_lod_bias = {:.2f}", bias);
  }
  if (bias < 1.0) {
    ctx.r3.u64 = uint32_t(int32_t(double(ctx.r3.s32) * bias));
  }
}

// ---- Car detail (nfscarbon-nx) -------------------------------------------------------------------
// The car renderer (sub_825A43A8, CarRenderInfo::Render) asks sub_822C9AE0 for the car's projected
// size in pixels (the same focal * radius / (distance - radius) as above), then picks the car's model
// LOD by counting the thresholds the size is below (80/40/20/10 px, or 120/25/20/10 for some cars),
// starting at the car's minimum LOD (+12492) and clamped to its maximum (+12496). At the start of Race
// Wars twenty-odd cars stand next to each other at full detail: 3500-5700 draws per frame. Scaling the
// size by nfscarbon_car_lod_bias < 1 for those two calls only (return addresses 0x825A442C and
// 0x825A4454) makes the other cars switch to their simpler models sooner; the player's car, right in
// front of the camera, stays far above the 80-120 px of the top level. Very far cars, under the
// renderer's own minimum size, also drop out a little sooner.

REXCVAR_DEFINE_DOUBLE(nfscarbon_car_lod_bias, 1.0, "Carbon",
                      "Scale of the projected car size the car renderer's LOD choice sees (1.0 = game "
                      "default; 0.6 = other cars switch to simpler models sooner, fewer draws)")
    .range(0.3, 1.0);

extern "C" void __imp__sub_822C9AE0(PPCContext& __restrict ctx, uint8_t* base);

extern "C" REX_FUNC(sub_822C9AE0) {
  const uint64_t ret = ctx.lr;
  const uint32_t view = ctx.r3.u32;
  __imp__sub_822C9AE0(ctx, base);
  if (ret != 0x825A442C && ret != 0x825A4454) {
    return;
  }
  const double bias = REXCVAR_GET(nfscarbon_car_lod_bias);
  static std::atomic<bool> logged{false};
  if (!logged.exchange(true, std::memory_order_relaxed)) {
    REXLOG_INFO("[lod] car detail test active, nfscarbon_car_lod_bias = {:.2f}", bias);
  }
  if (bias < 1.0) {
    const int32_t size = ctx.r3.s32;
    int32_t scaled = int32_t(double(size) * bias);
    // The renderer skips a car whose size is under the view's minimum (+44) + 10 (sub_825A43A8:
    // cmpw r25,r10 after addi r10,r10,10). Only the LOD choice should change: a car that was big
    // enough to be drawn stays big enough (the first test showed far cars as lone tail lights).
    const int32_t min_size = int32_t(REX_LOAD_U32(view + 44)) + 10;
    if (size >= min_size && scaled < min_size) {
      scaled = min_size;
    }
    ctx.r3.u64 = uint32_t(scaled);
  }
}

// ---- Diagnostic: draw no cars -----------------------------------------------------------------
// nfscarbon_debug_hide_cars = true skips the car renderer (sub_825A43A8) entirely, to measure how many
// of the draws of a crowded race start are cars. Not for playing.
REXCVAR_DEFINE_BOOL(nfscarbon_debug_hide_cars, false, "Carbon",
                    "Diagnostic: do not draw cars (measures their share of the draws)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" void __imp__sub_825A43A8(PPCContext& __restrict ctx, uint8_t* base);

extern "C" REX_FUNC(sub_825A43A8) {
  if (REXCVAR_GET(nfscarbon_debug_hide_cars)) {
    ctx.r3.u64 = 0;
    return;
  }
  __imp__sub_825A43A8(ctx, base);
}
