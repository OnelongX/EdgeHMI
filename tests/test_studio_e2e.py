#!/usr/bin/env python3
"""End-to-end integration test for EdgeHMI Studio (P0 + P1):
- Bearer token auth on POST /api/config (401 rejected, 200 accepted)
- Engineer PIN protection on critical buttons (403 rejected, 200 accepted)
- Retentive NVS persistence across config hot-reloads
- RS485 comm-loss Fail-Safe trip & recovery
- Compound AND rule evaluation on solar_inverter.json
"""

from __future__ import annotations

import hashlib
import json
import os
import subprocess
import sys
import tempfile
import time
import urllib.error
import urllib.request
from pathlib import Path

ROOT = Path(__file__).resolve().parent.parent
sys.path.insert(0, str(ROOT / "tools"))
from discover_devices import discover_devices  # noqa: E402
from pack_epk import pack_epk, verify_epk  # noqa: E402

PORT = 18767
UDP_PORT = 56899
BASE = f"http://127.0.0.1:{PORT}"
TOKEN = "edgehmi-secret-2026"


def req_json(
    method: str, path: str, body: dict | str | None = None, token: str | None = None
) -> tuple[int, dict]:
    data = None
    if body is not None:
        data = (body if isinstance(body, str) else json.dumps(body)).encode("utf-8")
    headers = {"Content-Type": "application/json"}
    if token:
        headers["Authorization"] = f"Bearer {token}"
    r = urllib.request.Request(BASE + path, data=data, headers=headers, method=method)
    try:
        with urllib.request.urlopen(r, timeout=5) as resp:
            return resp.status, json.loads(resp.read().decode("utf-8"))
    except urllib.error.HTTPError as e:
        return e.code, json.loads(e.read().decode("utf-8"))


