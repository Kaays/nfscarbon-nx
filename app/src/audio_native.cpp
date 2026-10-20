// SPDX-License-Identifier: GPL-3.0-or-later
// nfscarbon-nx: native versions of the hottest loops of the game's audio server (RWAudioCore Dac).
//
// In Race Wars the server needs ~5.2 ms of CPU for each 5.33 ms packet of sound and falls behind: the
// output gets only 1,200-1,500 of its 1,880 packets per 10 s (slow, crackling sound). Stack sampling
// (session 22) puts these three loops at the top of the server's own time. The recompiled versions
// load and store every float through guest memory, one PowerPC instruction at a time; these do the same
// arithmetic, in the same order and with the same roundings, on registers (and NEON for the gain sum).
// nfsmw-nx did the same for Most Wanted's engine (nfsmw_audio_remuestreo.cpp, nfsmw_audio_filtro.cpp,
// nfsmw_audio_suma.cpp): -17.5 % CPU for its audio server, zero differences over 159 million samples.
//
// Guest floats are big-endian: every load and store swaps bytes.
//
//   sub_826CD8D0  gain sum: dst[i] += src[i] * gain. Aligned blocks of 64 use VMX (multiply, then add:
//                 two roundings); anything else the scalar loop (fmadds: one rounding).
//   sub_826C8BB0  one-pole filter: y = s * b + a * y (fmuls then fmadds), only when it has no
//                 sub-filter (+8 == 0); with one it calls the recompiled function as before.
//   sub_826D15D0  linear resampler, 16.16 position: out = s0 + (s1 - s0) * (frac * k).
//   sub_826CB1D8  the same resampler with a 32-bit step (the server's biggest single item).
//   sub_826BEB68  16-bit samples to float.
//   sub_826D1FC0  ramped gain sum of a 256-sample block (a voice whose volume changes): the first 64
//                 samples with a per-sample gain ramp, the other 192 at the final gain. VMX in the game
//                 (vmaddfp = multiply then add, two roundings), here the same lane-wise operations on
//                 registers instead of through the PPC context.
//   sub_826B56A8  sample unpack, mode 1 (big-endian int16 to float * 2^-15), from zofo007's
//                 NFS-Carbon-NX (src/audio_unpack_fast.h, GPL-3.0); other modes run the recompiled code.
//
// nfscarbon_audio_native = false puts the recompiled code back without a new NRO.

#include <cmath>
#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/logging.h>
#include <rex/platform.h>

#if defined(__aarch64__)
#include <arm_neon.h>
#endif
#include <simde/x86/ssse3.h>

#ifndef NFSCARBON_AUDIO_NATIVE_TEST
#include "../generated/default/nfscarbon_pch.h"

REXCVAR_DEFINE_BOOL(nfscarbon_audio_native, true, "Carbon",
                    "Native versions of the three hottest loops of the game's audio server (gain sum, "
                    "filter, resampler). false = the recompiled code");
#endif

