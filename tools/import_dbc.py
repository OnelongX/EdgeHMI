#!/usr/bin/env python3
"""
EdgeHMI CAN Vector .dbc Signal Importer.
Parses standard BO_ / SG_ definitions from automotive / BMS / inverter .dbc files
and converts them directly into EdgeHMI `can_bus.signals` JSON objects.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Any

BO_RE = re.compile(r"^\s*BO_\s+(\d+)\s+([A-Za-z0-9_]+)\s*:\s*(\d+)\s+(\S+)")
SG_RE = re.compile(
    r"^\s*SG_\s+([A-Za-z0-9_]+)\s*(?:[mM]\d*\s*)?:\s*"
    r"(\d+)\|(\d+)@([01])([+-])\s*"
    r"\(\s*([-+0-9.eE]+)\s*,\s*([-+0-9.eE]+)\s*\)\s*"
    r"\[\s*[-+0-9.eE]+\s*\|\s*[-+0-9.eE]+\s*\]\s*"
    r'"([^"]*)"'
)


def sanitize_tag_name(raw_name: str) -> str:
    cleaned = re.sub(r"[^a-zA-Z0-9_]", "_", raw_name).strip("_").lower()
    if not cleaned or cleaned[0].isdigit():
        cleaned = "can_" + cleaned
    return cleaned[:31]


def parse_dbc_signals(dbc_text: str, max_signals: int = 32) -> list[dict[str, Any]]:
    signals: list[dict[str, Any]] = []
    cur_id: int | None = None
    cur_ext: bool = False
    seen_tags: set[str] = set()

    for line in dbc_text.splitlines():
        m_bo = BO_RE.match(line)
        if m_bo:
            raw_id = int(m_bo.group(1))
            # Vector DBC marks 29-bit extended CAN IDs with bit 31 (0x80000000)
            cur_ext = (raw_id & 0x80000000) != 0
            cur_id = raw_id & 0x1FFFFFFF
            continue

        if cur_id is None:
            continue

        m_sg = SG_RE.match(line)
        if not m_sg:
            continue

        sig_name, sb_s, bl_s, endian_s, sign_s, scale_s, off_s, unit = m_sg.groups()
        start_bit = int(sb_s)
        bit_len = int(bl_s)
        if not (1 <= bit_len <= 32 and 0 <= start_bit <= 63):
            continue

        base_tag = sanitize_tag_name(sig_name)
        tag = base_tag
        suffix = 2
        while tag in seen_tags:
            tag = f"{base_tag[:28]}_{suffix}"
            suffix += 1
        seen_tags.add(tag)

        # DBC @1 = Intel little-endian (LSB start_bit), @0 = Motorola big-endian (MSB start_bit)
        # Note: EdgeHMI C11 `can_codec.c` natively uses Vector DBC MSB start_bit numbering for big_endian!
        big_endian = endian_s == "0"

        entry: dict[str, Any] = {
            "tag": tag,
            "can_id": cur_id,
            "ext_id": cur_ext,
            "start_bit": start_bit,
            "bit_len": bit_len,
            "signed": sign_s == "-",
            "big_endian": big_endian,
            "scale": float(scale_s),
            "offset": float(off_s),
            "unit": unit[:16],
            "default": 0.0,
        }
        signals.append(entry)
        if len(signals) >= max_signals:
            break

    return signals


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: import_dbc.py <file.dbc>")
        return 2
    dbc_path = Path(sys.argv[1])
    sigs = parse_dbc_signals(dbc_path.read_text(encoding="utf-8", errors="replace"))
    print(json.dumps({"signals": sigs}, ensure_ascii=False, indent=2))
    return 0


if __name__ == "__main__":
    sys.exit(main())
