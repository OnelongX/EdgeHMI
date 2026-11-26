#!/usr/bin/env python3
"""
EdgeHMI Configuration Validator & ESP32-S3 Memory Budget Estimator.
Validates schema constraints, tag bindings, Modbus register collisions,
widget screen-boundary clipping, and static SRAM/PSRAM footprint.
"""

from __future__ import annotations

import json
import re
import sys
from pathlib import Path
from typing import Any

MAX_TAGS = 128
MAX_RULES = 64
MAX_PAGES = 8
MAX_WIDGETS_PER_PAGE = 48

VALID_TYPES = {"bool", "u16", "i16", "u32", "f32"}
VALID_BYTE_ORDERS = {"ABCD", "CDAB", "BADC", "DCBA"}
VALID_FCS = {1, 2, 3, 4, 5, 6, 16}
VALID_OPS = {">", ">=", "<", "<=", "==", "!="}
VALID_WIDGETS = {
    "card", "label", "value", "gauge", "bar", "button", "switch", "badge", "chart",
    "slider", "spinbox", "alarm_list", "qrcode", "image", "dropdown", "led",
}
VALID_IO_MODES = {"di", "do", "adc", "pwm"}
VALID_CALC_OPS = {"add", "sub", "mul", "div", "avg", "integrate"}
VALID_NET_POLICIES = {"wifi_only", "cellular_only", "wifi_primary", "cellular_primary"}
VALID_CELL_GENS = {"auto", "4g", "5g", "5g_redcap"}
VALID_CELL_IFACES = {"uart_ppp", "usb_ecm"}


def plan_modbus_blocks(
    points: list[dict[str, Any]], max_gap: int = 4, max_regs: int = 32
) -> list[dict[str, Any]]:
    """Mirror of C11 edgehmi_mb_plan_blocks: coalesce adjacent readable registers."""
    readable = [
        p for p in points
        if isinstance(p, dict)
        and "r" in str(p.get("rw", "r"))
        and p.get("fc") in {1, 2, 3, 4}
        and isinstance(p.get("slave"), int)
        and isinstance(p.get("addr"), int)
    ]
    readable.sort(key=lambda p: (p["slave"], p["fc"], p["addr"]))
    blocks: list[dict[str, Any]] = []
    for p in readable:
        span = 1 if p["fc"] in {1, 2} else (2 if p.get("type") in {"u32", "f32"} else 1)
        merged = False
        if blocks:
            blk = blocks[-1]
            blk_end = blk["start_addr"] + blk["quantity"]
            new_end = max(blk_end, p["addr"] + span)
            if (
                blk["slave"] == p["slave"]
                and blk["fc"] == p["fc"]
                and len(blk["tags"]) < 32
                and p["addr"] <= blk_end + max_gap
                and (new_end - blk["start_addr"]) <= max_regs
            ):
                blk["quantity"] = new_end - blk["start_addr"]
                blk["tags"].append(p["tag"])
                merged = True
        if not merged:
            blocks.append({
                "slave": p["slave"],
                "fc": p["fc"],
                "start_addr": p["addr"],
                "quantity": span,
                "tags": [p["tag"]],
            })
    return blocks


def nvs_safe_key(tag_name: str) -> str:
    """Mirror of C11 edgehmi_nvs_safe_key (FNV-1a 24-bit suffix when > 15 chars)."""
    if len(tag_name) <= 15:
        return tag_name
    h = 2166136261
    for b in tag_name.encode("utf-8"):
        h ^= b
        h = (h * 16777619) & 0xFFFFFFFF
    h24 = (h ^ (h >> 24)) & 0xFFFFFF
    return f"{tag_name[:8]}_{h24:06x}"


