// SPDX-License-Identifier: GPL-3.0-or-later
// nfscarbon-nx: frame handoff between the game's two render threads without Sleep(0) loops.
//
// Port of nfsmw-nx/app/src/nfsmw_espera_fotograma.cpp (StevensND). Carbon kept Most Wanted's
// scheme, with new addresses:
//
//   flag 0x82C651C4: 1 = a command list is ready, 0 = it has been executed.
//   - Preparer, sub_824E9948: waits with Sleep(0) (return 0x824E99E4) while the flag is 1.
//     Then sub_824E9A80 sets it to 1 and calls sub_824F0210, which fills the list 0x82B8C764.
//   - Executor, sub_824E9720: waits with Sleep(0) (return 0x824E977C) while the flag is 0,
//     runs the list with sub_824694C0 (return 0x824E981C), sets the flag to 0 and calls
//     sub_829439E0 (the preparer calls it too).
//   Sleep(ms) is sub_82943548 (li r4,0; b SleepEx). Those are the only two Sleep(0) calls in the game.
//
// On the Xbox 360 each thread had its own hardware thread and yielding cost nothing. Here the game
// gets 3 cores and runs at 280-295 % of 300 in Race Wars: a thread spinning on Sleep(0) takes CPU
// from the audio mixer and the renderer. MW measured 41 % of the preparer in those yields.
//
// What it does, as in nfsmw-nx:
//   1. Those two Sleep(0) calls sleep until the flag changes (notified on entry to sub_824F0210 and
//      sub_829439E0), at most nfscarbon_handoff_max_us. The game rechecks the flag, so an extra
//      notification changes nothing and a lost one costs at most the timeout.
//   2. While the list is open, not closed and has no new commands, the executor calls sub_824694C0 with
//      0 commands in a loop without sleeping; it now sleeps in short pauses instead.
//   3. Both threads go to priority 0x3A (one step above the other game threads, 0x3B), so that when
//      they wake up they do not wait for a 10 ms time slice. Both block hundreds of times per second.
//
// Every hook calls the original afterwards: the game's own checks decide, exactly as before.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <mutex>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>
#include <rex/thread.h>

#include "../generated/default/nfscarbon_pch.h"

#if REX_PLATFORM_SWITCH
// sdk/src/core/threading_switch.cpp (its header pulls in the SDK's newlib shims).
extern "C" bool RexSwitchSetCurrentThreadPriorityOk(int priority);
#endif

REXCVAR_DEFINE_BOOL(nfscarbon_handoff_sleep, true, "Carbon",
                    "The game's two render threads sleep until the frame flag changes instead of "
                    "looping on Sleep(0)")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfscarbon_handoff_max_us, 1000, "Carbon",
                     "Longest sleep per Sleep(0) of the frame handoff, in microseconds")
    .range(100, 100000)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_BOOL(nfscarbon_executor_no_spin, true, "Carbon",
                    "The executor sleeps in short pauses while the command list is still being "
                    "written, instead of calling the list runner with 0 commands in a loop")
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);
REXCVAR_DEFINE_INT32(nfscarbon_executor_pause_us, 100, "Carbon",
                     "Pause of the executor while it waits for commands (us)");
REXCVAR_DEFINE_INT32(nfscarbon_executor_wait_max_us, 2000, "Carbon",
                     "Longest wait of the executor for commands before the game's loop rechecks (us)");
REXCVAR_DEFINE_INT32(nfscarbon_handoff_priority, 0x3A, "Carbon",
                     "Switch: Horizon priority of the two handoff threads. 0 = leave them at 0x3B "
                     "like the rest of the game")
    .range(0, 0x3B)
    .lifecycle(rex::cvar::Lifecycle::kInitOnly);

