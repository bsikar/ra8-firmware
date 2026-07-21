# examples/ek_ra8d2/hw_validated/

Apps here have been confirmed working on a stock EK-RA8D2 board.  They are
split into three subdirectories based on how much HIL CI can automate:

| Subdir | Count | CI test type |
|--------|-------|-------------|
| `uart/` | 23 | Flash + assert expected string on /dev/ttyACM0 (strong) |
| `smoke/` | 30 | Flash + verify CPU alive via J-Link (weak; covers LED/USB/network apps) |
| `manual/` | 3 | Build only; run/verify requires physical interaction or special equipment |

To build any app: `make <appname>` from the repo root.

For apps to promote from `hw_pending/`: move with `git mv`, add to the
appropriate subdir here, and add a HIL entry in `scripts/hil/suite.sh`.
