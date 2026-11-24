# Release criteria

Do not label a release as tested until all applicable checks below pass from a clean checkout.

- The target manifest, PPU database, relocated libsre database, SPU image registry and per-image
  records all agree on the pinned NPEA00014 executable and firmware 4.93.
- A clean Git clone initializes every dependency at a recorded commit. Dependency fetch scripts
  verify their SHA-256 pins.
- No game executable, firmware, key, save, generated C, cache, local path or other user-supplied game
  content appears in tracked files or release archives.
- The setup validates bad inputs before building, rejects other title/firmware pins, reports each
  step, runs the local build and tests, and offers to launch the result.
- The setup and manual build are exercised on a supported Windows x64 system using user-supplied
  files. Record the exact toolchain and test results; do not claim a full playthrough without one.
- License notices are present and the target-specific analysis/recompiler material has been reviewed
  before public distribution.
- Release archives contain only the project and permitted dependencies. Publish SHA-256 checksums,
  privacy notes, supported-input requirements, status and troubleshooting guidance.

The source-only state of this repository has not yet passed the native Windows build and game-run
checks; see [Status](status.md).
