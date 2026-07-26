# Arduino Nano V3 - Headless Serial Firmware

`src/rx5808-nano-v3-serial/rx5808-nano-v3-serial.ino` is the classic
**Arduino Nano 3.0 (ATmega328)** version of the headless serial firmware.
It speaks the exact same serial protocol as the
[Nano R4 variant](/docs/nano-r4-serial.md) - same commands (`CHANNEL`,
`SWEEP`, `SCAN BEST`, `SCAN WORST`, `STREAM`, `CHECK`, `HELP`), same output
lines - so the same logging script and documentation apply. Refer to the
Nano R4 doc for the full command and output reference; this page only
covers what's different on the V3.

## Differences from the Nano R4 version

- **One serial port, not two.** On the Nano V3 the USB connector and the
  D0/D1 (RX/TX) pins are the *same* hardware UART - the USB chip is just a
  USB-to-serial converter wired to those pins. Use USB **or** the TX/RX
  pins, not both at once (if something is wired to D0/D1 while USB is
  connected, they will fight each other and uploads/comms will fail).
- **Flash-stored strings.** The ATmega328 has only 2KB of RAM, so all the
  fixed text lives in program memory (`F()`/`PSTR()`). No functional
  difference, just an implementation detail.
- **Fixed 10-bit ADC.** No `analogReadResolution()` on AVR - readings are
  natively 0-1023, the same scale the R4 sketch is configured for, so RSSI
  values are directly comparable between the two boards.
- **Board reset on connect.** The Nano V3 auto-resets when the serial port
  opens (DTR), so expect the `READY` line about a second after connecting.

## Hardware

Single RX5808 receiver module (no diversity), wired per the original
project's single-receiver schematic - just the module and the resistors:

| Signal            | Nano pin | Notes                                    |
|-------------------|----------|------------------------------------------|
| RX5808 SPI Data   | D10      | Through a 1k series resistor             |
| RX5808 SPI Select | D11      | Through a 1k series resistor             |
| RX5808 SPI Clock  | D12      | Through a 1k series resistor             |
| RX5808 RSSI       | A6       | With a 100k resistor from RSSI to GND    |
| RX5808 +5V        | 5V       |                                          |
| RX5808 GND        | GND      |                                          |

The 1k series resistors protect the RX5808's 3.3V logic inputs from the
Nano's 5V outputs. The 100k pull-down keeps the RSSI line from floating.
The RX5808 module must have the [SPI mod](/docs/rx5808-spi-mod.md) done.

No buttons, no OLED, no LEDs, no buzzer.

## Flashing

1. In the Arduino IDE select **Board: Arduino Nano**.
2. Set **Processor: ATmega328P**. If the upload fails with a sync error -
   common on inexpensive clone boards - try **ATmega328P (Old Bootloader)**
   instead.
3. Open `src/rx5808-nano-v3-serial/rx5808-nano-v3-serial.ino` and upload.

## Logging

`tools/rx5808_serial_logger.py` works unchanged - the protocol is
identical. See the [Nano R4 doc](/docs/nano-r4-serial.md#logging-to-csv)
for usage.
