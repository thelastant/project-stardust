"""Shared local build helpers for the command-line game converter."""

from __future__ import annotations

import hashlib
import json
import os
from pathlib import Path
import shutil
import subprocess
import tempfile

ROOT = Path(__file__).resolve().parents[1]
TARGET = json.loads((ROOT / "config" / "target.json").read_text(encoding="utf-8"))
SETUP_PRESET = "setup-release"
DELIVERY = ROOT / "ProjectStardust"
GAME_OUTPUT = DELIVERY / "NPEA00014"


def sha256(path: Path) -> str:
    digest = hashlib.sha256()
    with path.open("rb") as stream:
        for block in iter(lambda: stream.read(1024 * 1024), b""):
            digest.update(block)
    return digest.hexdigest()


def validate_eboot_elf(path: Path) -> None:
    if path.name.casefold() != "eboot.elf" or not path.is_file():
        raise ValueError("Choose the decrypted game executable named EBOOT.ELF.")
    expected = TARGET["eboot_elf"]
    if path.stat().st_size != expected["size"] or sha256(path).lower() != expected["sha256"].lower():
        raise ValueError(
            "This EBOOT.ELF does not match the supported NPEA00014 executable. "
            "Use the already-updated executable version pinned by this project, not another "
            "update or region."
        )


def validate_pup(path: Path) -> None:
    if path.name.casefold() != "ps3updat.pup" or not path.is_file():
        raise ValueError("Choose the firmware update file named PS3UPDAT.PUP.")


def detect_game_dir(eboot: Path) -> Path:
    """Infer nearby source files when possible; EBOOT.ELF itself may be anywhere."""
    resolved = eboot.resolve()
    if not resolved.is_file() or resolved.name.casefold() != "eboot.elf":
        raise ValueError("Choose the decrypted game executable named EBOOT.ELF.")

    for directory in (resolved.parent, *resolved.parents):
        game_dir = directory / "PS3_GAME"
        if game_dir.is_dir():
            return directory
        if directory.name.casefold() == "usrdir" and directory.parent.name.casefold() == "ps3_game":
            return directory.parent.parent
        if directory.name.casefold() == "usrdir":
            return directory.parent

    return resolved.parent


def prepare_game_folder(source: Path, destination: Path, eboot: Path) -> Path:
    """Stage output as NPEA00014/USRDIR/EBOOT.ELF, without a PS3_GAME wrapper."""
    source = source.resolve()
    destination = destination.resolve()
    eboot = eboot.resolve()
    if not destination.exists():
        destination.parent.mkdir(parents=True, exist_ok=True)
        staging = Path(tempfile.mkdtemp(prefix=".NPEA00014-", dir=destination.parent))
        staging.rmdir()
        try:
            payload = source / "PS3_GAME" if (source / "PS3_GAME").is_dir() else source
            if payload.name.casefold() == "usrdir":
                shutil.copytree(payload, staging / "USRDIR")
            elif (payload / "USRDIR").is_dir():
                shutil.copytree(payload, staging)
            elif payload.name.casefold() == "ps3_game":
                shutil.copytree(payload, staging)
            else:
                staging.mkdir(parents=True)
            (staging / "USRDIR").mkdir(parents=True, exist_ok=True)
            shutil.copy2(eboot, staging / "USRDIR" / "EBOOT.ELF")
            staging.rename(destination)
        except Exception:
            if staging.exists():
                shutil.rmtree(staging)
            raise
    else:
        if not destination.is_dir():
            raise ValueError(f"The NPEA00014 output path is not a directory: {destination}")
        if (destination / "PS3_GAME").exists():
            raise ValueError(
                f"{destination} still has a PS3_GAME wrapper. Move that old output aside; "
                "the builder writes directly into NPEA00014."
            )
        (destination / "USRDIR").mkdir(parents=True, exist_ok=True)
        output_eboot = destination / "USRDIR" / "EBOOT.ELF"
        if eboot != output_eboot.resolve():
            shutil.copy2(eboot, output_eboot)
    if not (destination / "USRDIR" / "EBOOT.ELF").is_file():
        raise ValueError(f"The required output is missing: {destination / 'USRDIR' / 'EBOOT.ELF'}")
    return destination


