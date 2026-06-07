#!/usr/bin/env python3
"""
spire — SP-1 Emulator GUI
Virtual buttons, LED visualization, and audio monitoring for the SP-1 emulator.
Connects to Renode via its monitor socket.
"""

import socket
import json
import threading
import tkinter as tk
from collections import deque
import wave
import struct
import os
import sys

RENODE_HOST = "127.0.0.1"
RENODE_PORT = 3334

class RenodeClient:
    def __init__(self):
        self.sock = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        self.lock = threading.Lock()

    def connect(self, host=RENODE_HOST, port=RENODE_PORT):
        self.sock.connect((host, port))
        self._recv()

    def cmd(self, command):
        with self.lock:
            self.sock.sendall((command + "\n").encode())
            return self._recv()

    def _recv(self):
        data = b""
        while True:
            chunk = self.sock.recv(4096)
            if not chunk:
                break
            data += chunk
            if b"\n" in chunk:
                break
        return data.decode(errors="replace").strip()

    def read_gpio(self, port, pin):
        resp = self.cmd(f"sysbus ReadDoubleWord 0x50000000")
        return int(resp.split()[-1], 16) if resp else 0

    def write_gpio(self, port, pin, value):
        self.cmd(f"gpio{port} State {pin} {value}")

    def monitor_gpio(self, gpio_name, pin, callback):
        def _watch():
            try:
                self.cmd(f"""
emulation CreateExternalPort gpio_{gpio_name}_{pin}
gpio_{gpio_name}_{pin} Connect gpioPortB StateChanged {pin}
""")
            except:
                pass
        threading.Thread(target=_watch, daemon=True).start()


class SP1GUI:
    def __init__(self):
        self.root = tk.Tk()
        self.root.title("spire — SP-1 Emulator")
        self.root.geometry("480x420")
        self.root.configure(bg="#1a1a2e")
        self.root.resizable(False, False)

        self.renode = None
        self.audio_buffer = deque(maxlen=44100)
        self.wav_file = None
        self._setup_ui()
        self._connect_renode()

    def _setup_ui(self):
        title = tk.Label(
            self.root, text="spire", font=("Helvetica", 18, "bold"),
            fg="#e94560", bg="#1a1a2e"
        )
        title.pack(pady=(12, 4))

        subtitle = tk.Label(
            self.root, text="sp-1 stem player emulator",
            font=("Helvetica", 9), fg="#888", bg="#1a1a2e"
        )
        subtitle.pack(pady=(0, 16))

        self._make_section("playback leds")
        self.led_frame = tk.Frame(self.root, bg="#1a1a2e")
        self.led_frame.pack(pady=(0, 12))
        self.leds = {}
        for i, name in enumerate(["P1", "P2", "P3", "P4"]):
            canvas = tk.Canvas(
                self.led_frame, width=36, height=36,
                bg="#1a1a2e", highlightthickness=0
            )
            self.leds[name] = canvas.create_oval(6, 6, 30, 30, fill="#333", outline="#555")
            canvas.pack(side=tk.LEFT, padx=10)
            tk.Label(self.led_frame, text=name, font=("Helvetica", 8),
                     fg="#666", bg="#1a1a2e").pack(side=tk.LEFT, padx=(0, 6))

        self._make_section("buttons")
        self.btn_frame = tk.Frame(self.root, bg="#1a1a2e")
        self.btn_frame.pack(pady=(0, 12))

        buttons = [
            ("play", "top"), ("trk1", ""), ("trk2", ""), ("trk3", ""), ("trk4", ""),
            ("vol+", ""), ("vol-", ""), ("fwd", ""), ("rev", ""),
            ("func", "bottom")
        ]
        self.button_widgets = {}
        for name, pos in buttons:
            btn = tk.Button(
                self.btn_frame, text=name, font=("Helvetica", 9, "bold"),
                width=5, height=2, bg="#16213e", fg="#ccc",
                activebackground="#e94560", activeforeground="#fff",
                relief=tk.FLAT, bd=1
            )
            btn.pack(side=tk.LEFT, padx=3)
            self.button_widgets[name] = btn

        self._make_section("audio")
        self.audio_canvas = tk.Canvas(
            self.root, width=420, height=60, bg="#0f0f23", highlightthickness=1,
            highlightbackground="#333"
        )
        self.audio_canvas.pack(pady=(0, 12))

        self.status = tk.Label(
            self.root, text="connecting to renode...",
            font=("Helvetica", 8), fg="#666", bg="#1a1a2e"
        )
        self.status.pack(pady=(8, 4))

    def _make_section(self, text):
        frame = tk.Frame(self.root, bg="#1a1a2e")
        frame.pack(pady=(0, 2))
        tk.Label(
            frame, text=text.upper(), font=("Helvetica", 7, "bold"),
            fg="#555", bg="#1a1a2e"
        ).pack()

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
                self.root.after(0, lambda m=msg: self.status.configure(
                    text=m, fg="#e94560"))
                self.root.after(2000, _try)

        threading.Thread(target=_try, daemon=True).start()

    def _start_polling(self):
        def poll():
            try:
                val = self.renode.cmd("gpioPortB ReadGPIO")
                if val:
                    try:
                        v = int(val.split()[-1], 16)
                    except:
                        v = 0
                else:
                    v = 0

                colors = {
                    "P1": "#e94560" if (v >> 13) & 1 else "#333",
                    "P2": "#e94560" if (v >>  0) & 1 else "#333",
                    "P3": "#e94560" if (v >> 12) & 1 else "#333",
                    "P4": "#e94560" if (v >>  1) & 1 else "#333",
                }
                for name, color in colors.items():
                    self.led_canvas(name).itemconfigure(self.leds[name], fill=color)

                self._draw_audio()
            except:
                pass

            self.root.after(50, poll)

        self.root.after(500, poll)

    def led_canvas(self, name):
        return [w for w in self.led_frame.winfo_children()
                if isinstance(w, tk.Canvas)][
            ["P1", "P2", "P3", "P4"].index(name)
        ]

    def _draw_audio(self):
        c = self.audio_canvas
        c.delete("all")
        buf = list(self.audio_buffer)
        if not buf:
            c.create_text(210, 30, text="no audio data", fill="#444",
                          font=("Helvetica", 9))
            return
        step = max(1, len(buf) / 420)
        h = 60
        for i in range(419):
            idx = int(i * step)
            idx2 = int((i + 1) * step)
            if idx >= len(buf):
                break
            v = abs(buf[idx]) / 32768.0 * (h / 2) + h / 2
            v2 = abs(buf[min(idx2, len(buf) - 1)]) / 32768.0 * (h / 2) + h / 2
            c.create_line(i, v, i + 1, v2, fill="#e94560", width=1)
        c.create_rectangle(0, h / 2 - 1, 420, h / 2 + 1, fill="#333", outline="")

    def feed_audio(self, samples):
        self.audio_buffer.extend(samples)
        if self.wav_file:
            self.wav_file.writeframes(
                struct.pack(f"<{len(samples)}h", *samples))

    def record_audio(self, filename="spire_output.wav"):
        self.wav_file = wave.open(filename, "wb")
        self.wav_file.setnchannels(2)
        self.wav_file.setsampwidth(2)
        self.wav_file.setframerate(48000)

    def stop_recording(self):
        if self.wav_file:
            self.wav_file.close()
            self.wav_file = None

    def run(self):
        self.root.mainloop()


if __name__ == "__main__":
    gui = SP1GUI()
    gui.run()
