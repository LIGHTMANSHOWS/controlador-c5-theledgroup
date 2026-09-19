#!/usr/bin/env python3
"""Tester práctico por zona usando patch_map.json, sin compilar EXE."""

from __future__ import annotations

import argparse
import json
import socket
import struct
import time
from pathlib import Path
from typing import Any


PIXELS_PER_CONTROLLER = 1200
PIXELS_PER_PACKET = 600
C5_PORT = 7777
MAGIC = b"C5P6"
VERSION = 2
FORMAT_RGB555_BE = 1
HEADER = struct.Struct("<4sBBBBHBBHHHH")


COLORS = {
    "black": (0, 0, 0),
    "white": (180, 180, 180),
    "red": (180, 0, 0),
    "green": (0, 180, 0),
    "blue": (0, 0, 180),
    "yellow": (180, 180, 0),
    "cyan": (0, 180, 180),
    "magenta": (180, 0, 180),
}


def checksum16(data: bytes) -> int:
    return sum(data) & 0xFFFF


def rgb555_word(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)


def load_json(path: Path) -> dict[str, Any]:
    with path.open("r", encoding="utf-8") as f:
        return json.load(f)


def iter_packets(frame: bytes, frame_id: int):
    for packet_id in range(2):
        pixel_offset = packet_id * PIXELS_PER_PACKET
        payload = frame[pixel_offset * 2:(pixel_offset + PIXELS_PER_PACKET) * 2]
        yield HEADER.pack(
            MAGIC, VERSION, FORMAT_RGB555_BE, 0, 0,
            frame_id & 0xFFFF, packet_id, 2, pixel_offset,
            PIXELS_PER_PACKET, len(payload), checksum16(payload)
        ) + payload


def output_to_local_pixel(output: int, output_pixel: int) -> int:
    return (output - 1) * 200 + output_pixel


def build_frames(patch: list[dict[str, Any]], zone: str, color: tuple[int, int, int]) -> dict[str, bytes]:
    words_by_ip: dict[str, list[int]] = {}
    for row in patch:
        ip = row["controller_ip"]
        words_by_ip.setdefault(ip, [0] * PIXELS_PER_CONTROLLER)
        if zone == "*" or row["zone"] == zone:
            local_pixel = output_to_local_pixel(int(row["output"]), int(row["output_pixel"]))
            if 1 <= local_pixel <= PIXELS_PER_CONTROLLER:
                words_by_ip[ip][local_pixel - 1] = rgb555_word(*color)

    frames: dict[str, bytes] = {}
    for ip, words in words_by_ip.items():
        frame = bytearray()
        for word in words:
            frame.extend(struct.pack(">H", word))
        frames[ip] = bytes(frame)
    return frames


def main() -> int:
    parser = argparse.ArgumentParser(description="Enviar tester por zona usando patch_map")
    parser.add_argument("patch_map", type=Path)
    parser.add_argument("--zone", default="*", help="Nombre de zona, o * para todo")
    parser.add_argument("--color", choices=sorted(COLORS), default="red")
    parser.add_argument("--fps", type=float, default=25)
    parser.add_argument("--seconds", type=float, default=2)
    parser.add_argument("--port", type=int, default=C5_PORT)
    args = parser.parse_args()

    data = load_json(args.patch_map)
    patch = data["patch"]
    color = COLORS[args.color]
    frames = build_frames(patch, args.zone, color)
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    frame_count = max(1, int(args.fps * args.seconds))

    print(f"zona={args.zone} color={args.color} fps={args.fps} segundos={args.seconds}")
    print("destinos:")
    for ip in frames:
        print(f"  {ip}")

    for frame_id in range(1, frame_count + 1):
        started = time.monotonic()
        for ip, frame in frames.items():
            for packet in iter_packets(frame, frame_id):
                sock.sendto(packet, (ip, args.port))
        remaining = (1 / args.fps) - (time.monotonic() - started)
        if remaining > 0:
            time.sleep(remaining)

    sock.close()
    print(f"enviado {frame_count} frames por destino")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())

