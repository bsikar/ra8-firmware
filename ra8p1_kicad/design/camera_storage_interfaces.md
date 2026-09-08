# Camera, removable storage and external-memory allocation

Revision 13, 2026-09-08. Target: R7KA8P1KFLCAC#UC0, MIPI-enabled BGA289.
This is an engineering allocation record for native KiCad implementation,
not a completed schematic, verified timing closure or hardware qualification.
Cross-references: [radio](radio_interface.md), [power](system_power_design.md),
[single button](single_button_power.md), and [parts inventory](../PARTS-CHECKLIST.md).

## CMS-001: Architecture and evidence boundary

Reserve 64 MiB of 32-bit SDRAM, at least 64 MiB of soldered NOR, a separate
4-bit microSD socket, and the CU450_OV5640 camera interface in MIPI mode.
This preserves the minimum evaluation-board memory capacities without
pretending that 64 MiB of firmware/assets NOR is a GB-scale music library.
Music resides on microSD in this baseline. An additional managed eMMC is
possible, but no exact in-stock production eMMC is selected in this record.

| Function | Allocation | Reason / restriction |
| --- | --- | --- |
| External RAM | SDRAMC, 16M x 32, 64 MiB | EK capacity baseline; complete port reservation in CMS-004 |
| Soldered firmware/assets | OSPI0; reserve octal group | Existing EK firmware is part-specific; Quad alternative is not Octal performance equivalence |
| Removable music/data | SDHI1_B, four data bits, 3.3 V | Avoids SDRAM, radio and proposed SSI1_A audio |
| Camera | Two-lane MIPI CSI-2 + P501/P709/P511/P512/P010 | EK DVP mapping conflicts with SDHI1_B and radio |
| Audio coordination | SSI1_A: P907/P906/P206 | Do not reuse SSI0_A, SSI0_B or SSI1_B blindly |
| Five physical controls | Power INT P303; page P309/P310; volume P311/P909 | Independent IRQ-DS channels; CMS-009 |
| Application shutdown | POWER_KILL_N P903/D9 | NMOS-open-drain GPIO, switched VCC; BTN-009 |
| Optional managed storage | Reserve SDHI0_C, four data bits | Eight bits would consume audio P206; PD02/PD03 also replace the EK SCI8 console |

