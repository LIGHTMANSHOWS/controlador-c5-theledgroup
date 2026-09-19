#!/usr/bin/env python3
"""Genera network fijo para xLights: Art-Net a localhost.

xLights siempre emite a 127.0.0.1. El Lightman C5 Manager/Bridge deriva luego
los universos/pixeles a los controladores C5 reales.
"""

from __future__ import annotations

import argparse
import xml.etree.ElementTree as ET
from pathlib import Path


def indent(elem: ET.Element, level: int = 0) -> None:
    i = "\n" + level * "  "
    if len(elem):
        if not elem.text or not elem.text.strip():
            elem.text = i + "  "
        for child in elem:
            indent(child, level + 1)
        if not child.tail or not child.tail.strip():
            child.tail = i
    if level and (not elem.tail or not elem.tail.strip()):
        elem.tail = i


def build_network(universes: int, channels: int, start_universe: int) -> ET.ElementTree:
    root = ET.Element("Networks")
    network = ET.SubElement(root, "network")
    network.set("NetworkType", "ArtNet")
    network.set("ComPort", "127.0.0.1")
    network.set("BaudRate", "0")
    network.set("MaxChannels", str(universes * channels))
    network.set("Name", "Lightman Bridge Localhost")
    network.set("Description", "xLights envia todo a localhost; Lightman C5 Manager deriva a C5")
    network.set("Universes", str(universes))
    network.set("StartUniverse", str(start_universe))
    network.set("ChannelsPerUniverse", str(channels))
    network.set("OneOutput", "1")
    indent(root)
    return ET.ElementTree(root)


def main() -> int:
    parser = argparse.ArgumentParser(description="Generar xlights_networks.xml fijo para Lightman Bridge")
    parser.add_argument("-o", "--output", type=Path, default=Path("examples/xlights_networks.xml"))
    parser.add_argument("--universes", type=int, default=100)
    parser.add_argument("--channels", type=int, default=510)
    parser.add_argument("--start-universe", type=int, default=0)
    args = parser.parse_args()

    args.output.parent.mkdir(parents=True, exist_ok=True)
    tree = build_network(args.universes, args.channels, args.start_universe)
    tree.write(args.output, encoding="utf-8", xml_declaration=True)
    print(f"xLights network generado: {args.output}")
    print(f"Destino: 127.0.0.1 | Universos: {args.start_universe}..{args.start_universe + args.universes - 1} | canales/universo: {args.channels}")
    print(f"Capacidad: {(args.universes * args.channels) // 3} pixeles RGB")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

