//! Host-safe build options for the standalone production-source test gate.

/// Capture secure-register writes instead of performing them on the host.
pub const off_target: bool = true;

/// Keep root-of-trust authentication disabled for standalone compilation.
pub const enable_root_of_trust: bool = false;
