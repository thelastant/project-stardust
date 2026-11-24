# Troubleshooting

## Setup says a prerequisite is missing

Install the Windows prerequisites listed in [Building](building.md#windows), including PowerShell 7,
Visual Studio 2026 with the Windows SDK, LLVM/Clang, CMake 3.29 or newer, Ninja, Git, and the Vulkan
SDK. Install the `VSSetup` PowerShell module for the current user:

```powershell
Install-Module VSSetup -Scope CurrentUser
```

Restart setup after installing or changing `PATH` or `VULKAN_SDK`.

## The EBOOT pin does not match

Choose the base-game `EBOOT.BIN` under `PS3_GAME/USRDIR`. Updates, another region/title ID, and
modified executables are unsupported. Setup compares its SHA-256 with the target manifest before
running the build.

## Firmware validation fails

The required firmware is version 4.93. The decrypt tool checks the PUP structure and the extracted
firmware component hashes. A different version, damaged file, or incomplete download stops setup.

## Dependency download or submodule initialization fails

Check the network connection and retry. The project pins dependency revisions and download hashes;
do not bypass a failed integrity check by substituting an unverified file. When using a source
archive rather than `git clone`, the submodule contents may be missing; use a complete clone with
recursive submodule initialization.

## GameInput installation is cancelled or fails

Setup fetches the pinned GameInput package and launches its runtime installer after asking for
consent. Approve the Windows administrator prompt if you want controller support, then rerun setup.

## The compiler runs out of memory

The PPU output is split into 256 files. Lower `CMAKE_BUILD_PARALLEL_LEVEL` before rerunning setup;
for example, set it to `4` in the terminal that launches setup. Keep the `generated/` directory so
the emitted code does not need to be regenerated.

## Setup stops with a command error

Read the last command and its output in the setup window. The manual equivalent of each build step
is documented in [Building](building.md). Share logs only after removing local paths.

## The build succeeds but the game does not run correctly

This is experimental software and a successful build does not guarantee full compatibility. Check
[Status](status.md), preserve the local build logs, and report the scene and exact failure without
attaching game or firmware files.
