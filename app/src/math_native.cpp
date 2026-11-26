// SPDX-License-Identifier: GPL-3.0-or-later
// nfscarbon-nx: native versions of two small vector math routines of the game, the hottest leaf
// functions of its preparation thread at race starts (console stack sampling, collector_pal, fps
// under 22: 2.1 % and 1.5 % of that thread in the functions themselves).
//
//   TransformPoint (out, v, m): out = x * m[0] + y * m[1] + z * m[2] + m[3], four floats.
//   MulMatrix (out, a, b):      out = a * b, 4x4 floats, row i of out = row i of a times b.
//
// The recompiled code spends most of its time on the PowerPC way of loading 16 bytes from any
// address (two aligned loads, a permute table and a byte permute) and of storing four words, and
// reloads the output pointer from the stack before every store. This keeps the arithmetic exactly
// as it is translated (the same SIMDe operations on the same register layout, the same flush mode)
// and only replaces the loads and stores, so the results are the same to the bit. Checked against a
// literal model of the recompiled functions at every alignment (x86, test harness outside the tree).
// Only a 4-byte aligned output takes this path, as the game's always is; anything else runs the
// recompiled code.

#include <cstdint>
#include <cstring>

#include <rex/cvar.h>
#include <rex/platform.h>

#include <simde/x86/sse4.1.h>
#include <simde/x86/ssse3.h>

#ifndef NFSCARBON_MATH_NATIVE_TEST
#include "../generated/default/nfscarbon_pch.h"

REXCVAR_DEFINE_BOOL(nfscarbon_math_native, true, "Carbon",
                    "Native versions of the game's point transform and 4x4 matrix product (same "
                    "results to the bit). false = the recompiled code");
#endif

