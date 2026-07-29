/*
 * rx5808-nano-v3-serial
 *
 * Headless RX5808 receiver firmware for the classic Arduino Nano 3.0
 * (ATmega328). Same serial protocol as the Nano R4 variant
 * (rx5808-nano-r4-serial), adapted for the AVR:
 *
 *   - One serial port only. On the Nano V3 the USB connector and the D0/D1
 *     (RX/TX) pins are the same hardware UART, so use one or the other -
 *     not both at once.
 *   - Fixed strings live in flash (F()/PSTR) because the ATmega328 only has
 *     2KB of RAM.
 *   - No analogReadResolution() - the AVR ADC is always 10-bit (0-1023),
 *     matching the R4 sketch's configured scale.
 *
 * Single receiver (no diversity), no OLED, no buttons, no EEPROM.
 *
 * SPI driver based on fs_skyrf_58g-main.c Written by Simon Chambers
 *
 * The MIT License (MIT)
 *
 * Copyright (c) 2015 Marko Hoepken
 *
 * Permission is hereby granted, free of charge, to any person obtaining a copy
 * of this software and associated documentation files (the "Software"), to deal
 * in the Software without restriction, including without limitation the rights
 * to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
 * copies of the Software, and to permit persons to whom the Software is
 * furnished to do so, subject to the following conditions:
 *
 * The above copyright notice and this permission notice shall be included in all
 * copies or substantial portions of the Software.
 *
 * THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
 * IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
 * FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
 * AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
 * LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
 * OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
 * SOFTWARE.
 */

#include <Arduino.h>
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>


// === Pins ====================================================================
//
// Matches the original rx5808-pro-diversity single-receiver wiring: 1k series
// resistors in the three SPI lines, 100k pull-down from RSSI to GND.
// Change these if your RX5808 module is wired to different pins.
//
// =============================================================================

#define PIN_SPI_DATA 10
#define PIN_SPI_SLAVE_SELECT 11
#define PIN_SPI_CLOCK 12
#define PIN_RSSI A6 // A6 is analog-only on the Nano V3, which is all we need.

// === Serial ==================================================================

#define SERIAL_BAUD 115200

// === Tuning ===================================================================

// Time to let RSSI settle after retuning to a new channel, in milliseconds.
#define CHANNEL_SETTLE_MS 30

// How often STREAM emits a data line, in milliseconds.
#define STREAM_INTERVAL_MS 50

// RSSI raw ADC calibration range, used only to compute the 0-100 percent
// value. Every RX5808 module reads slightly differently, so adjust these if
// the percent reading seems off - it has no effect on the raw value.
#define RSSI_MIN_VAL 90
#define RSSI_MAX_VAL 220

// Comment this out if you don't want the extra 8 L-Band / 5.3GHz channels.
#define USE_LBAND


// === Channel table ===========================================================

struct Band {
    char letter;
    uint16_t frequencies[8];
};

static const Band BANDS[] = {
    { 'A', { 5865, 5845, 5825, 5805, 5785, 5765, 5745, 5725 } },
    { 'B', { 5733, 5752, 5771, 5790, 5809, 5828, 5847, 5866 } },
    { 'E', { 5705, 5685, 5665, 5645, 5885, 5905, 5925, 5945 } },
    { 'F', { 5740, 5760, 5780, 5800, 5820, 5840, 5860, 5880 } },
    { 'R', { 5658, 5695, 5732, 5769, 5806, 5843, 5880, 5917 } },
    #ifdef USE_LBAND
        { 'L', { 5362, 5399, 5436, 5473, 5510, 5547, 5584, 5621 } },
    #endif
};

#define BAND_COUNT (sizeof(BANDS) / sizeof(BANDS[0]))
#define CHANNELS_PER_BAND 8


// === State ====================================================================

static uint8_t currentBand = 0;
static uint8_t currentChannel = 0;
static bool streaming = false;
static uint32_t nextStreamTick = 0;

static char lineBuf[64];

struct CommandBuffer {
    char data[32];
    uint8_t length = 0;
};

static CommandBuffer commandBuffer;

