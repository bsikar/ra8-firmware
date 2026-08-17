# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie

# media_dl core -- the PORTABLE half of the comic/manga/manhwa/webtoon
# downloader: fetch, state, cache, export, verify, report and pack, written
# over the fw_if_fs filesystem interface and the injected ::mdl_net_iface_t
# vtable. This is the code the RA8 itself runs. It names no host library, no
# libcurl, and no POSIX call those interfaces do not already provide.
#
# It lives under apps/shared/ rather than inside a product because the SAME
# core is about to have two build FORMS:
#
#   apps/stand_alone/media_dl/     the host CLI (this repo builds it today)
#   apps/threadx_modules/          a loadable on-device module (placeholder)
#
# A core that lived inside the stand_alone product would have to be reached
# by ../.. from the second form, which is how a shared dependency becomes a
# copy. So the layering is a directory rule, and it is one-way:
#
#   apps/shared/ MUST NEVER include from apps/stand_alone/ or
#   apps/threadx_modules/ -- shared is BELOW the forms.
#
# grep for that rule: it is restated in the stand_alone listfile and enforced
# by the fact that nothing here names a form directory.
#
# This listfile is usable two ways. Standalone (`cmake -S apps/shared/media_dl`)
# it configures the core library and the core test suite on their own, which is
# what proves the core needs no form. Added by a form with add_subdirectory() it
# defines the same targets and hands the form the source-list and include-root
# variables it needs to compose its own binaries, through PARENT_SCOPE.
cmake_minimum_required(VERSION 3.20)
project(media_dl_core C)

set(CMAKE_C_STANDARD 23)
set(CMAKE_C_STANDARD_REQUIRED ON)

# The repository root, stated ONCE. This listfile reaches out of its own tree
# for the firmware libraries, the POSIX port, the shared test support and the
# vendored WebP recipe, and it is configured standalone (its own
# CMAKE_SOURCE_DIR) by the tools-build and tools-coverage gates, so
# CMAKE_SOURCE_DIR cannot serve. Every such reference goes through this
# variable: when the product moved from tools/media_dl to
# apps/stand_alone/media_dl, 59 hand-written ../.. prefixes all became wrong
# together and CMake reported only the first. A form sets it before adding this
# directory, so both halves resolve the tree the same way.
if(NOT DEFINED MDL_REPO_ROOT)
  set(MDL_REPO_ROOT ${CMAKE_CURRENT_SOURCE_DIR}/../../..)
endif()

# The core's own two roots, likewise stated once. Every source path below is
# absolute through them, because a form consumes these lists from ITS directory
# scope and a relative path would silently resolve against the form instead.
set(MDL_CORE_SRC_DIR ${CMAKE_CURRENT_SOURCE_DIR}/src)
set(MDL_CORE_INC ${CMAKE_CURRENT_SOURCE_DIR}/inc)
set(MDL_CORE_TESTS_DIR ${CMAKE_CURRENT_SOURCE_DIR}/tests)
set(MDL_CORE_SITES_DIR ${CMAKE_CURRENT_SOURCE_DIR}/sites)

