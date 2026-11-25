# Changelog

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
