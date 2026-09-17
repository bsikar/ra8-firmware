# ssie_audio_loop

SSIE I2S audio-integrity check on a bare EK-RA8D2. Brings up SSIE0 in
controller / I2S mode (16-bit data, 32-bit system word, AUDIO_MCK / 32 bit
clock divider) and round-trips a short sine pattern through the FIFOs.

Despite the name, no codec, speaker or microphone is involved: the loopback is
internal to SSIE0, TX FIFO into RX FIFO without ever leaving the chip, which is
what lets it run unattended. A real audio demo through an external CODEC is a
different app and needs hardware this one does not.
