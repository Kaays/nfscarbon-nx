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
