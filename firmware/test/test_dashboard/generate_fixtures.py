"""Run from repo root with server/.venv/bin/python. No network or secrets.

The server fixture exercises its actual encoder; the synthetic fixture covers
every PNG row filter, split IDAT, incompressible data and asymmetric pixels.
Checked-in bytes let native CI run without Python/Pillow/server dependencies.
"""
from pathlib import Path
import random
import struct
import sys
import zlib

ROOT = Path(__file__).resolve().parents[3]
sys.path.insert(0, str(ROOT / "server"))
import dashboard

DEST = Path(__file__).with_name("fixtures")
DEST.mkdir(exist_ok=True)
frame = dashboard.render(73, 23.5, 42.0)
(DEST / "server.png").write_bytes(dashboard.png(frame))
# The display buffer is MSB-first, 1=black; it is not a wire format.
(DEST / "server.bitmap").write_bytes(bytes(v ^ 255 for v in frame.tobytes()))


def chunk(kind, data):
    return struct.pack(">I", len(data)) + kind + data + struct.pack(">I", zlib.crc32(kind + data))


def paeth(a, b, c):
    p = a + b - c
    return min((a, b, c), key=lambda v: abs(p - v))


rng = random.Random(8100480)
rows = [bytes(rng.randrange(256) for _ in range(100)) for _ in range(480)]
scan = bytearray()
for y, row in enumerate(rows):
    f = y % 5
    scan.append(f)
    for x, value in enumerate(row):
        a = row[x - 1] if x else 0
        b = rows[y - 1][x] if y else 0
        c = rows[y - 1][x - 1] if y and x else 0
        predictor = (0, a, b, (a + b) // 2, paeth(a, b, c))[f]
        scan.append((value - predictor) & 255)


def png(data):
    compressed = zlib.compress(data)
    header = struct.pack(">IIBBBBB", 800, 480, 1, 0, 0, 0, 0)
    return (b"\x89PNG\r\n\x1a\n" + chunk(b"IHDR", header)
            + b"".join(chunk(b"IDAT", compressed[i:i + 777])
                       for i in range(0, len(compressed), 777))
            + chunk(b"IEND", b""))


(DEST / "filters.png").write_bytes(png(scan))
(DEST / "filters.bitmap").write_bytes(bytes(v ^ 255 for row in rows for v in row))
scan = bytearray(101 * 480)
(DEST / "short.png").write_bytes(png(scan[:-1]))
(DEST / "long.png").write_bytes(png(scan + b"\0"))
scan[0] = 5
(DEST / "bad-filter.png").write_bytes(png(scan))
