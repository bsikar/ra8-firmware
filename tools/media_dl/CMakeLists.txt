# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

# media_dl -- native host CLI for the RA8 e-reader's comic/manga/manhwa/webtoon
# downloader. Builds for the DEV MACHINE (x86_64 / arm64), NOT the ARM target:
# it links the firmware's platform-agnostic pieces (currently the ra8_err
# contract; ra8_jof / ra8_fs to follow) behind dependency-inversion seams and
# injects a host libcurl backend for the network. The logic above the seam is
# the same code the RA8 will run once the ESP32-C6 radio backend exists.
cmake_minimum_required(VERSION 3.20)
project(media_dl C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

# Keep host-tool coverage opt-in, matching tests/cmake/host_config.cmake. The
# flags are added after project() so CMake's compiler probe is not instrumented
# and gcovr never mistakes compiler-identification objects for project code.
option(RA8_COVERAGE "Emit gcov coverage data for media_dl and its tests" OFF)
if(RA8_COVERAGE)
  add_compile_options(
    --coverage
    -fprofile-arcs
    -ftest-coverage
    -O0
    -g
  )
  add_link_options(--coverage -fprofile-arcs -ftest-coverage)
endif()

find_package(CURL REQUIRED)

# Vendored miniz (the same third-party ZIP/DEFLATE the firmware comic reader
# uses) provides CBZ writing and gzip DEFLATE -- so archive export needs no
# system libraries. SOUP: compiled warning-free, exempt from our style bar.
set(MDL_MINIZ ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/third_party/miniz/miniz.c)
set_source_files_properties(
  ${MDL_MINIZ} PROPERTIES COMPILE_OPTIONS "-w;-fno-strict-aliasing"
                          COMPILE_DEFINITIONS "MINIZ_NO_STDIO;MINIZ_NO_MALLOC"
)

# The JOF exporter reuses the firmware's own JOF producer + streaming JPEG
# decoder host-side (RA8_OFF_TARGET), so a .jof the CLI writes is byte-identical
# to one the RA8 produces. These are hand-written first-party files, so they
# carry the project warning bar (-Wall -Wextra -Werror) from the target below
# like every other build that compiles them -- NOT the `-w` that SOUP gets.
# Compiling them quietly here hid host-only findings the ARM build never sees,
# which is what a 64-bit host build is for (#309).
set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../../libs)
set(MDL_PUBLIC_INC ${CMAKE_CURRENT_SOURCE_DIR}/inc ${FW}/ra8_mdl/inc)
set(MDL_FS_SRC
    ${FW}/if/src/fw_if_fs.c
    ${FW}/if/src/fw_if_fs_dir.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../port/posix/src/fw_if_fs_posix.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../port/posix/src/fw_if_fs_posix_bind.c
    ${CMAKE_CURRENT_SOURCE_DIR}/../../port/posix/src/fw_if_fs_posix_common.c
)
set(MDL_FS_INC ${FW}/if/inc ${CMAKE_CURRENT_SOURCE_DIR}/../../port/posix/inc
               ${CMAKE_CURRENT_SOURCE_DIR}/../../port/posix/src
)
set(MDL_JOF_SRC
    ${FW}/ra8_jof/src/ra8_jof.c
    ${FW}/ra8_jof/src/ra8_jof_produce.c
    ${FW}/ra8_jof/src/ra8_jof_produce_webp.c
    ${FW}/ra8_jof/src/ra8_jof_png.c
    ${FW}/ra8_jof/src/ra8_jof_png_chunk.c
    ${FW}/ra8_jpeg/src/ra8_jpeg_sw.c
    ${FW}/ra8_jpeg/src/ra8_jpeg_sw_decode.c
    ${FW}/ra8_jpeg/src/ra8_jpeg_sw_stream.c
    ${FW}/ra8_io/src/ra8_io_compress.c
    ${FW}/ra8_core/src/ra8_log.c
    ${FW}/ra8_core/src/ra8_error_handler.c
    ${FW}/ra8_core/src/ra8_decomp_limits.c
)
set(MDL_JOF_INC
    ${FW}/ra8_jof/inc
    ${FW}/ra8_jof/src
    ${FW}/ra8_jpeg/inc
    ${FW}/ra8_jpeg/src
    ${FW}/ra8_io/inc
)
set(MDL_STREAM_CORE_SRC ${FW}/ra8_io/src/ra8_io_stream.c)
set(MDL_STREAM_RAM_SRC ${FW}/ra8_io/src/ra8_io_stream_ram.c)
set(MDL_STREAM_POSIX_SRC ${CMAKE_CURRENT_SOURCE_DIR}/../../port/posix/src/ra8_io_stream_posix.c)
set(MDL_RABOOK_READER_SRC
    ${FW}/ra8_book/src/ra8_book.c ${FW}/ra8_book/src/ra8_book_stream.c
    ${FW}/ra8_book/src/ra8_book_chunked.c ${FW}/ra8_book/src/ra8_book_chunked_validate.c
)
set(MDL_RABOOK_READER_INC ${FW}/ra8_book/inc ${FW}/ra8_book/src ${FW}/ra8_mem/inc)

