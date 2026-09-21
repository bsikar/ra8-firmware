# audio_loopback

Pushes stereo PCM to the EK-RA8D2's on-board DA7212 CODEC over SSIE0, driven
entirely through the board layer's audio bring-up rather than a hand-rolled
SSIE sequence. Sound comes out of the AUDIO OUT jack.

Never verified end to end on silicon: it builds, and the CODEC pin routing
matches the board manual, but nobody has heard it. The main loop also feeds
silence -- the "loopback" in the name is a holdover from an earlier
hand-written SSIE test and the app is playback-only. Swap the silence buffer
for a sine table to hear anything.

**J41 must be jumpered for the CODEC.** Two of the CODEC's SSIE data pins are
shared with the parallel-camera data lines, so the board wires them to one
peripheral or the other, never both. If the camera is fitted, this app is mute
by construction.

When it does get bench time, the SSIE status register's overrun and underrun
flags are the first thing to read: a starved or over-fed FIFO is the usual
reason a correct-looking bring-up produces nothing.

Pin assignments follow EK-RA8D2 v1 UM Table 32 "Audio CODEC Port Pin
Assignments" p 38 and Section 6.6; the peripheral is HUM R01UH1065EJ0130 Ch 46
"SSIE".
