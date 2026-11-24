// Checks the contract header's inline helpers and the stub runtime hooks they rely on.

#include <stdio.h>

#include "sshd/ppu.h"

static int g_failures;

#define CHECK(cond)                                                           \
    do {                                                                      \
        if (!(cond)) {                                                        \
            fprintf(stderr, "check failed (line %d): %s\n", __LINE__, #cond); \
            g_failures++;                                                     \
        }                                                                     \
    } while (0)

static SshdPpuContext g_ctx;
static uint8_t g_mem[512];

SSHD_PPU_FUNC(testFunc) {
    (void)base;
    ctx->gpr[3] = 0x1234;
}

static void checkMemory(uint8_t* base) {
    for (uint32_t i = 0; i < sizeof g_mem; i++) base[i] = 0;

    sshdWrite32(base, 0x11, 0x01020304u);
    CHECK(base[0x11] == 1 && base[0x12] == 2 && base[0x13] == 3 && base[0x14] == 4);
    CHECK(sshdRead32(base, 0x11) == 0x01020304u);

    sshdWrite16(base, 0x21, 0xa1b2u);
    CHECK(base[0x21] == 0xa1 && base[0x22] == 0xb2);
    CHECK(sshdRead16(base, 0x21) == 0xa1b2u);

    sshdWrite64(base, 0x33, 0x0102030405060708ull);
    for (uint32_t i = 0; i < 8; i++) CHECK(base[0x33 + i] == i + 1);
    CHECK(sshdRead64(base, 0x33) == 0x0102030405060708ull);

    sshdWrite8(base, 0x40, 0xee);
    CHECK(sshdRead8(base, 0x40) == 0xee);
}

static void checkVectors(uint8_t* base) {
    SshdVr v = sshdVrFromBeWords(0x00010203u, 0x04050607u, 0x08090a0bu, 0x0c0d0e0fu);
    CHECK(SSHD_VR_W(v, 0) == 0x00010203u);
    CHECK(SSHD_VR_W(v, 3) == 0x0c0d0e0fu);
    CHECK(SSHD_VR_B(v, 0) == 0x00 && SSHD_VR_B(v, 15) == 0x0f);
    CHECK(sshdVrBeWord(v, 1) == 0x04050607u);

    sshdWriteVr(base, 0x105, v);  // low address bits are ignored: stored at 0x100
    for (uint32_t i = 0; i < 16; i++) CHECK(base[0x100 + i] == i);
    CHECK(base[0xff] == 0 && base[0x110] == 0);

    SshdVr r = sshdReadVr(base, 0x10a);
    CHECK(SSHD_VR_W(r, 0) == 0x00010203u);
    CHECK(SSHD_VR_W(r, 3) == 0x0c0d0e0fu);
    CHECK(__builtin_memcmp(&r, &v, sizeof r) == 0);
}

static void checkCr(SshdPpuContext* ctx) {
    ctx->xer_so = 1;
    sshdCmpS(ctx, 2, -1, 1);
    CHECK(ctx->cr[8] == 1 && ctx->cr[9] == 0 && ctx->cr[10] == 0 && ctx->cr[11] == 1);
    sshdCmpU(ctx, 3, (uint64_t)-1, 1);  // unsigned: greater
    CHECK(ctx->cr[12] == 0 && ctx->cr[13] == 1 && ctx->cr[14] == 0 && ctx->cr[15] == 1);

    sshdRecord(ctx, 0);
    CHECK(ctx->cr[0] == 0 && ctx->cr[1] == 0 && ctx->cr[2] == 1 && ctx->cr[3] == 1);
    sshdRecord(ctx, 0x8000000000000000ull);  // negative in 64-bit mode
    CHECK(ctx->cr[0] == 1 && ctx->cr[1] == 0 && ctx->cr[2] == 0);
    sshdRecord(ctx, 0x100000000ull);  // non-zero although the low word is 0
    CHECK(ctx->cr[0] == 0 && ctx->cr[1] == 1 && ctx->cr[2] == 0);

    sshdMtcrf(ctx, 0xff, 0xa5c3f00fu);
    CHECK(sshdMfcr(ctx) == 0xa5c3f00fu);
    sshdMtcrf(ctx, 0x81, 0x1ffffff2u);  // fields 0 and 7 only
    CHECK(sshdMfcr(ctx) == 0x15c3f002u);
    CHECK(sshdGetCrField(ctx, 7) == 2);
}

static void checkXer(SshdPpuContext* ctx) {
    sshdMtxer(ctx, 0xffffffffu);
    CHECK(ctx->xer_so == 1 && ctx->xer_ov == 1 && ctx->xer_ca == 1 && ctx->xer_count == 0x7f);
    CHECK(sshdMfxer(ctx) == 0xe000007full);
    sshdMtxer(ctx, 0);
    CHECK(sshdMfxer(ctx) == 0);
    sshdSetOv(ctx, true);
    sshdSetOv(ctx, false);
    CHECK(ctx->xer_ov == 0 && ctx->xer_so == 1);  // SO is sticky
}

static void checkDispatch(SshdPpuContext* ctx, uint8_t* base) {
    SshdFuncMapping mappings[] = {{0x2000, testFunc}};
    sshdRegisterFuncs(mappings, 1);
    CHECK(sshdLookup(0x2000) == testFunc);
    CHECK(sshdLookup(0x2004) == nullptr);

    ctx->gpr[3] = 0;
    ctx->jump_target = 0x2000;
    ctx->jump_site = 0x1000;
    sshdDispatch(ctx, base);
    CHECK(ctx->gpr[3] == 0x1234);
}

static void checkAtomics(SshdPpuContext* ctx, uint8_t* base) {
    sshdWrite32(base, 0x80, 5);
    ctx->reserve_valid = 0;
    CHECK(!sshdStwcx(ctx, base, 0x80, 9));
    CHECK(sshdRead32(base, 0x80) == 5);

    CHECK(sshdLwarx(ctx, base, 0x80) == 5);
    CHECK(sshdStwcx(ctx, base, 0x80, 9));
    CHECK(sshdRead32(base, 0x80) == 9);
    CHECK(!sshdStwcx(ctx, base, 0x80, 11));  // the reservation was consumed

    sshdLwarx(ctx, base, 0x80);
    sshdWrite32(base, 0x80, 7);  // memory changed under the reservation
    CHECK(!sshdStwcx(ctx, base, 0x80, 11));
    CHECK(sshdRead32(base, 0x80) == 7);

    sshdLwarx(ctx, base, 0x80);
    CHECK(!sshdStwcx(ctx, base, 0x84, 1));  // different address

    sshdWrite64(base, 0x90, 0x1122334455667788ull);
    CHECK(sshdLdarx(ctx, base, 0x90) == 0x1122334455667788ull);
    CHECK(sshdStdcx(ctx, base, 0x90, 3));
    CHECK(sshdRead64(base, 0x90) == 3);
}

static void checkDcbz(SshdPpuContext* ctx, uint8_t* base) {
    for (uint32_t i = 0; i < sizeof g_mem; i++) base[i] = 0xff;
    sshdDcbz(ctx, base, 0x1a7);
    for (uint32_t i = 0; i < sizeof g_mem; i++) {
        bool in_line = i >= 0x180 && i < 0x200;
        CHECK(base[i] == (in_line ? 0x00 : 0xff));
    }
}

int main(void) {
    checkMemory(g_mem);
    checkVectors(g_mem);
    checkCr(&g_ctx);
    checkXer(&g_ctx);
    checkDispatch(&g_ctx, g_mem);
    checkAtomics(&g_ctx, g_mem);
    checkDcbz(&g_ctx, g_mem);

    if (g_failures != 0) {
        fprintf(stderr, "ppu header check: %d failure(s)\n", g_failures);
        return 1;
    }
    puts("ppu header ok");
    return 0;
}
