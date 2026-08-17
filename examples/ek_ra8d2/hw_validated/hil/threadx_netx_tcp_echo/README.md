# threadx_netx_tcp_echo

ThreadX + NetX Duo serving an RFC 862 TCP echo on `192.168.1.42:7` over the
EK-RA8D2's R-Switch ethernet path (ETHA + RMAC + GWCA descriptor DMA), logging
each served echo to the console.

**This is the only app in the tree that puts anything on a wire**, which
matters more than the demo does. While it sat parked under a blocker that
turned out to be wrong, a TX data-corruption regression lived on `dev` for a
month with nothing able to see it (#499). Do not demote this app without first
establishing that the failure really is the bench and not the firmware.

On hardware the peer is the bench Pi: it assigns itself `192.168.1.1` on its
board-facing wired interface, opens TCP to the board, sends random bytes and
asserts a byte-exact echo. Verified byte-exact across payloads from a handful
of bytes up to a kilobyte -- the driver's MTU clamp plus IP fragmentation keeps
every individual frame clear of the accepted large-frame TX silicon limitation
(#21).

Headless, the emulator ships the peer in-process: it models the R-Switch
register cluster and its virtual host resolves the firmware over ARP, pings it,
connects to port 7 and byte-verifies the echo, so the verdict asserts both the
firmware's served-echo banner and the peer's own report.
