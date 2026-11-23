#pragma once

// The desktop's file chooser through xdg-desktop-portal (org.freedesktop.portal.FileChooser, OpenFile) on the session
// bus: the only picker a Flatpak has (zenity and kdialog aren't in its runtime), and a fallback outside one. Inside a
// Flatpak the chosen file comes back through the document portal (/run/user/<uid>/doc/<id>/<name>), exported for good,
// so the stored path still opens after a restart; a file the sandbox can already read comes back as its own path.
// libdbus-1 is opened at run time, as for the keep-awake (runtime/ps2xRuntime/src/lib/Kernel/host_keep_awake_linux.cpp).
#include <filesystem>
#include <string>
#include <vector>

namespace kamite::launcher::portal
{
    struct Filter
    {
        std::string name;               // "PS2 disc image (*.iso, *.chd)"
        std::vector<std::string> globs; // "*.iso", case-sensitive as the portal matches them
    };

    struct Request
    {
        std::string parentWindow; // "x11:<XID in hex>" (x11Parent), or "": the dialog has no parent
        std::string title;
        bool directory = false;   // a folder instead of a file (FileChooser version 3)
        std::vector<Filter> filters;
        std::string currentFolder;  // where the dialog opens ("": the desktop's choice)
        int replyTimeoutMs = 25000; // for OpenFile's own reply (not the player's choice): libdbus's default
    };

    enum class Outcome
    {
        Chosen,     // path holds it
        Cancelled,  // the player closed the dialog: don't offer another picker
        Unavailable // no bus, no portal, an error or a reply GHR can't read: error says which; try another picker
    };

    struct Result
    {
        Outcome outcome = Outcome::Unavailable;
        std::filesystem::path path;
        std::string error;
    };

    // Asks the portal and blocks until the player chooses, cancels, or the portal goes away. Linux only (elsewhere:
    // Unavailable). No bus is started for it: without $DBUS_SESSION_BUS_ADDRESS or $XDG_RUNTIME_DIR/bus, Unavailable.
    Result openFile(const Request &request);

    // The pure pieces, for the tests.
    // The object path the portal answers on: /org/freedesktop/portal/desktop/request/<sender>/<token>, the sender
    // being the caller's unique name without the ':' and with '.' as '_' (":1.42" -> "1_42").
    std::string requestPath(const std::string &uniqueName, const std::string &token);
    // A chosen file's path from the portal's file:// URI (percent-decoded; host empty or "localhost"). False for any
    // other scheme or host, a bad escape, a NUL, or a path that isn't absolute.
    bool pathFromUri(const std::string &uri, std::filesystem::path &out);
    // The parent_window string for an X11 window ("x11:1a00004"), "" for none (0).
    std::string x11Parent(unsigned long xid);
    // True inside a Flatpak sandbox (/.flatpak-info exists there and nowhere else).
    bool insideFlatpak();
}
