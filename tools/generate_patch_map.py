#!/usr/bin/env python3
"""Genera patch_map.json desde show_network.json + suit.json.

No compila nada. Sirve para validar estructura, universos/canales y derivacion
de pixeles a controladores C5.
"""

from __future__ import annotations

import argparse
import json
from pathlib import Path
from typing import Any


CHANNELS_PER_PIXEL = 3


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def artnet_address(global_pixel: int, dmx_data_channels: int) -> tuple[int, int]:
    channel0 = (global_pixel - 1) * CHANNELS_PER_PIXEL
    universe = channel0 // dmx_data_channels
    channel = channel0 % dmx_data_channels + 1
    return universe, channel


def suit_pixel_count(suit: dict[str, Any]) -> int:
    total = 0
    for controller in suit.get("controllers", []):
        for output in controller.get("outputs", []):
            total += int(output.get("pixels", 0))
    return total


def build_patch(show_path: Path) -> dict[str, Any]:
    show = load_json(show_path)
    root = show_path.parent
    dmx_data_channels = int(show["input"].get("dmx_data_channels", 510))

    patch: list[dict[str, Any]] = []
    summaries: list[dict[str, Any]] = []

    for show_suit in show.get("suits", []):
        suit_path = (root / show_suit["path"]).resolve()
        if not suit_path.exists():
            suit_path = (show_path.parent.parent / show_suit["path"]).resolve()
        suit = load_json(suit_path)

        expected_pixels = suit_pixel_count(suit)
        configured_pixels = int(show_suit.get("pixel_count", expected_pixels))
        if expected_pixels != configured_pixels:
            raise ValueError(
                f"{show_suit['id']}: pixel_count={configured_pixels}, "
                f"pero suit suma {expected_pixels}"
            )

        global_pixel = int(show_suit["start_pixel"])
        controller_totals: dict[str, int] = {}
        zone_totals: dict[str, int] = {}

        for controller in suit.get("controllers", []):
            controller_number = int(controller["number"])
            controller_ip = str(controller["ip"])
            controller_totals.setdefault(controller_ip, 0)

            for output in controller.get("outputs", []):
                output_number = int(output["output"])
                pixels = int(output.get("pixels", 0))
                zone = str(output.get("zone", "sin_zona"))
                zone_totals.setdefault(zone, 0)

                for output_pixel in range(1, pixels + 1):
                    universe, channel = artnet_address(global_pixel, dmx_data_channels)
                    if channel > dmx_data_channels:
                        raise AssertionError("canal fuera de rango util")
                    patch.append({
                        "global_pixel": global_pixel,
                        "suit_id": suit["id"],
                        "suit_name": suit["name"],
                        "zone": zone,
                        "controller_ip": controller_ip,
                        "controller_number": controller_number,
                        "output": output_number,
                        "output_pixel": output_pixel,
                        "universe": universe,
                        "channel": channel
                    })
                    controller_totals[controller_ip] += 1
                    zone_totals[zone] += 1
                    global_pixel += 1

        first_pixel = int(show_suit["start_pixel"])
        last_pixel = first_pixel + expected_pixels - 1
        first_u, first_ch = artnet_address(first_pixel, dmx_data_channels)
        last_u, last_ch = artnet_address(last_pixel, dmx_data_channels)
        summaries.append({
            "suit_id": suit["id"],
            "name": suit["name"],
            "pixels": expected_pixels,
            "start_pixel": first_pixel,
            "last_pixel": last_pixel,
            "start_universe": first_u,
            "start_channel": first_ch,
            "last_universe": last_u,
            "last_channel": last_ch,
            "controllers": controller_totals,
            "zones": zone_totals
        })

    return {
        "schema": "lightman.patch_map.v1",
        "source_show": str(show_path),
        "dmx_data_channels": dmx_data_channels,
        "total_pixels": len(patch),
        "patch": patch,
        "summary": summaries
    }


def print_summary(result: dict[str, Any]) -> None:
    print(f"Patch OK: {result['total_pixels']} pixeles")
    print(f"DMX canales utiles: {result['dmx_data_channels']}")
    for suit in result["summary"]:
        print()
        print(f"{suit['name']} ({suit['suit_id']})")
        print(f"  pixeles: {suit['pixels']}")
        print(
            f"  rango pixel: {suit['start_pixel']}..{suit['last_pixel']} | "
            f"Art-Net U{suit['start_universe']}:Ch{suit['start_channel']} "
            f"-> U{suit['last_universe']}:Ch{suit['last_channel']}"
        )
        print("  controladores:")
        for ip, count in suit["controllers"].items():
            print(f"    {ip}: {count} px")
        print("  zonas:")
        for zone, count in suit["zones"].items():
            print(f"    {zone}: {count} px")


def main() -> int:
    parser = argparse.ArgumentParser(description="Generar patch_map desde show_network")
    parser.add_argument("show_network", type=Path)
    parser.add_argument("-o", "--output", type=Path, default=Path("examples/patch_map_example.json"))
    args = parser.parse_args()

    result = build_patch(args.show_network.resolve())
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("w", encoding="utf-8") as f:
        json.dump(result, f, indent=2, ensure_ascii=False)
    print_summary(result)
    print()
    print(f"escrito: {args.output}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

