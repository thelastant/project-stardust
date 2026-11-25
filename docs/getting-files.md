# Getting your files

This project supports only **Super Stardust HD, title ID NPEA00014, game update 6.00**, with the
decrypted executable matching the `eboot_elf` pin in `config/target.json`. This game version is
separate from the required **PS3 system software 4.93** below.

## Required inputs

1. Your own decrypted `EBOOT.ELF` from **NPEA00014 game update 6.00**. It can be selected from
   anywhere on your computer. The builder verifies its file size and SHA-256 against the project
   pin; the game update number alone is not sufficient if the executable differs.
2. PlayStation 3 system software **4.93** as `PS3UPDAT.PUP` (it can also be anywhere).

Use files derived from your own game dump and firmware source. This project does not provide game or
firmware downloads. Any game version other than update 6.00, a different region/title ID, a modified
executable, or another firmware version is not accepted.

## Where to select them

Run `run.cmd` from the cloned project folder. Enter the paths to `EBOOT.ELF` and `PS3UPDAT.PUP` when
prompted, or pass both paths as command-line arguments. The builder checks the ELF's size and
SHA-256 locally before building. The decrypt tool checks the firmware version, extracts the required
firmware files and validates their target pins.

For reference, the pinned decrypted EBOOT is 8,251,176 bytes with SHA-256
`e6a286c20571264388a5ad93c42903de9b9a00c42595a4f4334c1441bfce89a1`. The corresponding encrypted
EBOOT.BIN pin is recorded in `config/target.json` as well.

The selected files are read locally and are never transmitted. The builder stages a local copy under
the ignored `ProjectStardust/NPEA00014` folder; the source files are left unchanged. Emitted outputs
are written under ignored `generated/` folders. The PC build is placed in the ignored
`ProjectStardust/` folder, and local saves remain there across rebuilds. None of these local game,
firmware, generated, or save files belong in a GitHub upload.

When the selected executable is located at `NPEA00014/USRDIR/EBOOT.ELF`, the builder also looks for
`ICON0.PNG` and `PIC0.PNG` in the containing `NPEA00014` folder. If found, these files are copied
locally and embedded into the produced Windows executable; its icon is also used for the desktop
shortcut. Missing artwork is optional and the build uses its default icon. The images remain local
and are excluded from the repository and source archive.

See [Privacy](privacy.md) for downloads and local logs, and [Building](building.md) for the manual
command sequence.