# WebP decode for the JOF exporter: the sites this CLI scrapes serve .webp pages
# (mdl_export's page-extension table has always accepted them), so the
# producer's WebP arm must be the real transcode. One call; the recipe is shared
# with the firmware app build and the host test build.
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ra8_webp_vendor.cmake)

# One responsibility per exporter translation unit. Both the CLI and every
# focused host-test binary compile the same complete writer set.
set(MDL_EXTRACT_SRC src/mdl_extract.c src/mdl_extract_hits.c)
set(MDL_CACHE_SRC src/mdl_cache.c src/mdl_cache_io.c)

set(MDL_EXPORT_SRC
    src/mdl_export.c
    src/mdl_export_workspace.c
    src/mdl_export_io.c
    src/mdl_export_meta.c
    src/mdl_export_zip.c
    src/mdl_export_tar.c
    src/mdl_export_epub.c
    src/mdl_export_jof.c
)
set(MDL_VERIFY_SRC src/mdl_verify.c)

# The strict reader and bounded EPUB -> RABOOK compiler are shared by the CLI
# and every focused downloader test. Build them once: duplicating the firmware
# parser/raster stack into each test executable wastes link time and can let
# per-target compile definitions drift.
set(MDL_RABOOK_RUNTIME_SRC
    src/mdl_export_rabook.c
    src/mdl_export_rabook_io.c
    src/mdl_verify_rabook.c
    ${MDL_RABOOK_READER_SRC}
    ${FW}/ra8_xml/src/ra8_xml.c
    ${FW}/ra8_xml/src/ra8_xml_doctype.c
    ${FW}/ra8_epub/src/ra8_epub_miniz_alloc.c
    ${FW}/ra8_epub/src/ra8_epub_open.c
    ${FW}/ra8_epub/src/ra8_epub_entry.c
    ${FW}/ra8_epub/src/ra8_epub_chapter.c
    ${FW}/ra8_epub/src/ra8_epub_xml_shim.c
    ${FW}/ra8_epub/src/ra8_epub_zip_guard.c
    ${FW}/ra8_rabook_compile/src/ra8_rabook_compile.c
    ${FW}/ra8_rabook_compile/src/ra8_rabook_finalize.c
    ${FW}/ra8_rabook_compile/src/ra8_rabook_container.c
    ${FW}/ra8_rabook_compile/src/ra8_rabook_gray4.c
    ${FW}/ra8_rabook_compile/src/ra8_rabook_pipeline.c
    ${FW}/ra8_rabook_compile/src/ra8_rabook_xml_shim.c
    ${FW}/ra8_io/src/ra8_io_compress.c
    ${FW}/ra8_reflow/src/ra8_img_arena.c
    ${FW}/ra8_reflow/src/ra8_stbtt_alloc.c
    ${FW}/ra8_reflow/src/ra8_stbtt_guard.c
    ${FW}/third_party/stb/stb_image_impl.c
    ${FW}/third_party/stb/stb_truetype_impl.c
)
add_library(mdl_rabook_runtime STATIC ${MDL_RABOOK_RUNTIME_SRC})
target_include_directories(
  mdl_rabook_runtime
  PRIVATE ${MDL_PUBLIC_INC}
          ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${FW}/ra8_core/inc
          ${FW}/third_party/miniz
          ${FW}/third_party/stb
          ${FW}/ra8_book/inc
          ${FW}/ra8_book/src
          ${FW}/ra8_mem/inc
          ${FW}/ra8_io/inc
          ${FW}/ra8_xml/inc
          ${FW}/ra8_xml/src
          ${FW}/ra8_epub/inc
          ${FW}/ra8_epub/src
          ${FW}/ra8_rabook_compile/inc
          ${FW}/ra8_rabook_compile/src
          ${FW}/ra8_reflow/inc
          ${FW}/ra8_reflow/src
          ${FW}/ra8_fs/inc
          ${MDL_FS_INC}
)
target_compile_definitions(
  mdl_rabook_runtime
  PRIVATE _GNU_SOURCE
          MINIZ_NO_TIME
          MINIZ_NO_STDIO
          MINIZ_NO_MALLOC
          RA8_OFF_TARGET
)
target_compile_options(mdl_rabook_runtime PRIVATE -Wall -Wextra -Werror)
set_source_files_properties(${FW}/third_party/stb/stb_image_impl.c PROPERTIES COMPILE_OPTIONS -w)
set_property(
  SOURCE ${FW}/third_party/stb/stb_truetype_impl.c
  APPEND
  PROPERTY COMPILE_OPTIONS
           -w
           -include
           "${FW}/ra8_reflow/inc/ra8_stbtt_alloc.h"
           "-DSTBTT_malloc(x,u)=ra8_stbtt_malloc(x)"
           "-DSTBTT_free(x,u)=ra8_stbtt_free(x)"
)
target_link_libraries(mdl_rabook_runtime PUBLIC m)

