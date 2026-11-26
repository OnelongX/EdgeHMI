#!/usr/bin/env python3
"""
EdgeHMI LAN Device Discovery CLI.
Broadcasts UDP discovery probes (port 56888) and lists all responding
ESP32-S3 EdgeHMI devices and local Studio simulators.
"""

from __future__ import annotations

import json
import socket
import sys
import time
from typing import Any

DEFAULT_UDP_PORT = 56888


def discover_devices(udp_port: int = DEFAULT_UDP_PORT, timeout_s: float = 1.2) -> list[dict[str, Any]]:
    probe = json.dumps({"cmd": "discover", "proto": 1}).encode("utf-8")
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_BROADCAST, 1)
    sock.settimeout(0.2)

    found: dict[str, dict[str, Any]] = {}
    try:
        for target in [("255.255.255.255", udp_port), ("127.0.0.1", udp_port)]:
            try:
                sock.sendto(probe, target)
            except OSError:
                pass

        deadline = time.time() + timeout_s
        while time.time() < deadline:
            try:
                data, addr = sock.recvfrom(2048)
            except socket.timeout:
                continue
            except OSError:
                break
            try:
                msg = json.loads(data.decode("utf-8"))
                if msg.get("type") == "edgehmi_device" and msg.get("id"):
                    msg["ip"] = addr[0]
                    msg["url"] = f"http://{addr[0]}:{msg.get('http_port', 80)}"
                    found[msg["id"]] = msg
            except Exception:
                pass
    finally:
        sock.close()

    return sorted(found.values(), key=lambda d: d["id"])


def main() -> int:
    as_json = "--json" in sys.argv
    udp_port = DEFAULT_UDP_PORT
    timeout_s = 1.2
    for i, arg in enumerate(sys.argv):
        if arg == "--port" and i + 1 < len(sys.argv):
            udp_port = int(sys.argv[i + 1])
        elif arg == "--timeout" and i + 1 < len(sys.argv):
            timeout_s = float(sys.argv[i + 1])

    devices = discover_devices(udp_port=udp_port, timeout_s=timeout_s)
    if as_json:
        print(json.dumps({"devices": devices}, indent=2, ensure_ascii=False))
        return 0

    if not devices:
        print(f"No EdgeHMI devices found on UDP port {udp_port}.")
        return 0

    print(f"Found {len(devices)} EdgeHMI device(s):")
    print(f"{'ID':<18} {'NAME':<26} {'URL':<26} {'FW':<8} {'CFG':<8} {'RES':<10} {'STATUS'}")
    print("-" * 106)
    for d in devices:
        res = f"{d.get('width', '?')}x{d.get('height', '?')}"
        st = "COMM_LOSS" if d.get("comm_loss") else "ONLINE"
        print(
            f"{d['id']:<18} {str(d.get('name', ''))[:25]:<26} {d['url']:<26} "
            f"{d.get('fw_ver', '-'):<8} {d.get('cfg_ver', '-'):<8} {res:<10} {st}"
        )
    return 0


if __name__ == "__main__":
    sys.exit(main())