namespace nfscarbon::audio_native {

inline uint32_t Load32(const uint8_t* base, uint32_t address) {
  uint32_t v;
  std::memcpy(&v, base + address, 4);
  return __builtin_bswap32(v);
}
inline void Store32(uint8_t* base, uint32_t address, uint32_t value) {
  value = __builtin_bswap32(value);
  std::memcpy(base + address, &value, 4);
}
inline float LoadF(const uint8_t* base, uint32_t address) {
  const uint32_t v = Load32(base, address);
  float f;
  std::memcpy(&f, &v, 4);
  return f;
}
inline void StoreF(uint8_t* base, uint32_t address, float f) {
  uint32_t v;
  std::memcpy(&v, &f, 4);
  Store32(base, address, v);
}
// Keeps the compiler from fusing a rounded product into a later add (fp-contract).
inline float Rounded(float x) {
#if defined(__aarch64__)
  asm volatile("" : "+w"(x));
#else
  asm volatile("" : "+x"(x));
#endif
  return x;
}

// sub_826CD8D0(r3 = dst, r4 = src, r6 = count, f1 = gain).
void GainSum(uint8_t* base, uint32_t dst, uint32_t src, uint32_t count, float gain) {
  if (((dst | src) & 127) == 0 && (count & 63) == 0) {
    // VMX path: vmulfp then vaddfp, 32 floats per iteration.
    const uint32_t n = count;
#if defined(__aarch64__)
    const float32x4_t g = vdupq_n_f32(gain);
    for (uint32_t i = 0; i < n; i += 4) {
      uint8_t* d = base + dst + i * 4;
      const uint8_t* s = base + src + i * 4;
      const float32x4_t sv = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(s)));
      const float32x4_t dv = vreinterpretq_f32_u8(vrev32q_u8(vld1q_u8(d)));
      float32x4_t m = vmulq_f32(g, sv);
      asm volatile("" : "+w"(m));  // vmulfp and vaddfp round separately: no fused multiply-add
      const float32x4_t r = vaddq_f32(m, dv);
      vst1q_u8(d, vrev32q_u8(vreinterpretq_u8_f32(r)));
    }
#else
    for (uint32_t i = 0; i < n; ++i) {
      const float p = Rounded(gain * LoadF(base, src + i * 4));
      StoreF(base, dst + i * 4, p + LoadF(base, dst + i * 4));
    }
#endif
    return;
  }
  // Scalar path: fmadds f0 = src * gain + dst.
  for (uint32_t i = 0; i < count; ++i) {
    StoreF(base, dst + i * 4, std::fmaf(LoadF(base, src + i * 4), gain, LoadF(base, dst + i * 4)));
  }
}

// The loop of sub_826C8BB0 (r3 = filter, r4 = count, r5 = src, r6 = dst), filter without sub-filter.
void OnePole(uint8_t* base, uint32_t filter, int32_t count, uint32_t src, uint32_t dst) {
  if (count <= 0) {
    return;
  }
  float y = LoadF(base, filter + 28);
  const float a = LoadF(base, filter + 32);
  const float b = LoadF(base, filter + 36);
  for (int32_t i = 0; i < count; ++i) {
    const float ay = Rounded(a * y);                       // fmuls f0,f13,f0
    y = std::fmaf(LoadF(base, src + 4 * i), b, ay);        // fmadds f0,f13,f12,f0
    StoreF(base, dst + 4 * i, y);
  }
  StoreF(base, filter + 28, y);
}

// sub_826D15D0(r3 = count, r4 = src, r5 = dst, r6 = &index, r7 = &position, r8/r9 = step), k at 0x820C7E18.
// Returns the step (r3 when the recompiled function returns).
uint32_t Resample(uint8_t* base, int32_t count, uint32_t src, uint32_t dst, uint32_t index_ptr,
                  uint32_t pos_ptr, uint32_t r8, uint32_t r9) {
  const uint32_t step = ((r8 << 16) & 0xFFFF0000u) | ((r9 >> 16) & 0xFFFFu);
  uint32_t frac = Load32(base, pos_ptr) >> 16;  // lhz: the upper half of the word
  if (count > 0) {
    const float k = LoadF(base, 0x820C7E18u);
    uint32_t index = Load32(base, index_ptr);
    for (int32_t i = 0; i < count; ++i) {
      const uint32_t at = index * 4 + src;
      const float s0 = LoadF(base, at);
      const float d = Rounded(LoadF(base, at + 4) - s0);   // fsubs
      const float t = Rounded(float(frac) * k);            // fcfid + frsp (exact) and fmuls
      StoreF(base, dst + 4 * i, std::fmaf(d, t, s0));      // fmadds
      const uint32_t moved = frac + step;
      index += moved >> 16;
      frac = moved & 0xFFFFu;
    }
    Store32(base, index_ptr, index);
  }
  Store32(base, pos_ptr, (frac & 0xFFFFu) << 16);
  return step;
}

