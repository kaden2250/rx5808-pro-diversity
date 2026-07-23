#!/usr/bin/env python3
"""
Serial logger for the rx5808-nano-r4-serial firmware
(src/rx5808-nano-r4-serial/rx5808-nano-r4-serial.ino).

Connects to the Nano R4 over serial, and for every line it sends:
  - appends it verbatim (with a host-side timestamp) to a .log file
  - parses DATA/SWEEP/BEST/WORST/CHECK lines into rows in a .csv file

Also forwards anything you type at the terminal to the board as a command
(e.g. "CHANNEL A4", "STREAM", "SWEEP", "SCAN BEST", "SCAN WORST", "CHECK"),
so this doubles as an interactive console.

Usage:
    python3 rx5808_serial_logger.py                    # defaults to COM5 @ 115200
    python3 rx5808_serial_logger.py --port /dev/ttyACM0
    python3 rx5808_serial_logger.py --port COM5 --baud 115200 --stream

See `python3 rx5808_serial_logger.py --help` for all options.
"""

import argparse
import csv
import datetime
import os
import sys
import threading

import serial


CSV_HEADER = [
    "host_time",
    "device_millis",
    "type",
    "rank",
    "channel",
    "frequency_mhz",
    "rssi_raw",
    "rssi_percent",
]


def parse_line(line):
    """
    Parses one line of firmware output into a CSV row dict, or None if the
    line isn't a DATA/SWEEP/BEST/CHECK reading (e.g. READY/OK/ERR/START/DONE/
    RESULT - those still go to the log file, just not the CSV).
    """
    fields = line.split(",")
    tag = fields[0]

    if tag == "DATA" and len(fields) == 6:
        return {
            "device_millis": fields[1],
            "type": "DATA",
            "rank": "",
            "channel": fields[2],
            "frequency_mhz": fields[3],
            "rssi_raw": fields[4],
            "rssi_percent": fields[5],
        }

    if tag in ("SWEEP", "BEST", "WORST") and len(fields) == 6 and fields[1].isdigit():
        return {
            "device_millis": "",
            "type": tag,
            "rank": fields[1],
            "channel": fields[2],
            "frequency_mhz": fields[3],
            "rssi_raw": fields[4],
            "rssi_percent": fields[5],
        }

    if tag == "CHECK" and len(fields) == 6 and fields[1] in ("BELOW", "CENTER", "ABOVE"):
        return {
            "device_millis": "",
            "type": f"CHECK_{fields[1]}",
            "rank": "",
            "channel": fields[2],
            "frequency_mhz": fields[3],
            "rssi_raw": fields[4],
            "rssi_percent": fields[5],
        }

    return None


def reader_loop(ser, csv_writer, csv_file, log_file, echo):
    while True:
        raw = ser.readline()
        if not raw:
            continue

        line = raw.decode("utf-8", errors="replace").strip()
        if not line:
            continue

        host_time = datetime.datetime.now().isoformat(timespec="milliseconds")

        log_file.write(f"{host_time}\t{line}\n")
        log_file.flush()

        row = parse_line(line)
        if row is not None:
            row["host_time"] = host_time
            csv_writer.writerow(row)
            csv_file.flush()

        if echo:
            print(line)


def writer_loop(ser):
    for raw_command in sys.stdin:
        command = raw_command.strip()
        if not command:
            continue
        ser.write((command + "\n").encode("utf-8"))


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--port", default="COM5",
        help="Serial port the board is on (e.g. /dev/ttyACM0, COM5). Default: COM5.",
    )
    parser.add_argument(
        "--baud", type=int, default=115200,
        help="Serial baud rate. Default: 115200.",
    )
    parser.add_argument(
        "--outdir", default="logs",
        help="Directory to write the .csv/.log files into (default: logs).",
    )
    parser.add_argument(
        "--stream", action="store_true",
        help="Automatically send STREAM after connecting.",
    )
    parser.add_argument(
        "--quiet", action="store_true",
        help="Don't echo received lines to the console.",
    )
    args = parser.parse_args()

    os.makedirs(args.outdir, exist_ok=True)
    run_id = datetime.datetime.now().strftime("%Y%m%d_%H%M%S")
    csv_path = os.path.join(args.outdir, f"rx5808_{run_id}.csv")
    log_path = os.path.join(args.outdir, f"rx5808_{run_id}.log")

    ser = serial.Serial(args.port, args.baud, timeout=1)

    with open(csv_path, "w", newline="") as csv_file, \
            open(log_path, "w") as log_file:
        csv_writer = csv.DictWriter(csv_file, fieldnames=CSV_HEADER)
        csv_writer.writeheader()
        csv_file.flush()

        print(f"Connected to {args.port} @ {args.baud} baud")
        print(f"Logging raw lines to {log_path}")
        print(f"Logging DATA/SWEEP/BEST/WORST rows to {csv_path}")
        print("Type a command and press enter to send it to the board "
              "(e.g. CHANNEL A4, STREAM, SWEEP, SCAN BEST, SCAN WORST). "
              "Ctrl+C to stop.")

        if args.stream:
            ser.write(b"STREAM\n")

        reader = threading.Thread(
            target=reader_loop,
            args=(ser, csv_writer, csv_file, log_file, not args.quiet),
            daemon=True,
        )
        reader.start()

        try:
            writer_loop(ser)
        except KeyboardInterrupt:
            pass
        finally:
            ser.close()
            print("\nStopped.")


if __name__ == "__main__":
    main()
