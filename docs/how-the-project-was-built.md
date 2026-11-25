# How Project Stardust was built

This is a reproducibility and provenance guide to the source project. It explains the design,
conversion/build pipeline, recorded reference material, tools, dependencies, testing, release contents,
and privacy boundaries. It describes evidence present in this repository at `v0.1.0-alpha.2`; it does
not claim access to undocumented conversations, private machines, or work before the repository's
initial commit.

## 1. What the project is

Project Stardust is a title-specific recompilation of **Super Stardust HD for PlayStation 3,
title ID NPEA00014**. It is not a PlayStation emulator and does not download or provide the game.
Python tools analyze supported user-provided executable data and translate PowerPC (PPU) and SPU
machine code into generated C. A native C/C++ runtime supplies the host implementation of the PS3
services and graphics/audio facilities needed by the title. CMake combines the handwritten runtime,
generated code, and permitted libraries into a Windows executable.

The supported game executable is **NPEA00014 game update 6.00**; the required PS3 firmware is **4.93**.
These are separate version requirements. `config/target.json` pins the exact executable by size and
SHA-256 and pins the firmware-derived components. A build or a
successful boot only demonstrates that particular run; it does not establish a complete playthrough
or full rendering compatibility. The gameplay hang reported for the alpha release remains unresolved.

## 2. What is known about project provenance

The repository has one configured Git remote, `origin`, at
<https://github.com/thelastant/project-stardust>. Its recorded root history starts with commit
`12f3229` (`Initial Super Stardust HD Windows project`), which is already a large source snapshot.
The Git history does not record the private work, prompts, local commands, or development sessions
that preceded that snapshot. Consequently, this file distinguishes **verified references in the
source** from any broader claim about what a person may have read or used.

### Recorded technical references and related repositories

