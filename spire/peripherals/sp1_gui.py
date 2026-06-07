#!/usr/bin/env python3
"""
spire — SP-1 Emulator GUI
Virtual device with clickable buttons and live LED visualization.
Connects to Renode via its monitor socket.
"""

import socket
import threading
import tkinter as tk

RENODE_HOST = "127.0.0.1"
RENODE_PORT = 3334

# GPIO state mirrored to these RAM addresses by firmware (emulator I/O)
LED_P0_ADDR = 0x2000FFF0
LED_P1_ADDR = 0x2000FFF4


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
                return self._recv()
            except Exception:
                return ""

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
    BG = "#1a1a2e"
    FG = "#ccc"
    RED = "#e94560"
    DIM = "#555"

    def __init__(self):
        self.root = tk.Tk()
        self.root.title("spire — SP-1 Emulator")
        self.root.geometry("460x520")
        self.root.configure(bg=self.BG)
        self.root.resizable(False, False)

        self.renode = None
        self.led_widgets = {}
        self.led_map = {}
        self._setup_ui()
        self._connect_renode()

    def _setup_ui(self):
        tk.Label(self.root, text="spire", font=("Helvetica", 18, "bold"),
                 fg=self.RED, bg=self.BG).pack(pady=(12, 2))
        tk.Label(self.root, text="sp-1 stem player emulator",
                 font=("Helvetica", 8), fg="#888", bg=self.BG).pack(pady=(0, 12))

        self._make_section("track leds")
        tframe = tk.Frame(self.root, bg=self.BG)
        tframe.pack(pady=(0, 14))
        for name in ["t1", "t2", "t3", "t4"]:
            c = tk.Canvas(tframe, width=28, height=28, bg=self.BG, highlightthickness=0)
            c.pack(side=tk.LEFT, padx=14)
            self.led_widgets[name] = c.create_oval(4, 4, 24, 24, fill="#333", outline=self.DIM)
            tk.Label(tframe, text=name, font=("Helvetica", 7), fg="#666", bg=self.BG).pack(side=tk.LEFT, padx=(0, 8))

        self._make_section("track buttons (ladder on ain0/p0.02)")
        tbframe = tk.Frame(self.root, bg=self.BG)
        tbframe.pack(pady=(0, 12))
        self.track_btns = {}
        for name in ["trk1", "trk2", "trk3", "trk4"]:
            btn = tk.Button(tbframe, text=name, font=("Helvetica", 9, "bold"),
                            width=6, height=2, bg="#16213e", fg=self.FG,
                            activebackground=self.RED, activeforeground="#fff",
                            relief=tk.FLAT, bd=1)
            btn.pack(side=tk.LEFT, padx=4)
            self.track_btns[name] = btn

        self._make_section("play button (ladder on ain0/p0.02)")
        pframe = tk.Frame(self.root, bg=self.BG)
        pframe.pack(pady=(0, 12))
        self.play_btn = tk.Button(pframe, text="PLAY", font=("Helvetica", 11, "bold"),
                                  width=14, height=2, bg="#0f3460", fg=self.FG,
                                  activebackground=self.RED, activeforeground="#fff",
                                  relief=tk.FLAT, bd=1)
        self.play_btn.pack()

        self._make_section("playback leds")
        lframe = tk.Frame(self.root, bg=self.BG)
        lframe.pack(pady=(0, 12))
        for name in ["p1", "p2", "p3", "p4"]:
            c = tk.Canvas(lframe, width=28, height=28, bg=self.BG, highlightthickness=0)
            c.pack(side=tk.LEFT, padx=14)
            self.led_widgets[name] = c.create_oval(4, 4, 24, 24, fill="#333", outline=self.DIM)
            tk.Label(lframe, text=name, font=("Helvetica", 7), fg="#666", bg=self.BG).pack(side=tk.LEFT, padx=(0, 8))

        self._make_section("function button (p0.27)")
        fframe = tk.Frame(self.root, bg=self.BG)
        fframe.pack(pady=(0, 12))
        self.fn_btn = tk.Button(fframe, text="\u2022\u2022", font=("Helvetica", 14, "bold"),
                                width=4, height=1, bg="#0f3460", fg=self.FG,
                                activebackground=self.RED, activeforeground="#fff",
                                relief=tk.FLAT, bd=1)
        self.fn_btn.pack()

        self._make_section("rocker + transport (ladder on ain1/p0.03)")
        rframe = tk.Frame(self.root, bg=self.BG)
        rframe.pack(pady=(0, 10))
        volframe = tk.Frame(rframe, bg=self.BG)
        volframe.pack(side=tk.LEFT, padx=20)
        tk.Label(volframe, text="vol", font=("Helvetica", 7), fg="#666", bg=self.BG).pack()
        vol_up = tk.Button(volframe, text="+", font=("Helvetica", 9, "bold"),
                           width=3, height=1, bg="#16213e", fg=self.FG,
                           activebackground=self.RED, relief=tk.FLAT, bd=1)
        vol_up.pack()
        vol_dn = tk.Button(volframe, text="-", font=("Helvetica", 9, "bold"),
                           width=3, height=1, bg="#16213e", fg=self.FG,
                           activebackground=self.RED, relief=tk.FLAT, bd=1)
        vol_dn.pack()
        tk.Frame(rframe, width=1, height=50, bg=self.DIM).pack(side=tk.LEFT, padx=16)
        navframe = tk.Frame(rframe, bg=self.BG)
        navframe.pack(side=tk.LEFT, padx=20)
        tk.Label(navframe, text="nav", font=("Helvetica", 7), fg="#666", bg=self.BG).pack()
        self.fwd_btn = tk.Button(navframe, text="\u25c0\u25c0", font=("Helvetica", 9, "bold"),
                                 width=4, height=1, bg="#16213e", fg=self.FG,
                                 activebackground=self.RED, relief=tk.FLAT, bd=1)
        self.fwd_btn.pack()
        self.rev_btn = tk.Button(navframe, text="\u25b6\u25b6", font=("Helvetica", 9, "bold"),
                                 width=4, height=1, bg="#16213e", fg=self.FG,
                                 activebackground=self.RED, relief=tk.FLAT, bd=1)
        self.rev_btn.pack()

        self.status = tk.Label(self.root, text="connecting...", font=("Helvetica", 8),
                                fg="#666", bg=self.BG)
        self.status.pack(pady=(10, 4))

    def _make_section(self, text):
        tk.Label(self.root, text=text.upper(), font=("Helvetica", 7, "bold"),
                 fg=self.DIM, bg=self.BG).pack(pady=(0, 2))

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
            "p1": (LED_P1_ADDR, 13),
            "p2": (LED_P0_ADDR, 0),
            "p3": (LED_P1_ADDR, 12),
            "p4": (LED_P0_ADDR, 1),
            "t1": (LED_P0_ADDR, 29),
            "t2": (LED_P0_ADDR, 26),
            "t3": (LED_P1_ADDR, 15),
            "t4": (LED_P1_ADDR, 14),
        }

        def poll():
            try:
                out0 = self.renode.read32(LED_P0_ADDR)
                out1 = self.renode.read32(LED_P1_ADDR)
                print(f"[spire-gui] P0=0x{out0:08X} P1=0x{out1:08X}", flush=True)
                for name, (base, pin) in self.led_map.items():
                    val = out1 if base == LED_P1_ADDR else out0
                    color = self.RED if (val >> pin) & 1 else "#333"
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
