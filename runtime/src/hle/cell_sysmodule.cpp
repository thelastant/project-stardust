// HLE handlers of the cellSysmodule module. RPCS3 `Modules/cellSysmodule.cpp` rejects module ids it
// does not know and tracks load counts; here every id succeeds, because no module is actually
// loaded (the HLE functions are always available). Handlers run under the kernel lock
// (the guest lock in legacy mode).

#include <cstdint>
#include <format>

#include "core/log.h"
#include "sshd/ppu_context.h"
#include "hle/hle.h"
#include "lv2/cell_error.h"

namespace sshd {
namespace {

constexpr char kModule[] = "cellSysmodule";

// The id argument is a u16 in the SDK.
void logModule(const char* verb, const SshdPpuContext* ctx) {
    const auto id = static_cast<std::uint16_t>(argU32(ctx, 0));
    logOnce(std::format("sysmodule-{}-{}", verb, id), "sshd: sysmodule: {} {:#06x}", verb, id);
}

// cellSysmoduleLoadModule(u16 id)
void sysmoduleLoadModule(SshdPpuContext* ctx, std::uint8_t* base) {
    (void)base;
    logModule("load", ctx);
    ctx->gpr[3] = kCellOk;
}

// cellSysmoduleUnloadModule(u16 id)
void sysmoduleUnloadModule(SshdPpuContext* ctx, std::uint8_t* base) {
    (void)base;
    logModule("unload", ctx);
    ctx->gpr[3] = kCellOk;
}

constexpr HleEntry kHandlers[] = {
        {kModule, 0x112a5ee9, "cellSysmoduleUnloadModule", sysmoduleUnloadModule},
        {kModule, 0x32267a31, "cellSysmoduleLoadModule", sysmoduleLoadModule},
};

}  // namespace

std::span<const HleEntry> cellSysmoduleHandlers() { return kHandlers; }

}  // namespace sshd
