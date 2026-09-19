#!/usr/bin/env python3
"""C5 Sender Manager: Art-Net -> C5 + servidor web local."""

from __future__ import annotations

import argparse
import base64
import http.client
import ipaddress
import json
import os
import shutil
import socket
import struct
import sys
import threading
import time
import webbrowser
import xml.etree.ElementTree as ET
from dataclasses import dataclass, asdict
from http.server import BaseHTTPRequestHandler, ThreadingHTTPServer
from pathlib import Path
from typing import Any
from urllib.parse import urlencode

OUTPUTS = 6
PIXELS_PER_OUTPUT = 200  # fallback profile when no map is loaded
MAX_PIXELS_PER_OUTPUT = 600
MAX_PIXELS_PER_CONTROLLER = 1200
CHANNELS_PER_PIXEL = 3
CHANNELS_PER_OUTPUT = PIXELS_PER_OUTPUT * CHANNELS_PER_PIXEL
DMX_CHANNELS = 512
DEFAULT_DMX_DATA_CHANNELS = 510
PIXELS_PER_PACKET = 600
C5_PORT = 7777
ARTNET_PORT = 6454
UNIVERSES_PER_CONTROLLER = 8  # ceil(1200 RGB pixels / 170 pixels per 510-channel universe)
XLIGHTS_DISCOVERY_UNIVERSES = 20 * UNIVERSES_PER_CONTROLLER
XLIGHTS_NETWORK_FILE = "xlights_networks.xml"
XLIGHTS_BRIDGE_NAME = "C5 Sender Manager"
XLIGHTS_BRIDGE_DESCRIPTION = "xLights envia Art-Net a localhost; C5 Sender Manager deriva a los C5"
SETTINGS_FILE = "c5_sender_settings.json"

# RGB555 tiene 32 niveles por canal (0..31). Nivel global 0 = apagado;
# niveles 1..8 multiplican 31 y hacen ``nivel * tope / 31`` exacto.
RGB555_STABLE_BRIGHTNESS_VALUES = tuple(range(0, 249, 31))


def normalize_max_brightness_value(value: int | float) -> int:
    """Devuelve uno de los nueve topes RGB555 estables (0..248)."""
    number = int(round(float(value)))
    if not 0 <= number <= 255:
        raise ValueError("El brillo RGB555 debe estar entre 0 y 255")
    return min(RGB555_STABLE_BRIGHTNESS_VALUES, key=lambda allowed: abs(allowed - number))

MAGIC = b"C5P6"
VERSION = 2
FORMAT_RGB555_BE = 1
FLAG_C5_BRIGHTNESS_LIMIT = 0x01
HEADER = struct.Struct("<4sBBBBHBBHHHH")


def logo_data_uri() -> str:
    candidates = []
    if hasattr(sys, "_MEIPASS"):
        candidates.append(os.path.join(sys._MEIPASS, "assets", "tlg-logo.png"))
    candidates.append(os.path.join(os.path.dirname(os.path.dirname(__file__)), "assets", "tlg-logo.png"))
    for path in candidates:
        try:
            with open(path, "rb") as f:
                return "data:image/png;base64," + base64.b64encode(f.read()).decode("ascii")
        except OSError:
            pass
    return ""


HTML = r"""<!doctype html>
<html lang="es">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>C5 Sender Manager</title>
  <style>
    :root{color-scheme:dark;--bg:#0b1020;--panel:#121a2e;--text:#e9eefc;--muted:#8ea0c4;--line:#263451;--good:#32d583;--bad:#ff5c7a;--warn:#fdb022;--input:#0c1324}
    *{box-sizing:border-box} body{margin:0;font-family:Segoe UI,Arial,sans-serif;background:radial-gradient(circle at top left,#152142,var(--bg) 42%);color:var(--text)}
    header{padding:20px 24px;border-bottom:1px solid var(--line);background:rgba(11,16,32,.92);position:sticky;top:0}
    h1{margin:0;font-size:22px}.sub{color:var(--muted);font-size:13px;margin-top:6px}.brand{display:flex;align-items:center;gap:16px}.logo{width:140px;max-height:58px;object-fit:contain;filter:drop-shadow(0 0 10px rgba(255,0,0,.45))}
    main{padding:18px;display:grid;gap:16px}.grid{display:grid;grid-template-columns:repeat(12,1fr);gap:16px}
    .card{background:linear-gradient(180deg,rgba(23,33,58,.94),rgba(18,26,46,.94));border:1px solid var(--line);border-radius:16px;padding:16px;box-shadow:0 14px 40px rgba(0,0,0,.24)}
    .s3{grid-column:span 3}.s4{grid-column:span 4}.s5{grid-column:span 5}.s7{grid-column:span 7}.s12{grid-column:span 12}
    h2{font-size:16px;margin:0 0 12px}.metric{font-size:28px;font-weight:800}.metric small{font-size:13px;color:var(--muted)}
    .pill{display:inline-flex;gap:7px;align-items:center;border:1px solid var(--line);border-radius:999px;padding:5px 9px;color:var(--muted);background:rgba(12,19,36,.7)}
    .dot{width:9px;height:9px;border-radius:50%;background:var(--good)}.bad .dot{background:var(--bad)}.warn .dot{background:var(--warn)}
    button,input,select{width:100%;border:1px solid var(--line);background:var(--input);color:var(--text);border-radius:10px;padding:10px;font-size:14px}
    button{border:0;font-weight:800;cursor:pointer;background:linear-gradient(180deg,#1d9bd1,#1377ad)}button.good{background:#087443}button.danger{background:#9f1239}button.secondary{background:#253653}
    .row{display:grid;grid-template-columns:repeat(12,1fr);gap:10px;align-items:end}.c3{grid-column:span 3}.c4{grid-column:span 4}.c6{grid-column:span 6}.c12{grid-column:span 12}
    label{display:block;color:var(--muted);font-size:12px;margin:8px 0 5px}.note{color:var(--muted);font-size:12px;line-height:1.45;background:rgba(12,19,36,.7);border:1px solid var(--line);border-radius:12px;padding:10px}
    table{width:100%;border-collapse:collapse;font-size:13px}th,td{border-bottom:1px solid var(--line);padding:8px;text-align:left}th{color:var(--muted)}
    @media(max-width:900px){.s3,.s4,.s5,.s7{grid-column:span 12}.c3,.c4,.c6{grid-column:span 12}}
  </style>
</head>
<body>
<header><div class="brand"><img class="logo" src="__LOGO__"><div><h1>C5 Sender Manager</h1><div class="sub">Art-Net → C5 201..220 · 30 fps · RGB · 510 canales útiles · <a href="/tablet" style="color:#54d1ff">vista tablet</a></div></div></div></header>
<main>
  <section class="grid">
    <div class="card s3"><h2>Sender</h2><div id="senderPill" class="pill"><span class="dot"></span><span id="running">...</span></div><div class="metric" id="fps">-- <small>fps</small></div></div>
    <div class="card s3"><h2>Art-Net</h2><div class="metric" id="artdmx">--</div><div class="sub">paquetes recibidos</div></div>
    <div class="card s3"><h2>C5 UDP</h2><div class="metric" id="frames">--</div><div class="sub">frames enviados</div></div>
    <div class="card s3"><h2>Último Art-Net</h2><div class="metric" id="idle">-- <small>ms</small></div></div>
  </section>
  <section class="grid">
    <div class="card s7">
      <h2>Control live</h2>
      <div class="row">
        <div class="c3"><button class="good" onclick="cmd('start')">START LIVE</button></div>
        <div class="c3"><button class="secondary" onclick="cmd('pause')">PAUSE</button></div>
        <div class="c3"><button class="danger" onclick="cmd('blackout')">BLACKOUT</button></div>
        <div class="c3"><button class="secondary" onclick="cmd('stop')">STOP</button></div>
        <div class="c12"><div class="note">Deja el EXE abierto. Resolume/xLights debe mandar Art-Net a 127.0.0.1:6454.</div></div>
      </div>
    </div>
    <div class="card s5">
      <h2>Tester rápido</h2>
      <div class="row">
        <div class="c6"><label>Modo</label><select id="testMode"><option value="outputs">Salidas por color</option><option value="white">Blanco</option><option value="red">Rojo</option><option value="green">Verde</option><option value="blue">Azul</option><option value="rainbow">Arcoiris 5s</option></select></div>
        <div class="c6"><label>FPS</label><select id="testFps"><option>30</option><option>25</option></select></div>
        <div class="c6"><button onclick="tester()">Enviar tester</button></div>
        <div class="c6"><button class="secondary" onclick="cmd('start')">Volver a live</button></div>
      </div>
      <label>Perfil de salida</label>
      <div class="row">
        <div class="c6"><select id="brightnessMode"><option value="current_limit">Limitador corriente</option><option value="max_brightness">Brillo máximo</option></select></div>
        <div class="c6"><label>Amperios</label><input id="currentLimit" type="number" min="0.1" max="3" step="0.1" value="2.5"></div>
        <div class="c6"><label>Nivel RGB555: 0 apagado; 1–8</label><input id="maxBrightness" type="number" min="0" max="8" step="1" value="3"></div>
        <div class="c6"><label>Gamma (C5 antiguo)</label><input id="brightnessGamma" type="number" min="0.4" max="4" step="0.1" value="1.6"></div>
        <div class="c6"><label><input id="temporalDither" type="checkbox" checked> Dither (C5 antiguo)</label></div>
        <div class="c6"><button class="secondary" onclick="saveBrightnessProfile()">Guardar perfil</button></div>
      </div>
      <div class="note">Nivel 0 apaga. Niveles 1–8 equivalen a 31, 62, 93, 124, 155, 186, 217 y 248: múltiplos de 31 sin residuo ni dither.</div>
    </div>
  </section>
  <section class="card">
    <h2>Controladores</h2>
    <table><thead><tr><th>#</th><th>IP</th><th>Estado</th><th>FPS</th><th>Frames</th><th>Packets C5</th><th>Canales</th></tr></thead><tbody>
      <tr><td>#1</td><td>192.168.1.201</td><td><span class="pill"><span class="dot"></span> Enviando</span></td><td id="fps2">--</td><td id="frames2">--</td><td id="packets2">--</td><td>510 · ignora 511/512</td></tr>
    </tbody></table>
  </section>
  <section class="card"><h2>Log</h2><div class="note" id="log" style="font-family:Consolas,monospace;min-height:100px"></div></section>
</main>
<script>
async function status(){
  const r=await fetch('/api/status'); const s=await r.json();
  running.textContent=s.running?'Live activo':'Pausado';
  senderPill.className='pill '+(s.running?'':'warn');
  fps.innerHTML=s.fps+' <small>fps</small>'; fps2.textContent=s.fps;
  artdmx.textContent=s.received_artdmx; frames.textContent=s.sent_frames; frames2.textContent=s.sent_frames;
  packets2.textContent=s.sent_packets; idle.innerHTML=s.artnet_idle_ms+' <small>ms</small>';
  if(document.activeElement!==currentLimit) currentLimit.value=s.current_limit_a;
  if(document.activeElement!==maxBrightness) maxBrightness.value=Math.round(s.max_brightness_value/31);
  if(document.activeElement!==brightnessGamma) brightnessGamma.value=s.brightness_gamma;
  brightnessMode.value=s.brightness_mode; temporalDither.checked=s.temporal_dither;
  log.innerHTML=s.log.map(x=>x.replaceAll('&','&amp;').replaceAll('<','&lt;')).join('<br>');
}
async function cmd(action){await fetch('/api/control',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({action})}); status();}
async function tester(){await fetch('/api/tester',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({mode:testMode.value,fps:Number(testFps.value)})}); status();}
async function saveBrightnessProfile(){const r=await fetch('/api/brightness-profile',{method:'POST',headers:{'Content-Type':'application/json'},body:JSON.stringify({mode:brightnessMode.value,limit_a:Number(currentLimit.value),max_value:Number(maxBrightness.value)*31,gamma:Number(brightnessGamma.value),dither:false})});if(!r.ok) alert((await r.json()).error);status();}
setInterval(status,700); status();
</script>
</body></html>"""


