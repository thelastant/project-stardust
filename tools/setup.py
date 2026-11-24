"""Guided, local first-run setup for Super Stardust HD."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import queue
import shutil
import subprocess
import sys
import threading
import tkinter as tk
from tkinter import filedialog, messagebox, ttk


ROOT = Path(__file__).resolve().parents[1]
TARGET = json.loads((ROOT / "config" / "target.json").read_text(encoding="utf-8"))


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


class SetupWindow(tk.Tk):
    def __init__(self) -> None:
        super().__init__()
        self.title("Super Stardust HD — Local Setup")
        self.geometry("760x570")
        self.minsize(660, 480)
        self.events: queue.Queue[tuple[str, str]] = queue.Queue()
        self.running = False

        ttk.Label(self, text="Build your own Super Stardust HD port",
                  font=("Segoe UI", 17, "bold")).pack(anchor="w", padx=18, pady=(16, 4))
        ttk.Label(self, text=(
            "This setup uses your own NPEA00014 game dump and PS3 firmware 4.93. "
            "Files stay on this PC; nothing is uploaded."
        ), wraplength=710).pack(anchor="w", padx=18, pady=(0, 12))

        form = ttk.Frame(self)
        form.pack(fill="x", padx=18)
        self.game = tk.StringVar()
        self.pup = tk.StringVar()
        self._path_row(form, 0, "Game folder (contains PS3_GAME)", self.game, self.pick_game)
        self._path_row(form, 1, "PS3UPDAT.PUP (firmware 4.93)", self.pup, self.pick_pup)

        self.status = tk.StringVar(value="Ready. Setup may take a while and downloads pinned dependencies.")
        ttk.Label(self, textvariable=self.status, wraplength=710).pack(anchor="w", padx=18, pady=(12, 4))
        self.log = tk.Text(self, height=17, state="disabled", wrap="word")
        self.log.pack(fill="both", expand=True, padx=18, pady=6)
        buttons = ttk.Frame(self)
        buttons.pack(fill="x", padx=18, pady=(0, 16))
        self.start_button = ttk.Button(buttons, text="Validate, build, and test", command=self.start)
        self.start_button.pack(side="left")
        self.launch_button = ttk.Button(buttons, text="Launch game", command=self.launch, state="disabled")
        self.launch_button.pack(side="left", padx=(8, 0))
        ttk.Button(buttons, text="Open project folder", command=lambda: os.startfile(ROOT)).pack(side="right")
        self.after(100, self.drain_events)

    def _path_row(self, parent, row, label, variable, callback):
        ttk.Label(parent, text=label).grid(row=row, column=0, sticky="w", pady=5)
        ttk.Entry(parent, textvariable=variable).grid(row=row, column=1, sticky="ew", padx=8)
        ttk.Button(parent, text="Browse…", command=callback).grid(row=row, column=2)
        parent.columnconfigure(1, weight=1)

    def pick_game(self):
        value = filedialog.askdirectory(title="Choose the folder containing PS3_GAME")
        if value:
            self.game.set(value)

    def pick_pup(self):
        value = filedialog.askopenfilename(title="Choose PS3UPDAT.PUP",
                                           filetypes=[("PS3 firmware update", "PS3UPDAT.PUP"), ("All files", "*")])
        if value:
            self.pup.set(value)

    def launch(self):
        executable = ROOT / "build" / "release" / "bin" / "ssHD.exe"
        if not executable.is_file():
            messagebox.showerror("Build not found", f"Could not find the game executable:\n{executable}")
            return
        os.startfile(executable)

    def append(self, text):
        self.log.configure(state="normal")
        self.log.insert("end", text.rstrip() + "\n")
        self.log.see("end")
        self.log.configure(state="disabled")

    def start(self):
        if self.running:
            return
        game = Path(self.game.get()).expanduser()
        pup = Path(self.pup.get()).expanduser()
        eboot = game / "PS3_GAME" / "USRDIR" / "EBOOT.BIN"
        if not game.is_dir() or not (game / "PS3_GAME").is_dir():
            messagebox.showerror("Game folder not found", "Choose your game folder, the one containing PS3_GAME.")
            return
        if not eboot.is_file() or not pup.is_file():
            messagebox.showerror("Required file not found", "The game folder must contain PS3_GAME/USRDIR/EBOOT.BIN, and the firmware file must exist.")
            return
        missing = [tool for tool in ("git", "pwsh", "cmake", "ninja", "clang")
                   if shutil.which(tool) is None]
        if missing:
            messagebox.showerror(
                "Build prerequisites missing",
                "Install the Windows prerequisites first. These commands were not found: "
                + ", ".join(missing) + ". See docs/building.md.",
            )
            return
        if sys.version_info < (3, 10):
            messagebox.showerror("Python version too old", "Setup requires Python 3.10 or newer.")
            return
        if not os.environ.get("VULKAN_SDK") or not Path(os.environ["VULKAN_SDK"]).is_dir():
            messagebox.showerror("Vulkan SDK not found", "Install the Vulkan SDK and set VULKAN_SDK.")
            return
        if not messagebox.askyesno(
            "GameInput runtime",
            "Setup will fetch the pinned GameInput package and install its Windows runtime. "
            "Windows may request administrator approval. Continue?",
        ):
            return
        self.status.set("Checking the game executable against the NPEA00014 pin…")
        self.start_button.configure(state="disabled")
        self.running = True
        threading.Thread(target=self.worker, args=(game.resolve(), pup.resolve(), eboot.resolve()), daemon=True).start()

    def worker(self, game: Path, pup: Path, eboot: Path):
        try:
            submodules = subprocess.run(
                ["git", "submodule", "status", "--recursive"], cwd=ROOT,
                check=False, capture_output=True, text=True,
            )
            if submodules.returncode or not submodules.stdout.strip():
                raise RuntimeError(
                    "This checkout has no initialized, pinned dependency submodules. "
                    "Use a complete Git clone of the project, not a source-only archive."
                )
            expected = TARGET["eboot_bin"]["sha256"]
            actual = sha256(eboot)
            if actual.lower() != expected.lower():
                raise RuntimeError(
                    "This EBOOT.BIN does not match the supported NPEA00014 executable. "
                    "Use the base game executable, not an update or another region."
                )
            self.events.put(("log", f"Game executable verified: {eboot}"))
            self.events.put(("status", "Preparing pinned build dependencies…"))
            commands = [
                ["git", "submodule", "update", "--init", "--recursive"],
                ["pwsh", "-NoProfile", "-File", "tools/fetch-ffmpeg.ps1"],
                ["pwsh", "-NoProfile", "-File", "tools/fetch-gameinput.ps1"],
                ["pwsh", "-NoProfile", "-File", "tools/build.ps1", "-Preset", "release",
                 "-EbootBin", str(eboot), "-Pup", str(pup)],
            ]
            for command in commands:
                self.run_command(command)
            gameinput_installers = list((ROOT / "generated" / "deps" / "gameinput").glob(
                "*/redist/GameInputRedist.msi"
            ))
            if len(gameinput_installers) != 1:
                raise RuntimeError("Could not locate the fetched GameInput runtime installer.")
            self.run_command(["msiexec", "/i", str(gameinput_installers[0])], accepted_codes=(0, 1638, 3010))
            emit_commands = [
                [sys.executable, "-B", "tools/ps3tool.py", "emit", "generated/inputs/EBOOT.elf",
                 "--chunks", "256", "--out", "generated/ppu", "--max-unhandled", "0"],
                [sys.executable, "-B", "tools/ps3tool.py", "emit", "generated/inputs/libsre.reloc.elf",
                 "--db", "config/libsre_functions.json", "--symbol-prefix", "libsre", "--chunks", "4",
                 "--out", "generated/ppu/libsre", "--max-unhandled", "0"],
                [sys.executable, "-B", "tools/ps3tool.py", "spu-emit", "--eboot", "generated/inputs/EBOOT.elf",
                 "--libsre", "generated/inputs/libsre.reloc.elf", "--out", "generated/ppu/spu",
                 "--max-unhandled", "0"],
                ["pwsh", "-NoProfile", "-File", "tools/build.ps1", "-Preset", "release",
                 "-GeneratedDir", "generated/ppu", "-GameDir", str(game), "-Test"],
            ]
            for command in emit_commands:
                self.run_command(command)
            self.events.put(("done", "Setup and tests completed. Your local build is ready to launch."))
        except Exception as error:
            self.events.put(("failed", str(error)))

    def run_command(self, command, accepted_codes=(0,)):
        label = Path(command[0]).name
        if command[0].lower() == "pwsh" and "-File" in command:
            label = Path(command[command.index("-File") + 1]).name
        self.events.put(("status", f"Running {label}…"))
        self.events.put(("log", "> " + subprocess.list2cmdline(command)))
        process = subprocess.Popen(
            command, cwd=ROOT, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
            text=True, errors="replace", bufsize=1,
        )
        for line in process.stdout:
            self.events.put(("log", line))
        code = process.wait()
        if code not in accepted_codes:
            raise RuntimeError(f"Command failed with exit code {code}: {subprocess.list2cmdline(command)}")

    def drain_events(self):
        try:
            while True:
                kind, value = self.events.get_nowait()
                if kind == "log":
                    self.append(value)
                elif kind == "status":
                    self.status.set(value)
                elif kind in ("done", "failed"):
                    self.running = False
                    self.start_button.configure(state="normal")
                    self.status.set(value)
                    if kind == "done":
                        self.launch_button.configure(state="normal")
                        messagebox.showinfo("Setup complete", value + "\n\nSelect Launch game to play.")
                    else:
                        self.append("SETUP STOPPED: " + value)
                        messagebox.showerror("Setup stopped", value + "\n\nSee the log for details.")
        except queue.Empty:
            pass
        self.after(100, self.drain_events)


if __name__ == "__main__":
    SetupWindow().mainloop()
