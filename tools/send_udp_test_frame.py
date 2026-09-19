#!/usr/bin/env python3
"""Build, validate and send C5 RGB555 two-packet frames."""
import argparse
import socket
import struct
import time

PIXELS = 1200
OUTPUTS = 6
PIXELS_PER_OUTPUT = 200
PIXELS_PER_PACKET = 600
UDP_PORT = 7777
VERSION = 2
FORMAT_RGB555_BE = 1
MAGIC = b"C5P6"
ACK_MAGIC = b"C5AK"
HEADER = struct.Struct("<4sBBBBHBBHHHH")
ACK = struct.Struct("<4sBBHHHHB")


def checksum16(data: bytes) -> int:
    return sum(data) & 0xFFFF


def rgb555_word(r: int, g: int, b: int) -> int:
    return ((r >> 3) << 10) | ((g >> 3) << 5) | (b >> 3)


def expand_rgb555(word: int) -> tuple[int, int, int]:
    values = ((word >> 10) & 31, (word >> 5) & 31, word & 31)
    return tuple((v << 3) | (v >> 2) for v in values)


def pixel(pattern: str, index: int, frame_id: int) -> tuple[int, int, int]:
    output, pos = divmod(index, PIXELS_PER_OUTPUT)
    solids = {
        "black": (0, 0, 0), "white": (255, 255, 255),
        "gray5": (13, 13, 13), "gray10": (26, 26, 26),
        "gray25": (64, 64, 64), "gray50": (128, 128, 128),
        "gray75": (191, 191, 191),
    }
    if pattern in solids:
        return solids[pattern]
    if pattern == "gray-ramp":
        level = (pos * 255) // (PIXELS_PER_OUTPUT - 1)
        return level, level, level
    if pattern == "outputs":
        return ((255, 0, 0), (0, 255, 0), (0, 0, 255),
                (255, 255, 0), (0, 255, 255), (255, 0, 255))[output]
    if pattern == "scan":
        return (255, 255, 255) if pos == frame_id % PIXELS_PER_OUTPUT else (0, 0, 0)
    return 0, 0, 0


def build_frame(pattern: str, frame_id: int) -> bytes:
    out = bytearray()
    for index in range(PIXELS):
        out += struct.pack(">H", rgb555_word(*pixel(pattern, index, frame_id)))
    return bytes(out)


def iter_packets(frame: bytes, frame_id: int):
    for packet_id in range(2):
        pixel_offset = packet_id * PIXELS_PER_PACKET
        payload = frame[pixel_offset * 2:(pixel_offset + PIXELS_PER_PACKET) * 2]
        yield HEADER.pack(MAGIC, VERSION, FORMAT_RGB555_BE, 0, 0,
                          frame_id & 0xFFFF, packet_id, 2, pixel_offset,
                          PIXELS_PER_PACKET, len(payload), checksum16(payload)) + payload


def self_test() -> None:
    for level in range(32):
        word = (level << 10) | (level << 5) | level
        r, g, b = expand_rgb555(word)
        assert r == g == b
    frame = build_frame("gray-ramp", 123)
    packets = list(iter_packets(frame, 123))
    assert len(frame) == 2400 and len(packets) == 2
    rebuilt = bytearray(2400)
    for expected_id, packet in enumerate(packets):
        fields = HEADER.unpack_from(packet)
        magic, version, fmt, flags, reserved, frame_id, packet_id, count, offset, pixels, size, crc = fields
        payload = packet[HEADER.size:]
        assert (magic, version, fmt, packet_id, count) == (MAGIC, VERSION, FORMAT_RGB555_BE, expected_id, 2)
        assert pixels == 600 and size == 1200 and crc == checksum16(payload)
        rebuilt[offset * 2:offset * 2 + size] = payload
    assert bytes(rebuilt) == frame
    print(f"OK RGB555: 32 grays neutral, frame={len(frame)}, packets=2, datagram={len(packets[0])}")


def send(args) -> None:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if args.wait_ack:
        sock.settimeout(args.ack_timeout)
    delay = 1 / args.fps if args.fps else 0
    frame_id = args.frame_id
    sent = 0
    while args.count == 0 or sent < args.count:
        started = time.monotonic()
        for packet in iter_packets(build_frame(args.pattern, frame_id), frame_id):
            sock.sendto(packet, (args.ip, args.port))
        if args.wait_ack:
            deadline = time.monotonic() + args.ack_timeout
            while time.monotonic() < deadline:
                try:
                    data, _ = sock.recvfrom(128)
                except socket.timeout:
                    break
                if len(data) == ACK.size and ACK.unpack(data)[0] == ACK_MAGIC and ACK.unpack(data)[3] == (frame_id & 0xFFFF) and ACK.unpack(data)[-1] == 2:
                    break
        sent += 1
        frame_id = (frame_id + 1) & 0xFFFF
        remaining = delay - (time.monotonic() - started)
        if remaining > 0:
            time.sleep(remaining)
    sock.close()
    print(f"sent {sent} frame(s), {sent * 2} packet(s) to {args.ip}:{args.port}")


def main() -> None:
    parser = argparse.ArgumentParser(description="C5 ESP32-C5 RGB555 UDP sender/test")
    parser.add_argument("ip", nargs="?")
    parser.add_argument("--port", type=int, default=UDP_PORT)
    parser.add_argument("--frame-id", type=int, default=1)
    parser.add_argument("--pattern", choices=("black", "white", "gray5", "gray10", "gray25", "gray50", "gray75", "gray-ramp", "outputs", "scan"), default="outputs")
    parser.add_argument("--fps", type=float, default=30)
    parser.add_argument("--count", type=int, default=1, help="0 runs continuously")
    parser.add_argument("--wait-ack", action="store_true")
    parser.add_argument("--ack-timeout", type=float, default=0.1)
    parser.add_argument("--self-test", action="store_true")
    args = parser.parse_args()
    if args.self_test:
        self_test()
    elif not args.ip:
        parser.error("ip is required unless --self-test is used")
    else:
        send(args)


if __name__ == "__main__":
    main()
