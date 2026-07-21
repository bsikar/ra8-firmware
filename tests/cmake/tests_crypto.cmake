# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# Tests that must be built against the REAL crypto backend.
#
# The rest of the host build compiles under RA8_SIMULATOR_MODE, where the
# crypto paths are deterministic stand-ins. These targets recompile the same
# sources without that define, which is the only way the production branch is
# ever executed on the host -- so they are exactly the tests that would go
# silently unbuilt if this fragment stopped being included.
#
# Included from tests/CMakeLists.txt. CMake include() is textual within the
# same directory scope, so every variable and target defined here is visible
# to the driver and to the fragments included after it.

# ---------------------------------------------------------------------------
# test_stub_crypto_gate (issue #180): reachability half of the stub-vs-production
# crypto gate. The auto-glob above already registered it (it links ra8_core_hal,
# built under RA8_SIMULATOR_MODE, so the guarded #if branch is active). Define
# RA8_INSECURE_STUB_CRYPTO on it too, so it faithfully compiles under the opt-in
# flag the gate governs -- both flags select the same insecure #if branch, and
# the test asserts those stub bodies are reachable-but-insecure (a fail-closed
# #else would return k_ra8_err_not_supported and fail every case).
# ---------------------------------------------------------------------------
if(TARGET test_stub_crypto_gate)
  target_compile_definitions(test_stub_crypto_gate PRIVATE RA8_INSECURE_STUB_CRYPTO)
endif()

# ---------------------------------------------------------------------------
# test_ra8_rsip_devsec_failclosed (issue #216): production fail-closed reachability
# for the RSIP device-security path (lifecycle / debug / tamper / DPA arm). The
# rest of the host build compiles ra8_rsip_devsec.c under RA8_SIMULATOR_MODE (the
# guarded #if branch, exercised by test_life / test_debug_level / test_tamper in
# test_ra8_rsip_devsec.c). This self-contained target (no ra8_core_hal, no sim) rebuilds
# JUST that TU with the two guard flags UNDEFINED so the production #else is the
# compiled body, and asserts every entry point returns k_ra8_err_not_supported and
# writes no fabricated state. -U wins over the directory-level -D because CMake
# emits DEFINES before FLAGS on the compile line. -DRA8_LOG_LEVEL=0 makes the
# RA8_CHECK_NULL_PTR log a no-op so the TU needs no ra8_log backend to link.
# ---------------------------------------------------------------------------
add_executable(
  test_ra8_rsip_devsec_failclosed ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_rsip_devsec_failclosed.c
                                  ${FW_ROOT}/libs/ra8_hal/src/ra8_rsip_devsec.c
)
target_include_directories(
  test_ra8_rsip_devsec_failclosed PRIVATE ${CMAKE_CURRENT_SOURCE_DIR} ${FW_ROOT}/libs/ra8_core/inc
                                          ${FW_ROOT}/libs/ra8_hal/inc
)
target_compile_options(
  test_ra8_rsip_devsec_failclosed
  PRIVATE -Wall
          -Wextra
          -Werror
          -Wno-unused-function
          -Wno-unused-parameter
          -Wno-unused-variable
          -URA8_SIMULATOR_MODE
          -URA8_INSECURE_STUB_CRYPTO
          -DRA8_LOG_LEVEL=0
)
add_test(NAME test_ra8_rsip_devsec_failclosed COMMAND test_ra8_rsip_devsec_failclosed)

