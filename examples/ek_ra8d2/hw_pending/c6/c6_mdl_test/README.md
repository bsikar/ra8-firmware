# c6_mdl_test

Cross-build fixture for the generated typed-artifact protobuf, RA8 c6link
client, and transactional transfer coordinator. Protocol v2 carries and echoes
the selected `ra8_mdl_format_t`, including `k_ra8_mdl_format_rabook`.
It is not hardware-validation evidence: the prior contents were copied from
`c6_fw_version` and never exercised media download at all.

The deterministic multi-chunk/cancel protocol test is
`tests/test_ra8_c6link_mdl.c`. Real HIL must remain pending until the mixed C6
image has been built, flashed, and shown to download more than one frame with
byte-for-byte digest equality. There is intentionally no `hil.conf` claiming
success before that run exists.

```sh
make build
```
