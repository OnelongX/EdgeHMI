#!/usr/bin/env python3
"""
Push a firmware .bin or verified .epk package to an EdgeHMI ESP32-S3 device
or PC Studio simulator via POST /api/ota/firmware with Bearer token & SHA-256 verification.
"""

from __future__ import annotations

import hashlib
import os
import sys
import urllib.error
import urllib.request
from pathlib import Path

from pack_epk import verify_epk


def main() -> int:
    if len(sys.argv) < 3:
        print("Usage: ota_push.py <firmware.bin|update.epk> <http://device-ip:port> [--token <token>] [--version <ver>]")
        return 2

    src = Path(sys.argv[1])
    base_url = sys.argv[2].rstrip("/")
    token = os.environ.get("EDGEHMI_API_TOKEN", "")
    version = ""
    for i, arg in enumerate(sys.argv):
        if arg == "--token" and i + 1 < len(sys.argv):
            token = sys.argv[i + 1]
        elif arg == "--version" and i + 1 < len(sys.argv):
            version = sys.argv[i + 1]

    raw = src.read_bytes()
    if raw[:4] == b"EPK1":
        info = verify_epk(raw)
        if info["type"] != "firmware":
            print(f"ERROR: EPK package type is '{info['type']}', expected 'firmware'")
            return 1
        payload = info["payload"]
        sha_hex = info["sha256"]
        version = version or info["version"]
        print(f"Verified EPK firmware v{version} ({len(payload)} bytes, sha256={sha_hex[:16]}...)")
    else:
        payload = raw
        sha_hex = hashlib.sha256(payload).hexdigest()
        print(f"Prepared raw firmware ({len(payload)} bytes, sha256={sha_hex[:16]}...)")

    headers = {
        "Content-Type": "application/octet-stream",
        "X-EdgeHMI-SHA256": sha_hex,
    }
    if token:
        headers["Authorization"] = f"Bearer {token}"
    if version:
        headers["X-EdgeHMI-Version"] = version

    req = urllib.request.Request(f"{base_url}/api/ota/firmware", data=payload, headers=headers, method="POST")
    try:
        with urllib.request.urlopen(req, timeout=30) as resp:
            print(f"HTTP {resp.status}: {resp.read().decode('utf-8')}")
            return 0
    except urllib.error.HTTPError as e:
        print(f"HTTP {e.code}: {e.read().decode('utf-8', errors='replace')}")
        return 1
    except Exception as e:
        print(f"OTA push failed: {e}")
        return 1


if __name__ == "__main__":
    sys.exit(main())
