// Minimal runtime for the contract hooks in sshd/ppu_context.h. It lets generated code be linked
// and exercised without the real runtime: dispatch works, everything that would need the system
// (HLE calls, unknown targets, unhandled instructions) reports and exits.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <vector>

#include "sshd/ppu_context.h"
#include "sshd/ppu_stub.h"

// Generated code defines these. They are weak so that targets without generated code still link;
// sshdHleCall then reports the bare index.
extern "C" {
extern const SshdImportInfo kSshdPpuImports[] __attribute__((weak));
extern const uint32_t kSshdPpuImportCount __attribute__((weak));
}

namespace {

// Function-local so registration is safe during static initialisation of other translation units.
std::vector<SshdFuncMapping>& funcTable() {
    static std::vector<SshdFuncMapping> table;
    return table;
}

// std::abort raises a modal "Debug Error!" dialog in debug CRT builds, which stalls unattended test
// runs; exit with a status code instead.
const char* g_label = nullptr;

[[noreturn]] void fatalExit() {
    if (g_label != nullptr) std::fprintf(stderr, "sshd: while running %s\n", g_label);
    std::fflush(stdout);
    std::fflush(stderr);
    std::_Exit(3);
}

}  // namespace

extern "C" {

void sshdStubResetFuncs(void) { funcTable().clear(); }

void sshdStubSetLabel(const char* label) { g_label = label; }

void sshdRegisterFuncs(const SshdFuncMapping* mappings, uint32_t count) {
    std::vector<SshdFuncMapping>& table = funcTable();
    table.insert(table.end(), mappings, mappings + count);
    std::stable_sort(table.begin(), table.end(),
                     [](const SshdFuncMapping& a, const SshdFuncMapping& b) { return a.guest < b.guest; });
}

SshdPpuFunc* sshdLookup(uint32_t addr) {
    const std::vector<SshdFuncMapping>& table = funcTable();
    auto it = std::lower_bound(table.begin(), table.end(), addr,
                               [](const SshdFuncMapping& m, uint32_t a) { return m.guest < a; });
    return (it != table.end() && it->guest == addr) ? it->host : nullptr;
}

void sshdUnknownTarget(SshdPpuContext* ctx, uint8_t* base, uint32_t target, uint32_t site) {
    (void)ctx;
    (void)base;
    std::fprintf(stderr, "sshd: unknown dynamic target 0x%08x (from 0x%08x)\n", target, site);
    fatalExit();
}

// The musttail keeps dynamic transfers from adding a host frame per hop.
void sshdDispatch(SshdPpuContext* __restrict ctx, uint8_t* __restrict base) {
    SshdPpuFunc* f = sshdLookup(ctx->jump_target);
    if (f != nullptr) {
        [[clang::musttail]] return f(ctx, base);
    }
    sshdUnknownTarget(ctx, base, ctx->jump_target, ctx->jump_site);
}

void sshdHleCall(SshdPpuContext* ctx, uint8_t* base, uint32_t import_index) {
    (void)ctx;
    (void)base;
    if (kSshdPpuImports != nullptr && &kSshdPpuImportCount != nullptr && import_index < kSshdPpuImportCount) {
        const SshdImportInfo& info = kSshdPpuImports[import_index];
        std::fprintf(stderr, "sshd: HLE call %s::%s (import %u, stub 0x%08x, fnid 0x%08x)\n", info.module, info.name,
                     import_index, info.stub, info.fnid);
    } else {
        std::fprintf(stderr, "sshd: HLE call, import index %u\n", import_index);
    }
    fatalExit();
}

void sshdHleCallImport(SshdPpuContext* ctx, uint8_t* base, const SshdImportInfo* import) {
    (void)ctx;
    (void)base;
    const char* name = import->name != nullptr ? import->name : "-";
    std::fprintf(stderr, "sshd: HLE call %s::%s (stub 0x%08x, fnid 0x%08x)\n", import->module, name, import->stub,
                 import->fnid);
    fatalExit();
}

// Reports success so that a harness can observe the call.
void sshdSyscall(SshdPpuContext* ctx, uint8_t* base, uint32_t site) {
    (void)base;
    (void)site;
    ctx->gpr[3] = 0;
}

void sshdYield(SshdPpuContext* ctx, uint8_t* base) {
    (void)base;
    ctx->preempt_budget = kSshdPreemptBudget;
}

void sshdUnhandled(SshdPpuContext* ctx, uint8_t* base, uint32_t site, uint32_t word) {
    (void)ctx;
    (void)base;
    std::fprintf(stderr, "sshd: unhandled instruction 0x%08x at 0x%08x\n", word, site);
    fatalExit();
}

void sshdTrap(SshdPpuContext* ctx, uint8_t* base, uint32_t site) {
    (void)ctx;
    (void)base;
    std::fprintf(stderr, "sshd: trap at 0x%08x\n", site);
    fatalExit();
}

// Single-threaded reservation: it holds while the address matches and memory still contains the
// value that lwarx/ldarx observed. Every store-conditional clears it, successful or not. There is no
// reservation table here, so reserve_version stays unused (the real runtime checks it per line).
uint32_t sshdLwarx(SshdPpuContext* ctx, uint8_t* base, uint32_t ea) {
    uint32_t value = sshdRead32(base, ea);
    ctx->reserve_addr = ea;
    ctx->reserve_value = value;
    ctx->reserve_valid = 1;
    return value;
}

uint64_t sshdLdarx(SshdPpuContext* ctx, uint8_t* base, uint32_t ea) {
    uint64_t value = sshdRead64(base, ea);
    ctx->reserve_addr = ea;
    ctx->reserve_value = value;
    ctx->reserve_valid = 1;
    return value;
}

bool sshdStwcx(SshdPpuContext* ctx, uint8_t* base, uint32_t ea, uint32_t value) {
    bool ok = ctx->reserve_valid && ctx->reserve_addr == ea && sshdRead32(base, ea) == ctx->reserve_value;
    if (ok) sshdWrite32(base, ea, value);
    ctx->reserve_valid = 0;
    return ok;
}

bool sshdStdcx(SshdPpuContext* ctx, uint8_t* base, uint32_t ea, uint64_t value) {
    bool ok = ctx->reserve_valid && ctx->reserve_addr == ea && sshdRead64(base, ea) == ctx->reserve_value;
    if (ok) sshdWrite64(base, ea, value);
    ctx->reserve_valid = 0;
    return ok;
}

// No reservation table here, so a notifying store is a plain store.
void sshdNotifyWrite8(uint8_t* base, uint32_t ea, uint8_t value) { sshdWrite8(base, ea, value); }

void sshdNotifyWrite16(uint8_t* base, uint32_t ea, uint16_t value) { sshdWrite16(base, ea, value); }

void sshdNotifyWrite32(uint8_t* base, uint32_t ea, uint32_t value) { sshdWrite32(base, ea, value); }

void sshdNotifyWrite64(uint8_t* base, uint32_t ea, uint64_t value) { sshdWrite64(base, ea, value); }

void sshdDcbz(SshdPpuContext* ctx, uint8_t* base, uint32_t ea) {
    (void)ctx;
    std::memset(base + (ea & ~(uint32_t)(kSshdDcbzLine - 1)), 0, kSshdDcbzLine);
}

uint64_t sshdMftb(SshdPpuContext* ctx) {
    (void)ctx;
    static std::atomic<uint64_t> counter{0};
    return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

uint64_t sshdMffs(SshdPpuContext* ctx) { return ctx->fpscr; }

// Field 0 is the most significant nibble of the 32-bit image.
void sshdMtfsf(SshdPpuContext* ctx, uint32_t field_mask, uint64_t value) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < 8; i++) {
        if (field_mask & (0x80u >> i)) mask |= 0xfu << (28 - 4 * i);
    }
    ctx->fpscr = (ctx->fpscr & ~mask) | ((uint32_t)value & mask);
}

// Bit numbering is big-endian: bit 0 is the most significant bit of the image.
void sshdMtfsb(SshdPpuContext* ctx, uint32_t bit, uint32_t value) {
    uint32_t mask = 0x80000000u >> (bit & 31);
    ctx->fpscr = value ? (ctx->fpscr | mask) : (ctx->fpscr & ~mask);
}

}  // extern "C"
