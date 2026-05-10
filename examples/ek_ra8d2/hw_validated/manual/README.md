# examples/ek_ra8d2/hw_validated/manual/

Apps here are hardware-confirmed but cannot be tested automatically by HIL
CI because they require either:
- A physical interaction (button press) that CI cannot trigger, or
- A peripheral not present on the HIL bench (I2C controller, RTT viewer).

HIL CI builds these but skips the run/verify step.  Manual sign-off is
required before promoting an app out of this directory.

To build: `make <appname>` from the repo root.

## Apps and blocking reason

| App | Why CI cannot auto-verify |
|-----|--------------------------|
| eth_loopback | Requires Ethernet loopback cable or connector on the RJ-45 jack |
| icu_extint_demo | All UART output is triggered by an external interrupt (SW1 / IRQ13); no automatic output at boot |
| iic_b_peripheral_demo | Peripheral-mode I2C; needs an external I2C controller to initiate transactions |
| kint_demo | All UART output is triggered by SW1 button press; no automatic output at boot |
| rtt_log_demo | Output goes to SEGGER RTT over SWD, not to the UART (/dev/ttyACM0) |
| spi_loopback | Requires COPI/CIPO loopback jumper wired on the SPI header |
| ssie_audio_loop | Requires an audio codec peripheral connected to the SSIE pins |
