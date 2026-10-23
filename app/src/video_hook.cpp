// SPDX-License-Identifier: GPL-3.0-or-later
// From zofo007's NFS-Carbon-NX (GPL-3.0), same USA default.xex; brought into nfscarbon-nx for the
// in-race picture-in-picture videos (boss and crew faces), which the game's own WMV3 decoder plays
// far below their frame rate on the Switch.
// Switch: cutscene video through FFmpeg instead of the game's recompiled WMV3 decoder.
//
// The game's XDK WMV3 decoder reaches about 22 FPS on the Switch (one 1280x720 frame is ~45 ms), so
// movies play in "slow motion" and fall seconds behind their audio. The MW port replaces that decoder
// with FFmpeg's; this does the same for Carbon's addresses, found in the generated code:
//   sub_8281EAF8  one compressed frame (DecodeData): fetches the frame, then calls the I or P decoder
//   sub_8283A8D8  the data-fetch trampoline: returns the next section of the frame in guest memory
//   sub_8282E0F0 / sub_82B01090  I decoder (scalar / VMX), sub_82893E40 / sub_828654E8  P decoder
//   sub_8280F1E0  reads whatever is left of the frame (the game calls it itself at the end)
// Context offsets: [+80] bit reader ([reader+24] = bytes left), [+3720/3724/3728] current Y/U/V
// planes, [+220]/[+224] origin of the picture inside them, [+136]/[+140] size in macroblocks,
// [+15472] codec 6 = WMV3, [+3892] loop filter, [+15560]/[+15536]/[+15564] state the decoders leave.
//
// Safety: every movie starts in "verify": the game decodes as usual and the same bytes go through
// FFmpeg; if the first frames match (Y within a tiny tolerance) the movie switches to native, else it
// stays with the game's decoder for good. carbon_video_wmv3_shadow keeps verifying all movie long and
// never switches (diagnostic). Movies with B frames and unknown context layouts are left to the game.

#include <rex/cvar.h>
#include <rex/filesystem/entry.h>
#include <rex/filesystem/file.h>
#include <rex/filesystem/vfs.h>
#include <rex/logging.h>
#include <rex/system/kernel_state.h>

#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "../generated/default/nfscarbon_pch.h"
#include "video_wmv3.h"

namespace rex::kernel::xboxkrnl {
std::string NfsmwUltimoWmvLeido();  // SDK xboxkrnl_io.cpp: last .wmv movie the game read
}

REXCVAR_DEFINE_BOOL(carbon_video_wmv3_native, true, "CarbonPerf",
                    "Cutscenes: decode WMV3 frames with FFmpeg instead of the game's slow decoder "
                    "(verified against the game's output on the first frames of each movie)");
REXCVAR_DEFINE_BOOL(carbon_video_wmv3_shadow, false, "CarbonDebug",
                    "Cutscenes: the game decodes every frame and FFmpeg only compares (diagnostic, logs differences)");