TABLET_HTML = r"""<!doctype html>
<html lang="es">
<head>
  <meta charset="utf-8">
  <meta name="viewport" content="width=device-width, initial-scale=1">
  <title>TLG C5 Tablet Monitor</title>
  <style>
    :root{color-scheme:dark;--bg:#05070d;--panel:#111827;--text:#f8fafc;--muted:#94a3b8;--line:#263244;--good:#32d583;--bad:#ff5c7a;--warn:#fdb022}
    *{box-sizing:border-box} body{margin:0;background:radial-gradient(circle at top,#1b1020,#05070d 48%);font-family:Segoe UI,Arial,sans-serif;color:var(--text)}
    header{position:sticky;top:0;z-index:2;background:rgba(5,7,13,.92);backdrop-filter:blur(8px);border-bottom:1px solid var(--line);padding:12px 14px;display:flex;align-items:center;gap:14px}
    .logo{width:124px;max-height:48px;object-fit:contain;filter:drop-shadow(0 0 12px rgba(255,0,0,.55))}.sub{color:var(--muted);font-size:12px}
    main{padding:12px}.grid{display:grid;grid-template-columns:repeat(auto-fit,minmax(168px,1fr));gap:10px}.card{background:linear-gradient(180deg,#121a2e,#0c1324);border:1px solid var(--line);border-radius:14px;padding:12px}
    .card.online{border-color:rgba(50,213,131,.65);box-shadow:0 0 18px rgba(50,213,131,.08)}.card.offline{opacity:.48}.top{display:flex;align-items:center;justify-content:space-between;font-weight:800}.dot{width:10px;height:10px;border-radius:50%;background:var(--good);display:inline-block;margin-right:6px}.offline .dot{background:#64748b}.metric{font-size:12px;color:var(--muted);line-height:1.7;margin-top:8px}.big{font-size:18px;color:var(--text);font-weight:900}
  </style>
</head>
<body>
  <header><img class="logo" src="__LOGO__"><div><div class="big">Tablet monitor · C5 201..220</div><div class="sub">Estado, RSSI, RX FPS, LED FPS, drops y paquetes · auto-refresh 1s</div></div></header>
  <main><div class="grid" id="grid"></div></main>
<script>
async function refresh(){
  const r=await fetch('/api/status'); const s=await r.json();
  grid.innerHTML='';
  for(const c of s.controllers){
    const cls=c.online?'online':'offline';
    grid.insertAdjacentHTML('beforeend',`
      <div class="card ${cls}">
        <div class="top"><span>#${String(c.num).padStart(2,'0')} / ${c.ip}</span><span><span class="dot"></span>${c.online?'OK':'OFF'}</span></div>
        <div class="metric">
          Habilitado: ${c.enabled?'sí':'no'}<br>
          Estado: ${c.last_status || '-'}<br>
          Frames enviados: ${c.sent_frames}<br>
          Paquetes enviados: ${c.sent_packets}<br>
          LED estimado: ${c.estimated_current_a} A · ${c.brightness_mode==='brillo C5'?'tope '+s.max_brightness_value+'/255':c.brightness_mode+' '+Math.round(c.current_scale*100)+'%'}<br>
          Salidas activas: ${c.active_outputs_mask}<br>
          Global: controlador #${c.num}
        </div>
      </div>`);
  }
}
setInterval(refresh,1000); refresh();
</script>
</body>
</html>"""


