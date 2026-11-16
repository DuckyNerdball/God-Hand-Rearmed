#pragma once

// OPTIONS > PRESETS (K, Nov 16; claude/presets-design.md): four ready-made sets of options, each including the one
// before it (Classic, Remastered, Rearmed, Sleazy Mode). Data and GHR.ini only, shared by the launcher and the game:
// which preset the settings match (any change to an option a preset sets makes them Custom), a preset written, the
// presets' text, and their photos (the pause menu's Olivia pictures, decoded from the player's disc).

#include <cstdint>
#include <string>
#include <vector>

namespace kamite
{
    class IniFile;
}

namespace kamite::settings
{
    struct Setting;
}

namespace kamite::presets
{
    enum : int
    {
        kClassic,
        kRemastered,
        kRearmed,
        kSleazy,
        kCount,
        kCustom = -1, // the settings match none of them
    };

    struct Text
    {
        const char *title;   // the in-game carousel's and the posters' ("SLEAZY MODE")
        const char *name;    // the launcher's ("Sleazy Mode")
        const char *tagline; // one line under the photo
        const char *band;    // the help band's text (two lines at most: the overlay shrinks it to fit)
        int photo;           // id/photo/orivia_tex0NN.tm2
    };
    extern const Text kText[kCount];
    extern const Text kCustomText; // in game only

    const Text &text(int preset); // kCustom: kCustomText

    // The preset the settings match exactly, or kCustom.
    int current(const IniFile &ini);
    // Writes the preset's values (not saved). Returns the settings it changed, for the caller to apply live; Intro
    // Skipper / Hard-On take effect at the next start.
    std::vector<const settings::Setting *> write(IniFile &ini, int preset);
    // Whether the settings a preset sets include this one (the options a change of which makes them Custom).
    bool controls(const settings::Setting &setting);

    // The photo's disc path ("id/photo/orivia_tex013.tm2").
    std::string photoPath(int photo);
    // The pause menu's photo from its TIM2 (512 x 256, 8 bits, CSM1 CLUT): the picture (x 0..295, y 13..216) as RGBA8
    // (opaque). False when the file is not that.
    bool decodePhoto(const std::vector<uint8_t> &tm2, int &width, int &height, std::vector<uint8_t> &rgba);
    inline constexpr float kPhotoAspect = 295.0f / 203.0f;
}
