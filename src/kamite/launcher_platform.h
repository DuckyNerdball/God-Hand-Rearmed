#pragma once

// The launcher's operating-system pieces (kept out of launcher.cpp: <windows.h> and raylib clash).
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <vector>

namespace kamite::launcher::platform
{
    // A file (folder = false: disc images) or folder picker over the window. Empty if cancelled or not available.
    std::filesystem::path pick(void *window, bool folder);
    // A mod's zip (Install from zip on the Mods tab). Empty if cancelled or not available.
    std::filesystem::path pickZip(void *window);
    // Opens a file or folder with the system's handler (the controls file in a text editor).
    void open(const std::filesystem::path &path);
    // Shift held right now (the launcher shows even when hidden).
    bool shiftHeld();
    // An HTTPS GET into `out` (redirects followed): true on status 200 with at most maxBytes. `received` (if given)
    // counts the bytes as they come. Blocks: run it on a thread. False on platforms without it.
    bool httpGet(const std::string &url, std::vector<uint8_t> &out, size_t maxBytes, const char *accept = nullptr,
                 std::atomic<uint64_t> *received = nullptr);
    // Starts the program again (the given executable, from its folder, no arguments); the caller then quits.
    bool relaunch(const std::filesystem::path &exe);
    // The running program's own executable.
    std::filesystem::path thisExe();
}
