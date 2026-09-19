#!/usr/bin/env python3
"""GUI nativa Windows para C5 Sender Manager."""

from __future__ import annotations

import queue
import threading
import time
import tkinter as tk
import webbrowser
from tkinter import filedialog, messagebox, ttk

import c5_sender_manager as core
from http.server import ThreadingHTTPServer


class ManagerApp(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("C5 Sender Manager")
        self.geometry("1400x900")
        self.minsize(1120, 760)
        self.configure(bg="#0b1020")

        output_settings = core.load_output_settings()
        source_ip = output_settings["c5_source_ip"]
        if source_ip == "auto" or source_ip not in core.local_ipv4_addresses():
            source_ip = core.detect_c5_source_ip("192.168.1.201")
        self.cfg = core.AppConfig(
            c5_ip="192.168.1.201",
            c5_source_ip=source_ip,
            bind="0.0.0.0",
            artnet_port=6454,
            web_port=8080,
            fps=30.0,
            order="RGB",
            dmx_data_channels=510,
            patch_map_path="",
            current_limit_a=output_settings["current_limit_a"],
            brightness_mode=output_settings["brightness_mode"],
            max_brightness_value=output_settings["max_brightness_value"],
            brightness_gamma=output_settings["brightness_gamma"],
            temporal_dither=output_settings["temporal_dither"],
        )
        self.state = core.SharedState(self.cfg)
        self.server: ThreadingHTTPServer | None = None
        self.ui_log: queue.Queue[str] = queue.Queue()
        self.logo_img: tk.PhotoImage | None = None
        self.network_var = tk.StringVar(value=self.cfg.c5_source_ip)
        self.current_limit_var = tk.StringVar(value=f"{self.cfg.current_limit_a:g}")
        self.brightness_mode_var = tk.StringVar(value=self.cfg.brightness_mode)
        self.max_brightness_var = tk.StringVar(value=str(round(self.cfg.max_brightness_value / 31)))
        self.gamma_var = tk.StringVar(value=f"{self.cfg.brightness_gamma:g}")
        self.dither_var = tk.BooleanVar(value=False)
        self.brightness_value_var = tk.DoubleVar(value=min(8, round(self.cfg.max_brightness_value / 31)))
        self.test_controller_var = tk.StringVar(value="")
        self.test_controller_summary = tk.StringVar(value="Esperando lectura de controladores…")
        self.test_pixel_vars = [tk.IntVar(value=1) for _ in range(core.OUTPUTS)]
        self.test_pixel_displays = [tk.StringVar(value="—") for _ in range(core.OUTPUTS)]
        self.test_pixel_labels: list[ttk.Label] = []
        self.test_pixel_scales: list[tk.Scale] = []
        self._syncing_pixel_scale = False
        self.pixel_test_running = False
        self.selected_test_output = 0
        self.pixel_test_status = tk.StringVar(value="Selecciona un píxel y pulsa INICIAR TEST")
        self.output_count_vars = [tk.IntVar(value=0) for _ in range(core.OUTPUTS)]
        self.output_enabled_vars = [tk.BooleanVar(value=False) for _ in range(core.OUTPUTS)]
        self.pixel_config_status = tk.StringVar(value="Lee un controlador para cargar su configuración.")
        self._brightness_save_after: str | None = None
        self._syncing_brightness_scale = False

        self._setup_style()
        self._build_ui()
        self._start_backend()
        self.after(300, self._refresh)
        self.protocol("WM_DELETE_WINDOW", self._on_close)

    def _setup_style(self) -> None:
        style = ttk.Style(self)
        style.theme_use("clam")
        style.configure("TFrame", background="#0b1020")
        style.configure("Card.TFrame", background="#121a2e", relief="flat")
        style.configure("TLabel", background="#0b1020", foreground="#e9eefc")
        style.configure("Muted.TLabel", background="#0b1020", foreground="#8ea0c4")
        style.configure("Card.TLabel", background="#121a2e", foreground="#e9eefc")
        style.configure("MutedCard.TLabel", background="#121a2e", foreground="#8ea0c4")
        style.configure("Big.TLabel", background="#121a2e", foreground="#e9eefc", font=("Segoe UI", 24, "bold"))
        style.configure("TButton", padding=10, font=("Segoe UI", 10, "bold"))
        style.configure("Compact.TButton", padding=(8, 5), font=("Segoe UI", 10, "bold"))
        style.configure("Live.TButton", padding=(14, 16), font=("Segoe UI", 13, "bold"))
        style.configure("Tester.TButton", padding=(12, 14), font=("Segoe UI", 12, "bold"))
        style.configure("Good.TButton", background="#087443", foreground="#ffffff")
        style.configure("Danger.TButton", background="#9f1239", foreground="#ffffff")
        style.configure("Secondary.TButton", background="#253653", foreground="#ffffff")
        style.configure("LiveGood.TButton", background="#087443", foreground="#ffffff", padding=(14, 16), font=("Segoe UI", 13, "bold"))
        style.configure("LiveDanger.TButton", background="#9f1239", foreground="#ffffff", padding=(14, 16), font=("Segoe UI", 13, "bold"))
        style.configure("LiveSecondary.TButton", background="#253653", foreground="#ffffff", padding=(14, 16), font=("Segoe UI", 13, "bold"))
        style.configure("TNotebook", background="#0b1020", borderwidth=0)
        style.configure("TNotebook.Tab", padding=(18, 10), font=("Segoe UI", 11, "bold"))
        style.map("Good.TButton", background=[("active", "#0a8f52")])
        style.map("Danger.TButton", background=[("active", "#be1745")])
        style.map("Secondary.TButton", background=[("active", "#31486f")])
        style.map("LiveGood.TButton", background=[("active", "#0a8f52")])
        style.map("LiveDanger.TButton", background=[("active", "#be1745")])
        style.map("LiveSecondary.TButton", background=[("active", "#31486f")])

    def _card(self, parent: tk.Widget, row: int, col: int, colspan: int = 1, rowspan: int = 1) -> ttk.Frame:
        card = ttk.Frame(parent, style="Card.TFrame", padding=16)
        card.grid(row=row, column=col, columnspan=colspan, rowspan=rowspan, sticky="nsew", padx=8, pady=8)
        return card

    def _build_ui(self) -> None:
        root = ttk.Frame(self, padding=18)
        root.pack(fill="both", expand=True)
        root.columnconfigure((0, 1, 2, 3), weight=1)
        root.rowconfigure(2, weight=1)

        logo_uri = core.logo_data_uri()
        if logo_uri.startswith("data:image/png;base64,"):
            self.logo_img = tk.PhotoImage(data=logo_uri.split(",", 1)[1])
            self.logo_img = self.logo_img.subsample(max(1, self.logo_img.width() // 170))
            ttk.Label(root, image=self.logo_img).grid(row=0, column=0, rowspan=2, sticky="w", padx=(0, 16))
        title = ttk.Label(root, text="C5 Sender Manager", font=("Segoe UI", 22, "bold"))
        title.grid(row=0, column=1, columnspan=3, sticky="w")
        subtitle = ttk.Label(root, text="Aplicación Windows · Art-Net → C5 201..220 · xLights Discover · 30 fps · 510 canales útiles", style="Muted.TLabel")
        subtitle.grid(row=1, column=1, columnspan=3, sticky="w", pady=(2, 12))

        tabs = ttk.Notebook(root)
        tabs.grid(row=2, column=0, columnspan=4, sticky="nsew")
        live_tab = ttk.Frame(tabs, style="TFrame", padding=4)
        setup_tab = ttk.Frame(tabs, style="TFrame", padding=4)
        pixel_tab = ttk.Frame(tabs, style="TFrame", padding=12)
        tabs.add(live_tab, text="En vivo")
        tabs.add(setup_tab, text="Configuración")
        tabs.add(pixel_tab, text="Probador de píxeles")
        live_tab.columnconfigure((0, 1, 2, 3), weight=1)
        live_tab.rowconfigure(2, weight=1)
        setup_tab.columnconfigure((0, 1, 2, 3), weight=1)

        c1 = self._card(live_tab, 0, 0)
        ttk.Label(c1, text="Sender", style="Card.TLabel", font=("Segoe UI", 11, "bold")).pack(anchor="w")
        self.running_label = ttk.Label(c1, text="Live activo", style="Big.TLabel")
        self.running_label.pack(anchor="w", pady=(8, 0))

        c2 = self._card(live_tab, 0, 1)
        ttk.Label(c2, text="Art-Net", style="Card.TLabel", font=("Segoe UI", 11, "bold")).pack(anchor="w")
        self.artnet_label = ttk.Label(c2, text="0", style="Big.TLabel")
        self.artnet_label.pack(anchor="w", pady=(8, 0))
        ttk.Label(c2, text="paquetes recibidos", style="MutedCard.TLabel").pack(anchor="w")

        c3 = self._card(live_tab, 0, 2)
        ttk.Label(c3, text="Frames C5", style="Card.TLabel", font=("Segoe UI", 11, "bold")).pack(anchor="w")
        self.frames_label = ttk.Label(c3, text="0", style="Big.TLabel")
        self.frames_label.pack(anchor="w", pady=(8, 0))
        ttk.Label(c3, text="2 paquetes por frame", style="MutedCard.TLabel").pack(anchor="w")

        c4 = self._card(live_tab, 0, 3)
        ttk.Label(c4, text="Web tablet", style="Card.TLabel", font=("Segoe UI", 11, "bold")).pack(anchor="w")
        self.web_label = ttk.Label(c4, text=":8080", style="Big.TLabel")
        self.web_label.pack(anchor="w", pady=(8, 0))
        web_buttons = ttk.Frame(c4, style="Card.TFrame")
        web_buttons.pack(fill="x", pady=(8, 0))
        ttk.Button(web_buttons, text="Abrir web", style="Compact.TButton", command=self._open_web).pack(side="left", fill="x", expand=True, padx=(0, 4))
        ttk.Button(web_buttons, text="Abrir tablet", style="Compact.TButton", command=self._open_tablet).pack(side="left", fill="x", expand=True, padx=(4, 0))

        controls = self._card(live_tab, 1, 0, colspan=2)
        controls.columnconfigure((0, 1, 2, 3), weight=1)
        ttk.Label(controls, text="Control live", style="Card.TLabel", font=("Segoe UI", 13, "bold")).grid(row=0, column=0, columnspan=4, sticky="w", pady=(0, 8))
        ttk.Label(controls, text="BRILLO RGB555 · pasos ×31", style="Card.TLabel", font=("Segoe UI", 11, "bold")).grid(row=1, column=0, sticky="w", padx=4)
        self.brightness_value_label = ttk.Label(
            controls, text=self._brightness_level_label(self.cfg.max_brightness_value), style="Big.TLabel")
        self.brightness_value_label.grid(row=2, column=0, sticky="w", padx=4)
        ttk.Scale(controls, variable=self.brightness_value_var, from_=0, to=8, orient="horizontal",
                  command=self._brightness_slider_changed).grid(row=2, column=1, columnspan=2, sticky="ew", padx=4)
        ttk.Button(controls, text="GUARDAR BRILLO", style="Secondary.TButton", command=self._save_brightness_profile).grid(row=2, column=3, sticky="ew", padx=4)
        ttk.Button(controls, text="START LIVE", style="LiveGood.TButton", command=self._start_live).grid(row=3, column=0, sticky="ew", padx=4, pady=(10, 4))
        ttk.Button(controls, text="PAUSE", style="LiveSecondary.TButton", command=self._pause).grid(row=3, column=1, sticky="ew", padx=4, pady=(10, 4))
        ttk.Button(controls, text="BLACKOUT", style="LiveDanger.TButton", command=self._blackout).grid(row=3, column=2, sticky="ew", padx=4, pady=(10, 4))
        ttk.Button(controls, text="STOP", style="LiveSecondary.TButton", command=self._pause).grid(row=3, column=3, sticky="ew", padx=4, pady=(10, 4))
        setup = self._card(setup_tab, 0, 0, colspan=4)
        setup.columnconfigure((0, 1, 2, 3), weight=1)
        ttk.Label(setup, text="Brillo y límite de corriente", style="Card.TLabel", font=("Segoe UI", 13, "bold")).grid(row=0, column=0, columnspan=4, sticky="w", pady=(0, 12))
        ttk.Radiobutton(setup, text="Limitador corriente", variable=self.brightness_mode_var, value="current_limit").grid(row=1, column=0, sticky="w", padx=4, pady=(8, 2))
        ttk.Radiobutton(setup, text="Brillo máximo", variable=self.brightness_mode_var, value="max_brightness").grid(row=1, column=1, sticky="w", padx=4, pady=(8, 2))
        ttk.Checkbutton(setup, text="Dither desactivado: brillo estable", variable=self.dither_var, state="disabled").grid(row=1, column=2, sticky="w", padx=4, pady=(8, 2))
        ttk.Label(setup, text="Amperios", style="MutedCard.TLabel").grid(row=2, column=0, sticky="w", padx=4)
        ttk.Label(setup, text="Nivel RGB555 (0 apagado; 1–8)", style="MutedCard.TLabel").grid(row=2, column=1, sticky="w", padx=4)
        ttk.Label(setup, text="Gamma (C5 antiguo)", style="MutedCard.TLabel").grid(row=2, column=2, sticky="w", padx=4)
        ttk.Entry(setup, textvariable=self.current_limit_var).grid(row=3, column=0, sticky="ew", padx=4, pady=4)
        ttk.Entry(setup, textvariable=self.max_brightness_var).grid(row=3, column=1, sticky="ew", padx=4, pady=4)
        ttk.Entry(setup, textvariable=self.gamma_var).grid(row=3, column=2, sticky="ew", padx=4, pady=4)
        ttk.Button(setup, text="GUARDAR PERFIL", style="Secondary.TButton", command=self._save_brightness_profile).grid(row=3, column=3, sticky="ew", padx=4, pady=4)

        ttk.Label(setup, text="Red y xLights", style="Card.TLabel", font=("Segoe UI", 13, "bold")).grid(row=4, column=0, columnspan=4, sticky="w", pady=(24, 8))
        ttk.Label(setup, text="xLights Discover mostrará “C5 Sender Manager”. También acepta Art-Net en 127.0.0.1:6454.", style="MutedCard.TLabel").grid(row=5, column=0, columnspan=4, sticky="w")
        ttk.Label(setup, text="Interfaz de salida hacia los C5", style="MutedCard.TLabel").grid(row=6, column=0, columnspan=2, sticky="w", pady=(14, 0))
        self.network_combo = ttk.Combobox(
            setup,
            textvariable=self.network_var,
            state="readonly",
            values=core.local_ipv4_addresses(),
        )
        self.network_combo.grid(row=7, column=0, columnspan=2, sticky="ew", padx=4, pady=4)
        self.network_combo.bind("<<ComboboxSelected>>", self._set_c5_source)
        ttk.Label(
            setup,
            text="Auto detecta Ethernet/Wi-Fi al abrir. 0.0.0.0 usa la ruta de Windows.",
            style="MutedCard.TLabel",
        ).grid(row=7, column=2, columnspan=2, sticky="w", padx=8)
        ttk.Button(
            setup,
            text="Instalar red xLights en un show...",
            style="Secondary.TButton",
            command=self._install_xlights_network,
        ).grid(row=8, column=0, columnspan=2, sticky="ew", padx=4, pady=(12, 4))
        ttk.Label(
            setup,
            text="Crea/actualiza xlights_networks.xml: Art-Net localhost, U0..U159, 510 canales, Auto Size apagado.",
            style="MutedCard.TLabel",
        ).grid(row=8, column=2, columnspan=2, sticky="w", padx=8)

        self._build_pixel_tester(pixel_tab)

        tester = self._card(live_tab, 1, 2, colspan=2)
        tester.columnconfigure((0, 1, 2), weight=1)
        ttk.Label(tester, text="Tester rápido", style="Card.TLabel", font=("Segoe UI", 13, "bold")).grid(row=0, column=0, columnspan=3, sticky="w", pady=(0, 12))
        self.test_mode = tk.StringVar(value="outputs")
        mode = ttk.Combobox(tester, textvariable=self.test_mode, state="readonly", values=["outputs", "white", "red", "green", "blue", "rainbow"])
        mode.grid(row=1, column=0, columnspan=2, sticky="ew", padx=4, pady=4)
        ttk.Button(tester, text="ENVIAR TESTER", style="Tester.TButton", command=self._send_tester).grid(row=1, column=2, sticky="ew", padx=4, pady=4)
        ttk.Label(tester, text="outputs = cada salida en un color. rainbow = arcoiris 5 segundos.", style="MutedCard.TLabel").grid(row=2, column=0, columnspan=3, sticky="w", pady=(10, 0))

        monitor = self._card(live_tab, 2, 0, colspan=2)
        monitor.columnconfigure(0, weight=1)
        ttk.Label(monitor, text="Log", style="Card.TLabel", font=("Segoe UI", 13, "bold")).grid(row=0, column=0, sticky="w")
        self.log_text = tk.Text(monitor, height=8, bg="#0c1324", fg="#e9eefc", insertbackground="#e9eefc", relief="flat", font=("Consolas", 10))
        self.log_text.grid(row=1, column=0, sticky="nsew", pady=(10, 0))
        monitor.rowconfigure(1, weight=1)

        controllers = self._card(live_tab, 2, 2, colspan=2)
        controllers.columnconfigure(0, weight=1)
        ttk.Label(controllers, text="Controladores 201..220", style="Card.TLabel", font=("Segoe UI", 13, "bold")).grid(row=0, column=0, sticky="w")
        self.controllers_text = tk.Text(controllers, height=8, bg="#0c1324", fg="#e9eefc", insertbackground="#e9eefc", relief="flat", font=("Consolas", 10))
        self.controllers_text.grid(row=1, column=0, sticky="nsew", pady=(10, 0))
        controllers.rowconfigure(1, weight=1)

    def _build_pixel_tester(self, parent: ttk.Frame) -> None:
        """Panel de instalación: un deslizador por cada pin/salida del C5."""
        parent.columnconfigure(0, weight=1, uniform="pixel-panels")
        parent.columnconfigure(1, weight=1, uniform="pixel-panels")
        card = ttk.Frame(parent, style="Card.TFrame", padding=20)
        card.grid(row=0, column=0, sticky="nsew", padx=(0, 7))
        card.columnconfigure(1, weight=1, minsize=260)
        ttk.Label(card, text="Probador por salida", style="Card.TLabel", font=("Segoe UI", 16, "bold")).grid(
            row=0, column=0, columnspan=4, sticky="w")
        ttk.Label(
            card,
            text="Elige el C5 y sus salidas. El slider solo selecciona el LED; INICIAR TEST lo mantiene encendido hasta pulsar APAGAR.",
            style="MutedCard.TLabel", wraplength=920,
        ).grid(row=1, column=0, columnspan=4, sticky="w", pady=(5, 16))
        ttk.Label(card, text="Controlador", style="Card.TLabel", font=("Segoe UI", 10, "bold")).grid(row=2, column=0, sticky="w")
        self.test_controller_combo = ttk.Combobox(card, textvariable=self.test_controller_var, state="readonly")
        self.test_controller_combo.grid(row=2, column=1, columnspan=2, sticky="ew", padx=10)
        self.test_controller_combo.bind("<<ComboboxSelected>>", self._update_pixel_tester)
        ttk.Button(card, text="LEER AHORA", style="Secondary.TButton", command=self._read_selected_controller).grid(
            row=2, column=3, sticky="e")
        ttk.Label(card, textvariable=self.test_controller_summary, style="MutedCard.TLabel").grid(
            row=3, column=0, columnspan=4, sticky="w", pady=(7, 14))

        for output in range(core.OUTPUTS):
            row = output + 4
            label = ttk.Label(card, text=f"Salida {output + 1}: esperando lectura", style="Card.TLabel")
            label.grid(row=row, column=0, sticky="w", pady=7)
            scale = tk.Scale(
                card, from_=1, to=1, orient="horizontal", resolution=1, showvalue=False,
                length=500, highlightthickness=0, bd=0, bg="#121a2e", troughcolor="#c9c8c1",
                activebackground="#dce6ff", command=lambda value, port=output: self._test_pixel(port, value),
            )
            scale.grid(row=row, column=1, sticky="w", padx=10, pady=7)
            adjust = ttk.Frame(card, style="Card.TFrame")
            adjust.grid(row=row, column=2, sticky="w")
            ttk.Button(adjust, text="−", style="Compact.TButton", width=2,
                       command=lambda port=output: self._adjust_test_pixel(port, -1)).pack(side="left", padx=(0, 3))
            ttk.Button(adjust, text="+", style="Compact.TButton", width=2,
                       command=lambda port=output: self._adjust_test_pixel(port, 1)).pack(side="left")
            ttk.Label(card, textvariable=self.test_pixel_displays[output], style="Card.TLabel", width=28).grid(
                row=row, column=3, sticky="w", padx=(8, 0))
            self.test_pixel_labels.append(label)
            self.test_pixel_scales.append(scale)
        ttk.Label(card, textvariable=self.pixel_test_status, style="MutedCard.TLabel").grid(
            row=10, column=0, columnspan=3, sticky="w", pady=(18, 0))
        buttons = ttk.Frame(card, style="Card.TFrame")
        buttons.grid(row=10, column=3, sticky="e", pady=(18, 0))
        ttk.Button(buttons, text="INICIAR TEST", style="Good.TButton", command=self._start_pixel_test).pack(side="left", padx=(0, 5))
        ttk.Button(buttons, text="APAGAR", style="Danger.TButton", command=self._stop_pixel_test).pack(side="left")
        self._build_pixel_config(parent)

    def _build_pixel_config(self, parent: ttk.Frame) -> None:
        config = ttk.Frame(parent, style="Card.TFrame", padding=20)
        config.grid(row=0, column=1, sticky="nsew", padx=(7, 0))
        config.columnconfigure(3, weight=1)
        ttk.Label(config, text="Configuración del controlador", style="Card.TLabel", font=("Segoe UI", 14, "bold")).grid(
            row=0, column=0, columnspan=4, sticky="w")
        ttk.Label(
            config, text="Define cuántos LEDs tiene cada salida. GUARDAR EN C5 lo escribe en NVS: se conserva incluso después de reiniciar.",
            style="MutedCard.TLabel", wraplength=900,
        ).grid(row=1, column=0, columnspan=4, sticky="w", pady=(5, 14))
        for output in range(core.OUTPUTS):
            row = output + 2
            ttk.Label(config, text=f"Salida {output + 1}", style="Card.TLabel").grid(row=row, column=0, sticky="w", pady=4)
            ttk.Checkbutton(config, text="Activa", variable=self.output_enabled_vars[output]).grid(row=row, column=1, sticky="w", padx=(18, 8))
            ttk.Spinbox(config, from_=0, to=core.MAX_PIXELS_PER_OUTPUT, increment=1,
                        textvariable=self.output_count_vars[output], width=8).grid(row=row, column=2, sticky="w", padx=8)
            ttk.Label(config, text="píxeles", style="MutedCard.TLabel").grid(row=row, column=3, sticky="w")
        ttk.Label(config, textvariable=self.pixel_config_status, style="MutedCard.TLabel").grid(
            row=8, column=0, columnspan=3, sticky="w", pady=(16, 0))
        ttk.Button(config, text="GUARDAR EN C5", style="Good.TButton", command=self._save_pixel_config).grid(
            row=8, column=3, sticky="e", pady=(16, 0))

    def _selected_controller_index(self) -> int | None:
        # El valor contiene el número estable para que cambiar una IP no rompa la selección.
        try:
            return int(self.test_controller_var.get().split(" ", 1)[0].lstrip("#")) - 1
        except (ValueError, IndexError):
            return None

    def _read_selected_controller(self) -> None:
        index = self._selected_controller_index()
        if index is None:
            messagebox.showinfo("Probador de píxeles", "Selecciona primero un controlador.", parent=self)
            return
        with self.state.lock:
            controller = dict(self.state.controllers[index])
        try:
            data = core.read_c5_status(controller["ip"], self.cfg.c5_source_ip)
            config = data.get("config", {})
            counts = config.get("pixels_per_output", [])
            mask = int(config.get("active_outputs_mask", 0))
            pins = config.get("led_gpio", [])
            if len(counts) != core.OUTPUTS or not all(isinstance(n, int) and 0 <= n <= core.MAX_PIXELS_PER_OUTPUT for n in counts):
                raise ValueError("El C5 respondió pero su configuración de salidas no es válida")
            with self.state.lock:
                target = self.state.controllers[index]
                target["online"] = True
                target["enabled"] = True
                target["pixels_per_output"] = counts
                target["active_outputs_mask"] = mask
                if len(pins) == core.OUTPUTS and all(isinstance(pin, int) for pin in pins):
                    target["led_gpio"] = pins
            self.state.add_log(f"Configuración leída de {controller['ip']}")
            self._update_pixel_tester()
        except (OSError, ValueError, TimeoutError) as exc:
            messagebox.showerror("Lectura del C5", f"No se pudo leer {controller['ip']}: {exc}", parent=self)

    def _update_pixel_tester(self, _event: tk.Event | None = None) -> None:
        index = self._selected_controller_index()
        if index is None:
            return
        with self.state.lock:
            controller = dict(self.state.controllers[index])
        counts = controller["pixels_per_output"]
        mask = int(controller["active_outputs_mask"])
        pins = controller.get("led_gpio", [None] * core.OUTPUTS)
        self.test_controller_summary.set(
            f"{controller['ip']} · {'online' if controller['online'] else 'sin respuesta'} · "
            f"máscara de salidas: 0x{mask:02X}"
        )
        for output, count in enumerate(counts):
            self.output_count_vars[output].set(count)
            self.output_enabled_vars[output].set(bool(mask & (1 << output)))
        for output, (count, pin, label, scale, variable) in enumerate(zip(counts, pins, self.test_pixel_labels, self.test_pixel_scales, self.test_pixel_vars)):
            pin_label = f"GPIO {pin}" if isinstance(pin, int) else "GPIO no informado"
            active = bool(mask & (1 << output)) and count > 0
            if active:
                value = max(1, min(count, variable.get()))
                variable.set(value)
                label.configure(text=f"Salida {output + 1} · {pin_label} · {count} LEDs")
                self._syncing_pixel_scale = True
                try:
                    scale.configure(from_=1, to=count, state="normal")
                    scale.set(value)
                finally:
                    self._syncing_pixel_scale = False
                self.test_pixel_displays[output].set(self._pixel_channel_label(index, counts, mask, output, value))
            else:
                variable.set(0)
                label.configure(text=f"Salida {output + 1} · {pin_label} · desactivada")
                self._syncing_pixel_scale = True
                try:
                    scale.configure(from_=0, to=1, state="disabled")
                    scale.set(0)
                finally:
                    self._syncing_pixel_scale = False
                self.test_pixel_displays[output].set("—")

    def _save_pixel_config(self) -> None:
        index = self._selected_controller_index()
        if index is None:
            messagebox.showinfo("Configuración del C5", "Selecciona primero un controlador.", parent=self)
            return
        try:
            counts = [int(variable.get()) for variable in self.output_count_vars]
            mask = sum((1 << output) for output, enabled in enumerate(self.output_enabled_vars) if enabled.get())
            with self.state.lock:
                controller = dict(self.state.controllers[index])
            result = core.save_c5_pixels(controller["ip"], self.cfg.c5_source_ip, counts, mask)
            saved_counts = result["pixels_per_output"]
            saved_mask = int(result["active_outputs_mask"])
            with self.state.lock:
                target = self.state.controllers[index]
                target["pixels_per_output"] = saved_counts
                target["active_outputs_mask"] = saved_mask
                target["online"] = True
                target["enabled"] = True
            total = sum(count for output, count in enumerate(saved_counts) if saved_mask & (1 << output))
            self.pixel_config_status.set(f"Guardado en C5: {total} LEDs activos. Persistirá tras reiniciar.")
            self.state.add_log(f"C5 {controller['ip']}: píxeles guardados ({total} activos)")
            self._update_pixel_tester()
        except (ValueError, OSError, KeyError, TypeError) as exc:
            self.pixel_config_status.set(f"No guardado: {exc}")
            messagebox.showerror("Configuración del C5", str(exc), parent=self)

    def _adjust_test_pixel(self, output: int, delta: int) -> None:
        """Mueve la selección exactamente un LED, igual que las flechas del slider."""
        index = self._selected_controller_index()
        if index is None:
            return
        with self.state.lock:
            controller = dict(self.state.controllers[index])
        count = controller["pixels_per_output"][output]
        active = bool(int(controller["active_outputs_mask"]) & (1 << output))
        if not active or count < 1:
            return
        pixel = max(1, min(count, self.test_pixel_vars[output].get() + delta))
        self._syncing_pixel_scale = True
        try:
            self.test_pixel_scales[output].set(pixel)
        finally:
            self._syncing_pixel_scale = False
        self._test_pixel(output, str(pixel))

    def _pixel_channel_label(self, controller_index: int, counts: list[int], mask: int, output: int, pixel: int) -> str:
        """Canales RGB uno basados dentro de bloques C5 fijos de 1.200 píxeles."""
        pixels_in_previous_controllers = controller_index * core.MAX_PIXELS_PER_CONTROLLER
        pixels_before = sum(count for index, count in enumerate(counts[:output]) if mask & (1 << index))
        first_channel = (pixels_in_previous_controllers + pixels_before + pixel - 1) * core.CHANNELS_PER_PIXEL + 1
        return f"Píxel {pixel}  (canales {first_channel}–{first_channel + 2})"

    def _test_pixel(self, output: int, raw_value: str) -> None:
        if self._syncing_pixel_scale:
            return
        index = self._selected_controller_index()
        if index is None:
            return
        pixel = round(float(raw_value))
        if pixel < 1:
            return
        with self.state.lock:
            controller = dict(self.state.controllers[index])
        count = controller["pixels_per_output"][output]
        pixel = max(1, min(count, pixel))
        self.test_pixel_vars[output].set(pixel)
        self.test_pixel_displays[output].set(
            self._pixel_channel_label(index, controller["pixels_per_output"], int(controller["active_outputs_mask"]), output, pixel)
        )
        self.selected_test_output = output
        self.pixel_test_status.set("LED seleccionado; pulsa INICIAR TEST" if not self.pixel_test_running else "Test activo; LED actualizado")
        if self.pixel_test_running:
            self._start_pixel_test()

    def _start_pixel_test(self) -> None:
        index = self._selected_controller_index()
        if index is None:
            messagebox.showinfo("Probador de píxeles", "Selecciona primero un controlador.", parent=self)
            return
        output = self.selected_test_output
        pixel = self.test_pixel_vars[output].get()
        if pixel < 1:
            messagebox.showinfo("Probador de píxeles", "Selecciona un LED de una salida activa.", parent=self)
            return
        # El test toma control de Live; el worker único cancela de inmediato el LED anterior.
        with self.state.lock:
            self.state.running = False
        core.start_pixel_tester(self.state, index, output, pixel, self.cfg.fps)
        self.pixel_test_running = True
        self.pixel_test_status.set(f"TEST ACTIVO · salida {output + 1}, píxel {pixel} fijo hasta APAGAR")

    def _stop_pixel_test(self) -> None:
        index = self._selected_controller_index()
        core.stop_pixel_tester(self.state, index)
        self.pixel_test_running = False
        self.pixel_test_status.set("Apagado. Puedes seleccionar otro LED y pulsar INICIAR TEST.")

    def _start_backend(self) -> None:
        threading.Thread(target=core.sender_thread, args=(self.state,), daemon=True).start()
        threading.Thread(target=core.detector_thread, args=(self.state,), daemon=True).start()
        self.server = ThreadingHTTPServer((self.cfg.web_bind, self.cfg.web_port), core.make_handler(self.state))
        threading.Thread(target=self.server.serve_forever, daemon=True).start()
        self.state.add_log("GUI iniciada")
        self.state.add_log("Web local http://127.0.0.1:8080")
        self.state.add_log(f"Web tablet/celular http://{core.local_ip()}:8080")

    def _start_live(self) -> None:
        core.stop_pixel_tester(self.state, self._selected_controller_index())
        self.pixel_test_running = False
        with self.state.lock:
            self.state.running = True
        self.state.add_log("START LIVE")

    def _pause(self) -> None:
        with self.state.lock:
            self.state.running = False
        self.state.add_log("PAUSE/STOP")

    def _blackout(self) -> None:
        core.stop_pixel_tester(self.state, self._selected_controller_index())
        self.pixel_test_running = False
        with self.state.lock:
            self.state.running = False
        core.run_tester(self.state, "black", self.cfg.fps)
        self.state.add_log("BLACKOUT")

    def _send_tester(self) -> None:
        core.stop_pixel_tester(self.state, self._selected_controller_index())
        self.pixel_test_running = False
        with self.state.lock:
            self.state.running = False
        core.run_tester(self.state, self.test_mode.get(), self.cfg.fps)

    def _save_current_limit(self) -> None:
        try:
            value = float(self.current_limit_var.get().replace(",", "."))
            core.save_current_limit(value)
        except (ValueError, OSError) as exc:
            messagebox.showerror("Límite de corriente", str(exc), parent=self)
            return
        with self.state.lock:
            self.cfg.current_limit_a = value
        self.state.add_log(f"Límite WS2815: {value:g} A por controlador")

    def _brightness_slider_changed(self, value: str) -> None:
        if self._syncing_brightness_scale:
            return
        level = max(0, min(8, round(float(value))))
        max_value = level * 31
        self.max_brightness_var.set(str(level))
        self.brightness_value_label.configure(text=self._brightness_level_label(max_value))
        self.brightness_mode_var.set("max_brightness")
        if self._brightness_save_after is not None:
            self.after_cancel(self._brightness_save_after)
        self._brightness_save_after = self.after(250, self._save_brightness_profile_silent)

    def _save_brightness_profile_silent(self) -> None:
        self._brightness_save_after = None
        self._save_brightness_profile(show_errors=False, log_prefix="Brillo")

    def _save_brightness_profile(self, show_errors: bool = True, log_prefix: str = "Perfil guardado") -> None:
        try:
            limit_a = float(self.current_limit_var.get().replace(",", "."))
            level = int(self.max_brightness_var.get())
            if not 0 <= level <= 8:
                raise ValueError("El nivel RGB555 debe estar entre 0 y 8")
            max_value = level * 31
            gamma = float(self.gamma_var.get().replace(",", "."))
            mode = self.brightness_mode_var.get()
            dither = False
            max_value = core.save_brightness_profile(mode, max_value, gamma, dither, limit_a)
        except (ValueError, OSError) as exc:
            if show_errors:
                messagebox.showerror("Perfil de salida", str(exc), parent=self)
            return
        with self.state.lock:
            self.cfg.brightness_mode = mode
            self.cfg.current_limit_a = limit_a
            self.cfg.max_brightness_value = max_value
            self.cfg.brightness_gamma = gamma
            self.cfg.temporal_dither = dither
        level = max_value // 31
        label = "limitador de corriente" if mode == "current_limit" else f"nivel RGB555 {level} ({max_value}/248)"
        self._syncing_brightness_scale = True
        try:
            self.brightness_value_var.set(min(8, round(max_value / 31)))
        finally:
            self._syncing_brightness_scale = False
        self.max_brightness_var.set(str(level))
        self.brightness_value_label.configure(text=self._brightness_level_label(max_value))
        self.state.add_log(f"{log_prefix}: {label}, gamma {gamma:g}, dither {'sí' if dither else 'no'}")

    @staticmethod
    def _brightness_level_label(max_value: int) -> str:
        level = max(0, min(8, round(max_value / 31)))
        return "Nivel 0 · apagado" if level == 0 else f"Nivel {level} · {level * 31}/248"

    def _set_c5_source(self, _event: tk.Event) -> None:
        source_ip = self.network_var.get()
        try:
            core.save_c5_source_ip(source_ip)
        except (ValueError, OSError) as exc:
            messagebox.showerror("Interfaz C5", str(exc), parent=self)
            return
        with self.state.lock:
            self.cfg.c5_source_ip = source_ip
        self.state.add_log(f"Interfaz C5 seleccionada: {source_ip}")

    def _open_web(self) -> None:
        webbrowser.open("http://127.0.0.1:8080")

    def _open_tablet(self) -> None:
        webbrowser.open("http://127.0.0.1:8080/tablet")

    def _install_xlights_network(self) -> None:
        folder = filedialog.askdirectory(title="Selecciona la carpeta del show xLights")
        if not folder:
            return
        try:
            destination = core.install_xlights_network(folder)
        except ValueError as exc:
            self.state.add_log(f"xLights: {exc}")
            messagebox.showerror("No se pudo preparar xLights", str(exc), parent=self)
            return
        self.state.add_log(f"Red xLights lista: {destination}")
        messagebox.showinfo(
            "xLights listo",
            "Se agregó C5 Sender Manager como controlador Art-Net local.\n\n"
            "Abre o recarga ese show en xLights: no debes crear ni configurar otro controlador.",
            parent=self,
        )

    def _refresh(self) -> None:
        snap = self.state.snapshot()
        choices = [f"#{c['num']:02d}  {c['ip']}" for c in snap["controllers"]]
        self.test_controller_combo.configure(values=choices)
        if not self.test_controller_var.get() and choices:
            # Se puede seleccionar cualquier C5 del rango; arrancamos por el primero detectado.
            first_online = next((i for i, c in enumerate(snap["controllers"]) if c["online"]), 0)
            self.test_controller_var.set(choices[first_online])
            self._update_pixel_tester()
        self.running_label.configure(text="Live activo" if snap["running"] else "Pausado")
        self.artnet_label.configure(text=str(snap["received_artdmx"]))
        self.frames_label.configure(text=str(snap["sent_frames"]))
        self.web_label.configure(text="patch" if snap.get("patch_loaded") else ":8080")
        self.log_text.configure(state="normal")
        self.log_text.delete("1.0", "end")
        self.log_text.insert("end", "\n".join(snap["log"]))
        self.log_text.see("end")
        self.log_text.configure(state="disabled")
        lines = []
        for c in snap["controllers"]:
            state = "ONLINE " if c["online"] else "offline"
            enabled = "ON " if c["enabled"] else "off"
            lines.append(
                f"#{c['num']:02d} {c['ip']:<15} {state:<7} {enabled:<3} "
                f"frames={c['sent_frames']:<6} {c['estimated_current_a']:.2f} A "
                f"{c.get('brightness_mode', 'corriente')}="
                f"{str(snap['max_brightness_value']) + '/255' if c.get('brightness_mode') == 'brillo C5' else format(c['current_scale'], '.0%')} "
                f"{c['last_status']}"
            )
        self.controllers_text.configure(state="normal")
        self.controllers_text.delete("1.0", "end")
        self.controllers_text.insert("end", "\n".join(lines))
        self.controllers_text.configure(state="disabled")
        self.after(500, self._refresh)

    def _on_close(self) -> None:
        self.state.shutdown = True
        if self.server:
            self.server.shutdown()
            self.server.server_close()
        self.destroy()


def main() -> int:
    app = ManagerApp()
    app.mainloop()
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
