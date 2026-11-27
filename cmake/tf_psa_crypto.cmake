# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# cmake/tf_psa_crypto.cmake
#
# The one place that knows how the vendored TF-PSA-Crypto tree is compiled.
#
# TF-PSA-Crypto is SOUP, pinned in build.zig.zon (package `tf_psa_crypto`) and
# resolved through cmake/zig_package.cmake. Before RA8FW-283 seven recipes
# re-globbed the same source set and re-typed the same include list: five
# secure-boot / HIL apps, the host known-answer tests and the combined Mbed TLS
# recipe. They had already drifted (dfu_bootloader alone added p256-m), and a
# change to the source set had to be made seven times. Every one of them now
# calls into this file.
#
# Two entry points:
#
#   ra8_tf_psa_crypto_paths(<prefix> [P256M])
#     Sets, in the caller's scope:
#       <prefix>_DIR           package root
#       <prefix>_SOURCES       the compiled source set, in glob order
#       <prefix>_INCLUDE_DIRS  public + private include trees the sources need
#     For recipes that wrap the tree in their own target (host KATs, the
#     combined Mbed TLS object library).
#
#   ra8_add_tf_psa_crypto(<target>
#                         [CONFIG <header>]
#                         [CONFIG_INCLUDE_DIRS <dir>...]
#                         [P256M])
#     Builds <target> as the on-target software crypto archive every
#     secure-boot image links: the port Mbed TLS config, the given
#     TF_PSA_CRYPTO_CONFIG_FILE (default tf_psa_crypto_config.h, the port
#     one), and a static-buffer allocator behind mbedtls_calloc because the
#     firmware has no libc heap (ra8_sbrk_trap). CONFIG_INCLUDE_DIRS are
#     searched first, so an app-local config header (secure_boot_hil's
#     tf_psa_crypto_config_sb.h, dfu_bootloader's tf_psa_crypto_config_dfu.h)
#     wins the lookup. P256M adds the p256-m ECDSA driver sources.
#
# The include and source order below is exactly what the per-app recipes used,
# so the consolidation does not change a single compile line's semantics; the
# RA8FW-283 equivalence check compared the loadable bytes of every secure-boot
# image before and after.

include(${CMAKE_CURRENT_LIST_DIR}/zig_package.cmake)

function(ra8_tf_psa_crypto_paths prefix)
  cmake_parse_arguments(PARSE_ARGV 1 _tfp "P256M" "" "")
  ra8_zig_package_dir(tf_psa_crypto _tf)

  set(_globs ${_tf}/core/*.c ${_tf}/drivers/builtin/src/*.c)
  if(_tfp_P256M)
    list(APPEND _globs ${_tf}/drivers/p256-m/*.c ${_tf}/drivers/p256-m/p256-m/*.c)
  endif()
  list(APPEND _globs ${_tf}/platform/*.c ${_tf}/utilities/*.c ${_tf}/extras/*.c)
  file(GLOB _sources CONFIGURE_DEPENDS ${_globs})

  set(_includes ${_tf}/include ${_tf}/drivers/builtin/include)
  if(_tfp_P256M)
    list(APPEND _includes ${_tf}/drivers/p256-m ${_tf}/drivers/p256-m/p256-m)
  endif()
  list(
    APPEND
    _includes
    ${_tf}/core
    ${_tf}/dispatch
    ${_tf}/drivers/builtin/src
    ${_tf}/platform
    ${_tf}/utilities
    ${_tf}/extras
  )

  set(${prefix}_DIR
      "${_tf}"
      PARENT_SCOPE
  )
  set(${prefix}_SOURCES
      ${_sources}
      PARENT_SCOPE
  )
  set(${prefix}_INCLUDE_DIRS
      ${_includes}
      PARENT_SCOPE
  )
endfunction()

function(ra8_add_tf_psa_crypto target)
  cmake_parse_arguments(PARSE_ARGV 1 _tfa "P256M" "CONFIG" "CONFIG_INCLUDE_DIRS")
  if(_tfa_UNPARSED_ARGUMENTS)
    message(FATAL_ERROR "ra8_add_tf_psa_crypto(${target}): unknown arguments "
                        "${_tfa_UNPARSED_ARGUMENTS}"
    )
  endif()
  if(NOT _tfa_CONFIG)
    set(_tfa_CONFIG "tf_psa_crypto_config.h")
  endif()

  if(_tfa_P256M)
    ra8_tf_psa_crypto_paths(_tfa_tree P256M)
  else()
    ra8_tf_psa_crypto_paths(_tfa_tree)
  endif()
  ra8_zig_package_dir(mbedtls _tfa_mbedtls)
  get_filename_component(_tfa_root "${CMAKE_CURRENT_FUNCTION_LIST_DIR}/.." ABSOLUTE)

  add_library(${target} STATIC ${_tfa_tree_SOURCES})
  # SYSTEM: consumers and clang-tidy treat the vendored psa headers as system
  # headers, so SOUP header diagnostics never weaken the first-party profile.
  target_include_directories(
    ${target} SYSTEM
    PUBLIC ${_tfa_CONFIG_INCLUDE_DIRS} ${_tfa_tree_INCLUDE_DIRS} ${_tfa_mbedtls}/include
           ${_tfa_root}/port/mbedtls/inc
  )
  target_compile_definitions(
    ${target}
    PUBLIC MBEDTLS_CONFIG_FILE="mbedtls_config.h" TF_PSA_CRYPTO_CONFIG_FILE="${_tfa_CONFIG}"
           MBEDTLS_PLATFORM_MEMORY MBEDTLS_MEMORY_BUFFER_ALLOC_C
  )
  # No warning suppression: ra8_add_app() applies the project warning profile
  # to the app target, not to this archive, so its compile line is the
  # toolchain flags plus this one. -fno-strict-aliasing stays because it
  # changes code generation, not diagnostics.
  target_compile_options(${target} PRIVATE -fno-strict-aliasing)
endfunction()
