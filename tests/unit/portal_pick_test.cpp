// The file chooser portal's D-Bus side (src/kamite/portal_file_chooser.cpp), driven by tests/unit/portal_bus.py
// against a fake portal on a private bus:
//
//   portal_pick_test disc|folder|zip <parent_window> <current_folder> [reply_timeout_ms]
//
// asks once, with the launcher's titles and filters for that kind, and prints one line on stdout:
// "chosen <path>", "cancelled[ <why>]" or "unavailable <why>".
#include "kamite/portal_file_chooser.h"

#include <cstdio>
#include <cstdlib>
#include <string>

namespace portal = kamite::launcher::portal;

int main(int argc, char **argv)
{
    if (argc == 2 && std::string(argv[1]) == "missing-python")
    {
        // CTest's stand-in for portal_bus when CMake found no python3: a skip, or a failure where every test must run.
        const char *all = std::getenv("KAMITE_REQUIRE_ALL_TESTS");
        std::fprintf(stderr, "skipped: no python3 for portal_bus.py\n");
        return all && std::string(all) == "1" ? 1 : 77;
    }
    if (argc < 4)
    {
        std::fprintf(stderr, "usage: portal_pick_test disc|folder|zip <parent_window> <current_folder> [timeout_ms]\n");
        return 2;
    }
    const std::string kind = argv[1];
    portal::Request request;
    request.parentWindow = argv[2];
    request.currentFolder = argv[3];
    if (argc > 4)
        request.replyTimeoutMs = std::atoi(argv[4]);
    if (kind == "folder")
    {
        request.title = "Choose the texture pack folder (the one holding replacements)";
        request.directory = true;
    }
    else if (kind == "zip")
    {
        request.title = "Choose a mod's zip to install";
        request.filters = {{"Mod (*.zip)", {"*.zip", "*.ZIP"}}, {"All files", {"*"}}};
    }
    else
    {
        request.title = "Choose your disc image (USA, SLUS-21503)";
        request.filters = {{"PS2 disc image (*.iso, *.chd)", {"*.iso", "*.ISO", "*.chd", "*.CHD"}},
                           {"All files", {"*"}}};
    }
    const portal::Result result = portal::openFile(request);
    switch (result.outcome)
    {
    case portal::Outcome::Chosen:
        std::printf("chosen %s\n", result.path.string().c_str());
        break;
    case portal::Outcome::Cancelled:
        std::printf("cancelled%s%s\n", result.error.empty() ? "" : " ", result.error.c_str());
        break;
    case portal::Outcome::Unavailable:
        std::printf("unavailable %s\n", result.error.c_str());
        break;
    }
    return 0;
}
