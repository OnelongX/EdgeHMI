#!/usr/bin/env python3
"""
EdgeHMI Offline/OTA Package (.epk) Packer & Verifier.
Binary format (64-byte header + raw payload) matches C11 ota_package.c byte-for-byte:
  [0..3]   magic        "EPK1"
  [4]      pkg_type     1=config, 2=firmware
  [5..7]   reserved     0x00, 0x00, 0x00
  [8..23]  version      16 bytes null-padded ASCII semver
  [24..27] payload_len  uint32 little-endian
  [28..59] sha256       32-byte SHA-256 digest of payload
  [60..63] header_crc32 uint32 little-endian IEEE CRC32 of bytes [0..59]
"""

from __future__ import annotations

import hashlib
import struct
import sys
import zlib
from pathlib import Path

EPK_MAGIC = b"EPK1"
EPK_HEADER_SIZE = 64
TYPE_MAP = {"config": 1, "firmware": 2}
TYPE_NAMES = {1: "config", 2: "firmware"}


def pack_epk(pkg_type: str, version: str, payload: bytes) -> bytes:
    if pkg_type not in TYPE_MAP:
        raise ValueError(f"Unsupported type '{pkg_type}', use config or firmware")
    ver_bytes = version.encode("ascii")[:15].ljust(16, b"\x00")
    sha = hashlib.sha256(payload).digest()
    hdr_prefix = struct.pack("<4sB3s16sI32s", EPK_MAGIC, TYPE_MAP[pkg_type], b"\x00\x00\x00", ver_bytes, len(payload), sha)
    hcrc = zlib.crc32(hdr_prefix) & 0xFFFFFFFF
    return hdr_prefix + struct.pack("<I", hcrc) + payload


def verify_epk(data: bytes) -> dict:
    if len(data) < EPK_HEADER_SIZE:
        raise ValueError("File too short for EPK1 header")
    magic, ptype, _res, ver_raw, plen, sha_expected, hcrc_got = struct.unpack("<4sB3s16sI32sI", data[:64])
    if magic != EPK_MAGIC:
        raise ValueError(f"Bad magic: {magic!r}")
    hcrc_calc = zlib.crc32(data[:60]) & 0xFFFFFFFF
    if hcrc_calc != hcrc_got:
        raise ValueError(f"Header CRC32 mismatch: expected {hcrc_calc:#x}, got {hcrc_got:#x}")
    if ptype not in TYPE_NAMES:
        raise ValueError(f"Invalid pkg_type: {ptype}")
    payload = data[64:]
    if len(payload) != plen:
        raise ValueError(f"Payload length mismatch: header={plen}, actual={len(payload)}")
    sha_actual = hashlib.sha256(payload).digest()
    if sha_actual != sha_expected:
        raise ValueError("Payload SHA-256 integrity mismatch")
    return {
        "type": TYPE_NAMES[ptype],
        "version": ver_raw.split(b"\x00", 1)[0].decode("ascii", errors="replace"),
        "payload_len": plen,
        "sha256": sha_actual.hex(),
        "payload": payload,
    }


def main() -> int:
    if len(sys.argv) >= 3 and sys.argv[1] == "--verify":
        p = Path(sys.argv[2])
        try:
            info = verify_epk(p.read_bytes())
            print(
                f"[VALID EPK] {p} | type={info['type']} version={info['version']} "
                f"len={info['payload_len']} sha256={info['sha256']}"
            )
            return 0
        except Exception as exc:
            print(f"[INVALID EPK] {p}: {exc}")
            return 1

    if len(sys.argv) != 5:
        print("Usage:")
        print("  pack_epk.py <config|firmware> <version> <input_file> <output.epk>")
        print("  pack_epk.py --verify <file.epk>")
        return 2

    ptype, version, src, dst = sys.argv[1], sys.argv[2], Path(sys.argv[3]), Path(sys.argv[4])
    payload = src.read_bytes()
    epk = pack_epk(ptype, version, payload)
    dst.write_bytes(epk)
    sha = hashlib.sha256(payload).hexdigest()
    print(f"Packed {dst} ({len(epk)} bytes) | type={ptype} v{version} sha256={sha}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
