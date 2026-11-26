#!/usr/bin/env python3
"""
EdgeHMI Studio — Zero-dependency PC Simulator & Live Config Hot-Reload Server.
Supports:
- Identical config.json schema & deterministic rule engine as ESP32-S3 C11 core
- Modbus Block Read planning stats
- Retentive NVS persistence across config reloads & server restarts
- Engineer PIN verification for critical widget writes
"""

from __future__ import annotations

import copy
import hashlib
import hmac
import ipaddress
import json
import math
import os
import random
import socket
import sys
import threading
import time
import urllib.error
import urllib.parse
import urllib.request
from collections import deque
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any

ROOT_DIR = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT_DIR / "tools"))
from discover_devices import discover_devices  # noqa: E402
from pack_epk import verify_epk  # noqa: E402
from validate_config import validate_config  # noqa: E402

CONFIGS_DIR = ROOT_DIR / "configs"
WEB_DIR = Path(__file__).resolve().parent / "web"
NVS_PATH = Path(os.environ.get("EDGEHMI_NVS_PATH", str(Path(__file__).resolve().parent / "retentive_nvs.json")))
DISCOVERY_PORT = int(os.environ.get("EDGEHMI_DISCOVERY_PORT", "56888"))


class IndustrialRuntime:
    def __init__(self, initial_config_path: Path) -> None:
        self.lock = threading.Lock()
        self.config: dict[str, Any] = {}
        self.stats: dict[str, Any] = {}
        self.tags: dict[str, dict[str, Any]] = {}
        self.history: dict[str, deque[float]] = {}
        self.csv_history: deque[dict[str, Any]] = deque(maxlen=500)
        self.last_csv_ts: float = 0.0
        self.rule_state: dict[str, dict[str, Any]] = {}
        self.alarms: deque[dict[str, Any]] = deque(maxlen=40)
        self.next_alarm_id: int = 1
        self.fault_overrides: dict[str, tuple[float, float]] = {}
        self.comm_loss: bool = False
        self.wifi_online: bool = True
        self.cell_online: bool = True
        self.cell_rssi_dbm: int = -71
        self.fw_version: str = "2.2.0"
        self.device_id: str = os.environ.get("EDGEHMI_DEVICE_ID", "edgehmi-sim001")
        self.http_port: int = 8765
        self.start_ts: float = time.time()
        self.active_template: str = initial_config_path.name
        self.nvs_store: dict[str, float] = self._load_nvs()

        raw = initial_config_path.read_text(encoding="utf-8")
        ok, msg = self.load_config_json(raw, template_name=initial_config_path.name)
        if not ok:
            raise RuntimeError(f"Failed to load initial config: {msg}")

    def _load_nvs(self) -> dict[str, float]:
        if NVS_PATH.exists():
            try:
                data = json.loads(NVS_PATH.read_text(encoding="utf-8"))
                if isinstance(data, dict):
                    return {str(k): float(v) for k, v in data.items()}
            except Exception:
                pass
        return {}

    def _save_nvs(self) -> None:
        try:
            NVS_PATH.write_text(json.dumps(self.nvs_store, indent=2), encoding="utf-8")
        except Exception:
            pass

    def _add_alarm(self, rule: str, level: str, message: str) -> None:
        self.alarms.appendleft({
            "id": self.next_alarm_id,
            "ts": time.strftime("%H:%M:%S"),
            "rule": rule,
            "level": level,
            "ack": False,
            "message": message,
        })
        self.next_alarm_id += 1

    def ack_alarms(self, alarm_id: int = 0) -> int:
        with self.lock:
            acked = 0
            for a in self.alarms:
                if (alarm_id == 0 or a.get("id") == alarm_id) and not a.get("ack"):
                    a["ack"] = True
                    acked += 1
            return acked

    def export_history_csv(self) -> str:
        with self.lock:
            cols = [k for k, v in self.tags.items() if "r" in str(v.get("rw", "r"))][:8]
            lines = ["timestamp_ms," + ",".join(cols)]
            for row in self.csv_history:
                vals = [f"{float(row.get(c, 0.0)):.2f}" for c in cols]
                lines.append(f"{row['ts_ms']}," + ",".join(vals))
            return "\n".join(lines) + "\n"

    def clear_nvs(self) -> None:
        with self.lock:
            self.nvs_store.clear()
            if NVS_PATH.exists():
                try:
                    NVS_PATH.unlink()
                except Exception:
                    pass
            for pt in self.config.get("modbus", {}).get("points", []):
                tname = pt["tag"]
                if pt.get("retentive") and tname in self.tags:
                    self.tags[tname]["value"] = float(pt.get("default", 0.0))
            self._add_alarm("nvs", "info", "Retentive NVS cleared — restored factory defaults")

    def load_config_json(self, raw_json: str, template_name: str | None = None) -> tuple[bool, str]:
        try:
            cfg = json.loads(raw_json)
        except Exception as exc:
            return False, f"JSON parse error: {exc}"

        errs, _warns, stats = validate_config(cfg)
        if errs:
            return False, "; ".join(errs[:3])

        with self.lock:
            # Preserve existing credentials when Studio sends back a redacted config snapshot
            if not template_name and self.config:
                prev_dev = self.config.get("device", {})
                new_dev = cfg.setdefault("device", {})
                if "api_token" not in new_dev and prev_dev.get("api_token"):
                    new_dev["api_token"] = prev_dev["api_token"]
                if "engineer_pin" not in new_dev and prev_dev.get("engineer_pin"):
                    new_dev["engineer_pin"] = prev_dev["engineer_pin"]

            self.config = cfg
            self.stats = stats
            if template_name:
                self.active_template = template_name
            self.tags.clear()
            self.history.clear()
            self.csv_history.clear()
            self.last_csv_ts = 0.0
            self.rule_state.clear()
            self.fault_overrides.clear()
            self.comm_loss = False
            self.alarms.clear()

            now_ms = int(time.time() * 1000)
            for pt in cfg["modbus"]["points"]:
                tname = pt["tag"]
                retentive = bool(pt.get("retentive", False))
                val = float(self.nvs_store[tname]) if (retentive and tname in self.nvs_store) else float(pt.get("default", 0.0))
                self.tags[tname] = {
                    "name": tname,
                    "source": "modbus",
                    "value": val,
                    "unit": pt.get("unit", ""),
                    "rw": pt.get("rw", "r"),
                    "retentive": retentive,
                    "quality": 1,
                    "updated_ms": now_ms,
                }
                self.history[tname] = deque([val] * 24, maxlen=24)

            for io in cfg.get("local_io", []):
                tname = io["tag"]
                mode = io.get("mode", "di")
                retentive = bool(io.get("retentive", False))
                val = float(self.nvs_store[tname]) if (retentive and tname in self.nvs_store) else float(io.get("default", 0.0))
                self.tags[tname] = {
                    "name": tname,
                    "source": f"gpio:{io.get('pin')}",
                    "value": val,
                    "unit": io.get("unit", ""),
                    "rw": "rw" if mode in ("do", "pwm") else "r",
                    "retentive": retentive,
                    "quality": 1,
                    "updated_ms": now_ms,
                }
                self.history[tname] = deque([val] * 24, maxlen=24)

            for sig in cfg.get("can_bus", {}).get("signals", []):
                tname = sig["tag"]
                val = float(sig.get("default", 0.0))
                self.tags[tname] = {
                    "name": tname,
                    "source": f"can:0x{int(sig.get('can_id', 0)):X}",
                    "value": val,
                    "unit": sig.get("unit", ""),
                    "rw": "r",
                    "retentive": False,
                    "quality": 1,
                    "updated_ms": now_ms,
                }
                self.history[tname] = deque([val] * 24, maxlen=24)

            for ct in cfg.get("calc_tags", []):
                tname = ct["tag"]
                self.tags[tname] = {
                    "name": tname,
                    "source": f"calc:{ct.get('op', 'add')}",
                    "value": 0.0,
                    "unit": ct.get("unit", ""),
                    "rw": "r",
                    "retentive": False,
                    "quality": 1,
                    "updated_ms": now_ms,
                }
                self.history[tname] = deque([0.0] * 24, maxlen=24)

            for r in cfg.get("rules", []):
                self.rule_state[r["id"]] = {
                    "active": False,
                    "pending": False,
                    "pending_since_ms": 0,
                    "triggers": 0,
                }

        return True, "reloaded"

    def _widget_requires_pin(self, widget_id: str | None) -> bool:
        if not widget_id:
            return False
        for pg in self.config.get("pages", []):
            for w in pg.get("widgets", []):
                if w.get("id") == widget_id:
                    return bool(w.get("pin_required", False))
        return False

    def write_tag(
        self,
        tag: str,
        value: float,
        force: bool = False,
        widget_id: str | None = None,
        pin: str | None = None,
    ) -> tuple[int, bool, str]:
        with self.lock:
            if tag not in self.tags:
                return 400, False, f"unknown tag '{tag}'"
            if not force and self.tags[tag]["rw"] == "r":
                return 400, False, f"tag '{tag}' is read-only"

            if not force and self._widget_requires_pin(widget_id):
                expected_pin = str(self.config.get("device", {}).get("engineer_pin", "1234"))
                if not hmac.compare_digest(str(pin or ""), expected_pin):
                    self._add_alarm("auth", "warn", f"Rejected protected write to {tag}: invalid Engineer PIN")
                    return 403, False, "Invalid Engineer PIN"

            self.tags[tag]["value"] = float(value)
            self.tags[tag]["updated_ms"] = int(time.time() * 1000)

            if not force and self.tags[tag].get("retentive"):
                self.nvs_store[tag] = float(value)
                self._save_nvs()
                self._add_alarm("nvs", "info", f"NVS persisted: {tag} = {value}")

            if force:
                self.fault_overrides[tag] = (float(value), time.time() + 4.0)
            return 200, True, "ok"

    def set_wan_state(self, wifi_online: bool | None = None, cell_online: bool | None = None) -> dict[str, Any]:
        with self.lock:
            if wifi_online is not None:
                self.wifi_online = bool(wifi_online)
            if cell_online is not None:
                self.cell_online = bool(cell_online)
            info = self._wan_status_unlocked()
            self._add_alarm(
                "wan",
                "warn" if info["active_link"] == "cellular" else ("critical" if info["active_link"] == "offline" else "info"),
                f"WAN link switched -> {info['active_link'].upper()} ({info['rat']} {info['rssi_dbm']} dBm, APN={info['apn']})",
            )
            return info

    def _wan_status_unlocked(self) -> dict[str, Any]:
        net = self.config.get("network", {})
        cell = net.get("cellular", {})
        pol = net.get("failover_policy", "wifi_primary")
        cell_en = bool(cell.get("enabled", False))
        gen = cell.get("generation", "4g")
        rat_map = {"4g": "4G Cat.1", "5g": "5G SA", "5g_redcap": "5G RedCap", "auto": "5G/4G Auto"}
        rat = rat_map.get(gen, "4G LTE") if (cell_en and self.cell_online) else "OFFLINE"
        if pol == "cellular_only":
            active = "cellular" if (cell_en and self.cell_online) else "offline"
        elif pol == "wifi_only":
            active = "wifi" if self.wifi_online else "offline"
        elif pol == "cellular_primary":
            active = "cellular" if (cell_en and self.cell_online) else ("wifi" if self.wifi_online else "offline")
        else:
            active = "wifi" if self.wifi_online else ("cellular" if (cell_en and self.cell_online) else "offline")
        return {
            "policy": pol,
            "active_link": active,
            "wifi_online": self.wifi_online,
            "cellular_enabled": cell_en,
            "cellular_online": cell_en and self.cell_online,
            "rat": rat,
            "interface": cell.get("interface", "uart_ppp"),
            "apn": cell.get("apn", "cmnet"),
            "rssi_dbm": self.cell_rssi_dbm if (cell_en and self.cell_online) else 0,
        }

    def set_comm_loss(self, active: bool) -> None:
        with self.lock:
            self.comm_loss = bool(active)
            for tdata in self.tags.values():
                if tdata["rw"] == "r" and tdata.get("source", "modbus") == "modbus":
                    tdata["quality"] = 3 if self.comm_loss else 1
            self._add_alarm(
                "rs485",
                "critical" if self.comm_loss else "info",
                "Simulated RS485 Bus Disconnect (STALE)" if self.comm_loss else "RS485 Bus Communication Restored",
            )

    def check_bearer_auth(self, auth_header: str | None) -> bool:
        with self.lock:
            expected = str(self.config.get("device", {}).get("api_token", ""))
        if not expected:
            return True
        return hmac.compare_digest(auth_header or "", f"Bearer {expected}")

    def apply_firmware_ota(
        self, raw_bytes: bytes, expected_sha256: str | None = None, new_ver: str | None = None
    ) -> tuple[int, bool, str]:
        if not raw_bytes:
            return 400, False, "empty firmware binary"

        payload = raw_bytes
        if raw_bytes[:4] == b"EPK1":
            try:
                info = verify_epk(raw_bytes)
                if info["type"] != "firmware":
                    return 422, False, f"invalid epk type: {info['type']}"
                payload = info["payload"]
                new_ver = new_ver or info["version"]
            except Exception as exc:
                return 422, False, f"epk verify failed: {exc}"

        actual_sha = hashlib.sha256(payload).hexdigest()
        if expected_sha256 and actual_sha.lower() != expected_sha256.lower():
            return 422, False, f"sha256 mismatch (got {actual_sha[:12]}...)"

        with self.lock:
            self.fw_version = new_ver or self.fw_version
            self._add_alarm(
                "ota",
                "info",
                f"Firmware OTA verified ({len(payload)}B, sha256={actual_sha[:10]}..., v{self.fw_version})",
            )
        return 200, True, f"ota_verified_v{self.fw_version}"

    def discovery_announce_dict(self) -> dict[str, Any]:
        with self.lock:
            dev = self.config.get("device", {})
            return {
                "type": "edgehmi_device",
                "id": self.device_id,
                "name": dev.get("name", "EdgeHMI-Studio"),
                "fw_ver": self.fw_version,
                "cfg_ver": self.config.get("version", "1.0.0"),
                "http_port": self.http_port,
                "width": dev.get("width", 800),
                "height": dev.get("height", 480),
                "comm_loss": self.comm_loss,
                "uptime_s": int(time.time() - self.start_ts),
            }

    def _eval_rule_when(self, w: dict[str, Any], active: bool) -> tuple[bool, bool]:
        if w.get("on_comm_loss") is True:
            ctag = w.get("tag")
            if ctag:
                t = self.tags.get(ctag)
                return True, bool(t and t["quality"] in (2, 3))
            return True, any(t["quality"] in (2, 3) for t in self.tags.values())

        if isinstance(w.get("all"), list):
            for c in w["all"]:
                t = self.tags.get(c["tag"])
                if not t or t["quality"] != 1:
                    return False, False
                if not self._eval_op(c["op"], t["value"], float(c["value"]), float(c.get("hysteresis", 0.0)), active):
                    return True, False
            return True, True

        if isinstance(w.get("any"), list):
            good = 0
            for c in w["any"]:
                t = self.tags.get(c["tag"])
                if t and t["quality"] == 1:
                    good += 1
                    if self._eval_op(c["op"], t["value"], float(c["value"]), float(c.get("hysteresis", 0.0)), active):
                        return True, True
            return (good > 0), False

        ctag = w.get("tag")
        t = self.tags.get(ctag)
        if not t or t["quality"] != 1:
            return False, False
        return True, self._eval_op(w["op"], t["value"], float(w["value"]), float(w.get("hysteresis", 0.0)), active)

    def _eval_op(self, op: str, val: float, thr: float, hyst: float, active: bool) -> bool:
        if op == ">":
            return (val > thr - hyst) if active else (val > thr)
        if op == ">=":
            return (val >= thr - hyst) if active else (val >= thr)
        if op == "<":
            return (val < thr + hyst) if active else (val < thr)
        if op == "<=":
            return (val <= thr + hyst) if active else (val <= thr)
        if op == "==":
            return abs(val - thr) <= (hyst if hyst > 0 else 1e-6)
        if op == "!=":
            return abs(val - thr) > (hyst if hyst > 0 else 1e-6)
        return False

    def step_simulation(self) -> None:
        now = time.time()
        now_ms = int(now * 1000)
        with self.lock:
            if "tank_level" in self.tags and "pump_cmd" in self.tags:
                running = self.tags["pump_cmd"]["value"] > 0.5
                lvl = self.tags["tank_level"]["value"]
                lvl = max(5.0, min(98.0, lvl + (0.65 if running else -0.45) + random.uniform(-0.1, 0.1)))
                self.tags["tank_level"]["value"] = round(lvl, 2)
                if "pump_rpm" in self.tags:
                    target_rpm = 1450.0 if running else 0.0
                    cur_rpm = self.tags["pump_rpm"]["value"]
                    self.tags["pump_rpm"]["value"] = round(cur_rpm + (target_rpm - cur_rpm) * 0.35, 1)
                if "outlet_flow" in self.tags:
                    self.tags["outlet_flow"]["value"] = round(
                        (12.4 + random.uniform(-0.4, 0.4)) if running else 0.0, 2
                    )
                if "motor_curr" in self.tags:
                    base_c = 3.8 + random.uniform(-0.15, 0.15) if running else 0.1
                    self.tags["motor_curr"]["value"] = round(base_c, 2)
                if "water_temp" in self.tags:
                    t = self.tags["water_temp"]["value"] + random.uniform(-0.08, 0.08)
                    self.tags["water_temp"]["value"] = round(max(18.0, min(35.0, t)), 2)

            elif "pv_power_w" in self.tags and "power_limit" in self.tags:
                enabled = self.tags.get("inv_enable", {"value": 1.0})["value"] > 0.5
                lim = max(0.0, min(100.0, self.tags["power_limit"]["value"]))
                irradiance = 3600.0 + 350.0 * math.sin(now / 6.0) + random.uniform(-60, 60)
                pv = (irradiance * (lim / 100.0)) if enabled else 0.0
                self.tags["pv_power_w"]["value"] = round(max(0.0, pv), 1)
                if "grid_power_w" in self.tags:
                    load = 3200.0 + 400.0 * math.cos(now / 8.0)
                    self.tags["grid_power_w"]["value"] = round(load - pv, 1)
                if "batt_soc" in self.tags:
                    soc = self.tags["batt_soc"]["value"] + (0.08 if pv > 2000 else -0.05)
                    self.tags["batt_soc"]["value"] = round(max(10.0, min(100.0, soc)), 2)
                if "inv_temp_c" in self.tags:
                    target_t = 34.0 + (pv / 5000.0) * 22.0
                    cur_t = self.tags["inv_temp_c"]["value"]
                    self.tags["inv_temp_c"]["value"] = round(cur_t + (target_t - cur_t) * 0.15, 2)

            expired = [k for k, (_, exp) in self.fault_overrides.items() if now > exp]
            for k in expired:
                del self.fault_overrides[k]
            for k, (fval, _) in self.fault_overrides.items():
                if k in self.tags:
                    self.tags[k]["value"] = fval

            for tname, tdata in self.tags.items():
                if not self.comm_loss or tdata["rw"] != "r" or tdata.get("source", "modbus") != "modbus":
                    tdata["quality"] = 1
                    tdata["updated_ms"] = now_ms
                else:
                    tdata["quality"] = 3
                self.history[tname].append(round(tdata["value"], 2))

            if "bms_pack_v" in self.tags:
                self.tags["bms_pack_v"]["value"] = round(52.8 + 0.15 * math.sin(now / 5.0), 2)
            if "bms_pack_curr" in self.tags:
                self.tags["bms_pack_curr"]["value"] = round(14.2 + random.uniform(-0.3, 0.3), 2)

            # Evaluate derived virtual calc_tags
            for ct in self.config.get("calc_tags", []):
                tn = ct["tag"]
                op = ct.get("op")
                sa = self.tags.get(ct.get("src_a", ""))
                sb = self.tags.get(ct.get("src_b", ""))
                if tn not in self.tags or not sa:
                    continue
                scale = float(ct.get("scale", 1.0)) or 1.0
                off = float(ct.get("offset", 0.0))
                va = float(sa["value"])
                vb = float(sb["value"]) if sb else 0.0
                if op == "add":
                    res = (va + vb) * scale + off
                elif op == "sub":
                    res = (va - vb) * scale + off
                elif op == "mul":
                    res = (va * vb) * scale + off
                elif op == "div":
                    if abs(vb) < 1e-9:
                        continue
                    res = (va / vb) * scale + off
                elif op == "avg":
                    res = ((va + vb) * 0.5) * scale + off
                elif op == "integrate":
                    res = self.tags[tn]["value"] + va * (0.25 / 3600.0) * scale
                else:
                    continue
                self.tags[tn]["value"] = round(res, 2)
                self.history[tn][-1] = self.tags[tn]["value"]

            if now - self.last_csv_ts >= 1.0:
                row: dict[str, Any] = {"ts_ms": now_ms}
                for tn, td in self.tags.items():
                    row[tn] = td["value"]
                self.csv_history.append(row)
                self.last_csv_ts = now

            for r in self.config.get("rules", []):
                if not r.get("enabled", True):
                    continue
                rid = r["id"]
                st = self.rule_state[rid]
                w = r["when"]
                valid, met = self._eval_rule_when(w, st["active"])
                if not valid:
                    st["pending"] = False
                    continue
                deb = int(w.get("debounce_ms", 0))
                if met:
                    if not st["active"]:
                        trigger_now = False
                        if deb == 0:
                            trigger_now = True
                        else:
                            if not st["pending"]:
                                st["pending"] = True
                                st["pending_since_ms"] = now_ms
                            elif (now_ms - st["pending_since_ms"]) >= deb:
                                trigger_now = True
                        if trigger_now:
                            st["active"] = True
                            st["pending"] = False
                            st["triggers"] += 1
                            then = r["then"]
                            if then["action"] == "set_tag":
                                tt = then["target_tag"]
                                if tt in self.tags:
                                    self.tags[tt]["value"] = float(then["value"])
                                    if self.tags[tt].get("retentive"):
                                        self.nvs_store[tt] = float(then["value"])
                                        self._save_nvs()
                                self._add_alarm(rid, "info", f"Interlock: {tt} -> {then['value']} ({r.get('desc', rid)})")
                            elif then["action"] == "alarm":
                                self._add_alarm(rid, then.get("level", "warn"), then.get("message", rid))
                    elif r.get("enforce") or r.get("then", {}).get("enforce"):
                        then = r["then"]
                        if then["action"] == "set_tag":
                            tt = then["target_tag"]
                            target_val = float(then["value"])
                            if tt in self.tags and abs(self.tags[tt]["value"] - target_val) > 1e-6:
                                self.tags[tt]["value"] = target_val
                                st["triggers"] += 1
                                self._add_alarm(rid, "warn", f"Enforced interlock: {tt} -> {target_val} ({r.get('desc', rid)})")
                else:
                    st["pending"] = False
                    st["active"] = False

    def raw_config_copy(self) -> dict[str, Any]:
        with self.lock:
            return copy.deepcopy(self.config)

    def snapshot(self) -> dict[str, Any]:
        with self.lock:
            redacted_cfg = copy.deepcopy(self.config)
            dev = redacted_cfg.get("device", {})
            auth_required = bool(dev.get("api_token"))
            dev.pop("api_token", None)
            dev.pop("engineer_pin", None)
            return {
                "template": self.active_template,
                "templates": sorted(p.name for p in CONFIGS_DIR.glob("*.json")),
                "config": redacted_cfg,
                "auth_required": auth_required,
                "stats": self.stats,
                "comm_loss": self.comm_loss,
                "wan": self._wan_status_unlocked(),
                "fw_ver": self.fw_version,
                "device_id": self.device_id,
                "nvs": dict(self.nvs_store),
                "tags": self.tags,
                "history": {k: list(v) for k, v in self.history.items()},
                "rules_state": self.rule_state,
                "alarms": list(self.alarms),
            }


