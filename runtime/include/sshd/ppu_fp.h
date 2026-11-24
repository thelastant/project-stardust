#pragma once

#include "ppu_context.h"

// Scalar floating-point helpers for generated code. Arithmetic is plain double arithmetic compiled
// with -ffp-contract=off; fused operations use __builtin_fma explicitly.
//
// FPSCR is modelled only as far as RPCS3 does it: the FPCC field (bits 16..19 in big-endian
// numbering, 0xF000 in the packed image) is written by fcmpu/fcmpo, and mffs/mtfsf/mtfsfi/mtfsb*
// move the raw image. Exception flags (FX, VX, OX, UX, ZX, XX), the enables and the rounding
// control bits are never derived from an operation: rounding is always to nearest even, and
// non-IEEE mode is not modelled.

// Single-precision results are computed in double and rounded to float (RPCS3 `f32`); the value
// stays in the FPR as a double.
SSHD_FORCEINLINE double sshdRoundSingle(double x) { return (double)(float)x; }

// Saturating double -> int32 conversion of an already integral value `r` (or NaN). The result is
// the sign-extended 32-bit integer (RPCS3 `FCTIWZ`); NaN gives INT32_MIN. The guards keep the C
// conversion defined for every input.
SSHD_FORCEINLINE uint64_t sshdSaturateInt32(double r) {
    if (!(r == r)) return (uint64_t)(int64_t)INT32_MIN;
    if (r >= 2147483648.0) return (uint64_t)(int64_t)INT32_MAX;
    if (r < -2147483648.0) return (uint64_t)(int64_t)INT32_MIN;
    return (uint64_t)(int64_t)(int32_t)r;
}

SSHD_FORCEINLINE uint64_t sshdSaturateInt64(double r) {
    if (!(r == r)) return (uint64_t)INT64_MIN;
    if (r >= 9223372036854775808.0) return (uint64_t)INT64_MAX;
    if (r < -9223372036854775808.0) return (uint64_t)INT64_MIN;
    return (uint64_t)(int64_t)r;
}

// fctiw/fctid round to nearest even (the FPSCR rounding mode is not modelled); the z forms
// truncate (RPCS3 `FCTIW`, `FCTIWZ`, `FCTID`, `FCTIDZ`).
SSHD_FORCEINLINE uint64_t sshdFctiw(double b) { return sshdSaturateInt32(__builtin_rint(b)); }
SSHD_FORCEINLINE uint64_t sshdFctiwz(double b) { return sshdSaturateInt32(__builtin_trunc(b)); }
SSHD_FORCEINLINE uint64_t sshdFctid(double b) { return sshdSaturateInt64(__builtin_rint(b)); }
SSHD_FORCEINLINE uint64_t sshdFctidz(double b) { return sshdSaturateInt64(__builtin_trunc(b)); }

// fcmpu/fcmpo: CR field = {lt, gt, eq, un}; the same nibble goes to FPSCR.FPCC (RPCS3 `FCMPU`
// via `ppu_set_fpcc`). fcmpo is identical because no exception state is modelled.
SSHD_FORCEINLINE void sshdFcmp(SshdPpuContext* ctx, uint32_t field, double a, double b) {
    uint32_t un = __builtin_isnan(a) || __builtin_isnan(b);
    uint32_t nibble = ((uint32_t)(a < b) << 3) | ((uint32_t)(a > b) << 2) | ((uint32_t)(a == b) << 1) | un;
    sshdSetCrField(ctx, field, nibble);
    ctx->fpscr = (ctx->fpscr & ~0xF000u) | (nibble << 12);
}

// Rc=1 on a floating-point instruction: CR1 = FPSCR bits 0..3 (FX, FEX, VX, OX), as in the
// architecture. RPCS3 instead writes the FPCC bits of the result there; since the exception bits
// are never set here, CR1 only reflects what mtfsf/mtfsb1 put in them.
SSHD_FORCEINLINE void sshdRecordFp(SshdPpuContext* ctx) { sshdSetCrField(ctx, 1, ctx->fpscr >> 28); }
