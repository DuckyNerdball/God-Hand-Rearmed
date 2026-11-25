// Mash Assistic: held input counts in the game's mash and waggle events (mash_assist.h).
//
// The two families (god-hand-decomp + the ELF; the notes' claude/mash-assist.md):
// - Struggles: the eleven enemy grab states on Gene (cEm00: escape counter +0x568 -= 1 per face press, -= 4 per left
//   stick direction press while the window +0x56A runs) and Gene's stun mash-out (pl00 mode 1 state 6, func_0011B5F8:
//   +0x15B0 -= 5 per face press, -= 20 per stick press, on top of -1 a frame). Every one reads GameWork +0x08 in the
//   same cluster: `lw v0,8(v1); andi 0xF0` for the face buttons, then `and 0xF00000` for the stick's direction bits.
// - Mash moves: Gene's eleven roulette mash states (fps60-notes "Button-mash meter"; Pummel = state 56 func_00116A88):
//   a face press raises the level +0x15B4 when the frames since the last press +0x15B0 are under the level's limit.
//
// Each hook runs just before the first load of the pressed word in a read cluster (flag mash_assist), so it works
// whatever the state does before (func_00236190 reads at the top of its frame and shows its prompt at the end). The
// press is ORed into the game's own word for the rest of that game frame: the frame Gene breaks free always carries a
// press anyway (the counter only drops on one), so nothing else sees input a human masher wouldn't have given.
//
// Cadence: on game frames (+0x210 counts main loops; at 60 FPS, +0x569 == 1, two make a 30 FPS frame), so 30 and 60
// FPS mash alike: 7 face presses a second (K, Nov 23; 15 then 10 looked too fast), a stick flick every 4 frames. 7 is
// 30/7 frames apart, so presses fall 4 or 5 frames apart (the remainder carried, under one frame). Kept as the last
// frame given, not a phase, so a state that reads only on some vblanks (held frames at 60) still gets its presses.
#include "kamite/mash_assist.h"

#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <cstring>

namespace kamite::mash_assist
{
    namespace
    {
        namespace GameWork = kamite::game::GameWork;

        constexpr uint32_t kFaceButtons = 0xF0u;  // triangle 0x10, circle 0x20, cross 0x40, square 0x80 (raw pad order)
        constexpr uint32_t kCircle = 0x20u;       // the mash moves ask for Circle (B) by name: the assist takes only it
        // The pad work object (D_005E8140, func_0031A808's p): +0x104..+0x107 the analog bytes RX RY LX LY, stored
        // inverted (~raw); func_0031B560 builds GameWork's sticks (+0x70 LX, +0x71 LY, up positive) and direction bits
        // from them, but only past a deadzone of 0x3F and a bit threshold of 0x50 (of 0x80): too stiff for "off
        // centre at all", so the assist reads the bytes itself. They are what the game gets (Swap Sticks, the stick
        // deadzone and the keyboard's stick keys already applied by the host).
        constexpr uint32_t kPadLeftX = 0x5E8246u, kPadLeftY = 0x5E8247u;
        constexpr int kStickOffCentre = 16; // of 128: drift is the stick deadzone's job; anything past this is held
        // The direction bits as func_0031B560 sets them (lui 0x80 / 0x40 / 0x10 / 0x20 at 0x31B7A8..0x31B7FC).
        constexpr uint32_t kStickRight = 0x800000u, kStickLeft = 0x400000u, kStickUp = 0x100000u, kStickDown = 0x200000u;
        // Spacing in 30 FPS game frames as num / den: face 30/7 (7 presses a second, K), stick every frame (K, Nov 25:
        // every 4 wasn't strong enough; a stick press is worth 4 face presses in a grab, 20 frames in the mash-out).
        constexpr uint32_t kFaceNum = 30u, kFaceDen = 7u, kStickNum = 1u, kStickDen = 1u;

        std::atomic<bool> g_on{false};
        // The last press given (EE thread only): its 30 FPS frame (the spacing) and its main loop (other reads in the
        // same vblank see it too; the next vblank of the same 30 FPS frame at 60 doesn't, or it would count twice).
        struct Given
        {
            bool any = false;
            uint32_t frame = 0u, loop = 0u;
            uint64_t next = 0u; // the next press's time in 1/den frames
        };
        Given g_face, g_stick;

        uint32_t rd32(const uint8_t *rdram, uint32_t address)
        {
            uint32_t value = 0;
            std::memcpy(&value, rdram + (address & PS2_RAM_MASK), sizeof(value));
            return value;
        }

