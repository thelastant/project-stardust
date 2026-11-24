# sshd_bench: fixed-input benchmarks in thread cycles (tools/sshd_bench). Built when the runtime core and the
# generated SPU tree exist. Call after sshd_runtime_add_core and sshd_spu_add_generated_tree.
function(sshd_bench_add)
  # Capture files of generated SPU code spans (written by the game's --spu-capture options) for the spu-replay case.
  # They hold game data, so the list is set per build directory on the command line and is empty by default.
  set(SSHD_SPU_REPLAY_CAPTURES "" CACHE STRING
      "Semicolon-separated .spucap files; each gets a bench_spu_replay_<name> smoke test")

  # RSX captures (written by --rsx-capture) for the rsx-fifo case. They hold game data too, so the list is empty by
  # default; the synthetic frame always gets a smoke test.
  set(SSHD_RSX_FIFO_CAPTURES "" CACHE STRING
      "Semicolon-separated .sshdcap files; each gets a bench_rsx_fifo_<name> smoke test")

  if(NOT TARGET sshd_spu_generated OR NOT TARGET sshd_runtime_core)
    return()
  endif()

  add_executable(sshd_bench tools/sshd_bench/main.cpp)
  target_compile_options(sshd_bench PRIVATE ${SSHD_ARCH_FLAGS} -Wall -Wextra)
  # The fixtures shared with the correctness tests live next to them.
  target_include_directories(sshd_bench PRIVATE runtime/tests)
  target_link_libraries(sshd_bench PRIVATE sshd_spu_generated sshd_runtime_core sshd_render sshd_crypto)

  # The LZMA case reads a real item, so it is registered under the conditions of spu_lzma_inflate.
  if(NOT SSHD_GAME_DIR STREQUAL "" AND NOT SSHD_EBOOT_ELF STREQUAL "")
    add_test(NAME bench_spu_lzma
      COMMAND sshd_bench spu-lzma --game-dir "${SSHD_GAME_DIR}" --elf "${SSHD_EBOOT_ELF}"
              --iterations 2 --warmup 1)
    set_tests_properties(bench_spu_lzma PROPERTIES
      PASS_REGULAR_EXPRESSION "bench spu-lzma: 2 iterations \\(1 warm-up\\), median [0-9]+ cycles .*; check ok"
      TIMEOUT 120)
  endif()

  add_test(NAME bench_spu_dma_ls COMMAND sshd_bench spu-dma-ls --iterations 2 --warmup 1)
  set_tests_properties(bench_spu_dma_ls PROPERTIES
    PASS_REGULAR_EXPRESSION "bench spu-dma-ls: 2 iterations \\(1 warm-up\\), median [0-9]+ cycles .*; check ok"
    TIMEOUT 120)

  add_test(NAME bench_spu_dma_restore COMMAND sshd_bench spu-dma-restore --iterations 2 --warmup 1)
  set_tests_properties(bench_spu_dma_restore PROPERTIES
    PASS_REGULAR_EXPRESSION "bench spu-dma-restore: 2 iterations \\(1 warm-up\\), median [0-9]+ cycles .*; check ok"
    TIMEOUT 120)

  add_test(NAME bench_rsv_wake COMMAND sshd_bench rsv-wake --rounds 200 --iterations 2 --warmup 1)
  set_tests_properties(bench_rsv_wake PROPERTIES
    PASS_REGULAR_EXPRESSION "bench rsv-wake: 2 iterations \\(1 warm-up\\), median [0-9]+ cycles .*; check ok"
    TIMEOUT 120)

  add_test(NAME bench_rsv_put COMMAND sshd_bench rsv-put --iterations 2 --warmup 1)
  set_tests_properties(bench_rsv_put PROPERTIES
    PASS_REGULAR_EXPRESSION "bench rsv-put: 2 iterations \\(1 warm-up\\), median [0-9]+ cycles .*; check ok"
    TIMEOUT 120)

  add_test(NAME bench_rsv_idle COMMAND sshd_bench rsv-idle --span-ms 50 --iterations 2 --warmup 1)
  set_tests_properties(bench_rsv_idle PROPERTIES
    PASS_REGULAR_EXPRESSION "bench rsv-idle: 2 iterations \\(1 warm-up\\), median [0-9]+ cycles .*; check ok"
    TIMEOUT 120)

  add_test(NAME bench_rsx_hash COMMAND sshd_bench rsx-hash --kb 64 --reps 16 --iterations 2 --warmup 1)
  set_tests_properties(bench_rsx_hash PROPERTIES
    PASS_REGULAR_EXPRESSION "bench rsx-hash: 2 iterations \\(1 warm-up\\), median [0-9]+ cycles .*; check ok"
    TIMEOUT 120)

  # The OS write watch is a Windows API.
  if(WIN32)
    add_test(NAME bench_rsx_write_watch
      COMMAND sshd_bench rsx-write-watch --range-kib 1024 --dirty-pages 4 --calls 4 --local-mib 8
              --iterations 2 --warmup 1)
    set_tests_properties(bench_rsx_write_watch PROPERTIES
      PASS_REGULAR_EXPRESSION "bench rsx-write-watch: 2 iterations \\(1 warm-up\\), median [0-9]+ cycles .*; check ok"
      TIMEOUT 120)

    add_test(NAME bench_rsx_write_watch_first
      COMMAND sshd_bench rsx-write-watch --mode first-write --pages 64 --local-mib 8 --iterations 2 --warmup 1)
    set_tests_properties(bench_rsx_write_watch_first PROPERTIES
      PASS_REGULAR_EXPRESSION "bench rsx-write-watch: 2 iterations \\(1 warm-up\\), median [0-9]+ cycles .*; check ok"
      TIMEOUT 120)
  endif()

  add_test(NAME bench_vertex_convert COMMAND sshd_bench vertex-convert --vertices 1024 --reps 2 --iterations 2 --warmup 1)
  set_tests_properties(bench_vertex_convert PROPERTIES
    PASS_REGULAR_EXPRESSION "bench vertex-convert: 2 iterations \\(1 warm-up\\), median [0-9]+ cycles .*; check ok"
    TIMEOUT 120)

  add_test(NAME bench_rsx_fifo_synthetic COMMAND sshd_bench rsx-fifo --synthetic --iterations 2 --warmup 1)
  set_tests_properties(bench_rsx_fifo_synthetic PROPERTIES
    PASS_REGULAR_EXPRESSION "bench rsx-fifo: 2 iterations \\(1 warm-up\\), median [0-9]+ cycles .*; check ok"
    TIMEOUT 120)

  add_test(NAME bench_rsx_fifo_pollute
    COMMAND sshd_bench rsx-fifo --synthetic --pollute-mb 4 --iterations 2 --warmup 1)
  set_tests_properties(bench_rsx_fifo_pollute PROPERTIES
    PASS_REGULAR_EXPRESSION "bench rsx-fifo: texture validations [0-9]+ per pass, [0-9]+ hash skips"
    FAIL_REGULAR_EXPRESSION "check FAILED|error: "
    TIMEOUT 120)

  foreach(capture IN LISTS SSHD_RSX_FIFO_CAPTURES)
    if(NOT EXISTS "${capture}")
      continue()
    endif()
    get_filename_component(stem "${capture}" NAME_WE)
    add_test(NAME bench_rsx_fifo_${stem} COMMAND sshd_bench rsx-fifo --capture "${capture}" --iterations 2 --warmup 1)
    set_tests_properties(bench_rsx_fifo_${stem} PROPERTIES
      LABELS run
      PASS_REGULAR_EXPRESSION "bench rsx-fifo: 2 iterations \\(1 warm-up\\), median [0-9]+ cycles .*; check ok"
      TIMEOUT 120)
  endforeach()

  add_test(NAME bench_spu_replay_synthetic COMMAND sshd_bench spu-replay --synthetic --iterations 2 --warmup 1)
  set_tests_properties(bench_spu_replay_synthetic PROPERTIES
    PASS_REGULAR_EXPRESSION "bench spu-replay: 2 iterations \\(1 warm-up\\), median [0-9]+ cycles .*; check ok"
    TIMEOUT 120)

  add_test(NAME bench_spu_dispatch
    COMMAND sshd_bench spu-dispatch --images 4 --entries 16 --dispatches 2000 --pollute-kb 32
            --iterations 2 --warmup 1)
  set_tests_properties(bench_spu_dispatch PROPERTIES
    PASS_REGULAR_EXPRESSION "bench spu-dispatch: 2 iterations \\(1 warm-up\\), median [0-9]+ cycles .*; check ok"
    TIMEOUT 120)

  foreach(capture IN LISTS SSHD_SPU_REPLAY_CAPTURES)
    if(NOT EXISTS "${capture}")
      continue()
    endif()
    get_filename_component(stem "${capture}" NAME_WE)
    add_test(NAME bench_spu_replay_${stem}
      COMMAND sshd_bench spu-replay --capture "${capture}" --iterations 2 --warmup 1)
    set_tests_properties(bench_spu_replay_${stem} PROPERTIES
      LABELS run
      PASS_REGULAR_EXPRESSION "bench spu-replay: 2 iterations \\(1 warm-up\\), median [0-9]+ cycles .*; check ok"
      TIMEOUT 120)
  endforeach()
endfunction()
