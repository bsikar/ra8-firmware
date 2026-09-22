# c6_mdl_test

Cross-build fixture for the generated typed-artifact protobuf, the RA8 c6link
client, and the transactional transfer coordinator. Protocol v2 carries and
echoes the selected `mdl_format_t`, including `k_mdl_format_rabook`.

It is not hardware-validation evidence. The deterministic multi-chunk / cancel
protocol test is `apps/shared_libs/mdl/tests/src/test_ra8_c6link_mdl.c`; real HIL stays pending
until a
mixed C6 image has been built, flashed, and shown to download more than one
frame with byte-for-byte digest equality. There is deliberately no `hil.conf`
here, because one would claim a success that has not happened.