| Reference | How the repository uses or cites it | What it does not mean |
| --- | --- | --- |
| [RPCS3](https://github.com/RPCS3/rpcs3) | Named in runtime, renderer, input, system-service and emitter documentation as a behavioral/reference implementation. The `config/nids.json` and `config/lv2_syscalls.json` name tables record their RPCS3 source commit. Two RPCS3 crypto source files are fetched at the commit and hashes pinned in `cmake/rpcs3_keys.cmake`; the decryptor reads key constants as data. | RPCS3 is not linked into this project. The two crypto files are not compiled or redistributed. Specific subsystems document their own independently derived behavior and exceptions; follow those subsystem notes rather than assuming code was copied. |
| [DAL-Rinne-Utopia-recomp](https://github.com/yuias/DAL-Rinne-Utopia-recomp) | `docs/analysis-tools.md` points to its `tools/install-ghidra-cell.ps1` as an example of installing the CellPPU language module in Ghidra. | It is not a build dependency or a source of this game's translated code. |
| [AMD FidelityFX-FSR](https://github.com/GPUOpen-Effects/FidelityFX-FSR) | `THIRD_PARTY_NOTICES.md` identifies three retained FSR sample/shader files and their MIT license. | Those files are groundwork only; FSR is not built, used at runtime, or offered as a setting. |
| IBM / Sony / Toshiba SPU manuals | `docs/spu.md` cites the public SPU ISA and assembly-language specifications for instruction and channel semantics. | The copyrighted manuals are references, not bundled project files. |
| The PowerPC architecture and RPCS3 semantic handlers | Emitter comments and generated semantic tests identify instruction behavior references. Test descriptions label architectural or RPCS3 cases; the source should be consulted for each operator. | A reference citation is not a claim that an entire external implementation was copied. |
| Ghidra | Optional analysis workflow for independent disassembly/export comparison, documented in `docs/analysis-tools.md`. | Ghidra and its JDK are not required for ordinary building or running the committed recompiler. |

### Reverse-engineering tools and MCP services

The project owner reports that the development workflow also used **IDA Pro**, an **IDA Pro MCP
server**, and **REA (Reverse Engineering Assistant) through MCP**, alongside the repository's
documented Ghidra and Python analysis tools. IDA Pro supplied interactive binary analysis; its MCP
server made that analysis available to the AI-assisted workflow; REA supplied additional
reverse-engineering assistance. These are development/research tools, not build or runtime
dependencies.

The source tree does not preserve IDA databases, MCP server versions/configuration, tool-call
transcripts, or a mapping from an individual MCP result to a specific analysis record. Therefore the
tool use above is owner-reported context, not independently reproducible provenance. Do not infer
specific IDA/REA findings or claim a particular database row came from a particular service unless
that evidence is separately recorded.

The project also contains open-source dependency repositories. They are implementation dependencies,
not evidence that their projects were used as design guides. Their exact source revisions are in the
Git submodule gitlinks listed below.

## 3. Conversion and build process

The supported Windows entry point is `run.cmd`, which invokes `tools/build_game.py`. That builder
accepts the user's decrypted `NPEA00014` game update 6.00 `USRDIR\EBOOT.ELF` and firmware 4.93
`PS3UPDAT.PUP`; the
builder validates and stages local copies and does not modify the original files.

The high-level sequence is:

1. **Validate the inputs.** `tools/build_support.py` checks the expected executable name/format and
   firmware package. `config/target.json` pins the supported executable and extracted outputs, so a
   different title/revision is rejected rather than silently translated.
2. **Find the title directory.** Given an EBOOT under `NPEA00014\USRDIR`, the builder identifies that
   same user's NPEA00014 root. The staged player files go in ignored local output, not tracked source.
3. **Fetch/verify build tools.** `tools/fetch-toolchain.ps1`, `tools/fetch-ffmpeg.ps1`, and
   `tools/fetch-gameinput.ps1` download pinned assets and check their recorded hashes. Builds can use
   local archives with supported `-From` options.
4. **Extract and decrypt local inputs.** `sshd_decrypt` reads the user's EBOOT/PUP and the pinned
   RPCS3 key constants to produce ignored files under `generated/inputs/`. The firmware PRX is
   relocated to the configured guest address; the Japanese system font is extracted for the save
   dialog. The key material and extracted files are not checked into Git.
5. **Translate PPU and SPU code locally.** `tools/ps3tool.py emit` translates the EBOOT and relocated
   `libsre` module using the committed analysis databases. `spu-emit` translates the registered SPU
   images. Generated C/C++ and intermediate analyses live under ignored `generated/`.
6. **Configure and compile.** CMake builds the handwritten runtime and generated trees using Clang,
   LLVM's resource compiler, Ninja, the Windows SDK and Vulkan SDK. The game target is created only
   when the generated code inputs are supplied. The build also creates test binaries and stages the
   needed runtime libraries and notices.
7. **Test and assemble.** The builder runs CTest, assembles a clean portable player directory with the
   game data, libraries, configuration, logs/saves locations, and then creates a desktop shortcut.
   See `tools/build_game.py`, `tools/build_support.py`, `docs/building.md` and `docs/tests.md` for
   command-level details.

### User artwork

When the user's NPEA00014 folder contains `ICON0.PNG` and/or `PIC0.PNG`, `prepare_game_artwork` copies
them only to ignored local output. It makes a Windows `.ico` from the user's icon (preserving aspect
ratio and resizing images over 256 pixels), generates a local resource script, and the Windows build
compiles it with `llvm-rc`. The final executable therefore contains the `IDI_ICON1` icon and
`PIC0_ARTWORK` data resource when those source images are present. The shortcut points to the final
game executable and uses its embedded icon. Missing artwork is optional and does not get substituted
with a repository copy.

## 4. Tools and host requirements

### Required for the supported Windows build

| Tool/component | Purpose and recorded version/source |
| --- | --- |
| Windows x64 | Supported host for the command-line release builder. |
| Visual Studio 2026 C++ tools and Windows SDK | MSVC/Windows SDK headers, libraries and CRT environment. The PowerShell build script enters the Visual Studio developer shell using `vswhere` when needed. |
| LunarG Vulkan SDK | Supplies `shaderc.hpp` and `shaderc_combined.lib`; it is installed by the user and is not included in the project archive. |
| PowerShell 7 | Build/fetch scripts, Windows development-shell setup, artwork resizing, and shortcut creation. |
| Git | Repository and submodule checkout. |
| Python 3.10 or newer | Conversion, validation, resource staging, and Python tests. No Python package manifest or project-specific pip dependency file is present; the scripts use the standard library. |
| LLVM/Clang 23.1.6 | Pinned compiler/toolchain in `tools/toolchain.json`; C, C++, `llvm-rc`, and related LLVM tools. Asset SHA-256 is checked by the fetch script. |
| CMake 4.5.1 | Pinned build generator/configuration tool. |
| Ninja 1.13.2 | Pinned build executor. |
| CTest | Distributed with CMake; executes registered native tests. |
| GitHub Actions | CI runs `.github/workflows/python-tests.yml` on Windows: repository hygiene and Python tests. CI does not run a full gameplay playthrough. |

The build also uses the Visual Studio Installer's `vswhere.exe`, Windows PowerShell/System.Drawing for
Windows presentation tasks, and Windows SDK `rc.exe` for other resource work. Embedded artwork is
compiled with LLVM `llvm-rc` because the Clang RC wrapper's preprocessing is incompatible with the
Windows resource compiler's line-marker parsing.

### Other supported/optional development environments

- **macOS:** Apple Silicon, macOS 14.4 or newer, Homebrew LLVM/Clang, CMake, Ninja, Homebrew FFmpeg
  and LunarG Vulkan SDK. The macOS build uses system GameController.framework and CoreAudio and has
  separate CMake presets and `tools/build.sh`; this is a separate platform workflow from the Windows
  release builder.
- **Ghidra 12.x and JDK 21:** optional `ghidra-export` cross-check; not needed by `recover` or build.
- **A sparse RPCS3 checkout:** optional `nidgen` utility input; it is not needed for normal builds.
- **WSL and a C toolchain:** optional SPU objdump cross-check only.
- **NVIDIA Streamline SDK:** optional Windows DLAA support. The standard release builder configures
  with this feature off; the SDK is not part of the repository or alpha release.
- **A user's installed GameInput Redistributable:** optional runtime dependency for controller input
  on Windows. It is not bundled; keyboard input remains available if GameInput is absent.

The command-level prerequisites and platform options are maintained in `docs/building.md`.

### Repository tool inventory

These are project-maintained tools, as distinct from the compilers/libraries they call:

| Path | Role |
| --- | --- |
| `run.cmd`, `tools/build_game.py`, `tools/build_support.py` | User entry point, build orchestration, input validation, local game staging, title-artwork resources, portable output assembly, and desktop shortcut. |
| `tools/build.ps1`, `tools/build.sh` | Windows/macOS CMake configure, compile and optional CTest wrappers. |
| `tools/fetch-toolchain.ps1`, `tools/toolchain.json` | Download, verify and prepare pinned LLVM/Clang, CMake and Ninja. |
| `tools/fetch-ffmpeg.ps1`, `tools/fetch-ffmpeg.sh` | Fetch and verify the Windows FFmpeg package. |
| `tools/fetch-gameinput.ps1` | Fetch and verify the Windows GameInput development package. |
| `tools/fetch-streamline.ps1` | Optional Streamline SDK fetch and verification. |
| `tools/fetch-ffmpeg-ps5.ps1`, `tools/fetch-gameinput-ps5.ps1` | Separate optional fetch/inspection utilities; not part of the normal player build sequence. |
| `tools/ps3tool.py`, `tools/ps3recomp/` | Target inspection, recovery/analysis, PPU and SPU translation, import/name processing, disassembly helpers and associated test support. |
| `tools/pgo.ps1` | Optional LLVM profile-generation/use workflow. |
| `tools/build-spu-objdump.sh` | Optional build of a reference SPU objdump for decoder comparison. |
| `tools/check_repository_hygiene.py` | CI guard against known game/firmware artifact extensions in tracked files. |
| `tools/tests/` | Python unit/integration tests for the project tools, input validation, parsing, recovery and translation. |
| `cmake/`, `CMakeLists.txt`, `CMakePresets.json` | Target graph, platform/compiler checks, generated-code integration, runtime libraries, external fetch pins and tests. |

LLVM itself includes many executables; the project's build uses the C/C++ compiler, linker/archive
tools, resource compiler (`llvm-rc`), and selected analysis/profile tools. This inventory describes
the programs this project invokes, not every binary shipped inside the LLVM distribution.

## 5. Dependency inventory

### Source submodules

The Git superproject pins each source submodule by the following exact commit. Clone with
`git clone --recurse-submodules ...` or run `git submodule update --init --recursive`; do not replace
these with moving branch heads when reproducing a build.

| Path | Upstream repository | Exact commit pinned by this project | Main purpose |
| --- | --- | --- | --- |
| `externals/Vulkan-Headers` | [KhronosGroup/Vulkan-Headers](https://github.com/KhronosGroup/Vulkan-Headers) | `d277a0a3cda9c682708c93279f0c72cd6b0ae3f3` | Vulkan API headers |
| `externals/VulkanMemoryAllocator` | [GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator](https://github.com/GPUOpen-LibrariesAndSDKs/VulkanMemoryAllocator) | `0f1a757da579831283413295cc7f3b47eb521c77` | Vulkan image/buffer allocation |
| `externals/imgui` | [ocornut/imgui](https://github.com/ocornut/imgui) | `f5befd2d29e66809cd1110a152e375a7f1981f06` | Debug overlay UI |
| `externals/libpng` | [pnggroup/libpng](https://github.com/pnggroup/libpng) | `c0bb00592f6a734e2a2dff92d659b0aa95ca1d44` | PNG decode/encode support |
| `externals/spdlog` | [gabime/spdlog](https://github.com/gabime/spdlog) | `47584c1d8106f06560cf9f7558b64f3b5e5b88e7` | Host logging |
| `externals/volk` | [zeux/volk](https://github.com/zeux/volk) | `653b2bfd1142df0f00f921eb2c1ae9943092340a` | Vulkan function loading |
| `externals/xxHash` | [Cyan4973/xxHash](https://github.com/Cyan4973/xxHash) | `2ec67caeb18ec15ae60c0853174331089acddfc0` | RSX texture validation hashes |
| `externals/yyjson` | [ibireme/yyjson](https://github.com/ibireme/yyjson) | `be9bd74bb3c96e00006c2075c330a6b8b90123dc` | JSON parsing/serialization |
| `externals/zlib` | [madler/zlib](https://github.com/madler/zlib) | `617e19a81f38c15ff04de9f86882f42ad8a8ffd1` | SELF decompression and libpng support |

Each row is a Git submodule, not an independently selected package-manager version. Per-file license
terms and additional source/binary-only dependencies are in `THIRD_PARTY_NOTICES.md`.

### Pinned downloaded components and external SDKs

| Component | Pin in the repository | Use / redistribution boundary |
| --- | --- | --- |
| LLVM/Clang | 23.1.6; asset SHA-256 in `tools/toolchain.json` | Build tool, fetched locally; not part of game output or source ZIP. |
| CMake | 4.5.1; asset SHA-256 in `tools/toolchain.json` | Build tool, fetched locally. |
| Ninja | 1.13.2; asset SHA-256 in `tools/toolchain.json` | Build tool, fetched locally. |
| FFmpeg Windows shared build | BtbN tag `autobuild-2026-11-24-12-41`, FFmpeg `n9.0.2-963-g4d831360c9`, archive SHA-256 `471961ba38074215ffb4e94e3d90ead94886d59e67b24b7bf057d6b80adf9fe0` | Runtime movie/audio decoding; fetched into ignored `generated/deps`, its runtime DLLs/notices are staged beside local executable. LGPL build; details and its own bundled components are in `THIRD_PARTY_NOTICES.md`. |
| Microsoft GameInput development package | NuGet `Microsoft.GameInput` 3.5.283, package SHA-256 `b5988cb8ff9d7009b6ddf6ad4e3ff87e91b00cc17ffcd5d628dabc21c208f100` | Header/static import support used to build Windows input backend. The redistributable runtime is installed by the user and is not bundled. |
| NVIDIA Streamline | 2.14.1, archive SHA-256 `92c4d954631a1710da86ca3fa8d5034f2b9503838c95fc4ae977ae149319781b` | Optional Windows DLAA SDK, ignored local download, disabled in standard source-release build. |
| RPCS3 key source files | RPCS3 commit `f9b8c209a3800c1865bcfeade611f65f5237138c`; `key_vault.h` and `.cpp` each have SHA-256 pins in `cmake/rpcs3_keys.cmake` | Read only as key constants by the local decryptor. No key source is committed or redistributed. |
| BIZ UDPGothic fallback font | Commit `18934af56b9c003ca58c54bffbf226848cb11032`; font and OFL SHA-256 pins in `cmake/ui_font.cmake` | Optional fallback when firmware font is unavailable; fetched locally with its license. |
| LunarG Vulkan SDK | User-installed; no SDK archive pin | Supplies shader compiler headers/library and Vulkan drivers/loaders. It is not redistributed. |
| Visual Studio / Windows SDK | User-installed; no installer pin in this repository | Host compiler support libraries, Windows SDK headers and system libraries. |
| macOS system frameworks | OS-provided | GameController, CoreAudio, AppKit and CommonCrypto are used by the macOS build; not downloaded or bundled. |
| macOS Homebrew FFmpeg/LLVM | User-installed and not pinned by this Windows release | macOS development/runtime dependency. See the macOS-specific build and FFmpeg notes. |

`THIRD_PARTY_NOTICES.md` is the authoritative place for licenses, notices, exact DLL behavior,
transitive FFmpeg components, and source/binary redistribution conditions. Component pins are
deliberately repeated here to make the build inventory discoverable; update both locations together.

## 6. Analysis inputs and data boundaries

Committed `config/` files are project analysis metadata (target pins, function/import names, PPU
seeds/overrides, and SPU image/function records). They let the recompiler reproduce the supported
translation without committing the commercial executable or its generated translation.

The local build reads user-provided game/firmware files and writes derived data under ignored paths,
principally `generated/`, `build/`, `ProjectStardust/`, and local toolchain folders. Depending on
selected operations, local-only products include decrypted ELF/PRX/font inputs, generated C, Ghidra
projects/exports, logs, shader/cache data, saves, copied artwork and final executable.

The builder does not upload these files. GitHub release artifacts contain source and permitted
third-party source submodules only; they do not contain the game, firmware, keys, generated code,
artwork, saves, logs, or a playable executable. See `docs/privacy.md` and `docs/getting-files.md`.

## 7. Tests, CI, and what the results prove

- `.github/workflows/python-tests.yml` runs on Windows and checks repository hygiene, then runs
  `python -B -m unittest discover -s tools/tests -t tools`.
- Native C/C++ tests are registered with CTest. `docs/tests.md` lists every test and its fixtures,
  runtime requirements, and skip conditions.
- The `game_boot` test needs user-provided/generated game and firmware inputs; it is not run by the
  source-only GitHub CI job.
- The Windows executable build and input-core test were verified locally for the alpha.2 work. Local
  Python suite result was 1,276 tests, 56 skipped. This is not an end-to-end gameplay certification.
- The reported run stopped flipping frames after gameplay had begun. Its census is a stall diagnostic,
  not a root-cause report; the no-frame-flips hang has not yet been diagnosed or fixed.

Do not infer gameplay stability from a green Python CI check, successful compilation, embedded
resources, or a boot test. The current release remains an experimental source-and-builder prerelease.

## 8. Development and release chronology

The visible root commits record these project changes:

| Commit | Recorded change |
| --- | --- |
| `12f3229` | Initial source project snapshot. |
| `2d3ce70` | Prepared source-only prerelease alpha.1. |
| `b97ad2e` | Fixed Windows CI path comparisons and added user-owned game-artwork staging/embedding. |
| `b5c2c3e` | Compiled artwork resources with LLVM `llvm-rc`; added artwork dimensions/resource test coverage. |
| `ea80966` | Limited single-player input to one controller at a time, with tests and documentation. |
| `2a0ce7f` | Prepared source-only prerelease alpha.2 and recorded the unresolved gameplay hang. |

The alpha.2 archive is
[`Project Stardust 0.1.0-alpha.2`](https://github.com/thelastant/project-stardust/releases/tag/v0.1.0-alpha.2).
Its uploaded ZIP digest is published on the release and in `SHA256SUMS.txt`. It is not a replacement
for the user's local build and contains no game binary.

The repository records source release version `VERSION`, the release notes in `CHANGELOG.md`, and
the tagged Git commit. The alpha.2 publication used Git/tagging and GitHub CLI (`gh release create`)
to attach the source archive and its SHA-256 manifest as a prerelease. This release-management step
is separate from the player's build and does not upload local game or firmware data.

## 9. Issue history: symptoms, investigation, fixes, and open work

The repository does not contain a complete private development diary or an export of GitHub Issues.
This ledger records the issues and resolutions recoverable from the current documentation, tests,
changelog, and visible commits. It is not a claim that no other issues were encountered. The
initial commit is a large source snapshot, so pre-snapshot issue chronology and tool-call details
cannot be reconstructed here.

| Issue / symptom | Investigation and action recorded | Resolution / evidence |
| --- | --- | --- |
| **Target codebase was much larger than the inherited game's.** The second target has roughly 3.3 million PPU words and 29,000 functions, versus about 210,000 words and 2,500 functions for the earlier target. | Adjusted emitter output chunking to 256 files to keep translation/compilation tractable; documented lowering build parallelism if the compiler runs out of memory. Fix emitter/header assumptions rather than hand-edit generated files. | Scaling approach and memory workaround documented in `docs/porting-guide.md`; not a runtime bug. |
| **Switch recovery missed tables in functions using multiple TOC bases**, producing unresolved branches and, at runtime, unknown dispatch targets/small-address faults. | Recovery now collects TOC bases from function descriptors, tries the alternatives at each switch site, and prefers the owning function's descriptor TOC where results disagree. Recovery starts are fed back into analysis and the function database is committed. | `docs/porting-guide.md` records the correction. The later unknown target at `0x0092d1a4` from `0x0092d178` was identified as a recovery gap and the recorded run progressed past it. |
| **SPU `bnusCore` thread initialization rejected thread number 1** in a group with one thread. | Corrected the validation to use the lv2 per-group thread table (256 slots), following the documented RPCS3 behavior, rather than comparing the index to the active thread count. | Subsequent run logs show thread 1 bound, started and waiting for its event; documented in `docs/spu.md`. |
| **SPU indirect dispatch fault at LS `0x07900`**, reached from `0x0dae4`. | Traced the target through a function-pointer table populated from static data records. Extended SPU function recovery to treat data-segment words pointing at function starts as entries. | `0x7900` became dispatchable and the documented 300-second run had zero unknown SPU targets. The indirect site remains unresolved statically but uses runtime dispatch. |
| **Boot test did not guarantee sufficient progress.** Earlier milestones stopped at an early syscall or first SPU group start. | Moved the `game_boot` milestone to the main thread's first `cellSpursEventFlagTryWait`, after an SPU LZMA batch. The documented procedure checks runs both with an empty HDD directory and existing installed data. | Stronger regression milestone in `cmake/game_boot.cmake` and `docs/tests.md`; explicitly not a gameplay or compatibility test. |
| **Freeze/no flips when leaving a level**, reported with `frame_limit=120`. | `docs/troubleshooting.md` reports the same run stable at 60 or vsync and recommends setting the saved `frame_limit` to 60. The teardown/frame-limit/SPURS path is implicated by observations. | 60/vsync is a workaround for that observed case. It is not evidence that the underlying issue or the separately reported alpha gameplay hang is fixed. |
| **Spurious `The SPURS is aborted.` during teardown** while every SPURS kernel thread is idle in an event wait. | Runtime records thread states and call chain, recognizes the all-kernels-idle condition, and ignores that abort so it does not kill the session. | Narrow false-abort case handled. Non-idle kernels and stopped-by-error SPU threads remain fatal; this is not a general SPURS implementation. |
| **Reported gameplay hang / frame loop stops flipping.** | Runtime stall detection, census, watchdog and thread-wait diagnostics provide evidence for investigation. Alpha.2 explicitly records the report as not root-caused and not fixed. | **Open.** Diagnostics are not a solution. No claim of a fix is made in this guide. |
| **Raw SPU thread groups and SPU `stop 0x120` unsupported.** | Limitation is explicitly listed in `docs/spu.md`; no implemented remedy is recorded. | **Open limitation.** |
| **SPURS instance B for ATRAC3plus gets a group/thread but is not started.** | For the current path, host-side ATRAC decoding makes that SPU group unnecessary. | Current-path accommodation, not an implementation of that SPU execution path. |
| **SPU reservation race window** around a snapshot comparison and 8-byte update. | The race window and its implications are documented; it remains listed as open. | **Open limitation; no fix recorded.** |
| **Windows CI path comparisons failed** because of path representation differences. | Commit `b97ad2e` records the Windows CI comparison fix and associated test/documentation changes. | Addressed in that commit; not evidence about unrelated Windows issues. |
| **Artwork resource compilation failed with Clang's RC wrapper** due to incompatible preprocessing/line-marker handling. | Commit `b5c2c3e` switched artwork resources to LLVM `llvm-rc` and expanded resource tests. | Addressed in that commit. |
| **Several controllers could be merged into one player's input.** | Commit `ea80966` changed selection so one controller is used for the single player and additional controllers are ignored; input tests/docs were updated. | Addressed for single-player selection in that commit. |
| **Risk of shipping game data/artwork or obsolete inherited build/installer material.** | Alpha.1 commit `2d3ce70` removed the obsolete GUI installer/packaging workflow and inherited target-specific artifacts/tooling; introduced a source-only builder and privacy/input documentation. Artwork is now staged only from the user's local game files. | Source-only release boundary and repository hygiene checks recorded; user game/firmware remain local. |
| **Save dialog may lack the firmware Japanese font.** | Added an optional, hash-pinned BIZ UDPGothic fallback under its OFL license; firmware font remains preferred. | Fallback implemented; network failure is non-fatal. |
| **Build setup/dependency download failures.** | Added pinned SHA-256-checked fetchers, local archive paths, offline build mode, explicit SDK prerequisites and troubleshooting guidance. | Mitigations exist; external network and user-installed SDK availability remain outside the repository's control. |
| **Ghidra CellPPU decoding/export setup can be incomplete or wrong-language.** | Documented installing the CellPPU module, selecting the language, running decoder probes, and using the Cell language where generic PPC truncates Cell-only instructions. | Analysis workflow has checks and setup guidance in `docs/analysis-tools.md`; Ghidra remains optional. |
| **Large builds can exhaust compiler memory.** | Reduce `CMAKE_BUILD_PARALLEL_LEVEL` (for example, to 4) and retain `generated/` so translation need not be repeated. | Documented operational workaround in `docs/troubleshooting.md`; actual available memory remains host-dependent. |

For additional symptoms and exact log strings, see `docs/troubleshooting.md`. For unsupported runtime
behavior, consult `docs/runtime.md`, `docs/spu.md`, `docs/rendering.md`, and `docs/status.md`. The
visible root history contains six project commits before the documentation update; it cannot support
a more detailed chronological account of undocumented issues or fixes.

## 10. Where to find the detailed material

- `README.md` — quick start and supported inputs.
- `docs/getting-files.md` / `docs/privacy.md` — user-owned input requirements and handling.
- `docs/building.md` — exact Windows/macOS build commands, flags, downloaded tools, and local outputs.
- `docs/analysis-tools.md`, `docs/emitter.md`, `docs/ppu.md`, `docs/spu.md` — recovery, translation,
  validation, and reference conventions.
- `docs/runtime.md`, `docs/rendering.md`, `docs/input.md`, `docs/audio.md` — runtime behavior and
  known differences/limits.
- `docs/tests.md`, `docs/status.md`, `docs/release-criteria.md` — what has and has not been verified.
- `docs/layout.md` — source tree map.
- `THIRD_PARTY_NOTICES.md` — licenses, component notices, exact downloads and redistribution terms.
