# examples/ek_ra8d2/hw_validated/manual/

Hardware-confirmed apps whose acceptance is a human observation: a picture on
the 7-inch LCD, a button pressed, a PC on the far end of a cable. CI builds
them and stops there, and promoting one out of here needs a person to sign it
off.

Most of them are device-mode USB apps, and they are here for a bench reason
rather than a maturity one. The board's two USB jacks are cabled to each other
for the self-loop self-tests in [`../hil/`](../hil/), where a single image
drives both the host and the device role and the two validate each other
unattended. With that loop fitted, a device-mode app's port answers the board's
own host, so verifying it means unplugging the loop and plugging into a PC --
a manual step. Nothing is lost by leaving them here: the self-loop self-tests
cover the same USB classes automatically, and exercise more of the path than an
external host would.

The rest need an eye on the panel, a thumb drive, or a peripheral the bench
does not carry.
