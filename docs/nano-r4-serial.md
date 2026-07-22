# Arduino Nano R4 - Headless Serial Firmware

`src/rx5808-nano-r4-serial/rx5808-nano-r4-serial.ino` is a separate, minimal
sketch for the **Arduino Nano R4** (Renesas RA4M1). It is not a build of the
main `rx5808-pro-diversity` firmware - the Nano R4 uses a different
microcontroller architecture (ARM Cortex-M4 instead of AVR), so the original
sketch's AVR-specific code (`avr/pgmspace.h`, `PROGMEM`, EEPROM layout) can't
be reused as-is. This sketch reimplements just the RX5808 tuning/RSSI logic
for a single receiver, with no OLED, no buttons, and no EEPROM settings.

It is controlled entirely over serial and reports frequency, RSSI, and a
timestamp for logging or integration with other systems.

## Hardware

Only one RX5808 receiver module is supported (no diversity).

| Signal          | Pin  |
|-----------------|------|
| SPI Data        | D10  |
| SPI Slave Select| D11  |
| SPI Clock       | D12  |
| RSSI (analog)   | A6   |
| Status LED      | Built-in LED |

These match the original project's single-receiver wiring - see
[DIY Simple - Arduino Nano](/docs/diy-arduino-nano.md) for the schematic
(ignore the button/OLED wiring). If your module is wired to different pins,
edit the `#define`s near the top of the `.ino` file.

## Serial interfaces

Both interfaces are active at all times - use whichever is convenient. Any
command sent to either one produces output on both.

- **USB** (`Serial`): 115200 baud, native USB CDC.
- **TX/RX** (`Serial1`, pins D0/D1): 115200 baud.

Commands are plain text, one per line (terminated with `\n`), case
insensitive.

## Commands

| Command        | Description                                                                 |
|----------------|-------------------------------------------------------------------------------|
| `CHANNEL A4`   | Tunes to a channel by band letter + number (bands: `A`, `B`, `E`, `F`, `R`, `L`; numbers `1`-`8`). |
| `SWEEP`        | Tunes across every channel, measuring RSSI at each, and reports the 5 strongest. Returns to the previously tuned channel afterward. |
| `SCAN BEST`    | Same as `SWEEP`, but then tunes to the single strongest channel found.       |
| `STREAM`       | Starts continuously streaming frequency/RSSI/timestamp for the current channel (every 50ms). |
| `STREAM OFF`   | Stops streaming.                                                              |

## Output format

All output lines are comma-separated with a leading tag identifying the
message type:

```
READY,rx5808-nano-r4-serial
OK,CHANNEL,<channel>,<frequency_mhz>
ERR,<reason>

DATA,<timestamp_ms>,<channel>,<frequency_mhz>,<rssi_raw>,<rssi_percent>

SWEEP,START
SWEEP,<rank 1-5>,<channel>,<frequency_mhz>,<rssi_raw>,<rssi_percent>
SWEEP,DONE

SCAN,START
BEST,1,<channel>,<frequency_mhz>,<rssi_raw>,<rssi_percent>
```

`timestamp_ms` is milliseconds since boot (`millis()`), not wall-clock time -
the Nano R4 has no RTC. Pair it with the time you receive the line on the
host if you need a wall-clock timestamp.

`rssi_raw` is the raw 10-bit ADC reading (0-1023). `rssi_percent` maps that
using `RSSI_MIN_VAL`/`RSSI_MAX_VAL` in the sketch (defaults: 90/220, same as
the main firmware) - every RX5808 module reads slightly differently, so
adjust those constants if the percentage seems off. This has no effect on
the raw value.

### Example session

```
> CHANNEL A4
< OK,CHANNEL,A4,5805

> STREAM
< OK,STREAM,ON
< DATA,10234,A4,5805,182,72
< DATA,10284,A4,5805,180,71
...
> STREAM OFF
< OK,STREAM,OFF

> SWEEP
< SWEEP,START
< SWEEP,1,F2,5760,205,94
< SWEEP,2,A4,5805,182,72
< SWEEP,3,R6,5843,150,50
< SWEEP,4,B1,5733,120,29
< SWEEP,5,E3,5665,95,4
< SWEEP,DONE

> SCAN BEST
< SCAN,START
< BEST,1,F2,5760,205,94
```

## Flashing

1. In the Arduino IDE, install the **Arduino UNO R4 Boards** package (Tools
   > Board > Boards Manager), which also covers the Nano R4.
2. Select **Arduino Nano R4** as the board.
3. Open `src/rx5808-nano-r4-serial/rx5808-nano-r4-serial.ino` and upload.
