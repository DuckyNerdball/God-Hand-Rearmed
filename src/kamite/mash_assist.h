#pragma once

class PS2Runtime;

// Mash Assistic ([Game] MashAssist, OPTIONS > Controls > Controller, off by default, live): the game's mash and waggle
// events take held input. While Gene struggles (an enemy's grab, his own stun mash-out), holding any face button counts
// as 7 presses a second (4 or 5 game frames apart) and holding the left stick off centre at all as a stick flick every fourth game frame; in the
// mash moves (Pummel, Spanking, Stinger, Cobra Twist, the spin and the rest of the eleven), holding Circle (B) does.
//
// How: a hook just before each of those states' read of the pressed-buttons word (GameWork +0x08) adds the press to the
// word, on the game frames it counts. Nothing else is sent: the struggle reads what a fast human would have pressed,
// and every other frame and state is untouched. Research and the sites: the notes' claude/mash-assist.md.
namespace kamite::mash_assist
{
    // Before the game starts: the hooks and the GHR.ini value (KAMITE_MASH_ASSIST).
    void install(PS2Runtime &runtime);

    bool enabled();
    // Live (the OPTIONS screen): the next read follows; keeps the variable in step for test cases.
    void setEnabled(bool on);
}
