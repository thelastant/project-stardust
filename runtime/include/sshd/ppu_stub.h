#pragma once

// Test support of the stub runtime (library sshd_ppu_stub). Deliberately not part of the contract
// in sshd/ppu_context.h: a real runtime has no use for it.

#ifdef __cplusplus
extern "C" {
#endif

// Forgets every registered function. sshdRegisterFuncs keeps duplicates and sshdLookup returns the
// first registration of a guest address, so a harness that registers several unrelated programs
// at the same guest addresses must reset between them.
void sshdStubResetFuncs(void);

// Names the running test; the stub's fatal exits (unhandled instruction, trap, unknown target, HLE
// call) print it. The pointer must stay valid until it is replaced; nullptr clears it.
void sshdStubSetLabel(const char* label);

#ifdef __cplusplus
}  // extern "C"
#endif
