# Privacy

The setup is designed for bring-your-own-game input:

- The game folder and firmware file are processed on the computer running setup. No upload step is
  implemented.
- The game executable is hashed locally and compared with the NPEA00014 pin. Firmware contents are
  parsed and checked locally by `sshd_decrypt`.
- Decrypted inputs, emitted code, build outputs, caches and local configuration are written under
  ignored local folders such as `generated/` and `build/`.
- Setup fetches pinned third-party build dependencies and the pinned RPCS3 source files used by the
  decrypt tool. It does not send game or firmware bytes to those services.
- Setup output may contain local file paths. Do not post logs publicly without reviewing them.

The project does not include telemetry. Network access is used for dependency downloads and is not
used to transfer user game files. If you need an offline build, follow the offline options in
[Building](building.md).
