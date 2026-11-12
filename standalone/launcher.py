"""Windows launcher for a player-selected, offline Roguelike installation."""
from __future__ import annotations

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
        self.folder = tk.StringVar()
        self.status = tk.StringVar(value="Select the PD2 folder you want the launcher to modify.")
        self.events = queue.Queue()
        self.busy = False

        frame = ttk.Frame(self, padding=18)
        frame.grid(sticky="nsew")
        frame.columnconfigure(0, weight=1)
        ttk.Label(frame, text="PD2 Roguelike", font=("TkDefaultFont", 18, "bold")).grid(
            row=0, column=0, columnspan=2, sticky="w", pady=(0, 8))
        ttk.Label(frame, text="Single Player only • PD2 Season 13 patch 2\n"
                              "The selected folder will be modified. Copy PD2 first to protect your original.\n"
                              "This does not block PD2 Online; using the mod online may risk your account.\n"
                              "In town, type run start.",
                  justify="left", wraplength=560).grid(row=1, column=0, columnspan=2, sticky="w", pady=(0, 12))
        ttk.Label(frame, text="PD2 installation folder to modify").grid(row=2, column=0, columnspan=2, sticky="w")
        self.folder_entry = ttk.Entry(frame, textvariable=self.folder)
        self.folder_entry.grid(row=3, column=0, sticky="ew", pady=(3, 12))
        self.folder_button = ttk.Button(frame, text="Browse…", command=self.browse)
        self.folder_button.grid(row=3, column=1, padx=(8, 0), pady=(3, 12))
        buttons = ttk.Frame(frame)
        buttons.grid(row=4, column=0, columnspan=2, sticky="w")
        self.install_button = ttk.Button(buttons, text="Install / Update", command=self.update_mod)
        self.install_button.grid(row=0, column=0, padx=(0, 8))
        self.play_button = ttk.Button(buttons, text="Play", command=self.play)
        self.play_button.grid(row=0, column=1, padx=(0, 8))
        self.recover_button = ttk.Button(buttons, text="Recover interrupted update", command=self.recover)
        self.recover_button.grid(row=0, column=2, padx=(0, 8))
        ttk.Button(buttons, text="Licenses", command=self.show_licenses).grid(row=0, column=3)
        ttk.Label(frame, textvariable=self.status, wraplength=540, justify="left").grid(
            row=5, column=0, columnspan=2, sticky="ew", pady=(14, 0))
        self.controls = (self.folder_entry, self.folder_button,
                         self.install_button, self.play_button, self.recover_button)
        self._initial_paths()
        self.after(100, self._deliver)

    def _initial_paths(self):
        folder = Path(sys.executable).resolve().parent if getattr(sys, "frozen", False) else Path.cwd()
        if (folder / "ProjectD2/Game.exe").is_file():
            self.folder.set(str(folder))

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


    def browse(self):
        path = filedialog.askdirectory(parent=self, mustexist=True)
        if path:
            self.folder.set(path)

    def _path(self):
        folder = self.folder.get().strip()
        if not folder:
            raise ValueError("Choose the PD2 installation folder to modify.")
        return Path(folder)

    def _run(self, message, operation, success):
        if self.busy:
            return
        try:
            folder = self._path()
        except ValueError as error:
            messagebox.showerror("PD2 Roguelike", str(error), parent=self)
            return
        self.busy = True
        for control in self.controls:
            control.state(["disabled"])
        self.status.set(message)

        def worker():
            try:
                self.events.put((success, operation(folder), None))
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
        def operation(folder):
            # Validate the selected folder before fetching or changing its files.
            install.inspect(folder)
            release = releases.latest()
            return install.install(folder, releases.download(release))
        self._run("Checking the latest release and installing it into the selected folder…", operation,
                  lambda state: f"Roguelike {state['version']} is installed. Choose Play, then Single Player.")

    def play(self):
        self._run("Starting your Roguelike installation…", install.launch,
                  lambda pid: "Choose Single Player. Online play is not blocked and may risk your account.")

    def recover(self):
        self._run("Recovering the interrupted update…", install.recover,
                  lambda changed: "Previous update rolled back. You can install again." if changed
                  else "There is no interrupted update to recover.")


def main():
    Launcher().mainloop()


if __name__ == "__main__":
    main()