add_executable(
  media_dl
  src/main.c
  src/mdl_app_admin.c
  src/mdl_app_cover.c
  src/mdl_app_direct.c
  src/mdl_app_library.c
  src/mdl_app_prepare.c
  src/mdl_app_series.c
  src/mdl_app_storage.c
  src/mdl_cli.c
  src/mdl_cli_nums.c
  src/mdl_cli_stream.c
  src/mdl_cli_usage.c
  src/mdl_stream.c
  src/mdl_net.c
  src/mdl_net_curl.c
  src/mdl_net_curl_credentials.c
  src/mdl_host_credentials.c
  src/mdl_url_guard.c
  src/mdl_sanitize.c
  src/mdl_pathfs.c
  src/mdl_robots.c
  src/mdl_session.c
  ${MDL_EXTRACT_SRC}
  src/mdl_search.c
  src/mdl_discover.c
  src/mdl_politeness.c
  src/mdl_config.c
  src/mdl_hash.c
  src/mdl_storage.c
  ${MDL_CACHE_SRC}
  src/mdl_urlname.c
  src/mdl_state.c
  src/mdl_state_codec.c
  src/mdl_state_decimal.c
  src/mdl_state_store.c
  src/mdl_fetch.c
  src/mdl_fetch_body.c
  src/mdl_fetch_page.c
  src/mdl_report.c
  src/mdl_pack.c
  src/mdl_library.c
  ${MDL_EXPORT_SRC}
  ${MDL_VERIFY_SRC}
  ${MDL_JOF_SRC}
  ${MDL_FS_SRC}
  ${MDL_STREAM_CORE_SRC}
  ${MDL_STREAM_POSIX_SRC}
  ${MDL_MINIZ}
)

target_include_directories(
  media_dl
  PRIVATE ${MDL_PUBLIC_INC}
          # Reuse the firmware error contract host-side. RA8_OFF_TARGET is the
          # same switch the host unit-test build uses so register-reaching code
          # takes the host-safe path.
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/third_party/miniz
          ${MDL_JOF_INC}
          ${MDL_RABOOK_READER_INC}
          ${MDL_FS_INC}
)

