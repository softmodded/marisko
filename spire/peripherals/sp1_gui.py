#!/usr/bin/env python3
"""
spire — SP-1 Emulator GUI
Virtual device with clickable buttons, live LEDs, and audio visualization.
Connects to Renode via its monitor socket.
"""

import socket
import threading
import tkinter as tk
from collections import deque

RENODE_HOST = "127.0.0.1"
RENODE_PORT = 3334


class RenodeClient:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.lock = threading.Lock()

    def connect(self, host=RENODE_HOST, port=RENODE_PORT):
        self.sock.settimeout(3)
        self.sock.connect((host, port))
        self._recv()

    def cmd(self, command):
        with self.lock:
            self.sock.sendall((command + "\n").encode())
            return self._recv()

    def _recv(self):
        data = b""
        while True:
            try:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                data += chunk
                if b"\n" in chunk or b"(machine-0)" in chunk:
                    break
            except socket.timeout:
                break
        return data.decode(errors="replace").strip()


class SP1GUI:
    BG = "#1a1a2e"
    FG = "#ccc"
    RED = "#e94560"
    DIM = "#555"

    def __init__(self):
        self.root = tk.Tk()
        self.root.title("spire — SP-1 Emulator")
        self.root.geometry("460x500")
        self.root.configure(bg=self.BG)
        self.root.resizable(False, False)

        self.renode = None
        self.audio_buffer = deque(maxlen=44100)
        self.led_widgets = {}
        self._setup_ui()
        self._connect_renode()

    def _setup_ui(self):
        tk.Label(self.root, text="spire", font=("Helvetica", 18, "bold"),
                 fg=self.RED, bg=self.BG).pack(pady=(12, 2))
        tk.Label(self.root, text="sp-1 stem player emulator",
                 font=("Helvetica", 8), fg="#888", bg=self.BG).pack(pady=(0, 14))

        # --- TRACK LEDS (top of device) ---
        self._make_section("track leds")
        tframe = tk.Frame(self.root, bg=self.BG)
        tframe.pack(pady=(0, 16))
        for name in ["t1", "t2", "t3", "t4"]:
            c = tk.Canvas(tframe, width=28, height=28, bg=self.BG, highlightthickness=0)
            c.pack(side=tk.LEFT, padx=14)
            self.led_widgets[name] = c.create_oval(4, 4, 24, 24, fill="#333", outline=self.DIM)
            tk.Label(tframe, text=name, font=("Helvetica", 7),
                     fg="#666", bg=self.BG).pack(side=tk.LEFT, padx=(0, 8))

        # --- TRACK BUTTONS (four buttons in a row) ---
        self._make_section("track buttons (ladder on ain0/p0.02)")
        tbframe = tk.Frame(self.root, bg=self.BG)
        tbframe.pack(pady=(0, 12))
        self.track_btns = {}
        for name in ["trk1", "trk2", "trk3", "trk4"]:
            btn = tk.Button(tbframe, text=name, font=("Helvetica", 9, "bold"),
                            width=6, height=2, bg="#16213e", fg=self.FG,
                            activebackground=self.RED, activeforeground="#fff",
                            relief=tk.FLAT, bd=1,
                            command=lambda n=name: self._press_track(n))
            btn.pack(side=tk.LEFT, padx=4)
            self.track_btns[name] = btn

        # --- PLAY BUTTON (above function, between track buttons and LEDs) ---
        self._make_section("play button (ladder on ain0/p0.02)")
        pframe = tk.Frame(self.root, bg=self.BG)
        pframe.pack(pady=(0, 12))
        self.play_btn = tk.Button(pframe, text="PLAY", font=("Helvetica", 11, "bold"),
                                  width=14, height=2, bg="#0f3460", fg=self.FG,
                                  activebackground=self.RED, activeforeground="#fff",
                                  relief=tk.FLAT, bd=1,
                                  command=lambda: self._press_button("play"))
        self.play_btn.pack()

        # --- PLAYBACK LEDS (between PLAY and FUNCTION) ---
        self._make_section("playback leds")
        lframe = tk.Frame(self.root, bg=self.BG)
        lframe.pack(pady=(0, 12))
        for name in ["p1", "p2", "p3", "p4"]:
            c = tk.Canvas(lframe, width=28, height=28, bg=self.BG, highlightthickness=0)
            c.pack(side=tk.LEFT, padx=14)
            self.led_widgets[name] = c.create_oval(4, 4, 24, 24, fill="#333", outline=self.DIM)
            tk.Label(lframe, text=name, font=("Helvetica", 7),
                     fg="#666", bg=self.BG).pack(side=tk.LEFT, padx=(0, 8))

        # --- FUNCTION BUTTON ---
        self._make_section("function button (p0.27)")
        fframe = tk.Frame(self.root, bg=self.BG)
        fframe.pack(pady=(0, 12))
        self.fn_btn = tk.Button(fframe, text="\u2022\u2022", font=("Helvetica", 14, "bold"),
                                width=4, height=1, bg="#0f3460", fg=self.FG,
                                activebackground=self.RED, activeforeground="#fff",
                                relief=tk.FLAT, bd=1,
                                command=lambda: self._press_button("function"))
        self.fn_btn.pack()

        # --- VOLUME ROCKER + FWD/REV (ladder on ain1/p0.03) ---
        self._make_section("rocker + transport (ladder on ain1/p0.03)")
        rframe = tk.Frame(self.root, bg=self.BG)
        rframe.pack(pady=(0, 10))

        volframe = tk.Frame(rframe, bg=self.BG)
        volframe.pack(side=tk.LEFT, padx=20)
        tk.Label(volframe, text="vol", font=("Helvetica", 7), fg="#666", bg=self.BG).pack()
        vol_up = tk.Button(volframe, text="+", font=("Helvetica", 9, "bold"),
                           width=3, height=1, bg="#16213e", fg=self.FG,
                           activebackground=self.RED, relief=tk.FLAT, bd=1,
                           command=lambda: self._press_button("vol_up"))
        vol_up.pack()
        vol_dn = tk.Button(volframe, text="-", font=("Helvetica", 9, "bold"),
                           width=3, height=1, bg="#16213e", fg=self.FG,
                           activebackground=self.RED, relief=tk.FLAT, bd=1,
                           command=lambda: self._press_button("vol_down"))
        vol_dn.pack()

        tk.Frame(rframe, width=1, height=50, bg=self.DIM).pack(side=tk.LEFT, padx=16)

        navframe = tk.Frame(rframe, bg=self.BG)
        navframe.pack(side=tk.LEFT, padx=20)
        tk.Label(navframe, text="nav", font=("Helvetica", 7), fg="#666", bg=self.BG).pack()
        self.fwd_btn = tk.Button(navframe, text="\u25c0\u25c0", font=("Helvetica", 9, "bold"),
                                 width=4, height=1, bg="#16213e", fg=self.FG,
                                 activebackground=self.RED, relief=tk.FLAT, bd=1,
                                 command=lambda: self._press_button("fwd"))
        self.fwd_btn.pack()
        self.rev_btn = tk.Button(navframe, text="\u25b6\u25b6", font=("Helvetica", 9, "bold"),
                                 width=4, height=1, bg="#16213e", fg=self.FG,
                                 activebackground=self.RED, relief=tk.FLAT, bd=1,
                                 command=lambda: self._press_button("rev"))
        self.rev_btn.pack()

        # --- STATUS ---
        self.status = tk.Label(self.root, text="connecting...", font=("Helvetica", 8),
                                fg="#666", bg=self.BG)
        self.status.pack(pady=(10, 4))

    def _make_section(self, text):
        tk.Label(self.root, text=text.upper(), font=("Helvetica", 7, "bold"),
                 fg=self.DIM, bg=self.BG).pack(pady=(0, 2))

    def _press_button(self, name):
        if not self.renode:
            return
        if name == "function":
            self.renode.cmd("gpioPortD ToggleButton btn_function")
        elif name == "play":
            self._set_adc_ladder(0, 0.05)
        elif name == "vol_up":
            self._set_adc_ladder(1, 0.1)
        elif name == "vol_down":
            self._set_adc_ladder(1, 0.3)
        elif name == "fwd":
            self._set_adc_ladder(1, 0.5)
        elif name == "rev":
            self._set_adc_ladder(1, 0.7)

    def _press_track(self, name):
        val = {"trk1": 0.15, "trk2": 0.35, "trk3": 0.55, "trk4": 0.75}[name]
        self._set_adc_ladder(0, val)

    def _set_adc_ladder(self, channel, voltage_fraction):
        pass

    def _connect_renode(self):
        def _try():
            try:
                self.renode = RenodeClient()
                self.renode.connect()
                self.root.after(0, lambda: self.status.configure(
                    text="connected", fg="#4ecca3"))
                self._start_polling()
            except Exception:
                msg = "no renode (retrying...)"
                self.root.after(0, lambda m=msg: self.status.configure(text=m, fg=self.RED))
                self.root.after(2000, _try)
        threading.Thread(target=_try, daemon=True).start()

    def _start_polling(self):
        def poll():
            try:
                resp = self.renode.cmd("gpioPortB ReadGPIO")
                try:
                    v = int(resp.strip().split()[-1], 16) if resp else 0
                except ValueError:
                    v = 0

                colors = {
                    "p1": self.RED if (v >> 13) & 1 else "#333",
                    "p2": self.RED if (v >> 0) & 1 else "#333",
                    "p3": self.RED if (v >> 12) & 1 else "#333",
                    "p4": self.RED if (v >> 1) & 1 else "#333",
                }
                resp2 = self.renode.cmd("gpioPortC ReadGPIO")
                try:
                    v2 = int(resp2.strip().split()[-1], 16) if resp2 else 0
                except ValueError:
                    v2 = 0
                colors.update({
                    "t1": self.RED if (v2 >> 29) & 1 else "#333",
                    "t2": self.RED if (v2 >> 26) & 1 else "#333",
                    "t3": self.RED if (v2 >> 15) & 1 else "#333",
                    "t4": self.RED if (v2 >> 14) & 1 else "#333",
                })

                for name, color in colors.items():
                    if name in self.led_widgets:
                        self.led_widgets[name].itemconfigure(
                            self.led_widgets[name], fill=color)
            except Exception:
                pass
            self.root.after(100, poll)
        self.root.after(500, poll)

    def run(self):
        self.root.mainloop()


if __name__ == "__main__":
    SP1GUI().run()
