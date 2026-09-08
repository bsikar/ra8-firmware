# Camera, removable storage and external-memory allocation

Revision 7, 2026-09-07. Target: R7KA8P1KFLCAC#UC0, MIPI-enabled BGA289.
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
| SD1CD | P406 / F14 | Independent card-detect contact; other contact to GND |
| Card supply / ground | Qualified +3.3 V / GND | 4 VDD / 6 VSS |

The socket's A/B detect switch is separate from its eight card contacts.
Confirm the actual symbol's A/B and shell pin identifiers against Hirose's
DM3AT-SF-PEJM5 drawing; do not rename a shell pad as a card contact.
microSD has no mechanical write-protect switch. SD1WP is available at P700,
which is already assigned to the radio; it is not needed for this socket.

Use 3.3 V signaling. Do not claim UHS/HS200/HS400 capability from an eMMC
marketing version. The RA8P1 SDHI SDR timing table gives a 20 ns minimum
clock period in the relevant 3.3 V conditions. Keep all CMD/DAT/CLK pins in
the selected _B timing group. Begin initialization at the card-specified
low speed; 50 MHz is an upper interface target, not an automatically valid
board clock. Select CMD/DAT pullups, card-detect pullup, source termination,
low-capacitance protection, effective bypass capacitance and any load switch
after their leakage, drive, timing and inrush calculations. Do not pull up
CLK by habit. Power-off states must not phantom-power the card through IOs.

Confirmed software-definition defect, not changed by this hardware task:
[connectors.h](../../libs/ra8_board_ek_ra8d2/inc/ra8_board_ek_ra8d2_connectors.h)
lines 922..949 calls P400 CMD, P401 CLK, P406 WP, P407 CD, and selects
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

Reserve OSPI0 as follows: CS#=P104/M6, CLK=P808/U5, DQS=P801/P6,
RESET#=P106/N6; IO0..IO7=P100/U6, P803/P7, P103/R4, P101/R5,
P102/P5, P800/T6, P802/R6, P804/R7. P104 is OM_0_CS1, not CS0.
The EK-RA8P1 also routes its flash ECS# to P105/N7; determine whether the
selected NOR actually has this pin before placing it.

Existing imported IS25LP01GJ-RHLE is 1 Gbit / 128 MiB Quad NOR, not Octal.
An in-stock capacity-baseline fallback is Winbond **W25Q512JVFIQ**, 64 MiB
Quad NOR. Its primary [selection guide](https://www.winbond.com/export/sites/winbond/product-selection-guide/file/2025-Product-Selection-Guide-Winbond-Code-Storage-Flash-Memory.pdf)
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
    "ospi0": "P104 P808 P801 P106 P100 P803 P103 P101 P102 P800 P802 P804 P105",
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
the entire 64 MiB NOR were used for that PCM. The native SD bus theoretical
payload is 25,000,000 bytes/s before protocol overhead and media stalls.
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

Suggested schematic note: `CMS-001..009: design/camera_storage_interfaces.md`.
Place the relevant local calculation/result beside each functional block,
not a large unrelated wall of text on the root sheet.

## CMS-008: Stocked single-device SDRAM alternative

Recommend **Alliance AS4C16M32SC-7TIN** as the sourced schematic candidate,
not as a pin-compatible replacement for the EK's BGA90 device. It preserves
64 MiB, x32, four-bank, 13-row/9-column organization. The 86-pin TSOP-II
needs its own exact symbol audit; this record does not change the native BOM.
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
Preserve them during subsequent work, regenerate the full PDF/BOM and
visually review the saved hierarchy before the design checkpoint commit.
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
