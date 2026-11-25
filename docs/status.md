# Status

Super Stardust HD (NPEA00014) is an experimental, title-specific recompilation. The repository
contains the target's recompiler inputs and a native runtime, but compatibility is not complete.

The existing development notes report successful runs beyond startup and into gameplay. Those
results have not been reproduced from this clean project copy, and a full playthrough has not been
verified. Graphics fidelity remains an active area of work. Treat a successful build or boot as
evidence only for that run, not as a compatibility guarantee.

The command-line builder was verified on Windows with Visual Studio 2026 and firmware 4.93. The
52-test CTest run passed, including `game_boot`; two optional SPU LZMA tests were skipped because
their external fixture was unavailable. A full playthrough and graphics fidelity have not been
verified.
