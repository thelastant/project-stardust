"""Build the Project Stardust PC version from a decrypted EBOOT.ELF and PS3UPDAT.PUP."""

from __future__ import annotations

import argparse
from pathlib import Path
import subprocess
import sys


ROOT = Path(__file__).resolve().parents[1]
if str(ROOT) not in sys.path:
    sys.path.insert(0, str(ROOT))

from tools import build_support  # noqa: E402


PRESET = "setup-release"


def input_path(prompt: str) -> Path:
    """Read a path from the console, accepting paths pasted with surrounding quotes."""
    value = input(prompt).strip().strip('"').strip("'").strip()
    return Path(value).expanduser()


def run_step(label: str, command: list[str]) -> None:
    print(f"\n[{label}]", flush=True)
    print("> " + subprocess.list2cmdline(command), flush=True)
    subprocess.run(command, cwd=ROOT, check=True)


def build(eboot: Path, pup: Path) -> tuple[Path, Path | None]:
    eboot = eboot.expanduser().resolve()
    pup = pup.expanduser().resolve()
    build_support.validate_eboot_elf(eboot)
    build_support.validate_pup(pup)

    game_dir = build_support.detect_game_dir(eboot)
    output = build_support.GAME_OUTPUT
    print(f"EBOOT.ELF: {eboot}")
    print(f"PS3UPDAT.PUP: {pup}")
    print(f"Game files: {game_dir}")
    print(f"PC version output: {build_support.DELIVERY}")
    print("\nThe source files will not be moved or modified.", flush=True)

    # Fetchers verify pinned dependencies and reuse their existing local copies.
    for script in (
        "tools/fetch-toolchain.ps1",
        "tools/fetch-ffmpeg.ps1",
        "tools/fetch-gameinput.ps1",
    ):
        run_step("Checking build dependencies", [
            "pwsh", "-NoProfile", "-File", script,
        ])

    print("\n[Preparing the game files]", flush=True)
    build_support.prepare_game_folder(game_dir, output, eboot)
    icon, pic = build_support.prepare_game_artwork(game_dir, output)
    if icon or pic:
        print("Using title artwork from the NPEA00014 game folder.")
    else:
        print("No ICON0.PNG or PIC0.PNG found in the NPEA00014 game folder; using the default icon.")

    run_step("Configuring the PC build", [
        "pwsh", "-NoProfile", "-File", "tools/build.ps1",
        "-Preset", PRESET,
        "-EbootElf", str(output / "USRDIR" / "EBOOT.ELF"),
        "-Pup", str(pup),
        "-GameDir", str(output),
    ])

    run_step("Converting the game executable", [
        sys.executable, "-B", "tools/ps3tool.py", "emit", str(eboot),
        "--chunks", "256", "--out", "generated/setup-ppu", "--max-unhandled", "0",
    ])
    run_step("Converting the firmware module", [
        sys.executable, "-B", "tools/ps3tool.py", "emit",
        "generated/inputs/libsre.reloc.elf",
        "--db", "config/libsre_functions.json",
        "--symbol-prefix", "libsre", "--chunks", "4",
        "--out", "generated/setup-ppu/libsre", "--max-unhandled", "0",
    ])
    run_step("Converting the firmware SPU code", [
        sys.executable, "-B", "tools/ps3tool.py", "spu-emit",
        "--eboot", str(eboot),
        "--libsre", "generated/inputs/libsre.reloc.elf",
        "--out", "generated/setup-ppu/spu", "--max-unhandled", "0",
    ])

    run_step("Building and testing the PC version", [
        "pwsh", "-NoProfile", "-File", "tools/build.ps1",
        "-Preset", PRESET,
        "-GeneratedDir", "generated/setup-ppu",
        "-EbootElf", str(output / "USRDIR" / "EBOOT.ELF"),
        "-Pup", str(pup),
        "-GameDir", str(output),
    ])
    run_step("Running tests", [
        "ctest", "--test-dir", f"build/{PRESET}", "--output-on-failure", "--parallel", "2",
    ])

    result = build_support.assemble_deliverable()
    shortcut_path = None
    try:
        shortcut = build_support.create_desktop_shortcut(result)
        shortcut_path = shortcut
        print(f"Desktop shortcut created: {shortcut}")
    except (OSError, subprocess.SubprocessError) as error:
        print(f"Warning: could not create the desktop shortcut: {error}", file=sys.stderr)
    return result, shortcut_path


def main() -> int:
    parser = argparse.ArgumentParser(
        description="Convert the supported decrypted EBOOT.ELF and PS3UPDAT.PUP into a Windows PC game."
    )
    parser.add_argument("eboot", nargs="?", help="path to the decrypted game EBOOT.ELF")
    parser.add_argument("pup", nargs="?", help="path to PS3UPDAT.PUP (firmware 4.93)")
    args = parser.parse_args()

    try:
        eboot = Path(args.eboot).expanduser() if args.eboot else input_path(
            "Path to decrypted EBOOT.ELF: "
        )
        pup = Path(args.pup).expanduser() if args.pup else input_path(
            "Path to PS3UPDAT.PUP (firmware 4.93): "
        )
        result, shortcut_path = build(eboot, pup)
    except (OSError, ValueError, subprocess.CalledProcessError) as error:
        print(f"\nBuild failed: {error}", file=sys.stderr)
        return getattr(error, "returncode", 1) or 1
    except KeyboardInterrupt:
        print("\nBuild cancelled.", file=sys.stderr)
        return 130

    print(f"\nDone. The PC version is in:\n{result}")
    if shortcut_path:
        print(f"Start the game from the desktop shortcut:\n{shortcut_path}")
    else:
        print("You can start the game by running 'SUPER STARDUST HD.exe' in that folder.")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
