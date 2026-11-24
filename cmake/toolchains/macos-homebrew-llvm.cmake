# Toolchain for macOS with Homebrew LLVM (brew install llvm). Apple's own clang is rejected by
# cmake/CompilerChecks.cmake, so the compilers come from the LLVM prefix.
#
# The prefix is SSHD_LLVM_PREFIX (cache variable or environment), else `brew --prefix llvm`.

if(NOT SSHD_LLVM_PREFIX AND DEFINED ENV{SSHD_LLVM_PREFIX})
  set(SSHD_LLVM_PREFIX "$ENV{SSHD_LLVM_PREFIX}")
endif()

if(NOT SSHD_LLVM_PREFIX)
  execute_process(
    COMMAND brew --prefix llvm
    OUTPUT_VARIABLE sshd_brew_llvm_prefix
    OUTPUT_STRIP_TRAILING_WHITESPACE
    ERROR_QUIET
    RESULT_VARIABLE sshd_brew_result)
  if(sshd_brew_result EQUAL 0)
    set(SSHD_LLVM_PREFIX "${sshd_brew_llvm_prefix}")
  endif()
endif()

if(NOT SSHD_LLVM_PREFIX OR NOT EXISTS "${SSHD_LLVM_PREFIX}/bin/clang++")
  message(FATAL_ERROR
    "SSHD: LLVM clang not found (prefix '${SSHD_LLVM_PREFIX}'). Install it with `brew install llvm` or "
    "set SSHD_LLVM_PREFIX to a directory that contains bin/clang++.")
endif()

# Cached, and mirrored into the environment so the try_compile projects (which re-read this file)
# do not run brew again.
set(SSHD_LLVM_PREFIX "${SSHD_LLVM_PREFIX}" CACHE PATH "LLVM installation prefix" FORCE)
set(ENV{SSHD_LLVM_PREFIX} "${SSHD_LLVM_PREFIX}")

set(CMAKE_C_COMPILER "${SSHD_LLVM_PREFIX}/bin/clang")
set(CMAKE_CXX_COMPILER "${SSHD_LLVM_PREFIX}/bin/clang++")
set(CMAKE_OBJCXX_COMPILER "${SSHD_LLVM_PREFIX}/bin/clang++")

# Homebrew binutils can put a GNU ar first on PATH; use the LLVM archiver that matches the compiler.
set(CMAKE_AR "${SSHD_LLVM_PREFIX}/bin/llvm-ar" CACHE FILEPATH "Archiver")
set(CMAKE_RANLIB "${SSHD_LLVM_PREFIX}/bin/llvm-ranlib" CACHE FILEPATH "Archive indexer")

# The C++26 library headers of this clang need the matching Homebrew libc++, not the one in the SDK
# (Homebrew's documented link flags for LLVM).
set(sshd_llvm_link_flags
  "-L${SSHD_LLVM_PREFIX}/lib/c++ -Wl,-rpath,${SSHD_LLVM_PREFIX}/lib/c++ -L${SSHD_LLVM_PREFIX}/lib/unwind -lunwind")
set(CMAKE_EXE_LINKER_FLAGS_INIT "${sshd_llvm_link_flags}")
set(CMAKE_SHARED_LINKER_FLAGS_INIT "${sshd_llvm_link_flags}")
set(CMAKE_MODULE_LINKER_FLAGS_INIT "${sshd_llvm_link_flags}")
