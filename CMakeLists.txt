cmake_minimum_required(VERSION 3.29)

project(SshdRecomp VERSION 0.1.0 LANGUAGES C CXX)

option(SSHD_OFFLINE_BUILD "Do not download any dependencies during configuration" OFF)

# Optional Clang instrumentation/PGO. Keep it off for normal builds; a trained profile can
# specialize the large generated PPU/SPU trees as well as the hand-written runtime.
option(SSHD_PGO_GENERATE "Instrument this build for profile-guided optimization" OFF)
set(SSHD_PGO_PROFILE "" CACHE FILEPATH "Merged LLVM instrumentation profile for a PGO-use build")
if(SSHD_PGO_GENERATE AND NOT SSHD_PGO_PROFILE STREQUAL "")
  message(FATAL_ERROR "SSHD_PGO_GENERATE and SSHD_PGO_PROFILE are mutually exclusive")
endif()
if(SSHD_PGO_GENERATE OR NOT SSHD_PGO_PROFILE STREQUAL "")
  if(NOT CMAKE_C_COMPILER_ID MATCHES "Clang" OR NOT CMAKE_CXX_COMPILER_ID MATCHES "Clang")
    message(FATAL_ERROR "SSHD PGO requires Clang/LLVM")
  endif()
  if(SSHD_PGO_GENERATE)
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:-fprofile-instr-generate>")
    add_link_options(-fprofile-instr-generate)
    add_compile_definitions(SSHD_PGO_INSTRUMENTED=1)
    message(STATUS "SSHD: LLVM PGO instrumentation enabled")
  else()
    if(NOT EXISTS "${SSHD_PGO_PROFILE}")
      message(FATAL_ERROR "SSHD_PGO_PROFILE does not exist: ${SSHD_PGO_PROFILE}")
    endif()
    add_compile_options("$<$<COMPILE_LANGUAGE:C,CXX,OBJCXX>:-fprofile-instr-use=${SSHD_PGO_PROFILE}>")
    message(STATUS "SSHD: LLVM PGO profile: ${SSHD_PGO_PROFILE}")
  endif()
endif()

if(APPLE)
  enable_language(OBJCXX)
  set(CMAKE_OBJCXX_STANDARD 26)
  set(CMAKE_OBJCXX_STANDARD_REQUIRED ON)
  set(CMAKE_OBJCXX_EXTENSIONS OFF)
  add_compile_options($<$<COMPILE_LANGUAGE:OBJCXX>:-fobjc-arc>)
endif()

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_C_EXTENSIONS OFF)
set(CMAKE_CXX_STANDARD 26)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_EXTENSIONS OFF)
# No C++ modules are used. The module scan would otherwise preprocess every source of a target before
# any of its objects builds, so one source that does not compile blocks the whole target.
set(CMAKE_CXX_SCAN_FOR_MODULES OFF)

include(cmake/CompilerChecks.cmake)
include(cmake/PpuCodegen.cmake)
include(cmake/SpuCodegen.cmake)
include(cmake/Bench.cmake)
include(cmake/Runtime.cmake)
include(cmake/Render.cmake)
include(cmake/Overlay.cmake)
include(cmake/Media.cmake)
include(cmake/Audio.cmake)
include(cmake/Input.cmake)
include(cmake/Decrypt.cmake)
include(cmake/GameConfig.cmake)

set(CMAKE_RUNTIME_OUTPUT_DIRECTORY ${CMAKE_BINARY_DIR}/bin)

enable_testing()

add_executable(sshd_runtime runtime/src/main.cpp)
target_compile_definitions(sshd_runtime PRIVATE SSHD_VERSION="${PROJECT_VERSION}")
target_compile_options(sshd_runtime PRIVATE ${SSHD_ARCH_FLAGS} -Wall -Wextra)
target_include_directories(sshd_runtime PRIVATE runtime/include)

add_library(sshd_ppu_stub STATIC runtime/src/ppu_stub_runtime.cpp)
target_compile_options(sshd_ppu_stub PRIVATE ${SSHD_ARCH_FLAGS} -Wall -Wextra)
target_include_directories(sshd_ppu_stub PUBLIC runtime/include)
if(APPLE)
  # The stub refers to the generated import table through weak references (see ppu_stub_runtime.cpp).
  # ld64 rejects an undefined weak symbol unless it is declared allowed; dyld then binds it to null.
  target_link_options(sshd_ppu_stub INTERFACE
    LINKER:-U,_kSshdPpuImports LINKER:-U,_kSshdPpuImportCount)
endif()

add_library(sshd_spu_stub STATIC runtime/src/spu_stub_runtime.cpp)
target_compile_options(sshd_spu_stub PRIVATE ${SSHD_ARCH_FLAGS} -Wall -Wextra)
target_include_directories(sshd_spu_stub PUBLIC runtime/include)

