include_guard(GLOBAL)

include("${CMAKE_SOURCE_DIR}/cmake/ffmpeg.cmake")

# The FFmpeg build is fetched and verified by tools/fetch-ffmpeg.ps1 into generated/deps/ffmpeg.
# Only its headers are used at build time; the DLLs are loaded at run time (see
# runtime/src/media/ffmpeg_api.h), so no import library is linked. Needs a successful fetch, which
# leaves a marker holding the SHA-256 of the pinned zip.
#
# On macOS FFmpeg comes from Homebrew (`brew install ffmpeg`) and is loaded in place from its lib
# directory; there is no pin and nothing is copied.
function(sshd_media_find_ffmpeg)
  set(SSHD_FFMPEG_DIR "" CACHE PATH
    "Extracted FFmpeg build (default: generated/deps/ffmpeg/<pinned asset name>; macOS: brew --prefix ffmpeg)")
  if(APPLE)
    set(ffmpeg_dir "${SSHD_FFMPEG_DIR}")
    if("${ffmpeg_dir}" STREQUAL "")
      execute_process(
        COMMAND brew --prefix ffmpeg
        OUTPUT_VARIABLE ffmpeg_dir
        OUTPUT_STRIP_TRAILING_WHITESPACE
        ERROR_QUIET
        RESULT_VARIABLE brew_result)
      if(NOT brew_result EQUAL 0)
        set(ffmpeg_dir "")
      endif()
    endif()
    if("${ffmpeg_dir}" STREQUAL "" OR
       NOT EXISTS "${ffmpeg_dir}/include/libavcodec/avcodec.h" OR
       NOT IS_DIRECTORY "${ffmpeg_dir}/lib")
      message(FATAL_ERROR
        "SSHD: FFmpeg not found (prefix '${ffmpeg_dir}'). Install it with `brew install ffmpeg` or "
        "set SSHD_FFMPEG_DIR to a directory that contains include/libavcodec/avcodec.h and lib/.")
    endif()
    set(SSHD_FFMPEG_RESOLVED_DIR "${ffmpeg_dir}" PARENT_SCOPE)
    set(SSHD_FFMPEG_DLLS "" PARENT_SCOPE)
    message(STATUS "SSHD: FFmpeg: ${ffmpeg_dir}")
    return()
  endif()

  set(expected_hash "${SSHD_FFMPEG_SHA256}")
  if("${SSHD_FFMPEG_DIR}" STREQUAL "")
    set(selection_file "${CMAKE_SOURCE_DIR}/generated/deps/ffmpeg/selected-build.json")
    if(EXISTS "${selection_file}")
      set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${selection_file}")
      file(READ "${selection_file}" selection_text)
      string(JSON selection_schema ERROR_VARIABLE selection_error GET "${selection_text}" schema)
      string(JSON selected_asset ERROR_VARIABLE asset_error GET "${selection_text}" asset)
      string(JSON selected_hash ERROR_VARIABLE hash_error GET "${selection_text}" sha256)
      string(LENGTH "${selected_hash}" selected_hash_length)
      if(selection_error OR asset_error OR hash_error OR NOT selection_schema EQUAL 1 OR
         NOT selected_asset MATCHES "^ffmpeg-n9\\.0\\.[0-9]+-[0-9]+-g[0-9a-fA-F]+-win64-lgpl-shared-9\\.0\\.zip$" OR
         NOT selected_hash_length EQUAL 64 OR NOT selected_hash MATCHES "^[0-9a-fA-F]+$")
        message(FATAL_ERROR "SSHD: invalid FFmpeg selection metadata (${selection_file}); rerun tools/fetch-ffmpeg.ps1")
      endif()
      string(TOLOWER "${selected_hash}" expected_hash)
      string(REGEX REPLACE "\\.zip$" "" asset_stem "${selected_asset}")
    else()
      string(REGEX REPLACE "\\.zip$" "" asset_stem "${SSHD_FFMPEG_ASSET}")
    endif()
    set(ffmpeg_dir "${CMAKE_SOURCE_DIR}/generated/deps/ffmpeg/${asset_stem}")
  else()
    set(ffmpeg_dir "${SSHD_FFMPEG_DIR}")
  endif()

  set(marker "${ffmpeg_dir}/.verified-sha256")
  # A fetch after this configure must re-run it.
  set_property(DIRECTORY APPEND PROPERTY CMAKE_CONFIGURE_DEPENDS "${marker}")
  set(fetch_hint "Run: pwsh -NoProfile -File tools/fetch-ffmpeg.ps1")
  if(NOT EXISTS "${marker}")
    message(FATAL_ERROR "SSHD: FFmpeg is not fetched (${marker} not found). ${fetch_hint}")
  endif()
  file(READ "${marker}" marker_hash)
  string(STRIP "${marker_hash}" marker_hash)
  string(TOLOWER "${marker_hash}" marker_hash)
  string(TOLOWER "${expected_hash}" pinned_hash)
  if(NOT marker_hash STREQUAL pinned_hash)
    message(FATAL_ERROR
      "SSHD: FFmpeg in ${ffmpeg_dir} does not match the pin in cmake/ffmpeg.cmake. ${fetch_hint}")
  endif()
  foreach(path include/libavcodec/avcodec.h LICENSE.txt)
    if(NOT EXISTS "${ffmpeg_dir}/${path}")
      message(FATAL_ERROR "SSHD: ${ffmpeg_dir}/${path} is missing. ${fetch_hint} -Force")
    endif()
  endforeach()

  # DLL names carry the library major versions of the pinned build. avcodec imports swresample
  # statically, so it is part of the set although the runtime calls none of its functions.
  set(ffmpeg_dlls "")
  foreach(stem avutil swresample swscale avcodec)
    file(GLOB dll "${ffmpeg_dir}/bin/${stem}-*.dll")
    list(LENGTH dll dll_count)
    if(NOT dll_count EQUAL 1)
      message(FATAL_ERROR "SSHD: expected one ${stem}-*.dll in ${ffmpeg_dir}/bin. ${fetch_hint} -Force")
    endif()
    list(APPEND ffmpeg_dlls "${dll}")
  endforeach()

  set(SSHD_FFMPEG_RESOLVED_DIR "${ffmpeg_dir}" PARENT_SCOPE)
  set(SSHD_FFMPEG_DLLS "${ffmpeg_dlls}" PARENT_SCOPE)
  message(STATUS "SSHD: FFmpeg: ${ffmpeg_dir}")