@dataclass
class AppConfig:
    c5_ip: str = "192.168.1.201"
    c5_port: int = C5_PORT
    c5_source_ip: str = "0.0.0.0"
    bind: str = "127.0.0.1"
    artnet_port: int = ARTNET_PORT
    web_bind: str = "0.0.0.0"
    web_port: int = 8080
    fps: float = 30.0
    order: str = "RGB"
    dmx_data_channels: int = DEFAULT_DMX_DATA_CHANNELS
    start_universe: int = 0
    first_controller_ip: int = 201
    controller_count: int = 20
    patch_map_path: str = ""
    current_limit_a: float = 2.5  # estimación por controlador, fuente de 12 V / 3 A
    ws2815_channel_ma: float = 15.0
    brightness_mode: str = "current_limit"  # current_limit | max_brightness
    max_brightness_value: int = 93
    brightness_gamma: float = 1.6
    temporal_dither: bool = True


def settings_path() -> Path:
    base = Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else Path(__file__).resolve().parent.parent
    return base / SETTINGS_FILE


def load_output_settings() -> dict[str, Any]:
    try:
        data = json.loads(settings_path().read_text(encoding="utf-8"))
        if not isinstance(data, dict):
            data = {}
    except (OSError, json.JSONDecodeError):
        data = {}

    mode = str(data.get("brightness_mode", "current_limit"))
    if mode not in ("current_limit", "max_brightness"):
        mode = "current_limit"

    def number(name: str, default: float, minimum: float, maximum: float) -> float:
        try:
            value = float(data.get(name, default))
            if minimum <= value <= maximum:
                return value
        except (ValueError, TypeError):
            pass
        return default

    try:
        if "max_brightness_value" in data:
            max_value = normalize_max_brightness_value(data["max_brightness_value"])
        else:
            max_value = round(number("max_brightness_percent", 32.0, 0.0, 100.0) * 255 / 100)
    except (ValueError, TypeError):
        max_value = 82
    if not 0 <= max_value <= 255:
        max_value = 82
    max_value = normalize_max_brightness_value(max_value)
    source_ip = str(data.get("c5_source_ip", "auto"))
    try:
        if source_ip != "auto":
            ipaddress.IPv4Address(source_ip)
    except ipaddress.AddressValueError:
        source_ip = "auto"

    return {
        "current_limit_a": number("current_limit_a", 2.5, 0.1, 3.0),
        "brightness_mode": mode,
        "max_brightness_value": max_value,
        "brightness_gamma": number("brightness_gamma", 1.6, 0.4, 4.0),
        "temporal_dither": bool(data.get("temporal_dither", True)),
        "c5_source_ip": source_ip,
    }


def load_current_limit() -> float:
    return float(load_output_settings()["current_limit_a"])


def save_output_settings(data: dict[str, Any]) -> None:
    mode = str(data.get("brightness_mode", "current_limit"))
    if mode not in ("current_limit", "max_brightness"):
        raise ValueError("Modo inválido")
    current_limit_a = float(data.get("current_limit_a", 2.5))
    max_brightness_value = normalize_max_brightness_value(data.get("max_brightness_value", 82))
    brightness_gamma = float(data.get("brightness_gamma", 1.6))
    temporal_dither = bool(data.get("temporal_dither", True))
    c5_source_ip = str(data.get("c5_source_ip", "auto"))
    if c5_source_ip != "auto":
        ipaddress.IPv4Address(c5_source_ip)
    if not 0.1 <= current_limit_a <= 3.0:
        raise ValueError("El límite debe estar entre 0,1 y 3,0 A")
    if not 0 <= max_brightness_value <= 255:
        raise ValueError("El brillo máximo debe estar entre 0 y 255")
    if not 0.4 <= brightness_gamma <= 4.0:
        raise ValueError("La curva gamma debe estar entre 0,4 y 4,0")
    path = settings_path()
    temporary = path.with_suffix(".tmp")
    temporary.write_text(json.dumps({
        "current_limit_a": current_limit_a,
        "brightness_mode": mode,
        "max_brightness_value": max_brightness_value,
        "brightness_gamma": brightness_gamma,
        "temporal_dither": temporal_dither,
        "c5_source_ip": c5_source_ip,
    }, indent=2), encoding="utf-8")
    temporary.replace(path)


def save_current_limit(value: float) -> None:
    settings = load_output_settings()
    settings["current_limit_a"] = value
    save_output_settings(settings)


def save_brightness_profile(mode: str, max_value: int, gamma: float, dither: bool,
                            current_limit_a: float) -> int:
    max_value = normalize_max_brightness_value(max_value)
    settings = load_output_settings()
    settings.update({
        "current_limit_a": current_limit_a,
        "brightness_mode": mode,
        "max_brightness_value": max_value,
        "brightness_gamma": gamma,
        "temporal_dither": dither,
    })
    save_output_settings(settings)
    return max_value


def save_c5_source_ip(ip: str) -> None:
    settings = load_output_settings()
    settings["c5_source_ip"] = ip
    save_output_settings(settings)


class SharedState:
    def __init__(self, cfg: AppConfig) -> None:
        self.cfg = cfg
        self.lock = threading.Lock()
        self.running = True
        self.shutdown = False
        self.received_artdmx = 0
        self.sent_frames = 0
        self.sent_packets = 0
        # Evita que una actualización del manager vuelva siempre al frame 1.
        # El firmware nuevo admite takeover por emisor; este valor también ayuda
        # durante la transición si aún hay un C5 con firmware anterior.
        self.frame_id = (time.monotonic_ns() >> 10) & 0xFFFF
        if self.frame_id == 0:
            self.frame_id = 1
        self.last_artnet_time = 0.0
        self.last_error = ""
        self.log: list[str] = []
        self.pixel_test_active = False
        self.pixel_test_session = 0
        self.controllers = [
            {"num": n + 1, "ip": f"192.168.1.{cfg.first_controller_ip + n}", "enabled": False,
             "online": False, "sent_frames": 0, "sent_packets": 0, "last_status": "",
             "pixels_per_output": [PIXELS_PER_OUTPUT] * OUTPUTS, "active_outputs_mask": 0,
             "led_gpio": [None] * OUTPUTS,
             "estimated_current_a": 0.0, "current_scale": 1.0, "brightness_mode": "corriente",
             "brightness_limit_protocol": False}
            for n in range(cfg.controller_count)
        ]
        self.patch_rows: list[dict[str, Any]] = []
        self.patch_by_ip: dict[str, list[dict[str, Any]]] = {}

    def add_log(self, text: str) -> None:
        with self.lock:
            self.log.append(time.strftime("%H:%M:%S ") + text)
            self.log = self.log[-80:]

    def snapshot(self) -> dict[str, Any]:
        with self.lock:
            idle = int((time.monotonic() - self.last_artnet_time) * 1000) if self.last_artnet_time else -1
            return {
                **asdict(self.cfg),
                "running": self.running,
                "received_artdmx": self.received_artdmx,
                "sent_frames": self.sent_frames,
                "sent_packets": self.sent_packets,
                "frame_id": self.frame_id,
                "fps": self.cfg.fps,
                "artnet_idle_ms": idle,
                "last_error": self.last_error,
                "log": list(self.log),
                "controllers": [dict(c) for c in self.controllers],
                "patch_loaded": bool(self.patch_rows),
                "patch_rows": len(self.patch_rows),
            }


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


def is_artpoll(packet: bytes) -> bool:
    """Reconoce el ArtPoll usado por el botón Discover de xLights."""
    return (
        len(packet) >= 14
        and packet[:8] == b"Art-Net\x00"
        and (packet[8] | (packet[9] << 8)) == 0x2000
    )


