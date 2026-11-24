# Getting your files

This project supports only **Super Stardust HD, title ID NPEA00014**, with the base executable
matching the pin in `config/target.json`.

## Required inputs

1. A game folder containing:
   `PS3_GAME/USRDIR/EBOOT.BIN`
2. PlayStation 3 system software **4.93** as `PS3UPDAT.PUP`.

Use files from your own dump and firmware source. This project does not provide game or firmware
downloads. A game update, a different region/title ID, a modified executable, or another firmware
version is not accepted.

## Where to select them

Run `python tools/setup.py` from the cloned project folder. Select the directory containing
`PS3_GAME` (not `PS3_GAME` itself), then select `PS3UPDAT.PUP`. The setup hashes the EBOOT locally
before starting the build. The decrypt tool then checks the firmware version, extracts the required
firmware files and validates their target pins.

The selected files are read in place; setup does not copy them into the repository or transmit them
to the project. Decrypted and emitted outputs are written under the ignored `generated/` directory.
The resulting game configuration points to the selected game folder on that computer.

See [Privacy](privacy.md) for downloads and local logs, and [Building](building.md) for the manual
command sequence.