endfunction()

# The media library: FFmpeg access for movie playback. sshd_runtime_core never sees FFmpeg headers.
# Call after sshd_runtime_add_core() and enable_testing().
function(sshd_media_add)
  sshd_media_find_ffmpeg()

  add_library(sshd_media STATIC runtime/src/media/ffmpeg_api.cpp runtime/src/media/ffmpeg_codecs.cpp)
  target_compile_options(sshd_media PRIVATE ${SSHD_ARCH_FLAGS} -Wall -Wextra)
  target_include_directories(sshd_media PUBLIC ${CMAKE_SOURCE_DIR}/runtime/src)
  # SYSTEM keeps the FFmpeg headers out of our warnings; nothing links against FFmpeg itself.
  target_include_directories(sshd_media SYSTEM PUBLIC "${SSHD_FFMPEG_RESOLVED_DIR}/include")
  target_link_libraries(sshd_media PUBLIC sshd_runtime_core)
  if(APPLE)
    target_compile_definitions(sshd_media PRIVATE SSHD_FFMPEG_LIBRARY_DIR="${SSHD_FFMPEG_RESOLVED_DIR}/lib")
  endif()

  if(WIN32)
    # The runtime loads the DLLs from the directory of the executable. The licence text and the
    # notice travel with them.
    set(bin_dir "${CMAKE_BINARY_DIR}/bin")
    set(licence_dir "${bin_dir}/licenses/ffmpeg")
    set(copied "")
    foreach(dll ${SSHD_FFMPEG_DLLS})
      get_filename_component(dll_name "${dll}" NAME)
      add_custom_command(OUTPUT "${bin_dir}/${dll_name}"
        COMMAND ${CMAKE_COMMAND} -E make_directory "${bin_dir}"
        COMMAND ${CMAKE_COMMAND} -E copy_if_different "${dll}" "${bin_dir}/${dll_name}"
        DEPENDS "${dll}"
        VERBATIM)
      list(APPEND copied "${bin_dir}/${dll_name}")
    endforeach()
    add_custom_command(OUTPUT "${licence_dir}/LICENSE.txt"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${licence_dir}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${SSHD_FFMPEG_RESOLVED_DIR}/LICENSE.txt" "${licence_dir}/LICENSE.txt"
      DEPENDS "${SSHD_FFMPEG_RESOLVED_DIR}/LICENSE.txt"
      VERBATIM)
    add_custom_command(OUTPUT "${licence_dir}/FFMPEG-NOTICE.txt"
      COMMAND ${CMAKE_COMMAND} -E make_directory "${licence_dir}"
      COMMAND ${CMAKE_COMMAND} -E copy_if_different
        "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md" "${licence_dir}/FFMPEG-NOTICE.txt"
      DEPENDS "${CMAKE_SOURCE_DIR}/THIRD_PARTY_NOTICES.md"
      VERBATIM)
    list(APPEND copied "${licence_dir}/LICENSE.txt" "${licence_dir}/FFMPEG-NOTICE.txt")
    add_custom_target(sshd_ffmpeg_runtime ALL DEPENDS ${copied})
  else()
    # Homebrew's dylibs are loaded in place; the target stays so add_dependencies on it keep working.
    add_custom_target(sshd_ffmpeg_runtime ALL)
  endif()

  add_executable(sshd_media_ffmpeg_tests runtime/tests/media_ffmpeg_test.cpp)
  target_compile_options(sshd_media_ffmpeg_tests PRIVATE ${SSHD_ARCH_FLAGS} -Wall -Wextra)
  target_link_libraries(sshd_media_ffmpeg_tests PRIVATE sshd_media)
  add_dependencies(sshd_media_ffmpeg_tests sshd_ffmpeg_runtime)

  add_test(NAME media_ffmpeg COMMAND sshd_media_ffmpeg_tests)
  set_tests_properties(media_ffmpeg PROPERTIES
    PASS_REGULAR_EXPRESSION "media ffmpeg tests: [0-9]+ passed, 0 failed")
endfunction()
