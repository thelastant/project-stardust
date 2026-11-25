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

Choose your decrypted, already-updated file named `EBOOT.ELF`; its folder does not matter. It must
match the project's supported update version and NPEA00014 executable pin. Other updates, regions,
and modified executables are unsupported.

## Firmware validation fails

The required firmware is version 4.93. The decrypt tool checks the PUP structure and the extracted
firmware component hashes. A different version, damaged file, or incomplete download stops setup.

## The game freezes or stops when leaving a level

Two different failures have been seen on the same teardown path:

- **Freeze**: the frame loop stops (`hang: no flips for 10 s` in the log) with every guest thread
  idle. Seen with `frame_limit` set to `120`; at `60` (or `vsync`) the same run has been stable.
  Set `"frame_limit": "60"` in `config.json` next to the executable, or pick 60 in the game's
  options, and retry.
- **Spurious SPURS abort**: libsre prints `The SPURS is aborted.` while every SPURS kernel thread
  is idle in its event wait. The runtime logs the thread states, the call chain and a
  `SPURS was shut down with every kernel idle` line, then ignores that abort so the session is not
  killed. A real SPU fault (a kernel not idle, or a stopped-by-error thread) is still fatal.

Both are known limitations of the SPURS teardown/frame-limit path, not of your files.

## Dependency download or submodule initialization fails

The command-line build fetches its pinned dependencies: submodules, the toolchain from
`tools/toolchain.json`, FFmpeg and GameInput. Check that the network and proxy allow
`github.com`, `api.nuget.org` and `raw.githubusercontent.com`, then rerun the build. A verified
component already on disk is not downloaded again. See
[What the command-line builder downloads](building.md#what-the-command-line-builder-downloads). Do not bypass a
failed integrity check by substituting an unverified file.

## GameInput installation is cancelled or fails

The build fetches the pinned GameInput package and links its static loader. Install the runtime from the
package's `redist/GameInputRedist.msi` if controller input does not work; the build itself does not
need administrator rights.

## The compiler runs out of memory

The PPU output is split into 256 files. Lower `CMAKE_BUILD_PARALLEL_LEVEL` before rerunning the build;
for example, set it to `4` in the terminal that launches `run.cmd`. Keep the `generated/` directory so
the emitted code does not need to be regenerated.

## Setup stops with a command error

Read the last command and its output in the command prompt. The manual equivalent of each build step
is documented in [Building](building.md). Share logs only after removing local paths.

## The build succeeds but the game does not run correctly

This is experimental software and a successful build does not guarantee full compatibility. Check
[Status](status.md), preserve the local build logs, and report the scene and exact failure without
attaching game or firmware files.