def artpoll_reply(local_address: str, universe: int) -> bytes:
    """Construye un ArtPollReply para que xLights descubra el puente.

    xLights registra los puertos Art-Net consecutivos a partir de respuestas
    separadas. Por eso se emite una respuesta por universo al recibir ArtPoll.
    """
    try:
        ip_bytes = socket.inet_aton(local_address)
    except OSError:
        ip_bytes = socket.inet_aton("127.0.0.1")
    universe = max(0, min(0x7FFF, int(universe)))
    packet = bytearray(239)  # longitud ArtPollReply de Art-Net 4
    packet[:8] = b"Art-Net\x00"
    packet[8:10] = struct.pack("<H", 0x2100)  # OpPollReply
    packet[10:14] = ip_bytes
    packet[14:16] = struct.pack(">H", ARTNET_PORT)
    packet[16:18] = struct.pack(">H", 14)  # versión de protocolo Art-Net
    packet[18] = (universe >> 8) & 0x7F      # NetSwitch
    packet[19] = (universe >> 4) & 0x0F      # SubSwitch
    packet[20:22] = b"\xff\xff"             # OEM desconocido
    packet[23] = 0xD0

    short_name = b"C5 Sender Manager"
    long_name = b"Lightman C5 Sender Manager - Art-Net Bridge"
    report = b"#0001 [OK] xLights Discover ready"
    packet[26:26 + len(short_name)] = short_name
    packet[44:44 + len(long_name)] = long_name
    packet[108:108 + len(report)] = report
    packet[172:174] = struct.pack(">H", 1)
    packet[174] = 0x80  # PortTypes[0]: salida DMX/Art-Net
    packet[182] = 0x80  # GoodOutput[0]
    packet[190] = universe & 0x0F  # SwOut[0]
    packet[200] = 0x01  # Style: controller
    return bytes(packet)


def ordered_rgb(c0: int, c1: int, c2: int, order: str) -> tuple[int, int, int]:
    if order == "RGB": return c0, c1, c2
    if order == "GRB": return c1, c0, c2
    if order == "RBG": return c0, c2, c1
    if order == "GBR": return c2, c0, c1
    if order == "BRG": return c1, c2, c0
    if order == "BGR": return c2, c1, c0
    return c0, c1, c2


def build_frame(universes: list[bytearray], start_universe: int, order: str,
                dmx_data_channels: int, pixels: int = MAX_PIXELS_PER_CONTROLLER) -> bytes:
    if not 0 <= pixels <= MAX_PIXELS_PER_CONTROLLER:
        raise ValueError("El C5 admite como máximo 1200 píxeles activos")
    dmx = bytearray()
    for offset in range(UNIVERSES_PER_CONTROLLER):
        dmx.extend(universes[start_universe + offset][:dmx_data_channels])
    frame = bytearray(MAX_PIXELS_PER_CONTROLLER * 2)
    for pixel in range(pixels):
        base = pixel * CHANNELS_PER_PIXEL
        c0, c1, c2 = dmx[base:base + CHANNELS_PER_PIXEL]
        struct.pack_into(">H", frame, pixel * 2,
                         rgb555_word(*ordered_rgb(c0, c1, c2, order)))
    return bytes(frame)


def build_controller_frame(universes: list[bytearray], controller_index: int, order: str,
                           dmx_data_channels: int, output_counts: list[int], active_mask: int) -> bytes:
    if len(output_counts) != OUTPUTS:
        raise ValueError("El C5 debe tener seis salidas")
    pixels = sum(count for output, count in enumerate(output_counts) if active_mask & (1 << output))
    return build_frame(universes, controller_index * UNIVERSES_PER_CONTROLLER,
                       order, dmx_data_channels, pixels)


def load_patch_map(path: str) -> tuple[list[dict[str, Any]], dict[str, list[dict[str, Any]]]]:
    if not path:
        return [], {}
    patch_path = Path(path)
    if not patch_path.exists() and getattr(sys, "frozen", False):
        # En el ejecutable de un solo archivo PyInstaller deja los recursos en
        # _MEIPASS, no junto al .exe del Escritorio.
        patch_path = Path(getattr(sys, "_MEIPASS", Path(sys.executable).resolve().parent)) / path
    if not patch_path.exists():
        return [], {}
    with patch_path.open("r", encoding="utf-8") as f:
        data = json.load(f)
    rows = list(data.get("patch", []))
    by_ip: dict[str, list[dict[str, Any]]] = {}
    for row in rows:
        by_ip.setdefault(str(row["controller_ip"]), []).append(row)
    return rows, by_ip


def _indent_xml(element: ET.Element, level: int = 0) -> None:
    """Aplica un formato legible sin depender de una versión concreta de Python."""
    padding = "\n" + "  " * level
    if len(element):
        if not element.text or not element.text.strip():
            element.text = padding + "  "
        for child in element:
            _indent_xml(child, level + 1)
        if not child.tail or not child.tail.strip():
            child.tail = padding
    if level and (not element.tail or not element.tail.strip()):
        element.tail = padding


def install_xlights_network(show_folder: str | Path, universes: int = XLIGHTS_DISCOVERY_UNIVERSES,
                            channels_per_universe: int = DEFAULT_DMX_DATA_CHANNELS,
                            start_universe: int = 0) -> Path:
    """Añade o actualiza el puente local en la red de un show xLights.

    Se conservan los demás controladores del show. xLights leerá este archivo
    al abrir (o recargar) el show y emitirá todos los universos al puente local.
    """
    if universes < 1 or channels_per_universe < 1:
        raise ValueError("La red xLights necesita al menos un universo y un canal")
    folder = Path(show_folder)
    if not folder.is_dir():
        raise ValueError("Selecciona la carpeta existente del show xLights")
    destination = folder / XLIGHTS_NETWORK_FILE
    if destination.exists():
        try:
            root = ET.parse(destination).getroot()
        except ET.ParseError as exc:
            raise ValueError(f"No se pudo leer {XLIGHTS_NETWORK_FILE}: {exc}") from exc
        if root.tag != "Networks":
            raise ValueError(f"{XLIGHTS_NETWORK_FILE} no contiene una red xLights válida")
    else:
        root = ET.Element("Networks")

    # xLights 2026 guarda un Controller con un hijo network por universo.
    # Conservamos el Controller existente para mantener los modelos asignados.
    known_names = {XLIGHTS_BRIDGE_NAME, "C5 Sender Manager Localhost", "Lightman Bridge Localhost"}
    for network in list(root.findall("network")):
        if network.get("Name") in known_names:
            root.remove(network)
    matches = [item for item in root.findall("Controller") if item.get("Name") in known_names]
    if len(matches) > 1:
        raise ValueError("Hay varios controladores C5 Sender Manager en el show")
    if matches:
        controller = matches[0]
    else:
        ids = [int(item.get("Id", "0")) for item in root.findall("Controller")]
        controller = ET.SubElement(root, "Controller", {
            "Id": str(max(ids, default=0) + 1), "Name": XLIGHTS_BRIDGE_NAME,
            "Type": "Ethernet", "Protocol": "ArtNet", "AutoLayout": "0",
            "ActiveState": "Active", "AutoUpload": "0", "Monitor": "1",
        })
    controller.set("Name", XLIGHTS_BRIDGE_NAME)
    controller.set("IP", "127.0.0.1")
    controller.set("Protocol", "ArtNet")
    controller.set("AutoSize", "0")
    controller.set("Description", XLIGHTS_BRIDGE_DESCRIPTION)
    for network in list(controller.findall("network")):
        controller.remove(network)
    for universe in range(start_universe, start_universe + universes):
        ET.SubElement(controller, "network", {
            "ComPort": "127.0.0.1", "BaudRate": str(universe),
            "NetworkType": "ArtNet", "MaxChannels": str(channels_per_universe),
            "Enabled": "Yes",
        })
    _indent_xml(root)

    if destination.exists():
        backup = destination.with_name("xlights_networks.before_c5_160.xml")
        if not backup.exists():
            shutil.copy2(destination, backup)
    temporary = destination.with_suffix(".xml.tmp")
    ET.ElementTree(root).write(temporary, encoding="utf-8", xml_declaration=True)
    temporary.replace(destination)
    return destination


