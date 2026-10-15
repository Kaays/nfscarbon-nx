// SPDX-License-Identifier: GPL-3.0-or-later
// nfscarbon-nx: sleep instead of spin while the game's D3D waits for ring space.
//
// Adapted from nfs-carbon-recomp/src/ring_wait_hook.cpp (Android, futex + dlsym),
// itself ported from NFSMW-Recompiled-Mobile and, before that, the same idea as
// nfsmw-nx/app/src/nfsmw_espera_anillo.cpp (StevensND).
//
// When the GPU command ring is full, Carbon's D3D waits for the command processor
// (CP) to advance its read pointer by spinning: sub_826DBD78 and sub_826DBB80
// reread the pointer in a loop and call the hang watchdog sub_826E0250 each time
// (it returns 0 once the pointer has not moved for 5 s). The Switch gives the
// game three slow cores; a spinning main thread steals one from the CP thread,
// which is the one that sets the frame rate.
//
// Hook: if the read pointer has not moved since the watchdog's last look, sleep
// until the native renderer publishes a new one (nfscarbon_cp_wait_rptr, in
// renderer/src/command_processor.cpp), capped at nfscarbon_ring_wait_max_us.
// Then call the original, which decides exactly as before.
//
// Only for the two ring-space loops, recognized by return address. The other
// watchdog callers wait on fences the CP writes mid-stream; sleeping there would
// add latency. Does nothing with the Xenos fallback renderer (it does not
// publish the sequence, so every wait would just time out).
//
// Addresses are for the XEX nfs-carbon-recomp was derived from (see
// docs/carbon/render-mode-addresses.md): another edition needs them re-derived
// with tools/find_fingerprints.py.

#include <atomic>
#include <cstdint>

#include <rex/cvar.h>

#include "../generated/default/nfscarbon_pch.h"
#include "nfscarbon_switch.h"

REXCVAR_DEFINE_BOOL(nfscarbon_ring_wait, true, "Carbon",
                    "The game's D3D sleeps while it waits for ring space instead of spinning "
                    "(native renderer only)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfscarbon_ring_wait_max_us, 1000, "Carbon",
                     "Longest sleep per watchdog call of the ring-space wait, in microseconds")
    .range(100, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

// renderer/src/command_processor.cpp
extern "C" std::atomic<uint32_t> nfsmw_cp_rptr_seq;
extern "C" bool nfscarbon_cp_wait_rptr(uint32_t before, uint32_t timeout_us);

extern "C" void __imp__sub_826E0250(PPCContext& __restrict ctx, uint8_t* base);

namespace {

// Return addresses of the watchdog calls in the two ring-space wait loops.
constexpr uint32_t kRingWaitReturn1 = 0x826DBE14;  // sub_826DBD78
constexpr uint32_t kRingWaitReturn2 = 0x826DBBF8;  // sub_826DBB80

// Offset in the D3D device of the pointer to the CP read pointer writeback
// (lwz rX,10768(rDevice) in both loops; 10384 in NFSMW).
constexpr uint32_t kDeviceRptrPointer = 10768;

bool Enabled() {
  static const bool enabled =
      REXCVAR_GET(nfscarbon_ring_wait) && nfscarbon::switch_port::NativeRendererActive();
  return enabled;
}

}  // namespace

extern "C" REX_FUNC(sub_826E0250) {
  const uint32_t ret = uint32_t(ctx.lr);
  if ((ret == kRingWaitReturn1 || ret == kRingWaitReturn2) && Enabled()) {
    // Sequence first, then the pointer: if the CP publishes in between, the
    // sequence has already changed and the wait returns immediately.
    const uint32_t before = nfsmw_cp_rptr_seq.load(std::memory_order_acquire);
    // r3 points at the watchdog state: { device, wait kind, seen rptr, ... }.
    const uint32_t state = ctx.r3.u32;
    const uint32_t device = REX_LOAD_U32(state + 0);
    const uint32_t seen = REX_LOAD_U32(state + 8);
    const uint32_t rptr = REX_LOAD_U32(REX_LOAD_U32(device + kDeviceRptrPointer));
    if (rptr == seen) {
      nfscarbon_cp_wait_rptr(before, uint32_t(REXCVAR_GET(nfscarbon_ring_wait_max_us)));
    }
  }
  __imp__sub_826E0250(ctx, base);
}
