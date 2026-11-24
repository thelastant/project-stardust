// Minimal runtime for the contract hooks in sshd/spu_context.h. It lets generated SPU code be linked
// and exercised without the real runtime: dispatch works over the selected image's table, everything
// that would need a thread (channels, stops, halts, unknown targets, unhandled words) reports and exits.

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstdlib>

#include "sshd/spu_context.h"
#include "sshd/spu_stub.h"

namespace {

const SshdSpuImageInfo* g_image = nullptr;
const char* g_label = nullptr;

// std::abort raises a modal "Debug Error!" dialog in debug CRT builds, which stalls unattended test
// runs; exit with a status code instead.
[[noreturn]] void fatalExit() {
    if (g_label != nullptr) std::fprintf(stderr, "sshd: while running %s\n", g_label);
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(3);
}

uint32_t loadWord(const uint8_t* ls, uint32_t addr) {
    const uint8_t* p = ls + (addr & (kSshdSpuLsSize - 1u));
    return (uint32_t)p[0] << 24 | (uint32_t)p[1] << 16 | (uint32_t)p[2] << 8 | (uint32_t)p[3];
}

}  // namespace

extern "C" {

void sshdSpuStubSelectImage(const SshdSpuImageInfo* image) { g_image = image; }

void sshdSpuStubSetLabel(const char* label) { g_label = label; }

// The musttail keeps dynamic transfers from adding a host frame per hop.
void sshdSpuDispatch(SshdSpuContext* __restrict ctx, uint8_t* __restrict ls) {
    const uint32_t target = ctx->jump_target & (kSshdSpuLsSize - 4u);
    if (g_image == nullptr) {
        std::fprintf(stderr, "sshd: spu dispatch to ls 0x%05x with no image selected\n", target);
        fatalExit();
    }
    const SshdSpuEntry* first = g_image->entries;
    const SshdSpuEntry* last = first + g_image->entry_count;
    const SshdSpuEntry* it =
            std::lower_bound(first, last, target, [](const SshdSpuEntry& e, uint32_t a) { return e.ls < a; });
    if (it == last || it->ls != target) {
        std::fprintf(stderr, "sshd: spu unknown target ls 0x%05x from ls 0x%05x (%s)\n", target, ctx->jump_site,
                     g_image->name);
        fatalExit();
    }
    for (uint32_t i = 0; i < it->check_count; i++) {
        if (loadWord(ls, target + 4 * i) != it->check[i]) {
            std::fprintf(stderr, "sshd: spu code mismatch at ls 0x%05x (%s)\n", target, g_image->name);
            fatalExit();
        }
    }
    [[clang::musttail]] return it->host(ctx, ls);
}

bool sshdSpuReadChannel(SshdSpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t ch, uint32_t* value) {
    (void)ctx;
    (void)ls;
    (void)value;
    std::fprintf(stderr, "sshd: spu rdch %u at ls 0x%05x\n", ch, site);
    fatalExit();
}

bool sshdSpuWriteChannel(SshdSpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t ch, uint32_t value) {
    (void)ctx;
    (void)ls;
    std::fprintf(stderr, "sshd: spu wrch %u (0x%08x) at ls 0x%05x\n", ch, value, site);
    fatalExit();
}

uint32_t sshdSpuChannelCount(SshdSpuContext* ctx, uint8_t* ls, uint32_t ch) {
    (void)ctx;
    (void)ls;
    std::fprintf(stderr, "sshd: spu rchcnt %u\n", ch);
    fatalExit();
}

bool sshdSpuStop(SshdSpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t code) {
    (void)ctx;
    (void)ls;
    std::fprintf(stderr, "sshd: spu stop 0x%x at ls 0x%05x\n", code, site);
    fatalExit();
}

void sshdSpuHalt(SshdSpuContext* ctx, uint8_t* ls, uint32_t site) {
    (void)ctx;
    (void)ls;
    std::fprintf(stderr, "sshd: spu halt at ls 0x%05x\n", site);
    fatalExit();
}

void sshdSpuUnhandled(SshdSpuContext* ctx, uint8_t* ls, uint32_t site, uint32_t word) {
    (void)ctx;
    (void)ls;
    std::fprintf(stderr, "sshd: spu unhandled word 0x%08x at ls 0x%05x\n", word, site);
    fatalExit();
}

}  // extern "C"
