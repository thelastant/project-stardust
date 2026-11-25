# Project Stardust — Super Stardust HD Recompilation

Project Stardust is an open-source, title-specific recompilation project for **Super Stardust HD on
PlayStation 3 (NPEA00014)**. Its tools translate the supported game's PowerPC and SPU code to native
code and link it with a Windows runtime. It is not an emulator, a general PS3 converter, or a game
download.

**Current release: [`0.1.0-alpha.2`](https://github.com/thelastant/project-stardust/releases/tag/v0.1.0-alpha.2)
— source and builder only.** The ZIP does not contain a playable game binary. Users supply their own
supported game and firmware files and build locally. The reported gameplay hang remains unresolved;
a complete playthrough has not been verified.

## Build from Command Prompt

Use the command-prompt script. It asks only for the decrypted `EBOOT.ELF` and PS3 firmware **4.93**
`PS3UPDAT.PUP`. You can paste or drag paths into the prompts, or pass both paths as arguments:

```bat
run.cmd "C:\path\to\EBOOT.ELF" "C:\path\to\PS3UPDAT.PUP"
```

Install the Windows requirements first: Visual Studio 2026 with the C++ tools and Windows SDK,
LunarG Vulkan SDK, Python 3.10+, Git, and PowerShell 7. Clang, CMake, Ninja, and pinned build
dependencies are fetched or verified as needed. The command checks the inputs, recompiles the game,
builds and tests the PC version, and puts it in `ProjectStardust\`:

   ```
    ProjectStardust\
      SUPER STARDUST HD.exe     the game
      *.dll                     runtime libraries loaded beside the executable
      Play.cmd                  starts the game
      config.json               portable paths and your saved settings
      NPEA00014\USRDIR\...      your game data
      data\libsre.reloc.elf     firmware module
      saves\                    save data
      cache\shaders\            shader cache
      fonts\  licenses\         UI font and dependency notices
      Logs\run.log              the game's log (written on every launch)
    ```

The final executable is `SUPER STARDUST HD.exe`. If `ICON0.PNG` and/or `PIC0.PNG` are present beside
the selected game's `NPEA00014` folder, the builder copies them into the local game folder and embeds
them in the executable. The desktop shortcut uses the executable's icon. These title assets are
never included in the repository or source ZIP.

Build intermediates stay under `build\setup-release\`; generated conversion files stay under
`generated\`. After a successful build, use the desktop shortcut to start the game. Your original
game and firmware files are never moved or modified. In-game settings can be changed from the game's
F1 menu. Saves and existing settings are retained when rebuilding.

The default build fetches pinned toolchain, FFmpeg, GameInput development files, and (when no local
key source is configured) the pinned firmware key-source files. Each download is verified by
SHA-256 and stays in ignored local folders. Optional NVIDIA Streamline/DLSS support is off in the
standard CLI build and is not included in the release archive.

Your original game and firmware files remain where they are and are never uploaded. A local game-data
copy and generated build files stay in ignored project folders.

For the exact input-file requirements and privacy details, see [Getting your files](docs/getting-files.md)
and [Privacy](docs/privacy.md). If the build stops, read the displayed command output and
[Troubleshooting](docs/troubleshooting.md). You can
also follow the [manual Windows build steps](docs/building.md#windows).

## Supported inputs

- The decrypted `EBOOT.ELF` already updated to the version supported by this project. Keep it with
  your own game files; the command stages a local copy into `ProjectStardust\NPEA00014\`.
- PlayStation 3 system software **4.93**, supplied as `PS3UPDAT.PUP`
- Windows x64 development environment: Visual Studio 2026 with the C++ tools and Windows SDK,
  LunarG Vulkan SDK, Python 3.10+, Git, and PowerShell 7. LLVM/Clang, CMake and Ninja
  come from `tools/fetch-toolchain.ps1`. See [Windows requirements](docs/building.md#windows).

The base executable is checked against the target pin. Firmware contents and extracted components
are validated by the project's decrypt tool. Other game IDs, executable updates, and firmware
versions are not supported.

## Project status

This is experimental software. A successful local run is not evidence of a complete playthrough,
and graphics fidelity and compatibility may have known limitations. Review [Status](docs/status.md).
See [Project overview](docs/project-overview.md) for the architecture, source contents, licensing,
and what the source release deliberately excludes.

## Purpose, licensing, and privacy

This open-source project contains a title-specific recompiler, runtime, build tools, tests, and
documentation. It recompiles one supported Super Stardust HD executable into native PC code; it is
not a general-purpose PS3 converter. Project code is under the ISC license in `LICENSE`. Third-party
source and dependency terms are listed in `THIRD_PARTY_NOTICES.md`; each component retains its own
license. No third-party vendor SDK or runtime is included in the source release.

The repository and source ZIP do not include game executables, firmware, cryptographic keys, saves,
generated game code, or PlayStation artwork. Users must provide their own lawful game and firmware
inputs. The title name is used only to identify compatibility. This is an unofficial project, not
affiliated with or endorsed by Sony. This notice is not legal advice or a guarantee that all uses are
lawful in every jurisdiction.

The builder processes selected paths locally. Build tools may download pinned third-party
dependencies; they do not send selected game or firmware files to this project's authors.
Review [Third-party notices](THIRD_PARTY_NOTICES.md) for dependency terms.

## Development

See [Project overview](docs/project-overview.md), [Building](docs/building.md), [Tests](docs/tests.md),
and `AGENTS.md` for developer instructions.
See [Release criteria](docs/release-criteria.md) for the project's verification policy.