set(SSHD_EBOOT_ELF "" CACHE FILEPATH "Decrypted EBOOT.elf for optional tests")
# The file sshd_inputs produces; a build step, so it need not exist yet.
if(SSHD_EBOOT_ELF STREQUAL "" AND NOT SSHD_EBOOT_BIN STREQUAL "")
  set_property(CACHE SSHD_EBOOT_ELF PROPERTY VALUE "${SSHD_INPUTS_DIR}/EBOOT.elf")
endif()

sshd_runtime_add_core()
sshd_ppu_add_overrides()
sshd_ppu_add_generated_tree()
sshd_spu_add_generated_tree()
sshd_decrypt_add()
sshd_media_add()
sshd_audio_add()
sshd_input_add()
sshd_render_add()
sshd_overlay_add()
if(TARGET sshd_game)
  set(SSHD_GAME_ARTWORK_RC "${CMAKE_SOURCE_DIR}/generated/setup-game-artwork/game_artwork.rc")
  if(WIN32 AND EXISTS "${SSHD_GAME_ARTWORK_RC}")
    enable_language(RC)
    target_sources(sshd_game PRIVATE "${SSHD_GAME_ARTWORK_RC}")
  endif()
  # sshd_media supplies the FFmpeg codec factory that game_main installs for the cellSail movies; the
  # FFmpeg DLLs it loads at run time are copied next to the executable by sshd_ffmpeg_runtime.
  target_link_libraries(sshd_game PRIVATE sshd_render sshd_overlay sshd_media)
  add_dependencies(sshd_game sshd_ffmpeg_runtime)
  sshd_game_config_write()
  option(SSHD_GUI_SUBSYSTEM "Link the game as a Windows GUI program, without a console window" OFF)
  if(WIN32 AND SSHD_GUI_SUBSYSTEM)
    target_link_options(sshd_game PRIVATE "SHELL:-Xlinker /SUBSYSTEM:WINDOWS" "SHELL:-Xlinker /ENTRY:mainCRTStartup")
  endif()
  if(WIN32)
    add_custom_command(TARGET sshd_game POST_BUILD
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "$<TARGET_FILE:sshd_game>" "$<TARGET_FILE_DIR:sshd_game>/ssHD.exe"
      COMMENT "Writing game executable copy ssHD.exe")
  endif()
endif()

add_executable(sshd_ppu_header_check runtime/tests/ppu_header_check.c)
sshd_ppu_codegen_options(sshd_ppu_header_check)
target_link_libraries(sshd_ppu_header_check PRIVATE sshd_ppu_stub)

add_test(NAME runtime_version COMMAND sshd_runtime --version)
set_tests_properties(runtime_version PROPERTIES
  PASS_REGULAR_EXPRESSION "sshd_runtime 0\\.1\\.0")

add_test(NAME runtime_musttail COMMAND sshd_runtime --selftest-musttail)
set_tests_properties(runtime_musttail PROPERTIES
  PASS_REGULAR_EXPRESSION "musttail ok")

add_test(NAME ppu_header_check COMMAND sshd_ppu_header_check)
set_tests_properties(ppu_header_check PROPERTIES
  PASS_REGULAR_EXPRESSION "ppu header ok")

sshd_spu_add_header_check()

find_package(Python3 3.10 REQUIRED COMPONENTS Interpreter)
sshd_ppu_add_semantics_harness()
sshd_spu_add_semantics_harness()
sshd_spu_add_programs()
sshd_spu_add_lzma_inflate_test()
sshd_bench_add()

add_test(NAME tools_unittest
  COMMAND ${Python3_EXECUTABLE} -B -m unittest discover -s tools/tests -t tools
  WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
# The relocated libsre exists only when the build produces it from a firmware update.
set(SSHD_LIBSRE_ELF "")
if(NOT SSHD_PUP STREQUAL "")
  set(SSHD_LIBSRE_ELF "${SSHD_INPUTS_DIR}/libsre.reloc.elf")
endif()
set_tests_properties(tools_unittest PROPERTIES
  ENVIRONMENT "SSHD_EBOOT_ELF=${SSHD_EBOOT_ELF};SSHD_LIBSRE_ELF=${SSHD_LIBSRE_ELF};SSHD_SKIP_REAL_EBOOT=1")
if(NOT SSHD_EBOOT_ELF STREQUAL "")
  add_test(NAME tools_unittest_eboot
    COMMAND ${Python3_EXECUTABLE} -B -m unittest discover -s tools/tests -t tools -p test_real_eboot.py
    WORKING_DIRECTORY ${CMAKE_SOURCE_DIR})
  set_tests_properties(tools_unittest_eboot PROPERTIES
    LABELS eboot
    ENVIRONMENT "SSHD_EBOOT_ELF=${SSHD_EBOOT_ELF}")
endif()
