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
