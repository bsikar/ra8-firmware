//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The host C dialect and warning bar the root build graph compiles every
//! first-party host translation unit at. It lives beside the graph's other
//! flag sets rather than in build.zig because two slices need it: the host
//! suites in the root graph and the vendored SOUP tree in `vendored_soup.zig`,
//! whose first-party bar is this set plus `-Wconversion`.

/// The host C dialect and warning set from tests/cmake/host_config.cmake.
/// `RA8_OFF_TARGET` and `UNIT_TEST` are the two definitions that file adds to
/// every host TU; without them the C that reads system registers reaches for
/// Cortex-M `mrs`.
/// `-Werror` stays on: a suite that only compiles under a looser dialect here
/// than it does under CMake would make the parity claim meaningless.
pub const c_flags = [_][]const u8{
    "-std=c23",
    "-Wall",
    "-Wextra",
    "-Wpedantic",
    "-Werror",
    "-DRA8_OFF_TARGET",
    "-DUNIT_TEST",
};
