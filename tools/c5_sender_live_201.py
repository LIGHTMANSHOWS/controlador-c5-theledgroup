#!/usr/bin/env python3
"""Launcher listo para usar: Art-Net localhost -> C5 192.168.1.201 a 30 fps."""

from __future__ import annotations

import sys

from artnet_to_c5_live import main


if __name__ == "__main__":
    if len(sys.argv) == 1:
        sys.argv.extend([
            "192.168.1.201",
            "--bind", "127.0.0.1",
            "--artnet-port", "6454",
            "--fps", "30",
            "--order", "RGB",
            "--dmx-data-channels", "510",
        ])
    raise SystemExit(main())
