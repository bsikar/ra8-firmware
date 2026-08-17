# blink_hal

The same 1 Hz blink as `blink`, driven through `ra8_gpio_*` and `ra8_time_*`
instead of raw register writes, across all three user LEDs.

Where `blink` answers "is the chip alive?", this answers "is the HAL stack
alive?". It pulls in the PFS unlock sequence, the SysTick tick, and the pin
validator -- so each LED pin is claimed and a double-assignment would be refused
rather than silently won by whoever wrote last. Run it second when bringing up a
board, once `blink` works.

Like `blink` it stays on the reset-default MOCO rate and skips CGC bring-up,
infrastructure init and TrustZone programming, so the failure surface is the HAL
and nothing else. When one of those subsystems is ready to switch on, this is
the canonical place to add it -- one at a time.

The demo speaks board coordinates (LED1 / LED2 / LED3) rather than chip pins;
the BSP maps them to P600 / P303 / PA07 per EK-RA8D2 v1 UM (R20UT5523EG0101)
Table 24 p 31.