extern "C" void __imp__sub_82943548(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_824694C0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_824F0210(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_829439E0(PPCContext& __restrict ctx, uint8_t* base);

namespace {

constexpr uint32_t kFlag = 0x82C651C4;
constexpr uint32_t kPreparerReturn = 0x824E99E4;  // Sleep(0) in sub_824E9948: waits while flag == 1
constexpr uint32_t kExecutorReturn = 0x824E977C;  // Sleep(0) in sub_824E9720: waits while flag == 0
constexpr uint32_t kCommandsReturn = 0x824E981C;  // sub_824694C0 in sub_824E9720's list loop

std::mutex g_mutex;
std::condition_variable g_cv;
std::atomic<uint32_t> g_changes{0};
std::atomic<int> g_waiting{0};

struct Counters {
  std::atomic<uint64_t> waits{0}, notified{0}, ns{0}, worst_ns{0};
};
Counters g_preparer, g_executor;
std::atomic<uint64_t> g_nocmd_waits{0}, g_nocmd_ns{0};
std::atomic<int64_t> g_next_report_ms{0};

int64_t NowNs() {
  return std::chrono::duration_cast<std::chrono::nanoseconds>(
             std::chrono::steady_clock::now().time_since_epoch())
      .count();
}

uint32_t Load32(const uint8_t* base, uint32_t address) {
  return REX_LOAD_U32(address);
}

void Max(std::atomic<uint64_t>& target, uint64_t value) {
  uint64_t prev = target.load(std::memory_order_relaxed);
  while (value > prev && !target.compare_exchange_weak(prev, value, std::memory_order_relaxed)) {
  }
}

void RaisePriorityOnce(bool& done, const char* role) {
  if (done) {
    return;
  }
  done = true;
#if REX_PLATFORM_SWITCH
  const int32_t priority = REXCVAR_GET(nfscarbon_handoff_priority);
  if (priority >= 0x1C && priority <= 0x3B) {
    const bool ok = RexSwitchSetCurrentThreadPriorityOk(int(priority));
    REXLOG_INFO("[handoff] {} thread at priority {:#x} ({})", role, uint32_t(priority),
                ok ? "accepted" : "REFUSED by the kernel");
    return;
  }
#endif
  REXLOG_INFO("[handoff] {} thread: priority left as is", role);
}

// The waiter count is read under the lock: otherwise a notification can be lost and cost the timeout.
void Notify() {
  g_changes.fetch_add(1, std::memory_order_acq_rel);
  bool any;
  {
    std::lock_guard<std::mutex> lock(g_mutex);
    any = g_waiting.load(std::memory_order_acquire) > 0;
  }
  if (any) {
    g_cv.notify_all();
  }
}

// Sleeps while the flag is still 'value', until notified or at most the timeout.
void Wait(const uint8_t* base, uint32_t value, Counters& c) {
  const uint32_t seen = g_changes.load(std::memory_order_acquire);
  if (Load32(base, kFlag) != value) {
    c.waits.fetch_add(1, std::memory_order_relaxed);
    c.notified.fetch_add(1, std::memory_order_relaxed);
    return;
  }
  const int64_t start = NowNs();
  bool notified;
  {
    std::unique_lock<std::mutex> lock(g_mutex);
    g_waiting.fetch_add(1, std::memory_order_acq_rel);
    notified = g_cv.wait_for(lock, std::chrono::microseconds(REXCVAR_GET(nfscarbon_handoff_max_us)),
                             [base, value, seen] {
                               return g_changes.load(std::memory_order_acquire) != seen ||
                                      Load32(base, kFlag) != value;
                             });
    g_waiting.fetch_sub(1, std::memory_order_acq_rel);
  }
  const uint64_t ns = uint64_t(NowNs() - start);
  c.waits.fetch_add(1, std::memory_order_relaxed);
  if (notified) {
    c.notified.fetch_add(1, std::memory_order_relaxed);
  }
  c.ns.fetch_add(ns, std::memory_order_relaxed);
  Max(c.worst_ns, ns);
}

void Report() {
  const int64_t now = NowNs() / 1000000;
  int64_t next = g_next_report_ms.load(std::memory_order_relaxed);
  if (now < next) {
    return;
  }
  if (!g_next_report_ms.compare_exchange_strong(next, now + 10000) || next == 0) {
    return;
  }
  auto line = [](Counters& c) {
    return fmt::format("{} waits ({} notified, {:.1f} ms asleep, worst {:.2f} ms)", c.waits.exchange(0),
                       c.notified.exchange(0), double(c.ns.exchange(0)) / 1e6,
                       double(c.worst_ns.exchange(0)) / 1e6);
  };
  const std::string p = line(g_preparer);
  const std::string e = line(g_executor);
  REXLOG_INFO("[handoff] last 10 s: preparer {}, executor {}; executor without commands {} waits, "
              "{:.1f} ms asleep",
              p, e, g_nocmd_waits.exchange(0), double(g_nocmd_ns.exchange(0)) / 1e6);
}

}  // namespace

// The game's Sleep(ms). Only the two Sleep(0) calls of the handoff are changed.
extern "C" REX_FUNC(sub_82943548) {
  static const bool enabled = REXCVAR_GET(nfscarbon_handoff_sleep);
  if (enabled && ctx.r3.u32 == 0) {
    const uint32_t ret = uint32_t(ctx.lr);
    if (ret == kPreparerReturn) {
      thread_local bool raised = false;
      RaisePriorityOnce(raised, "preparer");
      Wait(base, 1, g_preparer);
      Report();
      ctx.r3.u64 = 0;
      return;
    }
    if (ret == kExecutorReturn) {
      thread_local bool raised = false;
      RaisePriorityOnce(raised, "executor");
      Wait(base, 0, g_executor);
      ctx.r3.u64 = 0;
      return;
    }
  }
  __imp__sub_82943548(ctx, base);
}

// The executor's list runner. List: +0 commands written, +4 commands run, +8 closed by the preparer,
// +12 open. With 0 new commands on an open, unclosed list it returns at once and the game calls it again.
extern "C" REX_FUNC(sub_824694C0) {
  static const bool enabled = REXCVAR_GET(nfscarbon_executor_no_spin);
  if (enabled && uint32_t(ctx.lr) == kCommandsReturn && ctx.r4.u32 == 0) {
    const uint32_t list = ctx.r3.u32;
    const auto no_commands = [base, list] {
      return Load32(base, list + 12) != 0 && base[list + 8] == 0 &&
             Load32(base, list) == Load32(base, list + 4);
    };
    if (no_commands()) {
      const int64_t start = NowNs();
      const auto pause = std::chrono::microseconds(std::max(REXCVAR_GET(nfscarbon_executor_pause_us), 10));
      const int64_t limit =
          start + int64_t(std::max(REXCVAR_GET(nfscarbon_executor_wait_max_us), 50)) * 1000;
      do {
        rex::thread::Sleep(pause);
      } while (no_commands() && NowNs() < limit);
      g_nocmd_waits.fetch_add(1, std::memory_order_relaxed);
      g_nocmd_ns.fetch_add(uint64_t(NowNs() - start), std::memory_order_relaxed);
    }
  }
  __imp__sub_824694C0(ctx, base);
}

// Right after the flag is set to 1 (sub_824E9A80).
extern "C" REX_FUNC(sub_824F0210) {
  Notify();
  __imp__sub_824F0210(ctx, base);
}

// Right after the executor sets the flag to 0; the preparer also calls it (an extra notification is harmless).
extern "C" REX_FUNC(sub_829439E0) {
  Notify();
  __imp__sub_829439E0(ctx, base);
}
