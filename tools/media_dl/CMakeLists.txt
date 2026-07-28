# Copyright (c) 2026 Brighton Sikarskie
# SPDX-License-Identifier: MIT
#
# media_dl -- native host CLI for the RA8 e-reader's comic/manga/manhwa/webtoon
# downloader. Builds for the DEV MACHINE (x86_64 / arm64), NOT the ARM target:
# it links the firmware's platform-agnostic pieces (currently the ra8_err
# contract; ra8_jof / ra8_fs to follow) behind dependency-inversion seams
# and injects a host libcurl backend for the network. The logic above the seam
# is the same code the RA8 will run once the ESP32-C6 radio backend exists.
cmake_minimum_required(VERSION 3.20)
project(media_dl C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

find_package(CURL REQUIRED)

# posix_spawn_file_actions_addchdir() is POSIX.1-2024. macOS 26 ships it and
# has DEPRECATED the older _np spelling (which -Werror turns into a build
# failure); glibc 2.36 ships only the _np spelling and does not declare the
# standard name at all. So neither name works everywhere and neither platform
# can be assumed from the other -- probe for it and let the source pick.
include(CheckSymbolExists)
set(CMAKE_REQUIRED_DEFINITIONS -D_GNU_SOURCE)
check_symbol_exists(posix_spawn_file_actions_addchdir "spawn.h" MDL_HAVE_POSIX_SPAWN_ADDCHDIR)
unset(CMAKE_REQUIRED_DEFINITIONS)
if(MDL_HAVE_POSIX_SPAWN_ADDCHDIR)
  set(MDL_ADDCHDIR_DEF MDL_HAVE_POSIX_SPAWN_ADDCHDIR)
else()
  set(MDL_ADDCHDIR_DEF)
endif()

# Vendored miniz (the same third-party ZIP/DEFLATE the firmware comic reader
# uses) provides CBZ writing and gzip DEFLATE -- so archive export needs no
# system libraries. SOUP: compiled warning-free, exempt from our style bar.
set(MDL_MINIZ ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/third_party/miniz/miniz.c)
set_source_files_properties(${MDL_MINIZ} PROPERTIES COMPILE_OPTIONS "-w;-fno-strict-aliasing")

# The JOF exporter reuses the firmware's own JOF producer + streaming
# JPEG decoder host-side (RA8_OFF_TARGET), so a .jof the CLI writes is
# byte-identical to one the RA8 produces. These are hand-written first-party
# files, so they carry the project warning bar (-Wall -Wextra -Werror) from the
# target below like every other build that compiles them -- NOT the `-w` that
# SOUP gets. Compiling them quietly here hid host-only findings the ARM build
# never sees, which is what a 64-bit host build is for (#309).
set(FW ${CMAKE_CURRENT_SOURCE_DIR}/../../libs)
set(MDL_JOF_SRC
    ${FW}/ra8_jof/src/ra8_jof.c
    ${FW}/ra8_jof/src/ra8_jof_produce.c
    ${FW}/ra8_jof/src/ra8_jof_produce_webp.c
    ${FW}/ra8_jof/src/ra8_jof_png.c
    ${FW}/ra8_jof/src/ra8_jof_png_chunk.c
    ${FW}/ra8_hal/src/ra8_jpeg_sw.c
    ${FW}/ra8_hal/src/ra8_jpeg_sw_decode.c
    ${FW}/ra8_hal/src/ra8_jpeg_sw_stream.c
    ${FW}/ra8_io/src/ra8_io_compress.c
    ${FW}/ra8_core/src/ra8_log.c
    ${FW}/ra8_core/src/ra8_error_handler.c
    ${FW}/ra8_core/src/ra8_decomp_limits.c
)
set(MDL_JOF_INC
    ${FW}/ra8_jof/inc
    ${FW}/ra8_jof/src
    ${FW}/ra8_hal/inc
    ${FW}/ra8_hal/src
    ${FW}/ra8_io/inc
)

# WebP decode for the JOF exporter: the sites this CLI scrapes serve .webp pages
# (mdl_export's page-extension table has always accepted them), so the producer's
# WebP arm must be the real transcode. One call; the recipe is shared with the
# firmware app build and the host test build.
include(${CMAKE_CURRENT_SOURCE_DIR}/../../cmake/ra8_webp_vendor.cmake)
# rabook shells out to this python emitter (the RBKC container the C path can't
# write); pass its directory so the CLI can find cbz_compile.py at runtime.
set(MDL_EPUB_COMPILE_DIR ${CMAKE_CURRENT_SOURCE_DIR}/../epub_compile)

add_executable(
  media_dl
  src/main.c
  src/mdl_atomic.c
  src/mdl_cli.c
  src/mdl_net.c
  src/mdl_net_curl.c
  src/mdl_url_guard.c
  src/mdl_sanitize.c
  src/mdl_pathfs.c
  src/mdl_robots.c
  src/mdl_session.c
  src/mdl_extract.c
  src/mdl_search.c
  src/mdl_discover.c
  src/mdl_politeness.c
  src/mdl_config.c
  src/mdl_hash.c
  src/mdl_urlname.c
  src/mdl_state.c
  src/mdl_fetch.c
  src/mdl_report.c
  src/mdl_pack.c
  src/mdl_library.c
  src/mdl_export.c
  src/mdl_export_jof.c
  ${MDL_JOF_SRC}
  ${MDL_MINIZ}
)

target_include_directories(
  media_dl
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc
          # Reuse the firmware error contract host-side. RA8_OFF_TARGET is the same
          # switch the host unit-test build uses so register-reaching code takes the
          # host-safe path.
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/third_party/miniz
          ${MDL_JOF_INC}
)

# _GNU_SOURCE: mdl_export.c uses posix_spawn_file_actions_addchdir_np() and
# `environ`, both of which glibc gates behind it. It must be set on the compile
# line, not in the source, so it precedes every system header.
target_compile_definitions(
  media_dl PRIVATE _GNU_SOURCE RA8_OFF_TARGET ${MDL_ADDCHDIR_DEF}
                   MDL_EPUB_COMPILE_DIR="${MDL_EPUB_COMPILE_DIR}"
)
target_compile_options(media_dl PRIVATE -Wall -Wextra -Werror)
target_link_libraries(media_dl PRIVATE CURL::libcurl)
ra8_webp_attach(media_dl ${CMAKE_CURRENT_SOURCE_DIR}/../..)

# Host unit tests (unity_minimal harness, like the repo's tests/). The pure
# logic under test here (extract/config/export/url_guard/sanitize/robots) needs
# no libcurl, so this binary links only those units + vendored miniz. The
# network-seam tests (which do need the libcurl backend) live in their own
# binary below so neither test file exceeds the size cap.
enable_testing()
add_executable(
  test_media_dl
  tests/test_media_dl.c
  src/mdl_atomic.c
  src/mdl_url_guard.c
  src/mdl_sanitize.c
  src/mdl_robots.c
  src/mdl_extract.c
  src/mdl_config.c
  src/mdl_politeness.c
  src/mdl_export.c
  src/mdl_export_jof.c
  ${MDL_JOF_SRC}
  ${MDL_MINIZ}
)
target_include_directories(
  test_media_dl
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/third_party/miniz
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
          ${MDL_JOF_INC}
)
target_compile_definitions(
  test_media_dl PRIVATE _GNU_SOURCE RA8_OFF_TARGET ${MDL_ADDCHDIR_DEF}
                        MDL_EPUB_COMPILE_DIR="${MDL_EPUB_COMPILE_DIR}"
)
target_compile_options(test_media_dl PRIVATE -Wall -Wextra -Werror)
ra8_webp_attach(test_media_dl ${CMAKE_CURRENT_SOURCE_DIR}/../..)
add_test(NAME test_media_dl COMMAND test_media_dl)

# #310 network-seam tests: a scripted fake ::mdl_net_iface_t backend drives the
# vtable dispatchers with no network, and the libcurl backend's pure transfer
# classifier + bounded write callback are exercised directly. This binary links
# the dispatchers, the politeness clock seam, the libcurl backend and its URL
# guards (so it needs libcurl); src/ is on the include path for the promoted
# helpers in mdl_net_curl_internal.h.
add_executable(
  test_media_dl_net
  tests/test_media_dl_net.c
  src/mdl_atomic.c
  src/mdl_net.c
  src/mdl_net_curl.c
  src/mdl_politeness.c
  src/mdl_url_guard.c
)
target_include_directories(
  test_media_dl_net
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
)
target_compile_definitions(test_media_dl_net PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_net PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_media_dl_net PRIVATE CURL::libcurl)
add_test(NAME test_media_dl_net COMMAND test_media_dl_net)

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
  src/mdl_session.c
  src/mdl_robots.c
  src/mdl_net.c
  src/mdl_politeness.c
  src/mdl_url_guard.c
)
target_include_directories(
  test_media_dl_session
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
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
  src/mdl_state.c
  src/mdl_hash.c
  src/mdl_urlname.c
  src/mdl_sanitize.c
)
target_include_directories(
  test_media_dl_state
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
)
target_compile_definitions(test_media_dl_state PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_state PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_media_dl_state COMMAND test_media_dl_state)

