# ptp_time_transmitter

IEEE 1588-2019 PTP controller smoke test for the EK-RA8D2. Brings up the
on-chip Ethernet MAC at MAC `02:00:00:00:00:01`, waits for PHY link-up,
arms the PTP block in controller role on domain 0 with a 1 Hz Sync interval
(`k_ra_ptp_sync_int_1`), and sends one Sync + one Announce message every
second. SCI8 (PD_02 / PD_03 -- 115200 8N1) prints the running PTP wall
clock once per loop iteration so an attached terminal can see the clock
advance.

## Status: hardware bring-up

Same RMII pinout as `ethernet_tcp_echo` -- the EK-RA8D2 v1 J64
connector exits to a Wiznet WIZ810SR-style RJ-45 module on a daughter
board. Plug the EK-RA8D2 into a switch or a Linux host running `ptp4l`
and watch the peripheral latch on.

| Net          | Pin    | PSEL                       | Direction      |
|--------------|--------|----------------------------|----------------|
| ETH_REF_CLK  | P7_00  | k_ra_psel_ether_rmii (0x11)| 50 MHz REFCLK  |
| ETH_MDC      | P4_01  | k_ra_psel_ether_rmii (0x11)| MDIO clock     |
| ETH_MDIO     | P4_02  | k_ra_psel_ether_rmii (0x11)| MDIO data      |
| ETH_TXD0     | P7_01  | k_ra_psel_ether_rmii (0x11)| Tx data 0      |
| ETH_TXD1     | P7_02  | k_ra_psel_ether_rmii (0x11)| Tx data 1      |
| ETH_TX_EN    | P7_03  | k_ra_psel_ether_rmii (0x11)| Tx enable      |
| ETH_RXD0     | P7_04  | k_ra_psel_ether_rmii (0x11)| Rx data 0      |
| ETH_RXD1     | P7_05  | k_ra_psel_ether_rmii (0x11)| Rx data 1      |
| ETH_RX_DV    | P7_06  | k_ra_psel_ether_rmii (0x11)| Rx data valid  |
| ETH_RX_ER    | P7_07  | k_ra_psel_ether_rmii (0x11)| Rx error       |
| ETH_CRS_DV   | P7_08  | k_ra_psel_ether_rmii (0x11)| Carrier sense  |

## Test on Linux (`ptp4l`)

The board sources Sync + Announce messages but does not respond to
Delay_Req on the smoke-test path; that means a Linux host running
`ptp4l` in peripheral-only mode latches its time onto the controller and reports
the offset. The Wiznet daughter board on the EK-RA8D2 v1 is wired to
192.168.1.42 by default in the matching `ethernet_tcp_echo` example;
this app does not own an IP stack but PTP uses raw L2 multicast
(IEEE 1588-2019 Annex F), so any cable-connected peripheral on the same
broadcast domain will see the messages.

```sh
sudo apt install linuxptp
sudo ptp4l -i eth0 -s -m -2
# -i eth0   : interface to attach to
# -s        : peripheral-only mode
# -m        : print messages to stdout
# -2        : IEEE 802.3 (Layer-2) PTP transport
# Watch for "selected best primary clock" lines naming our 02:00:00:00:00:01 MAC
# and "controller offset" lines that should converge to a small (sub-microsecond) value.
```

The IP address `192.168.1.42` mentioned in the task brief is *only*
used by the `ethernet_tcp_echo` example for its RFC 793 listener; PTP
itself uses Layer-2 multicast addressing (`01:1B:19:00:00:00` for the
default forwardable PTP multicast group, IEEE 1588-2019 sec 19.3.5) so
no IP configuration is needed on this app.

## SCI8 logs

The on-board J-Link OB CDC bridge prints:

- `ra8d2: link up` when the PHY first reports BMSR.LINK_STATUS.
- `ra8d2: PTP controller ready (1 Hz Sync, domain 0)` once init completes.
- `ptp: <sec>.<nsec> s` once per Sync iteration so the operator can
  see the wall clock advance.

## Build + flash

```sh
make ptp_time_transmitter
make -C examples/ptp_time_transmitter flash
```

## What the firmware does

1. `ra_cgc_init()` -- XTAL + PLL1 -> CPUCLK0 = 1 GHz, PCLKA = 125 MHz.
2. `ra_time_init(cpuclk0_hz)` -- SysTick for `ra_delay_ms`.
3. SCI8 logging on PD_02 / PD_03 + LED1 (P6_00) heartbeat.
4. RMII pin-mux on P7 + P4 (eleven pins, all PSEL = 0x11).
5. `ra_eth_open` with MAC `02:00:00:00:00:01` and the default 8/8
   descriptor ring sizing.
6. Polling `ra_eth_link_status` until BMSR.LINK_STATUS = 1.
7. `ra_ptp_open` with domain 0, sync interval 1 Hz, locally-administered
   MAC, and `clock_class = default (248)`.
8. `ra_ptp_set_role(k_ra_ptp_role_controller)` and
   `ra_ptp_set_time(1767225600, 0)` to seed the wall clock to a fixed
   recent Unix epoch (2026-01-01T00:00:00Z).
9. Loop: every second, `ra_ptp_send_sync` + `ra_ptp_send_announce`,
   `ra_ptp_get_time` for the running wall clock, log over SCI8.

## Note on pin map vs EK-RA8D2 v1 board

Same caveat as `ethernet_tcp_echo`: the RMII pin set this app programs
targets a separate RMII-only daughter board, not the v1 board's
on-board PEF7071 PHY (which is wired RGMII per UM Table 26 p 33). To
run on the on-board PHY use `ra_board_ethernet_init()` from the
`ra_board_ek_ra8d2` BSP and run PTP on top of the BSP-routed RMAC0.

## BSP usage

Uses `ra_board_ek_ra8d2` BSP for LED1 init/toggle (P600 per UM Table
24 p 31). Ethernet pins are hand-rolled (see note above).

Validated 2026-05-02 against EK-RA8D2 v1 User's Manual (R20UT5523EG0101
Rev 1.01) Section 6.1 + Table 26 p 33 + Table 24 p 31, and IEEE
1588-2019.
