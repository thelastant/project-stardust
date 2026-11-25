include_guard(GLOBAL)

# Optimization level of generated SPU code and of the tests that run it, so the gates check the code
# that ships. The SPU tree is small, and its decode loops (Edge LZMA, the audio decoder) run 6-7 times
# faster than at the -O1 of the PPU tree: the game's start-up inflate went from 3.7 to 25 MB/s.
set(SSHD_SPU_CODEGEN_OPT_LEVEL -O3)

# Options for every target that includes the SPU contract headers. -frounding-math keeps the compiler
# from folding or moving float operations across the MXCSR changes (the SPU environment is round toward
# zero with flush-to-zero, see spu_context.h); the PPU tree does not need it and is not charged for it.
function(sshd_spu_codegen_options target)
  sshd_ppu_codegen_options(${target} ${ARGN})
  target_compile_options(${target} PRIVATE -frounding-math)
endfunction()

# Contract-header check for generated SPU code: the run-time test, plus object-only translation
# units that parse the headers as C++ and with -ffreestanding. Needs sshd_spu_codegen_options.
function(sshd_spu_add_header_check)
  add_executable(sshd_spu_header_check runtime/tests/spu_header_check.c)
  sshd_spu_codegen_options(sshd_spu_header_check)

  add_library(sshd_spu_header_freestanding OBJECT runtime/tests/spu_header_freestanding.c)
  sshd_spu_codegen_options(sshd_spu_header_freestanding)
  target_compile_options(sshd_spu_header_freestanding PRIVATE -ffreestanding)

  add_library(sshd_spu_header_cxx OBJECT runtime/tests/spu_header_cxx.cpp)
  sshd_spu_codegen_options(sshd_spu_header_cxx)

  # Built with the check so that a header regression fails the build of the test target.
  add_dependencies(sshd_spu_header_check sshd_spu_header_freestanding sshd_spu_header_cxx)

  add_test(NAME spu_header_check COMMAND sshd_spu_header_check)
  set_tests_properties(spu_header_check PROPERTIES PASS_REGULAR_EXPRESSION "spu header ok")

  add_executable(sshd_spu_vector_helpers_test runtime/tests/spu_vector_helpers_test.c)
  sshd_spu_codegen_options(sshd_spu_vector_helpers_test ${SSHD_SPU_CODEGEN_OPT_LEVEL})
  add_test(NAME spu_vector_helpers COMMAND sshd_spu_vector_helpers_test)
  set_tests_properties(spu_vector_helpers PROPERTIES PASS_REGULAR_EXPRESSION "spu vector helpers ok")
endfunction()

# Adds the generated SPU tree and its link test from SSHD_PPU_GENERATED_DIR/spu/spu_sources.cmake
# (written by 'ps3tool spu-emit --out <dir>/spu'; sshd_ppu_add_generated_tree already stops when it is
# missing). Call after sshd_spu_stub is defined.
# sshd_spu_generated links no hook library; each consumer picks the stub or the real runtime.
function(sshd_spu_add_generated_tree)
  if(SSHD_PPU_GENERATED_DIR STREQUAL "")
    return()
  endif()

  cmake_path(ABSOLUTE_PATH SSHD_PPU_GENERATED_DIR BASE_DIRECTORY "${CMAKE_SOURCE_DIR}"
             OUTPUT_VARIABLE generated_dir)
  set(sources_file "${generated_dir}/spu/spu_sources.cmake")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${sources_file}")
  include("${sources_file}")
  message(STATUS "SSHD: generated SPU code from ${generated_dir}/spu")

  add_library(sshd_spu_generated STATIC ${SSHD_SPU_GENERATED_SOURCES})
  sshd_spu_codegen_options(sshd_spu_generated ${SSHD_SPU_CODEGEN_OPT_LEVEL})
  target_include_directories(sshd_spu_generated PUBLIC "${generated_dir}/spu")

  add_executable(sshd_spu_link_test runtime/tests/spu_link_test.cpp)
  target_compile_options(sshd_spu_link_test PRIVATE ${SSHD_ARCH_FLAGS} -Wall -Wextra)
  target_link_libraries(sshd_spu_link_test PRIVATE sshd_spu_generated sshd_spu_stub)

  add_test(NAME spu_link COMMAND sshd_spu_link_test)
  set_tests_properties(spu_link PROPERTIES PASS_REGULAR_EXPRESSION "spu link ok")
