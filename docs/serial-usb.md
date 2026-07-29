# USB Serial Interface (main firmware)

The main `rx5808-pro-diversity` firmware can expose a USB serial command and
telemetry interface alongside the normal OLED/button UI. Unlike the
[headless Nano R4](/docs/nano-r4-serial.md) and
[headless Nano V3](/docs/nano-v3-serial.md) sketches - which replace the UI
entirely - this keeps the full original firmware intact and adds serial on
top, so the screen, buttons, diversity, and saved settings all keep working
as before.

## Enabling

In `src/rx5808-pro-diversity/settings.h`:

```c
#define USE_SERIAL_OUT
```

This is enabled by default. Comment it out to remove the serial code
entirely. It is **not compatible with `USE_IR_EMITTER`** - both want the
one hardware UART, so enable only one.

> **Flash/RAM note:** the ATmega328 build of the full firmware (OLED +
> diversity) is already close to the chip's limits. If enabling this pushes
> you over, free some space by disabling a feature you don't need in
> `settings.h` - `USE_LBAND` and `USE_VOLTAGE_MONITORING` are the usual
> candidates.

Connect at **115200 baud** over the Nano's USB port. (The original
`USE_SERIAL_OUT` used 250000 baud and emitted a fixed tab-separated dump
with no command input; this replaces that with the line protocol below, at
the same baud rate the other sketches and the logging tool use.)

## Commands

Plain text, one per line (`\n`), case insensitive.

| Command      | Description                                                     |
|--------------|-----------------------------------------------------------------|
| `CHANNEL A4` | Tune to a channel by band letter + number (`A`,`B`,`E`,`F`,`R`, plus `L` when `USE_LBAND` is on; numbers `1`-`8`). Also saves it as the startup channel, same as choosing it on the screen. |
| `STREAM`     | Start streaming `DATA` lines (every 50ms).                       |
| `STREAM OFF` | Stop streaming.                                                  |
| `HELP`       | List the commands.                                               |

## Output

```
READY,rx5808-pro-diversity
OK,CHANNEL,<channel>,<frequency_mhz>
ERR,<reason>
DATA,<millis>,<channel>,<frequency_mhz>,<rssiA_raw>,<rssiA_pct>[,<rssiB_raw>,<rssiB_pct>,<active>]
HELP,START / HELP,<command>,<description> / HELP,DONE
```

The three trailing `DATA` fields (B receiver raw/percent and which receiver
is currently active, `A` or `B`) are present only when `USE_DIVERSITY` is
enabled - which it is by default. If you build without diversity, `DATA`
lines end after the A-receiver percent.

`millis` is milliseconds since boot, not wall-clock time. `rssi_*_pct` uses
whatever RSSI calibration is stored in EEPROM (set via the on-screen
Calibrate RSSI menu), so calibrate first if the percentages look wrong -
the raw values are unaffected.

### Example

```
> CHANNEL A4
< OK,CHANNEL,A4,5805
> STREAM
< OK,STREAM,ON
< DATA,10234,A4,5805,180,71,165,64,A
< DATA,10284,A4,5805,182,72,164,63,A
> STREAM OFF
< OK,STREAM,OFF
```

## Interaction with the on-screen UI

Serial and the buttons drive the same receiver, so they cooperate rather
than conflict:

- `CHANNEL` retunes immediately and updates the saved start channel, so the
  new channel shows on the OLED and survives a power cycle.
- Streaming keeps running through screensaver, menu, and settings screens.
- In **Search** mode's auto-scan, pressing a button afterwards resumes
  scanning from where the *scan* left off, not from a channel you set over
  serial. Entering and leaving a menu resyncs it. Use manual mode (or just
  leave the buttons alone) if you're driving channels over serial.
- **Band Scan** and **RSSI calibration** sweep the receiver themselves, so
  `DATA` during those screens reflects the sweep, not a fixed channel.

## Logging

`tools/rx5808_serial_logger.py` works with this firmware for `CHANNEL`,
`STREAM`, and `DATA` lines. Note that its CSV columns were written for the
single-receiver sketches, so the diversity B-receiver fields land outside
the named columns - the raw `.log` file always has the complete lines.