struct SweepResult {
    uint8_t band;
    uint8_t channel;
    uint16_t rssiRaw;
};

struct ChannelRef {
    bool valid;
    uint8_t band;
    uint8_t channel;
    uint16_t freq;
};


// === RX5808 SPI (bit-banged) ==================================================
//
// Format is LSB first, with the following bits in order:
//     4 bits - address
//     1 bit  - read/write enable
//    20 bits - data
//
// Address for frequency select (Synth Register B) is 0x1. Expected data is
// (LSB): 7 bits A counter divider ratio, 1 bit separator, 12 bits N counter
// divider ratio.
//
// F_lo = 2 * (N * 32 + A) * (F_osc / R), where F_osc = 8MHz, R = 8.
//
// Refer to the RTC6715 datasheet for further details.

#define SPI_ADDRESS_SYNTH_A 0x01

static inline void spiSendBit(uint8_t value) {
    digitalWrite(PIN_SPI_CLOCK, LOW);
    delayMicroseconds(1);

    digitalWrite(PIN_SPI_DATA, value);
    delayMicroseconds(1);
    digitalWrite(PIN_SPI_CLOCK, HIGH);
    delayMicroseconds(1);

    digitalWrite(PIN_SPI_CLOCK, LOW);
    delayMicroseconds(1);
}

static inline void spiSendBits(uint32_t bits, uint8_t count) {
    for (uint8_t i = 0; i < count; i++) {
        spiSendBit(bits & 0x1);
        bits >>= 1;
    }
}

static void spiSetSynthRegisterB(uint16_t value) {
    digitalWrite(PIN_SPI_SLAVE_SELECT, LOW);
    delayMicroseconds(1);

    spiSendBits(SPI_ADDRESS_SYNTH_A, 4);
    spiSendBit(HIGH); // Enable write.
    spiSendBits(value, 20);

    digitalWrite(PIN_SPI_SLAVE_SELECT, HIGH);
    delayMicroseconds(1);
    digitalWrite(PIN_SPI_CLOCK, LOW);
    digitalWrite(PIN_SPI_DATA, LOW);
}

static uint16_t computeSynthRegisterB(uint16_t freqMhz) {
    uint16_t fLo = (freqMhz - 479) / 2;
    uint16_t nCounter = fLo / 32;
    uint16_t aCounter = fLo % 32;
    return (nCounter << 7) | aCounter;
}


// === Channel helpers ==========================================================

static void channelName(uint8_t bandIdx, uint8_t chIdx, char *out) {
    out[0] = BANDS[bandIdx].letter;
    out[1] = '1' + chIdx;
    out[2] = '\0';
}

static int8_t findBandIndex(char letter) {
    for (uint8_t i = 0; i < BAND_COUNT; i++) {
        if (BANDS[i].letter == letter) {
            return i;
        }
    }
    return -1;
}

static void tuneTo(uint8_t bandIdx, uint8_t chIdx) {
    currentBand = bandIdx;
    currentChannel = chIdx;
    spiSetSynthRegisterB(computeSynthRegisterB(BANDS[bandIdx].frequencies[chIdx]));
}

// Finds the channels with the next-lower and next-higher frequency compared
// to `freq`, across every band (not just the current one) - true RF
// neighbors, since band channel numbering doesn't run in frequency order.
static void findFrequencyNeighbors(uint16_t freq, ChannelRef &below, ChannelRef &above) {
    below.valid = false;
    above.valid = false;

    for (uint8_t b = 0; b < BAND_COUNT; b++) {
        for (uint8_t c = 0; c < CHANNELS_PER_BAND; c++) {
            uint16_t f = BANDS[b].frequencies[c];

            if (f < freq && (!below.valid || f > below.freq)) {
                below = { true, b, c, f };
            }
            if (f > freq && (!above.valid || f < above.freq)) {
                above = { true, b, c, f };
            }
        }
    }
}

static uint16_t readRssiRaw() {
    analogRead(PIN_RSSI); // Fake read to let the ADC settle.
    return analogRead(PIN_RSSI);
}

static uint8_t rssiToPercent(uint16_t raw) {
    long percent = map(raw, RSSI_MIN_VAL, RSSI_MAX_VAL, 0, 100);
    return (uint8_t) constrain(percent, 0, 100);
}