def validate_lan_target_url(target_url: str) -> tuple[bool, str]:
    parsed = urllib.parse.urlparse(target_url)
    if parsed.scheme not in ("http", "https") or not parsed.hostname:
        return False, "invalid target_url (http/https required)"
    host = parsed.hostname.lower()
    allowed_host = host in ("localhost",) or host.endswith(".local")
    if not allowed_host:
        try:
            ip = ipaddress.ip_address(host)
            allowed_host = ip.is_private or ip.is_loopback or ip.is_link_local
        except ValueError:
            allowed_host = False
    if not allowed_host:
        return False, f"blocked non-LAN target host: {host}"
    return True, "ok"


def make_handler(runtime: IndustrialRuntime):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, fmt: str, *args: Any) -> None:
            pass

        def _send_json(self, code: int, obj: Any) -> None:
            raw = json.dumps(obj, ensure_ascii=False).encode("utf-8")
            self.send_response(code)
            self.send_header("Content-Type", "application/json; charset=utf-8")
            self.send_header("Content-Length", str(len(raw)))
            self.end_headers()
            self.wfile.write(raw)

        def do_GET(self) -> None:
            if self.path == "/" or self.path == "/index.html":
                html = (WEB_DIR / "index.html").read_bytes()
                self.send_response(200)
                self.send_header("Content-Type", "text/html; charset=utf-8")
                self.send_header("Content-Length", str(len(html)))
                self.end_headers()
                self.wfile.write(html)
            elif self.path == "/api/health":
                self._send_json(200, {"status": "ok", "engine": "edgehmi-studio-sim", "fw_ver": runtime.fw_version})
            elif self.path == "/api/discover":
                devs = discover_devices(udp_port=DISCOVERY_PORT, timeout_s=0.6)
                self._send_json(200, {"ok": True, "udp_port": DISCOVERY_PORT, "devices": devs})
            elif self.path == "/api/tags":
                snap = runtime.snapshot()
                self._send_json(200, {"tags": list(snap["tags"].values())})
            elif self.path == "/api/alarms":
                snap = runtime.snapshot()
                self._send_json(200, {"alarms": snap["alarms"]})
            elif self.path == "/api/history.csv":
                csv_bytes = runtime.export_history_csv().encode("utf-8")
                self.send_response(200)
                self.send_header("Content-Type", "text/csv; charset=utf-8")
                self.send_header("Content-Disposition", 'attachment; filename="edgehmi_history.csv"')
                self.send_header("Content-Length", str(len(csv_bytes)))
                self.end_headers()
                self.wfile.write(csv_bytes)
            elif self.path == "/api/state":
                self._send_json(200, runtime.snapshot())
            else:
                self._send_json(404, {"error": "not found"})

        def do_POST(self) -> None:
            length = int(self.headers.get("Content-Length", "0"))
            raw_bytes = self.rfile.read(length) if length > 0 else b""

            if self.path == "/api/ota/firmware":
                if not runtime.check_bearer_auth(self.headers.get("Authorization")):
                    self._send_json(401, {"ok": False, "message": "missing or invalid Bearer token"})
                    return
                code, ok, msg = runtime.apply_firmware_ota(
                    raw_bytes,
                    expected_sha256=self.headers.get("X-EdgeHMI-SHA256"),
                    new_ver=self.headers.get("X-EdgeHMI-Version"),
                )
                self._send_json(code, {"ok": ok, "message": msg, "fw_ver": runtime.fw_version})
                return

            raw = raw_bytes.decode("utf-8", errors="replace")

            if self.path == "/api/config":
                if not runtime.check_bearer_auth(self.headers.get("Authorization")):
                    self._send_json(401, {"ok": False, "message": "missing or invalid Bearer token"})
                    return
                ok, msg = runtime.load_config_json(raw)
                self._send_json(200 if ok else 422, {"ok": ok, "message": msg})
            elif self.path == "/api/fleet/push_config":
                if not runtime.check_bearer_auth(self.headers.get("Authorization")):
                    self._send_json(401, {"ok": False, "message": "missing or invalid Bearer token"})
                    return
                try:
                    data = json.loads(raw)
                    target_url = str(data["target_url"]).rstrip("/")
                    url_ok, url_err = validate_lan_target_url(target_url)
                    if not url_ok:
                        self._send_json(400, {"ok": False, "message": url_err})
                        return

                    full_cfg = runtime.raw_config_copy()
                    cfg_raw = json.dumps(full_cfg, ensure_ascii=False).encode("utf-8")
                    tok = str(data.get("token") or "")
                    if not tok:
                        auth_hdr = self.headers.get("Authorization", "")
                        if auth_hdr.startswith("Bearer "):
                            tok = auth_hdr[7:]
                    hdrs = {"Content-Type": "application/json"}
                    if tok:
                        hdrs["Authorization"] = f"Bearer {tok}"
                    req = urllib.request.Request(f"{target_url}/api/config", data=cfg_raw, headers=hdrs, method="POST")
                    with urllib.request.urlopen(req, timeout=8) as resp:
                        self._send_json(resp.status, json.loads(resp.read().decode("utf-8")))
                except urllib.error.HTTPError as e:
                    self._send_json(e.code, {"ok": False, "message": e.read().decode("utf-8", errors="replace")})
                except Exception as exc:
                    self._send_json(502, {"ok": False, "message": str(exc)})
            elif self.path == "/api/fleet/mirror_tags":
                if not runtime.check_bearer_auth(self.headers.get("Authorization")):
                    self._send_json(401, {"ok": False, "message": "missing or invalid Bearer token"})
                    return
                try:
                    data = json.loads(raw)
                    target_url = str(data["target_url"]).rstrip("/")
                    url_ok, url_err = validate_lan_target_url(target_url)
                    if not url_ok:
                        self._send_json(400, {"ok": False, "message": url_err})
                        return
                    with urllib.request.urlopen(f"{target_url}/api/tags", timeout=3) as resp:
                        self._send_json(200, json.loads(resp.read().decode("utf-8")))
                except Exception as exc:
                    self._send_json(502, {"ok": False, "message": str(exc)})
            elif self.path == "/api/alarms/ack":
                if not runtime.check_bearer_auth(self.headers.get("Authorization")):
                    self._send_json(401, {"ok": False, "message": "missing or invalid Bearer token"})
                    return
                try:
                    data = json.loads(raw) if raw.strip() else {}
                    aid = int(data.get("id", 0))
                    acked = runtime.ack_alarms(aid)
                    self._send_json(200, {"ok": True, "acked": acked})
                except Exception as exc:
                    self._send_json(400, {"ok": False, "message": str(exc)})
            elif self.path == "/api/comm_loss":
                try:
                    data = json.loads(raw)
                    runtime.set_comm_loss(bool(data.get("active", False)))
                    self._send_json(200, {"ok": True, "comm_loss": runtime.comm_loss})
                except Exception as exc:
                    self._send_json(400, {"ok": False, "message": str(exc)})
            elif self.path == "/api/wan/failover":
                try:
                    data = json.loads(raw)
                    info = runtime.set_wan_state(
                        wifi_online=data.get("wifi_online"),
                        cell_online=data.get("cell_online"),
                    )
                    self._send_json(200, {"ok": True, "wan": info})
                except Exception as exc:
                    self._send_json(400, {"ok": False, "message": str(exc)})
            elif self.path == "/api/write":
                try:
                    data = json.loads(raw)
                    code, ok, msg = runtime.write_tag(
                        data["tag"],
                        float(data["value"]),
                        force=bool(data.get("force", False)),
                        widget_id=data.get("widget_id"),
                        pin=data.get("pin"),
                    )
                    self._send_json(code, {"ok": ok, "message": msg})
                except Exception as exc:
                    self._send_json(400, {"ok": False, "message": str(exc)})
            elif self.path == "/api/nvs/clear":
                runtime.clear_nvs()
                self._send_json(200, {"ok": True, "message": "cleared"})
            elif self.path == "/api/load_template":
                try:
                    data = json.loads(raw)
                    name = Path(data["name"]).name
                    target = CONFIGS_DIR / name
                    if not target.exists():
                        self._send_json(404, {"ok": False, "message": "template not found"})
                        return
                    ok, msg = runtime.load_config_json(target.read_text(encoding="utf-8"), template_name=name)
                    self._send_json(200 if ok else 422, {"ok": ok, "message": msg})
                except Exception as exc:
                    self._send_json(400, {"ok": False, "message": str(exc)})
            else:
                self._send_json(404, {"error": "not found"})

    return Handler


