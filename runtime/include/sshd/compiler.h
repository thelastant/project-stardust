#pragma once

#ifdef __cplusplus
#define SSHD_MUSTTAIL [[clang::musttail]]
#else
#define SSHD_MUSTTAIL __attribute__((musttail))
#endif
#define SSHD_NOINLINE __attribute__((noinline))
