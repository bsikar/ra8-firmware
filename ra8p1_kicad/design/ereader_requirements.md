# E-reader hardware requirements

## Product definition

Design a standalone RA8P1 e-reader using the existing `ereader/` KiCad project.
Integrate the e-paper timing controller, display power supplies, system power
management, and front-light driver on the main PCB. The Waveshare IT8951 HAT
is a prototyping reference, not a module in the finished product.

The following requirements were confirmed on 2026-09-05:

- Renesas RA8P1 host processor. The exact ordering code and package must be
  verified against the manufacturer's documentation before pin assignment.
- Capacitive touch input and physical buttons.
- Front light with independently adjustable warm and cool channels.
- A complete schematic and PCB design, developed and checked section by
  section, followed by whole-board integration and manufacturing checks.
- E-reader scope only. The Gaggia controller is a separate future project.

The active milestone is the complete electrical schematic, including readable
hierarchical sheets, qualified symbol pin mappings, ERC review, and a full
schematic PDF. PCB layout, footprint qualification, manufacturing outputs,
and physical bring-up are deferred; they are not schematic acceptance gates.

## Consolidated owner requirements (2026-09-07)

These requirements extend the original e-reader definition; they are not
optional substitutes for the display, touch, or front light. This is also a
portable music player. Unchecked items below are required work, not claims
that the current schematic implements them.

- [ ] RA8P1 host and ESP32-C6 radio, with complete supply, clock, reset,
  boot, programming, interconnect, and powered-off isolation circuits.
- [ ] Integrated e-paper controller, panel high-voltage power and VCOM,
  capacitive touch, and independently adjustable warm/cool front light.
  The Waveshare driver board is for prototyping only.
- [ ] Rechargeable battery, USB-C, charging and power-path management,
  protection, fuel measurement, safe power sequencing, and discharge.
- [ ] External RAM and soldered onboard storage at least matching the
  EK-RA8P1 baseline. The current reference is 64 MiB SDRAM and 64 MiB NOR;
  capacity alone does not establish equivalent bandwidth or compatibility.
- [ ] microSD for local music/files, with exact host interface, voltage,
  card-detect, protection, and repository-example compatibility checked.
- [ ] Premium headphone audio with both 3.5 mm single-ended and 4.4 mm
  balanced outputs, USB-DAC operation, and local-file playback.
  Define measurable noise, distortion, output impedance, load/power range,
  clocking, protection, and thermal requirements. Universal headphone
  compatibility or an unmeasured "best sound" claim is not acceptance.
- [ ] Built-in speakers and their amplifiers, with safe output selection,
  mute/pop suppression, and a complete battery/thermal budget.
- [ ] Camera capture hardware, grounded in the repository examples and
  official evaluation-board documentation, without pinmux conflicts.
- [ ] Five exposed physical buttons as specified below, plus touch input.
- [ ] Sensors, debug and factory recovery, including recovery when normal
  application firmware is unavailable.

### Five exposed buttons and recovery contract

The owner specified five buttons total on 2026-09-07. Allocate them as:

| Control | Required role |
| --- | --- |
| Power/wake | Power-on from off; wake/sleep request; orderly shutdown request; firmware-independent held-button forced off/recovery |
| Previous page | Page backward |
| Next page | Page forward |
| Volume down | Decrease audio volume |
| Volume up | Increase audio volume |

This count is one power button, two page buttons, and two dedicated volume
buttons. Do not silently collapse the latter four into two dual-use buttons.
Firmware may add contextual mappings, but the five physical controls remain.
For each, define debounce, ESD, pull defaults, voltage domain, leakage,
simultaneous-key behavior, and operation in off/boot/run/sleep/fault/update.
Reserve real MCU pins and verify wake-capable pin selection against the
selected sleep mode before completing the sheet.

The power button consolidates user-facing power/reset/recovery interaction;
it does not directly short both processors' unrelated boot-mode nets.
Normal startup is hardware controlled. A short press while powered is a
firmware input; a sustained press must ultimately force power off without
working firmware. Release and press again to restart. The exact forced-off
time, rail discharge, brownout/rearm behavior, and deep-sleep wake path require
electrical closure; a nominal timing calculation is not a guaranteed bound.

RA8P1 reset/boot and ESP32 boot service-pad pairs are internal fixture access,
not additional exposed buttons or fitted switches. Keep SWD and documented
ROM-loader access for blank/corrupt firmware. Any optional user-facing
button chord for a recovery UI is a separate firmware contract; do not claim
it substitutes for a proven hardware programming path.

### Execution and acceptance plan

Track the work in epic #821 and subsystem issues #822-#834, #840 (audio),
and #841 (camera). Issues #835-#838 cover deferred PCB/manufacturing stages.
Power and controls are shared work under #825/#832; memory, onboard storage
and microSD are under #827. Reopen interface and power-budget decisions when
the expanded audio/camera load invalidates earlier allocations.

