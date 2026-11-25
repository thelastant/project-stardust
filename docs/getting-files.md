# Getting your files

This project supports only **Super Stardust HD, title ID NPEA00014**, with the already-updated
decrypted executable matching the `eboot_elf` pin in `config/target.json`.

## Required inputs

1. Your own already-updated decrypted `EBOOT.ELF`. It can be selected from anywhere on your
   computer. It must match the update version pinned by this project.
2. PlayStation 3 system software **4.93** as `PS3UPDAT.PUP` (it can also be anywhere).

Use files derived from your own game dump and firmware source. This project does not provide game or
firmware downloads. A game update, a different region/title ID, a modified executable, or another
firmware version is not accepted.

## Where to select them

Run `run.cmd` from the cloned project folder. Enter the paths to `EBOOT.ELF` and `PS3UPDAT.PUP` when
prompted, or pass both paths as command-line arguments. The builder checks the ELF's size and
SHA-256 locally before building. The decrypt tool checks the firmware version, extracts the required
firmware files and validates their target pins.

The selected files are read locally and are never transmitted. The builder stages a local copy under
the ignored `ProjectStardust/NPEA00014` folder; the source files are left unchanged. Emitted outputs
are written under ignored `generated/` folders. The PC build is placed in the ignored
`ProjectStardust/` folder, and local saves remain there across rebuilds. None of these local game,
firmware, generated, or save files belong in a GitHub upload.

See [Privacy](privacy.md) for downloads and local logs, and [Building](building.md) for the manual
command sequence.
