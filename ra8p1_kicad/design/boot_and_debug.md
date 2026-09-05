# RA8P1 boot, reset and debug

The selected MCU is R7KA8P1KFLCAC#UC0. U1A and J1 are on
`mcu_clocks_debug.kicad_sch`; this is not the RA8D2 evaluation-board circuit.

## Mode selection

R2 pulls P201/MD high for normal startup with on-chip MRAM enabled and
the external bus initially disabled. SW2 pulls MD low for service entry:
hold BOOT while asserting and releasing external RESET. JTAG boot and
SCI/USB boot cannot be entered using POR alone (HUM 4.3-4.4, pp.234-235).

SWD/JTAG boot entry requires a debugger boot request while RES is low,
with MD high at release. Device lifecycle, authentication and TrustZone
settings can restrict access. A debug connector does not guarantee recovery
from every security configuration. The MCU uses code MRAM and extra MRAM,
not internal program flash. FSBL settings also affect startup (HUM 2.6).

ROM USB programming uses USBFS, not USBHS. The product USBHS data port
must not be described as a ROM recovery interface. J1 provides dedicated
SWD/JTAG access; USBFS and SCI service routing need explicit decisions
before final schematic integration.

## Reset

R1 pulls RES to +3V3_MCU; SW1 grounds it. Additional external reset
drivers must be open-drain/open-collector to avoid contention with the probe.
Do not fit an RC capacitor on RES: it must track VCC for the internal POR
circuit (Quick Design Guide 2.5 and 6.1-6.2, pp.16 and 30-31).

Service tooling shall hold RES low for at least 3 ms after VCC is valid.
This exceeds the 2.4 ms power-on minimum in Datasheet Table 2.52, p.118.
Do not use a shorter operating-state minimum as the universal service pulse.
Switch bounce is not a substitute for a controlled programmer reset pulse.

## J1: Cortex debug connection

| Contact | Net | MCU ball / function |
| --- | --- | --- |
| 1 | +3V3_MCU | Target VCC sense only |
| 2 | DBG_SWDIO | C6 / P210 / TMS / SWDIO |
| 3 | GND | Ground |
| 4 | DBG_SWCLK | D6 / P211 / TCK / SWCLK |
| 5 | GND | Ground |
| 6 | DBG_TDO_SWO | E7 / P209 / TDO / SWO |
| 7 | No connection | Key position |
| 8 | DBG_TDI | C7 / P208 / TDI |
| 9 | GND | Ground detect |
| 10 | MCU_RESET_N | D5 / RES |

The probe senses the target I/O voltage at contact 1; do not inject target
power there or drive signals into an unpowered MCU. R3-R5 are external
10 kohm pull-ups on SWDIO, SWCLK and TDI, consistent with HUM 21.4.
TDO/SWO is an output with no pull. One interface supports both CPU cores.
No onboard J-Link MCU is included.

Connector, switch and resistor ordering codes remain to be selected. Crystal
networks remain to be designed; open oscillator pins in this increment are
not intentionally unused pins.

## Oscillator qualification basis

R01AN7202EJ0102 Rev.1.02 explicitly assigns RA8P1 to matching group 10
(Table 4.1). Do not substitute results for group 8 (RA8M1/RA8D1).

- Main clock, Table 4.1.9: the 24 MHz example uses a 6 pF crystal,
  8 pF external capacitors on each side, and no damping resistance. Its
  measured negative resistance is -1050 ohm and the recommended maximum
  crystal ESR is 210 ohm. These are evaluation-board results, not a
  guarantee for another crystal or PCB.
- Sub-clock, Table 4.2.10: the examples use a 6 pF crystal and 4 pF
  external capacitors. Recommended maximum ESR decreases from 340 kohm
  in normal mode to 180, 120, and 50 kohm in low-power modes 1, 2, and 3.
- The imported FL2400022 candidate is specified as 10 pF in its part
  metadata; the public FL-series sheet does not decode specification code
  0022. The imported ABS07-32.768KHZ-1-T is the default 12.5 pF variant,
  with a 70 kohm maximum ESR and 0.5 uW maximum drive per the Abracon
  series sheet. Neither is the 6 pF part used in the group-10 examples.

Do not assign load capacitors from those examples to the imported parts
without matching evidence. In particular, a 70 kohm RTC crystal does not
meet the example's 50 kohm criterion for the lowest-drive mode.
Y1 case pads 2 and 4 are grounded; its resonator terminals remain open
pending the clock-part and capacitor decision.

The final selection must record exact ordering codes, load capacitance,
ESR, drive limits, MCU drive settings, and startup wait requirements.
Board-level verification must measure startup margin and excitation power;
do not probe the RTC resonator directly to measure its frequency. Use a
buffered clock output. Datasheet Tables 2.50-2.51 require oscillator
evaluation to determine stabilization waits, and HUM 9.3-9.4 governs the
connections and optional feedback/damping resistors.

## Sources

- [RX and RA oscillator design guide](https://www.renesas.com/en/document/apn/renesas-rx-and-ra-families-design-guide-main-clock-circuits-and-sub-clock-circuits-rev102),
  R01AN7202EJ0102 Rev.1.02, Tables 4.1, 4.1.9 and 4.2.10; sections 5-7.
- [Diodes FL series](https://www.diodes.com/assets/Datasheets/FL.pdf),
  DS40067 Rev.2, pin functions and electrical characteristics.
- [Abracon ABS07 series](https://abracon.com/Resonators/ABS07.pdf),
  revised 2022-08-10, electrical specifications and ordering options.

- [RA8P1 Hardware User's Manual](https://www.renesas.com/en/document/mah/ra8p1-group-users-manual-hardware),
  R01UH1064EJ0130 Rev.1.30, sections 2.4-2.6, 4.3-4.4 and 21.4.
- [RA8P1 Datasheet](https://www.renesas.com/en/document/dst/ra8p1-group-datasheet),
  R01DS0439EJ0130 Rev.1.30, pin list and Table 2.52.
- [RA8x2 MCU Quick Design Guide](https://www.renesas.com/en/document/apn/ra8p1-mcu-quick-design-guide),
  R01AN7883EU0110 Rev.1.10, sections 2 and 6. This guide explicitly includes
  RA8P1; family-specific details still require its own HUM.
