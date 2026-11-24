// Runtime hooks that emitted code calls for CPU-level behaviour: reservations, cache and sync
// instructions, the timebase, FPSCR access, and the fatal instruction paths.

#include <atomic>
#include <cstdint>
#include <cstring>
#include <format>
#include <mutex>
#include <vector>

#include "core/census.h"
#include "core/fatal.h"
#include "core/log.h"
#include "core/reservation.h"
#include "sshd/ppu_context.h"

extern "C" void sshdTraceU32(const char* tag, uint32_t v) { sshd::logInfo("sshd: TRACE {} {:#x}", tag, v); }

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <time.h>
#endif

namespace {

#if defined(_WIN32)
std::int64_t queryCounter() {
    LARGE_INTEGER value;
    QueryPerformanceCounter(&value);
    return value.QuadPart;
}

std::int64_t counterFrequency() {
    static const std::int64_t frequency = [] {
        LARGE_INTEGER value;
        QueryPerformanceFrequency(&value);
        return value.QuadPart;
    }();
    return frequency;
}
#else
// Nanoseconds, so the frequency is a constant. On Apple, CLOCK_UPTIME_RAW is the clock that does not
// count time asleep, like QueryPerformanceCounter does not on Windows.
std::int64_t queryCounter() {
#if defined(__APPLE__)
    return static_cast<std::int64_t>(clock_gettime_nsec_np(CLOCK_UPTIME_RAW));
#else
    timespec ts;
    clock_gettime(CLOCK_MONOTONIC, &ts);
    return static_cast<std::int64_t>(ts.tv_sec) * 1'000'000'000 + ts.tv_nsec;
#endif
}

std::int64_t counterFrequency() { return 1'000'000'000; }
#endif

// Initialised during static initialisation, so it is the process start for all practical purposes.
const std::int64_t g_start_counter = queryCounter();

// delta * mul / frequency without a 128-bit division (needs compiler-rt, which is not linked).
// The remainder term stays below frequency * mul, under 2^64 for a counter frequency up to 1 GHz.
std::uint64_t scaleCounter(std::uint64_t delta, std::uint64_t mul) {
    const auto frequency = static_cast<std::uint64_t>(counterFrequency());
    return (delta / frequency) * mul + (delta % frequency) * mul / frequency;
}

std::uint64_t elapsedCounter() {
    const std::int64_t now = queryCounter();
    return now > g_start_counter ? static_cast<std::uint64_t>(now - g_start_counter) : 0;
}

// One block per host thread, written only by that thread, so counting adds no shared-cache-line write to the
// reservation hooks. Each counter is an atomic so that the reader's loads are not a data race.
struct alignas(64) ReservationCounters {
    std::atomic<std::uint64_t> lwarx{0}, stwcx{0}, stwcx_failed{0};

    static void bump(std::atomic<std::uint64_t>& counter) {
        counter.store(counter.load(std::memory_order_relaxed) + 1, std::memory_order_relaxed);
    }
};

std::mutex g_counters_mutex;
// Blocks are never freed: a thread that exits keeps its totals in the sum.
std::vector<ReservationCounters*> g_counters;

ReservationCounters& threadCounters() {
    thread_local ReservationCounters* counters = [] {
        auto* block = new ReservationCounters;
        std::lock_guard lock(g_counters_mutex);
        g_counters.push_back(block);
        return block;
    }();
    return *counters;
}

void countStoreConditional(bool ok) {
    ReservationCounters& counters = threadCounters();
    ReservationCounters::bump(counters.stwcx);
    if (!ok) ReservationCounters::bump(counters.stwcx_failed);
}

// The put contract needs natural alignment for 1/2/4/8 bytes, and the emitter cannot prove an EA's alignment. A
// misaligned store is not expected at the routed sites, so the byte-wise path only has to be correct (each byte is a
// separate reservation-aware write, not atomic as a whole).
void notifyWrite(std::uint8_t* base, std::uint32_t ea, const std::uint8_t* big_endian, std::uint32_t size) {
    if (ea % size == 0) {
        sshd::reservations().put(base, ea, big_endian, size);
        return;
    }
    for (std::uint32_t i = 0; i < size; i++) sshd::reservations().put(base, ea + i, big_endian + i, 1);
}

}  // namespace

namespace sshd {

std::uint64_t systemTimeUs() { return scaleCounter(elapsedCounter(), 1000000u); }

PpuReservationStats ppuReservationStats() {
    PpuReservationStats total;
    std::lock_guard lock(g_counters_mutex);
    for (const ReservationCounters* block : g_counters) {
        total.lwarx += block->lwarx.load(std::memory_order_relaxed);
        total.stwcx += block->stwcx.load(std::memory_order_relaxed);
        total.stwcx_failed += block->stwcx_failed.load(std::memory_order_relaxed);
    }
    return total;
}

}  // namespace sshd

extern "C" {

void sshdUnhandled(SshdPpuContext* ctx, uint8_t* base, uint32_t site, uint32_t word) {
    (void)ctx;
    (void)base;
    sshd::fatal(sshd::FatalKind::UnhandledInstruction, std::format("word {:#010x} at {:#010x}", word, site));
}

void sshdTrap(SshdPpuContext* ctx, uint8_t* base, uint32_t site) {
    (void)ctx;
    (void)base;
    sshd::fatal(sshd::FatalKind::Trap, std::format("at {:#010x}", site));
}

// A reservation holds while the address matches, memory still contains the value that lwarx/ldarx
// observed (an ABA rewrite of the word is accepted) and the stripe version of the line is unchanged. The
// version moves on every reservation-aware write (PUTLLC, PUTLLUC, MFC PUT through the table, another
// store-conditional), so a write to any word of the line since the lwarx fails the store like it does on
// hardware. Guest loops read neighbouring words of the line between lwarx and stwcx. and rely on that to
// not act on a torn snapshot. Lines that share a stripe can fail the store spuriously; the guest's retry
// loops absorb that. Plain PPU stores to other words of the line still do not cancel the reservation:
// they are not routed through the table (see docs/spu.md). Every store-conditional clears the
// reservation, successful or not. The checks and the write happen under the stripe lock, so they are
// atomic against SPU GETLLAR/PUTLLC and PUT on the same line even though SPU threads run without the guest lock.
uint32_t sshdLwarx(SshdPpuContext* ctx, uint8_t* base, uint32_t ea) {
    ReservationCounters::bump(threadCounters().lwarx);
    if (sshd::lwmutexWatchEnabled()) sshd::lwmutexWatchCheck(sshd::LwmutexWatchKind::Lwarx, ea, 4, ctx->lr);
    uint64_t value;
    sshd::reservations().loadReserved(base, ea, 4, value, ctx->reserve_version);
    ctx->reserve_addr = ea;
    ctx->reserve_value = value;
    ctx->reserve_valid = 1;
    return static_cast<uint32_t>(value);
}

uint64_t sshdLdarx(SshdPpuContext* ctx, uint8_t* base, uint32_t ea) {
    ReservationCounters::bump(threadCounters().lwarx);
    if (sshd::lwmutexWatchEnabled()) sshd::lwmutexWatchCheck(sshd::LwmutexWatchKind::Lwarx, ea, 8, ctx->lr);
    uint64_t value;
    sshd::reservations().loadReserved(base, ea, 8, value, ctx->reserve_version);
    ctx->reserve_addr = ea;
    ctx->reserve_value = value;
    ctx->reserve_valid = 1;
    return value;
}

// The store-conditional checks run on every attempt, not only on success: a guest loop that retries on a word of
// an lwmutex is the evidence the watch looks for, whether or not a given store lands.
bool sshdStwcx(SshdPpuContext* ctx, uint8_t* base, uint32_t ea, uint32_t value) {
    if (sshd::lwmutexWatchEnabled()) sshd::lwmutexWatchCheck(sshd::LwmutexWatchKind::Stwcx, ea, 4, ctx->lr);
    const bool ok = ctx->reserve_valid && ctx->reserve_addr == ea &&
                    sshd::reservations().storeConditional(base, ea, 4, ctx->reserve_value, ctx->reserve_version, value);
    ctx->reserve_valid = 0;
    countStoreConditional(ok);
    return ok;
}

bool sshdStdcx(SshdPpuContext* ctx, uint8_t* base, uint32_t ea, uint64_t value) {
    if (sshd::lwmutexWatchEnabled()) sshd::lwmutexWatchCheck(sshd::LwmutexWatchKind::Stwcx, ea, 8, ctx->lr);
    const bool ok = ctx->reserve_valid && ctx->reserve_addr == ea &&
                    sshd::reservations().storeConditional(base, ea, 8, ctx->reserve_value, ctx->reserve_version, value);
    ctx->reserve_valid = 0;
    countStoreConditional(ok);
    return ok;
}

void sshdNotifyWrite8(uint8_t* base, uint32_t ea, uint8_t value) { notifyWrite(base, ea, &value, 1); }

void sshdNotifyWrite16(uint8_t* base, uint32_t ea, uint16_t value) {
    uint8_t buf[2];
    sshdWrite16(buf, 0, value);
    notifyWrite(base, ea, buf, 2);
}

void sshdNotifyWrite32(uint8_t* base, uint32_t ea, uint32_t value) {
    uint8_t buf[4];
    sshdWrite32(buf, 0, value);
    notifyWrite(base, ea, buf, 4);
}

void sshdNotifyWrite64(uint8_t* base, uint32_t ea, uint64_t value) {
    uint8_t buf[8];
    sshdWrite64(buf, 0, value);
    notifyWrite(base, ea, buf, 8);
}

void sshdDcbz(SshdPpuContext* ctx, uint8_t* base, uint32_t ea) {
    const uint32_t line = ea & ~static_cast<uint32_t>(kSshdDcbzLine - 1);
    if (sshd::lwmutexWatchEnabled()) {
        sshd::lwmutexWatchCheck(sshd::LwmutexWatchKind::Dcbz, line, kSshdDcbzLine, ctx->lr);
    }
    std::memset(base + line, 0, kSshdDcbzLine);
}

uint64_t sshdMftb(SshdPpuContext* ctx) {
    (void)ctx;
    return scaleCounter(elapsedCounter(), sshd::kTimebaseFrequency);
}

uint64_t sshdMffs(SshdPpuContext* ctx) { return ctx->fpscr; }

// Field 0 is the most significant nibble of the 32-bit image.
void sshdMtfsf(SshdPpuContext* ctx, uint32_t field_mask, uint64_t value) {
    uint32_t mask = 0;
    for (uint32_t i = 0; i < 8; i++) {
        if (field_mask & (0x80u >> i)) mask |= 0xfu << (28 - 4 * i);
    }
    ctx->fpscr = (ctx->fpscr & ~mask) | (static_cast<uint32_t>(value) & mask);
}

// Bit numbering is big-endian: bit 0 is the most significant bit of the image.
void sshdMtfsb(SshdPpuContext* ctx, uint32_t bit, uint32_t value) {
    const uint32_t mask = 0x80000000u >> (bit & 31);
    ctx->fpscr = value ? (ctx->fpscr | mask) : (ctx->fpscr & ~mask);
}

}  // extern "C"