1. Complete and independently review each electrical section and its exact
   symbol pin mapping; keep the BOM synchronized with placed parts.
2. Select viable parts using primary datasheets and dated DigiKey/Mouser
   stock, price, lifecycle, and performance evidence. Flag unqualified
   candidates and unavailable supporting specifications explicitly.
3. Connect hierarchical interfaces and actual MCU pins, including all rail,
   timing, pinmux, powered-off, recovery, and firmware dependencies.
4. Put useful equations, assumptions, results, and stable calculation IDs
   on the schematic and link them to full engineering documents. Verify
   calculations with Python; distinguish estimates from guaranteed limits.
5. Before each commit/push, review connectivity, pin types, ERC findings,
   calculations, BOM, and the native schematic plus every changed PDF page.
   Existing ERC errors are not a passing full-design gate.
6. Export the full multipage schematic PDF with the export script. Version
   editable project files, local libraries, necessary references, BOM,
   engineering documents, and exports using portable project-relative paths.
7. Complete whole-design review under #834 before claiming schematic done.
   PCB layout and footprint/model qualification remain deferred.

Latest owner direction is native KiCad GUI editing for design work. Scripts
are for exports and permitted supporting calculations/imports, not schematic
generation. Parallel agents research and independently review while one
editor owns the live KiCad design. Maintain compact, consistent symbols,
short clear wiring, readable hierarchy, standard power symbols, and honest
ERC drive semantics; do not add power flags merely to hide real errors.

Supporting engineering documents: [power](system_power_design.md),
[power button](single_button_power.md), [service](service_interface.md),
[audio](audio_subsystem.md), and
[camera/storage/memory](camera_storage_interfaces.md).

## Project organization

Keep the root project files together in `ereader/`. Place hierarchical sheets
beside the root schematic, grouped by electrical function. Reuse the functional
libraries in `libs/symbols/`, `libs/footprints/`, and `libs/3dmodels/`
according to `../LIBRARY_STANDARDS.md`. Use project-relative library
and model paths. Keep vendor references under `resources/` and electrical
design rationale under `design/`.

The hardware project, component libraries, references, and complete schematic
PDF are versioned together on the hardware branch. Machine-local KiCad state
and editor history are excluded. The large vendor design archive uses Git LFS;
see `../README.md` for clone and export instructions.

## Verified processor design basis

The selected design part remains `R7KA8P1KFLCAC#UC0`. Renesas RA8P1 Group
Datasheet R01DS0439EJ0130, Rev. 1.30 (2026-02-27), Figure 1.2 and Table 1.14,
identify the base part as a standard, dual-core device with MIPI DSI/CSI,
1 MB code MRAM, 2 MB SRAM, and no in-package serial flash. Its package code
is PLBG0289JA-A. The operating junction-temperature range is 0 to 95 deg C.
The suffix denotes full-tray packing, terminal material code C, and chip
version A. Component stock and commercial availability are not established by
this identification.

The 289-ball package is 12 x 12 mm on a 0.65 mm pitch. The alternate 224-ball
library symbol is not the selected board part. A switch to the extended
temperature grade requires a separate review of frequency and electrical
limits; it is not merely a BOM text substitution.

Table 1.16 requires one 0.1 uF bypass capacitor between each numbered
VCC/VCC2 supply and its matching VSS, placed close to the pins. VCL0 through
VCL11 each require a 0.22 uF local capacitor to the corresponding VSS0 through
VSS11. In the selected internal DCDC mode, all VLO pins connect to the input
of a 2.2 uH inductor. Its output feeds the common MCU_VCORE net, all VCL pins,
and a 47 uF output capacitor returned to VSS_DCDC. Never connect MCU_VCORE
directly to the 3.3 V supply. VCC_DCDC requires 22 uF and 0.1 uF in parallel
to VSS_DCDC. These connections follow the RA8P1 Hardware User's Manual
R01UH1064EJ0130 Rev. 1.30, Table 69.2 and Figure 69.1, pages 4042-4043.

The firmware contract is OFS2.DCDCEN = 1. External VDD mode is not selected:
it does not support software standby, deep software standby modes 1-3,
battery backup, or voltage scaling (section 69.2.2). Final passive selections
must also meet the regulator's electrical characteristics, including effective
capacitance and inductor current requirements.

L1 is TDK SPM5020T-2R2M-LR, explicitly recommended by the RA8x2 MCU
Quick Design Guide R01AN7883EU0110 Rev.1.10, Table 3. TDK specifies 2.2 uH
at +/-20%, maximum DCR 40.7 mOhm, typical temperature-rise current 4.6 A
(40 deg C rise), and typical inductance-change current 7 A (30% decrease).
The typical figures are not guaranteed minimum ratings. The same Renesas
table recommends Murata GRM32ER70J476KE20# and GRM31CR70J226KE19# for
the 47 uF and 22 uF capacitors; exact packing suffixes and final capacitor
qualification remain open. The guide is retained under `resources/`.

