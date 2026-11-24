"""Reject common game/firmware artifacts from tracked repository content."""

from __future__ import annotations

import subprocess
import sys
from pathlib import Path


FORBIDDEN_SUFFIXES = {
    ".bin", ".dat", ".edat", ".elf", ".iso", ".pup", ".psarc", ".prx",
    ".sav", ".sdat", ".self", ".sprx",
}


def main() -> int:
    try:
        output = subprocess.check_output(["git", "ls-files", "-z"])
    except (OSError, subprocess.CalledProcessError) as error:
        print(f"error: cannot inspect Git index: {error}", file=sys.stderr)
        return 1

    offenders = [
        Path(item.decode("utf-8", errors="replace"))
        for item in output.split(b"\0") if item
        if Path(item.decode("utf-8", errors="replace")).suffix.lower() in FORBIDDEN_SUFFIXES
    ]
    if offenders:
        for path in offenders:
            print(f"error: possible game/firmware artifact is tracked: {path}", file=sys.stderr)
        return 1

    print("repository hygiene: no known game/firmware artifact extensions are tracked")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
