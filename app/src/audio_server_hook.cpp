// SPDX-License-Identifier: GPL-3.0-or-later
// nfscarbon-nx: the game's audio server thread ("RWAudioCore Dac"): wait for its packet before each
// audio frame, and rescue it if the voice stops returning packets.
//
// Port of nfsmw-nx's app/src/nfsmw_audio_servidor.cpp (docs/audio-and-video.md, "Robotic audio" and
// "An intermittent hang of the audio server"). Carbon runs the same EA audio engine: the server loop
// sub_826C6AB0 is MW's sub_825E3E28 with the object fields 88 bytes lower.
//
// THE SERVER LOOP (sub_826C6AB0). Object in r31, a ring of 2 packets of 6144 bytes:
//   +64          XAudio voice
//   +12356       R: next packet to submit
//   +12360       W: next packet to mix
//   +12364       event (auto-reset)
//   +12368+88*i  packet i
//   +12544+4*i   state i: 0 free, 1 full, 2 submitted to the voice
//   0x82C236EF   byte: the server is running
// 1. Waits for the event (sub_82A4DB58, timeout -1) at 0x826C6B0C.
// 2. If slot W is not free, it waits again without trying to submit.
// 3. Otherwise: the game's audio commands (sub_826AA418), mixes into W (state 1), and submits in order
//    while the voice has a free node (bit 0x20 of sub_82A4C4E0): sub_82A4C630 at 0x826C6C10, state 2.
// 4. The end of each packet arrives through sub_826C6DA8 (state 0 and signal) from the SDK's audio
//    thread; each pass of the voice calls sub_826C6D88, which only signals.
//
// WHAT WAS SEEN (console, sessions 13-20)
// - Silent game frames ("huecos" in rex_perfil.log): 130-450 per 10 s in Race Wars. Each one is a
//   pass of the voice with no packet: the server was late, XAudio played the voice as silence.
// - With the server at a higher priority, some starts went silent and the race never loaded: the
//   same as nfsmw-nx's "intermittent hang of the audio server".
//
// WHAT THIS FILE DOES
// - nfscarbon_audio_wait_server_ms (30): before each audio frame, if the voice has no packet and the
//   server is running, the SDK's Audio Worker waits up to that long for it instead of mixing the frame
//   with the voice silent. The audio queue (audio_switch_tramas_en_cola, 53 ms) absorbs the wait.
// - nfscarbon_audio_rescue (on): the server's wait gets a timeout. If it spends 250 ms with slot W
//   occupied and no end of packet or submission, it retries the submissions like the game and, if W is
//   still occupied, frees it so the loop keeps servicing the game's audio commands (and the race load).

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <cstring>
#include <mutex>

#include <rex/audio/audio_system.h>
#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

#include "../generated/default/nfscarbon_pch.h"

REXCVAR_DEFINE_BOOL(nfscarbon_audio_rescue, true, "Carbon",
                    "Keep the game's audio server thread going if the XAudio voice stops returning "
                    "end-of-packet notifications (the race would never finish loading)");
REXCVAR_DEFINE_INT32(nfscarbon_audio_wait_server_ms, 30, "Carbon",
                     "Before each audio frame, if the game's audio server has not delivered its packet, wait "
                     "up to N ms for it instead of playing the frame silent. 0 = do not wait");

