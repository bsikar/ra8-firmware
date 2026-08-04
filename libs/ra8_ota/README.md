# libs/ra8_ota -- ra8_ota vs ra8_dfu

Both update the firmware; they are not layers of each other and never meet in
one image.

`ra8_ota` is an **in-application** updater: the running app streams a signed
image into the *inactive* code-MRAM bank in place, then `ra8_ota_commit_and_reboot()`
latches the hardware boot-bank swap and resets. `ra8_dfu` is a **bootloader**:
an immutable resident that at reset picks between two software slots
(`ra8_dfu_boot_decide()`), **copies** the winner to SRAM at `k_ra8_dfu_run_base`
and launches it there, and is fed over USB-DFU.

| | `ra8_ota` | `ra8_dfu` |
|---|---|---|
| Runs when | the application is running | at reset, before any application |
| A/B mechanism | hardware boot-bank swap; the image executes in place from MRAM | software slot choice; the image is copied to a fixed SRAM base and runs there |
| Fed by | an injected network interface (`ra8_ota_download_to_inactive_bank()`) | USB-DFU (`dfu-util`) |
| Needs a bootloader | no | it *is* the bootloader |
| World | NS | S |

**Which do I use?** If the device is in the field and updates itself over the
network, `ra8_ota`. If a human plugs in USB, or you need a recovery path that
works when the application is broken, `ra8_dfu`. They are not alternatives to
each other so much as different moments.

One thing that looks like a third mechanism and is not: `ra8_nsc_ota_commit`
(and `src/secure_app/`) is the secure-world half of `ra8_ota`'s bank latch, not
a separate updater.

<!-- disambig
this: libs/ra8_ota
that: libs/ra8_dfu
symbol: ra8_ota_download_to_inactive_bank
symbol: ra8_ota_commit_and_reboot
symbol: ra8_dfu_boot_decide
symbol: k_ra8_dfu_run_base
users: ra8_ota = 1
users: ra8_dfu = 8
-->
