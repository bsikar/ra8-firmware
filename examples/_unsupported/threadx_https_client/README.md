# threadx_https_client

ThreadX plus NetX Duo plus Mbed TLS: one worker thread brings NetX Duo up on a
static IPv4 address, dials a hard-coded HTTPS peer, completes a TLS handshake
and dumps the head of the response to the console.

There is no DNS and the peer address is a compile-time constant on purpose.
This app exists to exercise the TLS, certificate-pinning and TRNG integration,
not name resolution.

## Certificate pinning is armed by default

The peer's leaf certificate is pinned by SHA-256 as a constant in `main.c`, and
the placeholder ships as 32 zero bytes -- so an un-customised flash
deliberately refuses to send the request rather than silently trusting whatever
answers. Fill in the real digest, and expect to refill it, because leaf
certificates rotate every few months.

## No crypto runs on RSIP hardware

Mbed TLS 4.x dropped the 3.x `*_ALT` hooks in favour of the PSA driver-wrapper
interface, and this repo's `port/mbedtls/` is config headers only -- no
hardware fast paths -- so the handshake runs the portable C primitives. That is
not a gap waiting to be filled: the RSIP AES and SHA engines are not functional
on this silicon. The one piece of hardware in the path is the RSIP TRNG, which
reaches PSA through the external-RNG hook.

## What it is waiting for

A route to the public internet off the board's RMII connector, which means
bridging or NAT-ing a workstation interface onto the board's subnet. Board pins
follow EK-RA8D2 v1 UM Section 6.1 and Tables 13 p 24, 24 p 31 and 26 p 33.
