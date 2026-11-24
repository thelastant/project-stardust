// Checks the SPU contract header's inline helpers and the stack-free lowering macros against a minimal
// dispatcher written here; the real dispatcher belongs to the runtime.

#include <stdio.h>

#include "sshd/spu_context.h"
#include "sshd/spu_lower_stackfree.h"

static int g_failures;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

static_assert(alignof(SshdSpuContext) == 16);
static_assert(offsetof(SshdSpuContext, gpr) == 0);
static_assert(offsetof(SshdSpuContext, gpr[127]) == 127 * 16);
static_assert(offsetof(SshdSpuContext, fpscr) == 128 * 16);
static_assert(sizeof(SshdSpuContext) % alignof(SshdSpuContext) == 0);

static SshdSpuContext g_ctx;
static uint8_t g_ls[kSshdSpuLsSize] __attribute__((aligned(16)));

// Functions and dispatch table

static int g_rejected_entries;

SSHD_SPU_FUNC(funcA);
SSHD_SPU_FUNC(funcB);
SSHD_SPU_FUNC(funcLeaf);
SSHD_SPU_FUNC(funcGuarded);

static const SshdSpuEntry kEntries[] = {
        {0x100, 0, {0}, funcA},
        {0x200, 0, {0}, funcB},
        {0x300, 0, {0}, funcLeaf},
        {0x400, 2, {0x12345678u, 0x9abcdef0u}, funcGuarded},
};

static uint32_t loadWord(const uint8_t* ls, uint32_t addr) {
    uint32_t v;
    __builtin_memcpy(&v, ls + addr, sizeof v);
    return __builtin_bswap32(v);
}

void sshdSpuDispatch(SshdSpuContext* __restrict ctx, uint8_t* __restrict ls) {
    uint32_t target = ctx->jump_target & 0x3fffcu;
    for (uint32_t i = 0; i < sizeof kEntries / sizeof kEntries[0]; i++) {
        const SshdSpuEntry* entry = &kEntries[i];
        if (entry->ls != target) continue;
        for (uint32_t w = 0; w < entry->check_count; w++) {
            if (loadWord(ls, target + 4 * w) != entry->check[w]) {
                g_rejected_entries++;
                return;
            }
        }
        SSHD_MUSTTAIL return entry->host(ctx, ls);
    }
    g_rejected_entries++;
}

// A function that jumps to a known block of another function.
SSHD_SPU_FUNC(funcA) {
    SSHD_SPU_SF_ENTER(0x100);
    ctx->gpr[3] = sshdVrFromBeWords(0xaaaa0001u, 0, 0, 0);
    SSHD_SPU_SF_JUMP(funcB, 0x200);
}

// A computed transfer: the link register is written after the target is read, the way the emitter
// lowers `bisl $0,$0`.
SSHD_SPU_FUNC(funcB) {
    SSHD_SPU_SF_ENTER(0x200);
    uint32_t target = sshdSpuPreferred(ctx->gpr[0]);
    ctx->gpr[0] = sshdVrFromBeWords(0x204u, 0, 0, 0);
    SSHD_SPU_SF_DISPATCH(0x200, target);
}

SSHD_SPU_FUNC(funcLeaf) {
    (void)ls;
    SSHD_SPU_SF_ENTER(0x300);
    ctx->gpr[5] = sshdVrFromBeWords(0xbbbb0002u, 0, 0, 0);
    if (sshdSpuPreferred(ctx->gpr[6]) != 0) SSHD_SPU_SF_LEAVE();
    ctx->gpr[5] = sshdVrFromBeWords(0xdeadu, 0, 0, 0);  // not reached when gpr[6] is set
}

SSHD_SPU_FUNC(funcGuarded) {
    (void)ls;
    SSHD_SPU_SF_ENTER(0x400);
    ctx->gpr[7] = sshdVrFromBeWords(1, 0, 0, 0);
}

// Loops until the halt request is seen; the body raises the request itself after five rounds.
SSHD_SPU_FUNC(funcLoop) {
    (void)ls;
    for (;;) {
        SSHD_VR_W(ctx->gpr[8], 0)++;
        if (SSHD_VR_W(ctx->gpr[8], 0) == 5) __atomic_store_n(&ctx->halt, 1, __ATOMIC_RELEASE);
        SSHD_SPU_SF_LOOP(0x500);
    }
}

// Checks