// sub_826CB1D8: the same linear resampler with a plain 32-bit step (r4 = count, r5 = src, r6 = dst,
// r7 = &index, r8 = &position, r9 = step). The biggest single item of the server in Race Wars (stack
// sampling, session 22; LTO inlines it into its caller sub_826CB350). Returns the new r3 (dst + 4 * count
// when count > 0, otherwise the old r3).
uint32_t Resample32(uint8_t* base, uint32_t r3, int32_t count, uint32_t src, uint32_t dst,
                    uint32_t index_ptr, uint32_t pos_ptr, uint32_t step) {
  uint32_t index = Load32(base, index_ptr);
  uint32_t frac = Load32(base, pos_ptr) >> 16;  // lhz
  if (count > 0) {
    const float k = LoadF(base, 0x820C7E18u);
    for (int32_t i = 0; i < count; ++i) {
      const uint32_t at = index * 4 + src;
      const float s0 = LoadF(base, at);
      const float d = Rounded(LoadF(base, at + 4) - s0);
      const float t = Rounded(float(frac) * k);
      StoreF(base, dst + 4 * i, std::fmaf(d, t, s0));
      const uint32_t moved = frac + step;
      index += (moved >> 16) & 0xFFFFu;
      frac = moved & 0xFFFFu;
    }
    r3 = dst + 4 * uint32_t(count);
  }
  Store32(base, index_ptr, index);
  Store32(base, pos_ptr, (frac << 16) & 0xFFFF0000u);
  return r3;
}

// sub_826BEB68: 16-bit samples to float (r3 = count, r4 = src, r5 = dst).
void Int16ToFloat(uint8_t* base, int32_t count, uint32_t src, uint32_t dst) {
  for (int32_t i = 0; i < count; ++i) {
    uint16_t v;
    std::memcpy(&v, base + src + 2 * i, 2);
    StoreF(base, dst + 4 * i, float(int16_t(__builtin_bswap16(v))));
  }
}


// ---- sub_826D1FC0 -------------------------------------------------------------------------------
// r3 = dst, r4 = src (both 256 floats, 16-byte aligned blocks), f1 = start gain, f2 = gain change,
// r1 = stack (the game builds two vectors in the red zone below it; written the same way here).
// Lanes are kept in guest order (one bswap per float) instead of the recompiled code's full 16-byte
// reverse: every operation is lane-wise, so the results are the same.
namespace ramp {
inline simde__m128 LoadV(const uint8_t* base, uint32_t address) {
  const simde__m128i swap = simde_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  const simde__m128i v = simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(base + (address & ~0xFu)));
  return simde_mm_castsi128_ps(simde_mm_shuffle_epi8(v, swap));
}
inline void StoreV(uint8_t* base, uint32_t address, simde__m128 f) {
  const simde__m128i swap = simde_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(base + (address & ~0xFu)),
                        simde_mm_shuffle_epi8(simde_mm_castps_si128(f), swap));
}
// vmulfp then a separate rounding: never contracted into an FMA.
inline simde__m128 Mul(simde__m128 a, simde__m128 b) {
  simde__m128 m = simde_mm_mul_ps(a, b);
#if defined(__aarch64__)
  asm volatile("" : "+w"(m));
#else
  asm volatile("" : "+x"(m));
#endif
  return m;
}
inline simde__m128 Add(simde__m128 a, simde__m128 b) { return simde_mm_add_ps(a, b); }
}  // namespace ramp

