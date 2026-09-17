#!/usr/bin/env python3
"""Read the rig's Serial1 and stamp every line with the host's clock.

`pio device monitor` needs a terminal on stdin, which a background task does not
have. This is the same job without the terminal -- and it does one thing the
monitor does not: the timestamp is the host's, so a line of the log and a packet
in the capture can be put on the same axis afterwards.

Opening the port resets the board (macOS asserts DTR on open, and the board's
auto-reset circuit takes that as a reset), so the session always starts at
cycle 1 with RTC memory cleared. That is E1's finding and it is convenient here
rather than a nuisance.
"""

import sys
import time

import serial

port = sys.argv[1]
out = sys.argv[2]
baud = int(sys.argv[3]) if len(sys.argv) > 3 else 115200

with serial.Serial(port, baud, timeout=1) as ser, open(out, "w", buffering=1) as fh:
    ser.dtr = False
    ser.rts = False
    while True:
        raw = ser.readline()
        if not raw:
            continue
        line = raw.decode("utf-8", "replace").rstrip("\r\n")
        fh.write(f"{time.time():.3f}  {line}\n")