def validate_config(cfg: dict[str, Any]) -> tuple[list[str], list[str], dict[str, Any]]:
    errors: list[str] = []
    warnings: list[str] = []

    if not isinstance(cfg, dict):
        return ["Root must be a JSON object"], [], {}

    # 1. Version & Device
    ver = cfg.get("version")
    if not isinstance(ver, str) or not re.match(r"^\d+\.\d+\.\d+$", ver):
        errors.append("Field 'version' must follow semver (e.g. '1.0.0')")

    dev = cfg.get("device", {})
    if not isinstance(dev, dict):
        errors.append("Field 'device' must be an object")
        width, height = 800, 480
    else:
        width = dev.get("width", 0)
        height = dev.get("height", 0)
        if not isinstance(dev.get("name"), str) or not dev.get("name"):
            errors.append("device.name must be a non-empty string")
        if not (isinstance(width, int) and 128 <= width <= 1280):
            errors.append(f"device.width ({width}) must be integer in [128, 1280]")
        if not (isinstance(height, int) and 64 <= height <= 800):
            errors.append(f"device.height ({height}) must be integer in [64, 800]")
        pin = dev.get("engineer_pin")
        if pin is not None and (not isinstance(pin, str) or not re.match(r"^[0-9]{4,8}$", pin)):
            errors.append("device.engineer_pin must be a 4-8 digit string")
        idle_dim = dev.get("idle_dim_s")
        if idle_dim is not None and not (isinstance(idle_dim, int) and 0 <= idle_dim <= 65535):
            errors.append("device.idle_dim_s must be integer in [0, 65535]")
        idle_br = dev.get("idle_brightness_pct")
        if idle_br is not None and not (isinstance(idle_br, int) and 0 <= idle_br <= 100):
            errors.append("device.idle_brightness_pct must be integer in [0, 100]")

    mbtcp = cfg.get("modbus_tcp", {})
    if isinstance(mbtcp, dict):
        for pkey in ("port", "server_port"):
            pval = mbtcp.get(pkey)
            if pval is not None and not (isinstance(pval, int) and 1 <= pval <= 65535):
                errors.append(f"modbus_tcp.{pkey} ({pval}) must be integer in [1, 65535]")

    # 1b. Network & 4G/5G Cellular Validation
    net = cfg.get("network", {})
    if isinstance(net, dict):
        pol = net.get("failover_policy")
        if pol is not None and pol not in VALID_NET_POLICIES:
            errors.append(f"network.failover_policy invalid: '{pol}'")
        cell = net.get("cellular", {})
        if isinstance(cell, dict):
            gen = cell.get("generation")
            if gen is not None and gen not in VALID_CELL_GENS:
                errors.append(f"network.cellular.generation invalid: '{gen}'")
            iface = cell.get("interface")
            if iface is not None and iface not in VALID_CELL_IFACES:
                errors.append(f"network.cellular.interface invalid: '{iface}'")
            for pfield in ("tx_pin", "rx_pin", "rts_pin", "cts_pin", "pwrkey_pin", "reset_pin"):
                pv = cell.get(pfield)
                if pv is not None and not (isinstance(pv, int) and -1 <= pv <= 48):
                    errors.append(f"network.cellular.{pfield} ({pv}) must be in [-1, 48]")

    # 2. Modbus Points
    mb = cfg.get("modbus", {})
    points = mb.get("points", []) if isinstance(mb, dict) else []
    if not isinstance(points, list):
        errors.append("modbus.points must be a list")
        points = []
    if len(points) > MAX_TAGS:
        errors.append(f"modbus.points count ({len(points)}) exceeds MAX_TAGS ({MAX_TAGS})")

    tags: dict[str, dict[str, Any]] = {}
    used_regs: set[tuple[int, int, int]] = set()
    used_nvs_keys: dict[str, str] = {}
    for idx, pt in enumerate(points):
        tag = pt.get("tag", "")
        if not isinstance(tag, str) or not re.match(r"^[a-zA-Z_][a-zA-Z0-9_]{0,31}$", tag):
            errors.append(f"points[{idx}]: invalid tag name '{tag}'")
            continue
        if tag in tags:
            errors.append(f"points[{idx}]: duplicate tag '{tag}'")
        tags[tag] = pt

        slave = pt.get("slave")
        fc = pt.get("fc")
        addr = pt.get("addr")
        ptype = pt.get("type")
        if not (isinstance(slave, int) and 1 <= slave <= 247):
            errors.append(f"points[{idx}] ({tag}): invalid slave id {slave}")
        if fc not in VALID_FCS:
            errors.append(f"points[{idx}] ({tag}): unsupported Modbus FC {fc}")
        if ptype not in VALID_TYPES:
            errors.append(f"points[{idx}] ({tag}): invalid type '{ptype}'")
        bo = pt.get("byte_order")
        if bo is not None and bo not in VALID_BYTE_ORDERS:
            errors.append(f"points[{idx}] ({tag}): invalid byte_order '{bo}'")
        if pt.get("retentive") and pt.get("rw", "r") == "r":
            warnings.append(f"points[{idx}] ({tag}): retentive=true on read-only point has no write effect")
        if pt.get("retentive"):
            nkey = nvs_safe_key(tag)
            if nkey in used_nvs_keys:
                errors.append(
                    f"points[{idx}] ({tag}): NVS key '{nkey}' collides with '{used_nvs_keys[nkey]}'"
                )
            used_nvs_keys[nkey] = tag
        if isinstance(slave, int) and isinstance(fc, int) and isinstance(addr, int):
            span = 2 if ptype in {"u32", "f32"} else 1
            for offset in range(span):
                key = (slave, fc, addr + offset)
                if key in used_regs:
                    warnings.append(f"points[{idx}] ({tag}): overlapping register slave={slave} fc={fc} addr={addr + offset}")
                used_regs.add(key)

    # 2b. Optional Local IO (GPIO/ADC/PWM) & CAN Bus Signals
    local_io = cfg.get("local_io", [])
    if isinstance(local_io, list):
        used_pins: set[int] = set()
        for idx, io in enumerate(local_io):
            tag = io.get("tag", "")
            pin = io.get("pin")
            mode = io.get("mode")
            if not isinstance(tag, str) or not re.match(r"^[a-zA-Z_][a-zA-Z0-9_]{0,31}$", tag):
                errors.append(f"local_io[{idx}]: invalid tag name '{tag}'")
                continue
            if tag in tags:
                errors.append(f"local_io[{idx}]: duplicate tag '{tag}'")
            if not (isinstance(pin, int) and 0 <= pin <= 48):
                errors.append(f"local_io[{idx}] ({tag}): invalid GPIO pin {pin}")
            elif pin in used_pins:
                errors.append(f"local_io[{idx}] ({tag}): duplicate GPIO pin {pin}")
            else:
                used_pins.add(pin)
            if mode not in VALID_IO_MODES:
                errors.append(f"local_io[{idx}] ({tag}): invalid mode '{mode}'")
            if io.get("retentive"):
                nkey = nvs_safe_key(tag)
                if nkey in used_nvs_keys:
                    errors.append(f"local_io[{idx}] ({tag}): NVS key '{nkey}' collides with '{used_nvs_keys[nkey]}'")
                used_nvs_keys[nkey] = tag
            tags[tag] = {"tag": tag, "rw": "rw" if mode in {"do", "pwm"} else "r", "retentive": io.get("retentive", False)}

    can_bus = cfg.get("can_bus", {})
    if isinstance(can_bus, dict) and isinstance(can_bus.get("signals"), list):
        for idx, sig in enumerate(can_bus["signals"]):
            tag = sig.get("tag", "")
            if not isinstance(tag, str) or not re.match(r"^[a-zA-Z_][a-zA-Z0-9_]{0,31}$", tag):
                errors.append(f"can_bus.signals[{idx}]: invalid tag name '{tag}'")
                continue
            if tag in tags:
                errors.append(f"can_bus.signals[{idx}]: duplicate tag '{tag}'")
            bl = sig.get("bit_len", 0)
            sb = sig.get("start_bit", -1)
            if not (isinstance(bl, int) and 1 <= bl <= 32):
                errors.append(f"can_bus.signals[{idx}] ({tag}): bit_len must be 1..32")
            if not (isinstance(sb, int) and 0 <= sb <= 63):
                errors.append(f"can_bus.signals[{idx}] ({tag}): start_bit must be 0..63")
            tags[tag] = {"tag": tag, "rw": "r", "retentive": False}

    calc_tags = cfg.get("calc_tags", [])
    if isinstance(calc_tags, list):
        for idx, ct in enumerate(calc_tags):
            tag = ct.get("tag", "")
            op = ct.get("op")
            sa = ct.get("src_a")
            sb = ct.get("src_b")
            if not isinstance(tag, str) or not re.match(r"^[a-zA-Z_][a-zA-Z0-9_]{0,31}$", tag):
                errors.append(f"calc_tags[{idx}]: invalid tag name '{tag}'")
                continue
            if tag in tags:
                errors.append(f"calc_tags[{idx}]: duplicate tag '{tag}'")
            if op not in VALID_CALC_OPS:
                errors.append(f"calc_tags[{idx}] ({tag}): invalid op '{op}'")
            if sa not in tags:
                errors.append(f"calc_tags[{idx}] ({tag}): unknown src_a '{sa}'")
            if op != "integrate" and sb not in tags:
                errors.append(f"calc_tags[{idx}] ({tag}): unknown src_b '{sb}'")
            tags[tag] = {"tag": tag, "rw": "r", "retentive": False}

    if len(tags) > MAX_TAGS:
        errors.append(f"total tags across modbus+local_io+can ({len(tags)}) exceeds MAX_TAGS ({MAX_TAGS})")

    # 2c. Optional Industrial Recipes & Edge Schedules
    recipe_ids: set[str] = set()
    recipes = cfg.get("recipes", [])
    if isinstance(recipes, list):
        if len(recipes) > 8:
            errors.append(f"recipes count ({len(recipes)}) exceeds 8")
        for idx, rec in enumerate(recipes):
            rid = rec.get("id", "")
            if not rid or rid in recipe_ids:
                errors.append(f"recipes[{idx}]: missing or duplicate id '{rid}'")
            recipe_ids.add(rid)
            items = rec.get("items", [])
            if not isinstance(items, list) or not (1 <= len(items) <= 12):
                errors.append(f"recipes[{idx}] ({rid}): items must have 1..12 entries")
                continue
            for k, it in enumerate(items):
                t = it.get("tag")
                if t not in tags:
                    errors.append(f"recipes[{idx}] ({rid}).items[{k}]: unknown tag '{t}'")
                elif tags[t].get("rw", "r") == "r":
                    errors.append(f"recipes[{idx}] ({rid}).items[{k}]: tag '{t}' is read-only")

    schedules = cfg.get("schedules", [])
    if isinstance(schedules, list):
        if len(schedules) > 16:
            errors.append(f"schedules count ({len(schedules)}) exceeds 16")
        for idx, sc in enumerate(schedules):
            sid = sc.get("id", "")
            if not sid:
                errors.append(f"schedules[{idx}]: missing id")
            tm = sc.get("time")
            iv = sc.get("interval_s")
            if tm is not None:
                if not isinstance(tm, str) or not re.match(r"^([01][0-9]|2[0-3]):[0-5][0-9]$", tm):
                    errors.append(f"schedules[{idx}] ({sid}): invalid time '{tm}' (expected HH:MM)")
            elif not (isinstance(iv, int) and iv > 0):
                errors.append(f"schedules[{idx}] ({sid}): requires time ('HH:MM') or interval_s (>0)")
            if sc.get("recipe_id"):
                if sc["recipe_id"] not in recipe_ids:
                    errors.append(f"schedules[{idx}] ({sid}): unknown recipe_id '{sc['recipe_id']}'")
            else:
                ttag = sc.get("target_tag")
                if ttag not in tags:
                    errors.append(f"schedules[{idx}] ({sid}): unknown target_tag '{ttag}'")
                elif tags[ttag].get("rw", "r") == "r":
                    errors.append(f"schedules[{idx}] ({sid}): target_tag '{ttag}' is read-only")

    # 3. Rules
    rules = cfg.get("rules", [])
    if not isinstance(rules, list):
        errors.append("rules must be a list")
        rules = []
    if len(rules) > MAX_RULES:
        errors.append(f"rules count ({len(rules)}) exceeds MAX_RULES ({MAX_RULES})")

    rule_ids: set[str] = set()
    for idx, r in enumerate(rules):
        rid = r.get("id", "")
        if not rid or rid in rule_ids:
            errors.append(f"rules[{idx}]: missing or duplicate rule id '{rid}'")
        rule_ids.add(rid)

        when = r.get("when", {})
        then = r.get("then", {})
        if when.get("on_comm_loss") is True:
            ctag = when.get("tag")
            if ctag and ctag not in tags:
                errors.append(f"rules[{idx}] ({rid}): on_comm_loss unknown tag '{ctag}'")
        elif isinstance(when.get("all"), list) or isinstance(when.get("any"), list):
            conds = when.get("all") if isinstance(when.get("all"), list) else when.get("any")
            if not (1 <= len(conds) <= 4):
                errors.append(f"rules[{idx}] ({rid}): compound rule must have 1..4 conditions")
            for cidx, c in enumerate(conds):
                if c.get("tag") not in tags:
                    errors.append(f"rules[{idx}] ({rid}).cond[{cidx}]: unknown tag '{c.get('tag')}'")
                if c.get("op") not in VALID_OPS:
                    errors.append(f"rules[{idx}] ({rid}).cond[{cidx}]: invalid operator '{c.get('op')}'")
        else:
            ctag = when.get("tag")
            if ctag not in tags:
                errors.append(f"rules[{idx}] ({rid}): unknown condition tag '{ctag}'")
            if when.get("op") not in VALID_OPS:
                errors.append(f"rules[{idx}] ({rid}): invalid operator '{when.get('op')}'")
        if then.get("action") == "set_tag":
            ttag = then.get("target_tag")
            if ttag not in tags:
                errors.append(f"rules[{idx}] ({rid}): unknown target_tag '{ttag}'")
            elif tags[ttag].get("rw", "r") == "r":
                errors.append(f"rules[{idx}] ({rid}): target_tag '{ttag}' is read-only (rw='r')")

    # 4. Pages & Widgets
    pages = cfg.get("pages", [])
    if not isinstance(pages, list) or len(pages) == 0:
        errors.append("pages must be a non-empty list")
        pages = []
    if len(pages) > MAX_PAGES:
        errors.append(f"pages count ({len(pages)}) exceeds MAX_PAGES ({MAX_PAGES})")

    page_ids = {p.get("id") for p in pages if isinstance(p, dict) and p.get("id")}
    total_widgets = 0
    for pidx, pg in enumerate(pages):
        widgets = pg.get("widgets", [])
        if len(widgets) > MAX_WIDGETS_PER_PAGE:
            errors.append(f"pages[{pidx}]: widget count ({len(widgets)}) exceeds {MAX_WIDGETS_PER_PAGE}")
        seen_wids: set[str] = set()
        for widx, w in enumerate(widgets):
            total_widgets += 1
            wid = w.get("id", "")
            if not wid or wid in seen_wids:
                errors.append(f"pages[{pidx}].widgets[{widx}]: missing or duplicate id '{wid}'")
            seen_wids.add(wid)

            wtype = w.get("type")
            if wtype not in VALID_WIDGETS:
                errors.append(f"widget '{wid}': invalid type '{wtype}'")

            x, y, ww, wh = w.get("x", 0), w.get("y", 0), w.get("w", 0), w.get("h", 0)
            if isinstance(width, int) and isinstance(height, int):
                if x < 0 or y < 0 or (x + ww) > width or (y + wh) > height:
                    errors.append(
                        f"widget '{wid}' ({x},{y} {ww}x{wh}) overflows screen ({width}x{height})"
                    )

            bind = w.get("bind")
            if bind and bind not in tags:
                errors.append(f"widget '{wid}': binds to unknown tag '{bind}'")
            if (w.get("write_value") is not None or wtype in {"switch", "slider", "spinbox", "dropdown"}) and bind in tags:
                if tags[bind].get("rw", "r") == "r":
                    errors.append(f"widget '{wid}' ({wtype}): writes to read-only tag '{bind}'")

            nav = w.get("nav_page")
            if nav and nav not in page_ids:
                errors.append(f"widget '{wid}': nav_page '{nav}' does not exist")

            crules = w.get("color_rules", [])
            if isinstance(crules, list):
                if len(crules) > 4:
                    errors.append(f"widget '{wid}': color_rules exceeds max 4")
                for ci, cr in enumerate(crules):
                    if cr.get("op") not in VALID_OPS:
                        errors.append(f"widget '{wid}'.color_rules[{ci}]: invalid op '{cr.get('op')}'")
            vis = w.get("visible_when")
            if isinstance(vis, dict):
                if vis.get("tag") not in tags:
                    errors.append(f"widget '{wid}'.visible_when: unknown tag '{vis.get('tag')}'")
                if vis.get("op") not in VALID_OPS:
                    errors.append(f"widget '{wid}'.visible_when: invalid op '{vis.get('op')}'")

    # 5. ESP32-S3 Memory Budget Estimate
    sram_static_kb = round((len(tags) * 88 + len(rules) * 260 + total_widgets * 220 + 16384) / 1024.0, 2)
    fb_double_psram_kb = round((width * height * 2 * 2) / 1024.0, 1) if isinstance(width, int) else 0.0
    if fb_double_psram_kb > 256:
        warnings.append(
            f"Double-buffered 16-bit framebuffer requires {fb_double_psram_kb} KB -> ESP32-S3 with PSRAM required"
        )

    mb_blocks = plan_modbus_blocks(points)
    stats = {
        "tags": len(tags),
        "mb_blocks": len(mb_blocks),
        "blocks": mb_blocks,
        "retentive_tags": sum(1 for p in points if isinstance(p, dict) and p.get("retentive")),
        "rules": len(rules),
        "pages": len(pages),
        "widgets": total_widgets,
        "est_sram_kb": sram_static_kb,
        "est_fb_psram_kb": fb_double_psram_kb,
    }
    return errors, warnings, stats