def start_udp_discovery_responder(runtime: IndustrialRuntime, udp_port: int) -> None:
    def loop() -> None:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        try:
            sock.bind(("", udp_port))
        except OSError:
            sock.close()
            return
        while True:
            try:
                data, addr = sock.recvfrom(512)
                msg = json.loads(data.decode("utf-8"))
                if msg.get("cmd") == "discover":
                    reply = json.dumps(runtime.discovery_announce_dict(), ensure_ascii=False).encode("utf-8")
                    sock.sendto(reply, addr)
            except Exception:
                pass

    threading.Thread(target=loop, daemon=True).start()


def main() -> None:
    port = int(sys.argv[1]) if len(sys.argv) > 1 else 8765
    runtime = IndustrialRuntime(CONFIGS_DIR / "pump_station.json")
    runtime.http_port = port

    def sim_loop() -> None:
        while True:
            runtime.step_simulation()
            time.sleep(0.25)

    threading.Thread(target=sim_loop, daemon=True).start()
    start_udp_discovery_responder(runtime, DISCOVERY_PORT)

    srv = ThreadingHTTPServer(("127.0.0.1", port), make_handler(runtime))
    print(f"EdgeHMI Studio running at http://127.0.0.1:{port} (UDP Discovery :{DISCOVERY_PORT})")
    try:
        srv.serve_forever()
    except KeyboardInterrupt:
        pass


if __name__ == "__main__":
    main()
