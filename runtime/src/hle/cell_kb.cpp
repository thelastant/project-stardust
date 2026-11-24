#include <cstdint>
#include <span>

#include "hle/hle.h"
#include "lv2/cell_error.h"
#include "sshd/ppu_context.h"

namespace sshd {
namespace {

constexpr char kModule[] = "sys_io";
constexpr std::uint32_t kErrorInvalidParameter = 0x80121002;
constexpr std::uint32_t kErrorAlreadyInitialized = 0x80121003;
constexpr std::uint32_t kErrorUninitialized = 0x80121004;
constexpr std::uint32_t kErrorNoDevice = 0x80121007;
constexpr std::uint32_t kMaxKeyboards = 127;

bool g_initialized = false;

void keyboardInit(SshdPpuContext* ctx, std::uint8_t*) {
    if (g_initialized) {
        ctx->gpr[3] = kErrorAlreadyInitialized;
        return;
    }
    const std::uint32_t max_connect = argU32(ctx, 0);
    if (max_connect == 0 || max_connect > kMaxKeyboards) {
        ctx->gpr[3] = kErrorInvalidParameter;
        return;
    }
    g_initialized = true;
    ctx->gpr[3] = kCellOk;
}

void keyboardEnd(SshdPpuContext* ctx, std::uint8_t*) {
    if (!g_initialized) {
        ctx->gpr[3] = kErrorUninitialized;
        return;
    }
    g_initialized = false;
    ctx->gpr[3] = kCellOk;
}

void keyboardRead(SshdPpuContext* ctx, std::uint8_t*) {
    if (!g_initialized) {
        ctx->gpr[3] = kErrorUninitialized;
        return;
    }
    if (argU32(ctx, 0) >= kMaxKeyboards || argU32(ctx, 1) == 0) {
        ctx->gpr[3] = kErrorInvalidParameter;
        return;
    }

    // The physical controller is exposed through cellPad, not as a PS3 USB keyboard. Report no
    // keyboard instead of success with an untouched CellKbData buffer that the game may parse.
    ctx->gpr[3] = kErrorNoDevice;
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x433f6ec0, "cellKbInit", keyboardInit},
        {kModule, 0xbfce3285, "cellKbEnd", keyboardEnd},
        {kModule, 0xff0a21b7, "cellKbRead", keyboardRead},
};

}  // namespace

std::span<const HleEntry> cellKbHandlers() { return kHandlers; }

void resetCellKb() { g_initialized = false; }

}  // namespace sshd
