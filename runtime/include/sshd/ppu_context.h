#pragma once

// Contract between generated PPU code and the runtime: types, the CPU context, runtime hooks, and
// the small inline helpers that emitted code calls. Generated code is C23; the runtime is C++26,
// so this header compiles as both (it is wrapped in extern "C" for C++). Only <stdint.h> and
// <stddef.h> are included; clang builtins replace the C library string and math headers so that
// each generated translation unit parses little and the header can be checked with -ffreestanding.
//
// Naming: functions camelCase, types PascalCase, constants kPascalCase (as enumerators), macros
// SSHD_*. The generated code adds address-derived identifiers that deliberately do not follow the
// table: fn_%08x (functions), imp_%08x (import trampolines), L_%08x (labels), and the semantics
// harness uses case_<name>. A module other than the main executable has its functions and
// trampolines prefixed with the module's symbol prefix (libsre_fn_%08x).
//
// Guest memory: `base` points at a flat, big-endian guest address space. The runtime must reserve
// 4 GiB + kSshdGuestGuardSize bytes so that an unaligned access at the top of the 32-bit space
// stays inside the allocation. All guest accesses go through sshdRead*/sshdWrite* (any alignment).
//
// Vector registers: host lane i holds big-endian element (n - 1 - i), i.e. the whole 16-byte
// register is byte-reversed relative to guest memory (RPCS3 PPUInterpreter / PPUThread.h `v128`
// byte reversal; XenonRecomp `XenonUtils/ppc_context.h` does the same). Element-wise operations
// then need no index mapping; use the SSHD_VR_* accessors for big-endian element numbers.
//
// Condition register: one byte (0 or 1) per CR bit, bit 4*field + {0 lt, 1 gt, 2 eq, 3 so/un}, as
// in RPCS3 `cr_bits` and XenonRecomp `PPCCRRegister`. XER is unpacked the same way.
//
// HLE boundary: there is no per-import function. Each import is an index into the generated
// kSshdPpuImports table (stub address, FNID, module, name); emitted code calls SSHD_HLE(index) and
// the runtime resolves (module, FNID) to an implementation. A module's trampolines call
// SSHD_HLE_IMPORT with a pointer into that module's own import table, so the indices of two images
// never collide.

#include <stddef.h>
#include <stdint.h>

#include "compiler.h"

