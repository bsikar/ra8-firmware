/**
 * @file examples/ek_ra8d2/tz_secure_only_usb_fs/secure_exception.c
 * @brief No-op SecureFault placeholder for the secure-only USB experiment
 *
 * @par Tag
 * [Ring 1 / Boot] {World: S}
 *
 * @details
 * The dual-world `usb_cdc_echo` build supplies a custom
 * `SecureFault_Handler` that snapshots SFSR and spins. This experiment
 * runs entirely in the Secure world with no SAU partition, so a
 * SecureFault cannot fire by construction. We omit the custom handler
 * entirely; the weak alias to `Default_Handler` in `vector_table.c`
 * stays in effect and any unexpected fault traps to `bkpt #0` for
 * inspection just like the other core exceptions.
 *
 * The file is kept (rather than deleted) so the per-app source list in
 * `CMakeLists.txt` does not have to diverge from the sibling app.
 *
 * @copyright Copyright (c) 2026 Brighton Sikarskie
 * SPDX-License-Identifier: MIT
 */

/* Translation unit deliberately empty -- no symbols emitted. */
