// The README's generated parts (gen_readme.py): every setting and what each preset sets it to, as JSON on stdout.
// Built by gen_readme.py from src/kamite/settings_table.cpp, presets.cpp and ini_file.cpp; not part of the game.
#include "kamite/ini_file.h"
#include "kamite/presets.h"
#include "kamite/settings_table.h"

#include <cstdio>
#include <string>

using namespace kamite;

namespace
{
    void str(const char *text)
    {
        std::putchar('"');
        for (const char *p = text ? text : ""; *p; ++p)
        {
            const unsigned char c = static_cast<unsigned char>(*p);
            if (c == '"' || c == '\\')
                std::printf("\\%c", c);
            else if (c == '\n')
                std::printf("\\n");
            else if (c < 0x20)
                std::printf("\\u%04x", c);
            else
                std::putchar(c);
        }
        std::putchar('"');
    }
}

int main()
{
    IniFile presetIni[presets::kCount];
    for (int p = 0; p < presets::kCount; ++p)
        presets::write(presetIni[p], p);
    std::printf("{\"presets\":[");
    for (int p = 0; p < presets::kCount; ++p)
    {
        std::printf(p ? "," : "");
        std::printf("{\"name\":"), str(presets::kText[p].name);
        std::printf(",\"tagline\":"), str(presets::kText[p].tagline);
        std::printf(",\"band\":"), str(presets::kText[p].band), std::putchar('}');
    }
    std::printf("],\"settings\":[");
    for (size_t i = 0; i < settings::kSettingCount; ++i)
    {
        const settings::Setting &s = settings::kSettings[i];
        std::printf(i ? ",\n{" : "\n{");
        std::printf("\"id\":"), str(s.id);
        std::printf(",\"tab\":"), str(settings::tabName(s.tab));
        std::printf(",\"category\":"), str(s.category);
        std::printf(",\"name\":"), str(s.name);
        std::printf(",\"description\":"), str(s.description);
        std::printf(",\"note\":"), str(s.note);
        std::printf(",\"kind\":%d", static_cast<int>(s.kind));
        std::printf(",\"fallback\":"), str(s.fallback);
        std::printf(",\"unit\":"), str(s.unit);
        std::printf(",\"parent\":"), str(s.parent);
        std::printf(",\"parentValue\":"), str(s.parentValue);
        std::printf(",\"live\":%s", s.live ? "true" : "false");
        std::printf(",\"device\":%d", static_cast<int>(s.device));
        std::printf(",\"controlled\":%s", presets::controls(s) ? "true" : "false");
        std::printf(",\"choices\":[");
        for (int c = 0; c < s.choiceCount; ++c)
        {
            std::printf(c ? ",[" : "[");
            str(s.choices[c].value), std::putchar(','), str(s.choices[c].label), std::putchar(']');
        }
        std::printf("],\"values\":[");
        for (int v = 0; v < s.valueCount; ++v)
            std::printf(v ? ",%d" : "%d", s.values[v]);
        std::printf("],\"preset\":[");
        for (int p = 0; p < presets::kCount; ++p)
        {
            std::printf(p ? "," : "");
            str(settings::value(presetIni[p], s).c_str());
        }
        std::printf("]}");
    }
    std::printf("]}\n");
    return 0;
}
