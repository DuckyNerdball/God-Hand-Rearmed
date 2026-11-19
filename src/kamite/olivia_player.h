#pragma once

class PS2Runtime;

// Playable Olivia (pre-alpha, testers only: claude/playable-olivia-handoff.md). [Mods] Character=olivia
// (KAMITE_CHARACTER), Costume=dress|bunny (KAMITE_COSTUME): at boot, after body_detail (her body as it rebuilt it, so
// Money / Cash know her), her player files are made from the player's disc (olivia_player_build.h) and served in place
// of all of Gene's: his body files (every costume's) and his coat files (an empty coat). Gene's moves, God Hand and
// voice; what covers ground at her height. The camera looks at her at her height (its look point's height, cPlCamera
// +0x404, times her ratio; the camera's distance settings are the player's).
namespace kamite::olivia_player
{
    void install(PS2Runtime &runtime);
    bool active();
    float ratio(); // her height / Gene's (1 when she isn't playing)
}
