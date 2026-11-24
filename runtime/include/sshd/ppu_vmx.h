#pragma once

#include "ppu_context.h"
#include "ppu_int.h"

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

// Vector helpers for generated code. Register layout: see "Vector registers" in ppu_context.h
// (host lane i = big-endian element n-1-i, so the 128-bit value of a register has big-endian byte 0
// in its most significant byte).

// Partial vector loads and stores. `sh` below is ea & 15.

SSHD_FORCEINLINE SshdU128 sshdVrBits(SshdVr v) {
    SshdU128 bits;
    __builtin_memcpy(&bits, &v, sizeof bits);
    return bits;
}

SSHD_FORCEINLINE SshdVr sshdVrFromBits(SshdU128 bits) {
    SshdVr v;
    __builtin_memcpy(&v, &bits, sizeof v);
    return v;
}

// lvlx/lvlxl: the bytes from ea to the end of its 16-byte block go to the left (big-endian low
// numbers) of the register, the rest is zero (RPCS3 `LVLX`).
SSHD_FORCEINLINE SshdVr sshdLvlx(const uint8_t* base, uint32_t ea) {
    return sshdVrFromBits(sshdVrBits(sshdReadVr(base, ea)) << ((ea & 15u) * 8u));
}

// lvrx/lvrxl: the bytes from the block start up to (not including) ea go to the right of the
// register, the rest is zero; an aligned ea loads nothing and does not touch memory (RPCS3 `LVRX`).
SSHD_FORCEINLINE SshdVr sshdLvrx(const uint8_t* base, uint32_t ea) {
    uint32_t sh = ea & 15u;
    if (sh == 0) return sshdVrFromBits(0);
    return sshdVrFromBits(sshdVrBits(sshdReadVr(base, ea)) >> ((16u - sh) * 8u));
}

// stvlx/stvlxl: big-endian bytes 0 .. 15-sh go to ea .. (RPCS3 `STVLX`).
SSHD_FORCEINLINE void sshdStvlx(uint8_t* base, uint32_t ea, SshdVr v) {
    uint8_t* p = base + ea;
    for (uint32_t j = 0; j < 16u - (ea & 15u); j++) p[j] = SSHD_VR_B(v, j);
}

// stvrx/stvrxl: big-endian bytes 16-sh .. 15 go to the start of the block; nothing when sh == 0
// (RPCS3 `STVRX`).
SSHD_FORCEINLINE void sshdStvrx(uint8_t* base, uint32_t ea, SshdVr v) {
    uint32_t sh = ea & 15u;
    uint8_t* p = base + (ea & ~15u);
    for (uint32_t k = 0; k < sh; k++) p[k] = SSHD_VR_B(v, 16u - sh + k);
}

// Element stores write the element at its natural position in the register, at the element-aligned
// address (RPCS3 `STVEBX`, `STVEHX`, `STVEWX`). The element loads need no helper: RPCS3 `LVEBX`,
// `LVEHX` and `LVEWX` load the whole aligned block like `LVX`, which leaves the other elements
// undefined by the architecture.
SSHD_FORCEINLINE void sshdStvebx(uint8_t* base, uint32_t ea, SshdVr v) { sshdWrite8(base, ea, SSHD_VR_B(v, ea & 15u)); }

SSHD_FORCEINLINE void sshdStvehx(uint8_t* base, uint32_t ea, SshdVr v) {
    ea &= ~1u;
    sshdWrite16(base, ea, SSHD_VR_H(v, (ea & 15u) >> 1));
}

SSHD_FORCEINLINE void sshdStvewx(uint8_t* base, uint32_t ea, SshdVr v) {
    ea &= ~3u;
    sshdWrite32(base, ea, SSHD_VR_W(v, (ea & 15u) >> 2));
}