def dmx_rgb_for_row(universes: list[bytearray], row: dict[str, Any], order: str) -> tuple[int, int, int]:
    universe = int(row["universe"])
    channel0 = int(row["channel"]) - 1
    if universe < 0 or universe >= len(universes):
        return 0, 0, 0
    dmx = universes[universe]
    c0 = dmx[channel0] if 0 <= channel0 < len(dmx) else 0
    c1 = dmx[channel0 + 1] if 0 <= channel0 + 1 < len(dmx) else 0
    c2 = dmx[channel0 + 2] if 0 <= channel0 + 2 < len(dmx) else 0
    return ordered_rgb(c0, c1, c2, order)


def patch_output_counts(rows: list[dict[str, Any]]) -> list[int]:
    counts = [0] * OUTPUTS
    for row in rows:
        output = int(row["output"])
        pixel = int(row["output_pixel"])
        if 1 <= output <= OUTPUTS and 1 <= pixel <= MAX_PIXELS_PER_OUTPUT:
            counts[output - 1] = max(counts[output - 1], pixel)
    return counts


def build_patch_frame(universes: list[bytearray], rows: list[dict[str, Any]], order: str,
                      output_counts: list[int] | None = None) -> bytes:
    counts = output_counts if output_counts is not None else patch_output_counts(rows)
    offsets = [sum(counts[:output]) for output in range(OUTPUTS)]
    if sum(counts) > MAX_PIXELS_PER_CONTROLLER:
        raise ValueError("El mapa supera 1200 LEDs en un controlador C5")
    words = [0] * MAX_PIXELS_PER_CONTROLLER
    for row in rows:
        output = int(row["output"])
        output_pixel = int(row["output_pixel"])
        if output < 1 or output > OUTPUTS or output_pixel < 1 or output_pixel > counts[output - 1]:
            continue
        local_index = offsets[output - 1] + output_pixel - 1
        words[local_index] = rgb555_word(*dmx_rgb_for_row(universes, row, order))
    frame = bytearray()
    for word in words:
        frame.extend(struct.pack(">H", word))
    return bytes(frame)


def solid_frame(mode: str, step: int = 0, output_counts: list[int] | None = None,
                active_mask: int = (1 << OUTPUTS) - 1) -> bytes:
    colors = {
        "white": (180, 180, 180),
        "red": (180, 0, 0),
        "green": (0, 180, 0),
        "blue": (0, 0, 180),
    }
    configured = output_counts if output_counts is not None else [PIXELS_PER_OUTPUT] * OUTPUTS
    counts = [count if active_mask & (1 << output) else 0
              for output, count in enumerate(configured)]
    if len(counts) != OUTPUTS or sum(counts) > MAX_PIXELS_PER_CONTROLLER:
        raise ValueError("Configuración C5 de píxeles inválida")
    out = bytearray(MAX_PIXELS_PER_CONTROLLER * 2)
    index = 0
    for output, count in enumerate(counts):
        for pos in range(count):
            if mode == "outputs":
                rgb = [(180, 0, 0), (0, 180, 0), (0, 0, 180), (180, 180, 0), (0, 180, 180), (180, 0, 180)][output]
            elif mode == "rainbow":
                hue = ((pos / max(1, count)) + step / 150.0 + output * 0.08) % 1.0
                rgb = hsv_to_rgb(hue, 1.0, 0.45)
            else:
                rgb = colors.get(mode, (0, 0, 0))
            struct.pack_into(">H", out, index * 2, rgb555_word(*rgb))
            index += 1
    return bytes(out)


def pixel_test_frame(output: int, pixel: int, output_counts: list[int],
                     active_mask: int, color: tuple[int, int, int] = (255, 255, 255)) -> bytes:
    """Crea un cuadro negro con un solo LED encendido en la salida indicada.

    ``output`` es cero basado y ``pixel`` es uno basado, tal como se muestra al
    instalador. El firmware recibe siempre 1200 palabras RGB555; las posiciones
    no usadas por un puerto configurado se mantienen negras.
    """
    if len(output_counts) != OUTPUTS:
        raise ValueError("El controlador no informó seis salidas")
    if not 0 <= output < OUTPUTS or not (active_mask & (1 << output)):
        raise ValueError(f"La salida {output + 1} no está activa")
    count = output_counts[output]
    if not isinstance(count, int) or not 1 <= count <= MAX_PIXELS_PER_OUTPUT:
        raise ValueError(f"La salida {output + 1} no tiene una cantidad de píxeles válida")
    if not 1 <= pixel <= count:
        raise ValueError(f"LED {pixel} fuera de rango (1–{count}) en salida {output + 1}")

    # Debe coincidir con leds.c: las salidas desactivadas no ocupan posiciones
    # en el frame lógico aunque conserven su valor en la configuración.
    offset = sum(count for index, count in enumerate(output_counts[:output])
                 if active_mask & (1 << index)) + pixel - 1
    if offset >= MAX_PIXELS_PER_CONTROLLER:
        raise ValueError("La configuración de píxeles excede la capacidad del C5")
    frame = bytearray(MAX_PIXELS_PER_CONTROLLER * 2)
    struct.pack_into(">H", frame, offset * 2, rgb555_word(*color))
    return bytes(frame)


def limit_current(frame: bytes, limit_a: float, channel_ma: float) -> tuple[bytes, float, float]:
    """Escala RGB555 antes de UDP. Estima hasta channel_ma por cada canal a 31/31."""
    if not (0 < limit_a <= 100 and 0 < channel_ma <= 100):
        raise ValueError("Límite de corriente inválido")
    words = [word[0] for word in struct.iter_unpack(">H", frame)]
    channel_sum = sum(((word >> 10) & 31) + ((word >> 5) & 31) + (word & 31) for word in words)
    estimated_a = channel_sum * channel_ma / (31.0 * 1000.0)
    scale = min(1.0, limit_a / estimated_a) if estimated_a else 1.0
    if scale == 1.0:
        return frame, estimated_a, scale
    limited = bytearray(len(frame))
    limited_channel_sum = 0
    for index, word in enumerate(words):
        red = int(((word >> 10) & 31) * scale)
        green = int(((word >> 5) & 31) * scale)
        blue = int((word & 31) * scale)
        limited_channel_sum += red + green + blue
        struct.pack_into(">H", limited, index * 2, (red << 10) | (green << 5) | blue)
    return bytes(limited), limited_channel_sum * channel_ma / (31.0 * 1000.0), scale


def estimate_rgb555_current(frame: bytes, channel_ma: float) -> float:
    words = [word[0] for word in struct.iter_unpack(">H", frame)]
    channel_sum = sum(((word >> 10) & 31) + ((word >> 5) & 31) + (word & 31) for word in words)
    return channel_sum * channel_ma / (31.0 * 1000.0)


def _scaled_5bit(value: int, scale: float, gamma: float, dither: bool,
                 frame_id: int, pixel: int, channel: int) -> int:
    if value <= 0:
        return 0
    normalized = value / 31.0
    target = (normalized ** gamma) * 31.0 * scale
    base = int(target)
    fraction = target - base
    if dither and fraction > 0.0:
        # Patrón temporal determinista de 32 pasos: permite valores entre niveles RGB555
        # sin cambiar el protocolo que espera el firmware.
        threshold = ((frame_id * 13 + pixel * 7 + channel * 17) & 31) / 32.0
        if fraction > threshold:
            base += 1
    return max(0, min(31, base))


