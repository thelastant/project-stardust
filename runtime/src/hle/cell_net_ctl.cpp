// Offline-compatible cellNetCtl behavior. The runtime has no PSN/network service, but the game's
// net-start dialog still needs its sysutil completion events or callers can wait forever.

#include "hle/hle.h"

#include <cstdint>

#include "core/log.h"
#include "sshd/ppu_context.h"
#include "lv2/cell_error.h"

namespace sshd {
namespace {

constexpr char kModule[] = "cellNetCtl";
constexpr std::uint32_t kNotTerminated = 0x80130102;
constexpr std::uint32_t kNotInitialized = 0x80130101;
constexpr std::uint32_t kInvalidAddress = 0x80130107;
constexpr std::uint32_t kNotConnected = 0x80130108;
constexpr std::uint32_t kInvalidType = 0x8013010a;
constexpr std::uint32_t kInvalidSize = 0x8013010b;
constexpr std::uint32_t kDialogCanceled = 0x80130190;
constexpr std::uint32_t kNetStartTypeMax = 2;
constexpr std::uint32_t kNetStartParamSize = 12;
constexpr std::uint32_t kNetStartResultSize = 8;
constexpr std::uint32_t kDisconnected = 0;

bool g_initialized = false;

void netCtlInit(SshdPpuContext* ctx, std::uint8_t*) {
    if (g_initialized) {
        ctx->gpr[3] = kNotTerminated;
        return;
    }
    g_initialized = true;
    ctx->gpr[3] = kCellOk;
}

void netCtlTerm(SshdPpuContext* ctx, std::uint8_t*) {
    g_initialized = false;
    ctx->gpr[3] = kCellOk;
}

// cellNetCtlGetState(s32* state). This build has no network service, so it reports disconnected.
void netCtlGetState(SshdPpuContext* ctx, std::uint8_t* base) {
    if (!g_initialized) {
        ctx->gpr[3] = kNotInitialized;
        return;
    }
    const std::uint32_t state = argU32(ctx, 0);
    if (state == 0) {
        ctx->gpr[3] = kInvalidAddress;
        return;
    }
    sshdWrite32(base, state, kDisconnected);
    ctx->gpr[3] = kCellOk;
}

// cellNetCtlNetStartDialogLoadAsync(CellNetCtlNetStartDialogParam*).
void netCtlNetStartDialogLoadAsync(SshdPpuContext* ctx, std::uint8_t* base) {
    const std::uint32_t param = argU32(ctx, 0);
    if (param == 0) {
        ctx->gpr[3] = kInvalidAddress;
        return;
    }
    const std::uint32_t size = sshdRead32(base, param);
    const std::uint32_t type = sshdRead32(base, param + 4);
    if (size != kNetStartParamSize) {
        logWarn("sshd: cellNetCtl: net-start dialog has unexpected size {}", size);
        ctx->gpr[3] = kInvalidSize;
        return;
    }
    if (type >= kNetStartTypeMax) {
        ctx->gpr[3] = kInvalidType;
        return;
    }

    if (!g_initialized) {
        ctx->gpr[3] = kNotInitialized;
        return;
    }
    // Defer event delivery until the sysutil callback pump runs. The title registers its sysutil
    // callbacks just after this async call; queueSysutilEvent captures registered callbacks at
    // dispatch time, so enqueueing the events directly here would lose them.
    queueSysutilCall([type](SshdPpuContext*, std::uint8_t*) {
        const std::int32_t loaded = queueSysutilEvent(kSysutilNetStartLoaded, 0);
        const std::int32_t finished = queueSysutilEvent(kSysutilNetStartFinished, 0);
        logInfo("sshd: cellNetCtl: completed offline net-start dialog (type {}, callbacks {} / {})", type,
                loaded, finished);
    });
    ctx->gpr[3] = kCellOk;
}

// cellNetCtlNetStartDialogUnloadAsync(CellNetCtlNetStartDialogResult*).
void netCtlNetStartDialogUnloadAsync(SshdPpuContext* ctx, std::uint8_t* base) {
    if (!g_initialized) {
        ctx->gpr[3] = kNotInitialized;
        return;
    }
    const std::uint32_t result = argU32(ctx, 0);
    if (result == 0) {
        ctx->gpr[3] = kInvalidAddress;
        return;
    }
    if (sshdRead32(base, result) != kNetStartResultSize) {
        ctx->gpr[3] = kInvalidSize;
        return;
    }
    sshdWrite32(base, result + 4, kDialogCanceled);
    queueSysutilEvent(kSysutilNetStartUnloaded, 0);
    ctx->gpr[3] = kCellOk;
}

void netCtlNetStartDialogAbortAsync(SshdPpuContext* ctx, std::uint8_t*) {
    ctx->gpr[3] = g_initialized ? kCellOk : kNotInitialized;
}

void netCtlGetInfo(SshdPpuContext* ctx, std::uint8_t*) {
    ctx->gpr[3] = kNotConnected;
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x04459230, "cellNetCtlNetStartDialogLoadAsync", netCtlNetStartDialogLoadAsync},
        {kModule, 0x0f1f13d3, "cellNetCtlNetStartDialogUnloadAsync", netCtlNetStartDialogUnloadAsync},
        {kModule, 0x105ee2cb, "cellNetCtlTerm", netCtlTerm},
        {kModule, 0x1e585b5d, "cellNetCtlGetInfo", netCtlGetInfo},
        {kModule, 0x71d53210, "cellNetCtlNetStartDialogAbortAsync", netCtlNetStartDialogAbortAsync},
        {kModule, 0x8b3eba69, "cellNetCtlGetState", netCtlGetState},
        {kModule, 0xbd5a59fc, "cellNetCtlInit", netCtlInit},
};

}  // namespace

std::span<const HleEntry> cellNetCtlHandlers() { return kHandlers; }

void resetCellNetCtl() { g_initialized = false; }

}  // namespace sshd