target_compile_definitions(media_dl PRIVATE _GNU_SOURCE MINIZ_NO_TIME RA8_OFF_TARGET)
target_compile_options(media_dl PRIVATE -Wall -Wextra -Werror)
target_link_libraries(media_dl PRIVATE CURL::libcurl mdl_rabook_runtime)
ra8_webp_attach(media_dl ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Host unit tests (unity_minimal harness, like the repo's tests/). The pure
# logic under test here (extract/config/export/url_guard/sanitize/robots) needs
# no libcurl, so this binary links only those units + vendored miniz. The
# network-seam tests (which do need the libcurl backend) live in their own
# binary below so neither test file exceeds the size cap.
enable_testing()

set(MDL_TEST_COMMON_SRC
    src/mdl_url_guard.c
    src/mdl_sanitize.c
    src/mdl_urlname.c
    src/mdl_robots.c
    ${MDL_EXTRACT_SRC}
    src/mdl_config.c
    src/mdl_politeness.c
    src/mdl_hash.c
    src/mdl_storage.c
    tests/mdl_test_storage.c
    ${MDL_EXPORT_SRC}
    ${MDL_VERIFY_SRC}
    src/mdl_cli.c
    src/mdl_cli_nums.c
    src/mdl_cli_stream.c
    src/mdl_cli_usage.c
    src/mdl_stream.c
    src/mdl_net.c
    ${MDL_JOF_SRC}
    ${MDL_FS_SRC}
    ${MDL_STREAM_CORE_SRC}
    ${MDL_STREAM_RAM_SRC}
    ${MDL_MINIZ}
)

# Add one focused unit-test executable over the complete downloader core.
function(mdl_add_unit_test target source)
  add_executable(${target} ${source} ${MDL_TEST_COMMON_SRC})
  target_include_directories(
    ${target}
    PRIVATE ${MDL_PUBLIC_INC}
            ${CMAKE_CURRENT_SOURCE_DIR}/src
            ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
            ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/third_party/miniz
            ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
            ${MDL_JOF_INC}
            ${MDL_RABOOK_READER_INC}
            ${MDL_FS_INC}
  )
  target_compile_definitions(${target} PRIVATE _GNU_SOURCE MINIZ_NO_TIME RA8_OFF_TARGET)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(${target} PRIVATE mdl_rabook_runtime)
  ra8_webp_attach(${target} ${CMAKE_CURRENT_SOURCE_DIR}/../..)
  add_test(NAME ${target} COMMAND ${target})
endfunction()

mdl_add_unit_test(test_media_dl tests/test_media_dl.c)
target_sources(test_media_dl PRIVATE tests/test_media_dl_logic_cli.c)
mdl_add_unit_test(test_media_dl_export tests/test_media_dl_export.c)
target_sources(test_media_dl_export PRIVATE tests/test_media_dl_export_xml.c)
add_executable(
  test_media_dl_rabook_verify
  tests/test_media_dl_rabook_verify.c ${MDL_TEST_COMMON_SRC}
  ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/support/rabook_compile_test_fixture.c
)
target_include_directories(
  test_media_dl_rabook_verify
  PRIVATE ${MDL_PUBLIC_INC}
          ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/third_party/miniz
          ${FW}/ra8_rabook_compile/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
          ${MDL_JOF_INC}
          ${MDL_RABOOK_READER_INC}
          ${MDL_FS_INC}
)
target_compile_definitions(
  test_media_dl_rabook_verify PRIVATE _GNU_SOURCE MINIZ_NO_TIME RA8_OFF_TARGET
)
target_compile_options(test_media_dl_rabook_verify PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_media_dl_rabook_verify PRIVATE mdl_rabook_runtime)
ra8_webp_attach(test_media_dl_rabook_verify ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_test(NAME test_media_dl_rabook_verify COMMAND test_media_dl_rabook_verify)
mdl_add_unit_test(test_media_dl_export_fault tests/test_media_dl_export_fault.c)
target_sources(
  test_media_dl_export_fault
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/support/mdl_state_fs_fault.c
)
mdl_add_unit_test(test_media_dl_metadata tests/test_media_dl_metadata.c)

# #310 network-seam tests: a scripted fake ::mdl_net_iface_t backend drives the
# vtable dispatchers with no network, and the libcurl backend's pure transfer
# classifier + bounded write callback are exercised directly. This binary links
# the dispatchers, the politeness clock seam, the libcurl backend and its URL
# guards (so it needs libcurl); src/ is on the include path for the promoted
# helpers in mdl_net_curl_internal.h.
add_executable(
  test_media_dl_net
  tests/test_media_dl_net.c
  tests/test_media_dl_net_curl.c
  src/mdl_net.c
  src/mdl_net_curl.c
  src/mdl_net_curl_credentials.c
  src/mdl_politeness.c
  src/mdl_url_guard.c
  ${MDL_FS_SRC}
)
target_include_directories(
  test_media_dl_net
  PRIVATE ${MDL_PUBLIC_INC}
          ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
          ${MDL_FS_INC}
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
)
target_compile_definitions(test_media_dl_net PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_net PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_media_dl_net PRIVATE CURL::libcurl)
add_test(NAME test_media_dl_net COMMAND test_media_dl_net)

# Host credential policy tests keep path opening at composition while proving
# strict cookie/CA byte validation and nanosecond mutation detection.
add_executable(
  test_media_dl_credentials
  tests/test_media_dl_credentials.c
  src/mdl_net.c
  src/mdl_net_curl.c
  src/mdl_net_curl_credentials.c
  src/mdl_host_credentials.c
  src/mdl_url_guard.c
)
target_include_directories(
  test_media_dl_credentials
  PRIVATE ${MDL_PUBLIC_INC} ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
)
target_compile_definitions(test_media_dl_credentials PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_credentials PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_media_dl_credentials PRIVATE CURL::libcurl)
add_test(NAME test_media_dl_credentials COMMAND test_media_dl_credentials)

# #302 session-level tests: the honest User-Agent + robots.txt gating driven
# end-to-end through the #310 mdl_net vtable mock. A scripted fake backend
# serves robots.txt and records the transmitted User-Agent, proving the honest
# UA reaches the wire, a Disallow refuses a URL before any content fetch, and a
# Crawl-delay raises the per-host politeness floor -- all with no network. The
# fake backend replaces libcurl, so this binary links only the pure units above
# the seam and needs no CURL.
add_executable(
  test_media_dl_session
  tests/test_media_dl_session.c
  src/mdl_stream.c
  src/mdl_session.c
  src/mdl_robots.c
  src/mdl_net.c
  src/mdl_politeness.c
  src/mdl_url_guard.c
  ${MDL_STREAM_CORE_SRC}
  ${MDL_STREAM_RAM_SRC}
  ${MDL_STREAM_POSIX_SRC}
  ${FW}/ra8_core/src/ra8_log.c
  ${FW}/ra8_core/src/ra8_error_handler.c
)
target_include_directories(
  test_media_dl_session
  PRIVATE ${MDL_PUBLIC_INC}
          ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_io/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../port/posix/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
)
target_compile_definitions(test_media_dl_session PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_session PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_media_dl_session COMMAND test_media_dl_session)

# #305 library-state tests: the persistent state store + its content-identity
# hashing + the URL-to-name helpers, exercised as pure logic (no network, no
# libcurl). Covers save/load round-trips, atomic-write survival, corrupt-file
# handling, chapter/page lookup, and coverage/gaps rendering.
add_executable(
  test_media_dl_state
  tests/test_media_dl_state.c
  tests/test_media_dl_state_metadata.c
  src/mdl_state.c
  src/mdl_state_codec.c
  src/mdl_state_decimal.c
  src/mdl_state_store.c
  src/mdl_hash.c
  src/mdl_storage.c
  src/mdl_urlname.c
  src/mdl_sanitize.c
  ${MDL_FS_SRC}
)
target_include_directories(
  test_media_dl_state PRIVATE ${MDL_PUBLIC_INC} ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
                              ${CMAKE_CURRENT_SOURCE_DIR}/../../tests ${MDL_FS_INC}
)
target_compile_definitions(test_media_dl_state PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_state PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_media_dl_state COMMAND test_media_dl_state)

# #305 fetch-orchestrator tests: the resumable, deduping download loop driven
# end-to-end through the #310 mdl_net vtable mock with no network. A scripted
# fake backend writes deterministic page bytes into a temp directory, so the
# suite asserts that a first run fetches N pages, a second run fetches only the
# new ones, an interrupted run resumes to a byte-identical result, and a corrupt
# state file rebuilds cleanly. The fake replaces libcurl, so this binary links
# only the pure units above the seam and needs no CURL.
add_executable(
  test_media_dl_fetch
  tests/test_media_dl_fetch.c
  src/mdl_stream.c
  src/mdl_fetch.c
  src/mdl_fetch_body.c
  src/mdl_fetch_page.c
  ${MDL_CACHE_SRC}
  src/mdl_state.c
  src/mdl_state_codec.c
  src/mdl_state_decimal.c
  src/mdl_state_store.c
  src/mdl_hash.c
  src/mdl_storage.c
  src/mdl_urlname.c
  src/mdl_sanitize.c
  src/mdl_pathfs.c
  src/mdl_session.c
  src/mdl_robots.c
  src/mdl_net.c
  src/mdl_politeness.c
  src/mdl_url_guard.c
  ${MDL_EXTRACT_SRC}
  ${MDL_FS_SRC}
  ${MDL_STREAM_CORE_SRC}
  ${MDL_STREAM_RAM_SRC}
  ${FW}/ra8_core/src/ra8_log.c
  ${FW}/ra8_core/src/ra8_error_handler.c
)
target_include_directories(
  test_media_dl_fetch
  PRIVATE ${MDL_PUBLIC_INC}
          ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_io/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
          ${MDL_FS_INC}
)
target_compile_definitions(test_media_dl_fetch PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_fetch PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_media_dl_fetch COMMAND test_media_dl_fetch)

add_executable(
  test_media_dl_fetch_cache
  tests/test_media_dl_fetch_cache.c
  src/mdl_stream.c
  src/mdl_fetch.c
  src/mdl_fetch_body.c
  src/mdl_fetch_page.c
  ${MDL_CACHE_SRC}
  src/mdl_state.c
  src/mdl_state_codec.c
  src/mdl_state_decimal.c
  src/mdl_state_store.c
  src/mdl_hash.c
  src/mdl_storage.c
  src/mdl_urlname.c
  src/mdl_sanitize.c
  src/mdl_pathfs.c
  src/mdl_session.c
  src/mdl_robots.c
  src/mdl_net.c
  src/mdl_politeness.c
  src/mdl_url_guard.c
  ${MDL_EXTRACT_SRC}
  ${MDL_FS_SRC}
  ${MDL_STREAM_CORE_SRC}
  ${MDL_STREAM_RAM_SRC}
  ${FW}/ra8_core/src/ra8_log.c
  ${FW}/ra8_core/src/ra8_error_handler.c
)
target_include_directories(
  test_media_dl_fetch_cache
  PRIVATE ${MDL_PUBLIC_INC}
          ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_io/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
          ${MDL_FS_INC}
)
target_compile_definitions(test_media_dl_fetch_cache PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_fetch_cache PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_media_dl_fetch_cache COMMAND test_media_dl_fetch_cache)

# Persistent per-host cache contract: a scripted network proves 304 reuse,
# changed-entity publication, corrupt-index recovery, host isolation, and
# explicit refetch behavior through the real portable POSIX storage adapter.
add_executable(
  test_media_dl_cache
  tests/test_media_dl_cache.c
  tests/mdl_test_storage.c
  ${MDL_CACHE_SRC}
  src/mdl_hash.c
  src/mdl_storage.c
  src/mdl_sanitize.c
  src/mdl_url_guard.c
  ${MDL_FS_SRC}
)
target_include_directories(
  test_media_dl_cache
  PRIVATE ${MDL_PUBLIC_INC}
          ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_CURRENT_SOURCE_DIR}/tests
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_io/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
          ${MDL_FS_INC}
)
target_compile_definitions(test_media_dl_cache PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_cache PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_media_dl_cache COMMAND test_media_dl_cache)

# The network body sink and one-stage publication contract are qualified at
# their portable filesystem boundary. The POSIX adapter supplies real durable
# transactions while the delegating facade independently injects every fault
# phase, short backend writes, and cleanup errors.
add_executable(
  test_media_dl_fetch_body
  tests/test_media_dl_fetch_body.c
  src/mdl_fetch_body.c
  src/mdl_storage.c
  src/mdl_hash.c
  src/mdl_urlname.c
  src/mdl_sanitize.c
  ../../tests/support/mdl_state_fs_fault.c
  ${MDL_FS_SRC}
)
target_include_directories(
  test_media_dl_fetch_body
  PRIVATE ${MDL_PUBLIC_INC}
          ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tests/support
          ${MDL_FS_INC}
)
target_compile_definitions(test_media_dl_fetch_body PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_fetch_body PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_media_dl_fetch_body COMMAND test_media_dl_fetch_body)

# #301 governor tests: the per-host politeness governor (token-bucket rate
# limiting, exponential backoff + jitter on 429/503, Retry-After honouring in
# both forms, and the per-host in-flight cap) driven entirely through the
# injectable clock/sleep seams -- no network, no real time. The governor lives
# in mdl_politeness.c and is self-contained, so this binary links only that
# unit.
add_executable(test_media_dl_governor tests/test_media_dl_governor.c src/mdl_politeness.c)
target_include_directories(
  test_media_dl_governor
  PRIVATE ${MDL_PUBLIC_INC} ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
)
target_compile_definitions(test_media_dl_governor PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_governor PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_media_dl_governor COMMAND test_media_dl_governor)

# #304 search/discovery tests: the pure parse + policy path -- the titled-anchor
# hit scanner (mdl_extract_hits), the RFC 3986 query encoder, the `{q}` template
# expander, and the honest zero-vs-broken result classifier -- driven against a
# captured search-results fixture with NO network. Covers the matched, zero-hit
# and malformed-markup cases the issue requires. Links only the two pure units.
add_executable(
  test_media_dl_search tests/test_media_dl_search.c src/mdl_search.c ${MDL_EXTRACT_SRC}
)
target_include_directories(
  test_media_dl_search
  PRIVATE ${MDL_PUBLIC_INC} ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
)
target_compile_definitions(test_media_dl_search PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_search PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_media_dl_search COMMAND test_media_dl_search)

# CLI grammar/dispatch qualification stays independent of exporter/network
# implementations so a broken mode matrix cannot be hidden by optional tools.
add_executable(
  test_media_dl_cli
  tests/test_media_dl_cli.c
  tests/test_media_dl_cli_matrix.c
  src/mdl_cli.c
  src/mdl_cli_nums.c
  src/mdl_cli_stream.c
  src/mdl_cli_usage.c
  ${MDL_STREAM_CORE_SRC}
  ${MDL_STREAM_RAM_SRC}
  ${FW}/ra8_core/src/ra8_log.c
  ${FW}/ra8_core/src/ra8_error_handler.c
)
target_include_directories(
  test_media_dl_cli
  PRIVATE ${MDL_PUBLIC_INC}
          ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_io/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
)
target_compile_definitions(test_media_dl_cli PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_cli PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_media_dl_cli COMMAND test_media_dl_cli)

# Every checked-in site descriptor is exercised against a sanitised captured
# discovery, series, and chapter fixture. Adding a descriptor requires adding
# its fixture vectors here rather than making an untested support claim.
file(GLOB MDL_SHIPPED_SITE_CONFIGS CONFIGURE_DEPENDS "${CMAKE_CURRENT_SOURCE_DIR}/sites/*.conf")
list(LENGTH MDL_SHIPPED_SITE_CONFIGS MDL_SHIPPED_SITE_COUNT)
add_executable(
  test_media_dl_sites
  tests/test_media_dl_sites.c
  src/mdl_config.c
  ${MDL_EXTRACT_SRC}
  src/mdl_search.c
  src/mdl_urlname.c
  src/mdl_sanitize.c
  src/mdl_politeness.c
  src/mdl_hash.c
  src/mdl_storage.c
  tests/mdl_test_storage.c
  ${MDL_FS_SRC}
)
target_include_directories(
  test_media_dl_sites
  PRIVATE ${MDL_PUBLIC_INC}
          ${CMAKE_CURRENT_SOURCE_DIR}/tests
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
          ${MDL_FS_INC}
)
target_compile_definitions(
  test_media_dl_sites
  PRIVATE _GNU_SOURCE
          RA8_OFF_TARGET
          MDL_SITE_CONFIG_DIR="${CMAKE_CURRENT_SOURCE_DIR}/sites"
          MDL_SITE_FIXTURE_DIR="${CMAKE_CURRENT_SOURCE_DIR}/tests/fixtures/sites"
          MDL_SHIPPED_SITE_COUNT=${MDL_SHIPPED_SITE_COUNT}
)
target_compile_options(test_media_dl_sites PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_media_dl_sites COMMAND test_media_dl_sites)

# Real socket/libcurl orchestration: deterministic local HTTP, no internet.
find_program(MDL_BASH_EXECUTABLE NAMES bash REQUIRED)
find_program(MDL_OPENSSL_EXECUTABLE NAMES openssl REQUIRED)
find_package(Python3 REQUIRED COMPONENTS Interpreter)
add_test(
  NAME test_media_dl_http_integration
  COMMAND ${MDL_BASH_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/tests/http_integration.sh
          $<TARGET_FILE:media_dl> ${Python3_EXECUTABLE} ${CMAKE_COMMAND} ${MDL_OPENSSL_EXECUTABLE}
)
set_tests_properties(
  test_media_dl_http_integration PROPERTIES TIMEOUT 30 LABELS "integration;network"
)