// === Output ====================================================================

static void printLine(const __FlashStringHelper *msg) {
    Serial.println(msg);
}

static void printError(const __FlashStringHelper *reason) {
    Serial.print(F("ERR,"));
    Serial.println(reason);
}

static void printChannelAck() {
    char name[3];
    channelName(currentBand, currentChannel, name);
    snprintf_P(
        lineBuf, sizeof(lineBuf), PSTR("OK,CHANNEL,%s,%u"),
        name, BANDS[currentBand].frequencies[currentChannel]
    );
    Serial.println(lineBuf);
}

// DATA,<timestamp_ms>,<channel>,<frequency_mhz>,<rssi_raw>,<rssi_percent>
static void emitDataLine() {
    uint16_t raw = readRssiRaw();
    char name[3];
    channelName(currentBand, currentChannel, name);

    snprintf_P(
        lineBuf, sizeof(lineBuf), PSTR("DATA,%lu,%s,%u,%u,%u"),
        (unsigned long) millis(), name,
        BANDS[currentBand].frequencies[currentChannel],
        raw, rssiToPercent(raw)
    );
    Serial.println(lineBuf);
}

// SWEEP,<rank>,<channel>,<frequency_mhz>,<rssi_raw>,<rssi_percent>
static void emitSweepLine(const char *tag, uint8_t rank, const SweepResult &r) {
    char name[3];
    channelName(r.band, r.channel, name);

    snprintf_P(
        lineBuf, sizeof(lineBuf), PSTR("%s,%u,%s,%u,%u,%u"),
        tag, rank, name, BANDS[r.band].frequencies[r.channel],
        r.rssiRaw, rssiToPercent(r.rssiRaw)
    );
    Serial.println(lineBuf);
}

// CHECK,<role>,<channel>,<frequency_mhz>,<rssi_raw>,<rssi_percent>
static void emitCheckLine(const char *role, uint8_t band, uint8_t channel, uint16_t raw) {
    char name[3];
    channelName(band, channel, name);

    snprintf_P(
        lineBuf, sizeof(lineBuf), PSTR("CHECK,%s,%s,%u,%u,%u"),
        role, name, BANDS[band].frequencies[channel], raw, rssiToPercent(raw)
    );
    Serial.println(lineBuf);
}


// === Commands ==================================================================

// Tunes across every channel, taking an RSSI reading at each, and returns the
// top 5 (by raw RSSI) sorted from strongest to weakest. If worstOut is
// non-NULL, also fills it in with the single weakest channel found. Restores
// whatever channel was tuned before the sweep started.
//
// (No default argument here on purpose - the Arduino IDE's auto-generated
// prototypes don't play well with default arguments in .ino files.)
static uint8_t performSweep(SweepResult top[5], SweepResult *worstOut) {
    uint8_t topCount = 0;
    bool worstSet = false;
    uint8_t savedBand = currentBand;
    uint8_t savedChannel = currentChannel;

    for (uint8_t b = 0; b < BAND_COUNT; b++) {
        for (uint8_t c = 0; c < CHANNELS_PER_BAND; c++) {
            tuneTo(b, c);
            delay(CHANNEL_SETTLE_MS);
            uint16_t raw = readRssiRaw();

            if (topCount < 5 || raw > top[4].rssiRaw) {
                uint8_t insertPos = topCount < 5 ? topCount : 4;
                while (insertPos > 0 && top[insertPos - 1].rssiRaw < raw) {
                    top[insertPos] = top[insertPos - 1];
                    insertPos--;
                }
                top[insertPos] = { b, c, raw };
                if (topCount < 5) {
                    topCount++;
                }
            }

            if (worstOut != NULL && (!worstSet || raw < worstOut->rssiRaw)) {
                *worstOut = { b, c, raw };
                worstSet = true;
            }
        }
    }

    tuneTo(savedBand, savedChannel);
    delay(CHANNEL_SETTLE_MS);

    return topCount;
}

