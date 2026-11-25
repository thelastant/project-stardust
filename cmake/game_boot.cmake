# A headless game has no physical controls, so provide repeat Start presses to leave the title screen.
# Pin the first gameplay particle SPU image: this verifies boot reached the scene's SPU work, not just Vulkan
# startup or title-screen flips. With the input script it appears at about 15 s. Keep the 30 s watchdog below
# the known PPU jump-table gap later in the run.
set(SSHD_GAME_BOOT_PAD_SCRIPT "${CMAKE_CURRENT_BINARY_DIR}/game_boot_press_start.txt")
file(WRITE "${SSHD_GAME_BOOT_PAD_SCRIPT}" "1.0 tap start every 2.0 until 120.0\n")
set(SSHD_GAME_BOOT_ARGS
  "--no-config --no-window --seconds 30 --hdd-dir \"${CMAKE_BINARY_DIR}/hdd\" --pad keyboard --pad-script \"${SSHD_GAME_BOOT_PAD_SCRIPT}\"")
set(SSHD_GAME_BOOT_MILESTONE "first image eboot_ps3_spu_particle")
