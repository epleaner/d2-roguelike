"""Windows launcher for a separately copied, offline Roguelike installation."""
from __future__ import annotations

import json
from pathlib import Path
import queue
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk

from standalone import install, releases


class Launcher(tk.Tk):
    def __init__(self):
        super().__init__()
        self.title("PD2 Roguelike")
        self.minsize(600, 360)
        self.columnconfigure(0, weight=1)
        self.protocol("WM_DELETE_WINDOW", self._close)
        self.original = tk.StringVar()
        self.copy = tk.StringVar()
        self.status = tk.StringVar(value="Select your original PD2 installation and your separate copy.")
        self.events = queue.Queue()
        self.busy = False

        frame = ttk.Frame(self, padding=18)
        frame.grid(sticky="nsew")
        frame.columnconfigure(0, weight=1)
        ttk.Label(frame, text="PD2 Roguelike", font=("TkDefaultFont", 18, "bold")).grid(
            row=0, column=0, columnspan=2, sticky="w", pady=(0, 8))
        ttk.Label(frame, text="Single Player only • PD2 Season 13 patch 2\n"
                              "This does not block PD2 Online; using the mod online may risk your account.\n"
                              "Make a separate copy of PD2 first. In town, type run start.",
                  justify="left", wraplength=560).grid(row=1, column=0, columnspan=2, sticky="w", pady=(0, 12))
        ttk.Label(frame, text="Original PD2 installation (read-only)").grid(row=2, column=0, columnspan=2, sticky="w")
        self.original_entry = ttk.Entry(frame, textvariable=self.original)
        self.original_entry.grid(row=3, column=0, sticky="ew", pady=(3, 9))
        self.original_button = ttk.Button(frame, text="Browse…", command=lambda: self.browse(self.original))
        self.original_button.grid(row=3, column=1, padx=(8, 0), pady=(3, 9))
        ttk.Label(frame, text="Separate Roguelike copy").grid(row=4, column=0, columnspan=2, sticky="w")
        self.copy_entry = ttk.Entry(frame, textvariable=self.copy)
        self.copy_entry.grid(row=5, column=0, sticky="ew", pady=(3, 12))
        self.copy_button = ttk.Button(frame, text="Browse…", command=lambda: self.browse(self.copy))
        self.copy_button.grid(row=5, column=1, padx=(8, 0), pady=(3, 12))
        buttons = ttk.Frame(frame)
        buttons.grid(row=6, column=0, columnspan=2, sticky="w")
        self.install_button = ttk.Button(buttons, text="Install / Update", command=self.update_mod)
        self.install_button.grid(row=0, column=0, padx=(0, 8))
        self.play_button = ttk.Button(buttons, text="Play", command=self.play)
        self.play_button.grid(row=0, column=1, padx=(0, 8))
        self.recover_button = ttk.Button(buttons, text="Recover interrupted update", command=self.recover)
        self.recover_button.grid(row=0, column=2, padx=(0, 8))
        ttk.Button(buttons, text="Licenses", command=self.show_licenses).grid(row=0, column=3)
        ttk.Label(frame, textvariable=self.status, wraplength=540, justify="left").grid(
            row=7, column=0, columnspan=2, sticky="ew", pady=(14, 0))
        self.controls = (self.original_entry, self.original_button, self.copy_entry,
                         self.copy_button, self.install_button, self.play_button, self.recover_button)
        self._initial_paths()
        self.after(100, self._deliver)

    def _initial_paths(self):
        folder = Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else Path.cwd()
        if (folder / "ProjectD2/Game.exe").is_file():
            self.copy.set(str(folder))
            receipt = folder / install._METADATA / install._RECEIPT
            if receipt.is_file():
                try:
                    state = json.loads(receipt.read_text(encoding="utf-8"))
                    if isinstance(state.get("original"), str):
                        self.original.set(state["original"])
                except (OSError, ValueError, AttributeError):
                    pass

    def _close(self):
        if self.busy:
            messagebox.showinfo("PD2 Roguelike", "Wait for the current action to finish.", parent=self)
        else:
            self.destroy()

    def show_licenses(self):
        directory = Path(__file__).resolve().parent / "licenses"
        try:
            names = ("Python.txt", "PyInstaller.txt", "OpenSSL.txt", "zlib.txt",
                     "StormLib.txt", "StormLib-components.txt", "BH-AGPL.txt", "BH-Apache.txt",
                     "Native-components.txt")
            notices = [(name, (directory / name).read_text(encoding="utf-8")) for name in names]
        except (OSError, UnicodeError, ValueError) as error:
            messagebox.showerror("PD2 Roguelike", str(error), parent=self)
            return
        window = tk.Toplevel(self)
        window.title("PD2 Roguelike — licenses")
        window.geometry("780x600")
        window.columnconfigure(0, weight=1)
        window.rowconfigure(0, weight=1)
        text = tk.Text(window, wrap="word")
        scroll = ttk.Scrollbar(window, command=text.yview)
        text.configure(yscrollcommand=scroll.set)
        text.grid(row=0, column=0, sticky="nsew")
        scroll.grid(row=0, column=1, sticky="ns")
        text.insert("end", "The native Roguelike BH source and its notices are provided with each release.\n\n")
        for name, content in notices:
            text.insert("end", f"--- {name} ---\n\n{content}\n\n")
        text.configure(state="disabled")


    def browse(self, value):
        path = filedialog.askdirectory(parent=self, mustexist=True)
        if path:
            value.set(path)

    def _paths(self):
        original, copy = self.original.get().strip(), self.copy.get().strip()
        if not original or not copy:
            raise ValueError("Choose both the original PD2 installation and your separate copy.")
        return Path(original), Path(copy)

    def _run(self, message, operation, success):
        if self.busy:
            return
        try:
            paths = self._paths()
        except ValueError as error:
            messagebox.showerror("PD2 Roguelike", str(error), parent=self)
            return
        self.busy = True
        for control in self.controls:
            control.state(["disabled"])
        self.status.set(message)

        def worker():
            try:
                self.events.put((success, operation(*paths), None))
            except Exception as error:
                self.events.put((success, None, error))
        threading.Thread(target=worker, daemon=True).start()

    def _deliver(self):
        try:
            while True:
                success, result, error = self.events.get_nowait()
                self.busy = False
                for control in self.controls:
                    control.state(["!disabled"])
                if error:
                    self.status.set("Action failed. No automatic retry was attempted.")
                    messagebox.showerror("PD2 Roguelike", str(error), parent=self)
                else:
                    self.status.set(success(result))
        except queue.Empty:
            pass
        self.after(100, self._deliver)

    def update_mod(self):
        def operation(original, copy):
            # Validate roots before fetching or changing anything in the copy.
            install.inspect(original, copy)
            release = releases.latest()
            return install.install(original, copy, releases.download(release))
        self._run("Checking the latest release and installing it into the copy…", operation,
                  lambda state: f"Roguelike {state['version']} is installed. Choose Play, then Single Player.")

    def play(self):
        self._run("Starting your separate Roguelike copy…", install.launch,
                  lambda pid: "Choose Single Player. Online play is not blocked and may risk your account.")

    def recover(self):
        self._run("Recovering the interrupted update…", install.recover,
                  lambda changed: "Previous update rolled back. You can install again." if changed
                  else "There is no interrupted update to recover.")


def main():
    Launcher().mainloop()


if __name__ == "__main__":
    main()
