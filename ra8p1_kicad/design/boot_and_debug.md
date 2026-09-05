# MCU Architecture: Boot Modes, Debugging, and Feature Usage

This document explains the RA8D2 boot configuration, debugging interface, and lists the internal MCU features that will be used or omitted in the `ereader_rev1` design.

---

## 1. Boot Modes & Configuration

The RA8D2 selects its operating mode based on the state of the **MD (Mode) pin** (port pin `P201`) at the moment the device is released from reset.

| Operating Mode | MD Pin Level | J16 Connection (Eval Board) | Function / Description |
| :--- | :---: | :---: | :--- |
| **Normal Operation (Single-Chip Mode)** | **High (1)** | Open (or Pins 2-3) | Bypasses the bootloader; CPU executes user application code directly from the main internal Flash. |
| **SCI / USB Boot Mode** | **Low (0)** | Closed (Pins 1-2) | Boots into the internal factory ROM bootloader to flash code via UART or USB. |

### Design Recommendation for Production:
* **Default Mode**: Pull the MD pin (`P201`) **High** using a $4.7\text{ k}\Omega$ or $10\text{ k}\Omega$ resistor to $3.3\text{ V}$. This ensures the e-reader boots directly into the user interface on power-up.
* **Programming Mode Access**: Connect the MD pin to a small unpopulated test point or jumper footprint on the PCB. During factory programming, this pin can be grounded (pulled Low) to boot the MCU into loader mode for the initial software installation.

---

## 2. Debugging and Programming (JTAG/SWD)

The J-Link On-Board (J-Link OB) debugger circuit found on the evaluation board (which uses an auxiliary RA4M2 MCU) must be **omitted** from the production schematic to minimize board cost, size, and power consumption.

Instead, expose the MCU's **Serial Wire Debug (SWD)** pins to connect an external J-Link debugger/programmer.

### Required SWD Debug Pins:
1. **SWCLK** (Clock)
2. **SWDIO** (Data I/O)
3. **RESET** (System Reset)
4. **GND** (Ground reference)
5. **VCC** (Target voltage sense, $3.3\text{ V}$)

### Layout Implementation Options:
* **For Prototypes**: Route the 5 SWD lines to a small 10-pin $1.27\text{ mm}$ pitch header (such as a Samtec FTSH) or a footprint-less **Tag-Connect** header for easy debugger connection.
* **For Production Assembly**: Route the lines to small copper **Test Points** on the bottom of the PCB. The factory programming fixture will use spring-loaded pogo-pins to make contact and flash the board.

---

## 3. MCU Features: What We Use vs. What We Omit

To optimize pins, layout complexity, and power, the RA8D2 features will be utilized as follows:

### Omitted Features (Do Not Route/Connect):
* **Gigabit Ethernet**: Unused (e-readers rely on wireless connectivity).
* **Camera Interface (CEU)**: Unused (no camera is needed).
* **Audio Interfaces (PDM, SSIE Audio Codec)**: Unused (no microphone or audio codec needed).
* **MIPI DSI Graphic Connector**: Unused (replaced by E-Ink interface).
* **Parallel Graphics LCD Interface**: Unused (replaced by E-Ink interface).
* **USB Full-Speed (USBFS)**: Unused (rely solely on the faster USBHS port).

### Active/Repurposed Features (Route to Respective Peripherals):
* **Arm Helium (Vector Extensions)**: Leveraged internally by the CPU for fast PDF rendering, image processing/dithering, and text searches.
* **2D Graphics Engine (DRW)**: Used to compose user interface elements and render pages into the frame buffer in external RAM.
* **USB High-Speed (USBHS)**: Connected directly to the USB-C port (via ESD protection) for 480 Mbps data transfers and factory USB bootloader programming.
* **External Bus Interface (EXBUS)**: Used to connect:
  * External **SDRAM** (for system memory and frame buffers).
  * High-speed parallel connection to the **E-Ink Display Controller** (e.g., IT8951).
* **Octal SPI (OSPI)**: Used to interface with high-speed external SPI Flash for OS, fonts, and library storage.
* **SDIO (SD/MMC Host)**: Connected to the Wi-Fi/Bluetooth wireless module.
* **I2C Channels**: Connected to the capacitive touch screen controller, battery fuel gauge, accelerometer, and ambient light sensor.
* **PWM Channels (Timers)**: Connected to the dual-channel LED front-light driver to adjust brightness and color temperature.
