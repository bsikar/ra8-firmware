//! SPDX-License-Identifier: MIT
//! Copyright (c) 2026 Brighton Sikarskie
//!
//! Archive root for `ra8_dfu_device`, the USBX DFU device class glue.
//!
//! Its own archive because linking it is the opt-in to USBX: an app that
//! links `ra8_dfu` alone (dfu_copy_to_run) gets no reference to the USB
//! stack, as it did when this was a C file that compiled away without
//! ux_api.h.

comptime {
    _ = @import("device_abi");
}
