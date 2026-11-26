#!/usr/bin/env python3
"""
Validate a config.json locally and hot-push it to an EdgeHMI ESP32-S3 device
or the local EdgeHMI PC Studio simulator via POST /api/config (with Bearer token auth).
"""

from __future__ import annotations

import json
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

from validate_config import validate_config


def main() -> int:
    if len(sys.argv) < 3:
        print("Usage: push_config.py <config.json> <http://device-ip:port> [--token <api_token>]")
        return 2

    cfg_path = Path(sys.argv[1])
    base_url = sys.argv[2].rstrip("/")
    token = os.environ.get("EDGEHMI_API_TOKEN", "")
    if len(sys.argv) >= 5 and sys.argv[3] == "--token":
        token = sys.argv[4]

    raw = cfg_path.read_text(encoding="utf-8")
    cfg = json.loads(raw)
    if not token:
        token = str(cfg.get("device", {}).get("api_token", ""))

    errs, warns, stats = validate_config(cfg)
    for w in warns:
        print(f"WARN: {w}")
    if errs:
        print("Validation failed; aborting push:")
        for e in errs:
            print(f"  ERROR: {e}")
        return 1

    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"

    print(
        f"Validated OK (tags={stats['tags']} -> mb_blocks={stats['mb_blocks']}, "
        f"SRAM~{stats['est_sram_kb']}KB). Pushing to {base_url}/api/config ..."
    )
    req = urllib.request.Request(
        f"{base_url}/api/config",
        data=raw.encode("utf-8"),
        headers=headers,
        method="POST",
    )
    try:
        with urllib.request.urlopen(req, timeout=10) as resp:
            body = resp.read().decode("utf-8")
            print(f"HTTP {resp.status}: {body}")
            return 0
    except urllib.error.HTTPError as e:
        print(f"HTTP {e.code}: {e.read().decode('utf-8', errors='replace')}")
        return 1
    except Exception as e:
        print(f"Push failed: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