extern "C" void __imp__sub_8281EAF8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8283A8D8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8280F1E0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_8282E0F0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82B01090(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_82893E40(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_828654E8(PPCContext& __restrict ctx, uint8_t* base);

namespace {
using namespace carbon::video;
using Clock = std::chrono::steady_clock;

constexpr uint32_t kFrameBytesMax = 8u << 20;
constexpr int kVerifyGood = 6;  // consecutive matching frames (with a P among them) to go native

enum class Mode { kVerify, kNative, kGameOnly };

struct Movie {
  uint32_t ctx = 0;
  std::string path;
  MovieInfo info;
  Wmv3Decoder dec;
  Mode mode = Mode::kGameOnly;
  bool synced = false;  // FFmpeg holds the reference of the game's last frame
  bool saw_p = false;
  int good = 0;
  int rejected = 0;
  uint32_t native = 0, game = 0, compared = 0, mismatches = 0;
  uint32_t worst_max = 0;
  double worst_frac = 0;
  uint64_t native_ns = 0;
  Clock::time_point report_at;
};

std::mutex g_mutex;
std::unique_ptr<Movie> g_movie;

struct Scope {
  bool active = false;
  uint32_t ctx = 0;
  bool handled = false;  // FFmpeg decoded this frame, so its reference state stays in sync
  std::vector<uint8_t> data;
};
thread_local Scope t_scope;

uint8_t* Host(uint8_t* base, uint32_t addr) { return base + addr + REX_PHYS_HOST_OFFSET(addr); }

bool ReadHeader(const std::string& path, MovieInfo& info) {
  rex::filesystem::File* f = nullptr;
  rex::filesystem::FileAction action;
  const auto status = REX_KERNEL_FS()->OpenFile(nullptr, path, rex::filesystem::FileDisposition::kOpen,
                                                rex::filesystem::FileAccess::kGenericRead, false, true, &f,
                                                &action);
  if (status != 0 || !f) return false;
  std::vector<uint8_t> head(size_t(std::min<uint64_t>(f->entry()->size(), 64 * 1024)));
  size_t got = 0;
  const bool read = f->ReadSync(std::span<uint8_t>(head.data(), head.size()), 0, &got) == 0 && got == head.size();
  f->Destroy();
  return read && ParseAsfHeader(head.data(), head.size(), info);
}

void Report(Movie& m, bool force);

// Called on the decode thread at the first decoder call of a frame; (re)opens the movie state.
Movie* MovieFor(uint8_t* base, uint32_t ctx) {
  const std::string path = rex::kernel::xboxkrnl::NfsmwUltimoWmvLeido();
  if (g_movie && g_movie->ctx == ctx && g_movie->path == path) return g_movie.get();
  if (g_movie) Report(*g_movie, true);
  g_movie = std::make_unique<Movie>();
  Movie& m = *g_movie;
  m.ctx = ctx;
  m.path = path;
  m.report_at = Clock::now() + std::chrono::seconds(5);
  const uint32_t mbw = REX_LOAD_U32(ctx + 136), mbh = REX_LOAD_U32(ctx + 140);
  const uint32_t codec = REX_LOAD_U32(ctx + 15472), loop = REX_LOAD_U32(ctx + 3892);
  const bool header = !path.empty() && ReadHeader(path, m.info);
  const bool size_ok = header && m.info.width == int(mbw * 16) && m.info.height == int(mbh * 16);
  const bool ok = header && size_ok && codec == 6 && loop <= 1 && MaxBFrames(m.info) == 0 && m.dec.Open(m.info);
  m.mode = ok ? Mode::kVerify : Mode::kGameOnly;
  REXLOG_INFO("[video] WMV3 '{}' ctx {:08X}: header {} {}x{} (game {}x{} MB) seq {:02X}{:02X}{:02X}{:02X} codec {} "
              "loopfilter {} -> {}",
              path, ctx, header ? "ok" : "MISSING", m.info.width, m.info.height, mbw, mbh, m.info.sequence[0],
              m.info.sequence[1], m.info.sequence[2], m.info.sequence[3], codec, loop,
              ok ? (REXCVAR_GET(carbon_video_wmv3_shadow) ? "shadow compare" : "verify, then FFmpeg")
                 : "game decoder");
  return &m;
}

void Report(Movie& m, bool force) {
  const auto now = Clock::now();
  if (!force && now < m.report_at) return;
  m.report_at = now + std::chrono::seconds(5);
  REXLOG_INFO("[video] WMV3 '{}': mode {}, FFmpeg frames {} ({:.1f} ms avg), game frames {}, compared {} "
              "(mismatch {}, worst max {} / Y {:.3f}% px), rejected {}",
              m.path, m.mode == Mode::kNative ? "native" : (m.mode == Mode::kVerify ? "verify" : "game"),
              m.native, m.native ? double(m.native_ns) * 1e-6 / m.native : 0.0, m.game, m.compared, m.mismatches,
              m.worst_max, m.worst_frac * 100.0, m.rejected);
}

struct Planes {
  uint8_t* y;
  uint8_t* u;
  uint8_t* v;
  int sy, sc;
};

// The game's current picture, or false if the layout is not the one this was written for.
bool GamePlanes(uint8_t* base, uint32_t ctx, const MovieInfo& info, Planes& p) {
  const uint32_t off_y = REX_LOAD_U32(ctx + 220), off_c = REX_LOAD_U32(ctx + 224);
  if (off_y < 32 || (off_y - 32) % 32 || off_c < 16 || (off_c - 16) % 16) return false;
  p.sy = int((off_y - 32) / 32);
  p.sc = int((off_c - 16) / 16);
  if (p.sy != info.width + 64 || p.sc != info.width / 2 + 32) return false;  // 32/16 px borders
  const uint32_t y = REX_LOAD_U32(ctx + 3720), u = REX_LOAD_U32(ctx + 3724), v = REX_LOAD_U32(ctx + 3728);
  if (!y || !u || !v) return false;
  p.y = Host(base, y) + off_y;
  p.u = Host(base, u) + off_c;
  p.v = Host(base, v) + off_c;
  return true;
}

// Writes a picture plane with the replicated border the game's motion compensation expects.
void WritePlane(uint8_t* dst, int stride, const uint8_t* src, int src_stride, int w, int h, int border) {
  for (int y = 0; y < h; ++y) {
    uint8_t* row = dst + ptrdiff_t(y) * stride;
    std::memcpy(row, src + ptrdiff_t(y) * src_stride, w);
    std::memset(row - border, row[0], border);
    std::memset(row + w, row[w - 1], border);
  }
  for (int k = 1; k <= border; ++k) {
    std::memcpy(dst - ptrdiff_t(k) * stride - border, dst - border, size_t(w) + 2 * border);
    std::memcpy(dst + ptrdiff_t(h - 1 + k) * stride - border, dst + ptrdiff_t(h - 1) * stride - border,
                size_t(w) + 2 * border);
  }
}

void Compare(const uint8_t* a, int sa, const uint8_t* b, int sb, int w, int h, uint32_t& diff, uint32_t& max_diff) {
  for (int y = 0; y < h; ++y) {
    const uint8_t* ra = a + ptrdiff_t(y) * sa;
    const uint8_t* rb = b + ptrdiff_t(y) * sb;
    for (int x = 0; x < w; ++x) {
      const uint32_t d = uint32_t(std::abs(int(ra[x]) - int(rb[x])));
      diff += d != 0;
      max_diff = std::max(max_diff, d);
    }
  }
}

// Pulls the rest of the frame through the fetch trampoline (the capture hook sees it).
void Drain(PPCContext& ctx, uint8_t* base, uint32_t game_ctx) {
  const uint64_t lr = ctx.lr;
  ctx.r3.u64 = game_ctx;
  __imp__sub_8280F1E0(ctx, base);
  ctx.lr = lr;
}

// Replaces the I (is_p false) or P decoder. r3 = game context; returns 0 for success.
void Replace(PPCContext& ctx, uint8_t* base, void (*original)(PPCContext&, uint8_t*), bool is_p) {
  Scope& s = t_scope;
  const uint32_t game_ctx = ctx.r3.u32;
  if (!s.active || s.ctx != game_ctx || !REXCVAR_GET(carbon_video_wmv3_native)) {
    original(ctx, base);
    return;
  }
  std::lock_guard<std::mutex> lock(g_mutex);
  Movie& m = *MovieFor(base, game_ctx);
  Planes game;
  const bool layout = m.mode != Mode::kGameOnly && GamePlanes(base, game_ctx, m.info, game);
  if (m.mode != Mode::kGameOnly && !layout) {
    REXLOG_WARN("[video] WMV3: picture layout differs from the known one, using the game's decoder");
    m.mode = Mode::kGameOnly;
  }
  if (m.mode == Mode::kGameOnly || (m.mode == Mode::kNative && is_p && !m.synced)) {
    original(ctx, base);
    ++m.game;
    return;
  }
  const bool shadow = REXCVAR_GET(carbon_video_wmv3_shadow);
  const int w = m.info.width, h = m.info.height;

  if (m.mode == Mode::kNative) {
    const auto start = Clock::now();
    Drain(ctx, base, game_ctx);
    const uint8_t* src[3];
    int stride[3];
    if (s.data.size() > 1 && m.dec.Decode(s.data.data(), s.data.size(), src, stride)) {
      WritePlane(game.y, game.sy, src[0], stride[0], w, h, 32);
      WritePlane(game.u, game.sc, src[1], stride[1], w / 2, h / 2, 16);
      WritePlane(game.v, game.sc, src[2], stride[2], w / 2, h / 2, 16);
      if (is_p) {
        const bool mark = REX_LOAD_U32(game_ctx + 3892) != 0 || REX_LOAD_U32(game_ctx + 14824) != 0 ||
                          REX_LOAD_U32(game_ctx + 15196) != 0xFFFFFFFFu;
        REX_STORE_U32(game_ctx + 15560, mark ? 1 : 0);
        REX_STORE_U32(game_ctx + 15536, 1);
      }
      REX_STORE_U32(game_ctx + 15564, 0);
      m.synced = true;
      s.handled = true;
      ++m.native;
      m.native_ns += uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(Clock::now() - start).count());
    } else {
      // The frame was drained, so the game cannot decode it any more: keep the old picture.
      m.synced = false;
      if (++m.rejected >= 3) m.mode = Mode::kGameOnly;
      REXLOG_WARN("[video] WMV3: FFmpeg rejected a frame of {} bytes (rejected {})", s.data.size(), m.rejected);
    }
    ctx.r3.u64 = 0;
    Report(m, false);
    return;
  }

  // Verify: the game decodes, FFmpeg decodes the same bytes, the planes are compared.
  original(ctx, base);
  ++m.game;
  const uint64_t r3 = ctx.r3.u64;
  Drain(ctx, base, game_ctx);
  ctx.r3.u64 = r3;
  if (is_p && !m.synced) return;
  const uint8_t* src[3];
  int stride[3];
  if (s.data.size() <= 1 || !m.dec.Decode(s.data.data(), s.data.size(), src, stride)) {
    m.synced = false;
    if (!shadow) m.mode = Mode::kGameOnly;
    REXLOG_WARN("[video] WMV3 verify: FFmpeg could not decode a {} byte {} frame; game decoder kept",
                s.data.size(), is_p ? "P" : "I");
    return;
  }
  m.synced = true;
  s.handled = true;
  uint32_t dy = 0, my = 0, du = 0, mu = 0, dv = 0, mv = 0;
  Compare(game.y, game.sy, src[0], stride[0], w, h, dy, my);
  Compare(game.u, game.sc, src[1], stride[1], w / 2, h / 2, du, mu);
  Compare(game.v, game.sc, src[2], stride[2], w / 2, h / 2, dv, mv);
  ++m.compared;
  const double frac = double(dy) / (double(w) * h);
  m.worst_max = std::max(m.worst_max, std::max(my, std::max(mu, mv)));
  m.worst_frac = std::max(m.worst_frac, frac);
  const uint32_t chroma_limit = uint32_t(w) * uint32_t(h) / 200;
  const bool good = frac <= 0.005 && my <= 8 && mu <= 8 && mv <= 8 && du <= chroma_limit && dv <= chroma_limit;
  if (good) {
    m.good = std::min(m.good + 1, 1 << 20);
    m.saw_p |= is_p;
  } else {
    ++m.mismatches;
    m.good = 0;
    REXLOG_WARN("[video] WMV3 verify: {} frame differs: Y {} px (max {}), U {} (max {}), V {} (max {}), {} bytes, "
                "first bytes {:02X} {:02X} {:02X} {:02X}",
                is_p ? "P" : "I", dy, my, du, mu, dv, mv, s.data.size(), s.data.size() > 0 ? s.data[0] : 0,
                s.data.size() > 1 ? s.data[1] : 0, s.data.size() > 2 ? s.data[2] : 0,
                s.data.size() > 3 ? s.data[3] : 0);
    if (!shadow) m.mode = Mode::kGameOnly;
  }
  if (!shadow && m.mode == Mode::kVerify && m.good >= kVerifyGood && m.saw_p) {
    m.mode = Mode::kNative;
    REXLOG_INFO("[video] WMV3 '{}': {} frames match the game's decoder, FFmpeg takes over", m.path, m.good);
  }
  Report(m, false);
}

}  // namespace

// SDK hook (Switch): a movie file was just opened, so the previous movie is over. Its FFmpeg decoder (about
// 6 MB of frames and tables) is freed now: the process has only a few MB of slack and a cutscene thread
// failed to be created (C0000017) with the last decoder still alive.
extern "C" void CarbonOnWmvOpened() {
  std::lock_guard<std::mutex> lock(g_mutex);
  if (g_movie) {
    Report(*g_movie, true);
    g_movie.reset();
  }
}

extern "C" REX_FUNC(sub_8281EAF8) {
  Scope& s = t_scope;
  if (!REXCVAR_GET(carbon_video_wmv3_native) || s.active) {
    __imp__sub_8281EAF8(ctx, base);
    return;
  }
  s.active = true;
  s.ctx = ctx.r3.u32;
  s.handled = false;
  s.data.clear();
  __imp__sub_8281EAF8(ctx, base);
  s.active = false;
  if (s.data.size() > 1 && !s.handled) {
    // A frame went through the game's decoder alone (or a path this does not replace): FFmpeg is
    // stale until the next I frame.
    std::lock_guard<std::mutex> lock(g_mutex);
    if (g_movie && g_movie->ctx == s.ctx) g_movie->synced = false;
  }
}

extern "C" REX_FUNC(sub_8283A8D8) {
  const uint32_t pointer_at = ctx.r5.u32, size_at = ctx.r7.u32;
  __imp__sub_8283A8D8(ctx, base);
  Scope& s = t_scope;
  if (!s.active) return;
  const uint32_t pointer = REX_LOAD_U32(pointer_at), size = REX_LOAD_U32(size_at);
  if (pointer && size && s.data.size() + size <= kFrameBytesMax) {
    const uint8_t* bytes = Host(base, pointer);
    s.data.insert(s.data.end(), bytes, bytes + size);
  }
}

extern "C" REX_FUNC(sub_8282E0F0) { Replace(ctx, base, __imp__sub_8282E0F0, false); }
extern "C" REX_FUNC(sub_82B01090) { Replace(ctx, base, __imp__sub_82B01090, false); }
extern "C" REX_FUNC(sub_82893E40) { Replace(ctx, base, __imp__sub_82893E40, true); }
extern "C" REX_FUNC(sub_828654E8) { Replace(ctx, base, __imp__sub_828654E8, true); }