The existing [camera capture example](../../examples/ek_ra8d2/hw_validated/hil/camera_capture/README.md)
is a DVP/CEU capture path. The existing
[USB/microSD self-test](../../examples/ek_ra8d2/hw_validated/hil/usb_selftest_microsd/README.md)
uses SCI0 Simple-SPI, not native SDHI. Therefore neither example proves the
new simultaneous MIPI + native-SDHI allocation. CSI and VIN HAL source exists
in [ra8_mipi_csi.c](../../libs/ra8_hal/src/ra8_mipi_csi.c) and
[ra8_vin.c](../../libs/ra8_hal/src/ra8_vin.c); implementation is not evidence
of a validated RA8P1 capture configuration. Zephyr likewise documents only
DVP support for this shield on RA at retrieval time.
[Zephyr's CU450 shield documentation](https://docs.zephyrproject.org/latest/boards/shields/arducam_cu450_ov5640/doc/index.html).

## CMS-002: Exact camera module and connector contract

Use **Arducam CU450_OV5640**, the 36 x 40 mm Camera Expansion Board named in
the [EK-RA8P1 v1 manual, Rev.1.04, section 3 and Table 36](https://www.renesas.com/en/document/mat/ek-ra8p1-v1-users-manual).
This identifies the module and its actual 40-contact interface. The raw
OV5640 sensor datasheet does not define that connector. B0156, B0530,
OV5647 Raspberry Pi cameras, and arbitrary OV5640 breakout boards are not
pin-compatible substitutes without their own complete connector audit.

The following is the EK Table 36 MIPI-mode contract, visually checked
against the original PDF. Contact numbers are module-interface numbers,
not a promise about an unselected FFC connector's top/bottom contact side.

| Module contact(s) | Net/function | RA8P1 port / BGA289 ball |
| --- | --- | --- |
| 5 / 6 | CAM_DL1_P / CAM_DL1_N | Dedicated MIPI_DL1_P T3 / MIPI_DL1_N U3 |
| 8 / 9 | CAM_CL_P / CAM_CL_N | Dedicated MIPI_CL_P T2 / MIPI_CL_N U2 |
| 11 / 12 | CAM_DL0_P / CAM_DL0_N | Dedicated MIPI_DL0_P T1 / MIPI_DL0_N U1 |
| 20 | Camera SCL | P512 / P13, SCL1_A |
| 21 | Camera SDA | P511 / U15, SDA1_A |
| 25 | Camera reset | P709 / P16, GPIO |
| 26 | Camera XCLK | P501 / R8, GTIOC12A |
| 28 | Camera interrupt | P010 / P10, IRQ14 |
| 31, 34, 35, 36, 39 | Module +3.3 V | Camera supply domain; qualify peak current and sequencing |
| 1, 4, 7, 10, 13, 16, 19, 22, 29, 30, 32, 33, 37, 38, 40 | GND | Common ground |
| 2, 3, 14, 15, 17, 18, 23, 24, 27 | Unused in MIPI mode | Intentional no-connect on host |

The MCU additionally needs VCC18_MIPI at R2 (1.65..1.95 V), AVCC_MIPI
at T4 (2.90..3.60 V), and VSS_MIPI at R3 (GND). Remove any prior no-connect
markers from those supply/PHY pins when implementing this function.
The [RA8P1 datasheet](../../docs/reference/ra8p1-datasheet.pdf), Table 2.44,
specifies supply rise gradients in **us/V**, not V/us. A separate qualified
1.8 V supply, decoupling, ramp/sequence review and CSI clock calculation are
required; a global power label alone does not create that rail.

The module shares its differential contacts with DVP pins. Select MIPI in
the sensor configuration; do not wire both operating modes simultaneously.
The existing DVP setup's nominal 24 MHz XCLK, reset low/high delays and
SCCB transaction path are useful bring-up evidence, not a complete MIPI
sensor-register program. The PHY is shared with MIPI-DSI, so this allocation
precludes a simultaneous MIPI-DSI display; the e-paper interface does not
need DSI. Signal polarity and clock/data lane ordering are fixed above.

Production gates: obtain exact CU450 orderability, module revision/schematic,
power consumption and FFC/cable mating contract. An independently purchasable
CU450 listing with current DigiKey/Mouser stock was not verified. The EK
module supports prototyping, but this record does not authorize buying an
unverified camera or inferring its internal regulators. For camera power
gating, address SCCB pullups and every clock/control back-power path; pulling
RESET low is not equivalent to removing power safely.

## CMS-003: microSD native connection and a repository mismatch

Reserve the entire SDHI1_B six-signal bus. The BGA289 assignment is checked
against [RA8P1 Table 1.17](../../docs/reference/ra8p1-datasheet.pdf) and also
[RA8D2 Table 1.16](../../docs/reference/ra8d2-datasheet.pdf), Rev.1.30,
2026-02-27. P400/P401 appear on printed page 32 in both datasheets.

| Signal | Port / ball | DM3AT-SF-PEJM5 contact |
| --- | --- | --- |
| SD1CLK_B | P400 / P17 | 5 CLK |
| SD1CMD_B | P401 / N17 | 3 CMD |
| SD1DAT0_B | P402 / L14 | 7 DAT0 |
| SD1DAT1_B | P403 / H13 | 8 DAT1 |
| SD1DAT2_B | P404 / J13 | 1 DAT2 |
| SD1DAT3_B | P405 / G12 | 2 DAT3 |
| SD1CD | P406 / F14 | MP2 CD_B; MP4 CD_A to GND |
| Card supply / ground | Qualified +3.3 V / GND | 4 VDD / 6 VSS |

The socket's A/B detect switch is separate from its eight card contacts.
The project symbol `Connectors:DM3AT-SF-PEJM5` now names MP2 `CD_B`,
MP4 `CD_A`, and MP1/MP3/MP5/MP6 `SHIELD1`..`SHIELD4`. Ground all four
shield pads. Hirose's
[EDC-325165-00-00 drawing, page 1, note 2](https://www.hirose.com/product/download/?distributor=chip1&lang=en&num=DM3AT-SF-PEJM5&type=2d)
shows A/B open without a card and closed with a card. The mapping is an
audit of the imported pad identities: B is the rear contact beside DAT1
(MP2), and A is the side contact 10.5 mm forward (MP4). `MP` here is the
imported identifier, not a declaration that every such pad is a shield.
Pin 2 is displayed as `DAT3` to avoid confusing the card's DAT3/CD function
with this independent mechanical switch. No pin numbers were changed.
All fourteen pins remain passive, with consistent 150 mil pin lengths and
50 mil text. The two detect contacts and four shields are visually grouped.
The native Symbol Checker reports no issues. The socket is not yet placed
in the schematic; this library checkpoint does not complete the microSD
circuit, qualify the retained footprint, or add a populated BOM row.
microSD has no mechanical write-protect switch. SD1WP is available at P700,
which is already assigned to the radio; it is not needed for this socket.

Sourcing snapshot, 2026-09-08: the exact active socket is
[DigiKey HR1964CT-ND](https://www.digikey.com/en/products/detail/hirose-electric-co-ltd/DM3AT-SF-PEJM5/2533566),
30,851 in stock, USD 3.55 / 3.019 / 2.56560 each at quantities 1 / 10 / 100,
with a quoted 16-week manufacturer lead time. Stock is not reserved and
prices exclude tax/shipping. The imported Mouser part number
`798-DM3AT-SF-PEJM5` is retained; a current US/USD Mouser quote was not
verified in this checkpoint. Copy refreshed procurement fields into the
schematic instance and regenerate the BOM when it is actually placed.

Use 3.3 V signaling. Do not claim UHS/HS200/HS400 capability from an eMMC
marketing version. The RA8P1 SDHI SDR timing table gives a 20 ns minimum
clock period in the relevant 3.3 V conditions. Keep all CMD/DAT/CLK pins in
the selected _B timing group. Begin initialization at the card-specified
low speed; 50 MHz is an upper interface target, not an automatically valid
board clock. Select CMD/DAT pullups, card-detect pullup, source termination,
low-capacitance protection, effective bypass capacitance and any load switch
after their leakage, drive, timing and inrush calculations. Do not pull up
CLK by habit. Power-off states must not phantom-power the card through IOs.

Confirmed software-definition defect, not changed by this hardware task;
tracked separately in [issue #845](https://github.com/bsikar/ra8-firmware/issues/845):
[connectors.h](../../libs/ra8_board_ek_ra8d2/inc/ra8_board_ek_ra8d2_connectors.h)
calls P400 CMD, P401 CLK, P406 WP, P407 CD, and selects
instance 0. Both silicon datasheets instead give the assignment above;
P407 has no SD1CD function. The corresponding pin-init routine and demos
must be corrected and tested in a separate firmware change. Copying that
enum into a schematic would create a real wiring error.

The already-validated SPI fallback uses SCI0 SCK=P601, COPI=P603,
CIPO=P602, CS=P604 on EK Pmod2. It can coexist with SDRAM and the DVP
camera, but adopting it here would trade native-bus performance for earlier
firmware reuse. No switched dual-routing network is proposed for production.

## CMS-004: External RAM and soldered NOR baseline

Both EK boards contain 512 Mbit, 16M x 32 SDRAM, **IS42S32160F-6BLI**.
That is 64 MiB, not 512 MB. EK-RA8P1 uses **MX25LW51245GXDI00** for its
512 Mbit Octal NOR; EK-RA8D2 instead uses **IS25LX512M-JHLE**. Their
capacity is the same but commands, reset behavior and Octal byte ordering
are not interchangeable. See the EK-RA8P1 manual sections 6.3/6.4 and the
[committed EK-RA8D2 manual](../../docs/reference/ek-ra8d2-v1-users-manual.pdf).

The existing imported **IME5132SDBETG-6I** also provides 16M x 32 SDRAM at
3.0..3.6 V, but uses TSOP-86 rather than the EK part's BGA-90. Retaining it
preserves the user's imported part and the required capacity, subject to
pin-by-pin and timing qualification using its actual manufacturer's
[512 Mbit SDRAM datasheet](https://www.mouser.com/datasheet/2/1445/DS_SDRAM_512Mb_16Mx32_IME5132SDBET_B-3600416.pdf).
Do not infer pin equivalence from matching density or run 166 MHz merely
because the memory is rated for it. RA8P1 SDRAMC timing, selected clock,
trace skew, loading, refresh, voltage and temperature are independent limits.
CMS-010 records the current ISSI selection, exact pin-number contract and
remaining electrical qualification gates; CMS-008 retains historical sourcing.

Reserve these SDRAM signal ports, in bit order:

| Signal group | MCU ports |
| --- | --- |
| A0..A12 | PA03 PA02 PA01 PA00 P503 P504 P505 P506 P507 P508 P509 P510 P608 |
| BA0 / BA1 | PD00 / PC15 |
| DQ0..DQ7 | P302 P301 P300 P112 P113 P114 P115 P609 |
| DQ8..DQ15 | PA11 PA12 PA13 PA14 P610 P611 P612 P613 |
| DQ16..DQ23 | PC14 PC13 PC12 PC11 PC10 PC09 PC08 PC07 |
| DQ24..DQ31 | PC06 PC05 PC04 PC03 PC02 PC01 PC00 P607 |
| CKE / CLK | PA06 / PA15 |
| DQM0..DQM3 | P614 PA05 P615 PA04 |
| WE# / CAS# / RAS# / CS# | PA08 / PA09 / PA10 / P813 |

This consumes 57 distinct signal ports. It conflicts with OSPI1 and with
SSI0_B on P112..P115. Use OSPI0 for NOR, and the separate SSI1_A group
for audio. P708 is not in this SDRAM allocation.

The selected NOR is now Infineon S28HL01GTFPBHI030, 1 Gbit / 128 MiB,
3 V Octal DDR with a read data strobe. This doubles the EK capacity without
substituting a Quad device. Reserve OSPI0 as follows: CS#=P104/M6,
CLK=P808/U5, DQS=P801/P6; IO0..IO7=P100/U6, P803/P7, P103/R4,
P101/R5, P102/P5, P800/T6, P802/R6, P804/R7. P104 is OM_0_CS1,
not CS0. INT# uses P105/N7 as GPIO IRQ0, not an assumed ECS# protocol.
Flash RESET# joins the existing MCU_RESET_N wire; P106/N6 is released
from the NOR reservation. Internal MCU watchdog/software resets do not
assert that external wire. CMS-011 below is the controlling pin, passive,
reset, sourcing and qualification contract, including ten 30R series paths.

Existing imported IS25LP01GJ-RHLE is 1 Gbit / 128 MiB Quad NOR, not Octal.
The historical capacity-only comparison Winbond **W25Q512JVFIQ**, 64 MiB
Quad NOR, is not an approved fallback for the selected Octal interface.
Its primary [selection guide](https://www.winbond.com/export/sites/winbond/product-selection-guide/file/2025-Product-Selection-Guide-Winbond-Code-Storage-Flash-Memory.pdf)
confirms 2.7..3.6 V and 133 MHz STR, not Octal/DTR equivalence. The
[manufacturer datasheet, section 3.4](https://www.winbond.com/resource-files/W25Q512JV%20SPI%20RevB%2006252019%20KMS.pdf)
gives SOIC-16 pins: IO3=1, VCC=2, RESET#=3, CS#=7, IO1=8, IO2=9,
GND=10, IO0=15, CLK=16; 4..6 and 11..14 are NC/DNU. This older linked
revision is adequate for candidate identification, not final release:
the manufacturer index lists a newer 2026-05-25 revision requiring review.
It must not be silently substituted in the schematic or firmware.

## CMS-005: Optional eMMC and pin-conflict checks

RA8P1 hardware does support eMMC 4.51 through SDHI, including 1/4/8-bit SDR;
this is explicitly in the [RA8P1 hardware manual](https://www.renesas.com/en/document/mah/ra8p1-group-users-manual-hardware)
and [Renesas SDHI driver documentation](https://renesas.github.io/fsp/group___s_d_h_i.html).
Support is not limited to removable SD. Nevertheless, a modern eMMC's
maximum HS200/HS400 rate is not achievable just because its protocol can
fall back to this controller.

For a separate managed-storage option reserve SDHI0_C in four-bit mode:
CLK PD05/C16, CMD PD04/C14, DAT0 PD03/C15, DAT1 PD02/B17,
DAT2 PD01/B16, DAT3 P111/E8. No overlap with the preceding mandatory buses.
The EK SCI8 console already uses PD02/PD03; move that console if this option
is implemented. The eight-bit extension consumes DAT4 P110, DAT5 P109,
DAT6 P108, DAT7 P206, with DAT7 conflicting with SSI1_A audio data.
No eMMC MPN, supply network or connector is approved by this reservation.

The original EK DVP camera group uses P703/P702/P701/P700/P406/P405/P902/P400
for eight data bits, plus PB02/PB03/PB04 for synchronization/clock. It
therefore directly conflicts with both current radio and native microSD.
These are pin conflicts, not problems solved by software scheduling if the
two external devices remain physically connected without isolation.

## CMS-006: Python-verifiable allocation and capacity arithmetic

Executed using Python's standard library. This proves the listed sets and
arithmetic, not alternate-function register programming or timing closure.

```python
from itertools import combinations

groups = {
    "sdram": "PA03 PA02 PA01 PA00 P503 P504 P505 P506 P507 P508 P509 P510 P608 "
             "PD00 PC15 P302 P301 P300 P112 P113 P114 P115 P609 PA11 PA12 PA13 "
             "PA14 P610 P611 P612 P613 PC14 PC13 PC12 PC11 PC10 PC09 PC08 PC07 "
             "PC06 PC05 PC04 PC03 PC02 PC01 PC00 P607 PA06 PA15 P614 PA05 P615 "
             "PA04 PA08 PA09 PA10 P813",
    "ospi0": "P104 P808 P801 P100 P803 P103 P101 P102 P800 P802 P804 P105",
    "sdhi1": "P400 P401 P402 P403 P404 P405 P406",
    "camera_control": "P512 P511 P709 P501 P010",
    "radio": "P700 P701 P702 P703 P704 P705 P706 P707",
    "audio_ssi1_a": "P907 P906 P206",
    "emmc4_reserved": "PD05 PD04 PD03 PD02 PD01 P111",
    "buttons": "P309 P310 P311 P909 P303",
    "power_control": "P903",
}
sets = {name: set(pins.split()) for name, pins in groups.items()}
assert len(sets["sdram"]) == 57
assert len(sets["ospi0"]) == 12 and "P106" not in sets["ospi0"]
for name, pins in groups.items():
    assert len(pins.split()) == len(sets[name]), name
for a, b in combinations(sets, 2):
    assert not sets[a] & sets[b], (a, b, sets[a] & sets[b])
dvp = set("P703 P702 P701 P700 P406 P405 P902 P400 PB02 PB03 PB04".split())
assert dvp & sets["radio"] == set("P700 P701 P702 P703".split())
assert dvp & sets["sdhi1"] == set("P400 P405 P406".split())
assert {"P110", "P109", "P108", "P206"} & sets["audio_ssi1_a"] == {"P206"}

memory_bytes = 512 * 2**20 // 8
assert memory_bytes == 16 * 2**20 * 32 // 8 == 64 * 2**20
vga_yuv422 = 640 * 480 * 2
full_yuv422 = 2592 * 1944 * 2
assert 2 * full_yuv422 < memory_bytes
pcm_bytes_per_s = 192000 * 24 // 8 * 2
sd_raw_bytes_per_s = 50_000_000 * 4 // 8
print("Memory bytes / MiB", memory_bytes, memory_bytes / 2**20)
print("VGA YUV422 / full sensor YUV422 bytes", vga_yuv422, full_yuv422)
print("192 kHz, 24-bit stereo PCM bytes/s", pcm_bytes_per_s)
print("64 MiB all-PCM theoretical seconds", memory_bytes / pcm_bytes_per_s)
print("4-bit 50 MHz raw bus bytes/s", sd_raw_bytes_per_s)
```

Results: 67,108,864 bytes; VGA frame 614,400 bytes; full-resolution YUV422
frame 10,077,696 bytes; PCM 1,152,000 bytes/s; only 58.254222 seconds if
the historical minimum 64 MiB NOR capacity were used for that PCM; the
selected 128 MiB NOR is checked separately in CMS-011. The native SD bus
theoretical payload is 25,000,000 bytes/s before protocol overhead and media stalls.
Two large frames fitting RAM does not prove capture frame rate, CPU/cache
coherency, DMA arbitration, ISP throughput or available application heap.
Audio buffering must cover real card latency, not this raw bus-rate quotient.

## CMS-007: Procurement snapshots and release checklist

Retrieved 2026-09-07, USD, excluding tax/shipping; availability is not a
reservation. Search-index quantities older than the direct page were not
treated as current stock.

| Exact part | Source | Observed stock | Unit USD at 1 / 10 | Status |
| --- | --- | ---: | --- | --- |
| DM3AT-SF-PEJM5 | [DigiKey HR1964CT-ND](https://www.digikey.com/en/products/detail/hirose-electric-co-ltd/DM3AT-SF-PEJM5/2533566) | 30,866 | 3.55 / 3.019 | Existing socket candidate |
| W25Q512JVFIQ | [DigiKey W25Q512JVFIQ-ND](https://www.digikey.com/en/products/detail/winbond-electronics/W25Q512JVFIQ/10244707) | 1,807 | 15.69 / 14.56 | Quad fallback; purchase limits, no backorders |
| MX25LW51245GXDI00 | [DigiKey 1092-MX25LW51245GXDI00-ND](https://www.digikey.com/en/products/detail/macronix/MX25LW51245GXDI00/18110053) | 0 | 15.86 / 14.717 | Exact EK-RA8P1 Octal part; sourcing hold |
| IS25LX512M-JHLE | [DigiKey 706-IS25LX512M-JHLE-ND](https://www.digikey.com/en/products/detail/issi-integrated-silicon-solution-inc/IS25LX512M-JHLE/16529389) | 0 | 16.04 / 14.884 | Exact EK-RA8D2 Octal part; sourcing hold |
| IS25LP01GJ-RHLE | [DigiKey 706-IS25LP01GJ-RHLE-ND](https://www.digikey.com/en/products/detail/issi-integrated-silicon-solution-inc/IS25LP01GJ-RHLE/24617301) | 0 | 20.02 / 18.565 | Imported 128 MiB Quad; sourcing hold |
| IME5132SDBETG-6I | [DigiKey 5107-IME5132SDBETG-6I-ND](https://www.digikey.com/en/products/detail/intelligent-memory-ltd/IME5132SDBETG-6I/21801792) | 2 | 31.42 / 29.088 | Existing RAM; refreshed direct page, 26-week lead time |

For IME RAM, Mouser part 822-IME5132SDBETG-6I also exists; the observed
139-stock category result was older cached data, not a verified current US
snapshot. Refresh through the user's distributor browser before BOM release.
CU450 availability and the final camera connector/cable remain open.

Before marking the native sections complete: resolve those source gates;
verify every selected memory's symbol pins and supply domains; add local
bypass, startup/reset and pull networks with calculation IDs; check rail
inrush and hard-off behavior; reconcile this allocation against every other
sheet; perform schematic visual review and ERC. The GPIO set test excludes
unallocated power-control outputs, touch, e-paper, frontlight and additional
audio control nets, so it is not a full-project conflict signoff.

Suggested schematic note: `CMS-001..010: design/camera_storage_interfaces.md`.
Place the relevant local calculation/result beside each functional block,
not a large unrelated wall of text on the root sheet.

## CMS-008: Stocked single-device SDRAM alternative

Recommend **Alliance AS4C16M32SC-7TIN** as the sourced schematic candidate,
not as a pin-compatible replacement for the EK's BGA90 device. It preserves
64 MiB, x32, four-bank, 13-row/9-column organization. CMS-010 contains the
completed 86-pin identity audit; this record does not change the native BOM.
The existing IME part's two-unit DigiKey stock is not a robust sourcing basis.

Fresh direct-page snapshots, 2026-09-07, USD, excluding tax/shipping:

| Exact MPN | Distributor | Stock | Unit price 1 / 10 | Factory lead time |
| --- | --- | ---: | --- | --- |
| AS4C16M32SC-7TIN | [DigiKey 1450-1468-ND](https://www.digikey.com/en/products/detail/alliance-memory-inc/AS4C16M32SC-7TIN/9681183) | 178 | 32.99 / 30.54 | 16 weeks |
| AS4C16M32SC-7TIN | [Mouser 913-AS4C16M32SC-7TIN](https://www.mouser.com/ProductDetail/Alliance-Memory/AS4C16M32SC-7TIN?qs=qSfuJ%252Bfl%2Fd6SYuToPnq%2F9w%3D%3D) | 29 | 32.99 / 30.54 | 16 weeks |
| AS4C16M32SB-6BCN | [DigiKey 1450-AS4C16M32SB-6BCN-ND](https://www.digikey.com/en/products/detail/alliance-memory-inc/AS4C16M32SB-6BCN/25902539) | 364 | 31.12 / 28.811 | 16 weeks |
| AS4C16M32SB-6BCN | [Mouser 913-AS4C16M32SB-6BCN](https://www.mouser.com/en/ProductDetail/Alliance-Memory/AS4C16M32SB-6BCN?qs=3vio67wFuYob2ya%252BUS0X4g%3D%3D) | 468 | 31.12 / 28.82 | 16 weeks |

Use the SC datasheet's **7.5 ns minimum CL3 period / 133 MHz** rating, not
the 143 MHz family-catalog entry. Its VDD/VDDQ are 3.0..3.6 V LVTTL;
industrial ambient rating is -40..85 C. Input limits are VIH >=2.0 V and
VIL <=0.8 V; output guarantees are VOH >=2.4 V and VOL <=0.4 V at 4 mA.
Full-temperature self-refresh IDD6 is <=5 mA; x32 operating/burst/refresh
maxima are 70/90/170 mA under their distinct datasheet test conditions.
Those currents are not additive modes. Capacitive I/O switching adds load.
[Alliance SC primary datasheet, Rev.1.0, Tables 1/2/10/12/13](https://www.alliancememory.com/wp-content/uploads/AllianceMemory_512M-SDRAM_Cdie_AS4C16M32SC-AS4C32M16SC-AS4C64M8SC-7TIN_Sept2018_rev1.0.pdf).

The SB alternative is 90-ball, 8 x 13 mm BGA, 3.0..3.6 V, CL3/166 MHz;
the actually stocked BCN grade is only 0..70 C. Its Table 15 specifies
**60 mA maximum self-refresh**, visually verified as mA rather than uA.
Its larger inventory therefore does not justify selecting it for retention
sleep. At nominal 3.3 V, the RAM-only worst-case retention allocations are
16.5 mW for SC versus 198 mW for SB, a factor of 12. At 3.6 V they become
18 mW and 216 mW. Neither is an ultra-low-power retained-memory promise.
[Alliance SB primary datasheet, Rev.1.0, February 2023](https://www.alliancememory.com/wp-content/uploads/AllianceMemory_512Mb_AS4C16M32SB-6BxN_Datasheet_16Feb2023_ver1.0.pdf).

The existing [SDRAM HAL timing definitions](../../libs/ra8_hal/src/ra8_sdramc.c)
document 125 MHz, CL3, RAS=6, RCD=4, RP=4, WR=2 cycles,
refresh recovery=12 cycles and refresh interval=900 cycles. The following
checks those documented nominal durations against SC minimums. It does
not independently decode MCU registers or prove read/write setup, hold,
clock skew, signal integrity, mode-register recovery or self-refresh exit.

```python
from math import isclose

sdclk_hz = 125_000_000
tck_ns = 1e9 / sdclk_hz
cycles = {"tRAS": 6, "tRCD": 4, "tRP": 4, "tWR": 2, "tRFC": 12}
sc_min_ns = {"tRAS": 44, "tRCD": 15, "tRP": 15, "tWR": 15, "tRFC": 66}
assert tck_ns >= 7.5
for name, count in cycles.items():
    actual_ns = count * tck_ns
    assert actual_ns >= sc_min_ns[name], name
    print(name, "nominal ns", actual_ns, "margin ns", actual_ns - sc_min_ns[name])
assert (cycles["tRAS"] + cycles["tRP"]) * tck_ns >= 66  # same-bank tRC
refresh_us = 900 / sdclk_hz * 1e6
assert refresh_us <= 7.8  # datasheet conservative rounded interval
assert 8192 * 900 / sdclk_hz <= 64e-3
assert 16 * 2**20 * 32 // 8 == 64 * 2**20
for volts in (3.3, 3.6):
    print("retention mW at V", volts, "SC", volts * 5, "SB", volts * 60)
assert isclose(60 / 5, 12)
print("refresh us", refresh_us, "8192-row sweep ms", 8192 * 900 / sdclk_hz * 1e3)
```

Executed results: nominal durations RAS/RCD/RP/WR/RFC = 48/32/32/16/96 ns;
margins = 4/17/17/1/30 ns. Refresh interval = 7.2 us and full sweep =
58.9824 ms. The 1 ns write-recovery margin deserves explicit clock and
controller-encoding review; nominal arithmetic is not production signoff.
Keep VDD and VDDQ in the same sequenced 3.3 V domain and audit every supply
pin and bypass location. Do not power-gate RAM while MCU outputs remain
driven. Hard-off loses RAM; retained sleep requires correct self-refresh
entry/exit and an always-retained supply. Capacity equivalence does not
establish evaluation-board timing or firmware equivalence.

The retrieved SC document still labels its revision preliminary; confirm
the contractual current specification with Alliance for production release.
For this schematic phase, it is the best verified stocked industrial
single-x32 candidate found, not a claim of abundant long-term supply.

## CMS-009: Five exposed controls and wake allocation

This implements the allocation basis for the owner's five-button requirement
in [ereader_requirements.md](ereader_requirements.md), issues #821/#832.
Native checkpoint, 2026-09-07: page 8,
[user_controls.kicad_sch](../ereader/user_controls.kicad_sch), now contains
the four placed and locally wired page/volume circuits: 20 purchased parts,
12 GND symbols, four +3V3_MCU pullup connections and four output hierarchical
labels. The native XML export independently confirms the local connectivity.
Both visible CMS-009 calculation notes are now drawn and link back to this
record. Matching MCU-leaf Input labels/wires and all four root routes are
complete. Independent KiCad 10.0.5 XML export verifies each filtered node
through the hierarchy to exactly its assigned P309/A12, P310/E10, P311/B12
or P909/B14 pin. All eight native sheets are accessible and every root
sheet's port names/directions match its child labels. This closes the
four-key schematic-connectivity checkpoint, not firmware wake validation
or hardware qualification.

The full-project ERC run on 2026-09-07 at 21:22 local time reports 204
errors and two warnings, with zero findings on page 8 or the six assigned
MCU control pins. The whole project is not ERC-clean: 197 unconnected-pin
errors, four undriven-power errors, three VLO output-conflict errors and
two radio-label warnings remain. The run included error, warning and
exclusion severities; ignored project checks were not silently re-enabled.
The native GUI review additionally displays 15 excluded warnings: 17
warnings including exclusions and 221 total displayed findings. Those
excluded findings are not counted as fixed by the four-key work.
These findings must remain visible while the remaining sections are built.

Reciprocal native note identifiers are:

- `CMS-009 - FOUR INDEPENDENT PAGE / VOLUME KEYS`, UUID
  `0a046f20-7e8a-4e51-abb0-1f079de7b531`: references, DC corner assumptions,
  threshold/contact equations and held-key current.
- `CMS-009 - FILTER TIMING AND BUTTON BEHAVIOR`, UUID
  `6bf7c991-6015-499e-997f-02b0f6403ffd`: conditional capacitance range,
  nominal RC equations, debounce, boot/held-key behavior and ESD limits.

| Key / output net | Switch | Series, 1k | Pullup, 10k | Filter, 100n | TVS |
| --- | --- | --- | --- | --- | --- |
| PAGE_PREV_N | SW2 | R23 | R27 | C58 | D2 |
| PAGE_NEXT_N | SW3 | R24 | R28 | C59 | D3 |
| VOL_DOWN_N | SW4 | R25 | R29 | C60 | D4 |
| VOL_UP_N | SW5 | R26 | R30 | C61 | D5 |

These are the native references for every calculation in CMS-009 below.
SW2..SW5 pin 2, D2..D5 pin 1 and R23..R26 pin 1 form each protected
external node. R23..R26 pin 2, R27..R30 pin 2, C58..C61 pin 1 and the
corresponding assigned U1 pin form each filtered output node. Pullup pin 1
goes to +3V3_MCU; switch pin 1,
capacitor pin 2 and TVS pin 2 go to GND. No external or filtered node is
shared between keys. This pin-number mapping is from KiCad's XML netlist,
not an assumption based on the rotated resistor's appearance.

The four matching root routes and exact end-to-end netlist checks pass.
The full PDF and BOM have been refreshed, with native visual review of all
eight PDF pages. Preserve the verified routes and keep these exports
synchronized during subsequent work.
U1's P309_IN/P310_IN/P311_IN
and P909_IN selected pin functions already model the intended Input types;
the reusable default GPIO types are Bidirectional. Symbol functions do not
configure the MCU's firmware pin routing or prove wake behavior.

| Control / net | Port / BGA289 ball | External interrupt | Deep-standby enable bit |
| --- | --- | --- | --- |
| Previous page / PAGE_PREV_N | P309 / A12 | IRQ25-DS | DPSIER5.DIRQ25E, bit 1 |
| Next page / PAGE_NEXT_N | P310 / E10 | IRQ24-DS | DPSIER5.DIRQ24E, bit 0 |
| Volume down / VOL_DOWN_N | P311 / B12 | IRQ23-DS | DPSIER4.DIRQ23E, bit 7 |
| Volume up / VOL_UP_N | P909 / B14 | IRQ21-DS | DPSIER4.DIRQ21E, bit 5 |
| Power-controller INT / POWER_BUTTON_N | P303 / B6 | IRQ29-DS | DPSIER5.DIRQ29E, bit 5 |

Evidence: RA8P1 datasheet Table 1.17 and
[Hardware Manual R01UH1064EJ0130](https://www.renesas.com/en/document/mah/ra8p1-group-users-manual-hardware),
Tables 21.2/21.10/21.16, pp.849/869/877. All five use the VCC domain,
here switched +3V3_MCU. The four direct-key inputs are non-5V-tolerant;
P303 is 5V-tolerant, but its pullup still belongs to +3V3_MCU. Datasheet
Tables 2.5/2.7 give Schmitt limits 0.8*VCC high and 0.2*VCC low, with
1 uA off-state leakage for the direct-key pins and 5 uA for P303.

P312 was rejected despite its C13 entry in Table 1.17: HUM Table 21.10
visually marks it unavailable for MIPI289. P909 avoids relying on that
unresolved document conflict. Do not confuse the e-reader with EK header
routing. The GPIO test in CMS-006 includes these reservations. IRQ channels
also remain distinct from camera IRQ14 and radio IRQ19/26. Alternate IRQ
functions on SDRAM, debug and other peripheral pins must remain disabled.

The four placed direct-key circuits share this topology:

```text
+3V3_MCU -- 10k --+-- PAGE_PREV_N (or other allocated MCU input)
                 +-- 100nF -- GND
                 +-- 1k -- KEY_EXT -- normally-open switch -- GND
                          +-- ESD441DPYR pin 1; pin 2 -- GND
```

The placed parts use EVQP7A01P switches, RC0603FR-0710KL pullups,
RC0603FR-071KL series resistors and C1608X7R1H104K080AA capacitors from
[BTN-003/006](single_button_power.md). Disable MCU internal pulls; their
10..300 uA spread is not the resistor calculation. The external defaults
also exist during reset. No key is a boot strap, resistor ladder or matrix;
all four can be pressed simultaneously without ghosting or output contention.
The power switch retains its independent LTC2954 PB circuit and internal
reset/boot service pads remain separate. Do not wire that switch directly
to P303 or tie any of these keys to MD, radio BOOT, EN or KILL.
The separate shutdown output POWER_KILL_N uses P903/D9;
[BTN-009](single_button_power.md#btn-009-actual-ra8p1-interrupt-and-shutdown-pins)
contains the open-drain initialization and 70 mV guaranteed DC low-margin check.

D2..D5 place one **ESD441DPYR** at each direct key's external node, before
its 1k resistor. Its ground-only protection has no supply-rail connection that
could bypass hard-off. TI SLVSH26B section 5.6 explicitly specifies
<100 nA across temperature within the +/-5.5 V stand-off range; do not
substitute the 25 C typical leakage figure. The pin map is 1=IO, 2=GND.
[TI ESD441 datasheet](https://www.ti.com/lit/ds/symlink/esd441.pdf).
Fresh 2026-09-07 stock: [DigiKey 296-ESD441DPYRCT-ND](https://www.digikey.com/en/products/detail/texas-instruments/ESD441DPYR/28715599)
3,273, USD 0.34/0.207 at 1/10; [Mouser 595-ESD441DPYR](https://www.mouser.com/ProductDetail/Texas-Instruments/ESD441DPYR?qs=bpu3f%2FCR1jziUA14lbLOFw%3D%3D)
11,380, USD 0.34/0.149 at 1/10, both 9-week factory lead time.
Its component IEC rating is not enclosure certification. PCB ESD-current
return, resistor pulse stress and the residual MCU waveform require later
physical verification; do not claim the TVS clamps to 3.3 V.

The arithmetic below screens +3V3_MCU=3.0..3.6 V, resistors +/-1% plus
100 ppm/C over 100 C, and 0.5 Ohm initial contact resistance. Per direct key,
allocate 1 uA MCU +0.1 uA TVS +0.1 uA capacitor +1 uA board leakage.
The capacitor/board terms are design allowances, not manufacturer maxima.

```python
from itertools import product
from math import log

rp = (10000 * .99 * .99, 10000 * 1.01 * 1.01)
rs = (1000 * .99 * .99, 1000 * 1.01 * 1.01 + .5)
vmin, vmax, leakage = 3.0, 3.6, 2.2e-6
cases = list(product((vmin, vmax), rp, rs))
closed = lambda v, r, s: (v/r + leakage) / (1/r + 1/s)
vlo_max = max(closed(*case) for case in cases)
low_margin = min(.2*v - closed(v, r, s) for v, r, s in cases)
vhi_min = vmin - leakage * rp[1]
external_high_min = vhi_min - .1e-6 * (rs[1] - .5)
contact_min = vmin/(rp[1]+rs[1]) - leakage
four_held_max = 4 * (vmax/(rp[0]+rs[0]) + leakage)
assert low_margin > 0 and vhi_min > .8*vmin
assert contact_min > 10e-6 and external_high_min > 2
assert vmax/rs[0] < 50e-3  # capacitor discharge/contact peak screen
assert vlo_max < .341555 and low_margin > .31503
assert vhi_min > 2.97755 and contact_min > 265.14e-6
assert external_high_min > 2.97745 and four_held_max < 1.345e-3
print("low max / low margin / high min V", vlo_max, low_margin, vhi_min)
print("external open-contact minimum V", external_high_min)
print("contact min uA / four held max mA", contact_min*1e6, four_held_max*1e3)
print("nominal RC assert/release ms",
      -(10000*1000/11000)*100e-9*log((.2-1/11)/(1-1/11))*1e3,
      -10000*100e-9*log((1-.8)/(1-1/11))*1e3)
channels = [25, 24, 23, 21, 29, 14, 19, 26]
assert len(channels) == len(set(channels))
assert sum(1 << (irq-16) for irq in (21, 23)) == 0xA0
assert sum(1 << (irq-24) for irq in (24, 25, 29)) == 0x23
power_high_min = vmin - (5+2+1)*1e-6*rp[1]
power_low_margin = .2*vmin - .4
power_sink_max = vmax/rp[0] + (5+1)*1e-6
assert power_high_min > .8*vmin and power_low_margin > 0
assert power_sink_max < 3e-3
print("power high min / low margin V / INT sink max mA",
      power_high_min, power_low_margin, power_sink_max*1e3)
```

Executed, with outward-rounded bounds: closed <0.341555 V; correlated
minimum low margin >0.31503 V; filtered input VHIGH >2.97755 V;
contact >265.14 uA; four held <1.345 mA. The exact screened input-high
value is 2.9775578 V and contact current is 265.141555... uA; do not round
these lower bounds upward inside a >= claim. The exposed open-contact
voltage is a different node: allowing the TVS's 0.1 uA through the maximum
series resistance gives 2.97745579 V, hence >2.97745 V and above 2 V.
Nominal RC threshold delays are 0.193 ms assertion / 1.514 ms release.
These are noise filtering, not complete debounce: require 20 ms continuously
stable state per key before delivering a press/release. The switch specifies
10 ms bounce. RC timing is nominal; debounce must not depend on a precise
X7R capacitance. Opposing page or volume keys cancel their corresponding
action until one is released; firmware limits volume to the qualified audio
range. Reset/update may ignore ordinary actions until all keys release.

**Wake contract:** retained-rail Sleep uses each routed IRQ; Software Standby
also needs WUPEN configuration. Hardware disables the ordinary PCLKB IRQ
filter in Software Standby (HUM 14.5.6), so retain the external RC and debounce
after wake. Deep Sleep uses the allocated IELSR/NVIC slot's DSLPWUPIRQEN
bit, not blindly the external IRQ number. Deep Software Standby is a reset
wake: preserve/capture the wake cause and reconstruct state. For all five
keys, enable DPSIER4 bits 5/7 and DPSIER5 bits 0/1/5; select falling edges
by clearing the corresponding DPSIEGR3/4 bits. Preserve unrelated sources.
After modifying enables, observe the manual's six-PCLKB-cycle wait and
read-before-zero flag clearing (11.2.33/34). Recheck inputs and pending
events when entering sleep; do not enter edge-wake standby with a key already
held. Read, debounce and wait for release after reset so one held key does
not create repeated actions. These are firmware requirements, not changes
to the current firmware or proof that SDRAM is retained in every mode.

In hard-off, the four direct keys do nothing: no live pullups or energy source
remain in their circuit. Only the power controller starts the rails. In a
firmware freeze, page/volume cannot recover the system; the held power key
still forces off independently. POWER_BUTTON_N's existing 10k pullup to
the switched rail accepts LTC2954 INT <=0.4 V at 3 mA with >=0.2 V low
margin at VCC=3.0 V; a conservative sink requirement is <0.374 mA. Its
high-state allowance of 5 uA P303 +2 uA INT +1 uA board gives >=2.918392 V.
The 2 uA INT allowance exceeds the published 1 uA test at 3 V; it is not
a new manufacturer guarantee at every voltage. Keep P303 input-only and
use the controller's debounce, not the four direct-key RC for the PB timer.
The visible schematic math notes use calculation ID `CMS-009` and link to
`../design/camera_storage_interfaces.md`. Their SW2..SW5, R23..R26,
R27..R30, C58..C61 and D2..D5 references match the reciprocal table above.
The notes show the voltage/leakage/tolerance assumptions, threshold/contact
equations and nominal RC calculations, not only component values. Keep
these two notes and this record synchronized when values or assumptions
change; their presence is not hardware qualification.

## CMS-010: IS42S32160F-7TLI pin and electrical contract

Native SDRAM interconnect and reset-default checkpoint, 2026-09-08, for
[memory issue #827](https://github.com/bsikar/ra8-firmware/issues/827).
This section records native SDRAM implementation and its electrical
contract, not electrical qualification or fabrication approval. It does not
change firmware, ERC settings or the existing CMS-009 controls checkpoint.
The selected part is ISSI
IS42S32160F-7TLI; this supersedes the CMS-008 historical Alliance candidate
for implementation without rewriting that sourcing history. Footprint geometry
is outside this pin-number audit. Page 10 contains the placed memory units,
completed power/bypass wiring and all 57 signal connections through the
root hierarchy to page 2. The ten command/clock pullups and all 57 selected
MCU electrical pin roles are now implemented. Startup, signal-integrity,
timing, power-distribution and shutdown qualification remain open.

Primary evidence:

- [ISSI-authored Rev. C, 2025-02-26, distributor-hosted copy](https://www.mouser.com/datasheet/3/3722/1/42_45R_S_32160F.pdf):
  p3 (TSOP86 pinout), p14 (DC), p15 (current/capacitance), pp16-18
  (AC limits and test conditions), pp19-20 (initialization), and p58
  (exact industrial TSOP ordering code). The manufacturer-hosted
  [family URL](https://www.issi.com/WW/pdf/42-45R-S-32160F.pdf) retrieved
  during this review served older Rev. B, not the cited Rev. C. A second
  [distributor-hosted Rev. C copy](https://www.farnell.com/datasheets/4555730.pdf)
  corroborates the current initialization and AC-test text.
- [RA8P1 datasheet R01DS0439EJ0130](https://www.renesas.com/en/document/dst/ra8p1-group-datasheet):
  Table 1.17 (MIPI-enabled BGA289 pin functions), Tables 2.3/2.4/2.7
  (supply and logic levels), Table 2.50 (SDCLK waveform limits), and
  Table 2.57 / Figures 2.45-2.51 (SDRAM timing).
- [RA8P1 HUM R01UH1064EJ0130](https://www.renesas.com/en/document/mah/ra8p1-group-users-manual-hardware):
  section 9.2.32 p354 (disabled SDCLK is high), section 9.10.10
  (clock selection), sections 15.3.18/19 pp613-614 (initialization and
  address shift), section 15.6.6 (self-refresh), section 15.6.11.1 p671
  (controller initialization), and Table 15.38 pp680-681 (address mapping).
- [EK-RA8P1 Rev.1.04 Table 30 pp36-37](https://www.renesas.com/en/document/mat/ek-ra8p1-v1-users-manual)
  corroborates the port reservation, but does not establish ISSI
  timing or its TSOP pin numbers.

### CMS-010A: Complete pin-number map

The 57 signal rows below match CMS-004 and the current project MCU library
pin identities. The memory A0 pin receives MCU external-bus A02, not A00.
MCU A02..A14 connect to memory A0..A12; MCU A15/A16 connect to BA0/BA1.
For x32, select the nine-bit row-address shift, `SDADR.MXC=01`, and 32-bit
bus width. Memory A10/AP receives MCU A12's precharge-select function.
There are 13 row bits, nine column bits, four banks, and four bytes/word:
`8192 * 512 * 4 * 4 = 67108864 bytes = 64 MiB`.

| Signal | ISSI pin | MCU port | U1 ball |
| --- | ---: | --- | --- |
| CLK | 68 | PA15 | E1 |
| CKE | 67 | PA06 | C1 |
| CS# | 20 | P813 | B1 |
| RAS# | 19 | PA10 | F2 |
| CAS# | 18 | PA09 | F4 |
| WE# | 17 | PA08 | F3 |
| BA0 | 22 | PD00 | K4 |
| BA1 | 23 | PC15 | K1 |
| A0 | 25 | PA03 | G2 |
| A1 | 26 | PA02 | F1 |
| A2 | 27 | PA01 | H4 |
| A3 | 60 | PA00 | G1 |
| A4 | 61 | P503 | H2 |
| A5 | 62 | P504 | H1 |
| A6 | 63 | P505 | H3 |
| A7 | 64 | P506 | J1 |
| A8 | 65 | P507 | J2 |
| A9 | 66 | P508 | J3 |
| A10/AP | 24 | P509 | J4 |
| A11 | 21 | P510 | K3 |
| A12 | 69 | P608 | K2 |
| DQM0 | 16 | P614 | E3 |
| DQM1 | 71 | PA05 | G3 |
| DQM2 | 28 | P615 | E2 |
| DQM3 | 59 | PA04 | D1 |
| DQ0 | 2 | P302 | A5 |
| DQ1 | 4 | P301 | C4 |
| DQ2 | 5 | P300 | B5 |
| DQ3 | 7 | P112 | A4 |
| DQ4 | 8 | P113 | A2 |
| DQ5 | 10 | P114 | B3 |
| DQ6 | 11 | P115 | A3 |
| DQ7 | 13 | P609 | A1 |
| DQ8 | 74 | PA11 | B4 |
| DQ9 | 76 | PA12 | B2 |
| DQ10 | 77 | PA13 | C3 |
| DQ11 | 79 | PA14 | D4 |
| DQ12 | 80 | P610 | D3 |
| DQ13 | 82 | P611 | D2 |
| DQ14 | 83 | P612 | E4 |
| DQ15 | 85 | P613 | C2 |
| DQ16 | 31 | PC14 | F5 |
| DQ17 | 33 | PC13 | J5 |
| DQ18 | 34 | PC12 | G5 |
| DQ19 | 36 | PC11 | H5 |
| DQ20 | 37 | PC10 | M5 |
| DQ21 | 39 | PC09 | L4 |
| DQ22 | 40 | PC08 | M4 |
| DQ23 | 42 | PC07 | K5 |
| DQ24 | 45 | PC06 | N4 |
| DQ25 | 47 | PC05 | L5 |
| DQ26 | 48 | PC04 | L3 |
| DQ27 | 50 | PC03 | L1 |
| DQ28 | 51 | PC02 | L2 |
| DQ29 | 53 | PC01 | M3 |
| DQ30 | 54 | PC00 | M1 |
| DQ31 | 56 | P607 | M2 |

| Supply or unused group | ISSI pins | Connection |
| --- | --- | --- |
| VDD | 1, 15, 29, 43 | +3V3_MCU |
| VDDQ | 3, 9, 35, 41, 49, 55, 75, 81 | Same +3V3_MCU |
| VSS | 44, 58, 72, 86 | GND |
| VSSQ | 6, 12, 32, 38, 46, 52, 78, 84 | GND |
| NC | 14, 30, 57, 70, 73 | Explicit no-connect |

All 86 numbers were checked against ISSI Rev. C p3 and compared with the
existing `Memory:IME5132SDBETG-6I` symbol:
57 signal, 12 power, 12 ground and five NC identities match. A distinct
ISSI symbol `Memory:IS42S32160F-7TLI` now reuses that four-unit drawing
through native Save As, with separate sourced identity, datasheet and BOM
fields and no inherited IME on-die-ECC claim. The native symbol has 25
control/address Input pins, 32 Bidirectional DQ pins, 24 Power input
supply/ground pins and five Not connected pins. Native Symbol Checker
reported no issues. All four U14 units are now placed. The 12 supply pins
connect to +3V3_MCU, the 12 ground pins connect to GND, and the five NC
pins remain isolated. Those power/bypass and NC connections are unchanged
from the preceding checkpoint. Native XML now verifies all 56 direct U1-U14
signal pairs against the table above. CLK is the remaining signal:
U1.E1 and R43.2 form `SDRAM_CLK_SRC`; R43.1 and U14.68 form `SDRAM_CLK`.
The populated 0R link preserves separate source and load nets.

Each leaf now has four vector hierarchical labels,
`SDRAM_A[0..12]`, `SDRAM_BA[0..1]`, `SDRAM_DQ[0..31]` and
`SDRAM_DQM[0..3]`, plus six scalar labels: `SDRAM_CLK`, `SDRAM_CKE`,
`SDRAM_CS_N`, `SDRAM_RAS_N`, `SDRAM_CAS_N` and `SDRAM_WE_N`.
All ten root sheet-pin connections join the matching leaf labels; the
whole-project XML contains 284 nets. No memory signal is marked no-connect.

The checkpoint CLI ERC reports 145 errors and two active warnings, with
zero findings on page 10 and no SDRAM-related root findings. All 147 active
finding identities are unchanged by the MCU pin-role and pullup work.
The native GUI shows 145 errors and 17 warnings, including the same 15 existing excluded
warnings; all four ignored checks are unchanged. No new exclusion or rule
waiver was used to obtain these counts. The project is not ERC-clean.
The native BOM contains 63 groups, 173 components and 18 columns, including
R43 and one quantity-ten row for R44-R53. Reciprocal CMS-010 notes are
present on pages 2 and 10.

All 57 selected MCU signal pins now have the intended electrical types:
25 Output address/control/clock pins and 32 Bidirectional DQ pins. The
project symbol and all four embedded MCU definitions are updated.
Symbol pin typing is an ERC model, not firmware pin configuration or
dynamic driver qualification. R44-R53 implement the ten 10k command/clock
pullups specified below. This checkpoint does not establish full-circuit
ERC acceptance or electrical qualification.

### CMS-010B: Shared supply, pull and bypass basis

Use the same switched +3V3_MCU for VDD, VDDQ and their pullups. Both MCU VCC
and VCC2 must remain in the 3.3 V domain: DQ0..19 use VCC and DQ20..31 use
VCC2. The 3.0..3.6 V memory range is narrower than the MCU's general
operating range. No separately powered probe or peripheral may inject this
bus during hard-off. The PWR-003 250 mA memory allocation remains a design
limit to qualify, not a proven worst-case consumption bound: ISSI's -7 IDD4
maximum is 210 mA with outputs open, before external I/O charging current.
Do not scale that table limit linearly with clock rate or omit bus loading.
Include all added capacitors in SYS-007's main-rail
discharge-capacitance budget. [Main digital power basis](power_decoupling.md).

The placed default network is ten separate 10k pullups on CKE, DQM0..3,
CS#, RAS#, CAS#, WE# and CLK. Every resistor pin 1 connects to switched
+3V3_MCU; its pin 2 connects to the signal in this native reference map:

| Reference | Signal net | U14 pin |
| --- | --- | ---: |
| R44 | SDRAM_CKE | 67 |
| R45 | SDRAM_DQM0 | 16 |
| R46 | SDRAM_DQM1 | 71 |
| R47 | SDRAM_DQM2 | 28 |
| R48 | SDRAM_DQM3 | 59 |
| R49 | SDRAM_CS_N | 20 |
| R50 | SDRAM_RAS_N | 19 |
| R51 | SDRAM_CAS_N | 18 |
| R52 | SDRAM_WE_N | 17 |
| R53 | SDRAM_CLK | 68 |

R53 is on R43's memory side, not `SDRAM_CLK_SRC`. The five CKE/DQM pulls
preserve the required high states while MCU pins are inputs; CS# high
inhibits commands. The
other control pulls avoid floating command inputs, and CLK high matches
the peripheral's disabled-clock polarity. Do not substitute a CKE pull-down
or rely on firmware-enabled internal pullups during reset. These pulls do
not supply the missing power-on clock or prove initialization by themselves.

All ten parts are YAGEO RC0603FR-0710KL, DigiKey 311-10.0KHRCT-ND,
with native value `10k` and footprint geometry deferred. The verified
2026-09-08 [DigiKey sourcing snapshot](https://www.digikey.com/en/products/detail/yageo/RC0603FR-0710KL/729827)
is 2,866,522 in stock, USD 0.10 / 0.025 / 0.0122 at quantities 1 / 10 / 100;
availability is not reserved. The part is 0603, +/-1%, +/-100 ppm/C,
0.1 W at 70 C, with a -55..155 C operating range and 75 V maximum working
voltage; the power/temperature derating requirement still applies.
Screen +/-1% initial tolerance and +/-100 ppm/C over a conservative 100 C
change, as in BTN-006:
`Rmin = 10000 * 0.99 * 0.99 = 9801 Ohm` and
`Rmax = 10000 * 1.01 * 1.01 = 10201 Ohm`.
ISSI input leakage is +/-5 uA; the selected non-5V-tolerant MCU ports
have +/-1 uA off-state leakage. Add 1 uA board leakage as a qualification
allocation, not a vendor guarantee. For a tristated MCU input,
`Ioff = (5 + 1 + 1) uA = 7 uA`, hence
`VHIGHmin = 3.0 - 7e-6 * 10201 = 2.928593 V`.
For an actively LOW MCU output, its input-leakage term is not added again:
`Isink = 3.6 / 9801 + (5 + 1)e-6 = 0.373309458... mA`, below the
ordinary control pins' 1 mA DC test condition. The CLK pullup is selected
for the MCU's reset/disabled-clock polarity, not a pull-down leakage failure.
The CLK output itself is PA15's high-speed drive class and needs the
separate waveform qualification below, not the generic control-pin VOL proof.
[Yageo RC0603FR-0710KL specification](https://www.yageogroup.com/component-documentation/download/specsheet/RC0603FR-0710KL).

The conservative resistor stress uses the full 3.6 V across Rmin, without
credit for a nonzero GPIO low voltage:
`Pmax = 3.6^2 / 9801 = 1.322314049... mW` per resistor.
For all ten signals LOW at once, the pull-resistor rail load is
`10 * 3.6 / 9801 = 3.673094582... mA`, and their total heat is
`10 * 3.6^2 / 9801 = 13.223140495... mW`.
Including the memory/board leakage terms in the conservative sink sum gives
`10 * Isink = 3.733094582... mA`. These are static screens, not estimates
of normal command duty cycle or memory switching current. Reserve 4 mA
inside the existing PWR-003 250 mA SDRAM allocation, leaving 246 mA for
the memory and dynamic I/O qualification; do not increase the main
1.65 A allocation silently. The remaining 246 mA is an allocation, not a
verified worst-case bound. All screens require the stated leakage,
temperature and resistor bounds; they do not establish lifetime drift,
signal integrity or board thermal qualification.

These ten pullups introduce no always-on source or intentional capacitance:
their rail is the same switched +3V3_MCU as MCU VCC/VCC2 and memory
VDD/VDDQ. Do not count them as guaranteed shutdown discharge paths when
MCU pins tristate, or permit an externally powered probe to inject the bus
during hard-off. Retained self-refresh must actively hold CKE LOW against
R44, adding up to `3.6 / 9801 = 0.367309458... mA` resistor current.
Releasing that drive lets the pullup change the intended retention state.
Hard-off still discards RAM; startup, retention and shutdown qualification
remain separate from the completed passive interconnections.

Page 10's completed bypass wiring is C76-C87, 12 x 100nF, one for each
VDD/VDDQ pin, plus C88, 10uF local bulk, with no separate filter or load
switch splitting VDDQ. The parts are C1608X7R1H104K080AA and
C3216X7R1V106K160AC. Their nominal total is 11.2uF, within the main-rail
capacitance budget; effective capacitance and power-distribution impedance
are not qualified by the nominal sum or schematic wiring.
These are engineering starting values, not an ISSI capacitance minimum
or proof of effective capacitance, impedance or transient response. Qualify
bias, temperature, aging and mounting inductance with the selected parts.
The [Renesas quick guide](https://www.renesas.com/en/document/apn/ra8p1-mcu-quick-design-guide)
and [memory architecture note](https://www.renesas.com/en/document/apn/getting-started-ra8p1-memory-architecture-configurations-and-topologies)
reviewed here do not prescribe exact SDRAM series-resistor/bypass values.

There is no VREF pin or DDR-style VTT requirement. R43 now provides the
populated source-series clock-resistor position on page 2. Its exact part
is YAGEO RC0603JR-070RL, DigiKey 311-0.0GRCT-ND, with a blank deferred
footprint and the native value `0R`. The
[manufacturer part specification](https://www.yageogroup.com/component-documentation/download/specsheet/RC0603JR-070RL)
and [RC_L series Table 2, p5](https://www.yageogroup.com/content/datasheet/asset/file/PYU-RC_GROUP_51_ROHS_L)
identify a 0603 jumper with initial resistance <50 mOhm and 1 A rated
current; this is not a claim of an ideal zero resistance or a useful
"5% of zero" tolerance. Its copied native sourcing snapshot is
2026-09-08: [DigiKey stock 7,899,620](https://www.digikey.com/en/products/detail/yageo/RC0603JR-070RL/726675),
USD 0.10 / 0.011 / 0.0066 at quantities 1 / 10 / 100. Stock is not reserved.
R43 is an SI-tuning starting link, not approved damping or timing closure;
no 22/33 Ohm value is approved yet. R53 now pulls up its
memory-side `SDRAM_CLK` node, not `SDRAM_CLK_SRC`.
Decide address/control and bidirectional DQ damping from the actual load,
driver model and both-direction timing. A clock-only delay can consume
write-hold margin. Do not claim the EK resistor value is valid for this
TSOP memory, or copy OSPI trace rules into the SDRAM timing contract.

### CMS-010C: Initialization and retention contract

ISSI Rev. C pp19-20 requires simultaneous VDD/VDDQ rise and at least
100 us with stable CLK, CKE/DQM high and NOP or command-inhibit states,
followed by precharge-all, at least two auto-refresh cycles and MRS.
The note permits MRS before the refresh cycles. A conservative 200 us
and eight refresh cycles may be retained, but waiting with CLK stopped
does not satisfy the stable-clock interval. DQ must not be driven against
the memory during initialization. Program CL2 or CL3; this part does not
support CL1. Do not transfer the Alliance mode-register options blindly.

RA8P1 SDCKOCR resets to zero and disabled SDCLK is high. Firmware must
hold safe command/mask states while starting SDCLK, wait the specified
stable-clock interval, then follow HUM Figure 15.48's 32-bit sequence.
ISSI's startup contract replaces the prior Alliance simultaneous-clock
wording blocker; it does not establish that existing firmware implements it.
In particular,
BSIZE/EXENB sequencing must follow the 32-bit case, rather than using the
16-bit exception. Program mode/timing/address shift, enable refresh and
finally enable accesses with the specified readback/barrier. Existing
`ra8_sdramc_init()` is EK-RA8D2 firmware, not approved ISSI startup:
its reviewed clock enable is immediately followed by initialization with
no explicit clock-stable 100 us pause. No firmware was changed here.

For the -7 grade, the timing-programming basis includes tRC=63 ns,
tRP/tRCD=20 ns, tRRD/tDPL=14 ns, tDAL=35 ns and tXSR=70 ns.
Honor both the 14 ns tMRD entry and p17's two-cycle requirement.
Round each requirement up using the actual controller clock and register
encoding; these values are not already-converted register settings.

Hard-off discards RAM. Retention requires self-refresh entry before stopping
SDCLK, keeping supply present and CKE actively low, including the HUM's
IOKEEP/standby-output handling. Tristating CKE against its pullup exits that
controlled state. Wake must honor the selected part's tXSR, restore the
clock/control sequence and only then permit bus access. Ordinary power-down
is not self-refresh and cannot retain data indefinitely.

### CMS-010D: DC and timing qualification gates

PWR-002's fixed-PWM static main-rail range is
3.242044111..3.358485094 V, not a transient envelope. RA SDRAM inputs require
VIH >=0.7*VCC or 0.7*VCC2, not the generic GPIO 0.8 factor. Against ISSI
VOH >=2.4 V, the static read-high margin is only 49.060435 mV. The rail at
zero high margin is 2.4/0.7 = 3.428571429 V; this is not an acceptable
operating target or allowance to spend on ringing. Establish a positive
noise-margin requirement and measured/modelled IO waveforms, including
rail overshoot and ground offset. Static read-low margin is 572.613233 mV.
ISSI's VOH/VOL limits use -2/+2 mA tests, not the prior Alliance 4 mA test.
Ordinary control-output DC screens give 742.044111 mV high and 300 mV low
margin using the RA 1 mA test, but do not prove dynamic edges or PA15 CLK.

RA Table 2.57 condition 2 requires SDCLK high-speed/high drive, other bus
outputs high drive, and a 15 pF output-load condition. BCLK operation permits
125 MHz; BCLKA has a separate 133 MHz limit. Do not configure simultaneous
CSC operation while claiming these condition-2 timings. ISSI p15 lists
characterized clock capacitance 3.5 pF, command/address 3.8 pF and DQ 6 pF;
MCU input capacitance for these pins is <=8 pF under its listed test.
Interconnect and probes add loading and are not included by those numbers.

The following are zero-interconnect screens at 125 MHz/CL3, not timing
closure. RA delay/setup/hold limits come from Table 2.57. ISSI timings
come from Rev. C pp16-18; no typical parameters are used as
all-corner guarantees.

| Screen | Arithmetic, ns | Unallocated result |
| --- | --- | ---: |
| Read setup | 8 - 5.4 - 2.1 | 0.5 ns |
| Write/address/control setup | 8 - 6.0 - 1.5 | 0.5 ns |
| Write/address/control hold | 0.8 - 0.8 | 0 ns |
| Read hold at ISSI's 50 pF test load only | 2.5 - 1.5 | 1.0 ns |

ISSI's -7 tOH=2.5 ns and tAC=5.4 ns use the stated 50 pF test load.
Rev. C gives no 0 pF hold-time endpoint; neither the former Alliance
1.8 ns endpoint nor interpolation supplies an ISSI arbitrary-load guarantee.
The 1 ns read-hold result is therefore a test-load screen, not PCB closure.
ISSI uses a 1.4 V AC timing reference and a 1 ns transition assumption;
RA bus output timing uses the half-supply crossing. Correct the reference
levels and apply ISSI's slow-edge adjustments before adding flight,
skew, jitter, duty-cycle and model uncertainties. Also verify ISSI's
2.5 ns minimum clock-high/low widths and 0.3..1.2 ns transition condition;
RA's separate SDCLK waveform limits are not an automatic compatibility proof.

For matched reference levels, let tc be MCU-to-memory clock flight and td
be data flight. A first write screen adds (td-tc) to hold and subtracts it
from setup; a read setup screen subtracts tc+td. Clock-only series delay
therefore improves write setup but worsens write hold and read setup.
These signs explain why matching lengths or slowing SDCLK alone does not
close every constraint. At 62.5 MHz/CL3 the raw setup screens become 8.5 ns,
but raw write hold remains zero. Use lower frequency for initial evaluation
if appropriate; it is not an approved solution to the hold/DC/startup gates.

### CMS-010E: Reproducible identity and arithmetic checks

Run this Python block from the repository root. It checks the displayed
pin-map coverage against independently transcribed memory signal numbers,
capacity, pull and voltage corners, and timing arithmetic. Manufacturer
identities were visually/source checked above; Python cannot turn those
datasheet inputs into a board qualification or verify a future schematic.

```python
from pathlib import Path
from math import isclose
import re

document = Path('ra8p1_kicad/design/camera_storage_interfaces.md').read_text()
section = document.split('## CMS-010: IS42S32160F-7TLI pin and electrical contract')[1]
rows = re.findall(
    r'^\| ([A-Za-z0-9#/]+) \| (\d+) \| (P[0-9A-D][0-9]{2}) \| ([A-Z][0-9]+) \|$',
    section, re.M)
assert len(rows) == 57
assert len({row[0] for row in rows}) == 57
assert len({row[2] for row in rows}) == 57
assert len({row[3] for row in rows}) == 57
signals = {name: int(number) for name, number, port, ball in rows}
expected = {'CLK': 68, 'CKE': 67, 'CS#': 20, 'RAS#': 19, 'CAS#': 18, 'WE#': 17}
for prefix, pins in (
    ('A', [25, 26, 27, 60, 61, 62, 63, 64, 65, 66, 24, 21, 69]),
    ('BA', [22, 23]),
    ('DQM', [16, 71, 28, 59]),
    ('DQ', [2, 4, 5, 7, 8, 10, 11, 13, 74, 76, 77, 79, 80, 82, 83, 85,
            31, 33, 34, 36, 37, 39, 40, 42, 45, 47, 48, 50, 51, 53, 54, 56]),
):
    expected.update({f'{prefix}{index}': pin for index, pin in enumerate(pins)})
expected['A10/AP'] = expected.pop('A10')
assert signals == expected
groups = re.findall(r'^\| (VDDQ?|VSSQ?|NC) \| ([0-9, ]+) \| ([^|]+) \|$', section, re.M)
assert len(groups) == 5
other_pins = [int(pin) for name, numbers, connection in groups for pin in numbers.split(',')]
assert sorted(list(signals.values()) + other_pins) == list(range(1, 87))
expected_groups = {
    'VDD': [1, 15, 29, 43], 'VDDQ': [3, 9, 35, 41, 49, 55, 75, 81],
    'VSS': [44, 58, 72, 86], 'VSSQ': [6, 12, 32, 38, 46, 52, 78, 84],
    'NC': [14, 30, 57, 70, 73],
}
assert {name: [int(pin) for pin in numbers.split(',')]
        for name, numbers, connection in groups} == expected_groups
assert 2**13 * 2**9 * 4 * 4 == 64 * 2**20
print('57 unique signals and MCU identities; all 86 memory pins covered once; 64 MiB')

pull_rows = re.findall(r'^\| (R\d+) \| (SDRAM_[A-Z0-9_]+) \| (\d+) \|$', section, re.M)
expected_pulls = {
    'R44': ('SDRAM_CKE', 67), 'R45': ('SDRAM_DQM0', 16),
    'R46': ('SDRAM_DQM1', 71), 'R47': ('SDRAM_DQM2', 28),
    'R48': ('SDRAM_DQM3', 59), 'R49': ('SDRAM_CS_N', 20),
    'R50': ('SDRAM_RAS_N', 19), 'R51': ('SDRAM_CAS_N', 18),
    'R52': ('SDRAM_WE_N', 17), 'R53': ('SDRAM_CLK', 68),
}
assert len(pull_rows) == 10
assert {ref: (net, int(pin)) for ref, net, pin in pull_rows} == expected_pulls
assert len({pin for net, pin in expected_pulls.values()}) == 10
assert {pin for net, pin in expected_pulls.values()} <= set(signals.values())
print('R44-R53 ten-pull reference/target map PASS; native wiring checked separately')

placed_bypass_nominal_uf = 12 * .1 + 10
assert isclose(placed_bypass_nominal_uf, 11.2)
print('C76-C88 nominal bypass uF', placed_bypass_nominal_uf,
      '; not effective capacitance or proof of the whole-rail capacitance budget')

vmin, vmax = 3.242044111302129, 3.3584850935146022  # PWR-002 static, not ripple
read_high_margin = 2.4 - .7 * vmax
read_low_margin = .3 * vmin - .4
assert isclose(read_high_margin, .04906043453977871)
assert isclose(read_low_margin, .5726132333906387)
assert vmax < 2.4 / .7
print('read high/low static margins V', read_high_margin, read_low_margin)
print('zero high-margin rail V', 2.4 / .7)
print('ordinary control high/low margins V', vmin - .5 - 2.0, .8 - .5)

rmin, rmax = 10000 * .99 * .99, 10000 * 1.01 * 1.01
memory_leak, mcu_leak, board_allocation = 5e-6, 1e-6, 1e-6
pull_high = 3.0 - (memory_leak + mcu_leak + board_allocation) * rmax
pull_sink = 3.6 / rmin + memory_leak + board_allocation
assert isclose(pull_high, 2.928593) and pull_high > 2.0
assert isclose(pull_sink, .3733094582185491e-3)
assert pull_sink < .374e-3 and pull_sink < 1e-3
print('pull high V / low-output sink mA', pull_high, pull_sink * 1e3)

pull_resistor_current = 3.6 / rmin
pull_resistor_power = 3.6**2 / rmin
ten_pull_current = 10 * pull_resistor_current
ten_sink_screen = 10 * pull_sink
ten_resistor_power = 10 * pull_resistor_power
pull_allocation = 4e-3
memory_allocation = 250e-3  # PWR-003; not a manufacturer maximum
remaining_memory_allocation = memory_allocation - pull_allocation
assert isclose(pull_resistor_current, .3673094582185491e-3)
assert isclose(pull_resistor_power, 1.322314049586777e-3)
assert isclose(ten_pull_current, 3.673094582185491e-3)
assert isclose(ten_sink_screen, 3.733094582185491e-3)
assert isclose(ten_resistor_power, 13.22314049586777e-3)
assert ten_pull_current < ten_sink_screen < pull_allocation
assert isclose(remaining_memory_allocation, .246)
print('per-pull resistor current mA / heat mW',
      pull_resistor_current * 1e3, pull_resistor_power * 1e3)
print('all ten LOW: resistor current mA / sink screen mA / resistor heat mW',
      ten_pull_current * 1e3, ten_sink_screen * 1e3, ten_resistor_power * 1e3)
print('pull allocation mA / remaining memory and dynamic IO allocation mA',
      pull_allocation * 1e3, remaining_memory_allocation * 1e3,
      '; both within existing PWR-003 memory allocation, not hardware qualification')

for frequency in (125_000_000, 62_500_000):
    period_ns = 1e9 / frequency
    read_setup = period_ns - 5.4 - 2.1
    write_setup = period_ns - 6.0 - 1.5
    write_hold = .8 - .8
    read_hold_test_load = 2.5 - 1.5  # ISSI 50 pF test; no arbitrary-load guarantee
    assert isclose(read_setup, write_setup)
    assert isclose(write_hold, 0) and isclose(read_hold_test_load, 1.0)
    assert isclose(read_setup, .5 if frequency == 125_000_000 else 8.5)
    print('Hz / raw read setup / write setup / write hold / read hold at 50 pF ns',
          frequency, read_setup, write_setup, write_hold, read_hold_test_load)
print('CMS-010 arithmetic PASS; startup, SI, positive noise margin and hardware qualification OPEN')
```

The selected order code is IS42S32160F-7TLI, industrial -40..85 C,
86-pin TSOP-II, -7 speed grade (143 MHz at CL3). CMS-008's Alliance
stock/prices are historical and must not populate the ISSI symbol or BOM.
Use ISSI manufacturer identity and a verified matching distributor order
code; do not inherit the IME or Alliance procurement fields through Save As.

Sourcing refresh, 2026-09-08, for this exact ordering code:

| Distributor / order code | Stock | USD one / ten | Availability restriction |
| --- | ---: | --- | --- |
| [DigiKey 706-1417-5-ND](https://www.digikey.com/en/products/detail/issi-integrated-silicon-solution-inc/IS42S32160F-7TLI/5319838) | 440 | 17.04 / 15.812 | Maximum 108 per 30 days; no backorders |
| [Mouser 870-42S32160F7TLI](https://www.mouser.com/en/ProductDetail/ISSI/IS42S32160F-7TLI?qs=N2Tevpb%252Bvoaf83cqUgjrcA%3D%3D) | 27 | 17.05 / 15.82 | 52-week stated lead time; 324 on order without a delivery date |

This is a distributor snapshot, not reserved stock or a volume-supply
guarantee. The native symbol uses the DigiKey ordering code and snapshot.

Reciprocal native annotations now present:

- Page 2, `CMS-010 | R43: 0R source clock link.`, UUID
  `5e3b6d56-5b13-4749-aa0d-b25d6926db3d`: populated tuning position,
  with damping and timing explicitly unqualified.
- Page 10, `CMS-010 | SDRAM power and bypass`, UUID
  `3dfe9a7d-05b9-43c6-b336-a2b76637c897`: shared supply, C76-C88
  identities and the nominal `12 * 0.1 + 10 = 11.2 uF` calculation.
- Page 10, `CMS-010 | R44-R53: 10k reset defaults`, UUID
  `5e5b0bd4-c7e4-4068-83f4-db1d46ee8941`: resistor and leakage bounds,
  `VHIGHmin = 3.0 - 7uA * 10201 = 2.928593 V`, and qualification limits.
- Page 10, `CMS-010 | Pull load and power states`, UUID
  `47df23ed-e7e5-4c56-9ffa-338aec18de44`: sink-current and resistor-power
  equations, all-ten-LOW totals, the 4 mA allocation and retention/hard-off
  constraints.

All four annotations hyperlink to this CMS-010 contract. Page 10's block
heading now identifies signal and reset-default circuits, with startup,
SI/timing and hardware qualification explicitly open. Preserve these
reciprocal references when the circuit changes. A future timing annotation
should show the local results,
`125 MHz raw setup 0.5 ns; write hold 0 ns; read-high static margin 49.06 mV.
Startup/SI qualification OPEN.` That additional timing note is not claimed
present in the current schematic.

## CMS-011: 128 MiB Octal NOR electrical contract

Tracking: [memory issue #827](https://github.com/bsikar/ra8-firmware/issues/827).
Selected: **Infineon S28HL01GTFPBHI030**, industrial -40..85 C, 1 Gbit,
2.7..3.6 V core and IO, 24-ball 8 x 8 mm BGA, 1 mm pitch, tray.
The [manufacturer product record](https://www.infineon.com/part/S28HL01GTFPBHI030)
identifies an active preferred product. This is an Octal capacity upgrade,
not a pin-compatible or command-compatible substitution for the EK Macronix.
The MCU VCC2 bank remains at +3V3_MCU with SDRAM; a 1.8 V NOR is not a
substitute for this contract.

**Native library checkpoint:** `Memory:S28HL01GTFPBHI030` is implemented in
`../libs/symbols/Memory.kicad_sym`, with 13 interface pins in unit A and
11 power/reserved pins in unit B. All 24 balls are explicit; DNU balls use
the unconnected electrical type. The native Symbol Checker reports no
issues. Both units use 150 mil pins, 50 mil text and filled body outlines.
Exact manufacturer/distributor fields and the dated sourcing snapshot are
embedded in the library symbol; its footprint remains deliberately blank.

Circuit placement and wiring remain in progress. The symbol is not yet
instantiated in the schematic or exported BOM. This section does not claim
the following circuit components, nets or annotations are placed or
ERC-clean. No new reference numbers or annotation UUIDs have been assigned
in this record. Preserve the completed CMS-010 checkpoint; its BOM and ERC
counts are not a report on the new NOR circuit.
Reciprocal CMS-011 annotations must be added to the native MCU/NOR pages,
then the exported netlist, BOM, ERC and full PDF independently checked.

### CMS-011A: Source revision and exact pin contract

The released English manufacturer datasheet available through Mouser is
[002-18216 Rev. AB, 2024-05-13][nor-ab], not advance information. Its
Figure 1 / Table 7, pp.6-7, define the following ball map and directions.
The newer manufacturer [Chinese Rev. AD, 2025-06-30][nor-ad] is a revision
cross-check, not a replacement for controlling English specifications:
its notice gives English precedence. The revision history, p.179, records
AC changes to tDIS, tBE units and thermal data, then AD changes to tSU/tHD.
The current English download redirects to authentication. Obtain and
review current English AD and applicable errata before design release;
no separate public errata found in this review is not proof none exist.

| Flash signal | Flash ball | MCU port | MCU ball | Connection / native pin type |
| --- | --- | --- | --- | --- |
| DQ0 | D3 | P100 | U6 | 30R series; bidirectional |
| DQ1 | D2 | P803 | P7 | 30R series; bidirectional |
| DQ2 | C4 | P103 | R4 | 30R series; bidirectional |
| DQ3 | D4 | P101 | R5 | 30R series; bidirectional |
| DQ4 | D5 | P102 | P5 | 30R series; bidirectional |
| DQ5 | E3 | P800 | T6 | 30R series; bidirectional |
| DQ6 | E2 | P802 | R6 | 30R series; bidirectional |
| DQ7 | E1 | P804 | R7 | 30R series; bidirectional |
| CK | B2 | P808 | U5 | 30R series; flash input |
| DS | C3 | P801 | P6 | 30R series; flash output, MCU input in this mode |
| CS# | C2 | P104 | M6 | Direct OM_0_CS1; flash input, 10k pullup |
| INT# | A5 | P105 | N7 | Direct GPIO IRQ0; flash open-collector, 47k pullup |
| RESET# | A4 | - | - | Direct MCU_RESET_N; flash input, existing R1 pullup |

| Flash supply / unused | Flash balls | Connection / native pin type |
| --- | --- | --- |
| VCC | B4 | +3V3_MCU, power input |
| VCCQ | D1, E4 | Same +3V3_MCU node, power input |
| VSS | B3 | GND, power input |
| VSSQ | C1, E5 | GND, power input |
| DNU | A2, A3, B1, B5, C5 | Individually unconnected, no-connect type/marker |

A1 is depopulated: do not create a twenty-fifth pin. There are 13 signal,
6 supply/ground and 5 DNU balls. Supply symbols must represent actual nets,
not hide absent wiring; no local PWR_FLAG is justified by this passive load.
Use one common VCC/VCCQ supply node so sequencing cannot make VCCQ exceed
VCC. Do not insert independently switched or delayed VCCQ branches.
The [RA8P1 datasheet Rev.1.30][nor-ra-ds], Tables 1.17 and 2.1, confirms
the GPIO/IRQ allocation and voltage domain; CMS-006 checks reservation
collisions. P106/N6 is free, and no dedicated flash-reset GPIO is required.

### CMS-011B: Series, default-state and local supply components

[RA8P1 hardware manual Rev.1.30][nor-ra-hum], Table 45.2, p.2997, requires
external **30 ohm +/-1%** series resistors on the eight SIO lines, SCLK and
DQS for the JESD251 driver definition. Thus this circuit requires ten,
not only a clock tuning position. SCLKN is unused. Do not copy SDRAM R43's
0R rule to NOR, and do not add series elements to CS#, RESET# or INT# by
analogy. The proposed MCU-side grouping is a layout starting point; it
does not establish bidirectional signal integrity or prescribe trace lengths.

Select ten **RT0603BRD0730RL**, 30R, +/-0.1%, +/-25 ppm/C. The
[YAGEO RT specification, V17, 2026-02-12][nor-rt] gives 0.1 W at 70 C,
derating to zero at 155 C, and 75 V maximum working voltage for this case.
The actual continuous voltage limit is the lower of 75 V and sqrt(P*R),
not permission to apply 75 V to 30R. These are signal resistors, not
series DC supply resistors. Pulse loading, temperature rise and parasitic
impedance remain SI/qualification tasks.

For a conservative 100 C departure from the resistance reference temperature:

```text
R30min = 30*(1-0.001)*(1-25e-6*100) = 29.895075 ohm
R30max = 30*(1+0.001)*(1+25e-6*100) = 30.105075 ohm
Initial tolerance plus TCR therefore remain inside 29.7..30.3 ohm.
Ordinary +/-1%, 100 ppm/C parts span 29.403..30.603 ohm for the same screen.
```

This does not include aging, assembly drift or AC impedance. Infineon's
factory CFR4N/V[7:5]=101 selects an **internal** nominal 30R driver
(Rev. AB Table 58, p.97); it does not replace the ten external parts.
Keep that initial setting for qualification; any tuning must recheck both
read and write directions and should not rewrite nonvolatile settings
at every boot.

Proposed passive contract, with no new native reference numbers implied:

| Function | Quantity / value | Exact MPN | Existing native donor |
| --- | --- | --- | --- |
| DQ0..7, CK, DS series | 10 x 30R | YAGEO RT0603BRD0730RL | New sourced value; do not inherit 0R metadata |
| CS# idle-high | 1 x 10k to +3V3_MCU | YAGEO RC0603FR-0710KL | R44 or R27, with new reference |
| INT# idle-high | 1 x 47k to +3V3_MCU | YAGEO RC0603FR-0747KL | New sourced value |
| Local VCC / VCCQ bypass | 3 x 100n to GND | TDK C1608X7R1H104K080AA | C76, one copy per supply ball |
| Shared local bulk bypass | 1 x 10u to GND | TDK C3216X7R1V106K160AC | C88 |
| RESET# idle-high | Existing R1, no added pull | Existing common-reset network | No new component |

The capacitor proposal is an engineering starting point, not a claimed
manufacturer minimum or completed impedance design. Nominal total is
`3*0.1 + 10 = 10.3 uF`; initial +/-10% alone gives 9.27..11.33 uF.
[TDK 100n product data][nor-c100] and [10u product data][nor-c10] identify
50 V X7R 0603 and 35 V X7R 1206 respectively. PWR-001 records the 100n
nominal DC-bias curve; the [10u characterization sheet][nor-c10-curve]
is reference characterization, not an all-corners effective-capacitance
guarantee. Include DC bias, temperature, aging, mounting inductance,
rail ripple and main-rail discharge in later PDN qualification.

### CMS-011C: Pull and common-reset arithmetic

Use the PWR-002 static rail envelope 3.242044111..3.358485094 V, with a
separate 3.6 V stress screen; neither includes unqualified transient ripple.
RC0603 initial +/-1% and +/-100 ppm/C over 100 C give multiplicative
resistance bounds 0.9801..1.0201 times nominal. The
[10k manufacturer specification][nor-rc10] and
[47k manufacturer specification][nor-rc47] identify the selected parts.
At the conservative 125 C resistor-temperature screen, linear derating
leaves `0.1*(155-125)/(155-70) = 35.294118 mW`; the DC loads below fit
that power allowance and the lower of sqrt(P*R) or the 75 V case limit.
This does not qualify contamination leakage or reset/interrupt edge speed.

Flash ILI/ILO are +/-2 uA at 85 C (Rev. AB Table 87, pp.131-132,
VCC maximum, input at VIH or VSS, CS# HIGH test conditions).
P105 uses the ordinary **1 uA** MCU input/off-state bound, not an invented
6 uA limit (RA8P1 Table 2.7, p.57). Allocate another 1 uA per node to board
leakage; that is an acceptance condition requiring verification.

```text
R47min/max = 46064.7 / 47944.7 ohm
INT high adverse current = 2uA flash + 1uA MCU + 1uA board = 4uA
INT VHIGHmin = 3.242044111 - 4uA*47944.7 = 3.050265311 V
INT high margin = VHIGHmin - 0.8*3.358485094 = 0.363477236 V
INT low sink = 3.6/46064.7 + 1uA MCU + 1uA board = 80.150949 uA
INT resistor stress = 3.6^2/46064.7 = 0.281343415 mW

R10min/max = 9801 / 10201 ohm
CS high adverse current = 2uA flash + 1uA MCU + 1uA board = 4uA
CS VHIGHmin = 3.242044111 - 4uA*10201 = 3.201240111 V
CS low sink = 3.6/9801 + 2uA flash + 1uA board = 0.370309458 mA
CS resistor stress = 3.6^2/9801 = 1.322314050 mW
```

Infineon recommends a 5k..10k INT pullup, but its tabulated VOL <=0.2 V
test is only 100 uA. The selected 47k is an explicit design departure to
stay within that guaranteed DC test load: do not silently claim it is the
vendor's recommendation. Its release-edge RC time and interrupt detection
need qualification with actual trace/input capacitance and configured IRQ
filtering, including release during active transactions rather than only
the table's CS# HIGH leakage test. There is no arbitrary frequency or
maximum-capacitance guarantee in this calculation. Flash off-state leakage is not added again to the
actively sinking flash output; board/MCU adverse current is.

The current common reset net contains R1.2, U2.1, U1.D5, J1.10 and U7.3.
The new flash A4 joins this same +3V3_MCU-domain wire. The independent reset
review used 5 uA MCU, 5 uA U7 and 2 uA flash adverse source current:
`3.6/9801 + 12uA = 379.309458 uA`, below U2's 1 mA VOL test load.
The flash adds at most `2uA*10201 = 20.402 mV` to this DC drop screen.
For an explicit **25 uA total** adverse high-state leakage allocation,
`3.02395 - 25uA*10201 = 2.768925 V`, with
`2.768925 - 0.8*3.02395 = 0.349765 V` margin at the minimum U2 falling
threshold. This total allocation must cover the board and attached probe;
it is not a measured load or a sum of guaranteed capacitance bounds.
Flash RESET# adds at most 7.5 pF (Table 85, p.130), while MCU/U7 typical
capacitances do not establish a guaranteed whole-node maximum. Keep the
existing R1 and qualify sink, release edge, probe loading and brownout;
no additional NOR pullup is proposed. U2's source/threshold basis remains
in [PWR-002](power_decoupling.md#pwr-002-main-rail-regulation-and-reset-headroom).

### CMS-011D: Power, reset and transaction contract

Apply Rev. AB sections 4.13/4.15, pp.73-81, and Table 89, pp.140-141:
CS# must track the rising supply and remain inactive during initialization.
For 1 Gbit, tPU is 500 us maximum after VCC reaches its operating minimum;
tRP is 200 ns minimum, tRH is 500 us **from RESET# LOW to CS# LOW**,
and tRS is 50 ns from RESET# HIGH to CS# LOW. A long-held reset can
cover tRH; 500 us after every release is not the datasheet definition.

The implementation policy is deliberately conservative: keep CS# HIGH and
wait at least **1 ms after both qualified supply and external reset release**
before the first NOR command. This is a firmware/fixture obligation, not a
newly implemented delay circuit. U2's 12..28 ms release delay and the 3 ms
service reset cover the long-reset case; a short debug pulse must still
meet tRP and the pre-access delay. NOR reset timing must not be inferred
from the MCU's shorter minimum reset pulse/internal wait.

Hardware RESET# only handles assertions of the external MCU_RESET_N wire.
An internal MCU watchdog or software reset can leave NOR powered in its
previous volatile Octal state. Initialization must recover the current
protocol without unsafe speculative writes, inspect device status, then
configure the intended mode before XIP or DMA. Use the read-only DS
direction in the selected xSPI flash profile; do not apply
HyperBus write-mask signaling or drive against the flash strobe output.
Preserve the factory nonvolatile SPI startup configuration and enter Octal through volatile
configuration. Hardware reset reloads nonvolatile configuration; it does
not erase a previously changed nonvolatile mode back to factory SPI.
Recovery after internal resets, resets during writes and corrupt settings
requires explicit testing. No firmware changes are included in this record.

The full cold-restart screen requires VCC below 0.7 V for at least 25 us
after a drop below the 2.4 V cutoff; observe the specified minimum rise/fall
times of 1/30 us per volt, not an assumed instantaneous safe ramp.
Include the local 10.3 uF in SYS-007 hard-off/discharge qualification;
the whole-rail capacitance/active-load contract still controls. Neither
shared reset nor supply discharge guarantees completion of interrupted
program/erase. Journaled metadata and recoverable images remain required.

This part is not a promise of the Macronix LW family's true simultaneous
read/write behavior. Use the documented suspend/read/resume restrictions,
or execute update-critical code from internal MRAM/SDRAM. Do not assume
uninterrupted XIP from NOR while an embedded write operation blocks reads.

### CMS-011E: Clock and power qualification gates

The proposed performance baseline is **125 MHz Octal DDR with DS**, not
Quad, and not a production timing guarantee. The selected HL-T limit is
166 MHz: `1e9/166e6 = 6.024096386 ns`. A 166.666667 MHz MCU divider with
a 6 ns period exceeds it despite rounded marketing labels. Frequency
tolerance also belongs in the final selected-clock proof.

The RA8P1 Rev.1.30 Table 2.65 high-speed conditions, drive selections and
15 pF loading must be applied to the actual paths including all ten series
resistors. In particular, minimum specified MCU CK slew and the flash AC
test slew are not interchangeable; a nominal frequency comparison does
not close timing. Verify DS alignment, input/output loading, trace skew,
setup/hold, clock duty cycle, ringing and overshoot at supply/temperature
corners. Current English AD review is also required; retaining AB's more
conservative low-speed SPI setup/hold values is an interim screen, not
permission to disregard a changed released specification.

Rev. AB Table 87, pp.132-134, gives 1 Gbit program/erase maxima of 66 mA,
POR 80 mA, 85 C standby 160 uA and deep-power-down 26 uA. Read-current
figures exclude output switching. Its 173 mA DDR row is labeled 200 MHz
for both HL/HS devices although the selected HL maximum is 166 MHz.
That ambiguity prevents treating 173 mA as a clean manufacturer bound for
this exact selected operating point.

Reserve **250 mA for this NOR domain** as a qualification allocation,
including its IO switching and pulls, not a manufacturer maximum.
For an illustrative 15 pF total load on each of eight data outputs plus DS,
with one charging transition per clock cycle on every output:

```text
I_switch_screen = 9 * 15pF * Vrail_max * fCK
At 125 MHz: 56.674436 mA; 173 + 56.674436 = 229.674436 mA
At 166 MHz: 75.263651 mA; 173 + 75.263651 = 248.263651 mA
```

This is charge arithmetic, not a simulation or a guarantee that 173 mA is
valid/monotonic at lower clocks. The illustrative 15 pF must include the
actual receiving/input and interconnect load; flash input capacitance is
not a replacement for that output-load budget. Do not add the MCU's CK
driver loss to NOR current while omitting it from the MCU rail budget.
Reserve 1 mA within the 250 mA for the two external pulls and leakage;
the executable screen below checks this conservative static allowance.

The earlier PWR-003 NOR allocation of 100 mA is superseded for this
selected part. Keeping other allocations unchanged raises the main rail
from **1.65 A to 1.80 A**, the radio-off cold-start reference screen from
1.730 A to **1.880 A**, and the wake screen from 1.670 A to **1.820 A**.
Radio-on cold start would screen at **2.380 A** and remains prohibited.
These are allocations/reference-current sums, not all-corners startup
maxima; non-DCDC MCU current, capacitor charging, regulator efficiency,
current-limit behavior, thermal rise and load transients still need closure.
At 1.80 A the existing nominal 2 A TPS63802 has only 0.20 A nameplate
headroom; do not certify guaranteed delivery or thermal margin from that
subtraction. Reopen [PWR-003](power_decoupling.md#pwr-003-tps63802-main-digital-converter)
and the battery/source budget before approving simultaneous operation.
High-quality audio, radio and the required storage scope are not silently
reduced to make this arithmetic fit.

### CMS-011F: Exact sourcing snapshot and reproducible checks

Snapshot 2026-09-08, USD excluding tax/shipping, not reserved stock. NOR,
30R and 47k rows were refreshed directly; 10k/capacitor rows retain the
same-date verified native-donor snapshots. Copy identity and order code,
not another value's inherited sourcing metadata.

| Exact MPN / DigiKey order code | Stock | USD at 1 / 10 / 100 |
| --- | ---: | --- |
| [S28HL01GTFPBHI030 / 448-S28HL01GTFPBHI030-ND][nor-dk] | 2553 | 22.21 / 20.586 / 18.9691 |
| [RT0603BRD0730RL / 13-RT0603BRD0730RLCT-ND][nor-r30-dk] | 10201 | 0.10 / 0.067 / 0.0559 |
| [RC0603FR-0747KL / 311-47.0KHRCT-ND][nor-r47-dk] | 2036190 | 0.10 / 0.025 / 0.0122 |
| [RC0603FR-0710KL / 311-10.0KHRCT-ND][nor-r10-dk] | 2866522 | 0.10 / 0.025 / 0.0122 |
| [C1608X7R1H104K080AA / 445-1314-1-ND][nor-c100-dk] | 372402 | 0.11 / 0.06 / 0.0359 |
| [C3216X7R1V106K160AC / 445-14799-1-ND][nor-c10-dk] | 9358 | 0.68 / 0.424 / 0.2903 |

The exact [Mouser NOR listing][nor-mouser], order code
727-S28HL01GTFPBHI30, was also checked, but a current direct-page price
and purchasable quantity could not be verified. Do not present an older
search-index nonstock/MOQ result as current stock. DigiKey is the current
verified source for this selection; refresh both before procurement.

Run from the worktree root. This checks the displayed contract against
independently transcribed pin identities and recomputes the engineering
screens. It neither reads nor creates a future native NOR schematic.

```python
from pathlib import Path
from math import isclose, sqrt
import re

document = Path('ra8p1_kicad/design/camera_storage_interfaces.md').read_text()
section = document.split('## CMS-011: 128 MiB Octal NOR electrical contract')[1]
rows = re.findall(
    r'^\| (DQ[0-7]|CK|DS|CS#|INT#|RESET#) \| ([A-E][1-5]) '
    r'\| (P\d{3}|-) \| ([A-Z]\d+|-) \| ([^|]+) \|$', section, re.M)
expected = {
    'DQ0': ('D3', 'P100', 'U6'), 'DQ1': ('D2', 'P803', 'P7'),
    'DQ2': ('C4', 'P103', 'R4'), 'DQ3': ('D4', 'P101', 'R5'),
    'DQ4': ('D5', 'P102', 'P5'), 'DQ5': ('E3', 'P800', 'T6'),
    'DQ6': ('E2', 'P802', 'R6'), 'DQ7': ('E1', 'P804', 'R7'),
    'CK': ('B2', 'P808', 'U5'), 'DS': ('C3', 'P801', 'P6'),
    'CS#': ('C2', 'P104', 'M6'), 'INT#': ('A5', 'P105', 'N7'),
    'RESET#': ('A4', '-', '-'),
}
assert len(rows) == 13
assert {name: (ball, port, mcu_ball) for name, ball, port, mcu_ball, conn in rows} == expected
assert sum('30R series' in conn for name, ball, port, mcu_ball, conn in rows) == 10
assert 'MCU_RESET_N' in next(conn for name, ball, port, mcu_ball, conn in rows if name == 'RESET#')
supply_rows = re.findall(
    r'^\| (VCCQ?|VSSQ?|DNU) \| ([A-E1-5, ]+) \| ([^|]+) \|$', section, re.M)
supply_groups = {name: balls.replace(',', '').split() for name, balls, conn in supply_rows}
assert supply_groups == {
    'VCC': ['B4'], 'VCCQ': ['D1', 'E4'], 'VSS': ['B3'],
    'VSSQ': ['C1', 'E5'], 'DNU': ['A2', 'A3', 'B1', 'B5', 'C5'],
}
all_balls = [item[0] for item in expected.values()] + [b for group in supply_groups.values() for b in group]
assert len(all_balls) == len(set(all_balls)) == 24
assert set(all_balls) == {f'{r}{c}' for r in 'ABCDE' for c in range(1, 6)} - {'A1'}
ports = {port for ball, port, mcu_ball in expected.values() if port != '-'}
assert len(ports) == 12 and 'P106' not in ports
reservation = re.search(r'"ospi0": "([^"]+)"', document).group(1).split()
assert ports == set(reservation)
assert len({mcu_ball for ball, port, mcu_ball in expected.values() if port != '-'}) == 12
capacity = 2**30 // 8
assert capacity == 128 * 2**20 and capacity >= 64 * 2**20
print('CMS-011: 24 balls once, 12 reserved MCU ports, ten 30R paths, 128 MiB PASS')

vmin, vmax, vstress = 3.242044111302129, 3.3584850935146022, 3.6
initial_tol, tcr, delta_t = .01, 100e-6, 100
def pull_bounds(nominal):
    return (nominal*(1-initial_tol)*(1-tcr*delta_t),
            nominal*(1+initial_tol)*(1+tcr*delta_t))
r10min, r10max = pull_bounds(10000)
r47min, r47max = pull_bounds(47000)
assert isclose(r10min, 9801) and isclose(r10max, 10201)
assert isclose(r47min, 46064.7) and isclose(r47max, 47944.7)
flash_leak, mcu_leak, board_alloc = 2e-6, 1e-6, 1e-6
int_high = vmin - (flash_leak+mcu_leak+board_alloc)*r47max
int_sink = vstress/r47min + mcu_leak + board_alloc
int_margin = int_high - .8*vmax
cs_high = vmin - (flash_leak+mcu_leak+board_alloc)*r10max
cs_sink = vstress/r10min + flash_leak + board_alloc
int_heat, cs_heat = vstress**2/r47min, vstress**2/r10min
assert isclose(int_high, 3.0502653113021294)
assert isclose(int_margin, .3634772364904473) and int_margin > 0
assert isclose(int_sink, 80.15094855713812e-6) and int_sink < 100e-6
assert isclose(int_heat, .28134341480569726e-3)
assert isclose(cs_high, 3.201240111302129)
assert cs_high > .65*vmax and .5 < .35*vmin
assert isclose(cs_sink, .3703094582185491e-3) and cs_sink < 1e-3
assert isclose(cs_heat, 1.322314049586777e-3)
pull_allocation = 1e-3
assert int_sink + cs_sink < pull_allocation
derated_power_125c = .1*(155-125)/(155-70)
assert isclose(derated_power_125c, .03529411764705882)
assert int_heat < derated_power_125c and cs_heat < derated_power_125c
assert vstress < min(75, sqrt(derated_power_125c*r10min), sqrt(derated_power_125c*r47min))
print('INT high V / margin V / sink uA / resistor mW',
      int_high, int_margin, int_sink*1e6, int_heat*1e3)
print('CS high V / high margin V / sink mA / resistor mW',
      cs_high, cs_high-.65*vmax, cs_sink*1e3, cs_heat*1e3)
print('two active-low pull sinks mA / allocated mA',
      (int_sink+cs_sink)*1e3, pull_allocation*1e3)

r30min = 30*(1-.001)*(1-25e-6*100)
r30max = 30*(1+.001)*(1+25e-6*100)
assert isclose(r30min, 29.895075) and isclose(r30max, 30.105075)
assert 30*.99 < r30min < r30max < 30*1.01
assert isclose(30*.99*.99, 29.403) and isclose(30*1.01*1.01, 30.603)
assert sqrt(.1*r30min) < 75  # Actual DC power limit is lower than case voltage.
print('30R initial+TCR screen ohm', r30min, r30max, '; aging/AC qualification OPEN')
nominal_cap_uf = 3*.1 + 10
assert isclose(nominal_cap_uf, 10.3)
assert isclose(nominal_cap_uf*.9, 9.27) and isclose(nominal_cap_uf*1.1, 11.33)
print('NOR bypass nominal/initial min/max uF', nominal_cap_uf,
      nominal_cap_uf*.9, nominal_cap_uf*1.1, '; not effective-C guarantee')

reset_sink = vstress/r10min + (5+5+2)*1e-6
reset_drop_added = flash_leak*r10max
reset_vtrip_min, reset_leak_alloc = 3.02395, 25e-6
reset_high = reset_vtrip_min - reset_leak_alloc*r10max
reset_margin = reset_high - .8*reset_vtrip_min
assert isclose(reset_sink, 379.3094582185491e-6) and reset_sink < 1e-3
assert isclose(reset_drop_added, .020402)
assert isclose(reset_high, 2.768925) and isclose(reset_margin, .349765)
assert reset_margin > 0
print('common reset sink uA / added drop mV / allocated high margin V',
      reset_sink*1e6, reset_drop_added*1e3, reset_margin)
pre_access_policy_s = .001
assert pre_access_policy_s > 500e-6 and pre_access_policy_s > 50e-9
print('1 ms pre-access is a required policy, not implemented firmware or an RC proof')

f_baseline, f_limit = 125e6, 166e6
assert isclose(1e9/f_limit, 6.024096385542169)
assert 1e9/6 > f_limit and f_baseline < f_limit
nor_allocation, ambiguous_read_row = .250, .173
for freq, expected_dynamic in ((f_baseline, .05667443595305891),
                               (f_limit, .07526365094566276)):
    dynamic = 9*15e-12*vmax*freq
    assert isclose(dynamic, expected_dynamic)
    print('Hz / illustrative switching mA / ambiguous-row sum mA',
          freq, dynamic*1e3, (ambiguous_read_row+dynamic)*1e3)
    assert ambiguous_read_row + dynamic + pull_allocation < nor_allocation
new_main = 1.65 - .100 + nor_allocation
cold_radio_off = 1.730 - .100 + nor_allocation
wake_screen = 1.670 - .100 + nor_allocation
assert isclose(new_main, 1.8) and isclose(cold_radio_off, 1.880)
assert isclose(wake_screen, 1.820) and isclose(cold_radio_off+.5, 2.380)
assert cold_radio_off+.5 > 2.0
print('main / cold radio-off / wake / prohibited cold radio-on allocations A',
      new_main, cold_radio_off, wake_screen, cold_radio_off+.5)
print('CMS-011 arithmetic PASS; native implementation, current/timing/reset qualification OPEN')
```

Proposed reciprocal native note, to be tailored to actual references when
placed: `CMS-011 | 128 MiB Octal NOR; 10 x 30R external JESD251 series.
CS 10k. INT 47k: ILOW <=80.151uA; VHIGH >=3.050265V (4uA screen).
3 x 100n + 10u = 10.3uF nominal. 125MHz DDR/DS candidate; SI/PDN open.
Shared external reset only; >=1ms pre-access policy; internal MCU reset
needs protocol recovery. NOR 250mA allocation reopens main-rail budget.`
The note must hyperlink here; record its actual native page/UUID after
placement, without implying that this proposal is already on the schematic.

[nor-ab]: https://www.mouser.com/datasheet/3/70/1/8HS01GT_S28HL512T_S28HL01GT_512MB_1GB_SEMPER_TM_FLASH_OCTAL_INTERFACE_1_8V_3-DataSheet-v68_00-EN.pdf
[nor-ad]: https://www.infineon.com/assets/row/public/documents/10/49/infineon-s28hs512t-s28hs01gt-s28hl512t-s28hl01gt-512mb-1gb-semper-tm-flash-octal-interface-1-8v-3-datasheet-cn.pdf
[nor-ra-ds]: https://www.renesas.com/en/document/dst/ra8p1-group-datasheet
[nor-ra-hum]: https://www.renesas.com/en/document/mah/ra8p1-group-users-manual-hardware
[nor-rt]: https://yageogroup.com/content/datasheet/asset/file/PYU-RT_1-TO-0-01_ROHS_L
[nor-rc10]: https://www.yageogroup.com/component-documentation/download/specsheet/RC0603FR-0710KL
[nor-rc47]: https://www.yageogroup.com/component-documentation/download/specsheet/RC0603FR-0747KL
[nor-c100]: https://product.tdk.com/en/search/capacitor/ceramic/mlcc/info?part_no=C1608X7R1H104K080AA
[nor-c10]: https://product.tdk.com/en/search/capacitor/ceramic/mlcc/info?part_no=C3216X7R1V106K160AC
[nor-c10-curve]: https://product.tdk.com/system/files/dam/doc/product/capacitor/ceramic/mlcc/charasheet/c3216x7r1v106k160ac.pdf
[nor-dk]: https://www.digikey.com/en/products/detail/infineon-technologies/S28HL01GTFPBHI030/15903885
[nor-r30-dk]: https://www.digikey.com/en/products/detail/yageo/RT0603BRD0730RL/1072456
[nor-r47-dk]: https://www.digikey.com/en/products/detail/yageo/RC0603FR-0747KL/730200
[nor-r10-dk]: https://www.digikey.com/en/products/detail/yageo/RC0603FR-0710KL/729827
[nor-c100-dk]: https://www.digikey.com/en/products/detail/tdk-corporation/C1608X7R1H104K080AA/513811
[nor-c10-dk]: https://www.digikey.com/en/products/detail/tdk/C3216X7R1V106K160AC/3956465
[nor-mouser]: https://www.mouser.com/ProductDetail/Infineon-Technologies/S28HL01GTFPBHI030?qs=sPbYRqrBIVlVJsyzP6oGfQ%3D%3D
