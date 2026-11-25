# Privacy

The command-line builder is designed for bring-your-own-game input:

- The game executable and firmware file are processed on the computer running the builder. No upload step is
  implemented.
- The game executable is hashed locally and compared with the NPEA00014 pin. Firmware contents are
  parsed and checked locally by `sshd_decrypt`.
- Decrypted inputs, emitted code, build outputs, caches and local configuration are written under
  ignored local folders such as `generated/` and `build/`.
- The builder downloads only pinned dependencies (open-source tools from
  `tools/toolchain.json`, FFmpeg and the Microsoft GameInput package, each verified by SHA-256) and,
  when no local copy is configured, the pinned firmware key-source files listed in
  `THIRD_PARTY_NOTICES.md`. It never uploads your game or firmware files. The separate manual fetch
  scripts only run when you start them.
- Build output may contain local file paths. Do not post logs publicly without reviewing them.

The project does not include telemetry. The builder and recompilation never send your game or
firmware files anywhere; only the pinned dependency downloads listed above contact the network.