#ifdef __cplusplus
extern "C" {
#endif

#define SSHD_FORCEINLINE static inline __attribute__((always_inline))

enum SshdPpuLimits {
    kSshdTraceSize = 64,
    kSshdDcbzLine = 128,
    kSshdPreemptBudget = 4096,
    kSshdGuestGuardSize = 0x10000
};

enum SshdSyncKind { kSshdSyncHeavy = 0, kSshdSyncLight = 1, kSshdSyncEieio = 2, kSshdSyncIsync = 3 };

typedef uint8_t SshdU8x16 __attribute__((vector_size(16)));
typedef int8_t SshdS8x16 __attribute__((vector_size(16)));
typedef uint16_t SshdU16x8 __attribute__((vector_size(16)));
typedef int16_t SshdS16x8 __attribute__((vector_size(16)));
typedef uint32_t SshdU32x4 __attribute__((vector_size(16)));
typedef int32_t SshdS32x4 __attribute__((vector_size(16)));
typedef uint64_t SshdU64x2 __attribute__((vector_size(16)));
typedef float SshdF32x4 __attribute__((vector_size(16)));

// The explicit alignment keeps 16 inside the MSVC STL's #pragma pack(8) templates (std::array, std::optional, ...);
// a bare vector_size type is capped to 8 there, and the vector loads on its elements then fault.
typedef union __attribute__((aligned(16))) SshdVr {
    SshdU8x16 u8;
    SshdS8x16 s8;
    SshdU16x8 u16;
    SshdS16x8 s16;
    SshdU32x4 u32;
    SshdS32x4 s32;
    SshdU64x2 u64;
    SshdF32x4 f32;
} SshdVr;

// Big-endian element i of a register (see the lane layout note above).
#define SSHD_VR_B(v, i) ((v).u8[15 - (i)])
#define SSHD_VR_SB(v, i) ((v).s8[15 - (i)])
#define SSHD_VR_H(v, i) ((v).u16[7 - (i)])
#define SSHD_VR_SH(v, i) ((v).s16[7 - (i)])
#define SSHD_VR_W(v, i) ((v).u32[3 - (i)])
#define SSHD_VR_SW(v, i) ((v).s32[3 - (i)])
#define SSHD_VR_F(v, i) ((v).f32[3 - (i)])
#define SSHD_VR_D(v, i) ((v).u64[1 - (i)])

// Both views are needed: fctiwz/stfiwx/fneg operate on the bit pattern.
typedef union SshdFpr {
    double f64;
    uint64_t u64;
} SshdFpr;

typedef struct SshdPpuContext {
    uint64_t gpr[32];
    SshdFpr fpr[32];
    SshdVr vr[32];
    uint8_t cr[32];
    uint64_t lr, ctr;
    uint8_t xer_so, xer_ov, xer_ca, xer_count;
    uint32_t fpscr;
    uint32_t vrsave;
    uint8_t vscr_nj, vscr_sat;        // NJ defaults to 1 (RPCS3 PPUThread.h `nj = true`); SAT is sticky
    uint32_t jump_target, jump_site;  // dynamic transfers (input of sshdDispatch)
    uint32_t reserve_addr;
    uint64_t reserve_value;
    uint64_t reserve_version;  // reservation stripe version at lwarx/ldarx
    uint8_t reserve_valid;
    int32_t preempt_budget;
    uint32_t trace_pos;
    uint32_t trace[kSshdTraceSize];
    void* runtime;  // owned by the runtime (thread object)
} SshdPpuContext;

static_assert(sizeof(SshdVr) == 16, "SshdVr must be 16 bytes");
static_assert(alignof(SshdVr) == 16, "SshdVr must be 16-byte aligned");
static_assert(sizeof(SshdFpr) == 8, "SshdFpr must be 8 bytes");
static_assert((kSshdTraceSize & (kSshdTraceSize - 1)) == 0, "trace ring size must be a power of two");
static_assert(offsetof(SshdPpuContext, vr) % 16 == 0, "vr[] must stay 16-byte aligned");

typedef void SshdPpuFunc(SshdPpuContext* __restrict ctx, uint8_t* __restrict base);
typedef struct SshdFuncMapping {
    uint32_t guest;
    SshdPpuFunc* host;
} SshdFuncMapping;
typedef struct SshdImportInfo {
    uint32_t stub;
    uint32_t fnid;
    const char* module;
    const char* name;
} SshdImportInfo;

// A function export of a module: the guest address of its descriptor {entry, toc}.
typedef struct SshdExportInfo {
    uint32_t opd;
    uint32_t fnid;
    const char* module;
} SshdExportInfo;

#define SSHD_PPU_FUNC(name) SSHD_NOINLINE void name(SshdPpuContext* __restrict ctx, uint8_t* __restrict base)

// Runtime hooks, implemented by the runtime.
void sshdDispatch(SshdPpuContext* __restrict ctx, uint8_t* __restrict base);  // target = jump_target
void sshdUnknownTarget(SshdPpuContext* ctx, uint8_t* base, uint32_t target, uint32_t site);
void sshdRegisterFuncs(const SshdFuncMapping* mappings, uint32_t count);
SshdPpuFunc* sshdLookup(uint32_t addr);  // nullptr when unknown
void sshdHleCall(SshdPpuContext* ctx, uint8_t* base, uint32_t import_index);
// Import call from the trampoline of a module other than the main executable; `import` points into
// that module's own import table.
void sshdHleCallImport(SshdPpuContext* ctx, uint8_t* base, const SshdImportInfo* import);
void sshdSyscall(SshdPpuContext* ctx, uint8_t* base, uint32_t site);  // number read from r11
void sshdYield(SshdPpuContext* ctx, uint8_t* base);
[[noreturn]] void sshdUnhandled(SshdPpuContext* ctx, uint8_t* base, uint32_t site, uint32_t word);
[[noreturn]] void sshdTrap(SshdPpuContext* ctx, uint8_t* base, uint32_t site);
uint32_t sshdLwarx(SshdPpuContext* ctx, uint8_t* base, uint32_t ea);
uint64_t sshdLdarx(SshdPpuContext* ctx, uint8_t* base, uint32_t ea);
bool sshdStwcx(SshdPpuContext* ctx, uint8_t* base, uint32_t ea, uint32_t value);
bool sshdStdcx(SshdPpuContext* ctx, uint8_t* base, uint32_t ea, uint64_t value);
// Same arguments and value order as sshdWriteN, but the store goes through the reservation table: it bumps the stripe
// version of the line and wakes the waiters on it, like an SPU PUT. For the few plain stores that an SPU waits on
// (see docs/spu.md); a plain sshdWriteN is invisible to a sleeping waiter. Any alignment; a store that is not
// naturally aligned is split into byte stores.
void sshdNotifyWrite8(uint8_t* base, uint32_t ea, uint8_t value);
void sshdNotifyWrite16(uint8_t* base, uint32_t ea, uint16_t value);
void sshdNotifyWrite32(uint8_t* base, uint32_t ea, uint32_t value);
void sshdNotifyWrite64(uint8_t* base, uint32_t ea, uint64_t value);
void sshdDcbz(SshdPpuContext* ctx, uint8_t* base, uint32_t ea);
uint64_t sshdMftb(SshdPpuContext* ctx);
uint64_t sshdMffs(SshdPpuContext* ctx);                                    // FPSCR image in the low 32 bits
void sshdMtfsf(SshdPpuContext* ctx, uint32_t field_mask, uint64_t value);  // bit 0x80 >> i = field i
void sshdMtfsb(SshdPpuContext* ctx, uint32_t bit, uint32_t value);

// Defined by generated code (ppu_tables.c).
extern const SshdFuncMapping kSshdPpuFuncMappings[];
extern const uint32_t kSshdPpuFuncMappingCount;
extern const SshdImportInfo kSshdPpuImports[];
extern const uint32_t kSshdPpuImportCount;
extern const char kSshdPpuExecutableSha256[];

// Guest memory. Addresses are uint32_t (the guest address space is 32-bit); every access is
// big-endian and may be unaligned.
SSHD_FORCEINLINE uint8_t sshdRead8(const uint8_t* base, uint32_t ea) { return base[ea]; }

SSHD_FORCEINLINE uint16_t sshdRead16(const uint8_t* base, uint32_t ea) {
    uint16_t v;
    __builtin_memcpy(&v, base + ea, sizeof v);
    return __builtin_bswap16(v);
}

SSHD_FORCEINLINE uint32_t sshdRead32(const uint8_t* base, uint32_t ea) {
    uint32_t v;
    __builtin_memcpy(&v, base + ea, sizeof v);
    return __builtin_bswap32(v);
}

SSHD_FORCEINLINE uint64_t sshdRead64(const uint8_t* base, uint32_t ea) {
    uint64_t v;
    __builtin_memcpy(&v, base + ea, sizeof v);
    return __builtin_bswap64(v);
}

SSHD_FORCEINLINE void sshdWrite8(uint8_t* base, uint32_t ea, uint8_t value) { base[ea] = value; }

SSHD_FORCEINLINE void sshdWrite16(uint8_t* base, uint32_t ea, uint16_t value) {
    value = __builtin_bswap16(value);
    __builtin_memcpy(base + ea, &value, sizeof value);
}

SSHD_FORCEINLINE void sshdWrite32(uint8_t* base, uint32_t ea, uint32_t value) {
    value = __builtin_bswap32(value);
    __builtin_memcpy(base + ea, &value, sizeof value);
}

SSHD_FORCEINLINE void sshdWrite64(uint8_t* base, uint32_t ea, uint64_t value) {
    value = __builtin_bswap64(value);
    __builtin_memcpy(base + ea, &value, sizeof value);
}

// lvx/stvx semantics: the low four address bits are ignored.
SSHD_FORCEINLINE SshdVr sshdReadVr(const uint8_t* base, uint32_t ea) {
    SshdU8x16 raw;
    __builtin_memcpy(&raw, base + (ea & ~15u), sizeof raw);
    SshdVr v;
    v.u8 = __builtin_shufflevector(raw, raw, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
    return v;
}

SSHD_FORCEINLINE void sshdWriteVr(uint8_t* base, uint32_t ea, SshdVr value) {
    SshdU8x16 raw = __builtin_shufflevector(value.u8, value.u8, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
    __builtin_memcpy(base + (ea & ~15u), &raw, sizeof raw);
}

// Words are given in big-endian element order (w0 is element 0).
SSHD_FORCEINLINE SshdVr sshdVrFromBeWords(uint32_t w0, uint32_t w1, uint32_t w2, uint32_t w3) {
    SshdU32x4 lanes = {w3, w2, w1, w0};
    SshdVr v;
    v.u32 = lanes;
    return v;
}

SSHD_FORCEINLINE uint32_t sshdVrBeWord(SshdVr v, uint32_t i) { return v.u32[3 - (i & 3)]; }

// Condition register and XER.
SSHD_FORCEINLINE void sshdSetCrField(SshdPpuContext* ctx, uint32_t field, uint32_t nibble) {
    uint8_t* bits = &ctx->cr[4 * field];
    bits[0] = (nibble >> 3) & 1;
    bits[1] = (nibble >> 2) & 1;
    bits[2] = (nibble >> 1) & 1;
    bits[3] = nibble & 1;
}

SSHD_FORCEINLINE uint32_t sshdGetCrField(const SshdPpuContext* ctx, uint32_t field) {
    const uint8_t* bits = &ctx->cr[4 * field];
    return ((uint32_t)bits[0] << 3) | ((uint32_t)bits[1] << 2) | ((uint32_t)bits[2] << 1) | bits[3];
}

SSHD_FORCEINLINE void sshdCmpS(SshdPpuContext* ctx, uint32_t field, int64_t a, int64_t b) {
    uint8_t* bits = &ctx->cr[4 * field];
    bits[0] = a < b;
    bits[1] = a > b;
    bits[2] = a == b;
    bits[3] = ctx->xer_so;
}

SSHD_FORCEINLINE void sshdCmpU(SshdPpuContext* ctx, uint32_t field, uint64_t a, uint64_t b) {
    uint8_t* bits = &ctx->cr[4 * field];
    bits[0] = a < b;
    bits[1] = a > b;
    bits[2] = a == b;
    bits[3] = ctx->xer_so;
}

// Rc=1 in 64-bit mode: CR0 from the signed 64-bit result (RPCS3 `ppu_cr_set<s64>`).
SSHD_FORCEINLINE void sshdRecord(SshdPpuContext* ctx, uint64_t r) { sshdCmpS(ctx, 0, (int64_t)r, 0); }

SSHD_FORCEINLINE uint32_t sshdMfcr(const SshdPpuContext* ctx) {
    uint32_t r = 0;
    for (uint32_t i = 0; i < 32; i++) r |= (uint32_t)(ctx->cr[i] & 1) << (31 - i);
    return r;
}

// crm bit 0x80 >> f selects field f.
SSHD_FORCEINLINE void sshdMtcrf(SshdPpuContext* ctx, uint32_t crm, uint32_t value) {
    for (uint32_t f = 0; f < 8; f++) {
        if (crm & (0x80u >> f)) sshdSetCrField(ctx, f, (value >> (28 - 4 * f)) & 15);
    }
}

SSHD_FORCEINLINE uint64_t sshdMfxer(const SshdPpuContext* ctx) {
    return ((uint64_t)(ctx->xer_so & 1) << 31) | ((uint64_t)(ctx->xer_ov & 1) << 30) |
           ((uint64_t)(ctx->xer_ca & 1) << 29) | (ctx->xer_count & 0x7f);
}

SSHD_FORCEINLINE void sshdMtxer(SshdPpuContext* ctx, uint64_t value) {
    ctx->xer_so = (value >> 31) & 1;
    ctx->xer_ov = (value >> 30) & 1;
    ctx->xer_ca = (value >> 29) & 1;
    ctx->xer_count = value & 0x7f;
}

// OV = ov; SO is sticky.
SSHD_FORCEINLINE void sshdSetOv(SshdPpuContext* ctx, bool ov) {
    ctx->xer_ov = ov;
    ctx->xer_so = ctx->xer_so | ctx->xer_ov;
}

// Function entry and loop back edges: the only places where the runtime may preempt.
SSHD_FORCEINLINE void sshdLoop(SshdPpuContext* ctx, uint8_t* base) {
    if (--ctx->preempt_budget < 0) sshdYield(ctx, base);
}

SSHD_FORCEINLINE void sshdEnter(SshdPpuContext* ctx, uint8_t* base, uint32_t addr) {
    ctx->trace[ctx->trace_pos & (kSshdTraceSize - 1)] = addr;
    ctx->trace_pos++;
    sshdLoop(ctx, base);
}

// Emitted code names its parameters `ctx` and `base`; the macros rely on that.
#define SSHD_ENTER(addr) sshdEnter(ctx, base, (addr))
#define SSHD_LOOP(site)      \
    do {                     \
        (void)(site);        \
        sshdLoop(ctx, base); \
    } while (0)
#define SSHD_CALL(fn, ret) \
    do {                   \
        ctx->lr = (ret);   \
        fn(ctx, base);     \
    } while (0)
// The target is evaluated before LR is written (bclrl may target LR itself).
#define SSHD_CALL_DYN(site, target, ret)       \
    do {                                       \
        uint32_t sshd_t_ = (uint32_t)(target); \
        ctx->lr = (ret);                       \
        ctx->jump_site = (site);               \
        ctx->jump_target = sshd_t_;            \
        sshdDispatch(ctx, base);               \
    } while (0)
// The block keeps a label directly in front of a tail call from taking the attribute (C23 parses
// "L: __attribute__((musttail)) return" as a label attribute and rejects it).
#define SSHD_TAIL(fn)                       \
    do {                                    \
        SSHD_MUSTTAIL return fn(ctx, base); \
    } while (0)
#define SSHD_TAIL_DYN(site, target)                   \
    do {                                              \
        ctx->jump_site = (site);                      \
        ctx->jump_target = (uint32_t)(target);        \
        SSHD_MUSTTAIL return sshdDispatch(ctx, base); \
    } while (0)
#define SSHD_HLE(index) sshdHleCall(ctx, base, (index))
#define SSHD_HLE_IMPORT(table, index) sshdHleCallImport(ctx, base, &(table)[(index)])
#define SSHD_SYSCALL(site) sshdSyscall(ctx, base, (site))
// sync, lwsync, eieio and isync. The empty asm takes `base` as an input and clobbers memory: an opaque call
// without `base` would not order accesses through the `__restrict` guest pointer. Only the heavy sync needs a
// host fence, since SPU threads run in parallel with PPU threads and observe guest memory.
#define SSHD_SYNC(kind)                                                        \
    do {                                                                       \
        __asm__ __volatile__("" : : "r"(base) : "memory");                     \
        if ((kind) == kSshdSyncHeavy) __atomic_thread_fence(__ATOMIC_SEQ_CST); \
    } while (0)
#define SSHD_TRAP(site) sshdTrap(ctx, base, (site))
#define SSHD_UNHANDLED(site, word) sshdUnhandled(ctx, base, (site), (word))
// Diagnostics: C-callable trace from generated guest code.
void sshdTraceU32(const char* tag, uint32_t v);

#ifdef __cplusplus
}  // extern "C"
#endif