def apply_max_brightness(frame: bytes, max_percent: float, gamma: float, dither: bool,
                         frame_id: int, channel_ma: float) -> tuple[bytes, float, float]:
    if not (0.0 <= max_percent <= 100.0 and 0.4 <= gamma <= 4.0):
        raise ValueError("Perfil de brillo inválido")
    scale = max_percent / 100.0
    limited = bytearray(len(frame))
    for pixel, (word,) in enumerate(struct.iter_unpack(">H", frame)):
        red = _scaled_5bit((word >> 10) & 31, scale, gamma, dither, frame_id, pixel, 0)
        green = _scaled_5bit((word >> 5) & 31, scale, gamma, dither, frame_id, pixel, 1)
        blue = _scaled_5bit(word & 31, scale, gamma, dither, frame_id, pixel, 2)
        struct.pack_into(">H", limited, pixel * 2, (red << 10) | (green << 5) | blue)
    output = bytes(limited)
    return output, estimate_rgb555_current(output, channel_ma), scale


def apply_output_profile(frame: bytes, cfg: AppConfig, frame_id: int,
                         cap_supported: bool = False) -> tuple[bytes, float, float, str, int | None]:
    if cfg.brightness_mode == "max_brightness":
        scale = cfg.max_brightness_value / 255.0
        if cap_supported:
            return frame, estimate_rgb555_current(frame, cfg.ws2815_channel_ma) * scale, scale, "brillo C5", cfg.max_brightness_value
        if cfg.max_brightness_value == 0:
            return bytes(len(frame)), 0.0, 0.0, "brillo sender", None
        # Un firmware anterior ignora los bits reservados del encabezado: nunca
        # le enviamos RGB555 sin limitar, porque encendería el traje al 100%.
        output, estimated_a, scale = apply_max_brightness(
            frame,
            cfg.max_brightness_value * 100.0 / 255.0,
            cfg.brightness_gamma,
            False,
            frame_id,
            cfg.ws2815_channel_ma,
        )
        return output, estimated_a, scale, "brillo sender", None
    output, estimated_a, scale = limit_current(frame, cfg.current_limit_a, cfg.ws2815_channel_ma)
    return output, estimated_a, scale, "corriente", None


def hsv_to_rgb(h: float, s: float, v: float) -> tuple[int, int, int]:
    i = int(h * 6.0)
    f = h * 6.0 - i
    p = v * (1.0 - s)
    q = v * (1.0 - f * s)
    t = v * (1.0 - (1.0 - f) * s)
    r, g, b = [(v, t, p), (q, v, p), (p, v, t), (p, q, v), (t, p, v), (v, p, q)][i % 6]
    return int(r * 255), int(g * 255), int(b * 255)


def iter_c5_packets(frame: bytes, frame_id: int, brightness_limit: int | None = None):
    if len(frame) != MAX_PIXELS_PER_CONTROLLER * 2:
        raise ValueError("El protocolo C5 requiere exactamente 1200 LEDs por cuadro")
    if brightness_limit is not None and not 0 <= brightness_limit <= 255:
        raise ValueError("El brillo máximo debe estar entre 0 y 255")
    flags = FLAG_C5_BRIGHTNESS_LIMIT if brightness_limit is not None else 0
    limit_byte = brightness_limit if brightness_limit is not None else 0
    for packet_id in range(2):
        pixel_offset = packet_id * PIXELS_PER_PACKET
        payload = frame[pixel_offset * 2:(pixel_offset + PIXELS_PER_PACKET) * 2]
        yield HEADER.pack(MAGIC, VERSION, FORMAT_RGB555_BE, flags, limit_byte, frame_id & 0xFFFF,
                          packet_id, 2, pixel_offset, PIXELS_PER_PACKET, len(payload),
                          checksum16(payload)) + payload


def send_frame(sock: socket.socket, target: tuple[str, int], state: SharedState, frame: bytes,
               controller_index: int | None = None) -> int:
    with state.lock:
        frame_id = state.frame_id
        state.frame_id = (state.frame_id + 1) & 0xFFFF
        cfg = AppConfig(**asdict(state.cfg))
    cap_supported = False
    if controller_index is not None:
        with state.lock:
            cap_supported = bool(state.controllers[controller_index]["brightness_limit_protocol"])
    frame, estimated_a, scale, mode_label, brightness_limit = apply_output_profile(
        frame, cfg, frame_id, cap_supported)
    count = 0
    for packet in iter_c5_packets(frame, frame_id, brightness_limit):
        sock.sendto(packet, target)
        count += 1
    with state.lock:
        state.sent_frames += 1
        state.sent_packets += count
        if controller_index is not None:
            state.controllers[controller_index]["estimated_current_a"] = round(estimated_a, 3)
            state.controllers[controller_index]["current_scale"] = round(scale, 3)
            state.controllers[controller_index]["brightness_mode"] = mode_label
    return count


def c5_udp_socket(source_ip: str) -> socket.socket:
    sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    if source_ip != "0.0.0.0":
        sock.bind((source_ip, 0))
    return sock


def read_c5_status(ip: str, source_ip: str, timeout: float = 0.45) -> dict[str, Any]:
    source_address = None if source_ip == "0.0.0.0" else (source_ip, 0)
    conn = http.client.HTTPConnection(ip, 80, timeout=timeout, source_address=source_address)
    try:
        conn.request("GET", "/status")
        response = conn.getresponse()
        if response.status != 200:
            raise OSError(f"C5 HTTP {response.status}")
        return json.loads(response.read().decode("utf-8", errors="replace"))
    finally:
        conn.close()


def save_c5_pixels(ip: str, source_ip: str, pixels_per_output: list[int], active_mask: int,
                   timeout: float = 2.0) -> dict[str, Any]:
    """Persiste las longitudes de salida del C5 en NVS mediante /config/pixels."""
    if len(pixels_per_output) != OUTPUTS:
        raise ValueError("Se requieren seis cantidades de píxeles")
    if not 0 <= active_mask < (1 << OUTPUTS):
        raise ValueError("Máscara de salidas inválida")
    if any(not isinstance(value, int) or not 0 <= value <= MAX_PIXELS_PER_OUTPUT for value in pixels_per_output):
        raise ValueError(f"Cada salida debe tener entre 0 y {MAX_PIXELS_PER_OUTPUT} píxeles")
    total = sum(value for index, value in enumerate(pixels_per_output) if active_mask & (1 << index))
    if total > MAX_PIXELS_PER_CONTROLLER:
        raise ValueError(f"Las salidas activas suman {total} LEDs; el C5 admite {MAX_PIXELS_PER_CONTROLLER}")
    query = {f"pix{index + 1}": value for index, value in enumerate(pixels_per_output)}
    query["active_mask"] = active_mask
    source_address = None if source_ip == "0.0.0.0" else (source_ip, 0)
    conn = http.client.HTTPConnection(ip, 80, timeout=timeout, source_address=source_address)
    try:
        conn.request("GET", f"/config/pixels?{urlencode(query)}")
        response = conn.getresponse()
        body = response.read().decode("utf-8", errors="replace")
        if response.status != 200:
            raise OSError(f"C5 HTTP {response.status}: {body}")
        data = json.loads(body)
        if not data.get("ok"):
            raise OSError("El C5 no confirmó la configuración")
        return data
    finally:
        conn.close()


