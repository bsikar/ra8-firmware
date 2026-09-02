# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# XML consumer tests (EPUB and RABOOK).
#
# Three black-box targets drive the public C-linkage entry points.
#
# Included from tests/CMakeLists.txt. CMake include() is textual within the
# same directory scope, so every variable and target defined here is visible
# to the driver and to the fragments included after it.

# ---------------------------------------------------------------------------
# C++ harness for the C EPUB XML consumer compiled into ra8_core_hal.
# ---------------------------------------------------------------------------
add_executable(
  test_epub_xml_shim ${FW_ROOT}/apps/shared_libs/epub/tests/src/test_epub_xml_shim.cpp
                     $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_epub_xml_shim PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_epub_xml_shim PRIVATE -Wall -Wextra)
target_compile_features(test_epub_xml_shim PRIVATE cxx_std_17)
target_include_directories(
  test_epub_xml_shim
  PRIVATE ${RA8_TEST_SHARED_INCLUDE_DIRS}
          ${FW_ROOT}/libs/ra8_core/inc
          ${FW_ROOT}/apps/shared_libs/xml/inc
          ${FW_ROOT}/apps/shared_libs/epub/inc
          # epub_xml_shim_internal.h declares the shim's four extern "C" entry
          # points and their result struct; this test consumes that contract instead
          # of restating it, so the library's src/ has to be reachable.
          ${FW_ROOT}/apps/shared_libs/epub/src
          ${FW_ROOT}/apps/shared_libs/third_party/stb
          ${CMAKE_CURRENT_SOURCE_DIR}/mocks/inc
)
if(REFLOW_USE_LITEHTML)
  target_link_libraries(test_epub_xml_shim PRIVATE litehtml gumbo)
endif()
add_test(NAME test_epub_xml_shim COMMAND test_epub_xml_shim)

# ---------------------------------------------------------------------------
# Complementary line-coverage target for epub_xml_shim. Unlike the _cov
# sibling, this test is BLACK-BOX: it links the real (un-renamed) production
# entry points from ra8_core_hal and drives their length / parse / root /
# rootfile-resolution / spine-skip / cap / cover-meta / deep-recurse / nav-skip
# legs with fixtures. Calling the un-renamed functions lands the entry-point
# body lines in the shared production object so gcovr merges them into the
# aggregate (a renamed white-box copy would attribute them to a distinct
# function and drop the union). No production source is included by the test.
# ---------------------------------------------------------------------------
add_executable(
  test_epub_xml_shim_cov2 ${FW_ROOT}/apps/shared_libs/epub/tests/src/test_epub_xml_shim_cov2.cpp
                          $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_epub_xml_shim_cov2 PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_epub_xml_shim_cov2 PRIVATE -Wall -Wextra)
target_compile_features(test_epub_xml_shim_cov2 PRIVATE cxx_std_17)
target_include_directories(
  test_epub_xml_shim_cov2
  PRIVATE ${RA8_TEST_SHARED_INCLUDE_DIRS}
          ${FW_ROOT}/libs/ra8_core/inc
          ${FW_ROOT}/apps/shared_libs/xml/inc
          ${FW_ROOT}/apps/shared_libs/epub/inc
          # See test_epub_xml_shim: the shim's contract lives in
          # epub_xml_shim_internal.h under the library's src/.
          ${FW_ROOT}/apps/shared_libs/epub/src
          ${CMAKE_CURRENT_SOURCE_DIR}/mocks/inc
)
if(REFLOW_USE_LITEHTML)
  target_link_libraries(test_epub_xml_shim_cov2 PRIVATE litehtml gumbo)
endif()
add_test(NAME test_epub_xml_shim_cov2 COMMAND test_epub_xml_shim_cov2)

# ---------------------------------------------------------------------------
# C++ test target for ra8_rabook_xml_shim. Drives the public C-linkage entry
# point ra8_rabook_xml_parse_chapter() to verify DOM pre-order and MC/DC
# vectors for the iterative DFS (#149 stage-a XHTML parser).
# ---------------------------------------------------------------------------
add_executable(
  test_ra8_rabook_xml_shim
  ${FW_ROOT}/apps/shared_libs/xml/tests/src/test_ra8_rabook_xml_shim.cpp
  ${FW_ROOT}/apps/shared_libs/xml/tests/src/test_ra8_rabook_xml_shim_limits.cpp
  $<TARGET_OBJECTS:ra8_core_hal>
)
set_target_properties(test_ra8_rabook_xml_shim PROPERTIES LINKER_LANGUAGE CXX)
target_compile_options(test_ra8_rabook_xml_shim PRIVATE -Wall -Wextra)
target_compile_features(test_ra8_rabook_xml_shim PRIVATE cxx_std_17)
target_include_directories(
  test_ra8_rabook_xml_shim
  PRIVATE ${RA8_TEST_SHARED_INCLUDE_DIRS}
          ${FW_ROOT}/libs/ra8_core/inc
          ${FW_ROOT}/apps/shared_libs/xml/inc
          ${FW_ROOT}/apps/shared_libs/xml/tests/inc
          ${FW_ROOT}/apps/shared_libs/book/inc
          ${FW_ROOT}/apps/shared_libs/rabook_compile/inc
          ${FW_ROOT}/apps/shared_libs/rabook_compile/src
          ${CMAKE_CURRENT_SOURCE_DIR}/mocks/inc
)
if(REFLOW_USE_LITEHTML)
  target_link_libraries(test_ra8_rabook_xml_shim PRIVATE litehtml gumbo)
endif()
add_test(NAME test_ra8_rabook_xml_shim COMMAND test_ra8_rabook_xml_shim)
