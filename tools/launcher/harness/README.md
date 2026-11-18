# Launcher harness (Linux, no GPU)

Runs the real launcher (src/kamite/launcher.cpp and the saloon) on Linux under Xvfb + Mesa's software GL, with the
game's other modules stubbed (`stubs.cpp`), and drives it with xdotool for screenshots. For checking launcher work in
the workspace; GHR.exe itself is built on Windows as always.

Setup once: `apt-get install xvfb xdotool imagemagick libxrandr-dev libxinerama-dev libxcursor-dev libxi-dev libgl-dev`;
clone raylib 5.5 to /tmp/rl (built with CMake to /tmp/rlb), imgui v1.92.7-docking to /tmp/im, rlImGui Raylib_5_5 to
/tmp/rli, the libsdl-org dav1d / libavif forks (the commits in src/CMakeLists.txt) built static to /tmp/avt/b; stub
headers in /tmp/stub/kamite (git_commit.h, default_ini.h). Make /tmp/lh/exe/resources with links to the repo's
resources folders, a fake.iso and a GHR.ini with `[Launcher] PresetPicked=1`.

`build.sh` builds /tmp/lh/launcher; `drive.sh` (W, H) clicks and keys through Home, the snacks, the zoom, Options and
Play, shooting to /tmp/lh/shots. Hold clicks and keys ~0.25 s (the launcher polls input once a frame; software GL
draws about 11 frames a second). Env: LH_W / LH_H window, LH_NOISO (not installed), LH_UPDATE (an update is out).
Snack lines and menu sounds read the extracted disc from /mnt/user-data/uploads/Claude/God Hand Extract.
