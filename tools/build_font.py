#!/usr/bin/env python3
"""
EdgeHMI CJK Subset Font Builder.
Scans one or more EdgeHMI config.json files + built-in industrial HMI glossary,
deduplicates all Unicode codepoints, estimates LVGL 9 flash footprint, and generates
an exact lv_font_conv command (or compiles directly if lv_font_conv is installed).
"""

from __future__ import annotations

import argparse
import json
import shutil
import struct
import subprocess
import sys
from pathlib import Path
from typing import Any

INDUSTRIAL_GLOSSARY = (
    "工业人机交互边缘控制水泵站液位压力流量温度电流电压功率转速频率"
    "自动手动启动停止急停复位确认取消密码权限工程师告警故障过流过温"
    "断线通讯正常离线运行待机并网光储逆变器电池防逆流限功率降额"
    "设定目标上限下限保存恢复出厂默认历史曲线导出扫码配网诊断监控"
    "开关状态数值曲线仪表进度条系统主菜单返回下一页上一页单位"
    "控制屏引导边缘终端监测演示连接热点设备参数运行中待机中"
    "主电机水泵蓄水池瞬时累计夜间高峰节能模式告警列表配网手机巡检"
    "℃，°C%kWhMWAVrpmHzm3/h·—：（）【】！？、。，±≥≤≠←→"
)


def read_ttf_codepoints(font_path: Path) -> set[int] | None:
    """Zero-dependency TTF/OTF cmap (format 4 & 12) reader to avoid lv_font_conv empty-range errors."""
    try:
        data = font_path.read_bytes()
        if len(data) < 12 or data[:4] not in (b"\x00\x01\x00\x00", b"OTTO", b"true"):
            return None
        num_tables = struct.unpack(">H", data[4:6])[0]
        cmap_off = None
        for i in range(num_tables):
            off = 12 + i * 16
            tag, _, tbl_off, _ = struct.unpack(">4sIII", data[off:off + 16])
            if tag == b"cmap":
                cmap_off = tbl_off
                break
        if cmap_off is None:
            return None
        num_sub = struct.unpack(">H", data[cmap_off + 2:cmap_off + 4])[0]
        cps: set[int] = set()
        for i in range(num_sub):
            rec = cmap_off + 4 + i * 8
            plat, enc, sub_rel = struct.unpack(">HHI", data[rec:rec + 8])
            if plat not in (0, 3):
                continue
            sub = cmap_off + sub_rel
            fmt = struct.unpack(">H", data[sub:sub + 2])[0]
            if fmt == 4:
                seg_cnt = struct.unpack(">H", data[sub + 6:sub + 8])[0] // 2
                ends_off = sub + 14
                starts_off = ends_off + seg_cnt * 2 + 2
                for s in range(seg_cnt):
                    e = struct.unpack(">H", data[ends_off + s * 2:ends_off + s * 2 + 2])[0]
                    st = struct.unpack(">H", data[starts_off + s * 2:starts_off + s * 2 + 2])[0]
                    if st <= e and e != 0xFFFF:
                        cps.update(range(st, e + 1))
            elif fmt == 12:
                n_groups = struct.unpack(">I", data[sub + 12:sub + 16])[0]
                for g in range(n_groups):
                    goff = sub + 16 + g * 12
                    st, e, _ = struct.unpack(">III", data[goff:goff + 12])
                    if st <= e and (e - st) < 200000:
                        cps.update(range(st, e + 1))
        return cps if cps else None
    except Exception:
        return None


def collect_strings(obj: Any, out: list[str]) -> None:
    if isinstance(obj, str):
        out.append(obj)
    elif isinstance(obj, dict):
        for v in obj.values():
            collect_strings(v, out)
    elif isinstance(obj, list):
        for item in obj:
            collect_strings(item, out)


