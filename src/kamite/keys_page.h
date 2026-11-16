#pragma once

// CONTROLS > BUTTON MAPPING with the keyboard as the device in use (K, Nov 16): the keyboard & mouse page. Two columns of
// rows, each the action's key or mouse button (the prompt pack's key art) and its name, in the BUTTON MAPPING page's
// look (its panels and underscores, the CONTROLS title). Select a row, confirm, press the key: it becomes the action's
// key (ps2_host_input::controls::requestKeyBind; a key another action had is swapped). No game state here, so the
// preview tool (tools/options/overlay_preview.py --page keys) builds the page the game shows.

#include "kamite/options_overlay.h"

#include <deque>
#include <functional>
#include <map>
#include <string>
#include <vector>

namespace kamite
{
    class IniFile;
}

namespace kamite::keys_page
{
    // The rows that depend on options.
    struct Options
    {
        bool clickStickDash = false; // [Game] L3Dash: L3 dashes
        bool modern = true;          // [Game] KbmControlStyle=modern: R3 switches brawling / free camera
        bool presets = false;        // [Game] StyleSwitching: the d-pad keys switch styles
    };
    Options optionsFrom(const IniFile &ini);

    // A row: its controls.ini [Pad1] key and its words (the BUTTON MAPPING page's short names).
    struct RowDef
    {
        const char *key = "";
        const char *text = "";
        int arrow = 0;      // 1: ATTACK's down arrow after the text (the cross), 2: the up arrow (the triangle)
        bool fixed = false; // not rebindable here (MENU: Esc is the capture's cancel)
    };
    std::vector<RowDef> rows(const Options &options);
    // Rows in the left column (the rest are on the right).
    int leftCount(int rowCount);

    // What is armed (stage 3's pattern): the capture waiting for a key, or Reset waiting for its second press.
    enum class Mode : uint8_t
    {
        Browse,
        Capture,
        ResetArmed
    };

    struct PageData
    {
        std::deque<std::string> text;
        std::vector<options_overlay::KeysRow> rows;
        options_overlay::KeysView view;
        std::vector<RowDef> defs;

        const char *keep(std::string value)
        {
            text.push_back(std::move(value));
            return text.back().c_str();
        }
    };
    // labels: each key's first keyboard / mouse binding as prompt_art's compact label ("Spc", "M1"; "" = none), by
    // controls.ini key. art: the prompt pack's image for a label ("" = none: the label is written instead). artDir:
    // resources/options/mapping (the panel and underscores). cursor: a row (-1 none). resetArt / confirmArt / cancelArt:
    // the hints' key images.
    void build(PageData &d, const std::map<std::string, std::string> &labels,
               const std::function<std::string(const std::string &)> &art, const std::string &artDir, const Options &options,
               int cursor, Mode mode, const std::string &resetArt);
    // The cursor's next row: up / down within its column, left / right across to the same height.
    int move(const PageData &d, int cursor, int dx, int dy);

    constexpr const char *kHintRebind = "Rebind", *kHintBack = "Back", *kHintReset = "Reset", *kHintCancel = "Cancel";
}
