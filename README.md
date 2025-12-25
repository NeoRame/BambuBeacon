# BambuBeacon

..... still under construction .....

Parts you need:
- 3x 12bit WS2812 LED Ring
- 1x Wemos D1 Mini ESP32
- Soldering Iron
- some wires
- USB power supply 2A
- Some printed Parts
- BambuLab printer (make less sense without one)

## Wiring Notes ##
- Connect LED data to the GPIO defined by `LED_PIN` in `platformio.ini`
- Share ground between the ESP32 and the LED rings
- Power the LED rings from a stable 5V supply (2A or more recommended)
- Optional but recommended: place a small resistor (~330-470 Ohm) in series with the data line

## Quick Start ##
1. Build and flash the firmware with PlatformIO.
2. Power the device and connect to its Wi-Fi AP.
3. Open the setup page and configure Wi-Fi, then reboot into STA mode.
4. Open Printer Setup and enter printer IP/USN/access key.
5. Set LED ring count (2 or 3) and LEDs per ring (1-64), then save.
6. Verify status updates on the LED rings and check logs via WebSerial if needed.

## Project Description ##
BambuBeacon is an ESP32-based status light for BambuLab printers. It connects to your printer, listens for status updates, and drives multi-ring WS2812 LEDs to visualize printer state, progress, and connectivity in real time. The project includes a built-in web UI for setup, Wi-Fi configuration, and device management.

## Features ##
- Web-based setup for printer IP/USN/access key and device settings
- Wi-Fi AP mode for first-time configuration and recovery
- Real-time status visualization across 2 or 3 LED rings
- Adjustable LED brightness and per-ring LED counts
- DHCP-friendly printer discovery and tracking
- WebSerial console for live logs and troubleshooting
- JSON backup/restore of configuration
