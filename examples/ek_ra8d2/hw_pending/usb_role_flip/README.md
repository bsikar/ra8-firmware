# USB Runtime Role Flip Self-Loop

Validates dynamic runtime role swapping between the EK-RA8D2's two USB controllers
(USBFS on J11 and USBHS on J7) connected to each other via a Type-C to Type-C self-loop cable.

## Concept

In static configurations, each port's role is pinned at boot:
- Config A: USBFS = Device, USBHS = Host (`dfu_selftest_boot`, `usb_selftest_cdc`, `usb_selftest_dfu`)
- Config B: USBFS = Host, USBHS = Device (`dfu_selftest_fs_host`)

`usb_role_flip` exercises dynamic teardown and role reversal in a single running firmware image:
1. **Config A (Initial)**:
   - J11 (USBFS): Device role (`ra8_board_usb_port_init(k_ra8_board_usb_port_fs, k_ra8_board_usb_role_device)`)
   - J7 (USBHS): Host role (`ra8_board_usb_port_init(k_ra8_board_usb_port_hs, k_ra8_board_usb_role_host)`)
   - Initialises `ra8_usb_pal` and `ux_dcd_ra8_usb` on FS.
   - Initialises `ra8_usb_host` on HS.
   - Attaches FS device to host and verifies link readiness.
2. **Teardown Config A**:
   - Detaches FS device (`ra8_usb_pal_attach(false)`, `ra8_usb_device_attach(false)`).
   - Uninitialises DCD bridge (`ux_dcd_ra8_usb_uninitialize`).
   - Uninitialises PAL (`ra8_usb_pal_deinit`).
   - Uninitialises HS host (`ra8_usb_host_deinit`).
3. **Config B (Role Flip)**:
   - J11 (USBFS): Host role (`ra8_board_usb_port_init(k_ra8_board_usb_port_fs, k_ra8_board_usb_role_host)`)
   - J7 (USBHS): Device role (`ra8_board_usb_port_init(k_ra8_board_usb_port_hs, k_ra8_board_usb_role_device)`)
   - Initialises `ra8_usb_pal` and `ux_dcd_ra8_usb` on HS.
   - Initialises `ra8_usb_host` on FS.
   - Attaches HS device to host and verifies link readiness.
4. **Teardown Config B**:
   - Detaches HS device.
   - Uninitialises DCD bridge (`ux_dcd_ra8_usb_uninitialize`).
   - Uninitialises PAL (`ra8_usb_pal_deinit`).
   - Uninitialises FS host (`ra8_usb_host_deinit`).

Once both configurations complete cleanly, the app emits `USB SELFTEST ROLE-FLIP PASS`
over SCI8 (115200 8N1) and lights LED2.
