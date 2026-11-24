#pragma once

// Instruction semantics helpers for generated SPU code. Each helper takes the source registers by
// value and returns the new register image, so `ctx->gpr[rt] = sshdSpuOp(ctx->gpr[ra], ...)` is
// correct when rt aliases a source. Lane-wise operations work on the host lanes directly, because
// SshdVr keeps one host lane per big-endian element (see spu_context.h); only operations that name an
// element (the preferred slot, doubleword halves) go through SSHD_VR_*.
//
// The harness builds with UBSan traps, so every shift count is in range, signed overflow is avoided
// by doing the arithmetic on unsigned lanes, and rotates are written without a shift by the width.
//
// Like the other contract headers this compiles as C23 and C++26 and with -ffreestanding.

#include "spu_context.h"

#if defined(__ARM_NEON)
#include <arm_neon.h>
#endif

#ifdef __cplusplus
extern "C" {
#endif

// --- Constants -------------------------------------------------------------------------------

SSHD_FORCEINLINE SshdVr sshdSpuSplatW(uint32_t x) {
    SshdVr v;
    v.u32 = (SshdU32x4){x, x, x, x};
    return v;
}

SSHD_FORCEINLINE SshdVr sshdSpuSplatH(uint16_t x) {
    SshdVr v;
    v.u16 = (SshdU16x8){x, x, x, x, x, x, x, x};
    return v;
}

SSHD_FORCEINLINE SshdVr sshdSpuSplatB(uint8_t x) {
    SshdVr v;
    v.u8 = (SshdU8x16){x, x, x, x, x, x, x, x, x, x, x, x, x, x, x, x};
    return v;
}

// --- Add and subtract ------------------------------------------------------------------------

SSHD_FORCEINLINE SshdVr sshdSpuA(SshdVr a, SshdVr b) {
    a.u32 = a.u32 + b.u32;
    return a;
}

SSHD_FORCEINLINE SshdVr sshdSpuAh(SshdVr a, SshdVr b) {
    a.u16 = a.u16 + b.u16;
    return a;
}

// Subtracts from. sf computes RB - RA.
SSHD_FORCEINLINE SshdVr sshdSpuSf(SshdVr a, SshdVr b) {
    b.u32 = b.u32 - a.u32;
    return b;
}

SSHD_FORCEINLINE SshdVr sshdSpuSfh(SshdVr a, SshdVr b) {
    b.u16 = b.u16 - a.u16;
    return b;
}

// Carry out of x + y + cin (cin is 0 or 1 per lane), as 0 or 1.
SSHD_FORCEINLINE SshdU32x4 sshdSpuCarryOut(SshdU32x4 x, SshdU32x4 y, SshdU32x4 cin) {
    SshdU32x4 sum = x + y;
    SshdU32x4 sum_c = sum + cin;
    return ((SshdU32x4)(sum < x) | (SshdU32x4)(sum_c < sum)) & 1u;
}

// The extended forms take their carry-in from the least significant bit of each word of RT.
SSHD_FORCEINLINE SshdVr sshdSpuAddx(SshdVr a, SshdVr b, SshdVr rt) {
    a.u32 = a.u32 + b.u32 + (rt.u32 & 1u);
    return a;
}

SSHD_FORCEINLINE SshdVr sshdSpuCg(SshdVr a, SshdVr b) {
    SshdVr r;
    r.u32 = sshdSpuCarryOut(a.u32, b.u32, (SshdU32x4){0, 0, 0, 0});
    return r;
}

SSHD_FORCEINLINE SshdVr sshdSpuCgx(SshdVr a, SshdVr b, SshdVr rt) {
    SshdVr r;
    r.u32 = sshdSpuCarryOut(a.u32, b.u32, rt.u32 & 1u);
    return r;
}

// RB - RA - 1 + carry-in, i.e. RB + ~RA + carry-in.
SSHD_FORCEINLINE SshdVr sshdSpuSfx(SshdVr a, SshdVr b, SshdVr rt) {
    b.u32 = b.u32 + ~a.u32 + (rt.u32 & 1u);
    return b;
}

// Borrow generate is the carry of RB + ~RA + 1: 1 when RB - RA does not borrow.
SSHD_FORCEINLINE SshdVr sshdSpuBg(SshdVr a, SshdVr b) {
    SshdVr r;
    r.u32 = (SshdU32x4)(b.u32 >= a.u32) & 1u;
    return r;
}

SSHD_FORCEINLINE SshdVr sshdSpuBgx(SshdVr a, SshdVr b, SshdVr rt) {
    SshdVr r;
    r.u32 = sshdSpuCarryOut(~a.u32, b.u32, rt.u32 & 1u);
    return r;
}

// --- Multiply --------------------------------------------------------------------------------

// Halfword views of the words: "lo" is the low halfword of each word (bits 16 to 31 in ISA numbering).
SSHD_FORCEINLINE SshdS32x4 sshdSpuLoS(SshdVr v) { return (SshdS32x4)(v.u32 << 16) >> 16; }
SSHD_FORCEINLINE SshdS32x4 sshdSpuHiS(SshdVr v) { return v.s32 >> 16; }
SSHD_FORCEINLINE SshdU32x4 sshdSpuLoU(SshdVr v) { return v.u32 & 0xffffu; }
SSHD_FORCEINLINE SshdU32x4 sshdSpuHiU(SshdVr v) { return v.u32 >> 16; }

// A 16 x 16 bit product always fits in 32 bits (at most 2^30 signed), so the signed lanes cannot overflow.
SSHD_FORCEINLINE SshdVr sshdSpuMpy(SshdVr a, SshdVr b) {
    SshdVr r;
    r.s32 = sshdSpuLoS(a) * sshdSpuLoS(b);
    return r;
}

SSHD_FORCEINLINE SshdVr sshdSpuMpyu(SshdVr a, SshdVr b) {
    SshdVr r;
    r.u32 = sshdSpuLoU(a) * sshdSpuLoU(b);
    return r;
}

// High halfword of RA times low halfword of RB, signed, shifted left 16.
SSHD_FORCEINLINE SshdVr sshdSpuMpyh(SshdVr a, SshdVr b) {
    SshdVr r;
    r.u32 = (SshdU32x4)(sshdSpuHiS(a) * sshdSpuLoS(b)) << 16;
    return r;
}

// Signed low halfword product shifted right 16 (arithmetic).
SSHD_FORCEINLINE SshdVr sshdSpuMpys(SshdVr a, SshdVr b) {
    SshdVr r;
    r.s32 = (sshdSpuLoS(a) * sshdSpuLoS(b)) >> 16;
    return r;
}

SSHD_FORCEINLINE SshdVr sshdSpuMpyhh(SshdVr a, SshdVr b) {
    SshdVr r;
    r.s32 = sshdSpuHiS(a) * sshdSpuHiS(b);
    return r;
}

SSHD_FORCEINLINE SshdVr sshdSpuMpyhha(SshdVr a, SshdVr b, SshdVr rt) {
    rt.u32 = rt.u32 + (SshdU32x4)(sshdSpuHiS(a) * sshdSpuHiS(b));
    return rt;
}

SSHD_FORCEINLINE SshdVr sshdSpuMpyhhu(SshdVr a, SshdVr b) {
    SshdVr r;
    r.u32 = sshdSpuHiU(a) * sshdSpuHiU(b);
    return r;
}

SSHD_FORCEINLINE SshdVr sshdSpuMpyhhau(SshdVr a, SshdVr b, SshdVr rt) {
    rt.u32 = rt.u32 + sshdSpuHiU(a) * sshdSpuHiU(b);
    return rt;
}

// RT = low(RA) * low(RB) + RC, signed halfwords, 32-bit sum.
SSHD_FORCEINLINE SshdVr sshdSpuMpya(SshdVr a, SshdVr b, SshdVr c) {
    c.u32 = c.u32 + (SshdU32x4)(sshdSpuLoS(a) * sshdSpuLoS(b));
    return c;
}

// --- Byte operations -------------------------------------------------------------------------

// (a + b + 1) >> 1 without the carry out of the byte: (a | b) - ((a ^ b) >> 1).
SSHD_FORCEINLINE SshdVr sshdSpuAvgb(SshdVr a, SshdVr b) {
    SshdVr r;
    r.u8 = (a.u8 | b.u8) - ((a.u8 ^ b.u8) >> 1);
    return r;
}

SSHD_FORCEINLINE SshdVr sshdSpuAbsdb(SshdVr a, SshdVr b) {
    SshdU8x16 gt = (SshdU8x16)(a.u8 > b.u8);
    SshdVr r;
    r.u8 = ((a.u8 - b.u8) & gt) | ((b.u8 - a.u8) & ~gt);
    return r;
}

SSHD_FORCEINLINE SshdU32x4 sshdSpuByteSum(SshdU32x4 w) {
    return (w & 0xffu) + ((w >> 8) & 0xffu) + ((w >> 16) & 0xffu) + (w >> 24);
}

// Per word: the sum of RB's four bytes in the upper halfword, the sum of RA's in the lower.
SSHD_FORCEINLINE SshdVr sshdSpuSumb(SshdVr a, SshdVr b) {
    SshdVr r;
    r.u32 = (sshdSpuByteSum(b.u32) << 16) | sshdSpuByteSum(a.u32);
    return r;
}

SSHD_FORCEINLINE SshdVr sshdSpuCntb(SshdVr a) {
    SshdU8x16 x = a.u8;
    x = x - ((x >> 1) & 0x55);
    x = (x & 0x33) + ((x >> 2) & 0x33);
    x = (x + (x >> 4)) & 0x0f;
    a.u8 = x;
    return a;
}

SSHD_FORCEINLINE SshdVr sshdSpuClz(SshdVr a) {
    for (uint32_t i = 0; i < 4; i++) {
        uint32_t w = a.u32[i];
        a.u32[i] = w != 0 ? (uint32_t)__builtin_clz(w) : 32u;
    }
    return a;
}

// --- Sign extension --------------------------------------------------------------------------

// Each halfword takes its low byte, sign-extended.
SSHD_FORCEINLINE SshdVr sshdSpuXsbh(SshdVr a) {
    a.s16 = (SshdS16x8)(a.u16 << 8) >> 8;
    return a;
}

SSHD_FORCEINLINE SshdVr sshdSpuXshw(SshdVr a) {
    a.s32 = (SshdS32x4)(a.u32 << 16) >> 16;
    return a;
}

// Each doubleword takes its low word (the odd word), sign-extended.
SSHD_FORCEINLINE SshdVr sshdSpuXswd(SshdVr a) {
    SshdVr r;
    for (uint32_t i = 0; i < 2; i++) {
        uint32_t lo = SSHD_VR_W(a, 2 * i + 1);
        SSHD_VR_W(r, 2 * i) = (uint32_t)((int32_t)lo >> 31);
        SSHD_VR_W(r, 2 * i + 1) = lo;
    }
    return r;
}

// --- Logical ---------------------------------------------------------------------------------

SSHD_FORCEINLINE SshdVr sshdSpuAnd(SshdVr a, SshdVr b) {
    a.u32 = a.u32 & b.u32;
    return a;
}

SSHD_FORCEINLINE SshdVr sshdSpuAndc(SshdVr a, SshdVr b) {
    a.u32 = a.u32 & ~b.u32;
    return a;
}

SSHD_FORCEINLINE SshdVr sshdSpuOr(SshdVr a, SshdVr b) {
    a.u32 = a.u32 | b.u32;
    return a;
}

SSHD_FORCEINLINE SshdVr sshdSpuOrc(SshdVr a, SshdVr b) {
    a.u32 = a.u32 | ~b.u32;
    return a;
}

SSHD_FORCEINLINE SshdVr sshdSpuXor(SshdVr a, SshdVr b) {
    a.u32 = a.u32 ^ b.u32;
    return a;
}

SSHD_FORCEINLINE SshdVr sshdSpuNand(SshdVr a, SshdVr b) {
    a.u32 = ~(a.u32 & b.u32);
    return a;
}

SSHD_FORCEINLINE SshdVr sshdSpuNor(SshdVr a, SshdVr b) {
    a.u32 = ~(a.u32 | b.u32);
    return a;
}

SSHD_FORCEINLINE SshdVr sshdSpuEqv(SshdVr a, SshdVr b) {
    a.u32 = ~(a.u32 ^ b.u32);
    return a;
}

// Bit select: RB where the RC bit is 1, RA where it is 0.
SSHD_FORCEINLINE SshdVr sshdSpuSelb(SshdVr a, SshdVr b, SshdVr c) {
    a.u32 = (b.u32 & c.u32) | (a.u32 & ~c.u32);
    return a;
}

// --- Compare ---------------------------------------------------------------------------------

SSHD_FORCEINLINE SshdVr sshdSpuCeqb(SshdVr a, SshdVr b) {
    SshdVr r;
    r.u8 = (SshdU8x16)(a.u8 == b.u8);
    return r;
}

SSHD_FORCEINLINE SshdVr sshdSpuCeqh(SshdVr a, SshdVr b) {
    SshdVr r;
    r.u16 = (SshdU16x8)(a.u16 == b.u16);
    return r;
}

SSHD_FORCEINLINE SshdVr sshdSpuCeq(SshdVr a, SshdVr b) {
    SshdVr r;
    r.u32 = (SshdU32x4)(a.u32 == b.u32);
    return r;
}

SSHD_FORCEINLINE SshdVr sshdSpuCgtb(SshdVr a, SshdVr b) {
    SshdVr r;
    r.u8 = (SshdU8x16)(a.s8 > b.s8);
    return r;
}

SSHD_FORCEINLINE SshdVr sshdSpuCgth(SshdVr a, SshdVr b) {
    SshdVr r;
    r.u16 = (SshdU16x8)(a.s16 > b.s16);
    return r;
}

SSHD_FORCEINLINE SshdVr sshdSpuCgt(SshdVr a, SshdVr b) {
    SshdVr r;
    r.u32 = (SshdU32x4)(a.s32 > b.s32);
    return r;
}

SSHD_FORCEINLINE SshdVr sshdSpuClgtb(SshdVr a, SshdVr b) {
    SshdVr r;
    r.u8 = (SshdU8x16)(a.u8 > b.u8);
    return r;
}

SSHD_FORCEINLINE SshdVr sshdSpuClgth(SshdVr a, SshdVr b) {
    SshdVr r;
    r.u16 = (SshdU16x8)(a.u16 > b.u16);
    return r;
}

SSHD_FORCEINLINE SshdVr sshdSpuClgt(SshdVr a, SshdVr b) {
    SshdVr r;
    r.u32 = (SshdU32x4)(a.u32 > b.u32);
    return r;
}

// --- Shift and rotate of elements ------------------------------------------------------------
//
// The count of each element comes from the same element of RB (the immediate forms splat it). Only
// the low bits take part: 6 for words and 5 for halfwords in the shifts (a count above the width gives
// 0, or sign fill for the arithmetic shifts), 5 and 4 in the rotates. The right shifts use the negated
// count, so a count of -n shifts right by n.

SSHD_FORCEINLINE SshdVr sshdSpuShl(SshdVr a, SshdVr b) {
    SshdU32x4 count = b.u32 & 0x3fu;
    a.u32 = (a.u32 << (count & 31u)) & (SshdU32x4)(count < 32u);
    return a;
}

SSHD_FORCEINLINE SshdVr sshdSpuShlh(SshdVr a, SshdVr b) {
    SshdU16x8 count = b.u16 & 0x1f;
    a.u16 = (a.u16 << (count & 15)) & (SshdU16x8)(count < 16);
    return a;
}

// A rotate by 0 would shift by the width, so the complementary count is reduced modulo the width.
SSHD_FORCEINLINE SshdVr sshdSpuRot(SshdVr a, SshdVr b) {
    SshdU32x4 count = b.u32 & 31u;
    a.u32 = (a.u32 << count) | (a.u32 >> ((32u - count) & 31u));
    return a;
}

SSHD_FORCEINLINE SshdVr sshdSpuRoth(SshdVr a, SshdVr b) {
    SshdU16x8 count = b.u16 & 15;
    a.u16 = (a.u16 << count) | (a.u16 >> ((16 - count) & 15));
    return a;
}

SSHD_FORCEINLINE SshdVr sshdSpuRotm(SshdVr a, SshdVr b) {
    SshdU32x4 count = (0u - b.u32) & 0x3fu;
    a.u32 = (a.u32 >> (count & 31u)) & (SshdU32x4)(count < 32u);
    return a;
}

SSHD_FORCEINLINE SshdVr sshdSpuRothm(SshdVr a, SshdVr b) {
    SshdU16x8 count = (0 - b.u16) & 0x1f;
    a.u16 = (a.u16 >> (count & 15)) & (SshdU16x8)(count < 16);
    return a;
}

// An arithmetic shift by the width or more gives the sign fill, which a shift by width - 1 also gives.
SSHD_FORCEINLINE SshdVr sshdSpuRotma(SshdVr a, SshdVr b) {
    SshdU32x4 count = (0u - b.u32) & 0x3fu;
    count = count ^ ((count ^ 31u) & (SshdU32x4)(count > 31u));
    a.s32 = a.s32 >> count;
    return a;
}

SSHD_FORCEINLINE SshdVr sshdSpuRotmah(SshdVr a, SshdVr b) {
    SshdU16x8 count = (0 - b.u16) & 0x1f;
    count = count ^ ((count ^ 15) & (SshdU16x8)(count > 15));
    a.s16 = a.s16 >> count;
    return a;
}

// --- Quadword shifts and rotates -------------------------------------------------------------
//
// With the byte-reversed lanes the whole register is one little-endian 128-bit integer whose top bit
// is bit 0 of the architected register, so an architected left shift is a plain left shift of that
// integer and a byte shift is a shift by 8 times the count. Every helper takes the count word the
// instruction names (the preferred word of RB, or the sign-extended I7) and applies its own mask, so
// the register and immediate forms share the code.

#if defined(__SSSE3__) || defined(__ARM_NEON)
// The vector forms below avoid unsigned __int128: its shifts compile to a pair of 64-bit GPR halves, which are written
// to the register file as two 8-byte stores, and the next 16-byte load of that register cannot be forwarded from two
// stores. The byte forms are one pshufb (tbl on arm64) with a computed control; the bit forms are two-step shifts of
// the u64 lanes.

// pshufb: lane h of the result is lane control[h] & 15 of a, or 0 when bit 7 of control[h] is set.
SSHD_FORCEINLINE SshdVr sshdSpuQwPermute(SshdVr a, SshdU8x16 control) {
    SshdVr r;
#if defined(__SSSE3__)
    typedef char SshdSpuQwC8x16 __attribute__((vector_size(16)));
    r.u8 = (SshdU8x16)__builtin_ia32_pshufb128((SshdSpuQwC8x16)a.u8, (SshdSpuQwC8x16)control);
#else
    // tbl gives 0 for an index of 16 or more, so keeping bit 7 and bits 0-3 gives pshufb's rule.
    r.u8 = (SshdU8x16)vqtbl1q_u8((uint8x16_t)a.u8, (uint8x16_t)(control & (uint8_t)0x8f));
#endif
    return r;
}

SSHD_FORCEINLINE SshdU8x16 sshdSpuQwLanes(void) {
    const SshdU8x16 lanes = {0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15};
    return lanes;
}

// The bit shifts move the bits that cross from one u64 lane to the other in two steps (by 1, then by 63 - n) because a
// vector shift by 64 gives 0, which covers n = 0 without a branch.
SSHD_FORCEINLINE SshdVr sshdSpuShlqbi(SshdVr a, uint32_t count) {
    const uint32_t n = count & 7u;
    const SshdU64x2 x = a.u64;
    const SshdU64x2 zero = {0, 0};
    const SshdU64x2 low_up = __builtin_shufflevector(zero, x, 0, 2);  // {0, lo}
    a.u64 = (x << n) | ((low_up >> 1) >> (63u - n));
    return a;
}

SSHD_FORCEINLINE SshdVr sshdSpuShlqby(SshdVr a, uint32_t count) {
    // Lanes below the count wrap to 0xe1..0xff, whose bit 7 makes pshufb write zero; a count of 16 to 31 does that
    // for every lane.
    return sshdSpuQwPermute(a, sshdSpuQwLanes() - (uint8_t)(count & 0x1fu));
}

// The "from bit shift count" forms take the byte count from bits 24 to 28 of the word.
SSHD_FORCEINLINE SshdVr sshdSpuShlqbybi(SshdVr a, uint32_t count) { return sshdSpuShlqby(a, count >> 3); }

SSHD_FORCEINLINE SshdVr sshdSpuRotqbi(SshdVr a, uint32_t count) {
    const uint32_t n = count & 7u;
    const SshdU64x2 x = a.u64;
    const SshdU64x2 swapped = __builtin_shufflevector(x, x, 1, 0);
    a.u64 = (x << n) | ((swapped >> 1) >> (63u - n));
    return a;
}

SSHD_FORCEINLINE SshdVr sshdSpuRotqby(SshdVr a, uint32_t count) {
    return sshdSpuQwPermute(a, (sshdSpuQwLanes() - (uint8_t)(count & 15u)) & (uint8_t)15);
}

SSHD_FORCEINLINE SshdVr sshdSpuRotqbybi(SshdVr a, uint32_t count) { return sshdSpuRotqby(a, count >> 3); }

// The mask forms shift right by the negated count.
SSHD_FORCEINLINE SshdVr sshdSpuRotqmbi(SshdVr a, uint32_t count) {
    const uint32_t s = (0u - count) & 7u;
    const SshdU64x2 x = a.u64;
    const SshdU64x2 zero = {0, 0};
    const SshdU64x2 high_down = __builtin_shufflevector(x, zero, 1, 2);  // {hi, 0}
    a.u64 = (x >> s) | ((high_down << 1) << (63u - s));
    return a;
}

SSHD_FORCEINLINE SshdVr sshdSpuRotqmby(SshdVr a, uint32_t count) {
    // 0x70 + lane + s keeps bit 7 clear exactly while lane + s < 16, and its low four bits are then lane + s.
    return sshdSpuQwPermute(a, sshdSpuQwLanes() + (uint8_t)(0x70u + ((0u - count) & 0x1fu)));
}

SSHD_FORCEINLINE SshdVr sshdSpuRotqmbybi(SshdVr a, uint32_t count) { return sshdSpuRotqmby(a, count >> 3); }
#else
__extension__ typedef unsigned __int128 SshdSpuU128;

SSHD_FORCEINLINE SshdSpuU128 sshdSpuToInt(SshdVr v) {
    SshdSpuU128 x;
    __builtin_memcpy(&x, &v, sizeof x);
    return x;
}

SSHD_FORCEINLINE SshdVr sshdSpuFromInt(SshdSpuU128 x) {
    SshdVr v;
    __builtin_memcpy(&v, &x, sizeof v);
    return v;
}

// Rotate by 0 < n < 128 bits, and by 0 without a shift by the width.
SSHD_FORCEINLINE SshdSpuU128 sshdSpuRotl128(SshdSpuU128 x, uint32_t n) { return (x << n) | (x >> ((128u - n) & 127u)); }

SSHD_FORCEINLINE SshdVr sshdSpuShlqbi(SshdVr a, uint32_t count) {
    return sshdSpuFromInt(sshdSpuToInt(a) << (count & 7u));
}

// A byte count above 15 shifts every byte out.
SSHD_FORCEINLINE SshdVr sshdSpuShlqby(SshdVr a, uint32_t count) {
    uint32_t s = count & 0x1fu;
    return sshdSpuFromInt(s < 16u ? sshdSpuToInt(a) << (8u * s) : (SshdSpuU128)0);
}

// The "from bit shift count" forms take the byte count from bits 24 to 28 of the word.
SSHD_FORCEINLINE SshdVr sshdSpuShlqbybi(SshdVr a, uint32_t count) { return sshdSpuShlqby(a, count >> 3); }

SSHD_FORCEINLINE SshdVr sshdSpuRotqbi(SshdVr a, uint32_t count) {
    return sshdSpuFromInt(sshdSpuRotl128(sshdSpuToInt(a), count & 7u));
}

SSHD_FORCEINLINE SshdVr sshdSpuRotqby(SshdVr a, uint32_t count) {
    return sshdSpuFromInt(sshdSpuRotl128(sshdSpuToInt(a), 8u * (count & 15u)));
}

SSHD_FORCEINLINE SshdVr sshdSpuRotqbybi(SshdVr a, uint32_t count) { return sshdSpuRotqby(a, count >> 3); }

// The mask forms shift right by the negated count.
SSHD_FORCEINLINE SshdVr sshdSpuRotqmbi(SshdVr a, uint32_t count) {
    return sshdSpuFromInt(sshdSpuToInt(a) >> ((0u - count) & 7u));
}

SSHD_FORCEINLINE SshdVr sshdSpuRotqmby(SshdVr a, uint32_t count) {
    uint32_t s = (0u - count) & 0x1fu;
    return sshdSpuFromInt(s < 16u ? sshdSpuToInt(a) >> (8u * s) : (SshdSpuU128)0);
}

SSHD_FORCEINLINE SshdVr sshdSpuRotqmbybi(SshdVr a, uint32_t count) { return sshdSpuRotqmby(a, count >> 3); }
#endif

// --- Shuffle and generate controls -----------------------------------------------------------

// Byte k (architected numbering) of RA || RB is selected by the matching byte of RC. The control
// bytes are read lane by lane because RC and RT share the same lane order.
SSHD_FORCEINLINE SshdVr sshdSpuShufb(SshdVr a, SshdVr b, SshdVr c) {
#if defined(__SSSE3__)
    // pshufb uses only bits 0-3 of a control byte (bits 4-6 are ignored) and gives 0 when bit 7 is set. Architected
    // byte k is host lane 15 - k, so sel ^ 0x0f is the lane index reversed with bit 7 kept: every special control
    // (1xxxxxxx) gives 0 from both pshufbs, which already is the 10xxxxxx result. Bit 4 picks RB.
    typedef char SshdSpuC8x16 __attribute__((vector_size(16)));
    const SshdU8x16 sel = c.u8;
    const SshdU8x16 lane = sel ^ (uint8_t)0x0f;
    const SshdU8x16 from_a = (SshdU8x16)__builtin_ia32_pshufb128((SshdSpuC8x16)a.u8, (SshdSpuC8x16)lane);
    const SshdU8x16 from_b = (SshdU8x16)__builtin_ia32_pshufb128((SshdSpuC8x16)b.u8, (SshdSpuC8x16)lane);
    const SshdU8x16 use_b = (SshdU8x16)((sel & (uint8_t)0x10) != (uint8_t)0);
    const SshdU8x16 v = (from_a & ~use_b) | (from_b & use_b);
    // 110xxxxx gives 0xff and 111xxxxx gives 0x80. Compares rather than a table, so the compiler drops them when
    // it knows the control has no special byte (a generate-controls result).
    const SshdU8x16 high = sel & (uint8_t)0xe0;
    const SshdU8x16 ones = (SshdU8x16)(high == (uint8_t)0xc0);
    const SshdU8x16 sign = (SshdU8x16)(high == (uint8_t)0xe0);
    const SshdU8x16 special = ones | (sign & (uint8_t)0x80);
    SshdVr r;
    r.u8 = v | special;
    return r;
#elif defined(__ARM_NEON)
    // Architected byte k of RA || RB is entry k ^ 0x0f of the table a, b (host lanes reversed within each register).
    // Keeping bit 7 makes every special control (1xxxxxxx) index past the table, which reads 0: the 10xxxxxx
    // result. The other special results are ORed in as above.
    const SshdU8x16 sel = c.u8;
    const uint8x16x2_t ab = {{(uint8x16_t)a.u8, (uint8x16_t)b.u8}};
    const SshdU8x16 v = (SshdU8x16)vqtbl2q_u8(ab, (uint8x16_t)((sel & (uint8_t)0x9f) ^ (uint8_t)0x0f));
    const SshdU8x16 high = sel & (uint8_t)0xe0;
    const SshdU8x16 ones = (SshdU8x16)(high == (uint8_t)0xc0);
    const SshdU8x16 sign = (SshdU8x16)(high == (uint8_t)0xe0);
    SshdVr r;
    r.u8 = v | ones | (sign & (uint8_t)0x80);
    return r;
#else
    uint8_t concat[32];
    for (uint32_t k = 0; k < 16; k++) {
        concat[k] = a.u8[15 - k];
        concat[16 + k] = b.u8[15 - k];
    }
    SshdVr r;
    for (uint32_t lane = 0; lane < 16; lane++) {
        uint8_t sel = c.u8[lane];
        uint8_t v = concat[sel & 0x1fu];
        if ((sel & 0xc0u) == 0x80u) {
            v = 0x00;
        } else if ((sel & 0xe0u) == 0xc0u) {
            v = 0xff;
        } else if ((sel & 0xe0u) == 0xe0u) {
            v = 0x80;
        }
        r.u8[lane] = v;
    }
    return r;
#endif
}

// The mask for inserting an element of `size` bytes (1, 2, 4 or 8) at the aligned position of `addr`:
// bytes 0x10..0x1f select RB-side data (the old quadword), the element's own bytes select the
// right-aligned bytes of the preferred slot (the leftmost eight for a doubleword).
// Built with a lane compare and blend: byte stores at a variable index into the vector make the compiler spill it to
// the stack and reload it, and the reload cannot be forwarded from the byte stores.
SSHD_FORCEINLINE SshdVr sshdSpuGenControl(uint32_t addr, uint32_t size) {
    // Host lane h holds architected byte 15 - h (SSHD_VR_B), so `byte` is the architected index of each lane.
    const SshdU8x16 byte = {15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0};
    const uint8_t t = (uint8_t)(addr & (16u - size));
    const uint8_t first = (uint8_t)(size < 8u ? 4u - size : 0u);
    // Bytes before t wrap above 15, so one unsigned compare finds the element's own bytes.
    const SshdU8x16 pos = byte - t;
    const SshdU8x16 inside = (SshdU8x16)(pos < (uint8_t)size);
    SshdVr r;
    r.u8 = (inside & (pos + first)) | (~inside & (byte + (uint8_t)0x10));
    return r;
}

// --- Form select mask and gather bits --------------------------------------------------------
//
// The leftmost element takes the most significant of the selected bits, which is the highest lane.

SSHD_FORCEINLINE SshdVr sshdSpuFsmb(uint32_t s) {
    SshdVr r;
    for (uint32_t lane = 0; lane < 16; lane++) r.u8[lane] = (uint8_t)(0u - ((s >> lane) & 1u));
    return r;
}

SSHD_FORCEINLINE SshdVr sshdSpuFsmh(uint32_t s) {
    SshdVr r;
    for (uint32_t lane = 0; lane < 8; lane++) r.u16[lane] = (uint16_t)(0u - ((s >> lane) & 1u));
    return r;
}

SSHD_FORCEINLINE SshdVr sshdSpuFsm(uint32_t s) {
    SshdVr r;
    for (uint32_t lane = 0; lane < 4; lane++) r.u32[lane] = 0u - ((s >> lane) & 1u);
    return r;
}

// The gathered bits go to the preferred word; the other words are zero.
SSHD_FORCEINLINE SshdVr sshdSpuGatherResult(uint32_t bits) {
    SshdVr r = sshdSpuSplatW(0);
    SSHD_VR_W(r, 0) = bits;
    return r;
}

SSHD_FORCEINLINE SshdVr sshdSpuGbb(SshdVr a) {
    uint32_t bits = 0;
    for (uint32_t lane = 0; lane < 16; lane++) bits |= (uint32_t)(a.u8[lane] & 1u) << lane;
    return sshdSpuGatherResult(bits);
}

SSHD_FORCEINLINE SshdVr sshdSpuGbh(SshdVr a) {
    uint32_t bits = 0;
    for (uint32_t lane = 0; lane < 8; lane++) bits |= (uint32_t)(a.u16[lane] & 1u) << lane;
    return sshdSpuGatherResult(bits);
}

SSHD_FORCEINLINE SshdVr sshdSpuGb(SshdVr a) {
    uint32_t bits = 0;
    for (uint32_t lane = 0; lane < 4; lane++) bits |= (a.u32[lane] & 1u) << lane;
    return sshdSpuGatherResult(bits);
}

SSHD_FORCEINLINE SshdVr sshdSpuOrx(SshdVr a) { return sshdSpuGatherResult(a.u32[0] | a.u32[1] | a.u32[2] | a.u32[3]); }

// --- Floating-point status and control register ------------------------------------------------
//
// fscrwr stores a register as it is (its unused bits are undefined); fscrrd returns the register with
// the unused bits forced to zero. The used bits per word are the rounding controls (bits 20:23 of
// word 0), the IEEE exception flags (50:55 of word 1, 82:87 of word 2), the single-precision flags
// (29:31, 61:63, 93:95, 125:127) and the divide-by-zero flags (116:119 of word 3).
SSHD_FORCEINLINE SshdVr sshdSpuFscrRead(SshdVr fpscr) {
    // Host lanes run opposite to the big-endian words, so lane 0 holds word 3.
    const SshdU32x4 used = {0x00000f07u, 0x00003f07u, 0x00003f07u, 0x00000f07u};
    fpscr.u32 = fpscr.u32 & used;
    return fpscr;
}

#ifdef __cplusplus
}  // extern "C"
#endif

// The floating-point helpers build on the types above.
#include "spu_fp.h"
