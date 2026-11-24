#pragma once

#include <cstdint>

#include "sshd/ppu_context.h"

namespace sshd {

// Adds the functions of a module as its own span next to the ones sshdRegisterFuncs set up. Same checks
// as sshdRegisterFuncs (aligned, sorted, non-null); a span that overlaps an existing one is a fatal
// error. Call before any guest thread starts.
void appendFuncs(const SshdFuncMapping* mappings, std::uint32_t count);

}  // namespace sshd
