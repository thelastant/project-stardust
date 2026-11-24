// Compiled as C++26: the SPU contract headers must parse as C++ and the lowering macros must work
// with [[clang::musttail]].

#include "sshd/spu_context.h"
#include "sshd/spu_lower_stackfree.h"
#include "sshd/spu_ops.h"

static_assert(sizeof(SshdSpuContext) % 16 == 0);
static_assert(kSshdSpuNoExit == 0xffffffffu);

extern "C" {
SSHD_SPU_FUNC(spuHeaderCxxCallee) { ctx->gpr[4] = sshdSpuLoadQ(ls, kSshdSpuLsSize + 0x10); }

SSHD_SPU_FUNC(spuHeaderCxxProbe) {
    SSHD_SPU_SF_ENTER(0x100);
    if (sshdSpuPreferred(ctx->gpr[3]) == 0) SSHD_SPU_SF_LEAVE();
    SSHD_SPU_SF_LOOP(0x108);
    if (sshdSpuPreferred(ctx->gpr[3]) == 1) SSHD_SPU_SF_JUMP(spuHeaderCxxCallee, 0x200);
    SSHD_SPU_SF_DISPATCH(0x10c, ctx->jump_target);
}
}
