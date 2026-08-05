#!/usr/bin/env python3
"""Classify + decode SavvyCAN CSV captures of an OEM Carminat radio <-> display session.

Reads the `Time Stamp,ID,Extended,Dir,Bus,LEN,D1..D8` files under docs/captures and:
  * splits traffic into the sync channel (0x3AF/0x3CF) and the functional channels
    (0x151/0x1F1/0x1C1, each acked on id|0x400),
  * reassembles ISO-TP on every functional channel,
  * names each reassembled payload with the command byte table from
    src/carminat/CarminatConstants.h,
  * reports the filler byte each id pads with, which is how the sender is identified.

Usage:  python tools/decode_oem_csv.py <file-or-dir> [...]  [--transactions] [--payload-hex]
"""
import argparse
import csv
import os
import sys
from collections import Counter, defaultdict

REPLY_FLAG = 0x400
FUNC_IDS = (0x151, 0x1F1, 0x1C1)
SYNC_ID, SYNC_REPLY_ID = 0x3AF, 0x3CF

REGISTER_BYTE = 0x70
ACK_DONE = 0x74
ACK_PARTIAL = (0x30, 0x01, 0x00)

CMD_NAMES = {
    0x21: "SCREEN      (showMenu / fullscreen / confirm box)",
    0x29: "HILITE      (highlightItem)",
    0x52: "CTRL        (setPower)",
    0x54: "CLOSE       (hidePopup / hideFullscreenText)",
    0x56: "CLOCK       (setTime, 'V')",
    0x74: "POPUP       (showPopupText)",
    0x76: "INFOROW     (showInfoPopup)",
    0x77: "TEXT        (setText, windowed)",
}

SYNC_NAMES = {
    0xB9: "ALIVE       (radio 500 ms heartbeat)",
    0xBA: "REQUEST     (radio asks panel to announce)",
    0xB0: "HELLO       (radio capability announce)",
    0x69: "PING        (panel ~1 Hz peer-alive)",
    0x61: "AUTH REQ    (panel 61 11 xx -> draw the hello burst)",
    0x70: "REGISTER    (1-byte function registration)",
}


def load(path):
    frames = []
    with open(path, newline="", encoding="utf-8-sig") as fh:
        for row in csv.DictReader(fh):
            if not row.get("ID"):
                continue
            data = []
            for i in range(1, 9):
                cell = (row.get(f"D{i}") or "").strip()
                if cell:
                    data.append(int(cell, 16))
            frames.append((int(row["Time Stamp"]), int(row["ID"], 16), bytes(data)))
    frames.sort(key=lambda f: f[0])
    return frames


def filler_of(payloads):
    """The byte an id pads its short frames with — the sender's fingerprint."""
    tail = Counter()
    for d in payloads:
        if len(d) == 8:
            run = 0
            for b in reversed(d):
                if run and b != d[-1]:
                    break
                run += 1
            if run >= 3:
                tail[d[-1]] += 1
    return tail.most_common(1)[0][0] if tail else None


def reassemble(frames, func_id):
    """ISO-TP reassembly for one functional id. Returns [(ts, kind, payload)]."""
    out, buf, want = [], None, 0
    for ts, cid, d in frames:
        if cid != func_id or not d:
            continue
        pci, low = d[0] & 0xF0, d[0] & 0x0F
        if len(d) >= 2 and pci == 0x10:                    # first frame
            buf, want, start = bytearray(d[2:]), (low << 8) | d[1], ts
        elif pci == 0x20 and buf is not None:              # consecutive frame
            buf += d[1:]
            if len(buf) >= want:
                out.append((start, "msg", bytes(buf[:want])))
                buf = None
        elif d[0] == REGISTER_BYTE and len(d) >= 2 and all(b == 0 for b in d[1:]):
            out.append((ts, "register", d[:1]))
        elif pci == 0x00 and 0 < low <= 7:                  # single frame
            out.append((ts, "msg", bytes(d[1:1 + low])))
        elif buf is None:
            out.append((ts, "single", d))                  # short / non-ISO-TP frame
    if buf is not None:
        out.append((start, "truncated", bytes(buf)))
    return out