static void handleChannelCommand(char *arg) {
    if (arg == NULL || strlen(arg) < 2) {
        printError(F("BAD_CHANNEL"));
        return;
    }

    int8_t bandIdx = findBandIndex(arg[0]);
    int channelNum = atoi(arg + 1);

    if (bandIdx < 0 || channelNum < 1 || channelNum > CHANNELS_PER_BAND) {
        printError(F("BAD_CHANNEL"));
        return;
    }

    tuneTo(bandIdx, channelNum - 1);
    printChannelAck();
}

static void handleSweepCommand() {
    bool wasStreaming = streaming;
    streaming = false;
    printLine(F("SWEEP,START"));

    SweepResult top[5];
    uint8_t count = performSweep(top, NULL);

    for (uint8_t i = 0; i < count; i++) {
        emitSweepLine("SWEEP", i + 1, top[i]);
    }
    printLine(F("SWEEP,DONE"));

    streaming = wasStreaming;
}

static void handleScanBestCommand() {
    bool wasStreaming = streaming;
    streaming = false;
    printLine(F("SCAN,START"));

    SweepResult top[5];
    uint8_t count = performSweep(top, NULL);

    if (count == 0) {
        printError(F("NO_SIGNAL"));
    } else {
        tuneTo(top[0].band, top[0].channel);
        delay(CHANNEL_SETTLE_MS);
        emitSweepLine("BEST", 1, top[0]);
    }

    streaming = wasStreaming;
}

static void handleScanWorstCommand() {
    bool wasStreaming = streaming;
    streaming = false;
    printLine(F("SCAN,START"));

    SweepResult top[5];
    SweepResult worst;
    performSweep(top, &worst);

    tuneTo(worst.band, worst.channel);
    delay(CHANNEL_SETTLE_MS);
    emitSweepLine("WORST", 1, worst);

    streaming = wasStreaming;
}

// Cross-checks that RSSI is actually responding to real tuning changes: reads
// RSSI on the current channel and on its true frequency neighbors (in
// either direction, across all bands), then returns to the original
// channel. This only proves anything when a known transmitter is active on
// the current channel - with no signal present, NO_PEAK is expected and
// does not indicate a problem.
static void handleCheckCommand() {
    bool wasStreaming = streaming;
    streaming = false;
    printLine(F("CHECK,START"));

    uint8_t savedBand = currentBand;
    uint8_t savedChannel = currentChannel;
    uint16_t centerFreq = BANDS[savedBand].frequencies[savedChannel];

    ChannelRef below, above;
    findFrequencyNeighbors(centerFreq, below, above);

    uint16_t belowRssi = 0;
    uint16_t aboveRssi = 0;

    if (below.valid) {
        tuneTo(below.band, below.channel);
        delay(CHANNEL_SETTLE_MS);
        belowRssi = readRssiRaw();
        emitCheckLine("BELOW", below.band, below.channel, belowRssi);
    } else {
        printLine(F("CHECK,BELOW,NONE,0,0,0"));
    }

    tuneTo(savedBand, savedChannel);
    delay(CHANNEL_SETTLE_MS);
    uint16_t centerRssi = readRssiRaw();
    emitCheckLine("CENTER", savedBand, savedChannel, centerRssi);

    if (above.valid) {
        tuneTo(above.band, above.channel);
        delay(CHANNEL_SETTLE_MS);
        aboveRssi = readRssiRaw();
        emitCheckLine("ABOVE", above.band, above.channel, aboveRssi);
    } else {
        printLine(F("CHECK,ABOVE,NONE,0,0,0"));
    }

    tuneTo(savedBand, savedChannel);
    delay(CHANNEL_SETTLE_MS);

    bool isPeak = (!below.valid || centerRssi > belowRssi)
        && (!above.valid || centerRssi > aboveRssi)
        && (below.valid || above.valid);
    if (isPeak) {
        printLine(F("CHECK,RESULT,PEAK"));
    } else {
        printLine(F("CHECK,RESULT,NO_PEAK"));
    }

    streaming = wasStreaming;
}

