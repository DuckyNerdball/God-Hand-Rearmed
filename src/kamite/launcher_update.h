#pragma once

// The launcher's in-place updates (claude/release.md): at every start the GitHub releases are read (silently nothing
// when offline); a newer one shows "What's new" and "Update" above the volume. Update downloads the release zip,
// writes its program files next to the old ones, swaps them in (the running GHR.exe and its DLLs are renamed aside,
// removed at the next start) and starts the game again. Never touched: GHR.ini and controls.ini (only keys they lack
// are added), memory cards, save states, cases, logs, texture packs, mods, the disc image.
// Release builds check; a dev build (testers build main) only with [Launcher] UpdateTest=1, as version 0.0.0.
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <string>
#include <thread>

namespace kamite::launcher::update
{
    // "v0.6.9 Beta", or with the commit for a dev build.
    std::string versionLabel();

    struct Release
    {
        std::string tag;   // "v0.7.0"
        std::string notes; // every newer release's notes, newest first: "# v0.7.0" then its body's lines
        std::string zipUrl;
        uint64_t zipSize = 0;
    };

    enum class CheckState : int
    {
        Off,      // not checked (a dev build)
        Checking,
        UpToDate,
        Available,
        Failed, // offline, GitHub unreachable: shown as up to date
    };
    struct Checker
    {
        std::atomic<int> state{static_cast<int>(CheckState::Off)};
        Release release; // valid once state is Available
        std::thread worker;
    };
    // test: a dev build's [Launcher] UpdateTest (any release counts as newer).
    void startCheck(Checker &checker, bool test);

    enum class Stage : int
    {
        Downloading,
        Installing,
        Done,   // swapped in: start the game again
        Failed, // nothing changed; `error` says why
    };
    struct Installer
    {
        std::atomic<int> stage{static_cast<int>(Stage::Downloading)};
        std::atomic<uint64_t> received{0};
        uint64_t total = 0;
        std::string error;
        std::thread worker;
    };
    void startInstall(Installer &installer, const Release &release, const std::filesystem::path &exeDir);

    // The launcher's start: removes the old program files a finished update renamed aside (on a thread: the old
    // process may still be closing).
    void cleanup(const std::filesystem::path &exeDir);
}
