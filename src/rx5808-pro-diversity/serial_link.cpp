/*
 * Serial command/streaming interface over USB, enabled by USE_SERIAL_OUT.
 *
 * Commands (one per line, case insensitive):
 *   CHANNEL <letter><num>  - tune to a channel, e.g. CHANNEL A4
 *   STREAM [ON|OFF]        - start/stop streaming DATA lines
 *   HELP                   - list commands
 *
 * Output lines:
 *   READY,rx5808-pro-diversity
 *   OK,CHANNEL,<name>,<frequency_mhz>
 *   ERR,<reason>
 *   DATA,<millis>,<name>,<frequency_mhz>,<rssiA_raw>,<rssiA_pct>
 *       [,<rssiB_raw>,<rssiB_pct>,<active A|B>]     (with USE_DIVERSITY)
 *
 * All fixed strings are kept in flash (F macro) to preserve RAM.
 */

#include "settings.h"

#ifdef USE_SERIAL_OUT

#include <Arduino.h>
#include <ctype.h>
#include <string.h>

#include "serial_link.h"
#include "settings_internal.h"
#include "settings_eeprom.h"
#include "receiver.h"
#include "channels.h"
#include "timer.h"


#define SERIAL_LINK_BAUD 115200
#define SERIAL_STREAM_INTERVAL 50
#define SERIAL_COMMAND_BUFFER_SIZE 20


namespace SerialLink {
    static char commandBuffer[SERIAL_COMMAND_BUFFER_SIZE];
    static uint8_t commandLength = 0;
    static bool streaming = false;
    static Timer streamTimer = Timer(SERIAL_STREAM_INTERVAL);


    static void printError(const __FlashStringHelper *reason) {
        Serial.print(F("ERR,"));
        Serial.println(reason);
    }

    // Band letter offsets must match the channelNames table in channels.cpp.
    static int8_t bandIndexFromLetter(char letter) {
        switch (letter) {
            case 'A': return 0;
            case 'B': return 1;
            case 'E': return 2;
            case 'F': return 3;
            case 'R': return 4;
            #ifdef USE_LBAND
                case 'L': return 5;
            #endif
            default: return -1;
        }
    }

    static void emitData() {
        Serial.print(F("DATA,"));
        Serial.print(millis());
        Serial.print(',');
        Serial.print(Channels::getName(Receiver::activeChannel));
        Serial.print(',');
        Serial.print(Channels::getFrequency(Receiver::activeChannel));
        Serial.print(',');
        Serial.print(Receiver::rssiARaw);
        Serial.print(',');
        Serial.print(Receiver::rssiA);
        #ifdef USE_DIVERSITY
            Serial.print(',');
            Serial.print(Receiver::rssiBRaw);
            Serial.print(',');
            Serial.print(Receiver::rssiB);
            Serial.print(',');
            Serial.print(
                Receiver::activeReceiver == Receiver::ReceiverId::A ? 'A' : 'B'
            );
        #endif
        Serial.println();
    }

    static void handleChannelCommand(const char *arg) {
        if (arg == NULL || strlen(arg) != 2) {
            printError(F("BAD_CHANNEL"));
            return;
        }

        int8_t band = bandIndexFromLetter(arg[0]);
        int8_t num = arg[1] - '0';

        if (band < 0 || num < 1 || num > 8) {
            printError(F("BAD_CHANNEL"));
            return;
        }

        uint8_t index = band * 8 + (num - 1);
        if (index >= CHANNELS_SIZE) {
            printError(F("BAD_CHANNEL"));
            return;
        }

        Receiver::setChannel(index);
        EepromSettings.startChannel = index;
        EepromSettings.markDirty();

        Serial.print(F("OK,CHANNEL,"));
        Serial.print(Channels::getName(index));
        Serial.print(',');
        Serial.println(Channels::getFrequency(index));
    }

    static void handleStreamCommand(const char *arg) {
        if (arg == NULL || strcmp_P(arg, PSTR("ON")) == 0) {
            streaming = true;
            streamTimer.reset();
            Serial.println(F("OK,STREAM,ON"));
        } else if (strcmp_P(arg, PSTR("OFF")) == 0) {
            streaming = false;
            Serial.println(F("OK,STREAM,OFF"));
        } else {
            printError(F("BAD_STREAM_ARG"));
        }
    }

    static void handleHelpCommand() {
        Serial.println(F("HELP,START"));
        Serial.println(F("HELP,CHANNEL <letter><num>,Tune to a channel - e.g. CHANNEL A4"));
        Serial.println(F("HELP,STREAM,Start streaming DATA lines"));
        Serial.println(F("HELP,STREAM OFF,Stop streaming"));
        Serial.println(F("HELP,HELP,Show this list"));
        Serial.println(F("HELP,DONE"));
    }

    static void handleCommand() {
        char *cmd = strtok(commandBuffer, " ");
        if (cmd == NULL) {
            return;
        }

        char *arg = strtok(NULL, " ");

        if (strcmp_P(cmd, PSTR("CHANNEL")) == 0) {
            handleChannelCommand(arg);
        } else if (strcmp_P(cmd, PSTR("STREAM")) == 0) {
            handleStreamCommand(arg);
        } else if (strcmp_P(cmd, PSTR("HELP")) == 0) {
            handleHelpCommand();
        } else {
            printError(F("UNKNOWN_COMMAND"));
        }
    }

    void setup() {
        Serial.begin(SERIAL_LINK_BAUD);
        Serial.println(F("READY,rx5808-pro-diversity"));
    }

    void update() {
        while (Serial.available()) {
            char c = Serial.read();

            if (c == '\r') {
                continue;
            }

            if (c == '\n') {
                commandBuffer[commandLength] = '\0';
                if (commandLength > 0) {
                    handleCommand();
                }
                commandLength = 0;
            } else if (commandLength < sizeof(commandBuffer) - 1) {
                commandBuffer[commandLength++] = toupper((unsigned char) c);
            }
        }

        if (streaming && Receiver::isRssiStable() && streamTimer.hasTicked()) {
            emitData();
            streamTimer.reset();
        }
    }
}

#endif // USE_SERIAL_OUT