struct RampScalars {
  float f11, inc, f10, l2, l3;
};
// The scalar part (FPU, no flush to zero).
inline RampScalars RampPrepare(const uint8_t* base, float f1, float f2) {
  // As the recompiled code computes them: in double, then rounded to single.
  const double d1 = f1, d2 = f2;
  RampScalars r;
  r.f11 = float(std::fma(d2, double(LoadF(base, uint32_t(-2113601536 + -5020))), d1));  // fmadds f11,f2,f13,f1
  r.inc = float(d2 * double(LoadF(base, uint32_t(-2113536000 + 11292))));               // fmuls f0,f2,f0
  r.f10 = float(d1 + d2);                                                               // fadds f10,f1,f2
  const uint32_t k = uint32_t(-2113536000 + 14732);
  r.l2 = float(std::fma(d2, double(LoadF(base, k + uint32_t(-22376))), d1));            // fmadds f0,f2,f13,f1
  r.l3 = float(std::fma(d2, double(LoadF(base, k)), d1));                               // fmadds f0,f2,f12,f1
  return r;
}
// The vector part (VMX, flush to zero).
inline void RampVectors(uint8_t* base, uint32_t dst, uint32_t src, uint32_t sp, const RampScalars& r) {
  using namespace ramp;
  const uint32_t c = uint32_t(-2113142784);
  const simde__m128 v63 = LoadV(base, c + uint32_t(-16096));
  // Per-vector lane offsets: k = 1..7 of a 128-byte block (v20, v19, ..., v14 in the game).
  const simde__m128 off[8] = {simde_mm_setzero_ps(),          LoadV(base, c + uint32_t(-16208)),
                              LoadV(base, c + uint32_t(-16192)), LoadV(base, c + uint32_t(-16176)),
                              LoadV(base, c + uint32_t(-16160)), LoadV(base, c + uint32_t(-16144)),
                              LoadV(base, c + uint32_t(-16128)), LoadV(base, c + uint32_t(-16112))};
  const simde__m128 v13 = LoadV(base, sp - 80);
  simde__m128 v0 = LoadV(base, sp - 64);
  for (uint32_t blk = 0; blk < 256; blk += 128) {
    simde__m128 g[8];
    g[0] = v0;
    for (int k = 1; k < 8; ++k) g[k] = Add(Mul(off[k], v13), v0);  // vmaddfp
    v0 = Add(Mul(v63, v13), v0);
    for (int k = 0; k < 8; ++k) {
      const uint32_t at = blk + 16 * k;
      StoreV(base, dst + at, Add(Mul(g[k], LoadV(base, src + at)), LoadV(base, dst + at)));
    }
  }
}
inline void RampTail(uint8_t* base, uint32_t dst, uint32_t src, uint32_t sp) {
  using namespace ramp;
  const simde__m128 g = LoadV(base, sp - 64);
  for (uint32_t at = 256; at < 1024; at += 16) {
    StoreV(base, dst + at, Add(Mul(g, LoadV(base, src + at)), LoadV(base, dst + at)));
  }
}
template <typename Ctx>
void RampSumCall(Ctx& ctx, uint8_t* base) {
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, sp = ctx.r1.u32;
  ctx.fpscr.disableFlushMode();
  const RampScalars r = RampPrepare(base, float(ctx.f1.f64), float(ctx.f2.f64));
  for (uint32_t i = 0; i < 4; ++i) StoreF(base, sp - 80 + 4 * i, r.inc);
  StoreF(base, sp - 64, float(ctx.f1.f64));
  StoreF(base, sp - 60, r.f10);
  StoreF(base, sp - 56, r.l2);
  StoreF(base, sp - 52, r.l3);
  ctx.fpscr.enableFlushMode();
  RampVectors(base, dst, src, sp, r);
  ctx.fpscr.disableFlushModeUnconditional();
  for (uint32_t i = 0; i < 4; ++i) StoreF(base, sp - 64 + 4 * i, r.f11);
  ctx.fpscr.enableFlushMode();
  RampTail(base, dst, src, sp);
  // What the recompiled function leaves in the FP registers the caller may still read.
  ctx.f11.f64 = double(r.f11);
  ctx.f10.f64 = double(r.f10);
}
// ---- sub_826B56A8, mode 1 ------------------------------------------------------------------------
// Ported from zofo007's NFS-Carbon-NX (src/audio_unpack_fast.h, GPL-3.0). The game reads n1 = r9
// samples at r6 + (r7 & ~1) and n2 = r10 more from r6 (the ring wraps) into one float run at r4.
// int16 * 2^-15 is exact, so the order of operations does not matter. Anything this does not model
// (other modes, a different scale constant, unaligned vector reads, overlap) returns false and the
// recompiled function runs instead.
inline void UnpackS16(const uint8_t* src, uint8_t* dst, uint32_t n) {
  const simde__m128 scale = simde_mm_set1_ps(1.0f / 32768.0f);
  const simde__m128i swap16 = simde_mm_setr_epi8(1, 0, 3, 2, 5, 4, 7, 6, 9, 8, 11, 10, 13, 12, 15, 14);
  const simde__m128i swap32 = simde_mm_setr_epi8(3, 2, 1, 0, 7, 6, 5, 4, 11, 10, 9, 8, 15, 14, 13, 12);
  uint32_t i = 0;
  for (; i + 8 <= n; i += 8) {
    const simde__m128i h = simde_mm_shuffle_epi8(
        simde_mm_loadu_si128(reinterpret_cast<const simde__m128i*>(src + 2 * size_t(i))), swap16);
    const simde__m128i lo32 = simde_mm_srai_epi32(simde_mm_unpacklo_epi16(h, h), 16);
    const simde__m128i hi32 = simde_mm_srai_epi32(simde_mm_unpackhi_epi16(h, h), 16);
    const simde__m128 lo = simde_mm_mul_ps(simde_mm_cvtepi32_ps(lo32), scale);
    const simde__m128 hi = simde_mm_mul_ps(simde_mm_cvtepi32_ps(hi32), scale);
    simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(dst + 4 * size_t(i)),
                          simde_mm_shuffle_epi8(simde_mm_castps_si128(lo), swap32));
    simde_mm_storeu_si128(reinterpret_cast<simde__m128i*>(dst + 4 * size_t(i) + 16),
                          simde_mm_shuffle_epi8(simde_mm_castps_si128(hi), swap32));
  }
  for (; i < n; ++i) {
    const int16_t v = int16_t(uint16_t(src[2 * size_t(i)] << 8 | src[2 * size_t(i) + 1]));
    float f = float(v) * (1.0f / 32768.0f);
    uint32_t u;
    std::memcpy(&u, &f, 4);
    u = __builtin_bswap32(u);
    std::memcpy(dst + 4 * size_t(i), &u, 4);
  }
}

