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
documents are identified, symbols and footprints are checked against those
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
