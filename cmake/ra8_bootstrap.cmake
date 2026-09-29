# SPDX-License-Identifier: MIT
# Copyright (c) 2026 Brighton Sikarskie
#
# cmake/ra8_bootstrap.cmake -- make this repository's cmake/ directory findable
# by name, so a per-app listfile can say `include(ra8_add_app)` from any depth
# instead of carrying its own walk up the directory tree (#779).
#
# The problem this solves
# -----------------------
# An app's CMakeLists.txt can be the TOP-LEVEL listfile (standalone
# `cmake -S examples/<tier>/<bucket>/<app> -B build`) or a subdirectory of the
# repo-root build. In the standalone case nothing has told CMake where the repo
# is, and apps sit at three different depths under examples/ (4, 5 and 6 path
# components), so a fixed relative include() is impossible. Every app therefore
# open-coded the same five-line discovery loop:
#
#     set(_d "${CMAKE_CURRENT_SOURCE_DIR}")
#     while(NOT EXISTS "${_d}/cmake/ra8_add_app.cmake" AND NOT "${_d}" STREQUAL "/")
#       get_filename_component(_d "${_d}" DIRECTORY)
#     endwhile()
#     include("${_d}/cmake/ra8_add_app.cmake")
#
# 237 of 237 app listfiles carried it verbatim.
#
# Where this file is included from, and why that covers both cases
# ----------------------------------------------------------------
# cmake/toolchain-ra8d2.cmake includes this file, exactly as it already includes
# cmake/ccache.cmake and for the same reason: the toolchain file is the one
# place EVERY cross build passes through, whatever the entry point, and it knows
# where the repo is because it knows where it itself lives.
# cmake/toolchain-ra8p1.cmake includes the RA8D2 toolchain verbatim, so the
# RA8P1 builds are covered by the same line.
#
#   - standalone: the app's own project() call processes the toolchain file,
#     which runs this, so CMAKE_MODULE_PATH is set in the app's scope before
#     the app's include(ra8_add_app) on the next line.
#   - embedded:   the repo root processes the toolchain file during its own
#     project() call (and includes this file directly as well), and
#     CMAKE_MODULE_PATH is inherited by every add_subdirectory() below it.
#
# Both paths therefore reach the recipe by name. An app configured with no
# toolchain file at all is already unbuildable (these are cross-only,
# freestanding targets), so it is not a case this seam has to serve.
#
# Variables exported
# ------------------
#   RA8_ROOT       absolute path to the repository root
#   RA8_CMAKE_DIR  absolute path to <repo>/cmake, also appended to
#                  CMAKE_MODULE_PATH
#
get_filename_component(RA8_CMAKE_DIR "${CMAKE_CURRENT_LIST_DIR}" ABSOLUTE)
get_filename_component(RA8_ROOT "${RA8_CMAKE_DIR}/.." ABSOLUTE)

# Idempotent: the toolchain file is re-read in every scope that calls project()
# (and once more per try_compile), so guard against piling up duplicates.
if(NOT "${RA8_CMAKE_DIR}" IN_LIST CMAKE_MODULE_PATH)
  list(APPEND CMAKE_MODULE_PATH "${RA8_CMAKE_DIR}")
endif()