template <typename Ctx>
bool TryUnpackMode1(Ctx& ctx, uint8_t* base) {
  if (ctx.r8.u32 != 1) return false;
  const int32_t n1 = ctx.r9.s32, n2 = ctx.r10.s32;
  if (n1 < 0 || n2 < 0 || Load32(base, 0x820C7F24u) != 0x38000000u) return false;  // f0 = 2^-15
  const uint32_t dst = ctx.r4.u32, src2 = ctx.r6.u32, src1 = src2 + (ctx.r7.u32 & ~1u);
  if (n2 >= 16 && (src2 & 15)) return false;  // the original's aligned vector loads would round down
  const uint64_t dst_bytes = 4ull * (uint64_t(n1) + uint64_t(n2));
  const auto one_region = [](uint32_t a, uint64_t len) {
    return len == 0 || ((uint64_t(a) + len - 1) >> 29) == (a >> 29);
  };
  const auto disjoint = [](uint32_t a, uint64_t alen, uint32_t b, uint64_t blen) {
    return alen == 0 || blen == 0 || uint64_t(a) + alen <= b || uint64_t(b) + blen <= a;
  };
  if (!one_region(dst, dst_bytes) || !one_region(src1, 2ull * n1) || !one_region(src2, 2ull * n2) ||
      !disjoint(dst, dst_bytes, src1, 2ull * n1) || !disjoint(dst, dst_bytes, src2, 2ull * n2)) {
    return false;
  }
  Store32(base, ctx.r1.u32 + 44, ctx.r6.u32);
  // The original's FP mode changes, in order (they decide the mode the caller continues in).
  ctx.fpscr.disableFlushMode();
  if ((dst & 0x7F) == 0 && n1 + n2 >= 32) ctx.fpscr.disableFlushMode();
  if (n1 >= 16) ctx.fpscr.enableFlushMode();
  if (n1 >= 16 ? (n1 & 15) != 0 : n1 != 0) ctx.fpscr.disableFlushMode();
  if (n2 >= 16) ctx.fpscr.enableFlushMode();
  if (n2 >= 16 ? (n2 & 15) != 0 : n2 != 0) ctx.fpscr.disableFlushMode();
  if (n1) UnpackS16(base + src1, base + dst, uint32_t(n1));
  if (n2) UnpackS16(base + src2, base + dst + 4 * size_t(n1), uint32_t(n2));
  return true;
}

}  // namespace nfscarbon::audio_native

