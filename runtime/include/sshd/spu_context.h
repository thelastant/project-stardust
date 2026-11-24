#pragma once

// Contract between generated SPU code and the runtime: the SPU context, the tables the generated
// code provides, the runtime hooks and the inline local-store accessors. Nothing here depends on how
// control transfers are lowered; that lives in one header per lowering (spu_lower_stackfree.h), so a
// second lowering adds a header and an emitter module without touching this file.
//
// Like ppu_context.h this compiles as C23 and C++26 and with -ffreestanding.
//
// Registers use SshdVr with the PPU's byte-reversed lane layout: SSHD_VR_W(v, 0) is the preferred-slot
// word, lane-wise operations need no index mapping, and a quadword load or store reverses the bytes
// once. The local store (LS) is kept as it appears in memory, big-endian. Generated functions name
// their parameters `ctx` and `ls`; the macros rely on that. Guest memory is never visible to
// generated SPU code, only the runtime's MFC hooks touch it.

#include <stddef.h>
#include <stdint.h>

#include "compiler.h"
#include "ppu_context.h"

#ifdef __cplusplus
extern "C" {
#endif

enum SshdSpuLimits { kSshdSpuLsSize = 0x40000, kSshdSpuTraceSize = 64, kSshdSpuNoExit = 0xffffffffu };

typedef struct SshdSpuContext {
    SshdVr gpr[128];       // byte-reversed lanes, as SshdVr in ppu_context.h
    SshdVr fpscr;          // fscrrd/fscrwr image; flags are not derived from operations
    uint32_t jump_target;  // LS address of the current transfer: dispatch input and entry-switch key
    uint32_t jump_site;    // LS address of the transferring instruction
    uint32_t srr0;
    uint32_t halt;        // non-zero: leave at the next check; written by other threads (__atomic)
    uint32_t exit_ls;     // a dispatch to this LS address returns to the run loop; kSshdSpuNoExit = none
    uint8_t int_enabled;  // RdMachStat bit 0; set and cleared by the E/D bits and iret
    int32_t image_delta;  // loaded LS address minus link LS address for the dispatched code image
    uint32_t trace_pos;
    uint32_t trace[kSshdSpuTraceSize];
    void* runtime;  // the runtime's thread object
} SshdSpuContext;

static_assert(alignof(SshdSpuContext) == 16, "SshdSpuContext must be 16-byte aligned");
static_assert(sizeof(SshdSpuContext) % 16 == 0, "SshdSpuContext size must keep arrays of contexts aligned");
static_assert(offsetof(SshdSpuContext, gpr) == 0, "gpr[] must stay first");
static_assert(offsetof(SshdSpuContext, fpscr) % 16 == 0, "fpscr must stay 16-byte aligned");
static_assert((kSshdSpuTraceSize & (kSshdSpuTraceSize - 1)) == 0, "trace ring size must be a power of two");

typedef void SshdSpuFunc(SshdSpuContext* __restrict ctx, uint8_t* __restrict ls);

SSHD_FORCEINLINE uint32_t sshdSpuRelocateLs(const SshdSpuContext* ctx, uint32_t link_addr) {
    return link_addr + (uint32_t)ctx->image_delta;
}

// One dispatchable block.
typedef struct SshdSpuEntry {
    uint32_t ls;
    uint32_t check_count;  // 1..4: words compared with LS before the jump
    uint32_t check[4];     // instruction words (generated tables only)
    SshdSpuFunc* host;
} SshdSpuEntry;

// ea is the absolute guest address of the segment's bytes.
typedef struct SshdSpuSegment {
    uint32_t ea, ls, size, code;
} SshdSpuSegment;

typedef struct SshdSpuImageInfo {
    const char* name;
    uint32_t ea, size;   // the whole image in guest memory (ELF: header to end of last segment)
    const char* sha256;  // of those bytes; checked by the runtime at load
    uint32_t entry_point;
    uint32_t segment_count;
    const SshdSpuSegment* segments;
    uint32_t entry_count;  // sorted by ls
    const SshdSpuEntry* entries;
} SshdSpuImageInfo;

#define SSHD_SPU_FUNC(name) SSHD_NOINLINE void name(SshdSpuContext* __restrict ctx, uint8_t* __restrict ls)

// Runtime hooks, implemented by the runtime.
void sshdSpuDispatch(SshdSpuContext* __restrict ctx, uint8_t* __restrict ls);  // target = jump_target
// The channel and stop hooks return false when the thread must leave.
bool sshdSpuReadChannel(SshdSpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t ch, uint32_t* value);
bool sshdSpuWriteChannel(SshdSpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t ch, uint32_t value);
uint32_t sshdSpuChannelCount(SshdSpuContext* ctx, uint8_t* ls, uint32_t ch);
bool sshdSpuStop(SshdSpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t code);  // true: continue at site + 4
[[noreturn]] void sshdSpuHalt(SshdSpuContext* ctx, uint8_t* ls, uint32_t site);    // heq/hgt/hlgt taken
[[noreturn]] void sshdSpuUnhandled(SshdSpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t word);

// Defined by generated code (spu_tables.c).
extern const SshdSpuImageInfo kSshdSpuImages[];
extern const uint32_t kSshdSpuImageCount;

// Quadword access to the local store: the address wraps at the LS size and its low four bits are
// ignored (lqd/stqd semantics). The byte reversal turns big-endian LS bytes into the register layout.
SSHD_FORCEINLINE SshdVr sshdSpuLoadQ(const uint8_t* ls, uint32_t addr) {
    SshdU8x16 raw;
    __builtin_memcpy(&raw, ls + (addr & ((uint32_t)kSshdSpuLsSize - 16u)), sizeof raw);
    SshdVr v;
    v.u8 = __builtin_shufflevector(raw, raw, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
    return v;
}

SSHD_FORCEINLINE void sshdSpuStoreQ(uint8_t* ls, uint32_t addr, SshdVr value) {
    SshdU8x16 raw = __builtin_shufflevector(value.u8, value.u8, 15, 14, 13, 12, 11, 10, 9, 8, 7, 6, 5, 4, 3, 2, 1, 0);
    __builtin_memcpy(ls + (addr & ((uint32_t)kSshdSpuLsSize - 16u)), &raw, sizeof raw);
}

// The preferred slot is word 0.
SSHD_FORCEINLINE uint32_t sshdSpuPreferred(SshdVr v) { return SSHD_VR_W(v, 0); }

// Block entry: the trace ring that the fatal-stop dump prints.
SSHD_FORCEINLINE void sshdSpuEnter(SshdSpuContext* ctx, uint32_t addr) {
    ctx->trace[ctx->trace_pos & (kSshdSpuTraceSize - 1)] = addr;
    ctx->trace_pos++;
}

// Acquire pairs with the release store of the thread that requests the halt, so state it wrote
// before the request is visible to the leaving thread.
SSHD_FORCEINLINE bool sshdSpuHaltRequested(const SshdSpuContext* ctx) {
    return __atomic_load_n(&ctx->halt, __ATOMIC_ACQUIRE) != 0;
}

#define SSHD_SPU_ENTER(addr) sshdSpuEnter(ctx, (addr))

// --- Host floating-point environment ---------------------------------------------------------
//
// Generated SPU code runs with the host MXCSR in the SPU environment: round toward zero with flush-to-
// zero and denormals-are-zero, all exceptions masked. The single-precision helpers of spu_fp.h rely on
// it (the SPU truncates, and its denormals are zeros). The runtime sets it around the generated code
// (runSpu); a helper that needs the default environment, round to nearest with denormals, switches
// to it for its own duration and restores the caller's value.
//
// On arm64 the same environment lives in FPCR, and the names below keep "Mxcsr" for the one control
// word either host has: RMode (bits 23:22) 0b11 rounds toward zero, and FZ (bit 24) flushes denormal
// operands as well as results, so it stands for both FTZ and DAZ. FPCR has no trap enables set by
// default, which matches the masked exceptions.
#if defined(__x86_64__) || defined(_M_X64)
enum SshdSpuMxcsr : uint32_t {
    kSshdSpuMxcsrHost = 0x1f80u,  // round to nearest, exceptions masked
    kSshdSpuMxcsrSpu = 0xffc0u,   // host | round toward zero (0x6000) | FTZ (0x8000) | DAZ (0x40)
};

SSHD_FORCEINLINE uint32_t sshdSpuMxcsrGet(void) { return __builtin_ia32_stmxcsr(); }

SSHD_FORCEINLINE void sshdSpuMxcsrSet(uint32_t value) { __builtin_ia32_ldmxcsr(value); }
#elif defined(__aarch64__)
enum SshdSpuMxcsr : uint32_t {
    kSshdSpuMxcsrHost = 0u,                      // round to nearest, no flush
    kSshdSpuMxcsrSpu = (3u << 22) | (1u << 24),  // round toward zero | FZ
};

SSHD_FORCEINLINE uint32_t sshdSpuMxcsrGet(void) { return (uint32_t)__builtin_arm_rsr64("fpcr"); }

SSHD_FORCEINLINE void sshdSpuMxcsrSet(uint32_t value) { __builtin_arm_wsr64("fpcr", (uint64_t)value); }
#else
#error "no SPU floating-point environment for this host"
#endif

// Switches to the default environment and returns the previous MXCSR for sshdSpuMxcsrSet. The float
// operations of the scope must sit between the two calls; the SPU codegen options (-frounding-math) and
// FENV_ACCESS in the helper keep the compiler from moving them out.
SSHD_FORCEINLINE uint32_t sshdSpuMxcsrEnterHost(void) {
    const uint32_t saved = sshdSpuMxcsrGet();
    sshdSpuMxcsrSet(kSshdSpuMxcsrHost);
    return saved;
}

#ifdef __cplusplus
}  // extern "C"
#endif
