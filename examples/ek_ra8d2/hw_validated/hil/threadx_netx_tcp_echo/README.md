# threadx_netx_tcp_echo

ThreadX + NetX Duo serving an RFC 862 TCP echo on `192.168.1.42:7` over the
EK-RA8D2's R-Switch ethernet path (ETHA + RMAC + GWCA descriptor DMA), logging
each served echo to the console.

**This is the only app in the tree that puts application data on an Ethernet
wire.** It is therefore the integration check for TX data-corruption
regressions (#499). Do not demote it without first distinguishing a bench
failure from a firmware failure.

On hardware an isolated test peer uses `192.168.1.1/24` on its board-facing
wired interface, while the firmware retains its documented default address
`192.168.1.42`. The peer opens TCP port 7, sends random bytes, and asserts a
byte-exact echo. These addresses describe the self-contained test link, not a
site network. Payloads from a handful of bytes up to a kilobyte have been
verified; the driver's MTU clamp plus IP fragmentation keeps every individual
frame clear of the accepted large-frame TX silicon limitation (#21).

Headless, the emulator ships the peer in-process: it models the R-Switch
register cluster and its virtual host resolves the firmware over ARP, pings it,
connects to port 7 and byte-verifies the echo, so the verdict asserts both the
firmware's served-echo banner and the peer's own report.
