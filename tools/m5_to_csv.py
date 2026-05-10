#!/usr/bin/env python3
"""
m5_to_csv.py — Convert Trimble DiNi M5 format to SDL30 Collector CSV format

Each BFFB setup (BS1, FS1, FS2, BS2) produces four CSV rows.
Voided measurements (##### in code field) and Station/Measurement
repeated resets are handled automatically.

Usage: python3 m5_to_csv.py input.dat [output.csv]
"""
import sys
import re
import os


def parse_block(s):
    """Parse a 22-char M5 data block. Returns (type, value) or (None, None)."""
    m = re.match(r'\s*(\w+)\s+([-+]?\d+\.?\d+)\s+m', s)
    return (m.group(1), float(m.group(2))) if m else (None, None)


def convert(infile, outfile):
    with open(infile, 'r', encoding='ascii', errors='replace') as f:
        raw = f.readlines()

    rows = []
    point_no = 1
    setup_no = 0
    prev_rl = None
    pending = []

    # BFFB per-setup accumulators
    bs1 = bs2 = fs1 = fs2 = None

    # States: IDLE → FS1 → FS2 → BS2 → WAIT_Z → IDLE
    state = 'IDLE'

    def reset():
        nonlocal state, pending, bs1, bs2, fs1, fs2
        state = 'IDLE'
        pending.clear()
        bs1 = bs2 = fs1 = fs2 = None

    for line in raw:
        parts = line.rstrip('\r\n').split('|')
        if len(parts) < 6:
            continue

        type_info = parts[2]
        rec_type  = type_info[:3].strip()

        # ── TO records ────────────────────────────────────────────────────────
        if rec_type == 'TO':
            if 'repeated' in type_info[4:].lower():
                reset()
            continue

        if rec_type != 'KD1':
            continue

        # ── Parse name and voided flag from info field ────────────────────────
        # info27 layout: PNo(8) + Code(5) + rest(14) = 27 chars
        # DiNi marks rejected readings with ##### in the 5-char Code field
        info27   = type_info[4:31]
        name     = info27[:8].strip()
        voided   = '#' in info27[8:13]

        if voided:
            continue

        b3t, b3v = parse_block(parts[3])
        b4t, b4v = parse_block(parts[4])
        b5t, b5v = parse_block(parts[5])

        # ── Z record (computed RL, blocks 3 and 4 empty) ─────────────────────
        if b5t == 'Z' and b3t is None and b4t is None:
            if prev_rl is None or state == 'IDLE':
                # Opening BM elevation
                prev_rl = b5v
                reset()
                continue

            if state == 'WAIT_Z' and all(v is not None for v in (bs1, bs2, fs1, fs2)):
                hi     = prev_rl + (bs1 + bs2) / 2.0
                tp_rl  = b5v
                for row in pending:
                    row['hi'] = hi
                    row['rl'] = prev_rl if row['sight'] in ('BS1', 'BS2') else tp_rl
                rows.extend(pending)
                prev_rl = tp_rl

            reset()
            continue

        # Skip height-diff and summary blocks
        if b3t in ('Sh', 'Db', 'Df') or b3t is None:
            continue

        meas = b3v
        dist = b4v if b4t == 'HD' else 0.0

        # ── State machine ─────────────────────────────────────────────────────
        if b3t == 'Rb':
            if state == 'IDLE':
                setup_no += 1
                bs1 = meas
                pending = [{'point': point_no, 'setup': setup_no, 'name': name,
                            'sight': 'BS1', 'staff': meas, 'dist': dist,
                            'hi': 0.0, 'rl': prev_rl or 0.0, 'status': 'OK'}]
                point_no += 1
                state = 'FS1'

            elif state == 'BS2':
                bs2 = meas
                pending.append({'point': point_no, 'setup': setup_no, 'name': name,
                                'sight': 'BS2', 'staff': meas, 'dist': dist,
                                'hi': 0.0, 'rl': prev_rl or 0.0, 'status': 'OK'})
                point_no += 1
                state = 'WAIT_Z'

        elif b3t == 'Rf':
            if state == 'FS1':
                fs1 = meas
                pending.append({'point': point_no, 'setup': setup_no, 'name': name,
                                'sight': 'FS1', 'staff': meas, 'dist': dist,
                                'hi': 0.0, 'rl': 0.0, 'status': 'OK'})
                point_no += 1
                state = 'FS2'

            elif state == 'FS2':
                fs2 = meas
                pending.append({'point': point_no, 'setup': setup_no, 'name': name,
                                'sight': 'FS2', 'staff': meas, 'dist': dist,
                                'hi': 0.0, 'rl': 0.0, 'status': 'OK'})
                point_no += 1
                state = 'BS2'

    # Write CSV
    with open(outfile, 'w', newline='') as f:
        f.write("Point,Setup,Name,Sight,Staff(m),Distance(m),HI(m),RL(m),Status\n")
        for row in rows:
            f.write(
                f"{row['point']},{row['setup']},{row['name']},{row['sight']},"
                f"{row['staff']:+.4f},{row['dist']:.3f},{row['hi']:.4f},"
                f"{row['rl']:.4f},{row['status']}\n"
            )

    print(f"Converted: {len(rows)} records, {setup_no} setups → {outfile}")


if __name__ == '__main__':
    if len(sys.argv) < 2:
        print(f"Usage: {sys.argv[0]} input.dat [output.csv]")
        sys.exit(1)
    inp = sys.argv[1]
    out = sys.argv[2] if len(sys.argv) > 2 else os.path.splitext(inp)[0] + '.csv'
    convert(inp, out)
