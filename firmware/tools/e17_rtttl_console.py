#!/usr/bin/env python3
# /// script
# requires-python = ">=3.10"
# dependencies = ["pyserial>=3.5,<4"]
# ///
"""E17: interactive RTTTL audition; uv run tools/e17_rtttl_console.py [port]."""
import argparse
import threading
from datetime import datetime
from pathlib import Path

import serial
from serial.tools import list_ports


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("port", nargs="?")
    parser.add_argument("--log", type=Path, help="Save timestamped commands and device replies")
    args = parser.parse_args()
    ports = [p.device for p in list_ports.comports() if p.vid == 0x1A86 and p.pid == 0x55D3]
    port = args.port
    if not port:
        if len(ports) != 1:
            parser.error(f"Specify a port; detected CH343P devices: {ports}")
        port = ports[0]
    done = threading.Event()
    log_lock = threading.Lock()
    log = args.log.open("a", encoding="utf-8", buffering=1) if args.log else None

    def record(direction, text):
        if log:
            with log_lock:
                log.write(f"{datetime.now().astimezone().isoformat()} {direction} {text}\n")

    try:
        # Set control lines before opening; opening may still reset this board.
        with serial.Serial(port=None, baudrate=115200, timeout=0.2, write_timeout=2) as device:
            device.dtr = False
            device.rts = False
            device.port = port
            device.open()

            def receive():
                try:
                    while not done.is_set():
                        raw = device.readline()
                        if raw:
                            text = raw.decode("utf-8", "replace").rstrip("\r\n")
                            print(f"\n{text}", flush=True)
                            record("RX", text)
                except serial.SerialException as exc:
                    print(f"\nUSB connection lost: {exc}", flush=True)
                    done.set()

            reader = threading.Thread(target=receive, daemon=True)
            reader.start()
            print(f"Connected to {port}. Paste RTTTL then Enter.\n"
                  ":repeat :stop :up :down :reset :help; :quit exits.\n"
                  "Wait for E17 READY (or send :help if already running).")
            try:
                while not done.is_set():
                    line = input("RTTTL> ").strip()
                    if line == ":quit":
                        break
                    if not line:
                        continue
                    if not line.isascii() or len(line) > 2048:
                        print("Use an ASCII line of at most 2048 characters.")
                        continue
                    record("TX", line)
                    device.write((line + "\n").encode("ascii"))
            except (EOFError, KeyboardInterrupt):
                pass
            finally:
                if not done.is_set():
                    device.write(b":stop\n")
                    device.flush()
                done.set()
                reader.join(timeout=1)
    except (serial.SerialException, OSError) as exc:
        parser.exit(1, f"Serial error: {exc}\n")
    finally:
        if log:
            log.close()


if __name__ == "__main__":
    main()