extern "C" void __imp__sub_82A4DB58(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82A4C630(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82A4C4E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826C6DA8(PPCContext& __restrict ctx, uint8_t* base);

namespace {

constexpr uint32_t kWaitReturn = 0x826C6B10;    // after bl sub_82A4DB58 in the server loop
constexpr uint32_t kSubmitReturn = 0x826C6C14;  // after bl sub_82A4C630 in the server loop
constexpr uint32_t kServerRunning = 0x82C236EF;
constexpr uint32_t kOffVoice = 64;
constexpr uint32_t kOffR = 12356;
constexpr uint32_t kOffW = 12360;
constexpr uint32_t kOffEvent = 12364;
constexpr uint32_t kOffPackets = 12368;
constexpr uint32_t kPacketBytes = 88;
constexpr uint32_t kOffStates = 12544;
constexpr uint32_t kFree = 0;
constexpr uint32_t kFull = 1;
constexpr uint32_t kSubmitted = 2;
constexpr uint8_t kFreeNodeBit = 0x20;
constexpr uint32_t kStatusTimeout = 0x102;

constexpr int64_t kWaitMs = 50;
constexpr int64_t kRescueWaitMs = 5;
constexpr int64_t kNoProgressMs = 250;
constexpr int64_t kRescueSummaryMs = 10000;
constexpr uint64_t kMaxDumps = 20;
// No submission within this long: the server is stopped on purpose (loads, videos), do not wait for it.
constexpr int64_t kServerStoppedMs = 300;

uint32_t Read32(const uint8_t* base, uint32_t address) {
  uint32_t v = 0;
  std::memcpy(&v, base + address, sizeof(v));
  return __builtin_bswap32(v);
}

void Write32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, sizeof(value));
}

uint32_t State(const uint8_t* base, uint32_t obj, uint32_t i) {
  return Read32(base, obj + kOffStates + 4 * i);
}

int64_t NowMs() {
  using namespace std::chrono;
  return duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count();
}

int64_t NowUs() {
  using namespace std::chrono;
  return duration_cast<microseconds>(steady_clock::now().time_since_epoch()).count();
}

std::atomic<uint64_t> g_ends{0};
std::atomic<uint64_t> g_submits{0};
std::atomic<int64_t> g_last_end_ms{0};
std::atomic<int64_t> g_last_submit_ms{0};
std::atomic<uint32_t> g_server_obj{0};
std::atomic<uint8_t*> g_server_base{nullptr};

std::mutex g_submit_mutex;
std::condition_variable g_submit_cv;

// Counters for the summary line every 10 s.
std::atomic<uint64_t> g_waits{0};
std::atomic<uint64_t> g_waits_expired{0};
std::atomic<uint64_t> g_wait_sum_us{0};
std::atomic<int64_t> g_wait_max_us{0};
std::atomic<int64_t> g_summary_ms{0};

struct Rescue {
  bool active = false;
  int64_t first_wait_ms = -1;
  int64_t since_ms = 0;
  int64_t last_summary_ms = 0;
  uint64_t ends_on_entry = 0;
  uint64_t freed = 0;
  uint64_t resubmits = 0;
  uint64_t times = 0;
};
Rescue g_rescue;  // only the server thread touches it

void Dump(const char* what, const uint8_t* base, uint32_t obj, int64_t now) {
  REXLOG_WARN("[audio] {}: R={} W={} states={}/{} running={} voice=0x{:08X} ends={} submits={} "
              "ms_without_end={} ms_without_submit={}",
              what, Read32(base, obj + kOffR), Read32(base, obj + kOffW), State(base, obj, 0),
              State(base, obj, 1), unsigned(base[kServerRunning]), Read32(base, obj + kOffVoice),
              g_ends.load(), g_submits.load(), now - g_last_end_ms.load(), now - g_last_submit_ms.load());
}

void MaybeSummary() {
  const int64_t now = NowMs();
  int64_t last = g_summary_ms.load(std::memory_order_relaxed);
  if (last == 0) {
    g_summary_ms.compare_exchange_strong(last, now, std::memory_order_relaxed);
    return;
  }
  if (now - last < 10000 || !g_summary_ms.compare_exchange_strong(last, now, std::memory_order_relaxed)) {
    return;
  }
  const uint64_t waits = g_waits.exchange(0, std::memory_order_relaxed);
  const uint64_t expired = g_waits_expired.exchange(0, std::memory_order_relaxed);
  const uint64_t sum_us = g_wait_sum_us.exchange(0, std::memory_order_relaxed);
  const int64_t max_us = g_wait_max_us.exchange(0, std::memory_order_relaxed);
  REXLOG_INFO("[audio] waits for the server's packet in {} s: {} (expired {}), mean {} us, max {} us",
              (now - last) / 1000, waits, expired, waits ? sum_us / waits : 0, max_us);
}

// SDK hook before each audio frame, on the Audio Worker thread (0x2B, above the server). Waits on the
// condition with 1 ms slices: the submission notifies before the game marks the packet as submitted, so
// each notification is checked again and a lost one only costs that millisecond.
void WaitServerPacket(size_t) {
  MaybeSummary();
  const int32_t max_ms = REXCVAR_GET(nfscarbon_audio_wait_server_ms);
  if (max_ms <= 0) {
    return;
  }
  const uint32_t obj = g_server_obj.load(std::memory_order_relaxed);
  const uint8_t* const base = g_server_base.load(std::memory_order_relaxed);
  if (obj == 0 || base == nullptr) {
    return;
  }
  const auto has_packet = [&] {
    return State(base, obj, 0) == kSubmitted || State(base, obj, 1) == kSubmitted;
  };
  if (has_packet()) {
    return;
  }
  if (base[kServerRunning] == 0 ||
      NowMs() - g_last_submit_ms.load(std::memory_order_relaxed) > kServerStoppedMs) {
    return;
  }
  const int64_t start_us = NowUs();
  const int64_t limit_us = start_us + int64_t(max_ms) * 1000;
  bool expired = false;
  {
    std::unique_lock<std::mutex> lock(g_submit_mutex);
    while (!has_packet()) {
      const int64_t now_us = NowUs();
      if (now_us >= limit_us) {
        expired = true;
        break;
      }
      g_submit_cv.wait_for(lock, std::chrono::microseconds(std::min<int64_t>(1000, limit_us - now_us)));
    }
  }
  const int64_t waited_us = NowUs() - start_us;
  g_waits.fetch_add(1, std::memory_order_relaxed);
  if (expired) {
    g_waits_expired.fetch_add(1, std::memory_order_relaxed);
  }
  g_wait_sum_us.fetch_add(uint64_t(waited_us), std::memory_order_relaxed);
  int64_t max = g_wait_max_us.load(std::memory_order_relaxed);
  while (waited_us > max && !g_wait_max_us.compare_exchange_weak(max, waited_us, std::memory_order_relaxed)) {
  }
}

// Submits the full packets in order while the voice has a free node, with the same condition and steps
// as 0x826C6BCC-0x826C6C28.
uint64_t Resubmit(PPCContext& ctx, uint8_t* base, uint32_t obj) {
  uint64_t submitted = 0;
  for (int round = 0; round < 2; ++round) {
    const uint32_t r = Read32(base, obj + kOffR) % 2;
    if (State(base, obj, r) != kFull) {
      break;
    }
    const uint32_t out = ctx.r1.u32 + 80;
    ctx.r3.u64 = Read32(base, obj + kOffVoice);
    ctx.r4.u64 = out;
    __imp__sub_82A4C4E0(ctx, base);
    if ((base[out] & kFreeNodeBit) == 0) {
      break;
    }
    Write32(base, obj + kOffStates + 4 * r, kSubmitted);
    ctx.r3.u64 = Read32(base, obj + kOffVoice);
    ctx.r4.u64 = obj + kOffPackets + kPacketBytes * r;
    ctx.r5.u64 = 0;
    __imp__sub_82A4C630(ctx, base);
    Write32(base, obj + kOffR, (r + 1) % 2);
    ++submitted;
  }
  return submitted;
}

}  // namespace