def main() -> int:
    if len(sys.argv) < 2:
        print("Usage: validate_config.py <config.json> [more.json ...]")
        return 2

    all_ok = True
    for arg in sys.argv[1:]:
        p = Path(arg)
        try:
            cfg = json.loads(p.read_text(encoding="utf-8"))
        except Exception as exc:
            print(f"[FAIL] {p}: JSON parse error: {exc}")
            all_ok = False
            continue

        errs, warns, stats = validate_config(cfg)
        if errs:
            all_ok = False
            print(f"[FAIL] {p} ({len(errs)} errors)")
            for e in errs:
                print(f"  ERROR: {e}")
        else:
            ratio = round(stats["tags"] / max(1, stats["mb_blocks"]), 1)
            print(
                f"[PASS] {p} | tags={stats['tags']} -> mb_blocks={stats['mb_blocks']} ({ratio}x) "
                f"retentive={stats['retentive_tags']} rules={stats['rules']} "
                f"pages={stats['pages']} widgets={stats['widgets']} | "
                f"SRAM~{stats['est_sram_kb']}KB, FB(PSRAM)~{stats['est_fb_psram_kb']}KB"
            )
        for w in warns:
            print(f"  WARN:  {w}")

    return 0 if all_ok else 1


if __name__ == "__main__":
    sys.exit(main())
