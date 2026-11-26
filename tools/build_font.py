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
    "℃，°C%kWhMWAVrpmHzm³/h·—：（）【】！？、。，±≥≤≠"
)


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
    print("  " + " ".join(cmd[:12]) + f" -r <{len(range_str)} chars> -o {args.out}")
    print("\nThen enable in firmware/main/CMakeLists.txt:")
    print("  target_compile_definitions(${COMPONENT_LIB} PRIVATE EDGEHMI_CUSTOM_FONT=font_edgehmi_cjk)")

    if args.run:
        if not args.font.exists():
            print(f"[ERR] Font file not found: {args.font}", file=sys.stderr)
            return 1
        if not shutil.which("npx"):
            print("[ERR] npx not found on PATH; install Node.js first", file=sys.stderr)
            return 1
        return subprocess.call(cmd)
    return 0


if __name__ == "__main__":
    sys.exit(main())
