// SPDX-License-Identifier: GPL-3.0-or-later
// nfscarbon-nx: the game's audio mixer thread ("RWAudioCore Dac") above the other game threads.
//
// Console measurement (docs/measurements.md, session 8): in Race Wars the CPU is at 270-290 % of 300 and
// the host audio output asks for blocks the game has not mixed yet, 27 to 339 times per 10 s (0 in Canyon
// and the menu). That is the "slow motion", stuttering sound: the mixer, a game thread at 0x3B, only gets
// the CPU after everything else. The frame handoff threads now run at 0x3A (frame_handoff_hook.cpp), so
// they preempt it too.
//
// The thread is created by sub_826C6358 with the name at 0x820510B8 and the entry sub_826C6CD0 (a thunk to
// its loop, sub_826C6AB0). The hook raises the priority once, when the thread starts, and runs it as
// before. 0x39 is one step above the handoff threads; the SDK's own audio threads (XMA decoder, audio
// worker) are at 0x2B. Horizon does not time-slice below 0x3B, which is safe only because the mixer
// blocks on its own between blocks (it is at 40-65 % of a core).
//
// 0x2D since session 21 (nfsmw-nx's value for the same server): with XMA decoding on the calling
// thread (audio_xma_en_trabajador = 0) eight cold starts had no silent menu and no hang.
// Off in session 20: three starts went silent and the race never loaded with it at 0x39
// (the same failure as nfsmw-nx's audio server hang). audio_server_hook.cpp handles the late packets now.
// On (0x39) in session 18: with the spinlock fix the freeze is gone, and in Race Wars the
// silent game frames (blocks the game hands over empty, `huecos` in rex_perfil.log) went from 350-450
// to 130-250 per 10 s. The mixer itself costs 58 % of a core there, so the rest needs a cheaper mixer.
//
// 0x2A since session 22: Horizon only moves a woken thread to another core when that core is idle, so
// at 0x2D the mixer waited behind the renderer (0x2C) on its own core while another core ran a 0x3B game
// thread (each packet ~3.4 ms of CPU but ~5.5 ms of wall time in Race Wars). Above the renderer and the
// XMA worker it preempts whatever runs on its core. Console test (with stack sampling on): no hang,
// missing blocks in Race Wars 107-358 per 10 s vs 268-391 at 0x2D without sampling.
//
// Off by default in session 13: it did not reduce the missing blocks, and with the mixer at 0x39
// the game froze at the start of a race with the mixer at 100 % of a core, most likely spinning on a
// guest spinlock (xeKeKfAcquireSpinLock yielded, which on Horizon never lets a 0x3B owner run).

#include <cstdint>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include "../generated/default/nfscarbon_pch.h"

#if REX_PLATFORM_SWITCH
// sdk/src/core/threading_switch.cpp
extern "C" bool RexSwitchSetCurrentThreadPriorityOk(int priority);
#endif

REXCVAR_DEFINE_INT32(nfscarbon_audio_mixer_priority, 0x2A, "Carbon",
                     "Switch: Horizon priority of the game's audio mixer thread (RWAudioCore Dac). "
                     "0 = leave it at 0x3B like the rest of the game")
    .range(0, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" void __imp__sub_826C6CD0(PPCContext& __restrict ctx, uint8_t* base);

// Entry of the "RWAudioCore Dac" thread.
extern "C" REX_FUNC(sub_826C6CD0) {
#if REX_PLATFORM_SWITCH
  const int32_t priority = REXCVAR_GET(nfscarbon_audio_mixer_priority);
  if (priority >= 0x1C && priority <= 0x3B) {
    const bool ok = RexSwitchSetCurrentThreadPriorityOk(int(priority));
    REXLOG_INFO("[audio] mixer thread (RWAudioCore Dac) at priority {:#x} ({})", uint32_t(priority),
                ok ? "accepted" : "REFUSED by the kernel");
  }
#endif
  __imp__sub_826C6CD0(ctx, base);
}
