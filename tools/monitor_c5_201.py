#!/usr/bin/env python3
"""Monitor simple para el controlador C5 en 192.168.1.201.

Lee http://IP/status y muestra una linea compacta cada segundo.
"""

from __future__ import annotations

import argparse
import json
import time
import urllib.error
import urllib.request


def fetch_status(ip: str, timeout: float) -> dict:
    url = f"http://{ip}/status"
    with urllib.request.urlopen(url, timeout=timeout) as response:
        raw = response.read().decode("utf-8", errors="replace")
    return json.loads(raw)


def yesno(value: object) -> str:
    return "si" if bool(value) else "no"


def fmt_ms(ms: object) -> str:
    try:
        value = int(ms)
    except (TypeError, ValueError):
        return "?"
    if value < 1000:
        return f"{value}ms"
    return f"{value / 1000:.1f}s"


def nested(data: dict, key: str) -> dict:
    value = data.get(key)
    return value if isinstance(value, dict) else {}


def print_status(data: dict) -> None:
    now = time.strftime("%H:%M:%S")
    cfg = nested(data, "config")
    sd = nested(data, "sd")
    udp_idle = data.get("udp_idle_ms", data.get("udp_ms_since_last_packet", "?"))
    udp_frames = data.get("udp_completed_frames", data.get("completed_frames", "?"))
    udp_packets = data.get("udp_valid_packets", data.get("valid_packets", "?"))
    led_fps = data.get("led_fps", "?")
    dropped = data.get("led_dropped_frames", data.get("dropped_frames", "?"))
    heap = data.get("free_heap", data.get("heap_free", "?"))
    ip = cfg.get("wifi_ip", data.get("ip", "?"))
    rssi = data.get("wifi_rssi", "?")

    print(
        f"{now} C5 {ip} ok={yesno(data.get('ok'))} "
        f"rssi={rssi} heap={heap} "
        f"udp_idle={fmt_ms(udp_idle)} udp_pkt={udp_packets} udp_frames={udp_frames} "
        f"led_fps={led_fps} drop={dropped} "
        f"sd={yesno(sd.get('mounted', sd.get('sd_mounted', False)))}",
        flush=True,
    )


def main() -> int:
    parser = argparse.ArgumentParser(description="Monitor HTTP /status para C5")
    parser.add_argument("ip", nargs="?", default="192.168.1.201", help="IP del C5")
    parser.add_argument("--interval", type=float, default=1.0, help="segundos entre lecturas")
    parser.add_argument("--timeout", type=float, default=2.0, help="timeout HTTP en segundos")
    parser.add_argument("--once", action="store_true", help="leer una vez y salir")
    args = parser.parse_args()

    while True:
        try:
            print_status(fetch_status(args.ip, args.timeout))
        except (urllib.error.URLError, TimeoutError, json.JSONDecodeError, OSError) as exc:
            print(f"{time.strftime('%H:%M:%S')} C5 {args.ip} sin respuesta: {exc}", flush=True)

        if args.once:
            return 0
        time.sleep(max(0.1, args.interval))


if __name__ == "__main__":
    raise SystemExit(main())
