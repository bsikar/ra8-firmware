# i3c_i2c_peripheral_demo

IIC_B peripheral-mode demo. The chip listens at a fixed 7-bit I2C address on
SDA/SCL and toggles LED1 on every successful transaction. There is **no UART
output** -- LED1 is the only observable signal, which also means a UART scrape
can never gate this app.

## Blocked on

An external I2C controller on the bus. The chip is in target mode and waits for a
controller to initiate transactions; the bench Pi has I2C controller capability
on its own header, but there is no jumper wiring to the EVM's IIC_B pins today.
Closing the gap needs both that wiring and a probe-able transfer counter in
`main.c`, since the LED alone gives an automated rig nothing to read.
