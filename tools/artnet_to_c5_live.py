#!/usr/bin/env python3
"""Receive Art-Net ArtDmx and send live RGB555 frames to the C5 controller.

The C5 live protocol sends one 1200-pixel frame as two UDP packets:
600 pixels x 2 bytes RGB555 = 1200 payload bytes per packet.
"""

from __future__ import annotations

import argparse
import socket
import struct
import time

OUTPUTS = 6
PIXELS_PER_OUTPUT = 200
CHANNELS_PER_PIXEL = 3
CHANNELS_PER_OUTPUT = PIXELS_PER_OUTPUT * CHANNELS_PER_PIXEL
DMX_CHANNELS = 512
DEFAULT_DMX_DATA_CHANNELS = 510
FRAME_PIXELS = OUTPUTS * PIXELS_PER_OUTPUT
FRAME_BYTES = FRAME_PIXELS * 2
PIXELS_PER_PACKET = 600
C5_PORT = 7777
ARTNET_PORT = 6454

MAGIC = b"C5P6"
VERSION = 2
FORMAT_RGB555_BE = 1
HEADER = struct.Struct("<4sBBBBHBBHHHH")


def checksum16(data: bytes) -> int:
    return sum(data) & 0xFFFF


def rgb555_word(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)


def parse_artdmx(packet: bytes) -> tuple[int, bytes] | None:
    if len(packet) < 18 or packet[:8] != b"Art-Net\x00":
        return None
    opcode = packet[8] | (packet[9] << 8)
    if opcode != 0x5000:
        return None
    universe = packet[14] | (packet[15] << 8)
    length = (packet[16] << 8) | packet[17]
    return universe, packet[18:18 + length]


def ordered_rgb(c0: int, c1: int, c2: int, order: str) -> tuple[int, int, int]:
    if order == "RGB":
        return c0, c1, c2
    if order == "GRB":
        return c1, c0, c2
    if order == "RBG":
        return c0, c2, c1
    if order == "GBR":
        return c2, c0, c1
    if order == "BRG":
        return c1, c2, c0
    if order == "BGR":
        return c2, c1, c0
    raise ValueError(f"orden no soportado: {order}")


def universes_per_output(dmx_data_channels: int) -> int:
    return (CHANNELS_PER_OUTPUT + dmx_data_channels - 1) // dmx_data_channels


def build_frame(universes: list[bytearray], start_universe: int, order: str, dmx_data_channels: int) -> bytes:
    frame = bytearray()
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
            frame.extend(struct.pack(">H", rgb555_word(*ordered_rgb(c0, c1, c2, order))))
    return bytes(frame)


def iter_c5_packets(frame: bytes, frame_id: int):
    for packet_id in range(2):
        pixel_offset = packet_id * PIXELS_PER_PACKET
        payload = frame[pixel_offset * 2:(pixel_offset + PIXELS_PER_PACKET) * 2]
        header = HEADER.pack(
            MAGIC,
            VERSION,
            FORMAT_RGB555_BE,
            0,
            0,
            frame_id & 0xFFFF,
            packet_id,
            2,
            pixel_offset,
            PIXELS_PER_PACKET,
            len(payload),
            checksum16(payload),
        )
        yield header + payload


def run(args: argparse.Namespace) -> None:
    u_per_output = universes_per_output(args.dmx_data_channels)
    total_universes = OUTPUTS * u_per_output
    universes = [bytearray(DMX_CHANNELS) for _ in range(total_universes)]

    artnet_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    artnet_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    artnet_sock.bind((args.bind, args.artnet_port))
    artnet_sock.setblocking(False)

    c5_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    c5_target = (args.c5_ip, args.c5_port)

    frame_id = args.frame_id & 0xFFFF
    sent = 0
    period = 1.0 / args.fps
    next_frame = time.monotonic() + period
    last_report = time.monotonic()
    received_artdmx = 0

    print(
        f"Art-Net {args.bind}:{args.artnet_port} universes "
        f"{args.start_universe}..{args.start_universe + total_universes - 1} "
        f"-> C5 {args.c5_ip}:{args.c5_port}, fps={args.fps}, order={args.order}, "
        f"dmx_data_channels={args.dmx_data_channels}"
    )

    try:
        while True:
            while True:
                try:
                    packet, _addr = artnet_sock.recvfrom(1024)
                except BlockingIOError:
                    break
                parsed = parse_artdmx(packet)
                if parsed is None:
                    continue
                universe, payload = parsed
                index = universe - args.start_universe
                if 0 <= index < total_universes:
                    universes[index][:len(payload)] = payload[:DMX_CHANNELS]
                    received_artdmx += 1

            now = time.monotonic()
            if now >= next_frame:
                frame = build_frame(universes, args.start_universe, args.order, args.dmx_data_channels)
                for c5_packet in iter_c5_packets(frame, frame_id):
                    c5_sock.sendto(c5_packet, c5_target)
                sent += 1
                frame_id = (frame_id + 1) & 0xFFFF
                next_frame += period
                if now - last_report >= 2:
                    print(f"live frames={sent} artdmx={received_artdmx} frame_id={frame_id}")
                    last_report = now
            else:
                time.sleep(min(0.001, next_frame - now))
    except KeyboardInterrupt:
        print(f"\nstop frames={sent} artdmx={received_artdmx}")
    finally:
        artnet_sock.close()
        c5_sock.close()


def main() -> int:
    parser = argparse.ArgumentParser(description="Art-Net to C5 live UDP bridge")
    parser.add_argument("c5_ip", help="C5 IP, e.g. 192.168.1.201")
    parser.add_argument("--c5-port", type=int, default=C5_PORT)
    parser.add_argument("--bind", default="0.0.0.0", help="Local Art-Net bind address")
    parser.add_argument("--artnet-port", type=int, default=ARTNET_PORT)
    parser.add_argument("--start-universe", type=int, default=0)
    parser.add_argument("--fps", type=float, default=30.0)
    parser.add_argument("--frame-id", type=int, default=1)
    parser.add_argument("--order", choices=["RGB", "GRB", "RBG", "GBR", "BRG", "BGR"], default="RGB")
    parser.add_argument("--dmx-data-channels", type=int, choices=[510, 512], default=DEFAULT_DMX_DATA_CHANNELS,
                        help="Canales utiles por universo. 510 ignora canales 511/512 para no partir pixeles RGB.")
    args = parser.parse_args()
    run(args)
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
