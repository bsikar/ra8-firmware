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

R1 pulls RES to +3V3_MCU; U2 pulls it low when its monitored supply is
below threshold or SW1 grounds U2's MR input. Additional external reset
drivers must be open-drain/open-collector to avoid contention with the probe.
No RC capacitor is fitted on RES. The external supervisor deliberately uses
the RES-pin startup path; do not assume the internal POR flag semantics of
a RES pin rising simultaneously with VCC (Quick Design Guide 2.5 and
6.1-6.2, pp.16 and 30-31; HUM 6.3.1-6.3.2).

Service tooling shall hold RES low for at least 3 ms after VCC is valid.
This exceeds the 2.4 ms power-on minimum in Datasheet Table 2.52, p.118.
Do not use a shorter operating-state minimum as the universal service pulse.
Switch bounce is not a substitute for a controlled programmer reset pulse.

### RST-001: external supervisor implementation basis

Implementation in progress: U2 is TI TPS3808G33DBVR. VDD and SENSE are
connected to +3V3_MCU, GND to ground, and CT has an intentional no-connect
marker. RESET connects to MCU_RESET_N and SW1 connects to MR through
SW_RESET_N. C44 is the local 100 nF VDD bypass. These connections have
been checked in the saved KiCad netlist. Native procurement fields and the
exported BOM record both exact ordering codes and dated sourcing. This adds a hardware undervoltage
reset independent of firmware configuration; it does not replace core-voltage
monitoring or qualify the complete power tree.