def extract_charset(config_paths: list[Path], include_glossary: bool = True) -> tuple[list[int], str]:
    chars: set[str] = set()
    # Always include printable ASCII (0x20..0x7E)
    for cp in range(0x20, 0x7F):
        chars.add(chr(cp))
    if include_glossary:
        chars.update(INDUSTRIAL_GLOSSARY)
    for p in config_paths:
        cfg = json.loads(p.read_text(encoding="utf-8"))
        strings: list[str] = []
        collect_strings(cfg, strings)
        for s in strings:
            for ch in s:
                if ord(ch) >= 0x20:
                    chars.add(ch)
    default_hdr = Path(__file__).resolve().parent.parent / "firmware" / "main" / "default_config.h"
    if default_hdr.exists():
        for ch in default_hdr.read_text(encoding="utf-8"):
            if ord(ch) >= 0x20:
                chars.add(ch)
    cps = sorted(ord(c) for c in chars)
    return cps, "".join(chr(c) for c in cps)


def compress_ranges(codepoints: list[int]) -> str:
    if not codepoints:
        return ""
    ranges: list[str] = []
    start = prev = codepoints[0]
    for cp in codepoints[1:]:
        if cp == prev + 1:
            prev = cp
        else:
            ranges.append(f"0x{start:X}" if start == prev else f"0x{start:X}-0x{prev:X}")
            start = prev = cp
    ranges.append(f"0x{start:X}" if start == prev else f"0x{start:X}-0x{prev:X}")
    return ",".join(ranges)


def main() -> int:
    ap = argparse.ArgumentParser(description="Build CJK subset LVGL 9 font from EdgeHMI config.json")
    ap.add_argument("configs", nargs="+", type=Path, help="Input config.json files")
    ap.add_argument("--font", type=Path, default=Path("NotoSansSC-Regular.ttf"), help="Source TTF/OTF font path")
    ap.add_argument("--size", type=int, default=16, help="Font pixel size (default: 16)")
    ap.add_argument("--bpp", type=int, default=4, choices=[1, 2, 4], help="Anti-alias bits per pixel (default: 4)")
    ap.add_argument("--out", type=Path, default=Path("firmware/main/font_edgehmi_cjk.c"), help="Output LVGL C font file")
    ap.add_argument("--run", action="store_true", help="Execute lv_font_conv directly if installed")
    args = ap.parse_args()

    cps, text = extract_charset(args.configs)
    if args.font.exists():
        supported = read_ttf_codepoints(args.font)
        if supported is not None:
            missing = [c for c in cps if c not in supported]
            if missing:
                cps = [c for c in cps if c in supported]
                print(
                    f"[WARN] Skipped {len(missing)} glyph(s) absent from {args.font.name}: "
                    + " ".join(f"U+{c:04X}({chr(c)})" for c in missing[:10])
                )
    non_ascii = [c for c in cps if c > 0x7E]
    range_str = compress_ranges(cps)
    est_kb = round((len(cps) * (args.size * args.size * args.bpp / 8.0) * 0.65) / 1024.0, 1)

    print(f"[EdgeHMI Font Subsetter] Scanned {len(args.configs)} config(s)")
    print(f"  Total glyphs : {len(cps)} (ASCII: {len(cps) - len(non_ascii)}, CJK/Symbols: {len(non_ascii)})")
    print(f"  Est. Flash   : ~{est_kb} KB (size={args.size}px, bpp={args.bpp})")
    print(f"  Sample CJK   : {''.join(chr(c) for c in non_ascii[:40])}...")

    cmd = [
        "npx", "--yes", "lv_font_conv",
        "--font", str(args.font),
        "--size", str(args.size),
        "--bpp", str(args.bpp),
        "--format", "lvgl",
        "--lv-include", "lvgl.h",
        "--no-compress",
        "-r", range_str,
        "-o", str(args.out),
    ]
    print("\nReady-to-run lv_font_conv command:")
    print("  " + " ".join(cmd[:13]) + f" -r <{len(range_str)} chars> -o {args.out}")
    print("\nThen enable in firmware/main/CMakeLists.txt:")
    print("  target_compile_definitions(${COMPONENT_LIB} PRIVATE EDGEHMI_CUSTOM_FONT=font_edgehmi_cjk)")

    if args.run:
        if not args.font.exists():
            print(f"[ERR] Font file not found: {args.font}", file=sys.stderr)
            return 1
        npx_bin = shutil.which("npx")
        if not npx_bin:
            print("[ERR] npx not found on PATH; install Node.js first", file=sys.stderr)
            return 1
        cmd[0] = npx_bin
        return subprocess.call(cmd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