endfunction()

# Semantics harness: hand-written cases are generated into C (tools/tests/spu_semantics) through the
# real emitter, compiled like emitted code and run natively. The driver supplies every runtime hook,
# so nothing is linked. Needs Python3_EXECUTABLE and sshd_spu_codegen_options; call it after
# sshd_ppu_add_semantics_harness, which probes SSHD_HAVE_UBSAN_TRAP_MUSTTAIL.
function(sshd_spu_add_semantics_harness)
  set(gen_dir "${CMAKE_BINARY_DIR}/spu_semantics")
  set(gen_stamp "${gen_dir}/generate.stamp")

  # The generator rewrites its outputs only when their content changes, so the custom command is
  # tracked through a stamp instead of the outputs (see sshd_ppu_add_semantics_harness).
  file(GLOB gen_inputs CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/ps3recomp/*.py"
    "${CMAKE_SOURCE_DIR}/tools/tests/spu_asm.py"
    "${CMAKE_SOURCE_DIR}/tools/tests/spu_semantics/*.py")
  add_custom_command(
    OUTPUT "${gen_stamp}"
    BYPRODUCTS "${gen_dir}/spu_semantics_cases.h" "${gen_dir}/spu_semantics_cases.c"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${gen_dir}"
    COMMAND ${Python3_EXECUTABLE} -B -m tests.spu_semantics.gen --out "${gen_dir}"
    COMMAND ${CMAKE_COMMAND} -E touch "${gen_stamp}"
    DEPENDS ${gen_inputs}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tools"
    COMMENT "Generating SPU semantics cases"
    VERBATIM)
  add_custom_target(sshd_spu_semantics_gen DEPENDS "${gen_stamp}")

  add_executable(sshd_spu_semantics
    runtime/tests/spu_semantics_main.c
    "${gen_dir}/spu_semantics_cases.c")
  set_source_files_properties("${gen_dir}/spu_semantics_cases.c" PROPERTIES GENERATED TRUE)
  add_dependencies(sshd_spu_semantics sshd_spu_semantics_gen)
  sshd_spu_codegen_options(sshd_spu_semantics ${SSHD_SPU_CODEGEN_OPT_LEVEL})
  target_include_directories(sshd_spu_semantics PRIVATE "${gen_dir}")

  if(SSHD_HAVE_UBSAN_TRAP_MUSTTAIL)
    target_compile_options(sshd_spu_semantics PRIVATE
      -fsanitize=undefined -fsanitize-trap=undefined)
  endif()

  add_test(NAME spu_semantics COMMAND sshd_spu_semantics)
  set_tests_properties(spu_semantics PROPERTIES
    PASS_REGULAR_EXPRESSION "spu semantics: [0-9]+ passed, 0 failed"
    TIMEOUT 120)
endfunction()

# Whole-program tests: SPU programs assembled in tools/tests/spu_programs are emitted through the real
# recovery and emitter into synthetic images, then run through the lv2 SPU thread group calls on the
# runtime core. Needs Python3_EXECUTABLE, sshd_spu_codegen_options and sshd_runtime_core, so call it
# after sshd_runtime_add_core. It reads no generated tree and no game files, and it links the real
# SPU runtime only: sshd_spu_stub defines the same symbols as the core dispatch.
function(sshd_spu_add_programs)
  if(NOT TARGET sshd_runtime_core)
    return()
  endif()

  set(gen_dir "${CMAKE_BINARY_DIR}/spu_programs")
  set(gen_stamp "${gen_dir}/generate.stamp")
  # One chunk per image; keep the names in step with PROGRAMS in tools/tests/spu_programs/gen.py.
  set(program_names sum_array atomic_counter jump_table throw_event)
  set(gen_sources "${gen_dir}/spu_tables.c" "${gen_dir}/spu_programs_data.c")
  foreach(name IN LISTS program_names)
    list(APPEND gen_sources "${gen_dir}/spu_${name}_000.c")
  endforeach()

  # The generator rewrites its outputs only when their content changes, so the custom command is
  # tracked through a stamp instead of the outputs (see sshd_ppu_add_semantics_harness).
  file(GLOB gen_inputs CONFIGURE_DEPENDS
    "${CMAKE_SOURCE_DIR}/tools/ps3recomp/*.py"
    "${CMAKE_SOURCE_DIR}/tools/tests/spu_asm.py"
    "${CMAKE_SOURCE_DIR}/tools/tests/spu_programs/*.py")
  add_custom_command(
    OUTPUT "${gen_stamp}"
    BYPRODUCTS ${gen_sources} "${gen_dir}/spu_funcs.h" "${gen_dir}/spu_programs_data.h"
    COMMAND ${CMAKE_COMMAND} -E make_directory "${gen_dir}"
    COMMAND ${Python3_EXECUTABLE} -B -m tests.spu_programs.gen --out "${gen_dir}"
    COMMAND ${CMAKE_COMMAND} -E touch "${gen_stamp}"
    DEPENDS ${gen_inputs}
    WORKING_DIRECTORY "${CMAKE_SOURCE_DIR}/tools"
    COMMENT "Generating SPU test programs"
    VERBATIM)
  add_custom_target(sshd_spu_programs_gen DEPENDS "${gen_stamp}")

  add_library(sshd_spu_programs_generated OBJECT ${gen_sources})
  set_source_files_properties(${gen_sources} PROPERTIES GENERATED TRUE)
  add_dependencies(sshd_spu_programs_generated sshd_spu_programs_gen)
  sshd_spu_codegen_options(sshd_spu_programs_generated ${SSHD_SPU_CODEGEN_OPT_LEVEL})
  target_include_directories(sshd_spu_programs_generated PRIVATE "${gen_dir}")

  add_executable(sshd_spu_programs runtime/tests/spu_programs_main.cpp
    $<TARGET_OBJECTS:sshd_spu_programs_generated>)
  add_dependencies(sshd_spu_programs sshd_spu_programs_gen)
  target_compile_options(sshd_spu_programs PRIVATE ${SSHD_ARCH_FLAGS} -Wall -Wextra)
  target_include_directories(sshd_spu_programs PRIVATE "${gen_dir}")
  target_link_libraries(sshd_spu_programs PRIVATE sshd_runtime_core)

  add_test(NAME spu_programs COMMAND sshd_spu_programs)
  set_tests_properties(spu_programs PROPERTIES
    PASS_REGULAR_EXPRESSION "spu programs: 4 passed, 0 failed"
    TIMEOUT 120)
endfunction()

# Recompiled LZMA inflate of the game's LZMA SPURS task on a real TLZC item of TLFILE.TLDAT. Needs the
# generated SPU tree, the runtime core, the game directory and the decrypted EBOOT.elf, so it is
# registered only when all of them are configured. Call after sshd_runtime_add_core.
function(sshd_spu_add_lzma_inflate_test)
  if(NOT TARGET sshd_spu_generated OR NOT TARGET sshd_runtime_core)
    return()
  endif()
  if(SSHD_GAME_DIR STREQUAL "" OR SSHD_EBOOT_ELF STREQUAL "")
    message(STATUS "SSHD: spu_lzma_inflate test not registered (SSHD_GAME_DIR or SSHD_EBOOT_ELF is empty)")
    return()
  endif()

  add_executable(sshd_spu_lzma_inflate_test runtime/tests/spu_lzma_inflate_test.cpp)
  target_compile_options(sshd_spu_lzma_inflate_test PRIVATE ${SSHD_ARCH_FLAGS} -Wall -Wextra)
  target_link_libraries(sshd_spu_lzma_inflate_test PRIVATE sshd_spu_generated sshd_runtime_core sshd_crypto)

  add_test(NAME spu_lzma_inflate
    COMMAND sshd_spu_lzma_inflate_test "${SSHD_GAME_DIR}" "${SSHD_EBOOT_ELF}")
  set_tests_properties(spu_lzma_inflate PROPERTIES
    PASS_REGULAR_EXPRESSION
      "spu lzma inflate: 0x3073c bytes sha256 f4ad1f7125d9b31abf228e8043587aebc8f1501f0f4debed6d9814d06a12c751"
    SKIP_RETURN_CODE 77
    TIMEOUT 120)
endfunction()
