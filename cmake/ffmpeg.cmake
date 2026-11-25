# Pin of the FFmpeg build fetched by tools/fetch-ffmpeg.ps1 (LGPL, shared, 9.0 release branch).
# The script reads these three values by regex, so keep each one on its own `set(...)` line.
# BtbN may delete old autobuild releases; keep a copy of the zip and pass it with -From. When
# changing the pin, update THIRD_PARTY_NOTICES.md as well.
set(SSHD_FFMPEG_RELEASE_TAG "autobuild-2026-11-24-12-41")
set(SSHD_FFMPEG_ASSET "ffmpeg-n9.0.2-963-g4d831360c9-win64-lgpl-shared-9.0.zip")
set(SSHD_FFMPEG_SHA256 "471961ba38074215ffb4e94e3d90ead94886d59e67b24b7bf057d6b80adf9fe0")
