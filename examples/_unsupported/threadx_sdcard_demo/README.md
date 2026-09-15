# threadx_sdcard_demo

A ThreadX SD-card smoke test that initializes the board clock, console, and
SDHI-backed `ra8_sdcard` HAL, then reads sector zero every five seconds. It
prints the first 16 bytes as hexadecimal and toggles LED1 after each successful
read.

The application exercises the repository's ThreadX port together with the
board SDHI pin setup and block-read path. Its vector table retains the project's
weak handler aliases, lets the upstream port supply PendSV and SVC as strong
symbols, and overrides SysTick to call the ThreadX timer interrupt.

Insert an SD card before running. SDHI routing is owned by
`ra8_board_sdhi_pins_init`; console output uses the on-board J-Link VCOM at
115200 8N1. A successful iteration prints `sdcard: blk0[0..15] =` followed by
16 bytes. Read failures are logged and retried after the five-second delay.
