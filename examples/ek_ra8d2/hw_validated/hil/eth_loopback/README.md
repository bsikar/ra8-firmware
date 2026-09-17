# eth_loopback

Runs ETHA port 0 through the full descriptor-ring lifecycle in MAC-only internal
loopback -- init, CONFIG mode, ring init, OPERATION mode, traffic accounting,
stats, deinit -- and reports success on the console. No PHY, no wire peer and no
off-chip traffic, which is also why the console line is the only possible
verifier: there is nothing on the wire to probe.
