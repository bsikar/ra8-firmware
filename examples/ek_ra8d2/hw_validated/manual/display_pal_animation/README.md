# display_pal_animation

Reference example for `libs/ra8_display_pal`: six horizontal RGB565 colour bars
scrolling vertically, one step per frame, through the PAL's public API only --
there is no call to `ra8_glcdc_*` anywhere in the app.

The point is that the app does not know what panel it is driving. It asks the
bound backend for its capabilities and picks a fast refresh on a continuously
scanned panel or a quality refresh on a bistable one; retargeting it at the
e-ink backend is one line of the display config and nothing else changes.

Someone has to watch the panel. The blue board LED toggles once per frame as a
liveness signal and the red LED comes on if any PAL call fails, but whether the
bars are actually painted, and actually scroll, is a human judgement.
