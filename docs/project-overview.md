# Project overview

## Purpose

Project Stardust is an open-source, title-specific recompilation project. It translates the supported
PowerPC and SPU code for **Super Stardust HD (NPEA00014)** into native C/C++, then links that output
to a Windows runtime which implements the PlayStation 3 services the title needs.

This is not an emulator, a general PS3-to-PC converter, or a game download. It supports only the
executable and firmware versions pinned in `config/target.json`. It is experimental; a passing build
or boot test is not proof of a complete playthrough or full graphical compatibility.

## What the source release contains

- Recompiler and local input-validation tools in `tools/`.
- Runtime, renderer, audio, input, and system-service implementation in `runtime/`.
- Title-specific analysis databases and build configuration in `config/`.
- CMake build scripts, tests, documentation, and the open-source third-party submodules.
- License and attribution information in `LICENSE` and `THIRD_PARTY_NOTICES.md`.

The project code is licensed under ISC; third-party components retain their own licenses. Review
their notices before modifying or redistributing them. The source ZIP includes open-source source
dependencies and license notices, not downloaded toolchain packages, firmware key sources, or
vendor runtimes. Optional NVIDIA Streamline/DLSS support is disabled in the standard release build
and its SDK is neither open source nor included.

## What is deliberately not included

The repository and source release do not contain the commercial game executable, game data, PS3
firmware, cryptographic keys, extracted or generated game code, saves, logs, or PS3 title artwork.
Those files are not open source and must be supplied locally by each user. The build tools validate
and process the selected files on the user's computer.

The title and product names are used only to state compatibility. Project Stardust is unofficial and
is not affiliated with or endorsed by Sony. This project documentation is not legal advice and
cannot determine whether a particular user's acquisition or use is lawful.

## How a user builds

1. Clone the repository and its source submodules.
2. Install the Windows development prerequisites listed in [Building](building.md).
3. Run `run.cmd`, then select a matching decrypted `EBOOT.ELF` and PS3 firmware 4.93
   `PS3UPDAT.PUP` from files the user is entitled to use.
4. The scripts validate inputs, fetch pinned build dependencies, generate the title code locally,
   build and run the tests, and write the local player folder under ignored `ProjectStardust/`.

No user game or firmware data is sent to the project or uploaded to GitHub. See
[Getting your files](getting-files.md), [Privacy](privacy.md), and [Troubleshooting](troubleshooting.md)
for input details, network behavior, and known limitations.

## Alpha release contents

`ProjectStardust-0.1.0-alpha.1.zip` is a portable **source-and-builder** archive. It contains the
open-source project and required source submodules, not a ready-to-play game executable. The game
must be built locally from user-provided inputs. Generated executables are intentionally not attached
to the public release because they contain translated game code.

See [Status](status.md) and [Release criteria](release-criteria.md) for verification scope and
remaining work.