# Keep host-tool coverage opt-in, matching tests/cmake/host_config.cmake. The
# flags are added after project() so CMake's compiler probe is not instrumented
# and gcovr never mistakes compiler-identification objects for project code.
# A form adds them at ITS directory scope, which every subdirectory inherits,
# so only a standalone configure of the core adds them here -- adding them
# twice would pass --coverage to the compiler twice.
option(RA8_COVERAGE "Emit gcov coverage data for media_dl and its tests" OFF)
if(RA8_COVERAGE AND CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  add_compile_options(
    --coverage
    -fprofile-arcs
    -ftest-coverage
    -O0
    -g
  )
  add_link_options(--coverage -fprofile-arcs -ftest-coverage)
endif()

# Vendored miniz (the same third-party ZIP/DEFLATE the firmware comic reader
# uses) provides CBZ writing and gzip DEFLATE -- so archive export needs no
# system libraries. SOUP: compiled warning-free, exempt from our style bar.
set(MDL_MINIZ ${MDL_REPO_ROOT}/libs/third_party/miniz/miniz.c)
set_source_files_properties(
  ${MDL_MINIZ} PROPERTIES COMPILE_OPTIONS "-w;-fno-strict-aliasing"
                          COMPILE_DEFINITIONS "MINIZ_NO_STDIO;MINIZ_NO_MALLOC"
)

# The JOF exporter reuses the firmware's own JOF producer + streaming JPEG
# decoder host-side (RA8_OFF_TARGET), so a .jof the core writes is
# byte-identical to one the RA8 produces. These are hand-written first-party
# files, so they carry the project warning bar (-Wall -Wextra -Werror) from the
# targets below like every other build that compiles them -- NOT the `-w` that
# SOUP gets. Compiling them quietly here hid host-only findings the ARM build
# never sees, which is what a 64-bit host build is for (#309).
set(FW ${MDL_REPO_ROOT}/libs)
set(MDL_PUBLIC_INC ${MDL_CORE_INC} ${FW}/ra8_mdl/inc)
set(MDL_FS_SRC
    ${FW}/if/src/fw_if_fs.c
    ${FW}/if/src/fw_if_fs_dir.c
    ${MDL_REPO_ROOT}/port/posix/src/fw_if_fs_posix.c
    ${MDL_REPO_ROOT}/port/posix/src/fw_if_fs_posix_bind.c
    ${MDL_REPO_ROOT}/port/posix/src/fw_if_fs_posix_common.c
    ${MDL_REPO_ROOT}/port/posix/src/fw_if_fs_posix_stream.c
)
set(MDL_FS_INC ${FW}/if/inc ${MDL_REPO_ROOT}/port/posix/inc ${MDL_REPO_ROOT}/port/posix/src)
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
set(MDL_STREAM_POSIX_SRC ${MDL_REPO_ROOT}/port/posix/src/ra8_io_stream_posix.c)
set(MDL_RABOOK_READER_SRC
    ${FW}/ra8_book/src/ra8_book.c ${FW}/ra8_book/src/ra8_book_stream.c
    ${FW}/ra8_book/src/ra8_book_chunked.c ${FW}/ra8_book/src/ra8_book_chunked_validate.c
)
set(MDL_RABOOK_READER_INC ${FW}/ra8_book/inc ${FW}/ra8_book/src ${FW}/ra8_mem/inc)

# WebP decode for the JOF exporter: the sites this downloader scrapes serve
# .webp pages (mdl_export's page-extension table has always accepted them), so
# the producer's WebP arm must be the real transcode. One call; the recipe is
# shared with the firmware app build and the host test build. CMake functions
# are global once defined, so a form calls ra8_webp_attach() without including
# the recipe a second time.
include(${MDL_REPO_ROOT}/cmake/ra8_webp_vendor.cmake)

# One responsibility per exporter translation unit. Both forms and every
# focused host-test binary compile the same complete writer set.
set(MDL_EXTRACT_SRC ${MDL_CORE_SRC_DIR}/mdl_extract.c ${MDL_CORE_SRC_DIR}/mdl_extract_hits.c)
set(MDL_CACHE_SRC ${MDL_CORE_SRC_DIR}/mdl_cache.c ${MDL_CORE_SRC_DIR}/mdl_cache_io.c)

set(MDL_EXPORT_SRC
    ${MDL_CORE_SRC_DIR}/mdl_export.c
    ${MDL_CORE_SRC_DIR}/mdl_export_workspace.c
    ${MDL_CORE_SRC_DIR}/mdl_export_io.c
    ${MDL_CORE_SRC_DIR}/mdl_export_meta.c
    ${MDL_CORE_SRC_DIR}/mdl_export_zip.c
    ${MDL_CORE_SRC_DIR}/mdl_export_tar.c
    ${MDL_CORE_SRC_DIR}/mdl_export_epub.c
    ${MDL_CORE_SRC_DIR}/mdl_export_epub_meta.c
    ${MDL_CORE_SRC_DIR}/mdl_export_jof.c
)
set(MDL_VERIFY_SRC ${MDL_CORE_SRC_DIR}/mdl_verify.c ${MDL_CORE_SRC_DIR}/mdl_verify_tarball.c)

# The strict reader and bounded EPUB -> RABOOK compiler are shared by both
# forms and every focused downloader test. Build them once: duplicating the
# firmware parser/raster stack into each test executable wastes link time and
# can let per-target compile definitions drift.
set(MDL_RABOOK_RUNTIME_SRC
    ${MDL_CORE_SRC_DIR}/mdl_export_rabook.c
    ${MDL_CORE_SRC_DIR}/mdl_export_rabook_io.c
    ${MDL_CORE_SRC_DIR}/mdl_verify_rabook.c
    ${MDL_RABOOK_READER_SRC}
    ${FW}/ra8_xml/src/ra8_xml.c
    ${FW}/ra8_xml/src/ra8_xml_decode.c
    ${FW}/ra8_xml/src/ra8_xml_doctype.c
    ${FW}/ra8_epub/src/ra8_epub_miniz_alloc.c
    ${FW}/ra8_epub/src/ra8_epub_open.c
    ${FW}/ra8_epub/src/ra8_epub_entry.c
    ${FW}/ra8_epub/src/ra8_epub_chapter.c
    ${FW}/ra8_epub/src/ra8_epub_xml_shim.c
    ${FW}/ra8_epub/src/ra8_epub_xml_toc.c
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
          ${MDL_CORE_SRC_DIR}
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

# ---------------------------------------------------------------------------
# The core, as one library target a form links instead of restating 40 source
# paths. It is an OBJECT library rather than a STATIC one on purpose: the
# tools-coverage ratchet measures gcov counters PER PRODUCTION FILE, and a
# static archive lets the linker drop any member the form happens not to
# reference -- that member's .gcda is then never written and its coverage
# silently vanishes from the report, which reads as a regression in a file
# nobody touched. An OBJECT library links every object unconditionally, so the
# object set a form gets is exactly the one it used to compile itself.
# ---------------------------------------------------------------------------
set(MDL_CORE_SRC
    ${MDL_CORE_SRC_DIR}/mdl_stream.c
    ${MDL_CORE_SRC_DIR}/mdl_net.c
    ${MDL_CORE_SRC_DIR}/mdl_url_guard.c
    ${MDL_CORE_SRC_DIR}/mdl_sanitize.c
    ${MDL_CORE_SRC_DIR}/mdl_pathfs.c
    ${MDL_CORE_SRC_DIR}/mdl_robots.c
    ${MDL_CORE_SRC_DIR}/mdl_session.c
    ${MDL_EXTRACT_SRC}
    ${MDL_CORE_SRC_DIR}/mdl_search.c
    ${MDL_CORE_SRC_DIR}/mdl_discover.c
    ${MDL_CORE_SRC_DIR}/mdl_politeness.c
    ${MDL_CORE_SRC_DIR}/mdl_config.c
    ${MDL_CORE_SRC_DIR}/mdl_hash.c
    ${MDL_CORE_SRC_DIR}/mdl_storage.c
    ${MDL_CACHE_SRC}
    ${MDL_CORE_SRC_DIR}/mdl_urlname.c
    ${MDL_CORE_SRC_DIR}/mdl_state.c
    ${MDL_CORE_SRC_DIR}/mdl_state_codec.c
    ${MDL_CORE_SRC_DIR}/mdl_state_decimal.c
    ${MDL_CORE_SRC_DIR}/mdl_state_store.c
    ${MDL_CORE_SRC_DIR}/mdl_fetch.c
    ${MDL_CORE_SRC_DIR}/mdl_fetch_body.c
    ${MDL_CORE_SRC_DIR}/mdl_fetch_page.c
    ${MDL_CORE_SRC_DIR}/mdl_report.c
    ${MDL_CORE_SRC_DIR}/mdl_pack.c
    ${MDL_CORE_SRC_DIR}/mdl_library.c
    ${MDL_EXPORT_SRC}
    ${MDL_VERIFY_SRC}
)
add_library(media_dl_core OBJECT ${MDL_CORE_SRC})
target_include_directories(
  media_dl_core
  PUBLIC ${MDL_PUBLIC_INC}
         ${MDL_CORE_SRC_DIR}
         # Reuse the firmware error contract host-side. RA8_OFF_TARGET is the
         # same switch the host unit-test build uses so register-reaching code
         # takes the host-safe path.
         ${MDL_REPO_ROOT}/libs/ra8_core/inc
         ${MDL_REPO_ROOT}/libs/third_party/miniz
         ${MDL_JOF_INC}
         ${MDL_RABOOK_READER_INC}
         ${MDL_FS_INC}
)
target_compile_definitions(media_dl_core PUBLIC _GNU_SOURCE MINIZ_NO_TIME RA8_OFF_TARGET)
target_compile_options(media_dl_core PRIVATE -Wall -Wextra -Werror)
target_link_libraries(media_dl_core PUBLIC mdl_rabook_runtime)

# The C6 backend is core production code -- it is the network backend the RA8
# itself will use -- but no host form composes it, so it is not part of
# MDL_CORE_SRC. Its own test target below builds it.
set(MDL_CORE_C6LINK_SRC ${MDL_CORE_SRC_DIR}/mdl_net_c6link.c)

# ---------------------------------------------------------------------------
# Core host unit tests (unity_minimal harness, like the repo's tests/). Every
# one of these runs with no network and no libcurl: that is the property that
# makes them CORE tests rather than form tests. Tests that qualify a form's
# composition (the argv grammar, the libcurl backend, host credentials, the
# end-to-end HTTP harness) live in that form's own tests/ directory.
# ---------------------------------------------------------------------------
enable_testing()

set(MDL_CORE_TEST_COMMON_SRC
    ${MDL_CORE_SRC_DIR}/mdl_url_guard.c
    ${MDL_CORE_SRC_DIR}/mdl_sanitize.c
    ${MDL_CORE_SRC_DIR}/mdl_urlname.c
    ${MDL_CORE_SRC_DIR}/mdl_robots.c
    ${MDL_EXTRACT_SRC}
    ${MDL_CORE_SRC_DIR}/mdl_config.c
    ${MDL_CORE_SRC_DIR}/mdl_politeness.c
    ${MDL_CORE_SRC_DIR}/mdl_hash.c
    ${MDL_CORE_SRC_DIR}/mdl_storage.c
    ${MDL_CORE_TESTS_DIR}/mdl_test_storage.c
    ${MDL_EXPORT_SRC}
    ${MDL_VERIFY_SRC}
    ${MDL_CORE_SRC_DIR}/mdl_stream.c
    ${MDL_CORE_SRC_DIR}/mdl_net.c
    ${MDL_JOF_SRC}
    ${MDL_FS_SRC}
    ${MDL_STREAM_CORE_SRC}
    ${MDL_STREAM_RAM_SRC}
    ${MDL_MINIZ}
)

# Add one focused unit-test executable over the complete downloader core.
function(mdl_add_core_test target source)
  add_executable(${target} ${source} ${MDL_CORE_TEST_COMMON_SRC})
  target_include_directories(
    ${target}
    PRIVATE ${MDL_PUBLIC_INC}
            ${MDL_CORE_SRC_DIR}
            ${MDL_REPO_ROOT}/libs/ra8_core/inc
            ${MDL_REPO_ROOT}/libs/third_party/miniz
            ${MDL_REPO_ROOT}/tests
            ${MDL_JOF_INC}
            ${MDL_RABOOK_READER_INC}
            ${MDL_FS_INC}
  )
  target_compile_definitions(${target} PRIVATE _GNU_SOURCE MINIZ_NO_TIME RA8_OFF_TARGET)
  target_compile_options(${target} PRIVATE -Wall -Wextra -Werror)
  target_link_libraries(${target} PRIVATE mdl_rabook_runtime)
  ra8_webp_attach(${target} ${MDL_REPO_ROOT})
  add_test(NAME ${target} COMMAND ${target})
endfunction()

mdl_add_core_test(test_media_dl ${MDL_CORE_TESTS_DIR}/test_media_dl.c)
mdl_add_core_test(test_media_dl_export ${MDL_CORE_TESTS_DIR}/test_media_dl_export.c)
target_sources(
  test_media_dl_export PRIVATE ${MDL_CORE_TESTS_DIR}/test_media_dl_export_workspace.c
                               ${MDL_CORE_TESTS_DIR}/test_media_dl_export_xml.c
)
add_executable(
  test_media_dl_rabook_verify
  ${MDL_CORE_TESTS_DIR}/test_media_dl_rabook_verify.c ${MDL_CORE_TEST_COMMON_SRC}
  ${MDL_REPO_ROOT}/tests/support/rabook_compile_test_fixture.c
)
target_include_directories(
  test_media_dl_rabook_verify
  PRIVATE ${MDL_PUBLIC_INC}
          ${MDL_CORE_SRC_DIR}
          ${MDL_REPO_ROOT}/libs/ra8_core/inc
          ${MDL_REPO_ROOT}/libs/third_party/miniz
          ${FW}/ra8_rabook_compile/inc
          ${MDL_REPO_ROOT}/tests
          ${MDL_JOF_INC}
          ${MDL_RABOOK_READER_INC}
          ${MDL_FS_INC}
)
target_compile_definitions(
  test_media_dl_rabook_verify PRIVATE _GNU_SOURCE MINIZ_NO_TIME RA8_OFF_TARGET
)
target_compile_options(test_media_dl_rabook_verify PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_media_dl_rabook_verify PRIVATE mdl_rabook_runtime)
ra8_webp_attach(test_media_dl_rabook_verify ${MDL_REPO_ROOT})
add_test(NAME test_media_dl_rabook_verify COMMAND test_media_dl_rabook_verify)
add_executable(
  test_media_dl_verify_stream ${MDL_CORE_TESTS_DIR}/test_media_dl_verify_stream.c
                              ${MDL_CORE_TEST_COMMON_SRC}
)
target_include_directories(
  test_media_dl_verify_stream
  PRIVATE ${MDL_PUBLIC_INC}
          ${MDL_CORE_SRC_DIR}
          ${MDL_REPO_ROOT}/libs/ra8_core/inc
          ${MDL_REPO_ROOT}/libs/third_party/miniz
          ${FW}/ra8_rabook_compile/inc
          ${MDL_REPO_ROOT}/tests
          ${MDL_JOF_INC}
          ${MDL_RABOOK_READER_INC}
          ${MDL_FS_INC}
)
target_compile_definitions(
  test_media_dl_verify_stream PRIVATE _GNU_SOURCE MINIZ_NO_TIME RA8_OFF_TARGET
)
target_compile_options(test_media_dl_verify_stream PRIVATE -Wall -Wextra -Werror)
target_link_libraries(test_media_dl_verify_stream PRIVATE mdl_rabook_runtime)
ra8_webp_attach(test_media_dl_verify_stream ${MDL_REPO_ROOT})
add_test(NAME test_media_dl_verify_stream COMMAND test_media_dl_verify_stream)
mdl_add_core_test(test_media_dl_export_fault ${MDL_CORE_TESTS_DIR}/test_media_dl_export_fault.c)
target_sources(
  test_media_dl_export_fault PRIVATE ${MDL_REPO_ROOT}/tests/support/mdl_state_fs_fault.c
)
mdl_add_core_test(test_media_dl_metadata ${MDL_CORE_TESTS_DIR}/test_media_dl_metadata.c)

# Progress reporting and chapter packing. Both units were linked ONLY into the
# media_dl executable, so their only coverage came from the end-to-end HTTP
# integration script -- which is why they sat near a third of their lines. They
# are pure over an injected ra8_io_stream_t sink and a storage facade, so they
# unit-test cleanly; this target adds them plus the transitive closure that
# mdl_fetch.c drags in (the same set test_media_dl_fetch links), on top of what
# mdl_add_core_test already supplies.
mdl_add_core_test(test_media_dl_report_pack ${MDL_CORE_TESTS_DIR}/test_media_dl_report_pack.c)
target_sources(
  test_media_dl_report_pack
  PRIVATE ${MDL_CORE_SRC_DIR}/mdl_report.c
          ${MDL_CORE_SRC_DIR}/mdl_pack.c
          ${MDL_CORE_SRC_DIR}/mdl_fetch.c
          ${MDL_CORE_SRC_DIR}/mdl_fetch_body.c
          ${MDL_CORE_SRC_DIR}/mdl_fetch_page.c
          ${MDL_CACHE_SRC}
          ${MDL_CORE_SRC_DIR}/mdl_state.c
          ${MDL_CORE_SRC_DIR}/mdl_state_codec.c
          ${MDL_CORE_SRC_DIR}/mdl_state_decimal.c
          ${MDL_CORE_SRC_DIR}/mdl_state_store.c
          ${MDL_CORE_SRC_DIR}/mdl_session.c
          ${MDL_CORE_SRC_DIR}/mdl_pathfs.c
)

# The C6 network backend is production core code but is not part of any host
# form's composition. Exercise it through a deterministic stub of the
# already-independent transfer coordinator so the tool coverage report measures
# the adapter without duplicating the full protobuf model here.
add_executable(
  test_media_dl_net_c6link ${MDL_CORE_TESTS_DIR}/test_media_dl_net_c6link.c
                           ${MDL_CORE_SRC_DIR}/mdl_net.c ${MDL_CORE_C6LINK_SRC}
)
target_include_directories(
  test_media_dl_net_c6link
  PRIVATE ${MDL_PUBLIC_INC}
          ${MDL_CORE_SRC_DIR}
          ${FW}/ra8_core/inc
          ${FW}/ra8_mdl/inc
          ${FW}/ra8_c6link/inc
          ${MDL_REPO_ROOT}/tests
)
target_compile_definitions(test_media_dl_net_c6link PRIVATE RA8_OFF_TARGET)
target_compile_options(test_media_dl_net_c6link PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_media_dl_net_c6link COMMAND test_media_dl_net_c6link)

# #302 session-level tests: the honest User-Agent + robots.txt gating driven
# end-to-end through the #310 mdl_net vtable mock. A scripted fake backend
# serves robots.txt and records the transmitted User-Agent, proving the honest
# UA reaches the wire, a Disallow refuses a URL before any content fetch, and a
# Crawl-delay raises the per-host politeness floor -- all with no network. The
# fake backend replaces the form's transport, so this binary links only the
# pure units above the seam and needs no CURL.
add_executable(
  test_media_dl_session
  ${MDL_CORE_TESTS_DIR}/test_media_dl_session.c
  ${MDL_CORE_SRC_DIR}/mdl_stream.c
  ${MDL_CORE_SRC_DIR}/mdl_session.c
  ${MDL_CORE_SRC_DIR}/mdl_robots.c
  ${MDL_CORE_SRC_DIR}/mdl_net.c
  ${MDL_CORE_SRC_DIR}/mdl_politeness.c
  ${MDL_CORE_SRC_DIR}/mdl_url_guard.c
  ${MDL_STREAM_CORE_SRC}
  ${MDL_STREAM_RAM_SRC}
  ${MDL_STREAM_POSIX_SRC}
  ${FW}/ra8_core/src/ra8_log.c
  ${FW}/ra8_core/src/ra8_error_handler.c
)
target_include_directories(
  test_media_dl_session
  PRIVATE ${MDL_PUBLIC_INC}
          ${MDL_CORE_SRC_DIR}
          ${MDL_REPO_ROOT}/libs/ra8_core/inc
          ${MDL_REPO_ROOT}/libs/ra8_io/inc
          ${MDL_REPO_ROOT}/port/posix/inc
          ${MDL_REPO_ROOT}/tests
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
  ${MDL_CORE_TESTS_DIR}/test_media_dl_state.c
  ${MDL_CORE_TESTS_DIR}/test_media_dl_state_metadata.c
  ${MDL_CORE_SRC_DIR}/mdl_state.c
  ${MDL_CORE_SRC_DIR}/mdl_state_codec.c
  ${MDL_CORE_SRC_DIR}/mdl_state_decimal.c
  ${MDL_CORE_SRC_DIR}/mdl_state_store.c
  ${MDL_CORE_SRC_DIR}/mdl_hash.c
  ${MDL_CORE_SRC_DIR}/mdl_storage.c
  ${MDL_CORE_SRC_DIR}/mdl_urlname.c
  ${MDL_CORE_SRC_DIR}/mdl_sanitize.c
  ${MDL_FS_SRC}
)
target_include_directories(
  test_media_dl_state PRIVATE ${MDL_PUBLIC_INC} ${MDL_REPO_ROOT}/libs/ra8_core/inc
                              ${MDL_REPO_ROOT}/tests ${MDL_FS_INC}
)
target_compile_definitions(test_media_dl_state PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_state PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_media_dl_state COMMAND test_media_dl_state)

# #305 fetch-orchestrator tests: the resumable, deduping download loop driven
# end-to-end through the #310 mdl_net vtable mock with no network. A scripted
# fake backend writes deterministic page bytes into a temp directory, so the
# suite asserts that a first run fetches N pages, a second run fetches only the
# new ones, an interrupted run resumes to a byte-identical result, and a corrupt
# state file rebuilds cleanly. The fake replaces the form's transport, so this
# binary links only the pure units above the seam and needs no CURL.
add_executable(
  test_media_dl_fetch
  ${MDL_CORE_TESTS_DIR}/test_media_dl_fetch.c
  ${MDL_CORE_SRC_DIR}/mdl_stream.c
  ${MDL_CORE_SRC_DIR}/mdl_fetch.c
  ${MDL_CORE_SRC_DIR}/mdl_fetch_body.c
  ${MDL_CORE_SRC_DIR}/mdl_fetch_page.c
  ${MDL_CACHE_SRC}
  ${MDL_CORE_SRC_DIR}/mdl_state.c
  ${MDL_CORE_SRC_DIR}/mdl_state_codec.c
  ${MDL_CORE_SRC_DIR}/mdl_state_decimal.c
  ${MDL_CORE_SRC_DIR}/mdl_state_store.c
  ${MDL_CORE_SRC_DIR}/mdl_hash.c
  ${MDL_CORE_SRC_DIR}/mdl_storage.c
  ${MDL_CORE_SRC_DIR}/mdl_urlname.c
  ${MDL_CORE_SRC_DIR}/mdl_sanitize.c
  ${MDL_CORE_SRC_DIR}/mdl_pathfs.c
  ${MDL_CORE_SRC_DIR}/mdl_session.c
  ${MDL_CORE_SRC_DIR}/mdl_robots.c
  ${MDL_CORE_SRC_DIR}/mdl_net.c
  ${MDL_CORE_SRC_DIR}/mdl_politeness.c
  ${MDL_CORE_SRC_DIR}/mdl_url_guard.c
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
          ${MDL_CORE_SRC_DIR}
          ${MDL_REPO_ROOT}/libs/ra8_core/inc
          ${MDL_REPO_ROOT}/libs/ra8_io/inc
          ${MDL_REPO_ROOT}/tests
          ${MDL_FS_INC}
)
target_compile_definitions(test_media_dl_fetch PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_fetch PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_media_dl_fetch COMMAND test_media_dl_fetch)

add_executable(
  test_media_dl_fetch_cache
  ${MDL_CORE_TESTS_DIR}/test_media_dl_fetch_cache.c
  ${MDL_CORE_SRC_DIR}/mdl_stream.c
  ${MDL_CORE_SRC_DIR}/mdl_fetch.c
  ${MDL_CORE_SRC_DIR}/mdl_fetch_body.c
  ${MDL_CORE_SRC_DIR}/mdl_fetch_page.c
  ${MDL_CACHE_SRC}
  ${MDL_CORE_SRC_DIR}/mdl_state.c
  ${MDL_CORE_SRC_DIR}/mdl_state_codec.c
  ${MDL_CORE_SRC_DIR}/mdl_state_decimal.c
  ${MDL_CORE_SRC_DIR}/mdl_state_store.c
  ${MDL_CORE_SRC_DIR}/mdl_hash.c
  ${MDL_CORE_SRC_DIR}/mdl_storage.c
  ${MDL_CORE_SRC_DIR}/mdl_urlname.c
  ${MDL_CORE_SRC_DIR}/mdl_sanitize.c
  ${MDL_CORE_SRC_DIR}/mdl_pathfs.c
  ${MDL_CORE_SRC_DIR}/mdl_session.c
  ${MDL_CORE_SRC_DIR}/mdl_robots.c
  ${MDL_CORE_SRC_DIR}/mdl_net.c
  ${MDL_CORE_SRC_DIR}/mdl_politeness.c
  ${MDL_CORE_SRC_DIR}/mdl_url_guard.c
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
          ${MDL_CORE_SRC_DIR}
          ${MDL_REPO_ROOT}/libs/ra8_core/inc
          ${MDL_REPO_ROOT}/libs/ra8_io/inc
          ${MDL_REPO_ROOT}/tests
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
  ${MDL_CORE_TESTS_DIR}/test_media_dl_cache.c
  ${MDL_CORE_TESTS_DIR}/mdl_test_storage.c
  ${MDL_CACHE_SRC}
  ${MDL_CORE_SRC_DIR}/mdl_hash.c
  ${MDL_CORE_SRC_DIR}/mdl_storage.c
  ${MDL_CORE_SRC_DIR}/mdl_sanitize.c
  ${MDL_CORE_SRC_DIR}/mdl_url_guard.c
  ${MDL_FS_SRC}
)
target_include_directories(
  test_media_dl_cache
  PRIVATE ${MDL_PUBLIC_INC}
          ${MDL_CORE_SRC_DIR}
          ${MDL_CORE_TESTS_DIR}
          ${MDL_REPO_ROOT}/libs/ra8_core/inc
          ${MDL_REPO_ROOT}/libs/ra8_io/inc
          ${MDL_REPO_ROOT}/tests
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
  ${MDL_CORE_TESTS_DIR}/test_media_dl_fetch_body.c
  ${MDL_CORE_SRC_DIR}/mdl_fetch_body.c
  ${MDL_CORE_SRC_DIR}/mdl_storage.c
  ${MDL_CORE_SRC_DIR}/mdl_hash.c
  ${MDL_CORE_SRC_DIR}/mdl_urlname.c
  ${MDL_CORE_SRC_DIR}/mdl_sanitize.c
  ${MDL_REPO_ROOT}/tests/support/mdl_state_fs_fault.c
  ${MDL_FS_SRC}
)
target_include_directories(
  test_media_dl_fetch_body
  PRIVATE ${MDL_PUBLIC_INC}
          ${MDL_CORE_SRC_DIR}
          ${MDL_REPO_ROOT}/libs/ra8_core/inc
          ${MDL_REPO_ROOT}/tests
          ${MDL_REPO_ROOT}/tests/support
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
add_executable(
  test_media_dl_governor ${MDL_CORE_TESTS_DIR}/test_media_dl_governor.c
                         ${MDL_CORE_SRC_DIR}/mdl_politeness.c
)
target_include_directories(
  test_media_dl_governor PRIVATE ${MDL_PUBLIC_INC} ${MDL_REPO_ROOT}/libs/ra8_core/inc
                                 ${MDL_REPO_ROOT}/tests
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
  test_media_dl_search ${MDL_CORE_TESTS_DIR}/test_media_dl_search.c
                       ${MDL_CORE_SRC_DIR}/mdl_search.c ${MDL_EXTRACT_SRC}
)
target_include_directories(
  test_media_dl_search PRIVATE ${MDL_PUBLIC_INC} ${MDL_REPO_ROOT}/libs/ra8_core/inc
                               ${MDL_REPO_ROOT}/tests
)
target_compile_definitions(test_media_dl_search PRIVATE _GNU_SOURCE RA8_OFF_TARGET)
target_compile_options(test_media_dl_search PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_media_dl_search COMMAND test_media_dl_search)

# Every checked-in site descriptor is exercised against a sanitised captured
# discovery, series, and chapter fixture. Adding a descriptor requires adding
# its fixture vectors here rather than making an untested support claim. The
# descriptors are data OF THE CORE -- both build forms ship them -- so they sit
# beside it, not inside a form.
file(GLOB MDL_SHIPPED_SITE_CONFIGS CONFIGURE_DEPENDS "${MDL_CORE_SITES_DIR}/*.conf")
list(LENGTH MDL_SHIPPED_SITE_CONFIGS MDL_SHIPPED_SITE_COUNT)
add_executable(
  test_media_dl_sites
  ${MDL_CORE_TESTS_DIR}/test_media_dl_sites.c
  ${MDL_CORE_SRC_DIR}/mdl_config.c
  ${MDL_EXTRACT_SRC}
  ${MDL_CORE_SRC_DIR}/mdl_search.c
  ${MDL_CORE_SRC_DIR}/mdl_urlname.c
  ${MDL_CORE_SRC_DIR}/mdl_sanitize.c
  ${MDL_CORE_SRC_DIR}/mdl_politeness.c
  ${MDL_CORE_SRC_DIR}/mdl_hash.c
  ${MDL_CORE_SRC_DIR}/mdl_storage.c
  ${MDL_CORE_TESTS_DIR}/mdl_test_storage.c
  ${MDL_FS_SRC}
)
target_include_directories(
  test_media_dl_sites
  PRIVATE ${MDL_PUBLIC_INC}
          ${MDL_CORE_TESTS_DIR}
          ${MDL_REPO_ROOT}/libs/ra8_core/inc
          ${MDL_REPO_ROOT}/tests
          ${MDL_FS_INC}
)
target_compile_definitions(
  test_media_dl_sites
  PRIVATE _GNU_SOURCE
          RA8_OFF_TARGET
          MDL_SITE_CONFIG_DIR="${MDL_CORE_SITES_DIR}"
          MDL_SITE_FIXTURE_DIR="${MDL_CORE_TESTS_DIR}/fixtures/sites"
          MDL_SHIPPED_SITE_COUNT=${MDL_SHIPPED_SITE_COUNT}
)
target_compile_options(test_media_dl_sites PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_media_dl_sites COMMAND test_media_dl_sites)

# ---------------------------------------------------------------------------
# What a build form needs to compose its own binaries. Published deliberately
# and once, so a form never re-derives a path the core already owns -- the same
# reason MDL_REPO_ROOT exists.
# ---------------------------------------------------------------------------
if(NOT CMAKE_CURRENT_SOURCE_DIR STREQUAL CMAKE_SOURCE_DIR)
  set(MDL_CORE_SRC_DIR
      ${MDL_CORE_SRC_DIR}
      PARENT_SCOPE
  )
  set(MDL_CORE_INC
      ${MDL_CORE_INC}
      PARENT_SCOPE
  )
  set(MDL_CORE_TESTS_DIR
      ${MDL_CORE_TESTS_DIR}
      PARENT_SCOPE
  )
  set(MDL_CORE_SITES_DIR
      ${MDL_CORE_SITES_DIR}
      PARENT_SCOPE
  )
  set(MDL_PUBLIC_INC
      ${MDL_PUBLIC_INC}
      PARENT_SCOPE
  )
  set(FW
      ${FW}
      PARENT_SCOPE
  )
  set(MDL_MINIZ
      ${MDL_MINIZ}
      PARENT_SCOPE
  )
  set(MDL_FS_SRC
      ${MDL_FS_SRC}
      PARENT_SCOPE
  )
  set(MDL_FS_INC
      ${MDL_FS_INC}
      PARENT_SCOPE
  )
  set(MDL_JOF_SRC
      ${MDL_JOF_SRC}
      PARENT_SCOPE
  )
  set(MDL_JOF_INC
      ${MDL_JOF_INC}
      PARENT_SCOPE
  )
  set(MDL_STREAM_CORE_SRC
      ${MDL_STREAM_CORE_SRC}
      PARENT_SCOPE
  )
  set(MDL_STREAM_RAM_SRC
      ${MDL_STREAM_RAM_SRC}
      PARENT_SCOPE
  )
  set(MDL_STREAM_POSIX_SRC
      ${MDL_STREAM_POSIX_SRC}
      PARENT_SCOPE
  )
  set(MDL_RABOOK_READER_INC
      ${MDL_RABOOK_READER_INC}
      PARENT_SCOPE
  )
endif()
