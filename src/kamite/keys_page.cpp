#include "kamite/keys_page.h"

#include "kamite/ini_file.h"
#include "kamite/settings_table.h"

#include <algorithm>

namespace kamite::keys_page
{
    Options optionsFrom(const IniFile &ini)
    {
        Options o;
        const auto on = [&](const char *id) {
            const settings::Setting *s = settings::find(id);
            return s && settings::on(ini, *s);
        };
        o.clickStickDash = on("L3Dash");
        o.presets = on("StyleSwitching");
        if (const settings::Setting *s = settings::find("KbmControlStyle"))
            o.modern = settings::value(ini, *s) == "modern";
        return o;
    }

    std::vector<RowDef> rows(const Options &options)
    {
        // The BUTTON MAPPING page's short names; port actions only while their option is on (as there).
        std::vector<RowDef> out{
            {"LUp", "MOVE FORWARD"},
            {"LDown", "MOVE BACK"},
            {"LLeft", "MOVE LEFT"},
            {"LRight", "MOVE RIGHT"},
            {"DodgeModifier", "DODGE (HOLD + MOVE)"},
        };
        if (options.clickStickDash)
            out.push_back({"L3", "DASH"});
        out.insert(out.end(), {{"Cross", "ATTACK", 1}, {"Square", "COMBO"}, {"Triangle", "ATTACK", 2}, {"Circle", "ACTION"},
                               {"L1", "180\xC2\xB0 TURN"}, {"R1", "ROULETTE"}, {"L2", "TAUNT"}, {"R2", "UNLEASH"}});
        if (options.modern)
            out.push_back({"R3", "BRAWL / FREECAM"});
        if (options.presets)
            out.insert(out.end(), {{"Up", "STYLE S"}, {"Right", "STYLE E"}, {"Down", "STYLE X"}, {"Left", "STYLE Y"}});
        out.push_back({"Start", "MENU", 0, true}); // Esc: the capture's cancel, so not rebound here
        return out;
    }

    int leftCount(int rowCount)
    {
        return (std::max(rowCount, 0) + 1) / 2;
    }

    void build(PageData &d, const std::map<std::string, std::string> &labels,
               const std::function<std::string(const std::string &)> &art, const std::string &artDir, const Options &options,
               int cursor, Mode mode, const std::string &resetArt)
    {
        d.text.clear();
        d.rows.clear();
        d.defs = rows(options);
        for (const RowDef &def : d.defs)
        {
            options_overlay::KeysRow row;
            const auto found = labels.find(def.key);
            const std::string label = found != labels.end() ? found->second : std::string();
            const std::string picture = label.empty() || !art ? std::string() : art(label);
            row.icon = d.keep(picture);
            row.keyText = picture.empty() ? d.keep(label) : "";
            row.text = def.text;
            row.fixed = def.fixed;
            if (def.arrow)
            {
                const std::string stem = artDir + (def.arrow == 1 ? "/arrow_down" : "/arrow_up");
                row.arrow = d.keep(stem + ".png");
                row.arrowLit = d.keep(stem + "_lit.png");
            }
            d.rows.push_back(row);
        }
        options_overlay::KeysView &v = d.view;
        v = options_overlay::KeysView{};
        v.underscore = d.keep(artDir + "/underscore.png");
        v.underscoreLit = d.keep(artDir + "/underscore_lit.png");
        v.panel = d.keep(artDir + "/panel.png");
        v.rows = d.rows.data();
        v.rowCount = static_cast<int>(d.rows.size());
        v.leftCount = leftCount(v.rowCount);
        v.cursor = cursor >= 0 && cursor < v.rowCount ? cursor : -1;
        v.capture = mode == Mode::Capture && v.cursor >= 0;
        v.resetArt = d.keep(resetArt);
        v.resetWord = kHintReset;
        if (v.capture)
        {
            const RowDef &def = d.defs[static_cast<size_t>(v.cursor)];
            std::string name = def.text;
            if (def.arrow)
                name += def.arrow == 1 ? " (DOWN)" : " (UP)";
            v.notice = d.keep("Press a key or mouse button for " + name + ".\nEsc cancels.");
            v.confirmWord = "";
            v.cancelWord = "";
            v.resetWord = "";
            v.resetArt = "";
        }
        else if (mode == Mode::ResetArmed)
        {
            v.notice = "Press \x01 again to reset the keyboard and mouse.";
            v.noticeIcons[0] = v.resetArt;
            v.confirmWord = kHintRebind;
            v.cancelWord = kHintBack;
        }
        else
        {
            v.confirmWord = kHintRebind;
            v.cancelWord = kHintBack;
        }
    }

    int move(const PageData &d, int cursor, int dx, int dy)
    {
        const int n = d.view.rowCount, nl = d.view.leftCount;
        if (n <= 0)
            return -1;
        if (cursor < 0 || cursor >= n)
            return 0;
        const bool right = cursor >= nl;
        const int first = right ? nl : 0, last = right ? n - 1 : nl - 1;
        if (dy != 0)
        {
            // Round within the column (the OPTIONS pages go round too).
            int next = cursor + dy;
            if (next < first)
                next = last;
            else if (next > last)
                next = first;
            return next;
        }
        if (dx != 0)
        {
            const int at = cursor - first;
            if (dx > 0 && !right && n > nl)
                return std::min(nl + at, n - 1);
            if (dx < 0 && right)
                return std::min(at, nl - 1);
        }
        return cursor;
    }
}