Authority: [TI TPS3808 datasheet](https://www.ti.com/lit/ds/symlink/tps3808.pdf),
SBVS050N, revised August 2026, sections 5, 6.5-6.6 and 7.3. The newer
revision is important: use its fixed-threshold accuracy, not an assumed
two-percent figure copied from another variant. The G33 nominal falling
threshold is 3.07 V; full-temperature accuracy is +/-1.5%, and fixed-version
hysteresis is at most 2.5% of the threshold. CT open selects 12..28 ms
reset-release delay (20 ms typical), without an external timing capacitor.

```text
Falling threshold minimum = 3.07*(1-0.015) = 3.02395 V
Falling threshold maximum = 3.07*(1+0.015) = 3.11605 V
Conservative rising-threshold screen = 3.11605*(1+0.025)
                                    = 3.19395125 V
Proposed 3.3 V source at -2% = 3.3*0.98 = 3.234 V
Static release margin = 3.234-3.19395125 = 0.04004875 V
Minimum delay / MCU power-on RES minimum = 12/2.4 = 5
```

The hysteresis screen applies the percentage to the maximum falling
threshold conservatively. The 2% rail tolerance is a proposed power-tree
requirement, NOT a verified regulator property. Ripple and distribution
losses must also fit within the release margin. The delay starts only after
SENSE and MR satisfy their release conditions; final rail settling must be
checked separately. SENSE-to-RESET propagation is 20 us typical with no
maximum in the table, so this part alone does not prove safe behavior during
an arbitrarily fast brownout or guarantee the USB analog rail never falls
below 3 V. USB-004's bead-drop screen is not a substitute for that analysis.

Implemented wiring: pin 6 VDD and pin 5 SENSE to +3V3_MCU, pin 2 GND,
pin 1 open-drain RESET to MCU_RESET_N with existing R1 10 kohm pull-up,
pin 4 CT intentionally open. SW1 connects to pin 3 MR so manual release also
receives the supervisor delay; the debugger connects directly to MCU_RESET_N.
MR has an internal pull-up. Do not connect MR to RESET, which would create
a self-holding reset loop. C44 provides the prescribed 100 nF VDD bypass:
TDK C1608X7R1H104K080AA, X7R, 50 V, +/-10%, sharing the documented
capacitance-screening basis of PWR-001 for the existing 100 nF bypasses.
No capacitor is added on MCU_RESET_N.

R1 at 3.6 V and -1% tolerance sinks at most 3.6/9900 = 0.363636 mA,
below the supervisor's 1 mA VOL test current at VDD >= 1.8 V. Its 0.4 V
maximum VOL must still be checked against the RA8P1 RES low threshold;
the current comparison alone is not logic-level qualification. Datasheet
Table 2.5 pp.48-49, note 1 explicitly includes RES in the VCC-domain
Schmitt-input row: VIL maximum is 0.2*VCC. At the minimum screened falling
threshold, 0.2*3.02395 = 0.60479 V, leaving 0.20479 V relative to the
supervisor's 0.4 V maximum VOL. This supports static low-level compatibility
near the monitored threshold; it does not cover an unpowered rail, total
debugger loading, leakage-driven high-level margin or reset-edge timing.
Cold startup
uses the MCU's RES-pin reset path (HUM 6.3.1 p.265), not an assumption that
PORF remains set while an external supervisor holds RES low.

The bundled KiCad Power_Supervisor:TPS3808DBV symbol has matching DBV
pin numbers but models RESET as a generic output. The project-local
Power_Devices:TPS3808G33DBVR variant was created through the Symbol Editor
with pin 1 changed to open collector; all six pin identities were checked
against TI Table 5-1. The installed global library is untouched. Comparing
the complete pre-existing symbol trees confirms no changes other than
serialization ordering and omission of the default 20 mil pin-name offset.
The compact supervisor retains the bundled symbol's uniform 100 mil pins.
The circuit is drawn, but reset high-level leakage, edge timing and complete
power-tree behavior remain unqualified.

[DigiKey 296-17195-1-ND](https://www.digikey.com/en/products/detail/texas-instruments/TPS3808G33DBVR/666727)
snapshot 2026-09-05: Active, 22054 in stock, 16-week standard lead time;
USD 1.84 / 1.355 / 1.0984 at quantities 1 / 10 / 100, excluding shipping,
tax and tariff. TI lists the exact ordering code in production.

Reproducible arithmetic screening (not a fitted-BOM or hardware test):

```sh
python3 - <<'PY'
from fractions import Fraction as F

low = F('3.07')*(1-F('.015'))
high = F('3.07')*(1+F('.015'))
release = high*(1+F('.025'))
margin = F('3.3')*F('.98')-release
if (low, high, release, margin) != (
        F('3.02395'), F('3.11605'), F('3.19395125'), F('.04004875')):
    raise ValueError('RST-001 threshold screening mismatch')
if F(12)/F('2.4') != 5 or F('3.6')/9900*1000 != F(4, 11):
    raise ValueError('RST-001 delay or pull-up screening mismatch')
vil_limit = F('.2')*low
low_margin = vil_limit-F('.4')
if (vil_limit, low_margin) != (F('.60479'), F('.20479')):
    raise ValueError('RST-001 static low-level screening mismatch')
print('RST-001 PASS: threshold, delay, pull-up and static low-level arithmetic.')
print('Hardware, high-level leakage, power-tree and transient checks remain open.')
PY
```

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

Connector and switch ordering codes remain to be selected. R1-R5 use
Yageo RC0603FR-0710KL; their ratings and sourcing are recorded in the BOM.
Both oscillator networks are wired as described below and in the linked
calculation records; board-level matching remains required.

## Oscillator qualification basis

The crystal-specific derivation, tolerance corners, parasitic sensitivity and
reproducible Python check are maintained in
[CLK-001: main oscillator load network](clock_calculations.md#clk-001-y1-main-oscillator-load-network).
The matching schematic annotation links to that record.

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
The imported FL2400022 remains in the candidate library but is no longer
the placed main crystal. Y1 is now Murata XRCGB24M000F3M19R0: 24 MHz,
CL 6 pF, ESR <= 100 ohm, drive <= 300 uW, initial tolerance +/-30 ppm,
temperature shift +/-40 ppm over -40 to +85 C, and aging +/-5 ppm/year
per exact specification JGC49-3003B. Its 100 ohm ESR is below the group-10
reference's 210 ohm screening limit; that comparison does not establish
oscillation margin on this board.

Y1 terminals 1 and 3 connect to U1.K17 EXTAL and U1.K16 XTAL through
local nets OSC_EXTAL and OSC_XTAL. The two other package lands are linked
internally and specified NC; the project symbol models these as NC pads
2 and 4, not grounded case pins. The exact footprint land numbering must
be reviewed at the deferred PCB stage against Murata Figure 1.

C35/C36 are TDK C1005NP01H080D050BA, 8 pF +/-0.5 pF, 50 V NP0, 0402,
each returning its oscillator node to ground. Equal 8 pF capacitors
provide 4 pF series load; an estimated 2 pF combined parasitic load gives
the crystal's nominal 6 pF. This is an initial matching network based on
Renesas's group-10 example, not a measured parasitic value. No external
feedback or damping resistor is fitted in this initial network. The PCB
must pass negative-resistance, startup, frequency and drive measurements;
adjust load capacitance or add damping only with matching evidence.
The RTC uses Y2 ABS07-LR-32.768KHZ-6-1-T and 4 pF C37/C38 on XCIN/XCOUT.
Its separate CLK-002 calculation records the exact part, assumed parasitics,
initial low-power-mode-2 setting and required hardware validation.

Sourcing checked 2026-09-05: DigiKey indexed stock 13,578 for Y1
(`490-18296-1-ND`, USD 0.32 / 0.275 / 0.239 at 1 / 10 / 100), and
73,746 for C35/C36 (`445-13781-1-ND`, USD 0.11 / 0.058 / 0.0341).
Both listings are active. Dated snapshots and exact links are in the
schematic-linked BOM; recheck stock before ordering.

The final selection must record exact ordering codes, load capacitance,
ESR, drive limits, MCU drive settings, and startup wait requirements.
Board-level verification must measure startup margin and excitation power;
do not probe the RTC resonator directly to measure its frequency. Use a
buffered clock output. Datasheet Tables 2.50-2.51 require oscillator
evaluation to determine stabilization waits, and HUM 9.3-9.4 governs the
connections and optional feedback/damping resistors.

## Sources

- [Murata exact Y1 specification](https://pim.murata.com/asset/pim4/ceramicResonatorCrystalUnit/SPEC_XRCGB24M000F3M19R0_PDF_CERAMICRESONATORCRYSTALUNIT),
  JGC49-3003B, ratings and terminal diagram, pages 2-3.
- [TDK exact load capacitor](https://product.tdk.com/en/search/capacitor/ceramic/mlcc/info?part_no=C1005NP01H080D050BA).
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
