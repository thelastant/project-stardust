# Super Stardust HD — Windows Recompilation

This project recompiles **Super Stardust HD for PlayStation 3 (NPEA00014)** into a native Windows
program. It is a title-specific recompilation, not a tool that converts arbitrary PS3 games.

## Local setup

The guided setup uses files you provide from your own copy of the game and PS3 system software. It
checks the supported game executable, extracts and validates the required firmware components,
emits the game's PPU/SPU recompilation locally, builds the program, and runs the available tests.

1. Install the prerequisites listed under [Windows requirements](docs/building.md#windows).
2. Clone this repository and open a terminal in its folder.
3. Run `python tools/setup.py`.
4. Select the folder containing `PS3_GAME` and your PS3 firmware **4.93** `PS3UPDAT.PUP`.
5. After setup succeeds, launch `build/release/bin/ssHD.exe`.

The guided setup fetches pinned open-source build dependencies and RPCS3 key-source files as
described in [Building](docs/building.md). It does not upload your game or firmware files.
Decrypted and generated files stay in ignored local build folders.

For the exact input-file requirements and privacy details, see [Getting your files](docs/getting-files.md)
and [Privacy](docs/privacy.md). If setup stops, read the displayed command output and
[Troubleshooting](docs/troubleshooting.md). You can
also follow the [manual Windows build steps](docs/building.md#windows).

## Supported inputs

- The base-game `EBOOT.BIN` from your own NPEA00014 dump:
  `<game folder>/PS3_GAME/USRDIR/EBOOT.BIN`
- PlayStation 3 system software **4.93**, supplied as `PS3UPDAT.PUP`
- Windows x64 development environment: Visual Studio 2026 with the Windows SDK, LLVM/Clang,
  CMake 3.29+, Ninja, Python 3.10+ with Tkinter, Git, Vulkan SDK, and PowerShell 7 with the
  `VSSetup` PowerShell module. See [Windows requirements](docs/building.md#windows).

The base executable is checked against the target pin. Firmware contents and extracted components
are validated by the project's decrypt tool. Other game IDs, executable updates, and firmware
versions are not supported.

## Project status

This is experimental software. A successful local run is not evidence of a complete playthrough,
and graphics fidelity and compatibility may have known limitations. Review [Status](docs/status.md).

## Legal and privacy

This repository is intended to contain recompilation source and build information, **not** game
content. It does not provide game executables, firmware, cryptographic keys, saves, or generated
game code. Obtain and use game and firmware files only where lawful. The project is unofficial and
is not affiliated with or endorsed by Sony.

Setup processes the paths you select locally. Build tools may download pinned third-party
dependencies; they do not send your selected game or firmware files to this project's authors.
Review [Third-party notices](THIRD_PARTY_NOTICES.md) for dependency terms.

## Development

See [Building](docs/building.md), [Tests](docs/tests.md), and `AGENTS.md` for developer instructions.
See [Release criteria](docs/release-criteria.md) for what remains before a tested public release.
