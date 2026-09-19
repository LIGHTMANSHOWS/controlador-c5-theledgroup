#!/usr/bin/env python3
"""Crea carpeta de show en Escritorio con patch, network xLights y JSON base."""

from __future__ import annotations

import argparse
import shutil
import subprocess
import sys
from pathlib import Path


def desktop() -> Path:
    return Path.home() / "Desktop"


def main() -> int:
    parser = argparse.ArgumentParser(description="Crear carpeta de show Lightman en Escritorio")
    parser.add_argument("--name", default="Show_Default")
    parser.add_argument("--root", type=Path, default=desktop() / "Lightman C5 Shows")
    args = parser.parse_args()

    project = Path(__file__).resolve().parents[1]
    show_dir = args.root / args.name
    examples_dir = show_dir / "examples"
    xlights_dir = show_dir / "xlights"
    config_dir = show_dir / "config"
    for d in (examples_dir, xlights_dir, config_dir, show_dir / "suits"):
        d.mkdir(parents=True, exist_ok=True)

    for filename in ("suit_alas_v1.json", "show_network_example.json", "zones_alas_v1.json"):
        shutil.copy2(project / "examples" / filename, examples_dir / filename)

    subprocess.check_call([
        sys.executable,
        str(project / "tools" / "generate_patch_map.py"),
        str(examples_dir / "show_network_example.json"),
        "-o",
        str(show_dir / "patch_map.json"),
    ])
    subprocess.check_call([
        sys.executable,
        str(project / "tools" / "generate_xlights_network.py"),
        "-o",
        str(xlights_dir / "xlights_networks.xml"),
        "--universes",
        "100",
        "--channels",
        "510",
    ])

    shutil.copy2(show_dir / "patch_map.json", config_dir / "patch_map.json")
    print(f"Show creado: {show_dir}")
    print(f"Patch bridge: {show_dir / 'patch_map.json'}")
    print(f"Network xLights: {xlights_dir / 'xlights_networks.xml'}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
