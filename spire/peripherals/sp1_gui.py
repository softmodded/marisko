#!/usr/bin/env python3
"""
spire — SP-1 Emulator GUI
Virtual device modeled after the physical SP-1 hardware.
Connects to Renode via its monitor socket.
"""

import socket
import threading
import tkinter as tk

RENODE_HOST = "127.0.0.1"
RENODE_PORT = 3334

LED_MIRROR_ADDR = 0x2000FFF0


class RenodeClient:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.lock = threading.Lock()

    def connect(self, host=RENODE_HOST, port=RENODE_PORT):
        self.sock.settimeout(3)
        try:
            self.sock.connect((host, port))
            self._recv()
            return True
        except Exception:
            return False

    def cmd(self, command):
        with self.lock:
            try:
                self.sock.sendall((command + "\n").encode())
                return self._recv_result()
            except Exception:
                return ""

    def _recv_result(self):
        data = b""
        while True:
            try:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                data += chunk
                if b"(machine-0)" in data or b"(monitor)" in data:
                    break
            except socket.timeout:
                if data:
                    break
                return ""
        text = data.decode(errors="replace").strip()
        lines = text.split('\n')
        for line in reversed(lines):
            line = line.strip()
            if line.startswith('0x') or line.startswith('-0x'):
                return line
        return text

    def read32(self, addr):
        resp = self.cmd(f"sysbus ReadDoubleWord {hex(addr)}")
        try:
            for line in resp.split('\n'):
                line = line.strip()
                if line.startswith('0x') or line.startswith('-0x'):
                    return int(line, 16)
        except:
            pass
        return 0

    def _recv(self):
        data = b""
        while True:
            try:
                chunk = self.sock.recv(4096)
                if not chunk:
                    break
                data += chunk
                if b"(machine-0)" in data or b"(monitor)" in data:
                    break
            except socket.timeout:
                break
        return data.decode(errors="replace").strip()