def sender_thread(state: SharedState) -> None:
    cfg = state.cfg
    rows, by_ip = load_patch_map(cfg.patch_map_path)
    with state.lock:
        state.patch_rows = rows
        state.patch_by_ip = by_ip
    total_universes = max(cfg.controller_count * UNIVERSES_PER_CONTROLLER,
                          max((int(row["universe"]) + 1 for row in rows), default=0))
    universes = [bytearray(DMX_CHANNELS) for _ in range(total_universes)]
    artnet_sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
    artnet_sock.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
    artnet_sock.setsockopt(socket.SOL_SOCKET, socket.SO_RCVBUF, 1024 * 1024)
    artnet_sock.bind((cfg.bind, cfg.artnet_port))
    artnet_sock.setblocking(False)
    c5_source_ip = cfg.c5_source_ip
    c5_sock = c5_udp_socket(c5_source_ip)
    state.add_log(
        f"Art-Net {cfg.bind}:{cfg.artnet_port} -> C5 201..{cfg.first_controller_ip + cfg.controller_count - 1}, "
        f"{cfg.fps:g} fps, salida {c5_source_ip}"
    )
    if rows:
        state.add_log(f"Patch cargado: {len(rows)} pixeles, {len(by_ip)} controlador(es)")
    else:
        state.add_log(f"Red fija: {cfg.controller_count} C5, {UNIVERSES_PER_CONTROLLER} universos cada uno, U0..U{total_universes - 1}")
    period = 1.0 / cfg.fps
    next_frame = time.monotonic() + period
    try:
        while not state.shutdown:
            while True:
                try:
                    packet, _addr = artnet_sock.recvfrom(1024)
                except BlockingIOError:
                    break
                if is_artpoll(packet):
                    # xLights construye la lista de salidas leyendo respuestas
                    # consecutivas; anunciamos U0..U159 en el mismo Discover.
                    bridge_ip = "127.0.0.1"
                    for universe in range(XLIGHTS_DISCOVERY_UNIVERSES):
                        artnet_sock.sendto(artpoll_reply(bridge_ip, universe), _addr)
                    state.add_log(f"xLights Discover: U0..U{XLIGHTS_DISCOVERY_UNIVERSES - 1} anunciados en {bridge_ip}")
                    continue
                parsed = parse_artdmx(packet)
                if parsed is None:
                    continue
                universe, payload = parsed
                index = universe - cfg.start_universe
                if 0 <= index < total_universes:
                    universes[index][:len(payload)] = payload[:DMX_CHANNELS]
                    with state.lock:
                        state.received_artdmx += 1
                        state.last_artnet_time = time.monotonic()
            now = time.monotonic()
            if now >= next_frame:
                with state.lock:
                    running = state.running
                    next_source_ip = state.cfg.c5_source_ip
                if next_source_ip != c5_source_ip:
                    replacement = c5_udp_socket(next_source_ip)
                    c5_sock.close()
                    c5_sock = replacement
                    c5_source_ip = next_source_ip
                    state.add_log(f"Interfaz C5: {c5_source_ip}")
                if running:
                    with state.lock:
                        controllers = [dict(c) for c in state.controllers]
                    for index, controller in enumerate(controllers):
                        if not controller.get("enabled") or not controller.get("online"):
                            continue
                        if by_ip:
                            controller_rows = by_ip.get(str(controller["ip"]), [])
                            if not controller_rows:
                                continue
                            frame = build_patch_frame(universes, controller_rows, cfg.order,
                                                      controller["pixels_per_output"])
                        else:
                            frame = build_controller_frame(universes, index, cfg.order,
                                                           cfg.dmx_data_channels,
                                                           controller["pixels_per_output"],
                                                           controller["active_outputs_mask"])
                        sent_packets = send_frame(c5_sock, (controller["ip"], cfg.c5_port), state, frame, index)
                        with state.lock:
                            state.controllers[index]["sent_frames"] += 1
                            state.controllers[index]["sent_packets"] += sent_packets
                next_frame += period
            else:
                time.sleep(min(0.001, next_frame - now))
    except Exception as exc:
        with state.lock:
            state.last_error = str(exc)
        state.add_log(f"ERROR sender: {exc}")
    finally:
        artnet_sock.close()
        c5_sock.close()


def detector_thread(state: SharedState) -> None:
    while not state.shutdown:
        for index, controller in enumerate(list(state.controllers)):
            ip = controller["ip"]
            online = False
            summary = ""
            cap_supported = False
            try:
                data = read_c5_status(ip, state.cfg.c5_source_ip)
                online = bool(data.get("ok"))
                cap_supported = data.get("brightness_limit_protocol") is True
                summary = f"rssi={data.get('wifi_rssi', '?')} fps={data.get('led_fps', '?')} drop={data.get('led_dropped_frames', '?')}"
                device_config = data.get("config", {})
                counts = device_config.get("pixels_per_output", [])
                mask = int(device_config.get("active_outputs_mask", 0))
                pins = device_config.get("led_gpio", [])
            except Exception:
                online = False
                summary = "sin respuesta"
            with state.lock:
                was_online = bool(state.controllers[index]["online"])
                state.controllers[index]["online"] = online
                state.controllers[index]["brightness_limit_protocol"] = cap_supported
                state.controllers[index]["last_status"] = summary
                if online and len(counts) == OUTPUTS and all(isinstance(n, int) and 0 <= n <= MAX_PIXELS_PER_OUTPUT for n in counts):
                    state.controllers[index]["pixels_per_output"] = counts
                    state.controllers[index]["active_outputs_mask"] = mask
                    if len(pins) == OUTPUTS and all(isinstance(pin, int) for pin in pins):
                        state.controllers[index]["led_gpio"] = pins
                if online and not state.controllers[index]["enabled"]:
                    state.controllers[index]["enabled"] = True
                if online != was_online:
                    state.log.append(time.strftime("%H:%M:%S ") + f"#{index + 1} {ip} {'online' if online else 'offline'}")
                    state.log = state.log[-80:]
            if state.shutdown:
                break
        time.sleep(2.0)


def run_tester(state: SharedState, mode: str, fps: float) -> None:
    def worker() -> None:
        sock = c5_udp_socket(state.cfg.c5_source_ip)
        frames = int(5 * fps) if mode == "rainbow" else int(2 * fps)
        state.add_log(f"Tester {mode} {frames} frames")
        for i in range(frames):
            if state.shutdown:
                break
            with state.lock:
                controllers = [dict(c) for c in state.controllers]
            for index, controller in enumerate(controllers):
                if controller.get("enabled") and controller.get("online"):
                    frame = solid_frame(mode, i, controller["pixels_per_output"],
                                        controller["active_outputs_mask"])
                    send_frame(sock, (controller["ip"], state.cfg.c5_port), state, frame, index)
            time.sleep(1.0 / fps)
        sock.close()
    threading.Thread(target=worker, daemon=True).start()


def start_pixel_tester(state: SharedState, controller_index: int, output: int, pixel: int,
                       fps: float) -> None:
    """Mantiene un único LED encendido hasta que se detenga explícitamente el test."""
    def worker() -> None:
        with state.lock:
            if not 0 <= controller_index < len(state.controllers):
                controller = None
            else:
                controller = dict(state.controllers[controller_index])
        if controller is None:
            state.add_log("Tester de LED: controlador no válido")
            return
        if not controller.get("online"):
            state.add_log(f"Tester de LED: {controller['ip']} sin respuesta")
            return
        try:
            frame = pixel_test_frame(output, pixel, controller["pixels_per_output"],
                                     int(controller["active_outputs_mask"]))
        except ValueError as exc:
            state.add_log(f"Tester de LED: {exc}")
            return
        sock = c5_udp_socket(state.cfg.c5_source_ip)
        try:
            state.add_log(f"Tester fijo: {controller['ip']} salida {output + 1}, LED {pixel}")
            while not state.shutdown:
                with state.lock:
                    if not state.pixel_test_active or state.pixel_test_session != session:
                        break
                send_frame(sock, (controller["ip"], state.cfg.c5_port), state, frame, controller_index)
                time.sleep(1.0 / max(1.0, fps))
        finally:
            sock.close()

    with state.lock:
        state.pixel_test_session += 1  # cancela cualquier selección anterior
        state.pixel_test_active = True
        session = state.pixel_test_session
    threading.Thread(target=worker, daemon=True).start()