#ifndef NFSCARBON_AUDIO_NATIVE_TEST

extern "C" void __imp__sub_826CD8D0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826C8BB0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826D15D0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826CB1D8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" void __imp__sub_826BEB68(PPCContext& __restrict ctx, uint8_t* base);

extern "C" REX_FUNC(sub_826CB1D8) {
  if (!REXCVAR_GET(nfscarbon_audio_native)) {
    __imp__sub_826CB1D8(ctx, base);
    return;
  }
  ctx.fpscr.disableFlushMode();
  ctx.r3.u64 = nfscarbon::audio_native::Resample32(base, ctx.r3.u32, ctx.r4.s32, ctx.r5.u32, ctx.r6.u32,
                                                   ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
}

extern "C" REX_FUNC(sub_826BEB68) {
  if (!REXCVAR_GET(nfscarbon_audio_native)) {
    __imp__sub_826BEB68(ctx, base);
    return;
  }
  ctx.fpscr.disableFlushMode();
  nfscarbon::audio_native::Int16ToFloat(base, ctx.r3.s32, ctx.r4.u32, ctx.r5.u32);
}

extern "C" REX_FUNC(sub_826CD8D0) {
  if (!REXCVAR_GET(nfscarbon_audio_native)) {
    __imp__sub_826CD8D0(ctx, base);
    return;
  }
  // The same flush-to-zero mode as the recompiled code: VMX flushes denormals, scalar FPU does not.
  const uint32_t dst = ctx.r3.u32, src = ctx.r4.u32, count = ctx.r6.u32;
  const float gain = float(ctx.f1.f64);
  if (((dst | src) & 127) == 0 && (count & 63) == 0) {
    ctx.fpscr.enableFlushMode();
  } else {
    ctx.fpscr.disableFlushMode();
  }
  nfscarbon::audio_native::GainSum(base, dst, src, count, gain);
}

extern "C" REX_FUNC(sub_826C8BB0) {
  if (!REXCVAR_GET(nfscarbon_audio_native) ||
      nfscarbon::audio_native::Load32(base, ctx.r3.u32 + 8) != 0) {
    __imp__sub_826C8BB0(ctx, base);
    return;
  }
  const int32_t count = ctx.r4.s32;
  ctx.fpscr.disableFlushMode();
  nfscarbon::audio_native::OnePole(base, ctx.r3.u32, count, ctx.r5.u32, ctx.r6.u32);
  ctx.r3.s64 = count;  // mr r3,r29
}

extern "C" void __imp__sub_826D1FC0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" REX_FUNC(sub_826D1FC0) {
  if (!REXCVAR_GET(nfscarbon_audio_native)) {
    __imp__sub_826D1FC0(ctx, base);
    return;
  }
  nfscarbon::audio_native::RampSumCall(ctx, base);
}

extern "C" void __imp__sub_826B56A8(PPCContext& __restrict ctx, uint8_t* base);
extern "C" REX_FUNC(sub_826B56A8) {
  if (!REXCVAR_GET(nfscarbon_audio_native) || !nfscarbon::audio_native::TryUnpackMode1(ctx, base)) {
    __imp__sub_826B56A8(ctx, base);
  }
}

extern "C" REX_FUNC(sub_826D15D0) {
  if (!REXCVAR_GET(nfscarbon_audio_native)) {
    __imp__sub_826D15D0(ctx, base);
    return;
  }
  ctx.fpscr.disableFlushMode();
  ctx.r3.u64 = nfscarbon::audio_native::Resample(base, ctx.r3.s32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32,
                                                 ctx.r7.u32, ctx.r8.u32, ctx.r9.u32);
}

#endif
