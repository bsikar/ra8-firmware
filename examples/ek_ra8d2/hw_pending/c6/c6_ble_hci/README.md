# c6_ble_hci

Pending C6 HIL bring-up application.

Binds `ra8_ble` (`libs/ra8_hal/inc/ra8_ble.h`) onto `ra8_c6link` (`ESP_HCI_IF`)
via `port/nimble/src/ble_hci_c6link.c`, probes ESP32-C6 co-processor readiness,
and issues real Bluetooth Core H4 commands over SPI:

1. `HCI_Reset` (`0x0C03`)
2. `HCI_Read_BD_ADDR` (`0x1009`)
3. `LE_Set_Advertising_Parameters` (`0x2006`)
4. `LE_Set_Advertising_Data` (`0x2008`)
5. `LE_Set_Advertising_Enable` (`0x200A`)

Hardware requirements match `examples/ek_ra8d2/hw_validated/c6/` (`SW4`
`1=OFF 2=OFF 3=ON 4=OFF`, C6 harness on Pmod1 / J26, C6 self-powered over USB).
