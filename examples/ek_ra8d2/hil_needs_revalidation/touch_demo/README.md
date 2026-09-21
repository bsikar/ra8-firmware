# touch_demo

Standalone GoodIX GT911 capacitive-touch bring-up (#122). The GT911 driver was
previously only exercised inside `ereader_ui`, with no standalone example and no
gate of its own.

`ra8_touch_open()` runs the real driver: it configures IIC_B channel 0, wakes the
GT911 by reading back its product-id string, and clears the status byte. The app
then polls `ra8_touch_read()` -- statically bounded -- for one touch frame and
reports the first decoded contact.

The bring-up half is the deterministic, **finger-free** part: reaching the
product-id check proves the whole `ra8_touch` -> IIC_B -> GT911 path came up, and
it holds whether or not anything is touching the panel. Keying the automated
check on that rather than on a decoded coordinate is what makes it stable on an
unattended bench, where the contact count is legitimately zero.

On the bench the GT911 lives on the ereader carrier's I2C0 bus. Touch a finger
to the panel and the banner reports that contact's panel-native coordinate; the
driver is the same one `ereader_ui` uses.
