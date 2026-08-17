# port/

Adapters between the vendored stacks in [`libs/third_party/`](../libs/third_party/)
and this firmware. Each directory is first-party code held to the full style and
safety rules; the stack it adapts is not. Nothing here reimplements a stack --
the adapter is the seam, so the vendored source stays unpatched and updatable.

Each port carries one fact worth knowing before you open it:

| Port | Bridges |
|---|---|
| [`threadx/`](threadx/) | Eclipse ThreadX to the RA8 clock tree -- it retunes the kernel tick, because the vendored assembly start-up programs SysTick from a compile-time clock constant. |
| [`netxduo/`](netxduo/) | NetX Duo to a link layer: the Ethernet driver over `ra8_net_pal`, and its wireless twin over the ESP32-C6. |
| [`usbx/`](usbx/) | USBX's device stack to the hand-written `ra8_usb` controller driver. |
| [`levelx/`](levelx/) | A LevelX wear-levelled NOR partition up to an `ra8_fs` block device. |
| [`mbedtls/`](mbedtls/) | The project-wide Mbed TLS feature set -- one configuration header, no source. |
| [`nimble/`](nimble/) | Apache NimBLE's HCI transport to the `ra8_ble` HCI ring. |
| [`esp-hosted/`](esp-hosted/) | The vendored esp-hosted core to this tree, including which Bluetooth host runs above HCI. |
| [`esp32_c6/`](esp32_c6/) | The integration contract for the media RPC component running on the C6 itself. |
| [`posix/`](posix/) | A root-confined hosted POSIX adapter, so host tools and unit tests drive the same filesystem interface the firmware does. |