class SP1GUI:
    SILVER = "#c0c0c0"
    DARK  = "#2a2a2a"
    BG    = "#1a1a1a"
    RED   = "#ffffff"
    DIM   = "#888888"
    OFF   = "#444444"

    def __init__(self):
        self.root = tk.Tk()
        self.root.title("spire — SP-1 Emulator")
        self.root.geometry("520x680")
        self.root.configure(bg=self.BG)
        self.root.resizable(False, False)

        self.renode = None
        self.led_widgets = {}
        self._led_canvases = {}
        self._setup_ui()
        self._connect_renode()

    def _setup_ui(self):
        # --- Body ---
        body = tk.Canvas(self.root, width=400, height=620, bg=self.SILVER,
                         highlightthickness=0, bd=0)
        body.place(x=60, y=30)
        body.create_rectangle(2, 2, 398, 618, outline="#999", width=2)

        # Title
        tk.Label(self.root, text="sp-1", font=("Helvetica", 8),
                 fg="#666", bg=self.SILVER).place(x=240, y=38)

        # --- Sliders (center vertical line of 4 faders) ---
        slider_y = [130, 230, 330, 430]
        self.slider_canvases = []
        for i, y in enumerate(slider_y):
            c = tk.Canvas(self.root, width=30, height=80, bg=self.SILVER,
                          highlightthickness=0)
            c.place(x=190, y=y)
            c.create_rectangle(10, 5, 22, 75, fill=self.DARK, outline="#999", width=1)
            knob_y = 40 + (i - 1.5) * 15
            c.create_rectangle(8, knob_y, 24, knob_y + 12, fill="#666",
                               outline="#888", width=1)
            self.slider_canvases.append(c)

        # --- Track buttons (under each slider) ---
        track_names = ["1", "2", "3", "4"]
        self.track_btns = {}
        for i, y in enumerate(slider_y):
            btn = tk.Button(self.root, text=track_names[i],
                            font=("Helvetica", 9, "bold"), width=2, height=3,
                            bg=self.DARK, fg=self.SILVER, relief=tk.FLAT,
                            activebackground="#555", activeforeground="#fff")
            btn.place(x=290, y=y + 20)

        # --- Right face: Play button + LEDs + Function button ---
        # Play button
        play_btn = tk.Button(self.root, text="PLAY", font=("Helvetica", 8, "bold"),
                             width=4, height=3, bg=self.DARK, fg=self.SILVER,
                             relief=tk.FLAT, activebackground="#555", activeforeground="#fff")
        play_btn.place(x=490, y=120)

        # Playback LEDs between Play and Function
        led_names = ["p1", "p2", "p3", "p4"]
        for i, name in enumerate(led_names):
            c = tk.Canvas(self.root, width=18, height=18, bg=self.BG,
                          highlightthickness=0)
            c.place(x=496, y=210 + i * 50)
            self.led_widgets[name] = c.create_oval(2, 2, 16, 16, fill=self.OFF,
                                                    outline="#555")
            self._led_canvases[name] = c

        # Function button
        fn_btn = tk.Button(self.root, text="••", font=("Helvetica", 11, "bold"),
                           width=4, height=2, bg=self.DARK, fg=self.SILVER,
                           relief=tk.FLAT, activebackground="#555", activeforeground="#fff")
        fn_btn.place(x=490, y=450)

        # --- Top face buttons ---
        # Volume circles (left side of top)
        self._make_top_button(140, 42, "+")   # vol up
        self._make_top_button(140, 78, "−")   # vol down

        # Rocker (beside volume)
        rocker_frame = tk.Frame(self.root, bg=self.SILVER)
        rocker_frame.place(x=200, y=45)
        btn_prev = tk.Button(rocker_frame, text="◀◀", font=("Helvetica", 7),
                             width=3, height=1, bg=self.DARK, fg=self.SILVER,
                             relief=tk.FLAT)
        btn_prev.pack()
        btn_next = tk.Button(rocker_frame, text="▶▶", font=("Helvetica", 7),
                             width=3, height=1, bg=self.DARK, fg=self.SILVER,
                             relief=tk.FLAT)
        btn_next.pack()

        # --- Track LEDs (small indicators near sliders) ---
        track_led_names = ["t1", "t2", "t3", "t4"]
        for i, y in enumerate(slider_y):
            c = tk.Canvas(self.root, width=12, height=12, bg=self.SILVER,
                          highlightthickness=0)
            c.place(x=310, y=y + 35)
            self.led_widgets[track_led_names[i]] = c.create_oval(
                1, 1, 11, 11, fill=self.OFF, outline="#999")
            self._led_canvases[track_led_names[i]] = c

        # --- Status bar ---
        self.status = tk.Label(self.root, text="connecting...", font=("Helvetica", 8),
                                fg="#666", bg=self.BG)
        self.status.place(x=20, y=660)

    def _make_top_button(self, x, y, text):
        c = tk.Canvas(self.root, width=22, height=22, bg=self.DARK,
                      highlightthickness=0)
        c.place(x=x, y=y)
        c.create_oval(2, 2, 20, 20, fill=self.DARK, outline="#555", width=1)
        tk.Label(self.root, text=text, font=("Helvetica", 8, "bold"),
                 fg=self.SILVER, bg=self.DARK).place(x=x + 6, y=y + 2)

    def _connect_renode(self):
        def _try():
            try:
                self.renode = RenodeClient()
                if not self.renode.connect():
                    raise Exception("connection failed")
                self.root.after(0, lambda: self.status.configure(
                    text="connected", fg="#4ecca3"))
                self._start_polling()
            except Exception:
                msg = "no renode (retrying...)"
                self.root.after(0, lambda m=msg: self.status.configure(text=m, fg="#e94560"))
                self.root.after(2000, _try)
        threading.Thread(target=_try, daemon=True).start()

    def _start_polling(self):
        self.led_map = {
            "p1": (1, 13),
            "p2": (0, 0),
            "p3": (1, 12),
            "p4": (0, 1),
            "t1": (0, 29),
            "t2": (0, 26),
            "t3": (1, 15),
            "t4": (1, 14),
        }

        def poll():
            try:
                packed = self.renode.read32(LED_MIRROR_ADDR)
                out0 = packed & 0xFFFF
                out1 = (packed >> 16) & 0xFFFF
                for name, (port, pin) in self.led_map.items():
                    val = out1 if port == 1 else out0
                    color = self.RED if (val >> pin) & 1 else self.OFF
                    if name in self.led_widgets and name in self._led_canvases:
                        self._led_canvases[name].itemconfigure(
                            self.led_widgets[name], fill=color)
            except Exception:
                pass
            self.root.after(100, poll)
        self.root.after(500, poll)

    def run(self):
        self.root.mainloop()


if __name__ == "__main__":
    SP1GUI().run()