# #305 fetch-orchestrator tests: the resumable, deduping download loop driven
# end-to-end through the #310 mdl_net vtable mock with no network. A scripted
# fake backend writes deterministic page bytes into a temp directory, so the
# suite asserts that a first run fetches N pages, a second run fetches only the
# new ones, an interrupted run resumes to a byte-identical result, and a
# corrupt state file rebuilds cleanly. The fake replaces libcurl, so this binary
# links only the pure units above the seam and needs no CURL.
add_executable(
  test_media_dl_fetch
  tests/test_media_dl_fetch.c
  src/mdl_atomic.c
  src/mdl_fetch.c
  src/mdl_state.c
  src/mdl_hash.c
  src/mdl_urlname.c
  src/mdl_sanitize.c
  src/mdl_pathfs.c
  src/mdl_session.c
  src/mdl_robots.c
  src/mdl_net.c
  src/mdl_politeness.c
  src/mdl_url_guard.c
  src/mdl_extract.c
)
target_include_directories(
  test_media_dl_fetch
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc ${CMAKE_CURRENT_SOURCE_DIR}/src
          ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
)
target_compile_definitions(test_media_dl_fetch PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_fetch PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_media_dl_fetch COMMAND test_media_dl_fetch)

# #301 governor tests: the per-host politeness governor (token-bucket rate
# limiting, exponential backoff + jitter on 429/503, Retry-After honouring in
# both forms, and the per-host in-flight cap) driven entirely through the
# injectable clock/sleep seams -- no network, no real time. The governor lives
# in mdl_politeness.c and is self-contained, so this binary links only that unit.
add_executable(test_media_dl_governor tests/test_media_dl_governor.c src/mdl_politeness.c)
target_include_directories(
  test_media_dl_governor
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
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
add_executable(test_media_dl_search tests/test_media_dl_search.c src/mdl_search.c src/mdl_extract.c)
target_include_directories(
  test_media_dl_search
  PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/inc ${CMAKE_CURRENT_SOURCE_DIR}/../../libs/ra8_core/inc
          ${CMAKE_CURRENT_SOURCE_DIR}/../../tests
)
target_compile_definitions(test_media_dl_search PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_search PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_media_dl_search COMMAND test_media_dl_search)