def main() -> int:
    with tempfile.TemporaryDirectory() as tmpdir:
        nvs_file = Path(tmpdir) / "test_nvs.json"
        env = dict(os.environ)
        env["EDGEHMI_NVS_PATH"] = str(nvs_file)
        env["EDGEHMI_DISCOVERY_PORT"] = str(UDP_PORT)
        env["EDGEHMI_DEVICE_ID"] = "edgehmi-e2etest"

        proc = subprocess.Popen(
            [sys.executable, str(ROOT / "studio" / "server.py"), str(PORT)],
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            env=env,
        )
        try:
            for _ in range(25):
                try:
                    code, h = req_json("GET", "/api/health")
                    if code == 200 and h.get("status") == "ok":
                        break
                except Exception:
                    time.sleep(0.15)
            else:
                raise RuntimeError("Studio server failed to start")

            pump_raw = (ROOT / "configs" / "pump_station.json").read_text(encoding="utf-8")
            solar_raw = (ROOT / "configs" / "solar_inverter.json").read_text(encoding="utf-8")

            # 0. Secret redaction in GET /api/state (api_token & engineer_pin must NOT leak)
            code, init_st = req_json("GET", "/api/state")
            assert code == 200 and init_st.get("auth_required") is True
            dev_obj = init_st["config"]["device"]
            assert "api_token" not in dev_obj, f"api_token leaked in /api/state: {dev_obj}"
            assert "engineer_pin" not in dev_obj, f"engineer_pin leaked in /api/state: {dev_obj}"

            # 1. Bearer token auth on POST /api/config
            code, unauth = req_json("POST", "/api/config", pump_raw)
            assert code == 401 and not unauth["ok"], f"Expected 401, got {code} {unauth}"

            code, bad_tok = req_json("POST", "/api/config", pump_raw, token="wrong-token")
            assert code == 401 and not bad_tok["ok"]

            # 2. Engineer PIN protection
            code, r_bad = req_json("POST", "/api/write", {
                "tag": "pump_cmd", "value": 0, "widget_id": "btn_stop", "pin": "0000"
            })
            assert code == 403 and not r_bad["ok"]

            code, r_ok = req_json("POST", "/api/write", {
                "tag": "pump_cmd", "value": 0, "widget_id": "btn_stop", "pin": "1234"
            })
            assert code == 200 and r_ok["ok"]
            req_json("POST", "/api/write", {"tag": "pump_cmd", "value": 1})

            # 3. Retentive NVS persistence across authorized hot-reload
            req_json("POST", "/api/write", {"tag": "auto_mode", "value": 0})
            code, _ = req_json("POST", "/api/config", pump_raw, token=TOKEN)
            assert code == 200
            _, st_ret = req_json("GET", "/api/state")
            assert st_ret["tags"]["auto_mode"]["value"] == 0.0
            req_json("POST", "/api/nvs/clear", {})

            # 4. RS485 comm-loss Fail-Safe trips pump_cmd -> 0 AND continuous enforce prevents manual restart
            req_json("POST", "/api/write", {"tag": "pump_cmd", "value": 1})
            code, cl = req_json("POST", "/api/comm_loss", {"active": True})
            assert code == 200 and cl["comm_loss"] is True
            time.sleep(0.9)
            _, st_cl = req_json("GET", "/api/state")
            assert st_cl["tags"]["pump_cmd"]["value"] == 0.0, f"Expected Fail-Safe stop, got {st_cl['tags']['pump_cmd']}"
            # Attempt manual override while RS485 is still offline -> enforce must immediately re-trip to 0
            req_json("POST", "/api/write", {"tag": "pump_cmd", "value": 1})
            time.sleep(0.6)
            _, st_enforced = req_json("GET", "/api/state")
            assert st_enforced["tags"]["pump_cmd"]["value"] == 0.0, "Enforce rule failed to re-trip pump!"
            req_json("POST", "/api/comm_loss", {"active": False})

            # 5. Hot-reload solar_inverter.json & verify compound AND rule (zero_export && backflow)
            code, ok = req_json("POST", "/api/config", solar_raw, token=TOKEN)
            assert code == 200 and ok["ok"]
            # Turn OFF zero_export -> injecting -180W must NOT clamp power_limit
            req_json("POST", "/api/write", {"tag": "zero_export", "value": 0})
            req_json("POST", "/api/write", {"tag": "power_limit", "value": 100})
            req_json("POST", "/api/write", {"tag": "grid_power_w", "value": -180.0, "force": True})
            time.sleep(0.8)
            _, st_no_clamp = req_json("GET", "/api/state")
            assert st_no_clamp["tags"]["power_limit"]["value"] == 100.0

            # Turn ON zero_export -> injecting -180W MUST clamp power_limit to 60%
            req_json("POST", "/api/write", {"tag": "zero_export", "value": 1})
            req_json("POST", "/api/write", {"tag": "grid_power_w", "value": -180.0, "force": True})
            time.sleep(1.0)
            _, st_clamped = req_json("GET", "/api/state")
            assert st_clamped["tags"]["power_limit"]["value"] == 60.0
            # Verify derived calc_tag bms_power_w = bms_pack_v * bms_pack_curr (~750W)
            assert st_clamped["tags"]["bms_power_w"]["value"] > 600.0, f"calc_tag failed: {st_clamped['tags'].get('bms_power_w')}"

            # 6. Firmware OTA via .epk package (auth + SHA-256 tamper detection + success)
            fw_bin = b"EDGEHMI-ESP32S3-FW-IMAGE-V1.2.5" * 16
            epk_bytes = pack_epk("firmware", "1.2.5", fw_bin)
            epk_info = verify_epk(epk_bytes)
            assert epk_info["version"] == "1.2.5" and epk_info["type"] == "firmware"

            # Unauthorized OTA -> 401
            r_unauth = urllib.request.Request(
                BASE + "/api/ota/firmware", data=epk_bytes,
                headers={"Content-Type": "application/octet-stream"}, method="POST"
            )
            try:
                urllib.request.urlopen(r_unauth, timeout=5)
                raise AssertionError("Expected 401")
            except urllib.error.HTTPError as e:
                assert e.code == 401

            # Bad SHA-256 header -> 422
            r_badsha = urllib.request.Request(
                BASE + "/api/ota/firmware", data=fw_bin,
                headers={
                    "Content-Type": "application/octet-stream",
                    "Authorization": f"Bearer {TOKEN}",
                    "X-EdgeHMI-SHA256": "0" * 64,
                }, method="POST"
            )
            try:
                urllib.request.urlopen(r_badsha, timeout=5)
                raise AssertionError("Expected 422")
            except urllib.error.HTTPError as e:
                assert e.code == 422

            # Valid EPK OTA -> 200 & fw_ver upgraded to 1.2.5
            r_ok_ota = urllib.request.Request(
                BASE + "/api/ota/firmware", data=epk_bytes,
                headers={
                    "Content-Type": "application/octet-stream",
                    "Authorization": f"Bearer {TOKEN}",
                    "X-EdgeHMI-SHA256": hashlib.sha256(fw_bin).hexdigest(),
                }, method="POST"
            )
            with urllib.request.urlopen(r_ok_ota, timeout=5) as resp:
                ota_res = json.loads(resp.read().decode("utf-8"))
                assert resp.status == 200 and ota_res["ok"] and ota_res["fw_ver"] == "1.2.5"

            # 7. Real UDP LAN Discovery + Auth-protected & SSRF-hardened Fleet Config Push
            devs = discover_devices(udp_port=UDP_PORT, timeout_s=0.8)
            assert any(d["id"] == "edgehmi-e2etest" and d["fw_ver"] == "1.2.5" for d in devs), f"Discovery failed: {devs}"

            code, disc_api = req_json("GET", "/api/discover")
            assert code == 200 and any(d["id"] == "edgehmi-e2etest" for d in disc_api["devices"])

            # Unauthenticated fleet push -> 401
            code, fpush_unauth = req_json("POST", "/api/fleet/push_config", {"target_url": BASE})
            assert code == 401 and not fpush_unauth["ok"]

            # Non-LAN external SSRF host -> 400
            code, fpush_ssrf = req_json(
                "POST", "/api/fleet/push_config", {"target_url": "http://example.com:80"}, token=TOKEN
            )
            assert code == 400 and not fpush_ssrf["ok"]

            # Authorized LAN/loopback push -> 200
            code, fpush = req_json("POST", "/api/fleet/push_config", {"target_url": BASE}, token=TOKEN)
            assert code == 200 and fpush["ok"]

            # Verify saving redacted config preserves api_token & engineer_pin
            _, red_st = req_json("GET", "/api/state")
            code, save_red = req_json("POST", "/api/config", json.dumps(red_st["config"]), token=TOKEN)
            assert code == 200 and save_red["ok"]
            # Token must STILL be enforced after saving redacted config
            code, still_401 = req_json("POST", "/api/config", json.dumps(red_st["config"]))
            assert code == 401

            # 8. History CSV export, Alarm Ack & Live Device Mirror Proxy (auth + SSRF + 200)
            time.sleep(1.1)
            with urllib.request.urlopen(BASE + "/api/history.csv", timeout=5) as csv_resp:
                csv_text = csv_resp.read().decode("utf-8")
                assert csv_resp.status == 200 and csv_text.startswith("timestamp_ms,"), f"Bad CSV: {csv_text[:80]}"
                assert len(csv_text.strip().splitlines()) >= 2

            code, ack_unauth = req_json("POST", "/api/alarms/ack", {"id": 0})
            assert code == 401
            code, ack_ok = req_json("POST", "/api/alarms/ack", {"id": 0}, token=TOKEN)
            assert code == 200 and ack_ok["ok"]

            code, mir_unauth = req_json("POST", "/api/fleet/mirror_tags", {"target_url": BASE})
            assert code == 401
            code, mir_ssrf = req_json("POST", "/api/fleet/mirror_tags", {"target_url": "http://example.com"}, token=TOKEN)
            assert code == 400
            code, mir_ok = req_json("POST", "/api/fleet/mirror_tags", {"target_url": BASE}, token=TOKEN)
            assert code == 200 and isinstance(mir_ok.get("tags"), list) and len(mir_ok["tags"]) > 0

            # 9. 4G/5G Multi-Link WAN Failover (Wi-Fi outage -> 5G RedCap -> Wi-Fi recovery)
            code, w_fail = req_json("POST", "/api/wan/failover", {"wifi_online": False})
            assert code == 200 and w_fail["wan"]["active_link"] == "cellular"
            assert w_fail["wan"]["rat"] == "5G RedCap", f"Expected 5G RedCap, got {w_fail['wan']}"
            code, w_rec = req_json("POST", "/api/wan/failover", {"wifi_online": True})
            assert code == 200 and w_rec["wan"]["active_link"] == "wifi"

            # 10. Batch Recipes, Operation Audit Trail & Vector .DBC CAN Signal Importer
            code, rec_unauth = req_json("POST", "/api/recipe/apply", {"recipe_id": "zero_export_strict"})
            assert code == 401 and not rec_unauth["ok"]
            code, rec_ok = req_json(
                "POST", "/api/recipe/apply",
                {"recipe_id": "zero_export_strict", "actor": "e2e_operator"},
                token=TOKEN,
            )
            assert code == 200 and rec_ok["ok"] and rec_ok["applied"] == 3
            _, st_rec = req_json("GET", "/api/state")
            assert st_rec["tags"]["power_limit"]["value"] == 80.0

            code, aud_res = req_json("GET", "/api/audit")
            assert code == 200 and len(aud_res.get("audits", [])) >= 3
            assert any(a["source"] == "recipe" and a["actor"] == "e2e_operator" for a in aud_res["audits"])

            sample_dbc = (
                'BO_ 854 BMS_Pack: 8 BMS\n'
                ' SG_ CellMaxTemp : 32|16@1- (0.1,-40) [-40|125] "C" Vector__XXX\n'
                'BO_ 2147484000 Ext_Inv: 8 INV\n'
                ' SG_ BusVolt : 7|16@0+ (0.1,0) [0|1000] "V" Vector__XXX\n'
            )
            code, dbc_res = req_json("POST", "/api/tools/parse_dbc", {"dbc": sample_dbc})
            assert code == 200 and dbc_res["ok"] and dbc_res["count"] == 2
            s0, s1 = dbc_res["signals"][0], dbc_res["signals"][1]
            assert s0["tag"] == "cellmaxtemp" and s0["signed"] is True and s0["offset"] == -40.0
            assert s1["ext_id"] is True and s1["big_endian"] is True and s1["start_bit"] == 0

            print("ALL FULL STUDIO v2.3 + RECIPES + AUDIT + DBC + 4G/5G + OTA + SECURITY E2E TESTS PASSED.")
            return 0
        finally:
            proc.terminate()
            proc.wait(timeout=5)


if __name__ == "__main__":
    sys.exit(main())
