#!/usr/bin/env python3
"""Capture Art-Net ArtDmx and write a C5 .lmp3 show file.

The .lmp3 file uses the firmware's LFS2 container:
header + RGB555_BE frames. It is intentionally simple so the C5 can upload or
play it through the same playback path as recorded files.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time
from pathlib import Path

OUTPUTS = 6
PIXELS_PER_OUTPUT = 200
CHANNELS_PER_PIXEL = 3
CHANNELS_PER_OUTPUT = PIXELS_PER_OUTPUT * CHANNELS_PER_PIXEL
DMX_CHANNELS = 512
DEFAULT_DMX_DATA_CHANNELS = 510
FRAME_PIXELS = OUTPUTS * PIXELS_PER_OUTPUT
FRAME_BYTES = FRAME_PIXELS * 2

LFS_MAGIC = b"LFS2"
LFS_VERSION = 2
PIXEL_FORMAT_RGB555_BE = 1
LFS_FLAG_RLE_REPEAT_PREVIOUS = 0x00000001


def rgb555_word(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)


def checksum32_update(checksum: int, data: bytes) -> int:
    for byte in data:
        checksum = ((checksum << 5) | (checksum >> 27)) & 0xFFFFFFFF
        checksum ^= byte
        checksum = (checksum + 0x9E3779B9) & 0xFFFFFFFF
    return checksum


def make_header(fps: int, frame_count: int, flags: int = 0) -> bytes:
    return struct.pack(
        "<4sHHBBHHIII",
        LFS_MAGIC,
        LFS_VERSION,
        FRAME_PIXELS,
        OUTPUTS,
        PIXEL_FORMAT_RGB555_BE,
        PIXELS_PER_OUTPUT,
        fps,
        FRAME_BYTES,
        frame_count,
        flags,
    )


def write_rle_run(file, frame: bytes, repeat: int) -> None:
    file.write(struct.pack("<HH", repeat, FRAME_BYTES))
    file.write(frame)


def parse_artdmx(packet: bytes) -> tuple[int, bytes] | None:
    if len(packet) < 18 or packet[:8] != b"Art-Net\x00":
        return None
    opcode = packet[8] | (packet[9] << 8)
    if opcode != 0x5000:
        return None
    universe = packet[14] | (packet[15] << 8)
    length = (packet[16] << 8) | packet[17]
    payload = packet[18:18 + length]
    return universe, payload


def universes_per_output(dmx_data_channels: int) -> int:
    return (CHANNELS_PER_OUTPUT + dmx_data_channels - 1) // dmx_data_channels


def build_frame(universes: list[bytearray], start_universe: int, color_order: str, dmx_data_channels: int) -> bytes:
    out = bytearray()
    order = color_order.upper()
    u_per_output = universes_per_output(dmx_data_channels)
    for output in range(OUTPUTS):
        dmx = bytearray()
        base_universe = start_universe + output * u_per_output
        for offset in range(u_per_output):
            dmx.extend(universes[base_universe + offset - start_universe][:dmx_data_channels])
        for pixel in range(PIXELS_PER_OUTPUT):
            base = pixel * CHANNELS_PER_PIXEL
            c0 = dmx[base] if base < len(dmx) else 0
            c1 = dmx[base + 1] if base + 1 < len(dmx) else 0
            c2 = dmx[base + 2] if base + 2 < len(dmx) else 0
            if order == "RGB":
                r, g, b = c0, c1, c2
            elif order == "GRB":
                g, r, b = c0, c1, c2
            elif order == "RBG":
                r, b, g = c0, c1, c2
            elif order == "GBR":
                g, b, r = c0, c1, c2
            elif order == "BRG":
                b, r, g = c0, c1, c2
            elif order == "BGR":
                b, g, r = c0, c1, c2
            else:
                raise ValueError(f"orden no soportado: {color_order}")
            out.extend(struct.pack(">H", rgb555_word(r, g, b)))
    return bytes(out)


def main() -> int:
    parser = argparse.ArgumentParser(description="Capture Art-Net and write C5 .lmp3")
    parser.add_argument("output", type=Path, help="Output .lmp3 path")
    parser.add_argument("--bind", default="0.0.0.0", help="Bind address")
    parser.add_argument("--port", type=int, default=6454, help="Art-Net UDP port")
    parser.add_argument("--start-universe", type=int, default=0, help="First Art-Net universe")
    parser.add_argument("--fps", type=int, default=30, help="Output show FPS")
    parser.add_argument("--seconds", type=float, default=10.0, help="Capture duration")
    parser.add_argument("--frames", type=int, default=0, help="Frame count override")
    parser.add_argument("--order", choices=["RGB", "GRB", "RBG", "GBR", "BRG", "BGR"], default="RGB",
                        help="Incoming Art-Net channel color order")
    parser.add_argument("--dmx-data-channels", type=int, choices=[510, 512], default=DEFAULT_DMX_DATA_CHANNELS,
                        help="Canales utiles por universo. 510 ignora canales 511/512 para no partir pixeles RGB.")
    parser.add_argument("--no-rle", action="store_true",
                        help="Write raw frames instead of RLE-compressed repeated frames")
    args = parser.parse_args()

    frame_limit = args.frames if args.frames > 0 else int(round(args.seconds * args.fps))
    if frame_limit <= 0:
        raise SystemExit("frames/seconds debe producir al menos 1 frame")

    u_per_output = universes_per_output(args.dmx_data_channels)
    total_universes = OUTPUTS * u_per_output
    universe_data = [bytearray(DMX_CHANNELS) for _ in range(total_universes)]
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    sock.bind((args.bind, args.port))
    sock.setblocking(False)

    checksum = 0
    rle_enabled = not args.no_rle
    flags = LFS_FLAG_RLE_REPEAT_PREVIOUS if rle_enabled else 0
    last_frame: bytes | None = None
    repeat = 0
    runs = 0
    period = 1.0 / args.fps
    next_frame = time.monotonic() + period
    written = 0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    with args.output.open("wb") as f:
        f.write(make_header(args.fps, 0, flags))
        while written < frame_limit:
            now = time.monotonic()
            while True:
                try:
                    packet, _addr = sock.recvfrom(1024)
                except BlockingIOError:
                    break
                parsed = parse_artdmx(packet)
                if parsed is None:
                    continue
                universe, payload = parsed
                index = universe - args.start_universe
                if 0 <= index < total_universes:
                    universe_data[index][:len(payload)] = payload[:DMX_CHANNELS]
            if now >= next_frame:
                frame = build_frame(universe_data, args.start_universe, args.order, args.dmx_data_channels)
                if rle_enabled:
                    if last_frame is None:
                        last_frame = frame
                        repeat = 1
                    elif frame == last_frame and repeat < 0xFFFF:
                        repeat += 1
                    else:
                        write_rle_run(f, last_frame, repeat)
                        runs += 1
                        last_frame = frame
                        repeat = 1
                else:
                    f.write(frame)
                checksum = checksum32_update(checksum, frame)
                written += 1
                next_frame += period
            else:
                time.sleep(min(0.002, next_frame - now))
        if rle_enabled and last_frame is not None:
            write_rle_run(f, last_frame, repeat)
            runs += 1
        f.seek(0)
        f.write(make_header(args.fps, written, flags))

    print(f"OK {args.output} frames={written} fps={args.fps} rle={int(rle_enabled)} runs={runs if rle_enabled else written} checksum=0x{checksum:08x}")
    print(f"Universes: {args.start_universe}..{args.start_universe + total_universes - 1} dmx_data_channels={args.dmx_data_channels}")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