static void checkLocalStore(void) {
    for (uint32_t i = 0; i < sizeof g_ls; i++) g_ls[i] = 0;
    SshdVr v = sshdVrFromBeWords(0x00010203u, 0x04050607u, 0x08090a0bu, 0x0c0d0e0fu);

    sshdSpuStoreQ(g_ls, 0x1235, v);  // low address bits are ignored: stored at 0x1230
    for (uint32_t i = 0; i < 16; i++) CHECK(g_ls[0x1230 + i] == i);
    CHECK(g_ls[0x122f] == 0 && g_ls[0x1240] == 0);

    SshdVr r = sshdSpuLoadQ(g_ls, 0x123a);
    CHECK(__builtin_memcmp(&r, &v, sizeof r) == 0);
    CHECK(sshdSpuPreferred(r) == 0x00010203u);
    CHECK(SSHD_VR_W(r, 3) == 0x0c0d0e0fu);

    // The address wraps at the LS size.
    sshdSpuStoreQ(g_ls, kSshdSpuLsSize + 0x20, v);
    CHECK(g_ls[0x20] == 0x00 && g_ls[0x2f] == 0x0f);
    SshdVr w = sshdSpuLoadQ(g_ls, 0xfffc0000u + 0x20);
    CHECK(__builtin_memcmp(&w, &v, sizeof w) == 0);

    // The last quadword is reachable and does not run past the buffer.
    sshdSpuStoreQ(g_ls, kSshdSpuLsSize - 16, v);
    CHECK(g_ls[kSshdSpuLsSize - 1] == 0x0f);
}

static void checkTrace(SshdSpuContext* ctx) {
    ctx->trace_pos = 0;
    for (uint32_t i = 0; i < 70; i++) SSHD_SPU_ENTER(0x1000 + 4 * i);
    CHECK(ctx->trace_pos == 70);
    CHECK(ctx->trace[(70 - 1) & (kSshdSpuTraceSize - 1)] == 0x1000 + 4 * 69);
    CHECK(ctx->trace[5] == 0x1000 + 4 * 69);  // 69 & 63; the ring holds the newest 64
    CHECK(ctx->trace[6] == 0x1000 + 4 * 6);
}

static void checkLowering(SshdSpuContext* ctx, uint8_t* ls) {
    // A -> (JUMP) B -> (DISPATCH to gpr[0]) leaf; the link written by B is the return site.
    ctx->halt = 0;
    ctx->trace_pos = 0;
    ctx->gpr[0] = sshdVrFromBeWords(0x40300u, 0, 0, 0);  // out-of-range bits are masked by the dispatcher
    ctx->gpr[6] = sshdVrFromBeWords(0, 0, 0, 0);
    ctx->jump_target = 0;
    funcA(ctx, ls);
    CHECK(sshdSpuPreferred(ctx->gpr[3]) == 0xaaaa0001u);
    CHECK(sshdSpuPreferred(ctx->gpr[0]) == 0x204u);
    CHECK(sshdSpuPreferred(ctx->gpr[5]) == 0xdeadu);  // the leaf ran to its end
    CHECK(ctx->jump_site == 0x200);
    CHECK(ctx->trace_pos == 3);
    CHECK(ctx->trace[0] == 0x100 && ctx->trace[1] == 0x200 && ctx->trace[2] == 0x300);

    // LEAVE returns before the statements after it.
    ctx->gpr[6] = sshdVrFromBeWords(1, 0, 0, 0);
    funcLeaf(ctx, ls);
    CHECK(sshdSpuPreferred(ctx->gpr[5]) == 0xbbbb0002u);

    // A halt request is seen at block entry.
    ctx->gpr[5] = sshdVrFromBeWords(0, 0, 0, 0);
    __atomic_store_n(&ctx->halt, 1, __ATOMIC_RELEASE);
    CHECK(sshdSpuHaltRequested(ctx));
    funcLeaf(ctx, ls);
    CHECK(sshdSpuPreferred(ctx->gpr[5]) == 0);
    ctx->halt = 0;
    CHECK(!sshdSpuHaltRequested(ctx));

    // A halt request is seen on a backward edge.
    ctx->gpr[8] = sshdVrFromBeWords(0, 0, 0, 0);
    funcLoop(ctx, ls);
    CHECK(sshdSpuPreferred(ctx->gpr[8]) == 5);
    CHECK(ctx->jump_site == 0x500);
    ctx->halt = 0;
}

static void checkEntryCheck(SshdSpuContext* ctx, uint8_t* ls) {
    ctx->gpr[7] = sshdVrFromBeWords(0, 0, 0, 0);
    ctx->jump_target = 0x400;
    g_rejected_entries = 0;

    sshdSpuDispatch(ctx, ls);  // LS does not hold the check words
    CHECK(g_rejected_entries == 1);
    CHECK(sshdSpuPreferred(ctx->gpr[7]) == 0);

    SshdVr words = sshdVrFromBeWords(0x12345678u, 0x9abcdef0u, 0, 0);
    sshdSpuStoreQ(ls, 0x400, words);
    sshdSpuDispatch(ctx, ls);
    CHECK(g_rejected_entries == 1);
    CHECK(sshdSpuPreferred(ctx->gpr[7]) == 1);

    ctx->jump_target = 0x700;  // not a dispatchable entry
    sshdSpuDispatch(ctx, ls);
    CHECK(g_rejected_entries == 2);
}

int main(void) {
    checkLocalStore();
    checkTrace(&g_ctx);
    checkLowering(&g_ctx, g_ls);
    checkEntryCheck(&g_ctx, g_ls);

    if (g_failures != 0) {
        fprintf(stderr, "spu header check: %d failure(s)\n", g_failures);
        return 1;
    }
    puts("spu header ok");
    return 0;
}
