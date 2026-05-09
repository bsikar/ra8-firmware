# watchdog_demo

IWDT watchdog + reset-cause demo for the bare EK-RA8D2 EVM.

On boot, logs the reset cause over SCI8 (115200 8N1, J-Link OB CDC port):

- `wdt: boot reason=power_on` -- first boot after a real power-on.
- `wdt: boot reason=iwdt`     -- woke up from a watchdog reset
  triggered by the previous run.
- `wdt: boot reason=other`    -- any other latched reset cause.

Then refreshes the IWDT for 30 seconds (LED1 toggles each refresh as
a heartbeat), logs `wdt: stopping refresh, expect reset`, and stops
feeding the watchdog. The IWDT counter underflows and the chip
resets; the next boot logs `iwdt`, demonstrating end-to-end
reset-cause introspection.

Build / flash:

```
make watchdog_demo
make -C examples/ek_ra8d2/watchdog_demo flash
```
