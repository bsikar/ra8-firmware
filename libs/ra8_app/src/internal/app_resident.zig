//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! The resident image's whole ra8_app surface, bound once: the `.ra8app`
//! admission gate (appimg_verify) and the `ra8_app_api_t` types, sandbox and
//! dispatch (app_api). A Zig file may belong to one module graph, so a
//! resident image that both verifies and serves binds this single root
//! instead of the two internal ones.

/// The `.ra8app` admission gate: container, grant, signature.
pub const verify = @import("appimg_verify.zig");

/// `ra8_app_api_t`: abi, memory, sandbox, kernel, module shims, image.
pub const api = @import("app_api.zig");
