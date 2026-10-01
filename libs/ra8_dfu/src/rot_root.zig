//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for the root of trust alone.
//!
//! `ra8_rot.c` compiled to an empty translation unit unless the app defined
//! `RA8_ENABLE_ROOT_OF_TRUST`, which is how an app that links `ra8_dfu`
//! without `ra8_psa_crypto` still linked. A prebuilt archive cannot see an
//! app's compile definitions, so the equivalent here is a separate artifact:
//! only the apps that opt in link it, and they are exactly the apps that
//! already link `ra8_psa_crypto` and the RSIP HAL. Apps that do not opt in
//! link nothing new.

comptime {
    _ = @import("rot_abi");
}