// End of a packet (state 0 and signal), from the SDK's audio thread.
extern "C" REX_FUNC(sub_826C6DA8) {
  g_ends.fetch_add(1, std::memory_order_relaxed);
  g_last_end_ms.store(NowMs(), std::memory_order_relaxed);
  __imp__sub_826C6DA8(ctx, base);
}

// Packet submission to the voice.
extern "C" REX_FUNC(sub_82A4C630) {
  const bool from_server = uint32_t(ctx.lr) == kSubmitReturn;
  __imp__sub_82A4C630(ctx, base);
  if (from_server) {
    g_submits.fetch_add(1, std::memory_order_relaxed);
    g_last_submit_ms.store(NowMs(), std::memory_order_relaxed);
    if (REXCVAR_GET(nfscarbon_audio_wait_server_ms) > 0) {
      { std::lock_guard<std::mutex> lock(g_submit_mutex); }
      g_submit_cv.notify_all();
    }
  }
}

// WaitForSingleObjectEx(handle, ms, alertable). Only the server loop's wait is changed.
extern "C" REX_FUNC(sub_82A4DB58) {
  const uint32_t obj = ctx.r31.u32;
  if (uint32_t(ctx.lr) != kWaitReturn || obj == 0 || Read32(base, obj + kOffEvent) != ctx.r3.u32) {
    __imp__sub_82A4DB58(ctx, base);
    return;
  }
  g_server_obj.store(obj, std::memory_order_relaxed);
  g_server_base.store(base, std::memory_order_relaxed);
  static std::atomic<bool> hook_set{false};
  if (!hook_set.exchange(true, std::memory_order_relaxed)) {
    rex::audio::SetGanchoAntesDeTrama(&WaitServerPacket);
    REXLOG_INFO("[audio] audio server found (object 0x{:08X}): wait for its packet up to {} ms, rescue {}",
                obj, REXCVAR_GET(nfscarbon_audio_wait_server_ms),
                REXCVAR_GET(nfscarbon_audio_rescue) ? "on" : "off");
  }
  if (!REXCVAR_GET(nfscarbon_audio_rescue)) {
    __imp__sub_82A4DB58(ctx, base);
    ctx.lr = kWaitReturn;
    return;
  }
  const uint32_t event = ctx.r3.u32;
  const uint32_t alertable = ctx.r5.u32;
  Rescue& r = g_rescue;
  for (;;) {
    if (r.first_wait_ms < 0) {
      r.first_wait_ms = NowMs();
    }
    ctx.r3.u64 = event;
    ctx.r4.u64 = uint32_t(r.active ? kRescueWaitMs : kWaitMs);
    ctx.r5.u64 = alertable;
    __imp__sub_82A4DB58(ctx, base);
    const bool signaled = ctx.r3.u32 != kStatusTimeout;
    const int64_t now = NowMs();

    if (r.active && g_ends.load(std::memory_order_relaxed) != r.ends_on_entry) {
      REXLOG_INFO("[audio] the voice returns end-of-packet again after {} ms of rescue (freed {}, resubmitted {})",
                  now - r.since_ms, r.freed, r.resubmits);
      r.active = false;
    }
    const bool w_busy = State(base, obj, Read32(base, obj + kOffW) % 2) != kFree;
    if (!r.active) {
      const int64_t reference = std::max({g_last_end_ms.load(std::memory_order_relaxed),
                                          g_last_submit_ms.load(std::memory_order_relaxed), r.first_wait_ms});
      const bool stuck = w_busy && base[kServerRunning] != 0 && now - reference >= kNoProgressMs;
      if (!stuck) {
        if (signaled) {
          break;
        }
        continue;
      }
      r.active = true;
      r.since_ms = now;
      r.last_summary_ms = now;
      r.ends_on_entry = g_ends.load(std::memory_order_relaxed);
      if (++r.times <= kMaxDumps) {
        Dump("the audio server made no progress for 250 ms; rescue", base, obj, now);
      }
    }
    // Rescue: first what the game would do; if slot W is still taken, free it so the loop mixes again.
    r.resubmits += Resubmit(ctx, base, obj);
    const uint32_t w = Read32(base, obj + kOffW) % 2;
    if (State(base, obj, w) != kFree) {
      Write32(base, obj + kOffStates + 4 * w, kFree);
      ++r.freed;
    }
    if (now - r.last_summary_ms >= kRescueSummaryMs && r.times <= kMaxDumps) {
      r.last_summary_ms = now;
      Dump("rescue continues", base, obj, now);
    }
    ctx.r3.u64 = 0;  // as if signaled
    break;
  }
  ctx.lr = kWaitReturn;
}
