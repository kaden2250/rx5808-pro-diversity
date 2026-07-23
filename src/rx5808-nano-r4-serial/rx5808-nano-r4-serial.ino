/*
 * rx5808-nano-r4-serial
 *
 * Headless RX5808 receiver firmware for the Arduino Nano R4.
 *
 * This is a stripped-down, single-receiver (no diversity) variant of
 * rx5808-pro-diversity built for boards with no OLED and no buttons. All
 * control happens over serial - both the native USB port (Serial) and the
 * hardware TX/RX pins (Serial1) are read for commands and are sent the same
 * output, so you can use whichever interface is convenient.
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
// Defaults match the original rx5808-pro-diversity single-receiver wiring.
// Change these if your RX5808 module is wired to different pins.
//
// =============================================================================

#define PIN_SPI_DATA 10
#define PIN_SPI_SLAVE_SELECT 11
#define PIN_SPI_CLOCK 12
#define PIN_RSSI A6

// === Serial ==================================================================

#define USB_SERIAL_BAUD 115200
#define UART_SERIAL_BAUD 115200 // Serial1, on the TX/RX pins.

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

static CommandBuffer usbCommandBuffer;
static CommandBuffer uartCommandBuffer;

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

static void printLineBoth(const char *msg) {
    Serial.println(msg);
    Serial1.println(msg);
}

static void printError(const char *reason) {
    snprintf(lineBuf, sizeof(lineBuf), "ERR,%s", reason);
    printLineBoth(lineBuf);
}

static void printChannelAck() {
    char name[3];
    channelName(currentBand, currentChannel, name);
    snprintf(
        lineBuf, sizeof(lineBuf), "OK,CHANNEL,%s,%u",
        name, BANDS[currentBand].frequencies[currentChannel]
    );
    printLineBoth(lineBuf);
}

// DATA,<timestamp_ms>,<channel>,<frequency_mhz>,<rssi_raw>,<rssi_percent>
static void emitDataLine() {
    uint16_t raw = readRssiRaw();
    char name[3];
    channelName(currentBand, currentChannel, name);

    snprintf(
        lineBuf, sizeof(lineBuf), "DATA,%lu,%s,%u,%u,%u",
        (unsigned long) millis(), name,
        BANDS[currentBand].frequencies[currentChannel],
        raw, rssiToPercent(raw)
    );
    printLineBoth(lineBuf);
}

// SWEEP,<rank>,<channel>,<frequency_mhz>,<rssi_raw>,<rssi_percent>
static void emitSweepLine(const char *tag, uint8_t rank, const SweepResult &r) {
    char name[3];
    channelName(r.band, r.channel, name);

    snprintf(
        lineBuf, sizeof(lineBuf), "%s,%u,%s,%u,%u,%u",
        tag, rank, name, BANDS[r.band].frequencies[r.channel],
        r.rssiRaw, rssiToPercent(r.rssiRaw)
    );
    printLineBoth(lineBuf);
}

// CHECK,<role>,<channel>,<frequency_mhz>,<rssi_raw>,<rssi_percent>
static void emitCheckLine(const char *role, uint8_t band, uint8_t channel, uint16_t raw) {
    char name[3];
    channelName(band, channel, name);

    snprintf(
        lineBuf, sizeof(lineBuf), "CHECK,%s,%s,%u,%u,%u",
        role, name, BANDS[band].frequencies[channel], raw, rssiToPercent(raw)
    );
    printLineBoth(lineBuf);
}


// === Commands ==================================================================

// Tunes across every channel, taking an RSSI reading at each, and returns the
// top 5 (by raw RSSI) sorted from strongest to weakest. If worstOut is
// given, also fills it in with the single weakest channel found. Restores
// whatever channel was tuned before the sweep started.
static uint8_t performSweep(SweepResult top[5], SweepResult *worstOut = NULL) {
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
        printError("BAD_CHANNEL");
        return;
    }

    int8_t bandIdx = findBandIndex(arg[0]);
    int channelNum = atoi(arg + 1);

    if (bandIdx < 0 || channelNum < 1 || channelNum > CHANNELS_PER_BAND) {
        printError("BAD_CHANNEL");
        return;
    }

    tuneTo(bandIdx, channelNum - 1);
    printChannelAck();
}

static void handleSweepCommand() {
    bool wasStreaming = streaming;
    streaming = false;
    printLineBoth("SWEEP,START");

    SweepResult top[5];
    uint8_t count = performSweep(top);

    for (uint8_t i = 0; i < count; i++) {
        emitSweepLine("SWEEP", i + 1, top[i]);
    }
    printLineBoth("SWEEP,DONE");

    streaming = wasStreaming;
}

static void handleScanBestCommand() {
    bool wasStreaming = streaming;
    streaming = false;
    printLineBoth("SCAN,START");

    SweepResult top[5];
    uint8_t count = performSweep(top);

    if (count == 0) {
        printError("NO_SIGNAL");
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
    printLineBoth("SCAN,START");

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
    printLineBoth("CHECK,START");

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
        printLineBoth("CHECK,BELOW,NONE,0,0,0");
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
        printLineBoth("CHECK,ABOVE,NONE,0,0,0");
    }

    tuneTo(savedBand, savedChannel);
    delay(CHANNEL_SETTLE_MS);

    bool isPeak = (!below.valid || centerRssi > belowRssi)
        && (!above.valid || centerRssi > aboveRssi)
        && (below.valid || above.valid);
    printLineBoth(isPeak ? "CHECK,RESULT,PEAK" : "CHECK,RESULT,NO_PEAK");

    streaming = wasStreaming;
}

static void handleHelpCommand() {
    printLineBoth("HELP,START");
    printLineBoth("HELP,CHANNEL <letter><num>,Tune to a channel - e.g. CHANNEL A4");
    printLineBoth("HELP,SWEEP,Scan every channel and report the top 5 by RSSI");
    printLineBoth("HELP,SCAN BEST,Scan every channel and tune to the strongest one found");
    printLineBoth("HELP,SCAN WORST,Scan every channel and tune to the weakest one found");
    printLineBoth("HELP,STREAM,Start streaming frequency/RSSI/timestamp for the current channel");
    printLineBoth("HELP,STREAM OFF,Stop streaming");
    printLineBoth("HELP,CHECK,Cross-check RSSI against frequency neighbors to confirm tuning");
    printLineBoth("HELP,HELP,Show this list");
    printLineBoth("HELP,DONE");
}

static void handleStreamCommand(char *arg) {
    if (arg == NULL || strcmp(arg, "ON") == 0) {
        streaming = true;
        nextStreamTick = millis();
        printLineBoth("OK,STREAM,ON");
    } else if (strcmp(arg, "OFF") == 0) {
        streaming = false;
        printLineBoth("OK,STREAM,OFF");
    } else {
        printError("BAD_STREAM_ARG");
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

    if (strcmp(cmd, "CHANNEL") == 0) {
        handleChannelCommand(strtok(NULL, " \t"));
    } else if (strcmp(cmd, "SWEEP") == 0) {
        handleSweepCommand();
    } else if (strcmp(cmd, "SCAN") == 0) {
        char *arg = strtok(NULL, " \t");
        if (arg != NULL && strcmp(arg, "BEST") == 0) {
            handleScanBestCommand();
        } else if (arg != NULL && strcmp(arg, "WORST") == 0) {
            handleScanWorstCommand();
        } else {
            printError("UNKNOWN_COMMAND");
        }
    } else if (strcmp(cmd, "STREAM") == 0) {
        handleStreamCommand(strtok(NULL, " \t"));
    } else if (strcmp(cmd, "CHECK") == 0) {
        handleCheckCommand();
    } else if (strcmp(cmd, "HELP") == 0) {
        handleHelpCommand();
    } else {
        printError("UNKNOWN_COMMAND");
    }
}

static void pollCommands(Stream &port, CommandBuffer &buf) {
    while (port.available()) {
        char c = port.read();

        if (c == '\r') {
            continue;
        }

        if (c == '\n') {
            buf.data[buf.length] = '\0';
            if (buf.length > 0) {
                handleCommand(buf.data);
            }
            buf.length = 0;
        } else if (buf.length < sizeof(buf.data) - 1) {
            buf.data[buf.length++] = c;
        }
    }
}


// === Arduino entry points ======================================================

void setup() {
    pinMode(PIN_SPI_DATA, OUTPUT);
    pinMode(PIN_SPI_SLAVE_SELECT, OUTPUT);
    pinMode(PIN_SPI_CLOCK, OUTPUT);
    pinMode(PIN_RSSI, INPUT);

    digitalWrite(PIN_SPI_SLAVE_SELECT, HIGH);
    digitalWrite(PIN_SPI_CLOCK, LOW);
    digitalWrite(PIN_SPI_DATA, LOW);

    // Force a 10-bit ADC reading (0-1023) to match RSSI_MIN_VAL/RSSI_MAX_VAL,
    // regardless of this core's default analogRead() resolution.
    analogReadResolution(10);

    Serial.begin(USB_SERIAL_BAUD);
    Serial1.begin(UART_SERIAL_BAUD);

    tuneTo(0, 0); // Default to channel A1 on boot.
    delay(CHANNEL_SETTLE_MS);

    printLineBoth("READY,rx5808-nano-r4-serial");
}

void loop() {
    pollCommands(Serial, usbCommandBuffer);
    pollCommands(Serial1, uartCommandBuffer);

    if (streaming && (int32_t) (millis() - nextStreamTick) >= 0) {
        emitDataLine();
        nextStreamTick = millis() + STREAM_INTERVAL_MS;
    }
}
