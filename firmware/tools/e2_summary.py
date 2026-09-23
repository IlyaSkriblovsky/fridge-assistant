#!/usr/bin/env python3
"""Summarize E2 serial logs: python3 tools/e2_summary.py <log>.

All latency columns in the rig are microseconds. Report milliseconds here.
P95 uses the nearest-rank convention; pairs are adjacent wakes of one shape.
Only status 500 with the expected complete upload counts as a valid sample.
"""
import csv
import math
import re
import statistics
import sys
from collections import defaultdict
from pathlib import Path

FIELDS = ('cycle scheme shape status bytes transport_us open_us headers_us '
          'tail_us release_us longest_write_us heap_before heap_min').split()


def main(path):
    groups = defaultdict(list)
    rows = []
    contents = Path(path).read_text()
    rssi = [int(v) for v in re.findall(r"rssi=(-?\d+)", contents)]
    if rssi:
        print(f"RSSI: {min(rssi)}..{max(rssi)} dBm; median {statistics.median(rssi)} dBm")
    for line in contents.splitlines():
        if 'E2 ROW,' not in line:
            continue
        values = next(csv.reader([line.split('E2 ROW,', 1)[1]]))
        row = {k: v if k in ('scheme', 'shape') else int(v)
               for k, v in zip(FIELDS, values, strict=True)}
        rows.append(row)
        expected = 128044 if row['shape'] == 'stream' else 44
        if row['status'] != 500 or row['bytes'] != expected:
            print(f"INVALID cycle={row['cycle']} status={row['status']} bytes={row['bytes']}")
            continue
        groups[row['shape'], row['scheme']].append(row)
    for key, samples in sorted(groups.items()):
        print(f"\n{' '.join(key)} n={len(samples)}")
        for field in ('transport_us', 'open_us', 'headers_us', 'tail_us',
                      'release_us', 'longest_write_us'):
            values = sorted(r[field] / 1000 for r in samples)
            print(f'{field:18} min={values[0]:.3f} median={statistics.median(values):.3f} '
                  f'p95={values[math.ceil(.95 * len(values)) - 1]:.3f} max={values[-1]:.3f} ms')
        print('requests with a write >= 1 s:', sum(r['longest_write_us'] >= 1000000 for r in samples))
        print('heap_min bytes:', min(r['heap_min'] for r in samples),
              'median drop:', statistics.median(r['heap_before'] - r['heap_min'] for r in samples))
    for shape in ('tiny', 'stream'):
        pairs = defaultdict(dict)
        for scheme in ('http', 'https'):
            for row in groups[shape, scheme]:
                pairs[row['cycle'] // 2][scheme] = row
        for field in ('transport_us', 'headers_us', 'release_us'):
            deltas = [(p['https'][field] - p['http'][field]) / 1000
                      for p in pairs.values() if len(p) == 2]
            if deltas:
                print(f'{shape} paired HTTPS-HTTP {field}: '
                      f'n={len(deltas)} median={statistics.median(deltas):.3f} ms '
                      f'range={min(deltas):.3f}..{max(deltas):.3f}')
    cycles = [row["cycle"] for row in rows]
    if len(set(cycles)) != len(cycles):
        raise ValueError("Repeated cycles: log contains multiple runs; split before comparing")
    print(f'\nRows: {len(rows)}; valid: {sum(map(len, groups.values()))}; '
          f'done marker: {"E2 DONE" in contents}')


if __name__ == '__main__':
    main(sys.argv[1])