def ascii_of(b):
    return "".join(chr(c) if 0x20 <= c < 0x7F else "." for c in b)


def describe(payload):
    if not payload:
        return "empty"
    cmd = payload[0]
    name = CMD_NAMES.get(cmd, f"UNKNOWN cmd 0x{cmd:02X}")
    text = ascii_of(payload)
    runs = [s for s in text.replace(".", " ").split() if len(s) >= 3]
    return f"{name}  len={len(payload)}" + (f"  text={runs}" if runs else "")


def report(path, args):
    frames = load(path)
    if not frames:
        return
    span = (frames[-1][0] - frames[0][0]) / 1e6
    by_id = defaultdict(list)
    for _, cid, d in frames:
        by_id[cid].append(d)

    print(f"\n{'=' * 78}\n{os.path.basename(path)}   {len(frames)} frames, {span:.1f} s\n{'=' * 78}")
    print(f"{'ID':>6} {'count':>6} {'filler':>7}  distinct data[0]")
    for cid in sorted(by_id):
        b0 = Counter(d[0] for d in by_id[cid] if d)
        f = filler_of(by_id[cid])
        print(f"  {cid:03X} {len(by_id[cid]):>6} {('0x%02X' % f) if f is not None else '-':>7}  "
              + " ".join(f"{v:02X}x{n}" for v, n in b0.most_common(8)))

    # sync channel
    sync = [(ts, cid, d) for ts, cid, d in frames if cid in (SYNC_ID, SYNC_REPLY_ID) and d]
    kinds = Counter((cid, d[0]) for _, cid, d in sync)
    if kinds:
        print("\n-- sync 0x3AF/0x3CF --")
        for (cid, b0), n in sorted(kinds.items()):
            print(f"  {cid:03X} {b0:02X}  x{n:<5} {SYNC_NAMES.get(b0, '?')}")
        auth = [(ts, d) for ts, cid, d in sync if cid == SYNC_REPLY_ID and d[0] == 0x61]
        for ts, d in auth[:6]:
            print(f"     {ts/1e6:9.3f}s  3CF {d.hex(' ').upper()}")

    # functional channels
    for fid in FUNC_IDS:
        msgs = reassemble(frames, fid)
        if not msgs:
            continue
        acks = Counter(d[:3] for _, cid, d in frames if cid == (fid | REPLY_FLAG) and d)
        print(f"\n-- 0x{fid:03X} -> ack 0x{fid | REPLY_FLAG:03X} --  "
              + " ".join(f"{a.hex().upper()}x{n}" for a, n in acks.most_common(4)))
        for ts, kind, p in msgs:
            if kind == "register":
                print(f"  {ts/1e6:9.3f}s  REGISTER 0x70")
            elif kind == "single":
                print(f"  {ts/1e6:9.3f}s  {p.hex(' ').upper():<24} {describe(p)}")
            else:
                tag = "MSG" if kind == "msg" else "MSG(truncated)"
                print(f"  {ts/1e6:9.3f}s  {tag} {describe(p)}")
                if args.payload_hex:
                    for i in range(0, len(p), 16):
                        print(f"      {i:04X}  {p[i:i+16].hex(' ').upper():<48} |{ascii_of(p[i:i+16])}|")


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("paths", nargs="+")
    ap.add_argument("--payload-hex", action="store_true", help="hex-dump reassembled payloads")
    args = ap.parse_args()
    files = []
    for p in args.paths:
        if os.path.isdir(p):
            files += [os.path.join(p, f) for f in sorted(os.listdir(p)) if f.lower().endswith(".csv")]
        else:
            files.append(p)
    for f in files:
        report(f, args)


if __name__ == "__main__":
    main()