static void handleHelpCommand() {
    printLine(F("HELP,START"));
    printLine(F("HELP,CHANNEL <letter><num>,Tune to a channel - e.g. CHANNEL A4"));
    printLine(F("HELP,SWEEP,Scan every channel and report the top 5 by RSSI"));
    printLine(F("HELP,SCAN BEST,Scan every channel and tune to the strongest one found"));
    printLine(F("HELP,SCAN WORST,Scan every channel and tune to the weakest one found"));
    printLine(F("HELP,STREAM,Start streaming frequency/RSSI/timestamp for the current channel"));
    printLine(F("HELP,STREAM OFF,Stop streaming"));
    printLine(F("HELP,CHECK,Cross-check RSSI against frequency neighbors to confirm tuning"));
    printLine(F("HELP,HELP,Show this list"));
    printLine(F("HELP,DONE"));
}

static void handleStreamCommand(char *arg) {
    if (arg == NULL || strcmp_P(arg, PSTR("ON")) == 0) {
        streaming = true;
        nextStreamTick = millis();
        printLine(F("OK,STREAM,ON"));
    } else if (strcmp_P(arg, PSTR("OFF")) == 0) {
        streaming = false;
        printLine(F("OK,STREAM,OFF"));
    } else {
        printError(F("BAD_STREAM_ARG"));
    }
}

static void handleCommand(char *line) {
    for (char *p = line; *p; p++) {
        *p = toupper((unsigned char) *p);
    }

    char *cmd = strtok(line, " \t");
    if (cmd == NULL) {
        return;
    }

    if (strcmp_P(cmd, PSTR("CHANNEL")) == 0) {
        handleChannelCommand(strtok(NULL, " \t"));
    } else if (strcmp_P(cmd, PSTR("SWEEP")) == 0) {
        handleSweepCommand();
    } else if (strcmp_P(cmd, PSTR("SCAN")) == 0) {
        char *arg = strtok(NULL, " \t");
        if (arg != NULL && strcmp_P(arg, PSTR("BEST")) == 0) {
            handleScanBestCommand();
        } else if (arg != NULL && strcmp_P(arg, PSTR("WORST")) == 0) {
            handleScanWorstCommand();
        } else {
            printError(F("UNKNOWN_COMMAND"));
        }
    } else if (strcmp_P(cmd, PSTR("STREAM")) == 0) {
        handleStreamCommand(strtok(NULL, " \t"));
    } else if (strcmp_P(cmd, PSTR("CHECK")) == 0) {
        handleCheckCommand();
    } else if (strcmp_P(cmd, PSTR("HELP")) == 0) {
        handleHelpCommand();
    } else {
        printError(F("UNKNOWN_COMMAND"));
    }
}

static void pollCommands() {
    while (Serial.available()) {
        char c = Serial.read();

        if (c == '\r') {
            continue;
        }

        if (c == '\n') {
            commandBuffer.data[commandBuffer.length] = '\0';
            if (commandBuffer.length > 0) {
                handleCommand(commandBuffer.data);
            }
            commandBuffer.length = 0;
        } else if (commandBuffer.length < sizeof(commandBuffer.data) - 1) {
            commandBuffer.data[commandBuffer.length++] = c;
        }
    }
}


// === Arduino entry points ======================================================

void setup() {
    pinMode(PIN_SPI_DATA, OUTPUT);
    pinMode(PIN_SPI_SLAVE_SELECT, OUTPUT);
    pinMode(PIN_SPI_CLOCK, OUTPUT);
    // No pinMode for PIN_RSSI: A6 on the Nano V3 is an analog-only input
    // with no digital pin circuitry behind it.

    digitalWrite(PIN_SPI_SLAVE_SELECT, HIGH);
    digitalWrite(PIN_SPI_CLOCK, LOW);
    digitalWrite(PIN_SPI_DATA, LOW);

    Serial.begin(SERIAL_BAUD);

    tuneTo(0, 0); // Default to channel A1 on boot.
    delay(CHANNEL_SETTLE_MS);

    printLine(F("READY,rx5808-nano-v3-serial"));
}

void loop() {
    pollCommands();

    if (streaming && (int32_t) (millis() - nextStreamTick) >= 0) {
        emitDataLine();
        nextStreamTick = millis() + STREAM_INTERVAL_MS;
    }
}