def stop_pixel_tester(state: SharedState, controller_index: int | None = None) -> None:
    """Detiene el test y deja negro el controlador que se estaba probando."""
    with state.lock:
        state.pixel_test_active = False
        state.pixel_test_session += 1
        controller = (dict(state.controllers[controller_index])
                      if controller_index is not None and 0 <= controller_index < len(state.controllers)
                      else None)
    if controller is None or not controller.get("online"):
        return

    def worker() -> None:
        sock = c5_udp_socket(state.cfg.c5_source_ip)
        try:
            frame = solid_frame("black", 0, controller["pixels_per_output"], controller["active_outputs_mask"])
            for _ in range(2):  # tolera la pérdida de un datagrama UDP
                send_frame(sock, (controller["ip"], state.cfg.c5_port), state, frame, controller_index)
                time.sleep(1.0 / max(1.0, state.cfg.fps))
        finally:
            sock.close()
    threading.Thread(target=worker, daemon=True).start()


def make_handler(state: SharedState):
    class Handler(BaseHTTPRequestHandler):
        def log_message(self, _format: str, *_args: Any) -> None:
            return

        def _send(self, status: int, content_type: str, data: bytes) -> None:
            self.send_response(status)
            self.send_header("Content-Type", content_type)
            self.send_header("Content-Length", str(len(data)))
            self.end_headers()
            self.wfile.write(data)

        def do_GET(self) -> None:
            if self.path == "/" or self.path.startswith("/?"):
                self._send(200, "text/html; charset=utf-8", HTML.replace("__LOGO__", logo_data_uri()).encode("utf-8"))
            elif self.path == "/tablet":
                self._send(200, "text/html; charset=utf-8", TABLET_HTML.replace("__LOGO__", logo_data_uri()).encode("utf-8"))
            elif self.path == "/api/status":
                self._send(200, "application/json", json.dumps(state.snapshot()).encode("utf-8"))
            else:
                self._send(404, "text/plain", b"not found")

        def do_POST(self) -> None:
            length = int(self.headers.get("Content-Length", "0") or "0")
            body = self.rfile.read(length) if length else b"{}"
            try:
                data = json.loads(body.decode("utf-8"))
            except json.JSONDecodeError:
                data = {}
            if self.path == "/api/control":
                action = data.get("action")
                with state.lock:
                    if action == "start":
                        state.running = True
                    elif action in ("pause", "stop", "blackout"):
                        state.running = False
                state.add_log(f"Control: {action}")
                if action == "blackout":
                    run_tester(state, "black", state.cfg.fps)
                self._send(200, "application/json", b'{"ok":true}')
            elif self.path == "/api/tester":
                with state.lock:
                    state.running = False
                run_tester(state, str(data.get("mode", "outputs")), float(data.get("fps", state.cfg.fps)))
                self._send(200, "application/json", b'{"ok":true}')
            elif self.path == "/api/current-limit":
                try:
                    limit_a = float(data.get("limit_a"))
                    save_current_limit(limit_a)
                    with state.lock:
                        state.cfg.current_limit_a = limit_a
                    state.add_log(f"Límite WS2815: {limit_a:g} A por controlador")
                    self._send(200, "application/json", b'{"ok":true}')
                except (ValueError, TypeError, OSError) as exc:
                    self._send(400, "application/json", json.dumps({"error": str(exc)}).encode("utf-8"))
            elif self.path == "/api/brightness-profile":
                try:
                    mode = str(data.get("mode", "current_limit"))
                    limit_a = float(data.get("limit_a", state.cfg.current_limit_a))
                    if "max_value" in data:
                        max_value = int(data["max_value"])
                    elif "max_percent" in data:
                        max_value = round(float(data["max_percent"]) * 255 / 100)
                    else:
                        max_value = state.cfg.max_brightness_value
                    gamma = float(data.get("gamma", state.cfg.brightness_gamma))
                    dither = bool(data.get("dither", True))
                    max_value = save_brightness_profile(mode, max_value, gamma, False, limit_a)
                    with state.lock:
                        state.cfg.brightness_mode = mode
                        state.cfg.current_limit_a = limit_a
                        state.cfg.max_brightness_value = max_value
                        state.cfg.brightness_gamma = gamma
                        state.cfg.temporal_dither = False
                    label = "corriente" if mode == "current_limit" else f"brillo max {max_value}/255"
                    state.add_log(f"Perfil de salida: {label}")
                    self._send(200, "application/json", b'{"ok":true}')
                except (ValueError, TypeError, OSError) as exc:
                    self._send(400, "application/json", json.dumps({"error": str(exc)}).encode("utf-8"))
            else:
                self._send(404, "text/plain", b"not found")
    return Handler


def local_ip() -> str:
    try:
        sock = socket.socket(socket.AF_INET, socket.SOCK_DGRAM)
        sock.connect(("8.8.8.8", 80))
        ip = sock.getsockname()[0]
        sock.close()
        return ip
    except OSError:
        return "127.0.0.1"


def local_ipv4_addresses() -> list[str]:
    ips = ["127.0.0.1", "0.0.0.0"]
    try:
        host = socket.gethostname()
        for info in socket.getaddrinfo(host, None, socket.AF_INET):
            ip = info[4][0]
            if ip not in ips:
                ips.append(ip)
    except OSError:
        pass
    primary = local_ip()
    if primary not in ips:
        ips.append(primary)
    return ips


def detect_c5_source_ip(target_ip: str) -> str:
    for source_ip in local_ipv4_addresses():
        if source_ip in ("0.0.0.0", "127.0.0.1"):
            continue
        try:
            with socket.create_connection((target_ip, 80), timeout=0.4,
                                          source_address=(source_ip, 0)):
                return source_ip
        except OSError:
            continue
    return "0.0.0.0"


def main() -> int:
    parser = argparse.ArgumentParser(description="C5 Sender Manager integrado")
    parser.add_argument("--c5-ip", default="192.168.1.201")
    parser.add_argument("--fps", type=float, default=30.0)
    parser.add_argument("--order", choices=["RGB", "GRB", "RBG", "GBR", "BRG", "BGR"], default="RGB")
    parser.add_argument("--bind", default="127.0.0.1")
    parser.add_argument("--artnet-port", type=int, default=6454)
    parser.add_argument("--web-port", type=int, default=8080)
    parser.add_argument("--no-browser", action="store_true")
    args = parser.parse_args()

    settings = load_output_settings()
    source_ip = settings["c5_source_ip"]
    if source_ip == "auto" or source_ip not in local_ipv4_addresses():
        source_ip = detect_c5_source_ip(args.c5_ip)
    cfg = AppConfig(c5_ip=args.c5_ip, c5_source_ip=source_ip,
                    fps=args.fps, order=args.order, bind=args.bind,
                    artnet_port=args.artnet_port, web_port=args.web_port,
                    current_limit_a=settings["current_limit_a"],
                    brightness_mode=settings["brightness_mode"],
                    max_brightness_value=settings["max_brightness_value"],
                    brightness_gamma=settings["brightness_gamma"],
                    temporal_dither=settings["temporal_dither"])
    state = SharedState(cfg)
    threading.Thread(target=sender_thread, args=(state,), daemon=True).start()
    threading.Thread(target=detector_thread, args=(state,), daemon=True).start()

    server = ThreadingHTTPServer((cfg.web_bind, cfg.web_port), make_handler(state))
    url = f"http://127.0.0.1:{cfg.web_port}"
    lan_url = f"http://{local_ip()}:{cfg.web_port}"
    state.add_log(f"Web local {url}")
    state.add_log(f"Web tablet/celular {lan_url}")
    print("C5 Sender Manager")
    print(f"Local:  {url}")
    print(f"Tablet: {lan_url}")
    print("Ctrl+C para cerrar")
    if not args.no_browser:
        threading.Timer(1.0, lambda: webbrowser.open(url)).start()
    try:
        server.serve_forever()
    except KeyboardInterrupt:
        pass
    finally:
        state.shutdown = True
        server.server_close()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