// lvsl/lvsr: permute control vectors; big-endian byte j is sh + j (lvsl) or 16 - sh + j (lvsr)
// (RPCS3 `LVSL`, `LVSR`).
SSHD_FORCEINLINE SshdVr sshdLvsl(uint32_t ea) {
    SshdU8x16 lanes = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    SshdVr v;
    v.u8 = lanes + (uint8_t)(ea & 15u);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdLvsr(uint32_t ea) {
    SshdU8x16 lanes = {31, 30, 29, 28, 27, 26, 25, 24, 23, 22, 21, 20, 19, 18, 17, 16};
    SshdVr v;
    v.u8 = lanes - (uint8_t)(ea & 15u);
    return v;
}

// vsel: bits of b where c is set, bits of a elsewhere (RPCS3 `VSEL`).
SSHD_FORCEINLINE SshdVr sshdVsel(SshdVr a, SshdVr b, SshdVr c) {
    SshdVr v;
    v.u32 = (b.u32 & c.u32) | (a.u32 & ~c.u32);
    return v;
}

// Permute, splat, pack, unpack and whole-register shift helpers. Merges, vsldoi and the splats are
// emitted as __builtin_shufflevector calls with constant host lane indices (see emit_ops_vmx_perm.py).

typedef int8_t SshdS8x8 __attribute__((vector_size(8)));
typedef uint8_t SshdU8x8 __attribute__((vector_size(8)));
typedef int16_t SshdS16x4 __attribute__((vector_size(8)));
typedef uint16_t SshdU16x4 __attribute__((vector_size(8)));

// SAT is sticky: it is only ever set here, never cleared (mtvscr clears it).
SSHD_FORCEINLINE void sshdSetSat(SshdPpuContext* ctx, int clamped) { ctx->vscr_sat |= (uint8_t)(clamped != 0); }

// vperm: big-endian byte i of the result is concat(a, b)[c[i] & 31] (RPCS3 `VPERM`). In the host
// layout that is b || a indexed by ~c & 31.
SSHD_FORCEINLINE SshdVr sshdVperm(SshdVr a, SshdVr b, SshdVr c) {
    SshdU8x16 idx = ~c.u8 & 0x1f;
    SshdVr v;
#if defined(__SSSE3__)
    // pshufb reads the low four index bits; indices above 15 take a, the rest take b.
    SshdU8x16 from_a = (SshdU8x16)(idx > 15);
    SshdU8x16 pa = (SshdU8x16)__builtin_ia32_pshufb128((SshdS8x16)a.u8, (SshdS8x16)idx);
    SshdU8x16 pb = (SshdU8x16)__builtin_ia32_pshufb128((SshdS8x16)b.u8, (SshdS8x16)idx);
    v.u8 = (pa & from_a) | (pb & ~from_a);
#elif defined(__ARM_NEON)
    // A two-register tbl reads byte idx of b || a directly; idx is at most 31.
    const uint8x16x2_t ba = {{(uint8x16_t)b.u8, (uint8x16_t)a.u8}};
    v.u8 = (SshdU8x16)vqtbl2q_u8(ba, (uint8x16_t)idx);
#else
    uint8_t ab[32];
    __builtin_memcpy(ab, &b, 16);
    __builtin_memcpy(ab + 16, &a, 16);
    for (uint32_t i = 0; i < 16u; i++) v.u8[i] = ab[idx[i]];
#endif
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVspltisb(int8_t imm) {
    SshdS8x16 zero = {};
    SshdVr v;
    v.s8 = zero + imm;
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVspltish(int16_t imm) {
    SshdS16x8 zero = {};
    SshdVr v;
    v.s16 = zero + imm;
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVspltisw(int32_t imm) {
    SshdS32x4 zero = {};
    SshdVr v;
    v.s32 = zero + imm;
    return v;
}

// vsl/vsr/vslo/vsro shift the whole 128-bit register. vsl/vsr use the low three bits of vB's
// big-endian byte 15 for the bit count; the architecture leaves the result undefined when the
// other bytes differ, where RPCS3 `VSL`/`VSR` shift each byte by its own count. vslo/vsro shift by
// whole octets, count = byte 15 bits 121:124 (RPCS3 `VSLO`: `b._u8[0] & 0x78`, host byte 0).
SSHD_FORCEINLINE SshdVr sshdVsl(SshdVr a, SshdVr b) { return sshdVrFromBits(sshdVrBits(a) << (b.u8[0] & 7u)); }

SSHD_FORCEINLINE SshdVr sshdVsr(SshdVr a, SshdVr b) { return sshdVrFromBits(sshdVrBits(a) >> (b.u8[0] & 7u)); }

SSHD_FORCEINLINE SshdVr sshdVslo(SshdVr a, SshdVr b) { return sshdVrFromBits(sshdVrBits(a) << (b.u8[0] & 0x78u)); }

SSHD_FORCEINLINE SshdVr sshdVsro(SshdVr a, SshdVr b) { return sshdVrFromBits(sshdVrBits(a) >> (b.u8[0] & 0x78u)); }

// Packs: the result holds a's elements in big-endian elements 0 .. n/2-1 and b's in the rest, which
// is `b` then `a` in host lane order. The saturating packs set SAT when any element is clamped
// (RPCS3 `VPKSHSS` etc.). Signed-to-unsigned packs clamp negative values to 0 and also set SAT.
SSHD_FORCEINLINE SshdVr sshdVpkshss(SshdPpuContext* ctx, SshdVr a, SshdVr b) {
    SshdS16x8 zero = {};
    SshdS16x8 lo = zero - 128, hi = zero + 127;
    SshdS16x8 ca = __builtin_elementwise_min(__builtin_elementwise_max(a.s16, lo), hi);
    SshdS16x8 cb = __builtin_elementwise_min(__builtin_elementwise_max(b.s16, lo), hi);
    sshdSetSat(ctx, __builtin_reduce_or((ca != a.s16) | (cb != b.s16)));
    SshdS8x8 na = __builtin_convertvector(ca, SshdS8x8);
    SshdS8x8 nb = __builtin_convertvector(cb, SshdS8x8);
    SshdVr v;
    v.s8 = __builtin_shufflevector(nb, na, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVpkshus(SshdPpuContext* ctx, SshdVr a, SshdVr b) {
    SshdS16x8 zero = {};
    SshdS16x8 hi = zero + 255;
    SshdS16x8 ca = __builtin_elementwise_min(__builtin_elementwise_max(a.s16, zero), hi);
    SshdS16x8 cb = __builtin_elementwise_min(__builtin_elementwise_max(b.s16, zero), hi);
    sshdSetSat(ctx, __builtin_reduce_or((ca != a.s16) | (cb != b.s16)));
    SshdU8x8 na = __builtin_convertvector(ca, SshdU8x8);
    SshdU8x8 nb = __builtin_convertvector(cb, SshdU8x8);
    SshdVr v;
    v.u8 = __builtin_shufflevector(nb, na, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVpkuhus(SshdPpuContext* ctx, SshdVr a, SshdVr b) {
    SshdU16x8 zero = {};
    SshdU16x8 hi = zero + 255;
    SshdU16x8 ca = __builtin_elementwise_min(a.u16, hi);
    SshdU16x8 cb = __builtin_elementwise_min(b.u16, hi);
    sshdSetSat(ctx, __builtin_reduce_or((ca != a.u16) | (cb != b.u16)));
    SshdU8x8 na = __builtin_convertvector(ca, SshdU8x8);
    SshdU8x8 nb = __builtin_convertvector(cb, SshdU8x8);
    SshdVr v;
    v.u8 = __builtin_shufflevector(nb, na, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVpkswss(SshdPpuContext* ctx, SshdVr a, SshdVr b) {
    SshdS32x4 zero = {};
    SshdS32x4 lo = zero - 32768, hi = zero + 32767;
    SshdS32x4 ca = __builtin_elementwise_min(__builtin_elementwise_max(a.s32, lo), hi);
    SshdS32x4 cb = __builtin_elementwise_min(__builtin_elementwise_max(b.s32, lo), hi);
    sshdSetSat(ctx, __builtin_reduce_or((ca != a.s32) | (cb != b.s32)));
    SshdS16x4 na = __builtin_convertvector(ca, SshdS16x4);
    SshdS16x4 nb = __builtin_convertvector(cb, SshdS16x4);
    SshdVr v;
    v.s16 = __builtin_shufflevector(nb, na, 0, 1, 2, 3, 4, 5, 6, 7);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVpkswus(SshdPpuContext* ctx, SshdVr a, SshdVr b) {
    SshdS32x4 zero = {};
    SshdS32x4 hi = zero + 65535;
    SshdS32x4 ca = __builtin_elementwise_min(__builtin_elementwise_max(a.s32, zero), hi);
    SshdS32x4 cb = __builtin_elementwise_min(__builtin_elementwise_max(b.s32, zero), hi);
    sshdSetSat(ctx, __builtin_reduce_or((ca != a.s32) | (cb != b.s32)));
    SshdU16x4 na = __builtin_convertvector(ca, SshdU16x4);
    SshdU16x4 nb = __builtin_convertvector(cb, SshdU16x4);
    SshdVr v;
    v.u16 = __builtin_shufflevector(nb, na, 0, 1, 2, 3, 4, 5, 6, 7);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVpkuwus(SshdPpuContext* ctx, SshdVr a, SshdVr b) {
    SshdU32x4 zero = {};
    SshdU32x4 hi = zero + 65535u;
    SshdU32x4 ca = __builtin_elementwise_min(a.u32, hi);
    SshdU32x4 cb = __builtin_elementwise_min(b.u32, hi);
    sshdSetSat(ctx, __builtin_reduce_or((ca != a.u32) | (cb != b.u32)));
    SshdU16x4 na = __builtin_convertvector(ca, SshdU16x4);
    SshdU16x4 nb = __builtin_convertvector(cb, SshdU16x4);
    SshdVr v;
    v.u16 = __builtin_shufflevector(nb, na, 0, 1, 2, 3, 4, 5, 6, 7);
    return v;
}

// vpkpx: each 32-bit pixel a:8 r:8 g:8 b:8 becomes 1:5:5:5 from the low bit of a and the top five
// bits of r, g and b (RPCS3 `VPKPX`).
SSHD_FORCEINLINE SshdU16x4 sshdPackPixels(SshdU32x4 w) {
    SshdU32x4 h = ((w >> 9) & 0xfc00u) | ((w >> 6) & 0x3e0u) | ((w >> 3) & 0x1fu);
    return __builtin_convertvector(h, SshdU16x4);
}

SSHD_FORCEINLINE SshdVr sshdVpkpx(SshdVr a, SshdVr b) {
    SshdVr v;
    v.u16 = __builtin_shufflevector(sshdPackPixels(b.u32), sshdPackPixels(a.u32), 0, 1, 2, 3, 4, 5, 6, 7);
    return v;
}

// Unpacks sign-extend half of the elements; `h` (high) is big-endian elements 0 .. n/2-1, which are
// the upper host lanes. vupk*px widen 1:5:5:5 pixels to a:8 r:8 g:8 b:8, the 1-bit field becoming
// 0x00 or 0xff (RPCS3 `VUPKHPX`).
SSHD_FORCEINLINE SshdVr sshdVupkhsb(SshdVr b) {
    SshdVr v;
    v.s16 = __builtin_convertvector(__builtin_shufflevector(b.s8, b.s8, 8, 9, 10, 11, 12, 13, 14, 15), SshdS16x8);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVupklsb(SshdVr b) {
    SshdVr v;
    v.s16 = __builtin_convertvector(__builtin_shufflevector(b.s8, b.s8, 0, 1, 2, 3, 4, 5, 6, 7), SshdS16x8);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVupkhsh(SshdVr b) {
    SshdVr v;
    v.s32 = __builtin_convertvector(__builtin_shufflevector(b.s16, b.s16, 4, 5, 6, 7), SshdS32x4);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVupklsh(SshdVr b) {
    SshdVr v;
    v.s32 = __builtin_convertvector(__builtin_shufflevector(b.s16, b.s16, 0, 1, 2, 3), SshdS32x4);
    return v;
}

SSHD_FORCEINLINE SshdU32x4 sshdUnpackPixels(SshdS32x4 x) {
    SshdU32x4 u = (SshdU32x4)x;
    return ((u << 6) & 0x1f0000u) | ((u << 3) & 0x1f00u) | (u & 0xff00001fu);
}

SSHD_FORCEINLINE SshdVr sshdVupkhpx(SshdVr b) {
    SshdVr v;
    v.u32 = sshdUnpackPixels(__builtin_convertvector(__builtin_shufflevector(b.s16, b.s16, 4, 5, 6, 7), SshdS32x4));
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVupklpx(SshdVr b) {
    SshdVr v;
    v.u32 = sshdUnpackPixels(__builtin_convertvector(__builtin_shufflevector(b.s16, b.s16, 0, 1, 2, 3), SshdS32x4));
    return v;
}

// VSCR: NJ is bit 16 and SAT bit 0 of the register's big-endian word 3 (AltiVec PEM). RPCS3's
// interpreter indexes `_u32[3]`, which is big-endian word 0 in its byte-reversed register layout,
// while its LLVM translator uses element 0 of the same byte-reversed layout, which is the PEM word;
// the PEM position is kept here because guest code is built for hardware.
SSHD_FORCEINLINE SshdVr sshdMfvscr(const SshdPpuContext* ctx) {
    return sshdVrFromBeWords(0, 0, 0, ((uint32_t)(ctx->vscr_nj & 1) << 16) | (ctx->vscr_sat & 1));
}

SSHD_FORCEINLINE void sshdMtvscr(SshdPpuContext* ctx, SshdVr v) {
    uint32_t vscr = sshdVrBeWord(v, 3);
    ctx->vscr_nj = (vscr >> 16) & 1;
    ctx->vscr_sat = vscr & 1;
}

// Integer add/subtract with saturation. SAT is set when any lane clamps, i.e. when the clamped result
// differs from the wrapped one (RPCS3 `VADDSBS` etc. compare the same two values). The wrapped sum
// is formed on the unsigned view because signed vector overflow is not defined.
#define SSHD_VMX_SATURATE(fn, builtin, op, view, uview)                        \
    SSHD_FORCEINLINE SshdVr fn(SshdPpuContext* ctx, SshdVr a, SshdVr b) {      \
        SshdVr v;                                                              \
        v.view = builtin(a.view, b.view);                                      \
        sshdSetSat(ctx, __builtin_reduce_or(v.uview != (a.uview op b.uview))); \
        return v;                                                              \
    }

SSHD_VMX_SATURATE(sshdVaddubs, __builtin_elementwise_add_sat, +, u8, u8)
SSHD_VMX_SATURATE(sshdVadduhs, __builtin_elementwise_add_sat, +, u16, u16)
SSHD_VMX_SATURATE(sshdVadduws, __builtin_elementwise_add_sat, +, u32, u32)
SSHD_VMX_SATURATE(sshdVaddsbs, __builtin_elementwise_add_sat, +, s8, u8)
SSHD_VMX_SATURATE(sshdVaddshs, __builtin_elementwise_add_sat, +, s16, u16)
SSHD_VMX_SATURATE(sshdVaddsws, __builtin_elementwise_add_sat, +, s32, u32)
SSHD_VMX_SATURATE(sshdVsububs, __builtin_elementwise_sub_sat, -, u8, u8)
SSHD_VMX_SATURATE(sshdVsubuhs, __builtin_elementwise_sub_sat, -, u16, u16)
SSHD_VMX_SATURATE(sshdVsubuws, __builtin_elementwise_sub_sat, -, u32, u32)
SSHD_VMX_SATURATE(sshdVsubsbs, __builtin_elementwise_sub_sat, -, s8, u8)
SSHD_VMX_SATURATE(sshdVsubshs, __builtin_elementwise_sub_sat, -, s16, u16)
SSHD_VMX_SATURATE(sshdVsubsws, __builtin_elementwise_sub_sat, -, s32, u32)
#undef SSHD_VMX_SATURATE

// vaddcuw: 1 where the unsigned sum carries out of 32 bits (b > ~a); vsubcuw: 1 where a - b does not
// borrow, i.e. a >= b (RPCS3 `VADDCUW`, `VSUBCUW`).
SSHD_FORCEINLINE SshdVr sshdVaddcuw(SshdVr a, SshdVr b) {
    SshdVr v;
    v.u32 = (SshdU32x4)(b.u32 > ~a.u32) & 1u;
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVsubcuw(SshdVr a, SshdVr b) {
    SshdVr v;
    v.u32 = (SshdU32x4)(a.u32 >= b.u32) & 1u;
    return v;
}

// Averages round up: (a + b + 1) >> 1 as (a | b) - ((a ^ b) >> 1), which cannot overflow. The shift
// is arithmetic for the signed forms (RPCS3 `VAVGSB` etc.).
#define SSHD_VMX_AVERAGE(fn, view, uview, utype)                         \
    SSHD_FORCEINLINE SshdVr fn(SshdVr a, SshdVr b) {                     \
        SshdVr v;                                                        \
        v.uview = (a.uview | b.uview) - (utype)((a.view ^ b.view) >> 1); \
        return v;                                                        \
    }

SSHD_VMX_AVERAGE(sshdVavgub, u8, u8, SshdU8x16)
SSHD_VMX_AVERAGE(sshdVavguh, u16, u16, SshdU16x8)
SSHD_VMX_AVERAGE(sshdVavguw, u32, u32, SshdU32x4)
SSHD_VMX_AVERAGE(sshdVavgsb, s8, u8, SshdU8x16)
SSHD_VMX_AVERAGE(sshdVavgsh, s16, u16, SshdU16x8)
SSHD_VMX_AVERAGE(sshdVavgsw, s32, u32, SshdU32x4)
#undef SSHD_VMX_AVERAGE

// vcmp*. with Rc=1: CR6 = {all lanes true, 0, no lane true, 0} (RPCS3 `ppu_cr_set(ppu, 6, ...)`).
SSHD_FORCEINLINE void sshdSetCr6(SshdPpuContext* ctx, SshdVr result) {
    uint64_t all = result.u64[0] & result.u64[1];
    uint64_t any = result.u64[0] | result.u64[1];
    sshdSetCrField(ctx, 6, ((uint32_t)(all == ~0ull) << 3) | ((uint32_t)(any == 0) << 1));
}

// Per-element shifts and rotates (RPCS3 `VSLB`, `VSRAW`, `VRLH` etc.): each element of vB supplies
// its own count, taken modulo the element width, so no shift amount reaches the width. Left shifts
// run on the unsigned view because shifting a negative value left is not defined in C. The rotate
// uses (width - n) & (width - 1) so that n == 0 shifts by 0 instead of by the full width.
#define SSHD_VMX_SHIFTS(sfx, view, sview, utype, stype, mask)         \
    SSHD_FORCEINLINE SshdVr sshdVrl##sfx(SshdVr a, SshdVr b) {        \
        SshdVr v;                                                     \
        utype n = b.view & mask;                                      \
        v.view = (a.view << n) | (a.view >> ((mask + 1 - n) & mask)); \
        return v;                                                     \
    }                                                                 \
    SSHD_FORCEINLINE SshdVr sshdVsl##sfx(SshdVr a, SshdVr b) {        \
        SshdVr v;                                                     \
        v.view = a.view << (b.view & mask);                           \
        return v;                                                     \
    }                                                                 \
    SSHD_FORCEINLINE SshdVr sshdVsr##sfx(SshdVr a, SshdVr b) {        \
        SshdVr v;                                                     \
        v.view = a.view >> (b.view & mask);                           \
        return v;                                                     \
    }                                                                 \
    SSHD_FORCEINLINE SshdVr sshdVsra##sfx(SshdVr a, SshdVr b) {       \
        SshdVr v;                                                     \
        v.sview = a.sview >> (stype)(b.view & mask);                  \
        return v;                                                     \
    }

SSHD_VMX_SHIFTS(b, u8, s8, SshdU8x16, SshdS8x16, 7)
SSHD_VMX_SHIFTS(h, u16, s16, SshdU16x8, SshdS16x8, 15)
SSHD_VMX_SHIFTS(w, u32, s32, SshdU32x4, SshdS32x4, 31)
#undef SSHD_VMX_SHIFTS

// Widening multiplies. "Even" and "odd" count elements in big-endian order (element 0 is even), so
// even elements are the odd host lanes (RPCS3 `VMULEUB` reads the high byte of each host halfword).
// The products of 8-bit and 16-bit elements fit the result type, so no lane overflows.
SSHD_FORCEINLINE SshdVr sshdVmuleub(SshdVr a, SshdVr b) {
    SshdVr v;
    v.u16 = (a.u16 >> 8) * (b.u16 >> 8);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVmuloub(SshdVr a, SshdVr b) {
    SshdVr v;
    v.u16 = (a.u16 & 0xffu) * (b.u16 & 0xffu);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVmulesb(SshdVr a, SshdVr b) {
    SshdVr v;
    v.s16 = (a.s16 >> 8) * (b.s16 >> 8);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVmulosb(SshdVr a, SshdVr b) {
    SshdVr v;
    SshdS16x8 ea = __builtin_convertvector(__builtin_shufflevector(a.s8, a.s8, 0, 2, 4, 6, 8, 10, 12, 14), SshdS16x8);
    SshdS16x8 eb = __builtin_convertvector(__builtin_shufflevector(b.s8, b.s8, 0, 2, 4, 6, 8, 10, 12, 14), SshdS16x8);
    v.s16 = ea * eb;
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVmuleuh(SshdVr a, SshdVr b) {
    SshdVr v;
    v.u32 = __builtin_convertvector(__builtin_shufflevector(a.u16, a.u16, 1, 3, 5, 7), SshdU32x4) *
            __builtin_convertvector(__builtin_shufflevector(b.u16, b.u16, 1, 3, 5, 7), SshdU32x4);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVmulouh(SshdVr a, SshdVr b) {
    SshdVr v;
    v.u32 = __builtin_convertvector(__builtin_shufflevector(a.u16, a.u16, 0, 2, 4, 6), SshdU32x4) *
            __builtin_convertvector(__builtin_shufflevector(b.u16, b.u16, 0, 2, 4, 6), SshdU32x4);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVmulesh(SshdVr a, SshdVr b) {
    SshdVr v;
    v.s32 = __builtin_convertvector(__builtin_shufflevector(a.s16, a.s16, 1, 3, 5, 7), SshdS32x4) *
            __builtin_convertvector(__builtin_shufflevector(b.s16, b.s16, 1, 3, 5, 7), SshdS32x4);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVmulosh(SshdVr a, SshdVr b) {
    SshdVr v;
    v.s32 = __builtin_convertvector(__builtin_shufflevector(a.s16, a.s16, 0, 2, 4, 6), SshdS32x4) *
            __builtin_convertvector(__builtin_shufflevector(b.s16, b.s16, 0, 2, 4, 6), SshdS32x4);
    return v;
}

// Clamp to the signed 16-bit, signed 32-bit or unsigned 32-bit range and set SAT when the value
// changed.
SSHD_FORCEINLINE int16_t sshdClampS16(SshdPpuContext* ctx, int32_t x) {
    int32_t r = x < -32768 ? -32768 : x > 32767 ? 32767 : x;
    sshdSetSat(ctx, r != x);
    return (int16_t)r;
}

SSHD_FORCEINLINE int32_t sshdClampS32(SshdPpuContext* ctx, int64_t x) {
    int64_t r = x < INT32_MIN ? INT32_MIN : x > INT32_MAX ? INT32_MAX : x;
    sshdSetSat(ctx, r != x);
    return (int32_t)r;
}

SSHD_FORCEINLINE uint32_t sshdClampU32(SshdPpuContext* ctx, uint64_t x) {
    uint64_t r = x > UINT32_MAX ? UINT32_MAX : x;
    sshdSetSat(ctx, r != x);
    return (uint32_t)r;
}

// vmhaddshs: (a * b >> 15) + c per halfword, saturated. The shifted product of -32768 * -32768 is
// 32768, which only the 32-bit intermediate keeps (RPCS3 `VMHADDSHS` special-cases it).
// vmhraddshs adds 0x4000 before the shift (RPCS3 `VMHRADDSHS`).
SSHD_FORCEINLINE SshdVr sshdVmhaddshs(SshdPpuContext* ctx, SshdVr a, SshdVr b, SshdVr c) {
    SshdVr v;
    for (int i = 0; i < 8; i++) v.s16[i] = sshdClampS16(ctx, (((int32_t)a.s16[i] * b.s16[i]) >> 15) + c.s16[i]);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVmhraddshs(SshdPpuContext* ctx, SshdVr a, SshdVr b, SshdVr c) {
    SshdVr v;
    for (int i = 0; i < 8; i++)
        v.s16[i] = sshdClampS16(ctx, (((int32_t)a.s16[i] * b.s16[i] + 0x4000) >> 15) + c.s16[i]);
    return v;
}

// vmladduhm: low 16 bits of a * b + c per halfword (RPCS3 `VMLADDUHM`).
SSHD_FORCEINLINE SshdVr sshdVmladduhm(SshdVr a, SshdVr b, SshdVr c) {
    SshdVr v;
    v.u16 = a.u16 * b.u16 + c.u16;
    return v;
}

// vmsum*: word i of the result is c's word i plus the products of the elements of a and b that lie
// in word i. Elements of one word share a host lane, so no big-endian mapping is needed. The modulo
// forms wrap in 32 bits; the saturating forms add exactly in 64 bits and clamp (RPCS3 `VMSUMUBM`,
// `VMSUMMBM` (a signed, b unsigned), `VMSUMUHM`, `VMSUMSHM`, `VMSUMUHS`, `VMSUMSHS`).
SSHD_FORCEINLINE SshdVr sshdVmsumubm(SshdVr a, SshdVr b, SshdVr c) {
    SshdVr v;
    for (int i = 0; i < 4; i++) {
        uint32_t sum = c.u32[i];
        for (int k = 0; k < 4; k++) sum += (uint32_t)a.u8[4 * i + k] * b.u8[4 * i + k];
        v.u32[i] = sum;
    }
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVmsummbm(SshdVr a, SshdVr b, SshdVr c) {
    SshdVr v;
    for (int i = 0; i < 4; i++) {
        uint32_t sum = c.u32[i];
        for (int k = 0; k < 4; k++) sum += (uint32_t)((int32_t)a.s8[4 * i + k] * b.u8[4 * i + k]);
        v.u32[i] = sum;
    }
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVmsumuhm(SshdVr a, SshdVr b, SshdVr c) {
    SshdVr v;
    for (int i = 0; i < 4; i++)
        v.u32[i] = c.u32[i] + (uint32_t)a.u16[2 * i] * b.u16[2 * i] + (uint32_t)a.u16[2 * i + 1] * b.u16[2 * i + 1];
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVmsumshm(SshdVr a, SshdVr b, SshdVr c) {
    SshdVr v;
    for (int i = 0; i < 4; i++)
        v.u32[i] = c.u32[i] + (uint32_t)((int32_t)a.s16[2 * i] * b.s16[2 * i]) +
                   (uint32_t)((int32_t)a.s16[2 * i + 1] * b.s16[2 * i + 1]);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVmsumuhs(SshdPpuContext* ctx, SshdVr a, SshdVr b, SshdVr c) {
    SshdVr v;
    for (int i = 0; i < 4; i++)
        v.u32[i] = sshdClampU32(ctx, (uint64_t)c.u32[i] + (uint64_t)a.u16[2 * i] * b.u16[2 * i] +
                                             (uint64_t)a.u16[2 * i + 1] * b.u16[2 * i + 1]);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVmsumshs(SshdPpuContext* ctx, SshdVr a, SshdVr b, SshdVr c) {
    SshdVr v;
    for (int i = 0; i < 4; i++)
        v.s32[i] = sshdClampS32(ctx, (int64_t)c.s32[i] + (int64_t)a.s16[2 * i] * b.s16[2 * i] +
                                             (int64_t)a.s16[2 * i + 1] * b.s16[2 * i + 1]);
    return v;
}

// vsum4*: word i of the result is b's word i plus the elements of a's word i, summed exactly in
// 64 bits before the clamp (RPCS3 `VSUM4SBS`, `VSUM4SHS`, `VSUM4UBS`).
SSHD_FORCEINLINE SshdVr sshdVsum4sbs(SshdPpuContext* ctx, SshdVr a, SshdVr b) {
    SshdVr v;
    for (int i = 0; i < 4; i++) {
        int64_t sum = b.s32[i];
        for (int k = 0; k < 4; k++) sum += a.s8[4 * i + k];
        v.s32[i] = sshdClampS32(ctx, sum);
    }
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVsum4shs(SshdPpuContext* ctx, SshdVr a, SshdVr b) {
    SshdVr v;
    for (int i = 0; i < 4; i++) v.s32[i] = sshdClampS32(ctx, (int64_t)b.s32[i] + a.s16[2 * i] + a.s16[2 * i + 1]);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVsum4ubs(SshdPpuContext* ctx, SshdVr a, SshdVr b) {
    SshdVr v;
    for (int i = 0; i < 4; i++) {
        uint64_t sum = b.u32[i];
        for (int k = 0; k < 4; k++) sum += a.u8[4 * i + k];
        v.u32[i] = sshdClampU32(ctx, sum);
    }
    return v;
}

// vsum2sws: big-endian word 1 = a0 + a1 + b1 and word 3 = a2 + a3 + b3, saturated; words 0 and 2
// are zero. vsumsws: word 3 = a0 + a1 + a2 + a3 + b3, the other words are zero. Big-endian word n is
// host lane 3 - n (RPCS3 `VSUM2SWS`, `VSUMSWS` read b's host lane 0 and 2 the same way).
SSHD_FORCEINLINE SshdVr sshdVsum2sws(SshdPpuContext* ctx, SshdVr a, SshdVr b) {
    SshdVr v = {};
    v.s32[2] = sshdClampS32(ctx, (int64_t)a.s32[3] + a.s32[2] + b.s32[2]);
    v.s32[0] = sshdClampS32(ctx, (int64_t)a.s32[1] + a.s32[0] + b.s32[0]);
    return v;
}

SSHD_FORCEINLINE SshdVr sshdVsumsws(SshdPpuContext* ctx, SshdVr a, SshdVr b) {
    SshdVr v = {};
    v.s32[0] = sshdClampS32(ctx, (int64_t)a.s32[3] + a.s32[2] + a.s32[1] + a.s32[0] + b.s32[0]);
    return v;
}

// Floating-point vector helpers. Each follows the body of the RPCS3 handler named in its comment,
// which is not always what the handler's `select<>` flag list suggests: the `exec` body decides where
// denormals are flushed and which operands `ppu_set_vnan` receives. Non-Java mode (VSCR[NJ]) is
// honoured per the context and NaN propagation is the "full" variant of `ppu_set_vnan`. Lanes use
// plain IEEE operations in round-to-nearest; denormals are flushed in software, never through MXCSR.

// RPCS3 `ppu_flush_denormal`: with NJ set, a lane whose exponent field is 0 keeps only its sign.
SSHD_FORCEINLINE SshdVr sshdFlushDenormals(const SshdPpuContext* ctx, SshdVr v) {
    SshdU32x4 denormal = (SshdU32x4)((v.u32 & 0x7f800000u) == 0u);
    denormal = denormal & (0u - (uint32_t)(ctx->vscr_nj & 1u));
    v.u32 = v.u32 & ~(denormal >> 1);
    return v;
}

SSHD_FORCEINLINE SshdU32x4 sshdIsNanBits(SshdU32x4 bits) { return (SshdU32x4)((bits & 0x7fffffffu) > 0x7f800000u); }

// RPCS3 `ppu_fix_vnan`: a NaN result becomes the default quiet NaN, whatever the host produced.
SSHD_FORCEINLINE SshdU32x4 sshdFixNanBits(SshdU32x4 r) {
    SshdU32x4 nan = sshdIsNanBits(r);
    return (r & ~nan) | (nan & 0x7fc00000u);
}

// RPCS3 `ppu_select_vnan`: operand `a` if it is a NaN (quieted, payload and sign kept), else `rest`.
SSHD_FORCEINLINE SshdU32x4 sshdSelectNan(SshdU32x4 a, SshdU32x4 rest) {
    SshdU32x4 na = sshdIsNanBits(a);
    return ((a | 0x7fc00000u) & na) | (rest & ~na);
}

// RPCS3 `ppu_set_vnan` with operands: the first NaN among the operands wins, otherwise the result
// with a NaN replaced by the default NaN.
SSHD_FORCEINLINE SshdU32x4 sshdSetVnan1(SshdU32x4 r, SshdU32x4 a) { return sshdSelectNan(a, sshdFixNanBits(r)); }

SSHD_FORCEINLINE SshdU32x4 sshdSetVnan2(SshdU32x4 r, SshdU32x4 a, SshdU32x4 b) {
    return sshdSelectNan(a, sshdSelectNan(b, sshdFixNanBits(r)));
}

// vaddfp, vsubfp (RPCS3 `VADDFP`, `VSUBFP`): flags use_nj fix_nj set_vnan fix_vnan; inputs and result
// are flushed, NaN operands a then b propagate.
SSHD_FORCEINLINE SshdVr sshdVaddfp(const SshdPpuContext* ctx, SshdVr a, SshdVr b) {
    a = sshdFlushDenormals(ctx, a);
    b = sshdFlushDenormals(ctx, b);
    SshdVr r;
    r.f32 = a.f32 + b.f32;
    r.u32 = sshdSetVnan2(r.u32, a.u32, b.u32);
    return sshdFlushDenormals(ctx, r);
}

SSHD_FORCEINLINE SshdVr sshdVsubfp(const SshdPpuContext* ctx, SshdVr a, SshdVr b) {
    a = sshdFlushDenormals(ctx, a);
    b = sshdFlushDenormals(ctx, b);
    SshdVr r;
    r.f32 = a.f32 - b.f32;
    r.u32 = sshdSetVnan2(r.u32, a.u32, b.u32);
    return sshdFlushDenormals(ctx, r);
}

// vmaddfp = a * c + b, vnmsubfp = -(a * c - b), both with a single rounding (RPCS3 `VMADDFP`,
// `VNMSUBFP`, `gv_fmafs`): flags use_nj fix_nj set_vnan fix_vnan, but `ppu_set_vnan` gets no operands
// so any NaN result, including one propagated from an operand, is the default NaN. vnmsubfp negates
// by sign-bit xor on the addend and on the result, as the handler does.
SSHD_FORCEINLINE SshdVr sshdVmaddfp(const SshdPpuContext* ctx, SshdVr a, SshdVr b, SshdVr c) {
    a = sshdFlushDenormals(ctx, a);
    b = sshdFlushDenormals(ctx, b);
    c = sshdFlushDenormals(ctx, c);
    SshdVr r;
    r.f32 = __builtin_elementwise_fma(a.f32, c.f32, b.f32);
    r.u32 = sshdFixNanBits(r.u32);
    return sshdFlushDenormals(ctx, r);
}

SSHD_FORCEINLINE SshdVr sshdVnmsubfp(const SshdPpuContext* ctx, SshdVr a, SshdVr b, SshdVr c) {
    a = sshdFlushDenormals(ctx, a);
    b = sshdFlushDenormals(ctx, b);
    c = sshdFlushDenormals(ctx, c);
    SshdVr r;
    r.f32 = __builtin_elementwise_fma(a.f32, c.f32, (SshdF32x4)(b.u32 ^ 0x80000000u));
    r.u32 = sshdFixNanBits(r.u32 ^ 0x80000000u);
    return sshdFlushDenormals(ctx, r);
}

// vmaxfp, vminfp (RPCS3 `VMAXFP`, `VMINFP`): the result is flushed but the inputs are not (the
// handlers apply no input flush although VMAXFP lists use_nj); NaN operands a then b propagate. For
// equal operands, including +0 and -0, the second operand is returned, which is what `gv_maxfs` /
// `gv_minfs` give as x86 `maxps` / `minps` (their definition is not among the reference sources).
SSHD_FORCEINLINE SshdVr sshdVmaxfp(const SshdPpuContext* ctx, SshdVr a, SshdVr b) {
    SshdU32x4 pick_a = (SshdU32x4)(a.f32 > b.f32);
    SshdVr r;
    r.u32 = sshdSetVnan2((a.u32 & pick_a) | (b.u32 & ~pick_a), a.u32, b.u32);
    return sshdFlushDenormals(ctx, r);
}

SSHD_FORCEINLINE SshdVr sshdVminfp(const SshdPpuContext* ctx, SshdVr a, SshdVr b) {
    SshdU32x4 pick_a = (SshdU32x4)(a.f32 < b.f32);
    SshdVr r;
    r.u32 = sshdSetVnan2((a.u32 & pick_a) | (b.u32 & ~pick_a), a.u32, b.u32);
    return sshdFlushDenormals(ctx, r);
}

// vrefp, vrsqrtefp (RPCS3 `VREFP`, `VRSQRTEFP`): exact reciprocal and reciprocal square root, which
// is within the architected estimate error. Flags use_nj fix_nj set_vnan fix_vnan; operand b.
SSHD_FORCEINLINE SshdVr sshdVrefp(const SshdPpuContext* ctx, SshdVr b) {
    SshdF32x4 one = {1.0f, 1.0f, 1.0f, 1.0f};
    b = sshdFlushDenormals(ctx, b);
    SshdVr r;
    r.f32 = one / b.f32;
    r.u32 = sshdSetVnan1(r.u32, b.u32);
    return sshdFlushDenormals(ctx, r);
}

SSHD_FORCEINLINE SshdVr sshdVrsqrtefp(const SshdPpuContext* ctx, SshdVr b) {
    SshdF32x4 one = {1.0f, 1.0f, 1.0f, 1.0f};
    b = sshdFlushDenormals(ctx, b);
    SshdVr r;
    r.f32 = one / __builtin_elementwise_sqrt(b.f32);
    r.u32 = sshdSetVnan1(r.u32, b.u32);
    return sshdFlushDenormals(ctx, r);
}

// vexptefp, vlogefp (RPCS3 `VEXPTEFP`, `VLOGEFP`): flag fix_vnan only, so no flush and no operand
// NaN propagation; a NaN result is the default NaN.
SSHD_FORCEINLINE SshdVr sshdVexptefp(SshdVr b) {
    SshdVr r;
    for (int i = 0; i < 4; i++) r.f32[i] = __builtin_exp2f(b.f32[i]);
    r.u32 = sshdFixNanBits(r.u32);
    return r;
}

SSHD_FORCEINLINE SshdVr sshdVlogefp(SshdVr b) {
    SshdVr r;
    for (int i = 0; i < 4; i++) r.f32[i] = __builtin_log2f(b.f32[i]);
    r.u32 = sshdFixNanBits(r.u32);
    return r;
}

// vrfin/vrfiz/vrfip/vrfim (RPCS3 `VRFIN`, `VRFIZ`, `VRFIP`, `VRFIM`): round to nearest even, toward
// zero, up, down; NaN operand b propagates. VRFIP and VRFIM flush the input as well (flags use_nj
// fix_nj), which matters: a negative denormal rounds down to -1.0 unflushed but to -0 flushed.
// VRFIN and VRFIZ flush the result only; flushing their input first gives the same result, so all
// four share one shape here.
#define SSHD_VMX_ROUND(fn, builtin)                                   \
    SSHD_FORCEINLINE SshdVr fn(const SshdPpuContext* ctx, SshdVr b) { \
        b = sshdFlushDenormals(ctx, b);                               \
        SshdVr r;                                                     \
        r.f32 = builtin(b.f32);                                       \
        r.u32 = sshdSetVnan1(r.u32, b.u32);                           \
        return sshdFlushDenormals(ctx, r);                            \
    }

SSHD_VMX_ROUND(sshdVrfin, __builtin_elementwise_roundeven)
SSHD_VMX_ROUND(sshdVrfiz, __builtin_elementwise_trunc)
SSHD_VMX_ROUND(sshdVrfip, __builtin_elementwise_ceil)
SSHD_VMX_ROUND(sshdVrfim, __builtin_elementwise_floor)
#undef SSHD_VMX_ROUND

// 2^e as a float for |e| <= 31, built from the exponent field so no libm call is needed.
SSHD_FORCEINLINE float sshdPow2f(int32_t e) {
    uint32_t bits = (uint32_t)(127 + e) << 23;
    float f;
    __builtin_memcpy(&f, &bits, sizeof f);
    return f;
}

// vcfsx/vcfux (RPCS3 `VCFSX`, `VCFUX`): convert and divide by 2^uimm. The handlers subtract the
// shift from the exponent field instead of multiplying; the two agree for every input because a
// nonzero integer converts to an exponent of at least 127 and the shift is at most 31.
SSHD_FORCEINLINE SshdVr sshdVcfsx(SshdVr b, uint32_t uimm) {
    SshdVr r;
    r.f32 = __builtin_convertvector(b.s32, SshdF32x4) * sshdPow2f(-(int32_t)uimm);
    return r;
}

SSHD_FORCEINLINE SshdVr sshdVcfux(SshdVr b, uint32_t uimm) {
    SshdVr r;
    r.f32 = __builtin_convertvector(b.u32, SshdF32x4) * sshdPow2f(-(int32_t)uimm);
    return r;
}

// vctsxs/vctuxs (RPCS3 `VCTSXS`, `VCTUXS`): multiply by 2^uimm, truncate, saturate. SAT is set when
// the scaled value is below the range (unsigned: below 0, even for -0.5) or at or above the upper
// bound; NaN gives 0 without SAT (flag fix_vnan, and the handler's comparisons are ordered). Lanes
// are range-checked before the C conversion, which is undefined for out-of-range values.
SSHD_FORCEINLINE SshdVr sshdVctsxs(SshdPpuContext* ctx, SshdVr b, uint32_t uimm) {
    SshdF32x4 scaled = b.f32 * sshdPow2f((int32_t)uimm);
    SshdVr r;
    int sat = 0;
    for (int i = 0; i < 4; i++) {
        float x = scaled[i];
        if (x < -2147483648.0f) {
            r.s32[i] = INT32_MIN;
            sat = 1;
        } else if (x >= 2147483648.0f) {
            r.s32[i] = INT32_MAX;
            sat = 1;
        } else if (x != x) {
            r.s32[i] = 0;
        } else {
            r.s32[i] = (int32_t)x;
        }
    }
    sshdSetSat(ctx, sat);
    return r;
}

SSHD_FORCEINLINE SshdVr sshdVctuxs(SshdPpuContext* ctx, SshdVr b, uint32_t uimm) {
    SshdF32x4 scaled = b.f32 * sshdPow2f((int32_t)uimm);
    SshdVr r;
    int sat = 0;
    for (int i = 0; i < 4; i++) {
        float x = scaled[i];
        if (x < 0.0f) {
            r.u32[i] = 0;
            sat = 1;
        } else if (x >= 4294967296.0f) {
            r.u32[i] = UINT32_MAX;
            sat = 1;
        } else if (x != x) {
            r.u32[i] = 0;
        } else {
            r.u32[i] = (uint32_t)x;
        }
    }
    sshdSetSat(ctx, sat);
    return r;
}

// vcmpbfp (RPCS3 `VCMPBFP`): bit 0 (value 0x80000000) is set when a > b or unordered, bit 1
// (0x40000000) when a < -b or unordered; a lane is within bounds when both are clear. The record
// form sets CR6 eq when every lane is in bounds (and lt = gt = so = 0); sshdSetCr6 produces exactly
// that because the result lanes can never be all ones.
SSHD_FORCEINLINE SshdVr sshdVcmpbfp(SshdVr a, SshdVr b) {
    SshdU32x4 above = ~(SshdU32x4)(a.f32 <= b.f32);
    SshdU32x4 below = ~(SshdU32x4)(a.f32 >= (SshdF32x4)(b.u32 ^ 0x80000000u));
    SshdVr r;
    r.u32 = (above & 0x80000000u) | ((below & 0x80000000u) >> 1);
    return r;
}
