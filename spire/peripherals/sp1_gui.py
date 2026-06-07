#!/usr/bin/env python3
"""
spire — SP-1 Emulator GUI
Virtual device modeled after the physical SP-1 hardware.
"""

import socket
import threading
import tkinter as tk

RENODE_HOST = "127.0.0.1"
RENODE_PORT = 3334

LED_MIRROR_ADDR   = 0x2000FFF0  # firmware writes packed P0+P1 output
FADER_MIRROR_BASE = 0x2000FFE0  # GUI writes fader values here

W = 600
H = 720

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
        for line in text.split('\n'):
            line = line.strip()
            if line.startswith('0x') or line.startswith('-0x'):
                return line
        return text

    def read32(self, addr):
        resp = self.cmd(f"sysbus ReadDoubleWord {hex(addr)}")
        for line in resp.split('\n'):
            line = line.strip()
            if line.startswith('0x') or line.startswith('-0x'):
                return int(line, 16)
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
    SILVER = "#c8c8c8"
    DARK  = "#2a2a2a"
    BG    = "#1a1a1a"
    WHITE = "#ffffff"
    DIM   = "#888888"
    OFF   = "#444444"
    RED   = "#e94560"

    def __init__(self):
        self.root = tk.Tk()
        self.root.title("spire — SP-1 Emulator")
        self.root.geometry(f"{W}x{H}")
        self.root.configure(bg=self.BG)
        self.root.resizable(False, False)

        self.renode = None
        self.led_widgets = {}
        self._led_canvases = {}
        self.fader_values = [2048, 2048, 2048, 2048]
        self._setup_ui()
        self._connect_renode()

    def _setup_ui(self):
        # --- Body ---
        body_x = 40
        body_w = 520
        self.body = tk.Canvas(self.root, width=body_w, height=660,
                              bg=self.SILVER, highlightthickness=0, bd=0)
        self.body.place(x=body_x, y=30)
        self.body.create_rectangle(0, 0, body_w, 660, outline="#999", width=2)

        # Title at top
        self.body.create_text(80, 22, text="sp • 1", font=("Helvetica", 11, "bold"),
                              fill="#666", anchor="w")

        # --- Row 1: Faders (4 sliders across) ---
        fader_x = [60, 160, 260, 360]
        self.fader_widgets = []
        for i in range(4):
            x = fader_x[i]
            y_base = 70
            h = 140
            # Track
            track_id = self.body.create_rectangle(x - 6, y_base, x + 6, y_base + h,
                                                   fill=self.DARK, outline="#999", width=1)
            # Knob
            knob_y = y_base + h - 20
            knob_id = self.body.create_rectangle(x - 10, knob_y, x + 10, knob_y + 16,
                                                  fill="#777", outline="#aaa", width=1)
            self.fader_widgets.append((x, y_base, h, track_id, knob_id))

        # Fader labels
        for i in range(4):
            self.body.create_text(fader_x[i], 56, text=f"fader {i+1}",
                                  font=("Helvetica", 7), fill="#888", anchor="s")

        # Drag bindings for faders
        self._dragging = None
        self.body.tag_bind("fader", "<Button-1>", self._fader_start)
        self.body.tag_bind("fader", "<B1-Motion>", self._fader_move)
        self.body.bind("<ButtonRelease-1>", self._fader_stop)

        # Make knobs draggable
        for _, (_, _, _, _, knob_id) in enumerate(self.fader_widgets):
            self.body.addtag_withtag("fader", knob_id)

        # --- Row 2: Track LEDs (under faders) ---
        track_leds_y = 230
        for i in range(4):
            x = fader_x[i]
            c = tk.Canvas(self.root, width=14, height=14, bg=self.SILVER,
                          highlightthickness=0)
            c.place(x=body_x + x - 7, y=30 + track_leds_y)
            name = f"t{i+1}"
            self.led_widgets[name] = c.create_oval(1, 1, 13, 13, fill=self.OFF, outline="#999")
            self._led_canvases[name] = c

        # --- Row 3: Track Buttons ---
        btn_y = 280
        self.track_btns = {}
        for i in range(4):
            x = fader_x[i]
            btn = tk.Button(self.root, text=f"trk{i+1}", font=("Helvetica", 9, "bold"),
                            width=5, height=2, bg=self.DARK, fg=self.SILVER, relief=tk.FLAT,
                            activebackground="#555", activeforeground="#fff")
            btn.place(x=body_x + x - 24, y=30 + btn_y)

        # --- Right side: Play button + LEDs + Function button ---
        right_x = body_x + body_w - 55
        rcx = right_x + 10

        # Play button (above LEDs)
        play_btn = tk.Button(self.root, text="PLAY", font=("Helvetica", 8, "bold"),
                             width=5, height=2, bg=self.DARK, fg=self.SILVER, relief=tk.FLAT,
                             activebackground="#555", activeforeground="#fff")
        play_btn.place(x=rcx - 12, y=90)

        # Playback LEDs
        led_names = ["p1", "p2", "p3", "p4"]
        for i, name in enumerate(led_names):
            c = tk.Canvas(self.root, width=18, height=18, bg=self.BG, highlightthickness=0)
            c.place(x=rcx - 4, y=180 + i * 55)
            self.led_widgets[name] = c.create_oval(2, 2, 16, 16, fill=self.OFF, outline="#555")
            self._led_canvases[name] = c

        # Function button (below LEDs)
        fn_btn = tk.Button(self.root, text="FUNC", font=("Helvetica", 9, "bold"),
                           width=5, height=2, bg=self.DARK, fg=self.SILVER, relief=tk.FLAT,
                           activebackground="#555", activeforeground="#fff")
        fn_btn.place(x=rcx - 12, y=440)

        # --- Top face: Volume circles + Rocker ---
        top_y = 34

        # Volume up (circle)
        self._make_circle_button(body_x + 20, top_y, "+")
        # Volume down (circle)
        self._make_circle_button(body_x + 50, top_y, "−")

        # Rocker (◀◀ / ▶▶) on left side
        rframe = tk.Frame(self.root, bg=self.SILVER)
        rframe.place(x=body_x + 5, y=top_y + 30)
        btn_prev = tk.Button(rframe, text="◀◀", font=("Helvetica", 7),
                             width=3, height=1, bg=self.DARK, fg=self.SILVER, relief=tk.FLAT)
        btn_prev.pack(pady=1)
        btn_next = tk.Button(rframe, text="▶▶", font=("Helvetica", 7),
                             width=3, height=1, bg=self.DARK, fg=self.SILVER, relief=tk.FLAT)
        btn_next.pack(pady=1)

        # --- Status ---
        self.status = tk.Label(self.root, text="connecting...", font=("Helvetica", 8),
                                fg="#666", bg=self.BG)
        self.status.place(x=20, y=695)

    def _make_circle_button(self, x, y, text):
        c = tk.Canvas(self.root, width=22, height=22, bg=self.SILVER, highlightthickness=0)
        c.place(x=x, y=y)
        c.create_oval(1, 1, 21, 21, fill=self.DARK, outline="#555", width=1)
        tk.Label(self.root, text=text, font=("Helvetica", 8, "bold"),
                 fg=self.SILVER, bg=self.DARK).place(x=x + 5, y=y + 1)

    def _fader_start(self, event):
        for i, (fx, fy, fh, tid, kid) in enumerate(self.fader_widgets):
            x0 = fx - 20
            y0 = fy
            x2 = fx + 20
            y2 = fy + fh
            bx, by = event.x, event.y
            if b'body' in str(self.body):
                bx = event.x
                by = event.y
            if x0 <= bx <= x2 and y0 <= by <= y2:
                self._dragging = i
                break

    def _fader_move(self, event):
        if self._dragging is None:
            return
        i = self._dragging
        fx, fy, fh, tid, kid = self.fader_widgets[i]
        new_y = max(fy, min(fy + fh - 16, event.y))
        knob_h = 16
        self.body.coords(kid, fx - 10, new_y, fx + 10, new_y + knob_h)
        fraction = (fy + fh - 16 - new_y) / (fh - 16)
        val = int(fraction * 4095)
        self.fader_values[i] = val
        addr = FADER_MIRROR_BASE + i * 4
        if self.renode:
            self.renode.cmd(f"sysbus WriteDoubleWord {hex(addr)} {val}")

    def _fader_stop(self, event):
        self._dragging = None

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
                self.root.after(0, lambda m=msg: self.status.configure(text=m, fg=self.RED))
                self.root.after(2000, _try)
        threading.Thread(target=_try, daemon=True).start()

    def _start_polling(self):
        self.led_map = {
            "p1": (1, 13), "p2": (0, 0), "p3": (1, 12), "p4": (0, 1),
            "t1": (0, 29), "t2": (0, 26), "t3": (1, 15), "t4": (1, 14),
        }

        def poll():
            try:
                packed = self.renode.read32(LED_MIRROR_ADDR)
                out0 = packed & 0xFFFF
                out1 = (packed >> 16) & 0xFFFF
                for name, (port, pin) in self.led_map.items():
                    val = out1 if port == 1 else out0
                    color = self.WHITE if (val >> pin) & 1 else self.OFF
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