namespace nfscarbon::math_native {

// The recompiled code keeps a vector register with its 16 bytes reversed (VectorMaskL): lane 3
// holds element 0. Loads and stores here keep that layout so the arithmetic below matches it.
inline simde__m128i Reverse() {
  return simde_mm_setr_epi8(15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
}
inline simde__m128i LoadV(const uint8_t* base, uint32_t address, uint32_t bytes = 16) {
  alignas(16) uint8_t raw[16] = {};
  std::memcpy(raw, base + address, bytes);
  return simde_mm_shuffle_epi8(simde_mm_load_si128(reinterpret_cast<const simde__m128i*>(raw)), Reverse());
}
inline void StoreV(uint8_t* base, uint32_t address, simde__m128i v) {
  alignas(16) uint8_t raw[16];
  simde_mm_store_si128(reinterpret_cast<simde__m128i*>(raw), simde_mm_shuffle_epi8(v, Reverse()));
  std::memcpy(base + address, raw, 16);
}
inline simde__m128 F(simde__m128i v) { return simde_mm_castsi128_ps(v); }
inline simde__m128i I(simde__m128 v) { return simde_mm_castps_si128(v); }
inline uint32_t Load32(const uint8_t* base, uint32_t a) {
  uint32_t v;
  std::memcpy(&v, base + a, 4);
  return __builtin_bswap32(v);
}
inline void Store32(uint8_t* base, uint32_t a, uint32_t v) {
  v = __builtin_bswap32(v);
  std::memcpy(base + a, &v, 4);
}

// r3 = out, r4 = point (3 floats), r5 = matrix (4 rows of 4 floats). Returns r3 the way the game
// does: written to 20(r1) first and read back at the end.
template <typename Ctx>
bool TransformPoint(Ctx& ctx, uint8_t* base) {
  const uint32_t out = ctx.r3.u32, point = ctx.r4.u32, m = ctx.r5.u32, sp = ctx.r1.u32;
  if (out & 3) return false;
  const simde__m128i v = LoadV(base, point, 12);  // the fourth element is never used
  const simde__m128i row0 = LoadV(base, m), row1 = LoadV(base, m + 16);
  const simde__m128i row2 = LoadV(base, m + 32), row3 = LoadV(base, m + 48);
  Store32(base, sp + 20, out);
  const simde__m128i z = simde_mm_shuffle_epi32(v, 0x55);  // vspltw 2
  const simde__m128i y = simde_mm_shuffle_epi32(v, 0xAA);  // vspltw 1
  const simde__m128i x = simde_mm_shuffle_epi32(v, 0xFF);  // vspltw 0
  ctx.fpscr.enableFlushMode();
  simde__m128 r = simde_mm_add_ps(simde_mm_mul_ps(F(row2), F(z)), F(row3));
  r = simde_mm_add_ps(simde_mm_mul_ps(F(y), F(row1)), r);
  r = simde_mm_add_ps(simde_mm_mul_ps(F(x), F(row0)), r);
  StoreV(base, out, I(r));
  ctx.r3.u64 = Load32(base, sp + 20);
  return true;
}

// r3 = out, r4 = a, r5 = b (4x4 floats each; out may be a or b: everything is read first).
template <typename Ctx>
bool MulMatrix(Ctx& ctx, uint8_t* base) {
  const uint32_t out = ctx.r3.u32, a = ctx.r4.u32, b = ctx.r5.u32, sp = ctx.r1.u32;
  if (out & 3) return false;
  const simde__m128i a0 = LoadV(base, a), a1 = LoadV(base, a + 16);
  const simde__m128i a2 = LoadV(base, a + 32), a3 = LoadV(base, a + 48);
  const simde__m128i b0 = LoadV(base, b), b1 = LoadV(base, b + 16);
  const simde__m128i b2 = LoadV(base, b + 32), b3 = LoadV(base, b + 48);
  Store32(base, sp + 20, out);
  // b transposed (vmrghw / vmrglw, as translated).
  const simde__m128i t0 = simde_mm_unpackhi_epi32(b2, b0);
  const simde__m128i t1 = simde_mm_unpackhi_epi32(b3, b1);
  const simde__m128i t2 = simde_mm_unpacklo_epi32(b2, b0);
  const simde__m128i t3 = simde_mm_unpacklo_epi32(b3, b1);
  const simde__m128 c0 = F(simde_mm_unpackhi_epi32(t1, t0));
  const simde__m128 c1 = F(simde_mm_unpacklo_epi32(t1, t0));
  const simde__m128 c2 = F(simde_mm_unpackhi_epi32(t3, t2));
  const simde__m128 c3 = F(simde_mm_unpacklo_epi32(t3, t2));
  ctx.fpscr.enableFlushMode();
  // vmsum4fp128: every lane holds the dot product.
  const auto dot = [](simde__m128i row, simde__m128 col) { return I(simde_mm_dp_ps(F(row), col, 0xFF)); };
  const simde__m128i d00 = dot(a0, c0), d01 = dot(a0, c1), d02 = dot(a0, c2), d03 = dot(a0, c3);
  const simde__m128i d10 = dot(a1, c0), d11 = dot(a1, c1), d12 = dot(a1, c2), d13 = dot(a1, c3);
  const simde__m128i d20 = dot(a2, c0), d21 = dot(a2, c1), d22 = dot(a2, c2), d23 = dot(a2, c3);
  const simde__m128i d30 = dot(a3, c0), d31 = dot(a3, c1), d32 = dot(a3, c2), d33 = dot(a3, c3);
  const auto row = [](simde__m128i e0, simde__m128i e1, simde__m128i e2, simde__m128i e3) {
    return simde_mm_unpackhi_epi32(simde_mm_unpackhi_epi32(e3, e1), simde_mm_unpackhi_epi32(e2, e0));
  };
  StoreV(base, out, row(d00, d01, d02, d03));
  StoreV(base, out + 16, row(d10, d11, d12, d13));
  StoreV(base, out + 32, row(d20, d21, d22, d23));
  StoreV(base, out + 48, row(d30, d31, d32, d33));
  ctx.r3.u64 = Load32(base, sp + 20);
  return true;
}

}  // namespace nfscarbon::math_native

#ifndef NFSCARBON_MATH_NATIVE_TEST

// USA addresses (the collector's edition ones, 0x82712FE8 and 0x82714578, found with
// tools/map_edition.py --reverse: both 0x1278 lower).
extern "C" void __imp__sub_82714260(PPCContext& __restrict ctx, uint8_t* base);
extern "C" REX_FUNC(sub_82714260) {
  if (!REXCVAR_GET(nfscarbon_math_native) || !nfscarbon::math_native::TransformPoint(ctx, base)) {
    __imp__sub_82714260(ctx, base);
  }
}

extern "C" void __imp__sub_827157F0(PPCContext& __restrict ctx, uint8_t* base);
extern "C" REX_FUNC(sub_827157F0) {
  if (!REXCVAR_GET(nfscarbon_math_native) || !nfscarbon::math_native::MulMatrix(ctx, base)) {
    __imp__sub_827157F0(ctx, base);
  }
}

#endif
