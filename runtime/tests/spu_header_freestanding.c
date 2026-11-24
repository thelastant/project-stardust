// Compiled with -ffreestanding: the SPU contract headers must parse without the C library headers.

#include "sshd/spu_context.h"
#include "sshd/spu_lower_stackfree.h"
#include "sshd/spu_ops.h"

SSHD_SPU_FUNC(spuHeaderFreestandingProbe) {
    SSHD_SPU_SF_ENTER(0x100);
    ctx->gpr[3] = sshdSpuLoadQ(ls, 0x110);
    sshdSpuStoreQ(ls, 0x120, ctx->gpr[3]);
    if (sshdSpuPreferred(ctx->gpr[3]) == 0) SSHD_SPU_SF_LEAVE();
    SSHD_SPU_SF_DISPATCH(0x104, ctx->jump_target);
}
