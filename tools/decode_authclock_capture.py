#!/usr/bin/env python3
"""Decode the persistent CAN capture from examples/02_canspy or 06_authclock.

Usage:
    python tools/decode_authclock_capture.py can-boot.bin
    python tools/decode_authclock_capture.py can-boot.bin --output can-boot.txt

The ESP32 writes the file in chronological order and freezes it when storage fills, so
the output always begins with the first frame the board observed after CAN was armed.
"""

from __future__ import annotations

import struct
import sys
from pathlib import Path


HEADER = struct.Struct("<8sII")
AUTHCLOCK_RECORD_V1V2 = struct.Struct("<IHBB8s")
AUTHCLOCK_RECORD_V3 = struct.Struct("<IIHBB8s")
CANSPY_RECORD = struct.Struct("<IIBB8s")


def main() -> int:
    if len(sys.argv) not in (2, 4) or (len(sys.argv) == 4 and sys.argv[2] != "--output"):
        print("usage: decode_authclock_capture.py CAN-BOOT.BIN [--output CAN-BOOT.TXT]",
              file=sys.stderr)
        return 2

    path = Path(sys.argv[1])
    output = sys.stdout
    close_output = False
    if len(sys.argv) == 4:
        output = Path(sys.argv[3]).open("w", encoding="ascii", newline="\n")
        close_output = True

    try:
        with path.open("rb") as capture:
            raw = capture.read(HEADER.size)
            if len(raw) != HEADER.size:
                raise ValueError("file is too short to contain a CAN capture header")
            magic, version, bitrate = HEADER.unpack(raw)
            if magic == b"AFFACAN1" and version in (1, 2):
                record = AUTHCLOCK_RECORD_V1V2
                source = f"AFFA authclock v{version}"
                extended_ids = False
                authclock_version = version
            elif magic == b"AFFACAN1" and version == 3:
                record = AUTHCLOCK_RECORD_V3
                source = f"AFFA authclock v{version}"
                extended_ids = False
                authclock_version = version
            elif magic == b"CANSCPY1" and version == 2:
                record = CANSPY_RECORD
                source = "AFFA passive listener"
                extended_ids = True
            else:
                raise ValueError(f"unsupported CAN capture {magic!r}, version {version}")

            print(f"# {source} CAN capture: {bitrate} bit/s", file=output)
            row = 0
            while raw := capture.read(record.size):
                if len(raw) != record.size:
                    raise ValueError(f"truncated record {row}")
                if extended_ids:
                    ms, can_id, flags, dlc, payload = record.unpack(raw)
                    direction = "TX" if flags & 0x01 else "RX"
                    suffix = " EXT" if flags & 0x02 else ""
                    suffix += " RTR" if flags & 0x04 else ""
                    can_id_text = f"{can_id:08X}" if flags & 0x02 else f"{can_id:03X}"
                else:
                    if authclock_version == 3:
                        ms, us, can_id, kind, dlc, payload = record.unpack(raw)
                        if us >= 1000:
                            raise ValueError(f"invalid microsecond offset {us} in record {row}")
                        timestamp = f"{ms:8d}.{us:03d}"
                    else:
                        ms, can_id, kind, dlc, payload = record.unpack(raw)
                        timestamp = f"{ms:8d}"
                    if authclock_version == 1:
                        directions = {0: "RX", 1: "TX"}
                    else:
                        # v2+ distinguishes scheduling from an actual controller outcome.
                        directions = {
                            0: "RX", 1: "TXQ", 2: "RXE", 3: "TX+",
                            4: "TX-", 5: "TX!",
                        }
                    try:
                        direction = directions[kind]
                    except KeyError as exc:
                        raise ValueError(f"unknown event kind {kind} in record {row}") from exc
                    suffix = ""
                    can_id_text = f"{can_id:03X}"
                if dlc > 8:
                    raise ValueError(f"invalid DLC {dlc} in record {row}")
                octets = " ".join(f"{byte:02X}" for byte in payload[:dlc])
                if extended_ids:
                    timestamp = f"{ms:8d}"
                print(f"{timestamp}  {direction} {can_id_text}{suffix}  {octets}", file=output)
                row += 1
            print(f"# {row} frame events", file=output)
    finally:
        if close_output:
            output.close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
