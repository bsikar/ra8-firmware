# c6_wifi_join

[#492](https://github.com/bsikar/ra8-firmware/issues/492): the first application
on this board that takes a real network all the way up over the ESP32-C6. It
associates the co-processor's Wi-Fi station with a bench access point through
`libs/ra8_c6link`, runs a NetX Duo DHCP client over the C6 link driver to obtain
a lease, and pings the leased gateway to show the path carries traffic.

`c6_wifi_link` stops at "the radio is up and has an address", which is exactly
the state an IP driver starts from. This app is that IP driver.
`wifi_hal_join` does the same job through the `ra8_wifi` facade.

## The C6 is only a layer-2 bridge

It forwards 802.3 frames between the Wi-Fi netif and the host and does nothing
else, so ARP, DHCP and ICMP all run on the RA8 over NetX Duo, exactly as the
on-chip Ethernet apps do. The only new part is the link driver: it transmits by
handing NetX's framed packet to the facade and receives through the facade's
802.3 callback on a dedicated poll worker. The C6 link is a single polled SPI
transport, so transmit and the receive poll are serialised behind one mutex
inside the driver.

PASS is gated on the DHCP lease, not on the ping. A lease is a full
DISCOVER/OFFER/REQUEST/ACK exchange and already proves traffic flows in both
directions; the ping is reported separately so that a gateway which filters ICMP
cannot turn a real success into a failure.

## Credentials

The SSID and passphrase are not in this tree. They are compiled in at build time
from the environment or the gitignored `coprocessor/esp32c6/wifi.env`, and the
bench passphrase lives in OpenBao. Built with neither set the image still links
and says so at runtime rather than baking a blank credential, so no secret is
ever committed -- but the passphrase does end up in the ELF, as it must for any
supplicant, so treat a credentialed image as a secret.