def assemble_deliverable() -> Path:
    """Build a fresh player folder, keeping only player data and portable settings.

    Everything else is regenerated, so an older build leaves no stale files behind. Player data is
    copied into staging first and only removed from the old deliverable after staging is complete.
    """
    build_bin = ROOT / "build" / SETUP_PRESET / "bin"
    source_exe = build_bin / "ssHD.exe"
    if not source_exe.is_file():
        raise ValueError(f"The build did not produce the game executable: {source_exe}")
    game_eboot = GAME_OUTPUT / "USRDIR" / "EBOOT.ELF"
    if not game_eboot.is_file():
        raise ValueError(f"The game data is missing: {game_eboot}")
    libsre = ROOT / "generated" / "inputs" / "libsre.reloc.elf"
    if not libsre.is_file():
        raise ValueError(f"The firmware module is missing: {libsre}")

    staging = ROOT / f".{DELIVERY.name}.new"
    if staging.exists():
        shutil.rmtree(staging)
    staging.mkdir()
    keep = ("NPEA00014", "saves", "Logs", "config.json")
    try:
        if DELIVERY.is_dir():
            for name in keep:
                if name == "Logs":
                    continue
                source = DELIVERY / name
                if source.is_dir():
                    shutil.copytree(source, staging / name)
                elif source.is_file():
                    shutil.copy2(source, staging / name)
            old_game_log = DELIVERY / "Logs" / "run.log"
            if old_game_log.is_file():
                (staging / "Logs").mkdir(exist_ok=True)
                shutil.copy2(old_game_log, staging / "Logs" / "run.log")
        shutil.copy2(source_exe, staging / "SUPER STARDUST HD.exe")
        removed_frame_gen_dlls = {"sl.dlss_g.dll", "sl.reflex.dll", "sl.pcl.dll", "nvngx_dlssg.dll"}
        for dll in sorted(build_bin.glob("*.dll")):
            if dll.name.casefold() in removed_frame_gen_dlls:
                continue
            shutil.copy2(dll, staging / dll.name)
        for name in ("fonts", "licenses"):
            source = build_bin / name
            if source.is_dir():
                shutil.copytree(source, staging / name)
        data_dir = staging / "data"
        data_dir.mkdir(exist_ok=True)
        shutil.copy2(libsre, data_dir / "libsre.reloc.elf")

        config = staging / "config.json"
        wanted = {
            "game_dir": "NPEA00014",
            "elf": "NPEA00014/USRDIR/EBOOT.ELF",
            "libsre": "data/libsre.reloc.elf",
            "hdd_dir": "saves",
            "shader_cache": "cache/shaders",
            "log": "Logs/run.log",
        }
        existing: dict[str, object] = {}
        if config.is_file():
            try:
                loaded = json.loads(config.read_text(encoding="utf-8"))
                if isinstance(loaded, dict):
                    existing = loaded
            except (OSError, ValueError):
                existing = {}
        existing.pop("frame_gen", None)
        for key in tuple(existing):
            if key.startswith("fg_"):
                del existing[key]
        # Player settings survive a rebuild; only paths owned by the builder are reset.
        merged = {**wanted, **{key: value for key, value in existing.items() if key not in wanted}}
        config.write_text(json.dumps(merged, indent=2) + "\n", encoding="utf-8")
        (staging / "Play.cmd").write_text(
            "@echo off\r\n"
            "rem Starts Project Stardust from this folder.\r\n"
            'cd /d "%~dp0"\r\n'
            'start "" "SUPER STARDUST HD.exe"\r\n',
            encoding="utf-8",
        )

        # Replace the previous deliverable only after the new one is complete. Moving entries rather
        # than renaming the directory works around Windows directory handles held by indexers.
        if DELIVERY.exists():
            try:
                shutil.rmtree(DELIVERY)
            except PermissionError as error:
                raise ValueError(
                    "The game or a file in ProjectStardust is in use. Close it and build again."
                ) from error
        DELIVERY.mkdir(parents=True, exist_ok=True)
        try:
            for entry in staging.iterdir():
                shutil.move(str(entry), str(DELIVERY / entry.name))
            staging.rmdir()
        except PermissionError as error:
            raise ValueError(
                "A file in ProjectStardust is in use. Close the game and build again."
            ) from error
    except Exception:
        # Leave the player's data where it was if the swap could not be finished.
        for name in keep:
            kept = staging / name
            if kept.exists() and not (DELIVERY / name).exists():
                try:
                    DELIVERY.mkdir(exist_ok=True)
                    shutil.move(str(kept), str(DELIVERY / name))
                except OSError:
                    pass
        if staging.exists():
            shutil.rmtree(staging, ignore_errors=True)
        raise
    return DELIVERY


def create_desktop_shortcut(delivery: Path) -> Path:
    """Create a Windows shortcut to the built game, with its portable folder as working directory."""
    if os.name != "nt":
        raise OSError("Desktop shortcuts are supported on Windows only.")
    powershell = shutil.which("powershell.exe") or shutil.which("pwsh.exe")
    if not powershell:
        raise OSError("PowerShell is required to create the desktop shortcut.")
    target = delivery / "SUPER STARDUST HD.exe"
    script = (
        "$shell = New-Object -ComObject WScript.Shell; "
        "$shortcutPath = Join-Path ([Environment]::GetFolderPath('Desktop')) 'SUPER STARDUST HD.lnk'; "
        "$shortcut = $shell.CreateShortcut($shortcutPath); "
        "$shortcut.TargetPath = $env:SSHD_SHORTCUT_TARGET; "
        "$shortcut.WorkingDirectory = $env:SSHD_SHORTCUT_DIRECTORY; "
        "$shortcut.IconLocation = $env:SSHD_SHORTCUT_TARGET + ',0'; "
        "$shortcut.Save(); [Console]::WriteLine($shortcutPath)"
    )
    environment = os.environ.copy()
    environment["SSHD_SHORTCUT_TARGET"] = str(target.resolve())
    environment["SSHD_SHORTCUT_DIRECTORY"] = str(delivery.resolve())
    result = subprocess.run(
        [powershell, "-NoProfile", "-NonInteractive", "-Command", script],
        check=True,
        capture_output=True,
        text=True,
        env=environment,
    )
    shortcut_path = result.stdout.strip()
    if not shortcut_path:
        raise OSError("PowerShell did not report the created desktop shortcut path.")
    return Path(shortcut_path)
