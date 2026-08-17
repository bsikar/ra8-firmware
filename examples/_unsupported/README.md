# examples/_unsupported/

Apps that cannot be hardware-validated on a stock **EK-RA8D2 v1**: each needs
extra hardware, a vendor binary blob, or a Renesas add-on board this project
does not own. They are kept as reference implementations, and they still have
to cross-compile and satisfy every static gate -- but nothing in CI flashes
them, so expect bit-rot that a refactor will not catch. Each app's own README
says what it is waiting for.

A new hardware-dependent app belongs here, so the next person scanning the tree
can tell at a glance what can and cannot be validated. The apps that *are*
validated every release live in [`../ek_ra8d2/`](../ek_ra8d2/README.md).
