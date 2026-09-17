# agt_cascade_demo

Chains AGT0 and AGT1 into one 32-bit virtual counter, toggling LED1 and logging
the running tick count on every AGT1 underflow.

AGT0 counts PCLKB and its underflow feeds AGT1's count source. That source
selection -- `AGTMR1.TCK[2:0] = 101b`, "underflow event signal from AGT0" --
exists on AGT1 only, which is why the cascade has a fixed direction and cannot
be built the other way round (HUM Ch 24.2.5 "AGTMR1" p 1168 note 6).

Each underflow stops and re-arms both halves, which is how `AGTCR.TUNDF` gets
cleared.
