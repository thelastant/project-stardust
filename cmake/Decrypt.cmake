include_guard(GLOBAL)

include("${CMAKE_SOURCE_DIR}/cmake/rpcs3_keys.cmake")

set(SSHD_EBOOT_BIN "" CACHE FILEPATH
  "Disc PS3_GAME/USRDIR/EBOOT.BIN to decrypt into generated/inputs/EBOOT.elf (empty: not decrypted)")
set(SSHD_PUP "" CACHE FILEPATH
  "PS3UPDAT.PUP to extract libsre.prx from (empty: not extracted)")

# Shared by both presets: the decrypted content is identical.
set(SSHD_INPUTS_DIR "${CMAKE_SOURCE_DIR}/generated/inputs")

# sshd_decrypt: the offline decryption tool for EBOOT.BIN and the firmware's libsre.sprx and Japanese font, its synthetic
# unit tests, and the build steps that produce generated/inputs. Call after sshd_runtime_add_core() and
# enable_testing().
function(sshd_decrypt_add)
  set(decrypt_options ${SSHD_ARCH_FLAGS} -Wall -Wextra)

  add_library(sshd_decrypt_lib STATIC
    tools/sshd_decrypt/cli.cpp
    tools/sshd_decrypt/crypto.cpp
    tools/sshd_decrypt/keys.cpp
    tools/sshd_decrypt/sce.cpp
    tools/sshd_decrypt/prx_reloc.cpp
    tools/sshd_decrypt/pup.cpp
    tools/sshd_decrypt/self.cpp)
  target_compile_options(sshd_decrypt_lib PRIVATE ${decrypt_options})
  target_include_directories(sshd_decrypt_lib PUBLIC tools/sshd_decrypt)
  target_link_libraries(sshd_decrypt_lib PUBLIC zlibstatic PRIVATE sshd_crypto)

  add_executable(sshd_decrypt tools/sshd_decrypt/main.cpp)
  target_compile_options(sshd_decrypt PRIVATE ${decrypt_options})
  target_link_libraries(sshd_decrypt PRIVATE sshd_decrypt_lib)

  add_executable(sshd_decrypt_tests
    tools/sshd_decrypt/tests/test_main.cpp
    tools/sshd_decrypt/tests/cli_test.cpp
    tools/sshd_decrypt/tests/crypto_test.cpp
    tools/sshd_decrypt/tests/keys_test.cpp
    tools/sshd_decrypt/tests/sce_test.cpp
    tools/sshd_decrypt/tests/pup_test.cpp
    tools/sshd_decrypt/tests/reloc_test.cpp
    tools/sshd_decrypt/tests/self_test.cpp)
  target_compile_options(sshd_decrypt_tests PRIVATE ${decrypt_options})
  target_link_libraries(sshd_decrypt_tests PRIVATE sshd_decrypt_lib)

  add_test(NAME decrypt_core COMMAND sshd_decrypt_tests)
  set_tests_properties(decrypt_core PROPERTIES
    PASS_REGULAR_EXPRESSION "decrypt core tests: [0-9]+ passed, 0 failed")

  if(SSHD_EBOOT_BIN STREQUAL "" AND SSHD_PUP STREQUAL "")
    message(STATUS "SSHD: input decryption not configured (SSHD_EBOOT_BIN and SSHD_PUP are empty)")
    return()
  endif()

  sshd_resolve_rpcs3_keys()

  # The pins are read by CMake and passed as arguments, so the tool needs no JSON parser.
  set(target_json "${CMAKE_SOURCE_DIR}/config/target.json")
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${target_json}")
  file(READ "${target_json}" target_text)
  string(JSON eboot_bin_sha256 GET "${target_text}" eboot_bin sha256)
  string(JSON eboot_elf_size GET "${target_text}" eboot_elf size)
  string(JSON eboot_elf_sha256 GET "${target_text}" eboot_elf sha256)
  string(JSON libsre_size GET "${target_text}" libsre_prx size)
  string(JSON libsre_sha256 GET "${target_text}" libsre_prx sha256)
  string(JSON libsre_firmware GET "${target_text}" libsre_prx firmware)
  string(JSON libsre_reloc_base GET "${target_text}" libsre_reloc_elf base)
  string(JSON libsre_reloc_size GET "${target_text}" libsre_reloc_elf size)
  string(JSON libsre_reloc_sha256 GET "${target_text}" libsre_reloc_elf sha256)
  string(JSON jpn_font_path GET "${target_text}" jpn_font path)
  string(JSON jpn_font_size GET "${target_text}" jpn_font size)
  string(JSON jpn_font_sha256 GET "${target_text}" jpn_font sha256)

  set(key_files "${SSHD_RPCS3_KEYS_DIR}/key_vault.h" "${SSHD_RPCS3_KEYS_DIR}/key_vault.cpp")
  set(libsre_path "dev_flash/sys/external/libsre.sprx")
  set(input_outputs)
  set(test_dir "${CMAKE_BINARY_DIR}/decrypt_test")

  if(NOT SSHD_EBOOT_BIN STREQUAL "")
    set(eboot_args self --keys "${SSHD_RPCS3_KEYS_DIR}" --in "${SSHD_EBOOT_BIN}"
      --expect-size ${eboot_elf_size} --expect-sha256 ${eboot_elf_sha256}
      --expect-input-sha256 ${eboot_bin_sha256})
    add_custom_command(OUTPUT "${SSHD_INPUTS_DIR}/EBOOT.elf"
      COMMAND sshd_decrypt ${eboot_args} --out "${SSHD_INPUTS_DIR}/EBOOT.elf"
      DEPENDS sshd_decrypt "${SSHD_EBOOT_BIN}" ${key_files}
      COMMENT "Decrypting EBOOT.BIN to generated/inputs/EBOOT.elf"
      VERBATIM)
    list(APPEND input_outputs "${SSHD_INPUTS_DIR}/EBOOT.elf")

    add_test(NAME decrypt_eboot COMMAND sshd_decrypt ${eboot_args} --out "${test_dir}/EBOOT.elf")
    set_tests_properties(decrypt_eboot PROPERTIES
      PASS_REGULAR_EXPRESSION "sha256 ${eboot_elf_sha256}: matches the pin"
      TIMEOUT 120)
  endif()

  if(NOT SSHD_PUP STREQUAL "")
    set(libsre_args pup --keys "${SSHD_RPCS3_KEYS_DIR}" --in "${SSHD_PUP}" --path ${libsre_path}
      --firmware ${libsre_firmware} --expect-size ${libsre_size} --expect-sha256 ${libsre_sha256})
    add_custom_command(OUTPUT "${SSHD_INPUTS_DIR}/libsre.prx"
      COMMAND sshd_decrypt ${libsre_args} --out "${SSHD_INPUTS_DIR}/libsre.prx"
      DEPENDS sshd_decrypt "${SSHD_PUP}" ${key_files}
      COMMENT "Extracting libsre.prx from the PUP to generated/inputs/libsre.prx"
      VERBATIM)
    list(APPEND input_outputs "${SSHD_INPUTS_DIR}/libsre.prx")

    add_test(NAME decrypt_libsre COMMAND sshd_decrypt ${libsre_args} --out "${test_dir}/libsre.prx")
    set_tests_properties(decrypt_libsre PROPERTIES
      PASS_REGULAR_EXPRESSION "sha256 ${libsre_sha256}: matches the pin"
      TIMEOUT 120)

    set(libsre_reloc_args reloc --in "${SSHD_INPUTS_DIR}/libsre.prx" --base ${libsre_reloc_base}
      --expect-input-sha256 ${libsre_sha256} --expect-size ${libsre_reloc_size}
      --expect-sha256 ${libsre_reloc_sha256})
    add_custom_command(OUTPUT "${SSHD_INPUTS_DIR}/libsre.reloc.elf"
      COMMAND sshd_decrypt ${libsre_reloc_args} --out "${SSHD_INPUTS_DIR}/libsre.reloc.elf"
      DEPENDS sshd_decrypt "${SSHD_INPUTS_DIR}/libsre.prx"
      COMMENT "Relocating libsre.prx to generated/inputs/libsre.reloc.elf"
      VERBATIM)
    list(APPEND input_outputs "${SSHD_INPUTS_DIR}/libsre.reloc.elf")

    add_test(NAME decrypt_libsre_reloc COMMAND sshd_decrypt ${libsre_reloc_args} --out "${test_dir}/libsre.reloc.elf")
    set_tests_properties(decrypt_libsre_reloc PROPERTIES
      PASS_REGULAR_EXPRESSION "sha256 ${libsre_reloc_sha256}: matches the pin"
      TIMEOUT 120)

    # A plain TrueType file in the same packages: no SELF decryption, so --raw.
    set(font_name SCE-PS3-SR-R-JPN.TTF)
    set(font_args pup --keys "${SSHD_RPCS3_KEYS_DIR}" --in "${SSHD_PUP}" --path ${jpn_font_path}
      --firmware ${libsre_firmware} --expect-size ${jpn_font_size} --expect-sha256 ${jpn_font_sha256} --raw)
    add_custom_command(OUTPUT "${SSHD_INPUTS_DIR}/${font_name}"
      COMMAND sshd_decrypt ${font_args} --out "${SSHD_INPUTS_DIR}/${font_name}"
      DEPENDS sshd_decrypt "${SSHD_PUP}" ${key_files}
      COMMENT "Extracting the Japanese system font from the PUP to generated/inputs/${font_name}"
      VERBATIM)
    list(APPEND input_outputs "${SSHD_INPUTS_DIR}/${font_name}")

    add_test(NAME decrypt_font COMMAND sshd_decrypt ${font_args} --out "${test_dir}/${font_name}")
    set_tests_properties(decrypt_font PROPERTIES
      PASS_REGULAR_EXPRESSION "sha256 ${jpn_font_sha256}: matches the pin"
      TIMEOUT 120)
  endif()

  add_custom_target(sshd_inputs ALL DEPENDS ${input_outputs})
endfunction()