# ---------------------------------------------------------------------------
# T5-04: real-backend crypto known-answer test. The rest of the host build
# compiles ra8_psa_crypto.c under RA8_SIMULATOR_MODE (a deterministic AES-GCM
# stand-in + an HMAC-tautology ECDSA "verify"), so the AEAD / signature paths
# are never checked against a real cipher. This target links the vendored
# TF-PSA-Crypto library directly -- the exact psa_* primitives the production
# !RA8_SIMULATOR_MODE path calls -- and pins them to NIST/RFC vectors. It is a
# self-contained executable (no ra8_core_hal, no sim). TF-PSA-Crypto is SOUP:
# compile it -w -fno-strict-aliasing like the other vendored trees; it is
# already excluded from the coverage filter (libs/third_party/).
set(_RA8_TFPSA_KAT_DIR ${FW_ROOT}/libs/third_party/tf-psa-crypto)
file(
  GLOB
  _RA8_TFPSA_KAT_SRC
  CONFIGURE_DEPENDS
  ${_RA8_TFPSA_KAT_DIR}/core/*.c
  ${_RA8_TFPSA_KAT_DIR}/drivers/builtin/src/*.c
  ${_RA8_TFPSA_KAT_DIR}/platform/*.c
  ${_RA8_TFPSA_KAT_DIR}/utilities/*.c
  ${_RA8_TFPSA_KAT_DIR}/extras/*.c
)
set(_RA8_TFPSA_KAT_INC
    ${_RA8_TFPSA_KAT_DIR}/include
    ${_RA8_TFPSA_KAT_DIR}/drivers/builtin/include
    ${_RA8_TFPSA_KAT_DIR}/core
    ${_RA8_TFPSA_KAT_DIR}/dispatch
    ${_RA8_TFPSA_KAT_DIR}/drivers/builtin/src
    ${_RA8_TFPSA_KAT_DIR}/platform
    ${_RA8_TFPSA_KAT_DIR}/utilities
    ${_RA8_TFPSA_KAT_DIR}/extras
)
add_library(ra8_tfpsa_host_kat OBJECT ${_RA8_TFPSA_KAT_SRC})
target_include_directories(ra8_tfpsa_host_kat PRIVATE ${_RA8_TFPSA_KAT_INC})
target_compile_options(ra8_tfpsa_host_kat PRIVATE -w -fno-strict-aliasing)
add_executable(
  test_psa_real_kat ${CMAKE_CURRENT_SOURCE_DIR}/test_psa_real_kat.c
                    $<TARGET_OBJECTS:ra8_tfpsa_host_kat>
)
target_include_directories(test_psa_real_kat PRIVATE ${_RA8_TFPSA_KAT_INC})
target_compile_options(test_psa_real_kat PRIVATE -Wall -Wextra -Werror)
add_test(NAME test_psa_real_kat COMMAND test_psa_real_kat)

if(RA8_REFLOW_USE_LITEHTML)
  add_executable(
    test_ra8_reflow_v2 ${CMAKE_CURRENT_SOURCE_DIR}/test_ra8_reflow_v2.cpp
                       $<TARGET_OBJECTS:ra8_core_hal>
  )
  set_target_properties(test_ra8_reflow_v2 PROPERTIES LINKER_LANGUAGE CXX)
  target_compile_options(
    test_ra8_reflow_v2
    PRIVATE -Wall
            -Wextra
            -Wno-unused-function
            -Wno-unused-parameter
            -Wno-unused-variable
            -Wno-address-of-packed-member
  )
  target_compile_features(test_ra8_reflow_v2 PRIVATE cxx_std_17)
  target_include_directories(
    test_ra8_reflow_v2
    PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}
            ${FW_ROOT}/libs/ra8_core/inc
            ${FW_ROOT}/libs/ra8_reflow/inc
            ${FW_ROOT}/libs/third_party/litehtml/include
            ${FW_ROOT}/libs/third_party/stb
            ${CMAKE_CURRENT_SOURCE_DIR}/mocks
  )
  target_link_libraries(test_ra8_reflow_v2 PRIVATE litehtml gumbo)
  #  if(APPLE)
  #    target_link_options(test_ra8_reflow_v2 PRIVATE
  #      "-Wl,-pagezero_size,0x4000"
  #      "-Wl,-segalign,0x4000"
  #    )
  #  endif()
  add_test(NAME test_ra8_reflow_v2 COMMAND test_ra8_reflow_v2)
endif()