        void wr32(uint8_t *rdram, uint32_t address, uint32_t value)
        {
            std::memcpy(rdram + (address & PS2_RAM_MASK), &value, sizeof(value));
        }

        // Due when num / den 30 FPS frames have passed since the last press given (a late press carries less than a
        // frame of its lateness into the next); a count that went back (a save state, a live switch of the rate)
        // counts as due. Other reads in the vblank it was given in see the same press.
        bool due(const uint8_t *rdram, Given &given, uint32_t num, uint32_t den)
        {
            const uint32_t loop = rd32(rdram, GameWork::address(GameWork::kFrameCount)); // main loops: vblanks at 60
            const uint32_t frame = rdram[GameWork::address(GameWork::kVblankDivider) & PS2_RAM_MASK] == 1u ? loop >> 1 : loop;
            if (given.any && loop == given.loop)
                return true;
            const uint64_t t = static_cast<uint64_t>(frame) * den;
            const bool fresh = !given.any || frame < given.frame || loop < given.loop;
            if (fresh || t >= given.next)
            {
                const uint64_t late = fresh ? 0u : (t - given.next < den ? t - given.next : den - 1u);
                given = Given{true, frame, loop, t - late + num};
                return true;
            }
            return false;
        }

        // The left stick's direction bit while it is off centre, else 0.
        uint32_t stickBit(const uint8_t *rdram)
        {
            const int x = 127 - static_cast<int>(rdram[kPadLeftX]); // raw - 128: right positive
            const int y = static_cast<int>(rdram[kPadLeftY]) - 127; // 128 - raw: up positive
            const int ax = x < 0 ? -x : x, ay = y < 0 ? -y : y;
            if (ax < kStickOffCentre && ay < kStickOffCentre)
                return 0u;
            if (ax >= ay)
                return x > 0 ? kStickRight : kStickLeft;
            return y > 0 ? kStickUp : kStickDown;
        }

        // Before a struggle's read of the pressed word (flag mash_assist): any held face button and the stick.
        void struggleRead(uint8_t *rdram, R5900Context *, PS2Runtime *)
        {
            const uint32_t held = rd32(rdram, GameWork::address(GameWork::kPadHeld));
            uint32_t add = 0u;
            if ((held & kFaceButtons) != 0u && due(rdram, g_face, kFaceNum, kFaceDen))
                add |= held & kFaceButtons;
            if (const uint32_t bit = stickBit(rdram); bit != 0u && due(rdram, g_stick, kStickNum, kStickDen))
                add |= bit;
            if (add != 0u)
            {
                const uint32_t pressed = GameWork::address(GameWork::kPadBtn);
                wr32(rdram, pressed, rd32(rdram, pressed) | add);
            }
        }

        // Before a mash move's read of the pressed word (flag mash_assist): Circle held.
        void moveRead(uint8_t *rdram, R5900Context *, PS2Runtime *)
        {
            if ((rd32(rdram, GameWork::address(GameWork::kPadHeld)) & kCircle) == 0u)
                return;
            if (!due(rdram, g_face, kFaceNum, kFaceDen))
                return;
            const uint32_t pressed = GameWork::address(GameWork::kPadBtn);
            wr32(rdram, pressed, rd32(rdram, pressed) | kCircle);
        }

        void setenvFlag(bool on)
        {
#ifdef _WIN32
            _putenv_s("KAMITE_MASH_ASSIST", on ? "1" : "0");
#else
            setenv("KAMITE_MASH_ASSIST", on ? "1" : "0", 1);
#endif
        }
    }

    void install(PS2Runtime &)
    {
        const char *value = std::getenv("KAMITE_MASH_ASSIST");
        const bool on = value && *value && *value != '0';
        ps2xRegisterHostHook("km_mash_assist_struggle", &struggleRead);
        ps2xRegisterHostHook("km_mash_assist_move", &moveRead);
        g_on.store(on, std::memory_order_relaxed);
        ps2xSetPatchFlag("mash_assist", on);
        if (on)
            KM_INFO(Game, "Mash Assistic on: held face buttons and the stick mash in struggles, held Circle in mash moves");
    }

    bool enabled()
    {
        return g_on.load(std::memory_order_relaxed);
    }

    void setEnabled(bool on)
    {
        g_on.store(on, std::memory_order_relaxed);
        ps2xSetPatchFlag("mash_assist", on);
        setenvFlag(on);
    }
}
