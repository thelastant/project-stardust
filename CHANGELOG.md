# Changelog

## Unreleased

- Clarify that the supported game executable is Super Stardust HD NPEA00014 update 6.00, separate
  from the required PS3 firmware 4.93, and document the pinned EBOOT hash.

## 0.1.0-alpha.2 — 2026-11-25

- Use the user's NPEA00014 `ICON0.PNG` and `PIC0.PNG` in the locally built executable; use its icon
  for the desktop shortcut. Keep user artwork out of the source repository and release archive.
- Restrict single-player input to one controller at a time; additional controllers are ignored rather
  than merged into player one's input.
- Fix Windows CI path comparisons and compile embedded artwork resources with LLVM's resource compiler.
- Update builder, input, and release documentation.

This is a source-and-builder release, not a prebuilt game. The reported gameplay hang is not fixed;
a complete playthrough has not been verified. Users must supply their own supported game executable
and firmware. See [Status](docs/status.md) and [Release criteria](docs/release-criteria.md).

## 0.1.0-alpha.1 — 2026-11-25

- Added a command-line builder for validating user-supplied NPEA00014 and firmware 4.93 inputs,
  recompiling the target locally, building, testing, and launching.
- Removed inherited target-specific save tooling, PPU overrides, UI controls, and stale SPU/Ghidra
  analysis records.
- Removed the obsolete GUI installer and packaging workflow.
- Removed bundled PlayStation artwork from the public source release.
- Added target-specific build, privacy, troubleshooting, and project-purpose documentation.
- Updated the recovery and emitter tests to match the pinned NPEA00014 databases.

This alpha is a source-and-builder release, not a prebuilt game. Users must supply their own
supported game executable and firmware. See [Status](docs/status.md) and
[Release criteria](docs/release-criteria.md).