The C3 BOM candidate is now GRM31CR70J226KE19L, 22 uF +/-10%, 6.3 V X7R,
with the exact packaging suffix and supplier link recorded in KiCad. Its
nominal value and reference-design recommendation do not establish minimum
effective capacitance under DC bias, temperature and aging; qualification
remains open.

The 47 uF reference candidate GRM32ER70J476KE20L is NOT approved for C9.
On 2026-09-05 the live Mouser browser search showed 7,034 in stock but an
End of Life flag; DigiKey's indexed listing showed zero stock and Active.
Resolve lifecycle with the manufacturer or qualify a current-production
replacement. Inventory alone is not evidence of ongoing production.
Keep C9's exact MPN unapproved until this and effective-capacitance/ESR
requirements are resolved. Sourcing evidence:
[Mouser reference part](https://www.mouser.com/ProductDetail/Murata-Electronics/GRM32ER70J476KE20L?qs=xcCo%252BfWZmQXLPFvGhPRdVA%3D%3D),
[DigiKey reference part](https://www.digikey.com/en/products/detail/murata-electronics/GRM32ER70J476KE20L/2039090).

The existing boot notes are provisional. Debug target-reference voltage must
follow the actual debug-pin supply domain; it is not a power input from the
probe. Boot-mode and device-lifecycle restrictions must be checked before
claiming a production recovery path.

Sources: [RA8P1 Group Datasheet](https://www.renesas.com/en/document/dst/ra8p1-group-datasheet)
and [RA8P1 Hardware User's Manual](https://www.renesas.com/en/document/mah/ra8p1-group-users-manual-hardware).

## Display section design inputs

The 6-inch 1448 x 1072 Waveshare HD HAT is the initial reference. Its panel
is not yet a confirmed final assembly. The selected assembly must establish
all of the following together:

- Exact panel model, revision, active area, outline, flex geometry, and mating
  connector contact orientation.
- Touch overlay, controller location, interface, voltage, and connector.
- Light guide and warm/cool LED strings, including current, forward-voltage
  range, connection topology, and independent channel access.
- Timing-controller compatibility, boot firmware, panel waveform data,
  temperature compensation, and programming method.
- Panel-specific VCOM, power rails, startup/shutdown order, discharge timing,
  and recovery after interrupted refresh or battery loss.

Do not select a front-light driver solely from the phrase "dual channel";
the actual LED assembly determines voltage and current requirements.
Do not treat an imported symbol or an evaluation-board circuit as verified
electrical compatibility with the selected RA8P1 ordering code.

## Section acceptance criteria

A section is complete for schematic integration when exact parts and source
documents are identified, symbol pin mappings are checked against those
documents, the connected circuit and passive values are reviewed, and its
power, timing, firmware, mechanical, and neighboring-sheet interfaces are
explicit. Record unresolved dependencies instead of selecting arbitrary
values to make a sheet appear complete.

PCB completion additionally requires a fabricator-supported stackup and BGA
escape strategy, placement and return-path review, routing constraints,
resolved ERC/DRC findings or justified exceptions, and verified fabrication
and assembly outputs. Hardware validation is separate from design-file
validation and requires assembled boards.

## Reference sources

- [Waveshare HD HAT product](https://www.waveshare.com/6inch-HD-e-Paper-HAT.htm)
- [Waveshare HD HAT documentation](https://www.waveshare.com/wiki/6inch_HD_e-Paper_HAT)
- [ITE IT8951 product](https://www.ite.com.tw/en/product/cate5/IT8951)
- [E Ink ED060KHE product](https://www.eink.com/product/detail/ED060KHE)

The ITE product page identifies a controller family candidate; it does not
establish component stock, panel-specific firmware availability, or approval
of an exact package for this board.

## ED060KHE assembly candidate

The existing `resources/datasheets/E-Ink_ED060KHE_Display_Simplified_Specification.pdf`
identifies the assembly as VD1405-FOH (ED060KHE). Its feature set includes
the requested touch and warm/cool front light. It remains a candidate, not
a selected or controller-qualified component.

The simplified specification, sections 5.4 through 5.7 (pages 5-6), establishes
separate panel, front-light, and touch connections. The front-light table
describes seven cool LEDs and six warm LEDs with separate channel terminals.
It does not establish operating current or forward-voltage limits, so those
counts alone are insufficient to size the LED driver.

The touch table specifies 1.8 V digital power and I/O, and a separate
2.7-3.5 V analog supply. Account for voltage-domain compatibility in the host
interface. The same page recommends a 10-contact connector while tabulating
only eight touch pins and labels the pin assignment "for Proto only".
Resolve this discrepancy against the final assembly drawing before selecting
the touch connector or releasing its pin mapping.

The simplified specification is insufficient to finalize the panel power
supplies, LED driver, or controller firmware. Obtain the matching full
electrical specification and controller/waveform support for the exact
assembly revision. Equal resolution does not establish compatibility with
the prototype Waveshare board's firmware.
