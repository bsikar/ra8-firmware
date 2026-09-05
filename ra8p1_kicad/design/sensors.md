# E-Reader Sensor and Peripheral Specifications

This document outlines the required sensors, power management, and auxiliary peripherals for the `ereader_rev1` hardware design.

---

## 1. Sensors (Environmental & State Detection)

### Ambient Light Sensor (ALS)
* **Function**: Measures ambient room light intensity.
* **Purpose**: Automatically adjusts the screen front-light brightness and color temperature.
* **Interface**: I2C

### 3-Axis Accelerometer (G-Sensor)
* **Function**: Detects device orientation.
* **Purpose**: Triggers screen rotation (portrait/landscape).
* **Interface**: I2C + Interrupt GPIO

### Hall Effect Sensor
* **Function**: Detects magnetic fields.
* **Purpose**: Triggers automatic wake/sleep when a magnetic protective cover is opened or closed.
* **Interface**: GPIO (Active Low/High)

### Temperature Sensor
* **Function**: Measures temperature near the display panel.
* **Purpose**: Adjusts E-Ink drive waveforms to compensate for physical response differences in warm/cold environments.
* **Interface**: I2C or Thermistor (often integrated into the E-Ink panel or PMIC)

---

## 2. Core Peripherals & Support ICs

### E-Ink Power Management IC (EPDC PMIC)
* **Purpose**: Generates the high positive and negative voltages (+15V, -15V, +22V, -20V) required to physically manipulate the charged pigment particles in the E-Ink display.
* **Example**: Texas Instruments TPS65185 or equivalent.
* **Interface**: I2C (control) + Enable/Interrupt GPIOs

### Capacitive Touch Screen Controller
* **Purpose**: Decodes multi-touch gestures from the capacitive overlay on top of the E-Ink display.
* **Interface**: I2C + Reset + Interrupt GPIOs

### Battery Fuel Gauge
* **Purpose**: Monitors battery voltage, current, state of charge (percentage), and health.
* **Interface**: I2C

### Dual-Channel LED Front-Light Driver
* **Purpose**: Drives the LEDs for front-lighting. Supports two independent channels to blend warm (amber) and cool (white) LEDs for color temperature adjustment.
* **Interface**: PWM or I2C

### USB-C Charger & Protection Controller
* **Purpose**: Handles lithium-polymer battery charging, USB-C CC line detection (for power delivery compatibility), overvoltage protection, and thermal monitoring.
* **Interface**: I2C (or standalone status pins)

### Haptic Motor Driver (Optional)
* **Purpose**: Drives a linear resonant actuator (LRA) or eccentric rotating mass (ERM) motor to provide tactile page-turn feedback.
