# elc_event_demo

Event Link Controller (ELC) software-event demo for the EK-RA8D2.
The ELC lets one peripheral fire another peripheral's input
without CPU involvement (HUM Ch 19, p 817..836). This demo:

1. `ra8_elc_init` -- powers on the controller and clears every
   ELSR slot.
2. `ra8_elc_link(0, k_ra8_elc_event_icu_irq0)` -- routes external
   IRQ0 into ELSR slot 0. On a real EVM a button on IRQ0 would
   trigger a hardware capture without a single CPU instruction.
3. Once a second, `ra8_elc_software_trigger(0)` fires the 3-step
   ELSEGR unlock-arm-set sequence (HUM Ch 19.2.2) so the runtime
   never depends on the user pressing a button.

SCI8 (115200 8N1, J-Link OB CDC) prints

```
elc: en=1 trig=NN
```

LED1 toggles per cycle; LED2 latches ON if any ELC call hard-fails.

## Build + flash

```sh
make elc_event_demo
make -C examples/ek_ra8d2/elc_event_demo flash
```

Bare EK-RA8D2 only -- no external pins required because the
software trigger replaces a real edge.
