#include "kamite/launcher.h"
#include "kamite/launcher_bark.h"
#include "kamite/launcher_saloon.h"

#include "kamite/character_mods.h"
#include "kamite/disc_files.h"
#include "kamite/game_disc.h"
#include "kamite/ini_file.h"
#include "kamite/launcher_platform.h"
#include "kamite/launcher_update.h"
#if defined(KAMITE_STEAM_SHORTCUT) && KAMITE_STEAM_SHORTCUT
#include "kamite/launcher_steam.h"
#define KAMITE_LAUNCHER_STEAM 1
#endif
#include "kamite/log.h"
#include "kamite/presets.h"
#include "kamite/settings_table.h"
#include "ps2_host_gamepads.h"
#include "ps2_host_input.h"
#include "runtime/ps2_display_options.h"
#include "runtime/ps2_hotkeys.h"

#include "imgui.h"
#include "imgui_internal.h"
#include "raylib.h"
#include "rlImGui.h"
#include "rlgl.h"

#include <cstdint>
#include <algorithm>
#include <atomic>
#include <chrono>
#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <ctime>
#include <fstream>
#include <functional>
#include <random>
#include <string>
#include <thread>
#include <vector>

namespace kamite::launcher
{
    namespace
    {
        namespace fs = std::filesystem;

        // Items whose activation makes the cursor's tick, not the confirm sound (boxes, dropdowns, radio buttons, folds:
        // K), and the buttons that play their own sound when let go (Back: cancel, Play: Play's; a confirm on the press
        // made both sound). Refilled each frame by the widgets right after they are drawn; read by frameSounds.
        std::vector<ImGuiID> g_tickItems, g_ownSoundItems, g_silentItems;
        void tickItem() { g_tickItems.push_back(ImGui::GetItemID()); }
        void silentItem() { g_silentItems.push_back(ImGui::GetItemID()); } // sliders make no sound (K)
        void ownSoundItem() { g_ownSoundItems.push_back(ImGui::GetItemID()); }

        // The updater's threads outlive the launcher's state (a slow check is left running when Play is pressed).
        update::Checker g_updateCheck;
        update::Installer g_updater;

        // ---- choices ------------------------------------------------------------------------------------------------
        struct Choice
        {
            const char *label;
            const char *value;
        };

        constexpr Choice kWindowModes[] = {{"Fullscreen (Borderless)", "fullscreen"}, {"Maximized Window", "maximized"}, {"Window", "window"}};

        std::string utf8(const fs::path &path)
        {
            const std::u8string text = path.u8string();
            return std::string(text.begin(), text.end());
        }

        fs::path fromUtf8(const std::string &text) { return fs::path(std::u8string(text.begin(), text.end())); }

        std::string lower(std::string text)
        {
            for (char &c : text)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return text;
        }

        // A combo over fixed choices; a value that is none of them (set by hand) is shown as one more entry and kept
        // unless another is picked. Returns true when changed (value updated).
        template <size_t N>
        bool choiceCombo(const char *id, const Choice (&choices)[N], std::string &value)
        {
            int current = -1;
            for (size_t i = 0; i < N; ++i)
                if (lower(value) == lower(choices[i].value))
                    current = static_cast<int>(i);
            std::vector<const char *> labels;
            for (const Choice &choice : choices)
                labels.push_back(choice.label);
            const std::string custom = "Custom: " + value;
            if (current < 0)
            {
                labels.push_back(custom.c_str());
                current = static_cast<int>(N);
            }
            int picked = current;
            ImGui::SetNextItemWidth(-1.0f);
            const bool changed = ImGui::Combo(id, &picked, labels.data(), static_cast<int>(labels.size()));
            tickItem();
            if (!changed || picked == current || picked >= static_cast<int>(N))
                return false;
            value = choices[picked].value;
            return true;
        }

        // ---- look ---------------------------------------------------------------------------------------------------
        const ImVec4 kAccent(0.91f, 0.38f, 0.16f, 1.0f);      // The game's fire orange
        const ImVec4 kAccentHover(1.00f, 0.50f, 0.22f, 1.0f);
        const ImVec4 kAccentActive(0.78f, 0.28f, 0.10f, 1.0f);
        const ImVec4 kPanel(0.10f, 0.09f, 0.09f, 0.92f);
        const ImVec4 kTextDim(0.66f, 0.62f, 0.58f, 1.0f);
        const ImVec4 kTextSoft(0.80f, 0.77f, 0.73f, 1.0f); // dimmer text straight on the background
        const ImVec4 kText(0.95f, 0.92f, 0.88f, 1.0f);
        const ImVec4 kHeading(1.0f, 0.62f, 0.30f, 1.0f); // the Options title and the categories' headings
        bool g_hotkeysOpen = false;                       // the Controls tab's Hotkeys: folded each time Options opens

        void applyStyle()
        {
            ImGuiStyle &style = ImGui::GetStyle();
            ImGui::StyleColorsDark(&style);
            style.WindowRounding = 0.0f;
            style.FrameRounding = 6.0f;
            style.PopupRounding = 6.0f;
            style.GrabRounding = 6.0f;
            style.TabRounding = 6.0f;
            style.FramePadding = ImVec2(12.0f, 8.0f);
            style.ItemSpacing = ImVec2(12.0f, 12.0f);
            style.WindowPadding = ImVec2(0.0f, 0.0f);
            ImVec4 *colors = style.Colors;
            colors[ImGuiCol_WindowBg] = ImVec4(0, 0, 0, 0);
            colors[ImGuiCol_ChildBg] = kPanel;
            colors[ImGuiCol_PopupBg] = ImVec4(0.12f, 0.11f, 0.10f, 0.98f);
            colors[ImGuiCol_Border] = ImVec4(0.35f, 0.22f, 0.15f, 0.6f);
            colors[ImGuiCol_FrameBg] = ImVec4(0.18f, 0.16f, 0.15f, 1.0f);
            colors[ImGuiCol_FrameBgHovered] = ImVec4(0.26f, 0.20f, 0.17f, 1.0f);
            colors[ImGuiCol_FrameBgActive] = ImVec4(0.32f, 0.22f, 0.17f, 1.0f);
            colors[ImGuiCol_Button] = ImVec4(0.20f, 0.17f, 0.15f, 1.0f);
            colors[ImGuiCol_ButtonHovered] = kAccentHover;
            colors[ImGuiCol_ButtonActive] = kAccentActive;
            colors[ImGuiCol_Header] = ImVec4(0.30f, 0.20f, 0.15f, 1.0f);
            colors[ImGuiCol_HeaderHovered] = kAccentHover;
            colors[ImGuiCol_HeaderActive] = kAccentActive;
            colors[ImGuiCol_CheckMark] = kAccent;
            colors[ImGuiCol_Tab] = ImVec4(0.18f, 0.15f, 0.13f, 1.0f);
            colors[ImGuiCol_TabHovered] = kAccentHover;
            colors[ImGuiCol_TabSelected] = kAccent;
            colors[ImGuiCol_NavCursor] = kAccentHover;
            colors[ImGuiCol_Separator] = ImVec4(0.35f, 0.22f, 0.15f, 0.6f);
            colors[ImGuiCol_Text] = ImVec4(0.95f, 0.92f, 0.88f, 1.0f);
            colors[ImGuiCol_TextDisabled] = kTextDim;
        }

        void drawBackground()
        {
            const int width = GetScreenWidth(), height = GetScreenHeight();
            DrawRectangleGradientV(0, 0, width, height, Color{20, 15, 13, 255}, Color{48, 20, 10, 255});
            // A low glow behind the title.
            DrawCircleGradient(width / 2, height / 4, static_cast<float>(std::max(width, height)) * 0.45f, Color{120, 45, 15, 70},
                               Color{0, 0, 0, 0});
        }

        // Every controller, as ImGui's gamepad keys (SDL3 through ps2_host_gamepads: the same devices the game uses).
        void feedGamepads(ImGuiIO &io)
        {
            using namespace ps2_host_gamepads;
            ps2_host_gamepads::update();
            bool held[16] = {};
            float lx = 0.0f, ly = 0.0f;
            for (int device = 0; device < kMaxDevices; ++device)
            {
                if (!connected(device))
                    continue;
                held[0] |= button(device, DPadUp);
                held[1] |= button(device, DPadDown);
                held[2] |= button(device, DPadLeft);
                held[3] |= button(device, DPadRight);
                held[4] |= button(device, South);
                held[5] |= button(device, East);
                held[6] |= button(device, West);
                held[7] |= button(device, North);
                held[8] |= button(device, LeftShoulder);
                held[9] |= button(device, RightShoulder);
                held[10] |= button(device, Start);
                held[11] |= button(device, Back);
                if (std::fabs(axis(device, LeftX)) > std::fabs(lx))
                    lx = axis(device, LeftX);
                if (std::fabs(axis(device, LeftY)) > std::fabs(ly))
                    ly = axis(device, LeftY);
            }
            const ImGuiKey keys[] = {ImGuiKey_GamepadDpadUp,   ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadDpadRight,
                                     ImGuiKey_GamepadFaceDown, ImGuiKey_GamepadFaceRight, ImGuiKey_GamepadFaceLeft, ImGuiKey_GamepadFaceUp,
                                     ImGuiKey_GamepadL1,       ImGuiKey_GamepadR1,       ImGuiKey_GamepadStart,    ImGuiKey_GamepadBack};
            for (int i = 0; i < 12; ++i)
                io.AddKeyEvent(keys[i], held[i]);
            auto stick = [&](ImGuiKey negative, ImGuiKey positive, float value)
            {
                const float dead = 0.35f;
                const float n = value < -dead ? std::min(1.0f, (-value - dead) / (1.0f - dead)) : 0.0f;
                const float p = value > dead ? std::min(1.0f, (value - dead) / (1.0f - dead)) : 0.0f;
                io.AddKeyAnalogEvent(negative, n > 0.0f, n);
                io.AddKeyAnalogEvent(positive, p > 0.0f, p);
            };
            stick(ImGuiKey_GamepadLStickLeft, ImGuiKey_GamepadLStickRight, lx);
            stick(ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadLStickDown, ly);
        }

        // ---- the screens --------------------------------------------------------------------------------------------
        enum class Screen
        {
            Home,
            Install,
            Options,
            FirstRun, // the wanted posters: a preset to start with (once, on a new install)
        };

        struct State
        {
            const Context *context = nullptr;
            IniFile ini;
            Screen screen = Screen::Home;
            bool installed = false;
            std::string installMessage;
            bool installError = false;
            char isoText[1024] = {};
            bool focusFirst = true; // put the gamepad/keyboard cursor on the screen's first button
            bool done = false;
            Result result = Result::Quit;
            ImFont *font = nullptr;
            float unit = 1.0f; // UI scale: window height / 720
            // The logo's layers (tools/logo/export_launcher_logo.py), all the same canvas, drawn into one rectangle:
            Texture2D logo{};        // resources/images/logo.png: GOD HAND and its shadow (it stays still)
            Texture2D kana{};        // resources/images/kana.png: the flat kana behind it (moves a little with the cursor)
            Texture2D rearmed{};     // resources/images/rearmed.png: the REARMED plate without its letters
            Texture2D rearmedText{}; // resources/images/rearmed_text.png: its letters, white (tinted gold / hot)
            Texture2D rearmedGlow{}; // resources/images/rearmed_glow.png: the letters blurred, for the hot glow
            Texture2D burst{}; // resources/images/burst.png: the speed lines behind the logo
            // The saloon (launcher_saloon.h, claude/saloon-launcher.md): the room behind every screen, Home's menu in
            // ink on its bill of fare, the logo in the doorway's glare, the Options zoom. Off (the plain gradient and
            // the old buttons) when resources/saloon is missing or can't be decoded.
            saloon::Scene scene;
            bool saloonOn = false;
            saloon::BillFonts billFonts;
            saloon::Bill bill;            // the menu's focus (its loop) and items, drawn before ImGui from last frame's
            bool focusOptions = false;    // back from Options: the loop goes back on Options
            float optionsShown = 0.0f;    // Options' panel over the zoom's last frame: fades in once it lands
            bool optionsLeaving = false;  // Back: the panel fades out, then the zoom plays backwards
            bool navCleared = false;      // the mouse left everything: the next key focuses the first item, with a tick
            bool zoomPending = false;     // Options came before the zoom's frames were read
            int slipFocus = 0;            // the corner slips' focused item (SlipItem), drawn before ImGui
            std::vector<Sound> snackVoices; // Gene's dry lines from se/pl00.afs (eating a snack)
            float voiceAt = -1.0f;        // a snack line waiting for the slurp to end (GetTime)
            Music ambience{};             // resources/sfx/launcher: the bar's murmur under the music (K, Nov 18)
            bool hasAmbience = false;
            Sound pen{}, slurp{}, bite{}; // the pencil loop drawn; the smoothie's last sip; a fruit or a garnish eaten
            Sound bookOpen{}, bookClose{}; // the Options zoom in to the book and back out (with the whoosh)
            bool hasPen = false, hasSlurp = false, hasBite = false, hasBookOpen = false, hasBookClose = false;
            bool penThisFrame = false;    // the pen played: no cursor tick on top of it
            int lastSlipFocus = 0;
            int snackVoice = -1;          // the one playing (lines never overlap or queue)
            Font splashFont{}; // resources/splash/splash.fnt, the Dominican font baked as an atlas
            std::string splash; // a random line of resources/splash/splash.txt, "" when there is none
            // The title's motion: the cursor (eased) shifts the burst layers and the kana by different amounts,
            // the bursts turn slowly and pulse; the wordmark does not move.
            float time = 0.0f;
            ImVec2 parallax{0.0f, 0.0f}; // -1..1 each way; 0 while the cursor is away
            float burstShown = 0.0f;     // fades the burst in on the screens with the logo, out on the others
            bool logoPlaced = false;     // logoMin/logoMax hold last frame's logo rectangle (without the parallax shift)
            bool logoThisFrame = false;  // title() laid the logo out this frame (drawLogo draws it after ImGui)
            ImVec2 logoMin{}, logoMax{};
            float playTime = -1.0f;      // seconds since Play was pressed (the plate's animation, the fade); -1: not yet
            bool audio = false;          // the audio device is open (closed again before the game opens its own)
            Music music{};               // resources/music/launcher.ogg, looped; fades out with Play
            bool hasMusic = false;
            // The game's menu sounds (read from the player's disc image): kMenuSoundIds' order.
            Sound menuSounds[4]{};
            float menuSoundBase[4]{};    // each sound's own volume in the game's bank
            bool hasMenuSound[4]{};
            // The start bark (launcher_bark.h, K Nov 10): one line, dealt from [Launcher] BarkDeck, rendered in the
            // background from the disc image; the music's tail is rendered when Play is pressed.
            std::thread barkWorker;
            bool barkStarted = false;
            int barkLine = -1;
            launcher_bark::Stereo barkPcm;   // empty: no voice (not read)
            launcher_bark::Wash wash;
            std::vector<int16_t> musicPcm;   // resources/music/launcher.ogg decoded, for its tail
            int musicRate = 0;
            fs::path musicPath;
            bool musicHandedOff = false;     // Play replaced the music stream with its tail
            float volume = 0.8f;         // [Launcher] Volume: music and sounds (the corner slider)
            bool muted = false;          // [Launcher] Mute
            ImGuiID volumeSliderId = 0;  // makes no sound when grabbed
            ImGuiID lastNavId = 0;       // the keyboard / gamepad cursor's item last frame
            bool popupWasOpen = false;
            bool soundThisFrame = false; // a confirm / cancel / Play sound already played this frame
            int quietFrames = 0;         // no cursor tick for a frame or two after a confirm / cancel (focus moves then)
            Screen soundScreen = Screen::Home;
            // Options > Presets (presets.h): the photos from the disc image (read once, when the tab first shows) and the
            // preset waiting for "Replace your custom settings?".
            Texture2D presetPhotos[presets::kCount]{};
            bool presetPhotosRead = false;
            int presetAsk = -1;
            bool presetOpenPopup = false;
            // The first run's wanted posters (resources/presets: tools/launcher/make_posters.py), shown once on a new
            // install (no boot ELF in game_data yet and no [Launcher] PresetPicked): the poster picked, the layers.
            bool firstRunPending = false;
            int firstRunPick = presets::kClassic;
            bool postersLoaded = false;
            Texture2D poster[presets::kCount]{}, posterTape[presets::kCount]{}, posterShadow[presets::kCount]{}, stamp{};
            float posterW = 0.0f, posterH = 0.0f, posterPad = 0.0f;
            float photoBox[presets::kCount][4]{}, stampAt[presets::kCount][2]{};
            float pickShown = 0.0f; // the raised poster, eased toward firstRunPick
            // Updates (launcher_update.h): above the volume; What's new and the progress in a popup.
            enum class UpdatePopup
            {
                None,
                Notes,   // What's new
                Working, // g_updater downloading / installing
                Failed,  // g_updater.error
            };
            UpdatePopup updatePopup = UpdatePopup::None;
            bool updateOpenPopup = false;
            fs::path exePath; // read at start: the update renames the running file aside
#if KAMITE_LAUNCHER_STEAM
            // Add to Steam (launcher_steam.h): the lower left corner's button and its popup.
            enum class SteamPopup
            {
                None,
                Confirm, // Steam runs: restart it?
                Working, // the job on steamWorker; steamStage follows it
                Done     // steamResult's message
            };
            steam::Status steam;              // probed when the launcher opens and after each job
            std::thread steamWorker;
            std::atomic<int> steamStage{-1};  // steam::Stage while a job runs
            steam::Result steamResult;        // read once the worker is joined
            SteamPopup steamPopup = SteamPopup::None;
            bool steamOpenPopup = false;
            bool steamFocus = false;          // put the cursor on the popup's first button (it changed page)
#endif
        };

        // GOD HAND's centre in the logo's canvas (tools/logo/export_launcher_logo.py prints it); the burst is centred
        // there. kRearmedRect: the REARMED plate (left, top, right, bottom as fractions of the canvas, same script).
        constexpr float kLogoCentreX = 0.5048f, kLogoCentreY = 0.5197f;
        constexpr float kRearmedRect[4] = {0.3110f, 0.7152f, 0.6961f, 0.8930f};
        // Play: the plate's animation, after the game's roulette pick (K's capture at 1/8 speed): the letters
        // heat up and bloom, a yellow bar shoots out both ways, the bar thins to streaks, all gone at 0.28 s;
        // the rest of the launcher fades to black meanwhile and the game starts at kPlayLength.
        constexpr float kPlayLength = 0.34f;
        // The game's menu sounds (se/core.ses on the disc; K's picks): the cursor's tick when the keyboard or gamepad
        // moves it (0x164), confirm (0x15E: buttons, boxes, tabs, dropdown picks; clicks too), cancel (0x161: Back, B /
        // Esc), Play (0x160, the main menu's confirm; it rings on into the game's boot: the launcher leaves the audio
        // device open for the game). The volume slider makes none.
        enum MenuSound
        {
            kTick,
            kConfirm,
            kCancel,
            kPlay
        };
        constexpr int kMenuSoundIds[4] = {0x164, 0x15E, 0x161, 0x160};
        constexpr float kMusicVolume = 0.6f; // under the player's volume

        // A sound of the game's SE bank (.ses: "SES\0", f32 version; u32s at +0x10.. = sections: +0x10 the sound table
        // (u32 count, then from +0x20 0x30-byte entries: +4 u16 tone, +0xF 0x80 = unused, +0x10 u8 volume 0..127), +0x20
        // the tone table (16 bytes each: u32 offset into the sample data, u32 sample rate), +0x28 / +0x2C the sample
        // data (PS-ADPCM)) as 16-bit mono PCM. False when the bank or the sound is not what that describes.
        bool decodeSesSound(const std::vector<uint8_t> &ses, int id, std::vector<int16_t> &pcm, int &rate, float &volume)
        {
            auto u32 = [&](size_t o) -> uint32_t {
                return o + 4 > ses.size() ? 0u : static_cast<uint32_t>(ses[o]) | (static_cast<uint32_t>(ses[o + 1]) << 8) |
                                                 (static_cast<uint32_t>(ses[o + 2]) << 16) | (static_cast<uint32_t>(ses[o + 3]) << 24);
            };
            if (ses.size() < 0x40 || std::memcmp(ses.data(), "SES", 4) != 0)
                return false;
            const uint32_t table = u32(0x10), tones = u32(0x20), toneBytes = u32(0x24), data = u32(0x28), dataSize = u32(0x2C);
            if (id < 0 || static_cast<uint32_t>(id) >= u32(table) || data + static_cast<uint64_t>(dataSize) > ses.size())
                return false;
            const size_t entry = table + 0x20 + 0x30 * static_cast<size_t>(id);
            if (entry + 0x30 > ses.size() || (ses[entry + 0xF] & 0x80) != 0)
                return false;
            const uint32_t tone = static_cast<uint32_t>(ses[entry + 4]) | (static_cast<uint32_t>(ses[entry + 5]) << 8);
            if (16ull * (tone + 1) > toneBytes)
                return false;
            const uint32_t offset = u32(tones + 16 * tone);
            rate = static_cast<int>(u32(tones + 16 * tone + 4));
            volume = std::clamp(static_cast<float>(ses[entry + 0x10]) / 127.0f, 0.0f, 1.0f);
            if (rate < 4000 || rate > 96000 || offset >= dataSize)
                return false;
            // PS-ADPCM: 16-byte frames (shift | filter << 4, flags (bit 0: last frame), 14 bytes = 28 samples).
            static constexpr int kFilter[5][2] = {{0, 0}, {60, 0}, {115, -52}, {98, -55}, {122, -60}};
            int s1 = 0, s2 = 0;
            pcm.clear();
            for (size_t p = data + offset; p + 16 <= data + static_cast<size_t>(dataSize); p += 16)
            {
                const int shift = ses[p] & 15, filter = std::min(ses[p] >> 4, 4);
                for (int i = 0; i < 28; ++i)
                {
                    const uint8_t b = ses[p + 2 + i / 2];
                    int v = (i & 1) ? (b >> 4) : (b & 15);
                    if (v > 7)
                        v -= 16;
                    v = shift <= 12 ? (v << 12) >> shift : 0;
                    v += (s1 * kFilter[filter][0] + s2 * kFilter[filter][1]) >> 6; // floor, as the SPU2
                    v = std::clamp(v, -32768, 32767);
                    pcm.push_back(static_cast<int16_t>(v));
                    s2 = s1;
                    s1 = v;
                }
                if ((ses[p + 1] & 1) != 0)
                    break;
            }
            return !pcm.empty();
        }

        // The menu sounds from the player's own disc (never shipped): loaded at the start and after an install.
        void loadMenuSounds(State &state, const fs::path &image)
        {
            if (!state.audio || state.hasMenuSound[kPlay] || image.empty())
                return;
            std::vector<uint8_t> ses;
            if (!disc_files::readFromImage(image, "se/core.ses", ses))
            {
                KM_INFO(Boot, "launcher: no menu sounds (se/core.ses not read from the disc image)");
                return;
            }
            for (int i = 0; i < 4; ++i)
            {
                std::vector<int16_t> pcm;
                int rate = 0;
                float volume = 1.0f;
                if (!decodeSesSound(ses, kMenuSoundIds[i], pcm, rate, volume))
                {
                    KM_INFO(Boot, "launcher: menu sound 0x" << std::hex << kMenuSoundIds[i] << std::dec << " not in se/core.ses");
                    continue;
                }
                Wave wave{};
                wave.frameCount = static_cast<unsigned int>(pcm.size());
                wave.sampleRate = static_cast<unsigned int>(rate);
                wave.sampleSize = 16;
                wave.channels = 1;
                wave.data = pcm.data();
                state.menuSounds[i] = LoadSoundFromWave(wave); // copies the samples
                state.hasMenuSound[i] = state.menuSounds[i].frameCount > 0;
                state.menuSoundBase[i] = volume;
            }
        }

        float userVolume(const State &state) { return state.muted ? 0.0f : state.volume; }

        // The launcher's own sound effects (resources/sfx/launcher, K's): one shot, restarted if still playing; a little
        // over the files' own level (K: the effects up a bit).
        constexpr float kEffectGain = 1.4f;

        void playEffect(State &state, Sound &sound, bool loaded, float level = 1.0f)
        {
            if (!loaded)
                return;
            SetSoundVolume(sound, kEffectGain * level * userVolume(state));
            PlayAudioStream(sound.stream); // not PlaySound (winmm's, see playMenuSound)
        }

        // The pencil loop drawn round something (the bill, the slip): its scribble instead of the cursor's tick.
        void penSound(State &state)
        {
            playEffect(state, state.pen, state.hasPen);
            state.penThisFrame = state.hasPen;
        }

        void menuSound(State &state, MenuSound which);

        // Into the book and back out (K, Nov 18): the whoosh (the menu's cancel sound) with the book opening or closing,
        // instead of the confirm. (menuSound marks the frame's sound as played.)
        void bookSound(State &state, bool open)
        {
            menuSound(state, kCancel);
            if (open)
                playEffect(state, state.bookOpen, state.hasBookOpen);
            else
                playEffect(state, state.bookClose, state.hasBookClose);
        }

        // A snack eaten: its sound (the slurp for the drink, a bite for the rest; K's), Gene's line halfway through it;
        // without the sound, the line at once.
        float soundLength(const Sound &sound) { return sound.stream.sampleRate > 0 ? static_cast<float>(sound.frameCount) / sound.stream.sampleRate : 0.0f; }
        void snackVoice(State &state);
        void eatSound(State &state, bool drink)
        {
            Sound &sound = drink ? state.slurp : state.bite;
            const bool loaded = drink ? state.hasSlurp : state.hasBite;
            if (!loaded)
            {
                snackVoice(state);
                return;
            }
            playEffect(state, sound, true, 2.0f); // (K: twice the files' level, against Gene's voice)
            state.voiceAt = static_cast<float>(GetTime()) + soundLength(sound) * 0.5f; // into its quiet roll-off (K)
        }

        void playMenuSound(State &state, MenuSound which, float scale = 1.0f)
        {
            if (!state.hasMenuSound[which])
                return;
            SetSoundVolume(state.menuSounds[which], state.menuSoundBase[which] * userVolume(state) * scale);
            // Not PlaySound: GHR.exe links winmm with /FORCE:MULTIPLE, and winmm's PlaySound took the call (no sound,
            // no error). raylib's PlaySound is exactly this.
            PlayAudioStream(state.menuSounds[which].stream);
        }

        uint32_t fnv1a(std::string_view text, uint32_t hash = 2166136261u)
        {
            for (const unsigned char c : text)
                hash = (hash ^ c) * 16777619u;
            return hash;
        }

        // One pick from a shuffled deck kept in GHR.ini ([Launcher] key = "<list id>:<last>:<rest, ...>", K Nov 10):
        // every entry comes once, in random order, before any comes back; a new deck never starts with the one just
        // used; a changed list (another id: a splash line added) starts a new deck.
        int dealFromDeck(State &state, const char *key, int count, uint32_t listId)
        {
            if (count <= 0)
                return -1;
            char id[16];
            std::snprintf(id, sizeof(id), "%08x", listId);
            const std::string saved = state.ini.get("Launcher", key, "");
            int last = -1;
            std::vector<int> rest;
            const size_t c1 = saved.find(':'), c2 = c1 == std::string::npos ? c1 : saved.find(':', c1 + 1);
            if (c2 != std::string::npos && saved.compare(0, c1, id) == 0)
            {
                last = std::atoi(saved.substr(c1 + 1, c2 - c1 - 1).c_str());
                std::string list = saved.substr(c2 + 1);
                for (size_t at = 0; at < list.size();)
                {
                    const size_t comma = std::min(list.find(',', at), list.size());
                    const std::string item = list.substr(at, comma - at);
                    const int v = item.empty() ? -1 : std::atoi(item.c_str());
                    if (v >= 0 && v < count && std::find(rest.begin(), rest.end(), v) == rest.end())
                        rest.push_back(v);
                    at = comma + 1;
                }
            }
            if (rest.empty())
            {
                for (int i = 0; i < count; ++i)
                    rest.push_back(i);
                std::mt19937 random(std::random_device{}() ^ static_cast<unsigned>(std::time(nullptr)));
                std::shuffle(rest.begin(), rest.end(), random);
                if (count > 1 && rest.front() == last)
                    std::swap(rest[0], rest[1]);
            }
            const int pick = rest.front();
            rest.erase(rest.begin());
            std::string value = std::string(id) + ":" + std::to_string(pick) + ":";
            for (size_t i = 0; i < rest.size(); ++i)
                value += (i ? "," : "") + std::to_string(rest[i]);
            state.ini.set("Launcher", key, value);
            state.ini.save();
            return pick;
        }

        // The start bark: deals this start's line and renders it in the background (with the music's reverb and the
        // music itself, for the tail on Play). Once per start, when the disc image is known.
        void startBark(State &state, const fs::path &image)
        {
            if (!state.audio || state.barkStarted || image.empty())
                return;
            state.barkStarted = true;
            uint32_t listId = 2166136261u;
            for (int i = 0; i < launcher_bark::count(); ++i)
                listId = fnv1a(std::string(launcher_bark::name(i)) + "\n", listId);
            state.barkLine = dealFromDeck(state, "BarkDeck", launcher_bark::count(), listId);
            const fs::path musicPath = state.hasMusic ? state.musicPath : fs::path();
            state.barkWorker = std::thread([&state, image, musicPath, line = state.barkLine] {
                const launcher_bark::ReadFile read = [&image](std::string_view path, std::vector<uint8_t> &bytes) {
                    return disc_files::readFromImage(image, path, bytes);
                };
                if (!launcher_bark::render(line, read, state.barkPcm))
                    state.barkPcm.lr.clear();
                if (musicPath.empty())
                    return;
                Wave wave = LoadWave(utf8(musicPath).c_str());
                if (wave.data != nullptr && wave.frameCount > 0)
                {
                    WaveFormat(&wave, static_cast<int>(wave.sampleRate), 16, 2);
                    const int16_t *pcm = static_cast<const int16_t *>(wave.data);
                    state.musicPcm.assign(pcm, pcm + static_cast<size_t>(wave.frameCount) * 2);
                    state.musicRate = static_cast<int>(wave.sampleRate);
                    launcher_bark::prepareWash(state.wash);
                }
                UnloadWave(wave);
            });
        }

        Sound soundFrom(const launcher_bark::Stereo &pcm)
        {
            std::vector<int16_t> samples(pcm.lr.size());
            for (size_t i = 0; i < samples.size(); ++i)
                samples[i] = static_cast<int16_t>(std::clamp(pcm.lr[i], -1.0f, 1.0f) * 32767.0f);
            Wave wave{};
            wave.frameCount = static_cast<unsigned int>(pcm.frames());
            wave.sampleRate = launcher_bark::kRate;
            wave.sampleSize = 16;
            wave.channels = 2;
            wave.data = samples.data();
            return LoadSoundFromWave(wave); // copies the samples
        }

        // Play: its own sound (lower under a bark, as K approved the mix), the bark, and the music's tail in place of
        // the music: it closes down and washes out under the voice, all faded out right after the last word. The
        // sounds ring on into the game's boot (the audio device is left to the game, as for Play's sound).
        void playBark(State &state)
        {
            if (state.barkWorker.joinable())
                state.barkWorker.join(); // ready long before Play, unless Play came at once
            const bool voice = !state.barkPcm.lr.empty();
            playMenuSound(state, kPlay, voice ? 0.55f : 1.0f);
            const auto start = std::chrono::steady_clock::now();
            float end = 1.5f;
            if (voice)
            {
                const Sound bark = soundFrom(state.barkPcm);
                SetSoundVolume(bark, userVolume(state));
                PlayAudioStream(bark.stream); // not PlaySound (winmm's, see playMenuSound)
                end = static_cast<float>(state.barkPcm.frames()) / launcher_bark::kRate;
            }
            if (state.hasMusic && !state.musicPcm.empty() && state.wash.fftSize != 0)
            {
                const size_t frames = state.musicPcm.size() / 2;
                const size_t from = static_cast<size_t>(GetMusicTimePlayed(state.music) * state.musicRate) % frames;
                launcher_bark::Stereo tail;
                launcher_bark::musicTail(state.musicPcm.data(), frames, state.musicRate, from, end, state.wash, tail);
                if (!tail.lr.empty())
                {
                    const Sound sound = soundFrom(tail);
                    SetSoundVolume(sound, kMusicVolume * userVolume(state));
                    StopMusicStream(state.music);
                    PlayAudioStream(sound.stream);
                    state.musicHandedOff = true;
                }
            }
            KM_INFO(Boot, "launcher: start bark " << (voice ? launcher_bark::name(state.barkLine) : "none (not read from the disc)")
                                                  << ", music tail " << (state.musicHandedOff ? "on" : "off") << " ("
                                                  << std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count()
                                                  << " ms)");
        }

        // The snacks' voice lines (K, Nov 18): Gene's dry lines from se/pl00.afs (even entries; the odd ones are their
        // echo twins), read from the player's disc like the bark. Dealt from a shuffled deck; one at a time (a bite
        // while one plays is silent: no overlap, no queue).
        constexpr int kSnackLines[] = {0, 2, 4, 6, 8, 10, 12}; // Wow, Nice, I love it, Awesome, Cool, Well how, Great (no Shit, K)

        void loadSnackVoices(State &state, const fs::path &image)
        {
            if (!state.audio || !state.snackVoices.empty() || image.empty())
                return;
            std::vector<uint8_t> afs;
            if (!disc_files::readFromImage(image, "se/pl00.afs", afs))
            {
                KM_INFO(Boot, "launcher: no snack lines (se/pl00.afs not read from the disc image)");
                return;
            }
            for (const int entry : kSnackLines)
            {
                launcher_bark::Stereo pcm;
                if (launcher_bark::voiceLine(afs, entry, pcm))
                    state.snackVoices.push_back(soundFrom(pcm));
            }
        }

        void snackVoice(State &state)
        {
            if (state.snackVoices.empty())
                return;
            if (state.snackVoice >= 0 && IsAudioStreamPlaying(state.snackVoices[static_cast<size_t>(state.snackVoice)].stream))
                return;
            const int count = static_cast<int>(state.snackVoices.size());
            const int pick = dealFromDeck(state, "SnackDeck", count, fnv1a("se/pl00.afs:0,2,4,6,8,10,12\n"));
            if (pick < 0 || pick >= count)
                return;
            Sound &sound = state.snackVoices[static_cast<size_t>(pick)];
            SetSoundVolume(sound, 0.65f * userVolume(state)); // under the effects (K)
            PlayAudioStream(sound.stream); // not PlaySound (winmm's, see playMenuSound)
            state.snackVoice = pick;
        }

        void menuSound(State &state, MenuSound which)
        {
            playMenuSound(state, which);
            state.soundThisFrame = true;
            state.quietFrames = 2;
        }

        // The frame's sounds from what ImGui did (after the launcher's window): an item activated (click or A / Enter:
        // confirm; not the volume slider), a dropdown closed with B / Esc (cancel), the keyboard / gamepad cursor moved
        // to another item (tick; mouse hovering moves nothing). Sounds the screens play themselves (Back, Play) win.
        void frameSounds(State &state)
        {
            ImGuiContext &g = *GImGui;
            const bool popupOpen = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
            const bool backKey = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            if (state.playTime < 0.0f && !state.soundThisFrame)
            {
                if (state.popupWasOpen && !popupOpen && backKey)
                    menuSound(state, kCancel);
                else if (g.ActiveIdIsJustActivated && g.ActiveId != 0 && g.ActiveId != state.volumeSliderId &&
                         std::find(g_silentItems.begin(), g_silentItems.end(), g.ActiveId) == g_silentItems.end())
                {
                    const auto listed = [&](const std::vector<ImGuiID> &items) {
                        return std::find(items.begin(), items.end(), g.ActiveId) != items.end();
                    };
                    const bool inPopup = g.ActiveIdWindow != nullptr && (g.ActiveIdWindow->Flags & ImGuiWindowFlags_Popup) != 0;
                    if (listed(g_ownSoundItems)) // Back / Play / a popup's buttons play their own sound when let go
                        ;
                    else if (listed(g_tickItems) || inPopup) // a box, a dropdown or a pick from its list
                    {
                        playMenuSound(state, kTick);
                        state.quietFrames = 2; // the dropdown's list opening moves the cursor
                    }
                    else
                        menuSound(state, kConfirm);
                }
                else if (g.NavId != state.lastNavId && (state.lastNavId != 0 || state.navCleared) && g.NavId != 0 && g.NavCursorVisible &&
                         !state.penThisFrame &&
                         state.quietFrames == 0 && state.soundScreen == state.screen)
                    playMenuSound(state, kTick);
            }
            if (state.quietFrames > 0 && !state.soundThisFrame)
                --state.quietFrames;
            if (g.NavId != 0)
                state.navCleared = false;
            state.lastNavId = g.NavId;
            state.popupWasOpen = popupOpen;
            state.soundScreen = state.screen;
        }


        void updateMotion(State &state, Screen screen)
        {
            const float dt = std::min(GetFrameTime(), 0.1f);
            state.time += dt;
            const float width = static_cast<float>(GetScreenWidth()), height = static_cast<float>(GetScreenHeight());
            ImVec2 target(0.0f, 0.0f);
            if (IsCursorOnScreen() && width > 0.0f && height > 0.0f)
            {
                const Vector2 mouse = GetMousePosition();
                target.x = std::clamp((mouse.x - width * 0.5f) / (width * 0.5f), -1.0f, 1.0f);
                target.y = std::clamp((mouse.y - height * 0.5f) / (height * 0.5f), -1.0f, 1.0f);
            }
            // The saloon: the room snaps to centre on the zoom's first frame and holds still through it and Options;
            // back at Home it eases in again (K).
            if (state.saloonOn && (state.scene.zoomFrame() > 0 || screen == Screen::Options))
            {
                state.parallax = ImVec2(0.0f, 0.0f);
                target = ImVec2(0.0f, 0.0f);
            }
            const float ease = 1.0f - std::exp(-dt * 3.0f);
            state.parallax.x += (target.x - state.parallax.x) * ease;
            state.parallax.y += (target.y - state.parallax.y) * ease;
            const float shown = screen == Screen::Options ? 0.0f : 1.0f;
            state.burstShown += (shown - state.burstShown) * (1.0f - std::exp(-dt * 5.0f));
        }

        // One layer of the burst: the texture centred at (x, y), radius in pixels, squashed vertically, turned.
        void drawBurstLayer(const Texture2D &burst, float x, float y, float radius, float angle, unsigned char alpha)
        {
            const float size = radius * 2.0f;
            rlPushMatrix();
            rlTranslatef(x, y, 0.0f);
            rlScalef(1.0f, 0.75f, 1.0f);
            rlRotatef(angle, 0.0f, 0.0f, 1.0f);
            DrawTexturePro(burst, Rectangle{0.0f, 0.0f, static_cast<float>(burst.width), static_cast<float>(burst.height)},
                           Rectangle{0.0f, 0.0f, size, size}, Vector2{radius, radius}, 0.0f, Color{255, 255, 255, alpha});
            rlPopMatrix();
        }

        void drawBurst(const State &state)
        {
            if (state.burst.id == 0u || state.burstShown < 0.01f)
                return;
            const float width = static_cast<float>(GetScreenWidth()), height = static_cast<float>(GetScreenHeight());
            const float diagonal = std::hypot(width, height);
            float x = width * 0.5f, y = height * 0.3f;
            if (state.logoPlaced)
            {
                x = state.logoMin.x + (state.logoMax.x - state.logoMin.x) * kLogoCentreX;
                y = state.logoMin.y + (state.logoMax.y - state.logoMin.y) * kLogoCentreY;
            }
            const float unit = state.unit, t = state.time, shown = state.burstShown;
            const ImVec2 p = state.parallax;
            if (state.saloonOn) // in the doorway's glare: sized by the logo, drifting with the room
            {
                const float r = (state.logoMax.x - state.logoMin.x) * 0.77f * state.scene.tuning().burst;
                drawBurstLayer(state.burst, x, y, r * (1.0f + 0.02f * std::sin(t * 1.1f + 1.0f)), t * 1.2f,
                               static_cast<unsigned char>(45.0f * shown));
                drawBurstLayer(state.burst, x, y, r * 0.78f * (1.0f + 0.03f * std::sin(t * 1.6f)), 40.0f - t * 2.0f,
                               static_cast<unsigned char>(70.0f * shown));
                return;
            }
            // Back layer: bigger, dimmer, turns one way and shifts the most; front layer: the other way, less.
            drawBurstLayer(state.burst, x - p.x * 36.0f * unit, y - p.y * 36.0f * unit,
                           diagonal * 0.80f * (1.0f + 0.02f * std::sin(t * 1.1f + 1.0f)), t * 1.2f,
                           static_cast<unsigned char>(45.0f * shown));
            drawBurstLayer(state.burst, x - p.x * 18.0f * unit, y - p.y * 18.0f * unit,
                           diagonal * 0.62f * (1.0f + 0.03f * std::sin(t * 1.6f)), 40.0f - t * 2.0f,
                           static_cast<unsigned char>(70.0f * shown));
        }

        float fontSize(const State &state, float base) { return std::round(base * state.unit); }

        void pushSize(const State &state, float base) { ImGui::PushFont(state.font, fontSize(state, base)); }

        // The splash text (Minecraft's): a random line of resources/splash/splash.txt, one per start, yellow with a hard
        // shadow, tilted up across the wordmark's bottom right corner and pulsing. Long lines are drawn smaller, so
        // each stays within 40% of the wordmark's width. Over the screens with the logo (Home, Install), fading out with
        // the burst on Options.
        void loadSplash(State &state, const fs::path &folder)
        {
            std::ifstream in(folder / "splash.txt", std::ios::binary);
            std::vector<std::string> lines;
            std::string line;
            while (std::getline(in, line))
            {
                if (line.size() >= 3 && static_cast<unsigned char>(line[0]) == 0xEF && static_cast<unsigned char>(line[1]) == 0xBB &&
                    static_cast<unsigned char>(line[2]) == 0xBF)
                    line.erase(0, 3); // a UTF-8 byte order mark (Notepad)
                while (!line.empty() && std::isspace(static_cast<unsigned char>(line.back())))
                    line.pop_back();
                size_t start = 0;
                while (start < line.size() && std::isspace(static_cast<unsigned char>(line[start])))
                    ++start;
                if (start < line.size())
                    lines.push_back(line.substr(start));
            }
            // The font is a baked atlas (tools/splash/bake_font.py: splash.fnt + splash.png, Latin-1), not the .ttf:
            // its terms keep the raw file out of the repo, and raylib's own atlas packer clipped and garbled this
            // font's tall glyphs (it assumes none is taller than the font size).
            std::error_code error;
            const fs::path fontPath = folder / "splash.fnt";
            if (lines.empty() || !fs::is_regular_file(fontPath, error))
                return;
            uint32_t listId = 2166136261u;
            for (const std::string &l : lines)
                listId = fnv1a(l + "\n", listId);
            state.splash = lines[static_cast<size_t>(dealFromDeck(state, "SplashDeck", static_cast<int>(lines.size()), listId))];
            state.splashFont = LoadFont(utf8(fontPath).c_str());
            if (state.splashFont.texture.id == 0u || state.splashFont.texture.id == GetFontDefault().texture.id)
            {
                state.splash.clear();
                return;
            }
            GenTextureMipmaps(&state.splashFont.texture);
            SetTextureFilter(state.splashFont.texture, TEXTURE_FILTER_TRILINEAR);
        }

        void drawSplash(const State &state)
        {
            if (state.splash.empty() || !state.logoPlaced || state.burstShown < 0.01f)
                return;
            const float logoWidth = state.logoMax.x - state.logoMin.x, logoHeight = state.logoMax.y - state.logoMin.y;
            const char *text = state.splash.c_str();
            float size = logoWidth * 0.047f;
            const Vector2 measured = MeasureTextEx(state.splashFont, text, size, 0.0f);
            if (measured.x > logoWidth * 0.40f)
                size *= logoWidth * 0.40f / measured.x;
            const Vector2 at{state.logoMin.x + logoWidth * 0.90f, state.logoMin.y + logoHeight * 0.68f}; // on the D's lower right
            // A line too long for the room to the window's right edge (a 4:3 window) tilts steeper, up to 40 degrees,
            // then shrinks (K, Nov 13); judged at the pulse's largest so it holds still.
            float tilt = 18.0f;
            {
                const Vector2 peak = MeasureTextEx(state.splashFont, text, size * 1.05f, 0.0f);
                const float room = static_cast<float>(GetScreenWidth()) - at.x - logoWidth * 0.02f;
                const auto reach = [&](float degrees) {
                    const float r = degrees * 3.14159265f / 180.0f;
                    return 0.5f * peak.x * std::cos(r) + 0.5f * peak.y * std::sin(r);
                };
                while (tilt < 40.0f && reach(tilt) > room)
                    tilt += 1.0f;
                if (room > 1.0f && reach(tilt) > room)
                    size *= room / reach(tilt);
            }
            size *= 1.0f + 0.05f * std::fabs(std::sin(state.time * 3.14159265f)); // the pulse, once a second
            const Vector2 box = MeasureTextEx(state.splashFont, text, size, 0.0f);
            const Vector2 origin{box.x * 0.5f, box.y * 0.5f};
            const float shadow = size * 0.05f; // closer to the letters (K, Nov 13; was 0.085)
            const unsigned char alpha = static_cast<unsigned char>(255.0f * std::clamp(state.burstShown, 0.0f, 1.0f));
            DrawTextPro(state.splashFont, text, Vector2{at.x + shadow, at.y + shadow}, origin, -tilt, size, 0.0f,
                        Color{62, 52, 0, alpha});
            DrawTextPro(state.splashFont, text, at, origin, -tilt, size, 0.0f, Color{255, 236, 64, alpha});
        }

        // Text straight on the background, centred, with a soft dark shadow so it reads over the bright desert.
        void centeredText(const char *text, const ImVec4 &color)
        {
            const float width = ImGui::CalcTextSize(text).x;
            ImGui::SetCursorPosX(std::max(0.0f, (ImGui::GetWindowWidth() - width) * 0.5f));
            const ImVec2 at = ImGui::GetCursorScreenPos();
            ImDrawList *draw = ImGui::GetWindowDrawList();
            const float size = ImGui::GetFontSize(), r = std::max(1.0f, size * 0.09f);
            for (int ring = 1; ring <= 2; ++ring)
                for (int i = 0; i < 12; ++i)
                {
                    const float a = static_cast<float>(i) * 0.5236f;
                    draw->AddText(ImVec2(at.x + std::cos(a) * r * ring, at.y + r * 0.6f + std::sin(a) * r * ring),
                                  IM_COL32(10, 6, 4, ring == 1 ? 46 : 22), text);
                }
            ImGui::TextColored(color, "%s", text);
        }

        bool bigButton(const State &state, const char *label, float width, bool accent)
        {
            if (accent)
            {
                ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.05f, 0.04f, 1.0f));
            }
            pushSize(state, 30.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
            const bool clicked = ImGui::Button(label, ImVec2(width, fontSize(state, 64.0f)));
            ImGui::PopFont();
            if (accent)
                ImGui::PopStyleColor(2);
            return clicked;
        }

        unsigned char alphaOf(float a) { return static_cast<unsigned char>(std::clamp(a, 0.0f, 1.0f) * 255.0f); }

        // ---- the saloon's paper UI (launcher_saloon.h) ---------------------------------------------------------------
        // An invisible button over a box on a paper (the bill, a slip): ImGui gets the box's inscribed rectangle on
        // screen (keyboard / gamepad focus, clicks); moving the mouse over the box itself (exactly, through the paper's
        // perspective) moves the focus there (K: hover moves the focus), without the cursor's tick.
        saloon::Quad paperQuad(Rectangle box, const saloon::Homography &paper)
        {
            saloon::Quad q;
            q.p[0] = paper.map(box.x, box.y);
            q.p[1] = paper.map(box.x + box.width, box.y);
            q.p[2] = paper.map(box.x + box.width, box.y + box.height);
            q.p[3] = paper.map(box.x, box.y + box.height);
            return q;
        }

        bool paperButton(const char *id, Rectangle box, const saloon::Homography &paper, bool hoverFocus, bool *focused = nullptr)
        {
            const saloon::Quad q = paperQuad(box, paper);
            // ImGui's box: the item's middle, as wide as its middle line and as tall as its middle column (a long line
            // on a tilted paper would have no inner box, and its outer one would reach over its neighbours).
            const Vector2 c = paper.map(box.x + box.width * 0.5f, box.y + box.height * 0.5f);
            const Vector2 l = paper.map(box.x, box.y + box.height * 0.5f), r = paper.map(box.x + box.width, box.y + box.height * 0.5f);
            const Vector2 t = paper.map(box.x + box.width * 0.5f, box.y), b = paper.map(box.x + box.width * 0.5f, box.y + box.height);
            const float halfW = std::max(1.0f, std::fabs(r.x - l.x) * 0.5f), halfH = std::max(1.0f, std::fabs(b.y - t.y) * 0.5f);
            const float x0 = c.x - halfW, x1 = c.x + halfW, y0 = c.y - halfH, y1 = c.y + halfH;
            ImGui::SetCursorScreenPos(ImVec2(x0, y0));
            // EnableNav: an invisible button is no keyboard / gamepad stop unless asked; ImGui's own focus frame hidden
            // (the pencil loop shows the focus)
            ImGui::PushStyleColor(ImGuiCol_NavCursor, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
            const bool pressed = ImGui::InvisibleButton(id, ImVec2(std::max(1.0f, x1 - x0), std::max(1.0f, y1 - y0)), ImGuiButtonFlags_EnableNav);
            ImGui::PopStyleColor();
            const ImGuiIO &io = ImGui::GetIO();
            const bool moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
            if (hoverFocus && moved && !ImGui::IsItemFocused() && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
                saloon::inside(q, Vector2{io.MousePos.x, io.MousePos.y}))
                ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
            if (focused != nullptr)
                *focused = ImGui::IsItemFocused();
            return pressed;
        }

        // The corner slip (K, Nov 17): a paper slip poking up from the picture's bottom right edge, in dark ink: the
        // version / update (Add to Steam and the volume moved onto the bill, K Nov 18). A slip's own units: x right, y down; about
        // kSlipShown of its height shows above the edge.
        enum SlipItem
        {
            kSlipNone,
            kSlipNotes,
            kSlipUpdate,
        };
        constexpr float kSlipH = 240.0f, kSlipShown = 168.0f;
        constexpr Color kSlipInk{40, 28, 20, 238};
        constexpr Color kSlipPencil{164, 30, 22, 220};
        constexpr float kSlipPi = 3.14159265f;
        struct Slip
        {
            saloon::Homography paper;
            saloon::Quad quad;
            float w = 0.0f, h = kSlipH;
        };
        Slip slipAt(const State &state, bool right)
        {
            const Rectangle pic = state.scene.picture();
            const float k = pic.height / 900.0f * 0.56f; // screen pixels a slip unit
            Slip slip;
            slip.w = 420.0f;
            const float angle = (right ? -1.4f : 1.8f) * DEG2RAD, c = std::cos(angle), sn = std::sin(angle);
            const float margin = pic.height * 0.03f;
            const float cx = right ? pic.x + pic.width - margin - slip.w * k * 0.5f : pic.x + margin + slip.w * k * 0.5f;
            const float cy = pic.y + pic.height - kSlipShown * k + slip.h * k * 0.5f;
            const float hw = slip.w * 0.5f, hh = slip.h * 0.5f;
            const Vector2 corners[4] = {{-hw, -hh}, {hw, -hh}, {hw, hh}, {-hw, hh}};
            for (int i = 0; i < 4; ++i)
                slip.quad.p[i] = Vector2{cx + (corners[i].x * c - corners[i].y * sn) * k, cy + (corners[i].x * sn + corners[i].y * c) * k};
            slip.paper = saloon::Homography::rectToQuad(slip.w, slip.h, slip.quad);
            return slip;
        }

        // A slip's paper: a soft shadow, then the paper (its rim a little darker), faded by `alpha`.
        void drawSlipPaper(const State &state, const Slip &slip, float alpha)
        {
            const Texture2D &paper = state.scene.paper();
            if (alpha <= 0.0f || paper.id == 0u)
                return;
            // the shadow: three widening quads, down and out a little
            const Vector2 centre{(slip.quad.p[0].x + slip.quad.p[2].x) * 0.5f, (slip.quad.p[0].y + slip.quad.p[2].y) * 0.5f};
            for (int ring = 3; ring >= 1; --ring)
            {
                const float grow = 3.0f * static_cast<float>(ring);
                Vector2 q[4];
                for (int i = 0; i < 4; ++i)
                {
                    const float dx = slip.quad.p[i].x - centre.x, dy = slip.quad.p[i].y - centre.y, d = std::max(1.0f, std::hypot(dx, dy));
                    q[i] = Vector2{slip.quad.p[i].x + dx / d * grow + 2.0f, slip.quad.p[i].y + dy / d * grow + 4.0f};
                }
                const Color shade{0, 0, 0, alphaOf(0.10f * alpha)};
                DrawTriangle(q[0], q[3], q[2], shade);
                DrawTriangle(q[0], q[2], q[1], shade);
            }
            // the paper, 4x4 cells: the rim's vertices darker
            constexpr int kCells = 4;
            const float tile = 300.0f; // slip units a tile of the paper texture
            rlDisableBackfaceCulling();
            rlSetTexture(paper.id);
            rlBegin(RL_QUADS);
            for (int y = 0; y < kCells; ++y)
                for (int x = 0; x < kCells; ++x)
                {
                    const int corners[4][2] = {{x, y}, {x, y + 1}, {x + 1, y + 1}, {x + 1, y}};
                    for (const auto &corner : corners)
                    {
                        const float u = slip.w * static_cast<float>(corner[0]) / kCells, v = slip.h * static_cast<float>(corner[1]) / kCells;
                        const bool rim = corner[0] == 0 || corner[0] == kCells || corner[1] == 0;
                        const float shade = rim ? 0.80f : 1.0f;
                        rlColor4ub(alphaOf(shade), alphaOf(shade * 0.96f), alphaOf(shade * 0.92f), alphaOf(alpha));
                        rlTexCoord2f(u / tile, v / tile);
                        const Vector2 at = slip.paper.map(u, v);
                        rlVertex2f(at.x, at.y);
                    }
                }
            rlEnd();
            rlSetTexture(0);
            rlDrawRenderBatchActive();
            rlEnableBackfaceCulling();
        }

        // What sits on the slips, in slip units.
        constexpr Vector2 kNotesAt{110.0f, 140.0f}, kUpdateAt{285.0f, 140.0f};

        Rectangle slipTextBox(const Font &font, const char *text, float size, Vector2 centre)
        {
            const float w = saloon::inkWidth(font, text, size);
            return Rectangle{centre.x - w * 0.5f - 14.0f, centre.y - size * 0.6f, w + 28.0f, size * 1.2f};
        }

        bool updateAvailable() { return g_updateCheck.state.load() == static_cast<int>(update::CheckState::Available); }
#if KAMITE_LAUNCHER_STEAM
        const char *steamLabel(const State &state) { return state.steam.added ? "Update Steam Entry" : "Add to Steam"; }
#endif

        // The slips' ink (before ImGui, from last frame's focus): their words, the speaker, the ruler, the loop.
        void drawSlipInk(const State &state, const Slip &slip, bool right, float alpha)
        {
            const saloon::BillFonts &f = state.billFonts;
            Color ink = kSlipInk, pencil = kSlipPencil;
            ink.a = alphaOf(ink.a / 255.0f * alpha);
            pencil.a = alphaOf(pencil.a / 255.0f * alpha);
            const auto loop = [&](Rectangle box, uint32_t seed) { saloon::inkLoop(box, 1.0f, seed, pencil, slip.paper, 4.0f); };
            saloon::beginInk();
            if (right)
            {
                const std::string version = update::versionLabel();
                const float vw = saloon::inkWidth(f.text, version.c_str(), 32.0f);
                const float scale = vw > 340.0f ? 340.0f / vw : 1.0f; // a dev build's long label
                saloon::inkText(f.text, version.c_str(), Vector2{40.0f + vw * scale * 0.5f, 48.0f}, 32.0f * scale, ink, slip.paper);
                if (updateAvailable() && state.screen == Screen::Home)
                {
                    const std::string out = g_updateCheck.release.tag + " is out";
                    const float ow = saloon::inkWidth(f.text, out.c_str(), 26.0f);
                    saloon::inkText(f.text, out.c_str(), Vector2{40.0f + ow * 0.5f, 88.0f}, 26.0f, ink, slip.paper);
                    saloon::inkText(f.item, "What's new", kNotesAt, 34.0f, ink, slip.paper);
                    saloon::inkText(f.item, "Update", kUpdateAt, 34.0f, ink, slip.paper);
                    if (state.slipFocus == kSlipNotes)
                        loop(slipTextBox(f.item, "What's new", 34.0f, kNotesAt), 21u);
                    if (state.slipFocus == kSlipUpdate)
                        loop(slipTextBox(f.item, "Update", 34.0f, kUpdateAt), 22u);
                }
            }
            saloon::endInk();
        }

        // The lower right corner's volume: a mute button (a speaker, crossed out when muted) left of a slider; both kept
        // in GHR.ini.
        void drawVolume(State &state)
        {
            if (state.saloonOn) // on the bill (drawSaloonHome)
                return;

            const float size = fontSize(state, 34.0f), margin = fontSize(state, 22.0f), slider = fontSize(state, 200.0f);
            const float gap = fontSize(state, 8.0f);
            ImGui::SetCursorPos(ImVec2(ImGui::GetWindowWidth() - margin - slider - gap - size, ImGui::GetWindowHeight() - margin - size));
            const ImVec2 p = ImGui::GetCursorScreenPos();
            if (ImGui::InvisibleButton("##mute", ImVec2(size, size)))
            {
                state.muted = !state.muted;
                state.ini.setBool("Launcher", "Mute", state.muted);
                state.ini.save();
            }
            const bool lit = ImGui::IsItemHovered() || ImGui::IsItemFocused();
            const ImU32 ink = ImGui::GetColorU32(lit ? kAccent : kText);
            ImDrawList *draw = ImGui::GetWindowDrawList();
            const auto at = [&](float x, float y) { return ImVec2(p.x + x * size, p.y + y * size); };
            draw->AddRectFilled(at(0.14f, 0.38f), at(0.32f, 0.62f), ink);
            const ImVec2 cone[4] = {at(0.31f, 0.38f), at(0.54f, 0.18f), at(0.54f, 0.82f), at(0.31f, 0.62f)};
            draw->AddConvexPolyFilled(cone, 4, ink);
            const float stroke = std::max(1.5f, size * 0.07f);
            if (state.muted || state.volume <= 0.0f)
            {
                draw->AddLine(at(0.66f, 0.36f), at(0.88f, 0.64f), ink, stroke);
                draw->AddLine(at(0.66f, 0.64f), at(0.88f, 0.36f), ink, stroke);
            }
            else
                for (int wave = 0; wave < (state.volume > 0.5f ? 2 : 1); ++wave)
                {
                    draw->PathArcTo(at(0.54f, 0.5f), size * (0.17f + 0.15f * static_cast<float>(wave)), -0.85f, 0.85f, 12);
                    draw->PathStroke(ink, ImDrawFlags_None, stroke);
                }
            ImGui::SameLine(0.0f, gap);
            ImGui::SetCursorPosY(ImGui::GetWindowHeight() - margin - size + (size - ImGui::GetFrameHeight()) * 0.5f);
            ImGui::SetNextItemWidth(slider);
            float percent = state.volume * 100.0f;
            if (ImGui::SliderFloat("##volume", &percent, 0.0f, 100.0f, "%.0f%%", ImGuiSliderFlags_AlwaysClamp))
            {
                state.volume = percent / 100.0f;
                if (state.muted && state.volume > 0.0f)
                {
                    state.muted = false;
                    state.ini.setBool("Launcher", "Mute", false);
                }
            }
            state.volumeSliderId = ImGui::GetItemID();
            if (ImGui::IsItemDeactivatedAfterEdit())
            {
                char text[16];
                std::snprintf(text, sizeof(text), "%.2f", state.volume);
                state.ini.set("Launcher", "Volume", text);
                state.ini.save();
            }
        }

        // What's new: the release notes (CHANGELOG lines), "# v0.7.0" per release, "## <date>" headings.
        void drawNotes(const State &state, const std::string &notes)
        {
            size_t at = 0;
            while (at <= notes.size())
            {
                size_t end = notes.find('\n', at);
                if (end == std::string::npos)
                    end = notes.size();
                std::string line = notes.substr(at, end - at);
                at = end + 1;
                line.erase(std::remove(line.begin(), line.end(), '`'), line.end());
                for (size_t bold; (bold = line.find("**")) != std::string::npos;)
                    line.erase(bold, 2);
                if (line.rfind("# ", 0) == 0)
                {
                    pushSize(state, 26.0f);
                    ImGui::TextColored(kHeading, "%s", line.c_str() + 2);
                    ImGui::PopFont();
                }
                else if (line.rfind("## ", 0) == 0)
                    ImGui::TextColored(kTextDim, "%s", line.c_str() + 3);
                else if (!line.empty())
                    ImGui::TextWrapped("%s", line.c_str());
            }
        }

        void startUpdate(State &state)
        {
            // The launcher's music and the bar's murmur stream from resources: closed so the files can be replaced.
            if (state.hasMusic)
            {
                StopMusicStream(state.music);
                UnloadMusicStream(state.music);
                state.hasMusic = false;
            }
            if (state.hasAmbience)
            {
                StopMusicStream(state.ambience);
                UnloadMusicStream(state.ambience);
                state.hasAmbience = false;
            }
            update::startInstall(g_updater, g_updateCheck.release, state.context->exeDir);
            state.updatePopup = State::UpdatePopup::Working;
        }

        // Home's lower right corner, over the volume (claude/release.md; K restyles it later): the version, and when a
        // newer release is out, What's new and Update.
        void drawUpdate(State &state)
        {
            using Popup = State::UpdatePopup;
            const float size = fontSize(state, 34.0f), margin = fontSize(state, 22.0f), gap = fontSize(state, 12.0f);
            const bool available = g_updateCheck.state.load() == static_cast<int>(update::CheckState::Available);
            const float right = ImGui::GetWindowWidth() - margin;
            float y = ImGui::GetWindowHeight() - margin - size - gap;
            const update::Release &release = g_updateCheck.release;
            if (state.saloonOn) // on the right slip (its ink is drawn before ImGui)
            {
                if (available)
                {
                    const Slip slip = slipAt(state, true);
                    bool focused = false;
                    if (paperButton("##notes", slipTextBox(state.billFonts.item, "What's new", 34.0f, kNotesAt), slip.paper, true, &focused))
                    {
                        state.updatePopup = Popup::Notes;
                        state.updateOpenPopup = true;
                    }
                    if (focused)
                        state.slipFocus = kSlipNotes;
                    if (paperButton("##update", slipTextBox(state.billFonts.item, "Update", 34.0f, kUpdateAt), slip.paper, true, &focused))
                    {
                        startUpdate(state);
                        state.updateOpenPopup = true;
                    }
                    if (focused)
                        state.slipFocus = kSlipUpdate;
                }
            }
            else if (available)
            {
                pushSize(state, 20.0f);
                const ImVec2 pad = ImGui::GetStyle().FramePadding;
                const float updateWidth = ImGui::CalcTextSize("Update").x + pad.x * 2.0f;
                const float notesWidth = ImGui::CalcTextSize("What's new").x + pad.x * 2.0f;
                y -= size;
                ImGui::SetCursorPos(ImVec2(right - updateWidth - gap - notesWidth, y));
                if (ImGui::Button("What's new", ImVec2(notesWidth, size)))
                {
                    state.updatePopup = Popup::Notes;
                    state.updateOpenPopup = true;
                }
                ImGui::SameLine(0.0f, gap);
                ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.05f, 0.04f, 1.0f));
                if (ImGui::Button("Update", ImVec2(updateWidth, size)))
                {
                    startUpdate(state);
                    state.updateOpenPopup = true;
                }
                ImGui::PopStyleColor(2);
                ImGui::PopFont();
                y -= gap * 0.6f;
            }
            if (!state.saloonOn)
            {
                pushSize(state, 18.0f);
                const std::string label = update::versionLabel() + (available ? "  \xC2\xB7  " + release.tag + " is out" : std::string());
                const ImVec2 textSize = ImGui::CalcTextSize(label.c_str());
                ImGui::SetCursorPos(ImVec2(right - textSize.x, y - textSize.y));
                ImGui::TextColored(kTextSoft, "%s", label.c_str());
                ImGui::PopFont();
            }

            if (state.updateOpenPopup)
            {
                state.updateOpenPopup = false;
                ImGui::OpenPopup("##update");
            }
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            if (!ImGui::BeginPopupModal("##update", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
                return;
            pushSize(state, 22.0f);
            const float width = fontSize(state, 640.0f), buttonWidth = fontSize(state, 200.0f);
            ImGui::Dummy(ImVec2(width, fontSize(state, 4.0f)));
            const bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            bool close = false;
            if (state.updatePopup == Popup::Working)
            {
                const auto stage = static_cast<update::Stage>(g_updater.stage.load());
                if (stage == update::Stage::Failed)
                {
                    if (g_updater.worker.joinable())
                        g_updater.worker.join();
                    state.updatePopup = Popup::Failed;
                }
                else if (stage == update::Stage::Done)
                {
                    if (g_updater.worker.joinable())
                        g_updater.worker.join();
                    KM_INFO(Boot, "launcher: restarting after the update");
                    platform::relaunch(state.exePath);
                    state.result = Result::Quit;
                    state.done = true;
                }
            }
            switch (state.updatePopup)
            {
            case Popup::Notes:
            {
                pushSize(state, 26.0f);
                ImGui::TextColored(kHeading, "What's new");
                ImGui::PopFont();
                ImGui::BeginChild("##notes", ImVec2(width, fontSize(state, 380.0f)));
                // Text only (nothing to put the cursor on): the arrows, the d-pad and either stick scroll it.
                {
                    float scroll = 0.0f;
                    for (const ImGuiKey key : {ImGuiKey_UpArrow, ImGuiKey_GamepadDpadUp, ImGuiKey_GamepadLStickUp, ImGuiKey_GamepadRStickUp})
                        scroll -= ImGui::GetKeyData(key)->AnalogValue > 0.0f ? ImGui::GetKeyData(key)->AnalogValue : (ImGui::IsKeyDown(key) ? 1.0f : 0.0f);
                    for (const ImGuiKey key : {ImGuiKey_DownArrow, ImGuiKey_GamepadDpadDown, ImGuiKey_GamepadLStickDown, ImGuiKey_GamepadRStickDown})
                        scroll += ImGui::GetKeyData(key)->AnalogValue > 0.0f ? ImGui::GetKeyData(key)->AnalogValue : (ImGui::IsKeyDown(key) ? 1.0f : 0.0f);
                    if (scroll != 0.0f)
                        ImGui::SetScrollY(ImGui::GetScrollY() + scroll * fontSize(state, 900.0f) * ImGui::GetIO().DeltaTime);
                }
                pushSize(state, 19.0f);
                drawNotes(state, release.notes);
                ImGui::PopFont();
                ImGui::EndChild();
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 6.0f)));
                if (ImGui::IsWindowAppearing())
                    ImGui::SetKeyboardFocusHere();
                ImGui::PushStyleColor(ImGuiCol_Button, kAccent);
                ImGui::PushStyleColor(ImGuiCol_Text, ImVec4(0.08f, 0.05f, 0.04f, 1.0f));
                const bool go = ImGui::Button("Update", ImVec2(buttonWidth, 0.0f));
                ImGui::PopStyleColor(2);
                ImGui::SameLine();
                const bool shut = ImGui::Button("Close", ImVec2(buttonWidth, 0.0f));
                ownSoundItem();
                if (go)
                    startUpdate(state);
                else if (shut || back)
                {
                    menuSound(state, kCancel);
                    close = true;
                }
                break;
            }
            case Popup::Working:
            {
                const uint64_t got = g_updater.received.load(), total = g_updater.total;
                if (static_cast<update::Stage>(g_updater.stage.load()) == update::Stage::Downloading)
                {
                    if (total > 0)
                        ImGui::Text("Downloading %s...  %llu of %llu MB", release.tag.c_str(),
                                    static_cast<unsigned long long>(got >> 20), static_cast<unsigned long long>((total + (1u << 20) - 1) >> 20));
                    else
                        ImGui::Text("Downloading %s...  %llu MB", release.tag.c_str(), static_cast<unsigned long long>(got >> 20));
                }
                else
                    ImGui::Text("Installing %s...", release.tag.c_str());
                ImGui::PushStyleColor(ImGuiCol_PlotHistogram, kAccent);
                ImGui::ProgressBar(total > 0 ? std::min(1.0f, static_cast<float>(got) / static_cast<float>(total)) : 0.0f,
                                   ImVec2(width, fontSize(state, 24.0f)), "");
                ImGui::PopStyleColor();
                pushSize(state, 17.0f);
                ImGui::TextColored(kTextDim, "The game restarts by itself when it's done. Your settings and saves stay.");
                ImGui::PopFont();
                break;
            }
            case Popup::Failed:
            {
                ImGui::TextUnformatted("The update didn't go through. Nothing was changed.");
                ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
                ImGui::TextColored(kTextDim, "%s", g_updater.error.c_str());
                ImGui::PopTextWrapPos();
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 10.0f)));
                if (ImGui::IsWindowAppearing())
                    ImGui::SetKeyboardFocusHere();
                const bool ok = ImGui::Button("OK", ImVec2(buttonWidth, 0.0f));
                if (ok || back)
                    close = true;
                break;
            }
            case Popup::None:
                close = true;
                break;
            }
            if (close)
            {
                state.updatePopup = Popup::None;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopFont();
            ImGui::EndPopup();
        }

#if KAMITE_LAUNCHER_STEAM
        void startSteamJob(State &state)
        {
            if (state.steamWorker.joinable())
                state.steamWorker.join();
            state.steamStage = static_cast<int>(steam::Stage::Downloading);
            state.steamPopup = State::SteamPopup::Working;
            const fs::path resources = state.context->exeDir / "resources";
            state.steamWorker = std::thread([&state, resources] { state.steamResult = steam::addOrUpdate(resources, state.steamStage); });
        }

        // Add to Steam, or Update Steam Entry once it is there. Steam reads its shortcut list only when it starts, so a
        // running Steam is restarted, after asking.
        void pressSteam(State &state)
        {
            if (steam::running())
                state.steamPopup = State::SteamPopup::Confirm;
            else
                startSteamJob(state);
            state.steamOpenPopup = true;
        }

        // Add to Steam: Steam's mark in the bill's lower left corner in the saloon (K, Nov 18; pressed there), else a
        // button in the lower left corner; hidden without Steam. Then its popup.
        void drawSteam(State &state)
        {
            using Popup = State::SteamPopup;
            if (!state.steam.available)
                return;
            if (!state.saloonOn)
            {
                const float size = fontSize(state, 34.0f), margin = fontSize(state, 22.0f);
                pushSize(state, 20.0f);
                ImGui::SetCursorPos(ImVec2(margin, ImGui::GetWindowHeight() - margin - size));
                if (ImGui::Button(steamLabel(state), ImVec2(0.0f, size)))
                    pressSteam(state);
                ImGui::PopFont();
            }
            if (state.steamOpenPopup)
            {
                state.steamOpenPopup = false;
                ImGui::OpenPopup("##steam");
            }
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            if (!ImGui::BeginPopupModal("##steam", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
                return;
            pushSize(state, 22.0f);
            ImGui::Dummy(ImVec2(fontSize(state, 460.0f), fontSize(state, 4.0f)));
            const float buttonWidth = fontSize(state, 200.0f);
            const bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            bool close = false;
            if (state.steamPopup == Popup::Working && state.steamStage.load() == static_cast<int>(steam::Stage::Done))
            {
                state.steamWorker.join();
                state.steam = steam::probe();
                state.steamPopup = Popup::Done;
                state.steamFocus = true;
            }
            switch (state.steamPopup)
            {
            case Popup::Confirm:
            {
                ImGui::TextUnformatted("Steam needs to restart to show God Hand: Rearmed.");
                ImGui::TextUnformatted("Restart Steam now?");
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 10.0f)));
                if (ImGui::IsWindowAppearing() || state.steamFocus)
                {
                    ImGui::SetKeyboardFocusHere();
                    state.steamFocus = false;
                }
                const bool yes = ImGui::Button("Restart Steam", ImVec2(buttonWidth, 0.0f));
                ownSoundItem();
                ImGui::SameLine();
                const bool no = ImGui::Button("Cancel", ImVec2(buttonWidth, 0.0f));
                ownSoundItem();
                if (yes)
                {
                    menuSound(state, kConfirm);
                    startSteamJob(state);
                }
                else if (no || back)
                {
                    menuSound(state, kCancel);
                    close = true;
                }
                break;
            }
            case Popup::Working:
            {
                const char *text = "Downloading the library art";
                switch (static_cast<steam::Stage>(state.steamStage.load()))
                {
                case steam::Stage::ClosingSteam:
                    text = "Closing Steam";
                    break;
                case steam::Stage::Writing:
                    text = "Adding God Hand: Rearmed to Steam";
                    break;
                case steam::Stage::StartingSteam:
                    text = "Starting Steam";
                    break;
                default:
                    break;
                }
                const int dots = static_cast<int>(GetTime() * 3.0) % 4;
                ImGui::Text("%s%.*s", text, dots, "...");
                break;
            }
            case Popup::Done:
            {
                ImGui::TextUnformatted(state.steamResult.message.c_str());
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 10.0f)));
                if (ImGui::IsWindowAppearing() || state.steamFocus)
                {
                    ImGui::SetKeyboardFocusHere();
                    state.steamFocus = false;
                }
                const bool ok = ImGui::Button("OK", ImVec2(buttonWidth, 0.0f));
                ownSoundItem();
                if (ok)
                {
                    menuSound(state, kConfirm);
                    close = true;
                }
                else if (back)
                {
                    menuSound(state, kCancel);
                    close = true;
                }
                break;
            }
            case Popup::None:
                close = true;
                break;
            }
            if (close)
            {
                state.steamPopup = Popup::None;
                ImGui::CloseCurrentPopup();
            }
            ImGui::PopFont();
            ImGui::EndPopup();
        }
#endif

        void drawLayer(const Texture2D &texture, const ImVec2 &min, const ImVec2 &max, Color tint)
        {
            if (texture.id == 0u || tint.a == 0)
                return;
            DrawTexturePro(texture, Rectangle{0.0f, 0.0f, static_cast<float>(texture.width), static_cast<float>(texture.height)},
                           Rectangle{min.x, min.y, max.x - min.x, max.y - min.y}, Vector2{0.0f, 0.0f}, 0.0f, tint);
        }

        float ramp(float t, float a, float b) { return std::clamp((t - a) / (b - a), 0.0f, 1.0f); }

        // Smooth in and out (smoothstep), so no step lands as a pop at 60 Hz.
        float ease(float t, float a, float b)
        {
            const float x = ramp(t, a, b);
            return x * x * (3.0f - 2.0f * x);
        }

        // A horizontal band from (x0) fading to nothing at (x1), either way (float edges: no pixel jitter).
        void band(float x0, float x1, float y, float h, Color c)
        {
            Color clear = c;
            clear.a = 0;
            const float left = std::min(x0, x1), width = std::fabs(x1 - x0);
            DrawRectangleGradientEx(Rectangle{left, y, width, std::max(1.0f, h)}, x1 >= x0 ? c : clear, x1 >= x0 ? c : clear,
                                    x1 >= x0 ? clear : c, x1 >= x0 ? clear : c);
        }

        // A layer scaled about a point (the glow's bloom rings).
        void drawLayerScaled(const Texture2D &texture, ImVec2 min, ImVec2 max, float cx, float cy, float sx, float sy, Color tint)
        {
            drawLayer(texture, ImVec2(cx + (min.x - cx) * sx, cy + (min.y - cy) * sy), ImVec2(cx + (max.x - cx) * sx, cy + (max.y - cy) * sy),
                      tint);
        }

        // The REARMED plate. t < 0: at rest; t >= 0: seconds since Play (the roulette pick, both ways from the plate).
        // K Nov 10: the plate no longer smears out to the screen edges; the letters bloom (additive glow rings, a hot
        // core and a soft flare) before the bar; every step eased.
        void drawPlate(const State &state, float t)
        {
            const ImVec2 min = state.logoMin, max = state.logoMax;
            const float w = max.x - min.x, h = max.y - min.y;
            const Color gold{240, 206, 90, 255};
            if (t < 0.0f)
            {
                drawLayer(state.rearmed, min, max, WHITE);
                drawLayer(state.rearmedText, min, max, gold);
                return;
            }
            const float px = min.x + w * kRearmedRect[0], py = min.y + h * kRearmedRect[1];
            const float pw = w * (kRearmedRect[2] - kRearmedRect[0]), ph = h * (kRearmedRect[3] - kRearmedRect[1]);
            const float cx = px + pw * 0.5f, cy = py + ph * 0.5f, screen = static_cast<float>(GetScreenWidth());
            const float reach = std::max(cx, screen - cx); // from the plate's centre to the farther screen edge
            const float glow = ease(t, 0.02f, 0.09f);                                // the letters heat up
            const float bloom = ease(t, 0.03f, 0.10f) * (1.0f - ease(t, 0.15f, 0.24f)); // their glow spreading
            const float bar = ease(t, 0.09f, 0.14f) * (1.0f - ease(t, 0.16f, 0.22f));
            const float streak = ease(t, 0.15f, 0.20f) * (1.0f - ease(t, 0.22f, 0.275f));
            const float live = 1.0f - ease(t, 0.24f, 0.28f), body = 1.0f - ease(t, 0.06f, 0.13f);
            drawLayer(state.rearmed, min, max, Color{255, 255, 255, alphaOf(body)});
            if (bar > 0.0f)
            {
                const float y0 = py + ph * 0.18f, bh = ph * 0.64f, a = bar * live;
                for (const float side : {-1.0f, 1.0f})
                {
                    band(cx, cx + side * reach * (0.4f + 0.6f * bar), y0, bh, Color{255, 226, 90, alphaOf(0.9f * a)});
                    band(cx, cx + side * reach * (0.3f + 0.4f * bar), y0 + bh * 0.1f, bh * 0.8f, Color{255, 250, 215, alphaOf(0.6f * a)});
                }
            }
            if (streak > 0.0f)
            {
                const float k = streak * live, thick = std::max(1.0f, ph / 45.0f);
                const float lines[5][3] = {{0.38f, 0.95f, 3.0f}, {0.5f, 0.7f, 2.0f}, {0.62f, 1.0f, 3.0f},
                                           {0.45f, 0.5f, 2.0f}, {0.56f, 0.85f, 2.0f}};
                for (const auto &line : lines)
                    for (const float side : {-1.0f, 1.0f})
                    {
                        const float y = py + ph * line[0], th = thick * line[2];
                        band(cx, cx + side * reach * line[1], y - th, th * 2.0f, Color{255, 170, 40, alphaOf(k)});
                        band(cx, cx + side * reach * line[1], y - th * 0.4f, th * 0.8f, Color{255, 245, 190, alphaOf(0.7f * k)});
                    }
            }
            // The letters: gold heating to a white-hot core, the glow blooming out in rings with a soft flare through
            // them (all additive, so it lights what is behind instead of only brightening the letters).
            drawLayer(state.rearmedText, min, max, Color{gold.r, gold.g, gold.b, alphaOf((1.0f - glow) * live)});
            if (glow > 0.0f || bloom > 0.0f)
            {
                BeginBlendMode(BLEND_ADDITIVE);
                const float a = live;
                const struct
                {
                    float sx, sy, alpha;
                    Color color;
                } rings[] = {{1.0f, 1.0f, 0.9f, {255, 120, 20, 255}},
                             {1.06f, 1.35f, 0.65f, {255, 140, 40, 255}},
                             {1.14f, 1.9f, 0.45f, {255, 110, 20, 255}},
                             {1.26f, 2.6f, 0.28f, {255, 90, 10, 255}}};
                for (const auto &ring : rings)
                {
                    const float grow = 1.0f + (ring.sx - 1.0f) * (0.5f + 0.5f * bloom), growY = 1.0f + (ring.sy - 1.0f) * (0.5f + 0.5f * bloom);
                    Color c = ring.color;
                    c.a = alphaOf(ring.alpha * std::max(glow * 0.6f, bloom) * a);
                    drawLayerScaled(state.rearmedGlow, min, max, cx, cy, grow, growY, c);
                }
                // The flare: a soft horizontal light through the letters' middle.
                const float fh = ph * (0.5f + 0.3f * bloom), fa = 0.55f * bloom * a;
                for (const float side : {-1.0f, 1.0f})
                {
                    band(cx, cx + side * pw * (0.55f + 0.5f * bloom), cy - fh * 0.5f, fh, Color{255, 150, 50, alphaOf(fa * 0.6f)});
                    band(cx, cx + side * pw * (0.45f + 0.35f * bloom), cy - fh * 0.12f, fh * 0.24f, Color{255, 235, 190, alphaOf(fa)});
                }
                EndBlendMode();
                drawLayer(state.rearmedText, min, max, Color{255, 200, 90, alphaOf(glow * a)});
                BeginBlendMode(BLEND_ADDITIVE);
                drawLayer(state.rearmedText, min, max, Color{255, 230, 170, alphaOf(0.75f * glow * a)}); // the white-hot core
                drawLayer(state.rearmedGlow, min, max, Color{255, 200, 120, alphaOf(0.5f * bloom * a)});
                EndBlendMode();
            }
        }

        // The logo, after ImGui laid it out: the kana behind (moved a little with the cursor; in the saloon the scene
        // draws it outside the doorway), GOD HAND, the plate.
        // While Play's animation runs the plate is drawn later, over the fade.
        // force: draw last frame's placement (before ImGui, under an open popup).
        void drawLogo(const State &state, bool force = false)
        {
            if (!(state.logoThisFrame || (force && state.logoPlaced)))
                return;
            if (!state.saloonOn) // (in the saloon the kana is outside the doorway, behind the doors: the scene draws it)
            {
                const ImVec2 shift(state.parallax.x * fontSize(state, 10.0f), state.parallax.y * fontSize(state, 7.0f));
                drawLayer(state.kana, ImVec2(state.logoMin.x + shift.x, state.logoMin.y + shift.y),
                          ImVec2(state.logoMax.x + shift.x, state.logoMax.y + shift.y), WHITE);
            }
            drawLayer(state.logo, state.logoMin, state.logoMax, WHITE);
            if (state.playTime < 0.0f)
                drawPlate(state, -1.0f);
        }

        void title(State &state)
        {
            if (state.saloonOn) // the logo is in the doorway (placed before ImGui): the screen starts under it
            {
                const float below = state.logoPlaced ? state.logoMax.y - ImGui::GetCursorScreenPos().y : fontSize(state, 300.0f);
                ImGui::Dummy(ImVec2(0.0f, std::max(fontSize(state, 24.0f), below + fontSize(state, 12.0f))));
                return;
            }
            if (state.logo.id != 0u && state.logo.height > 0)
            {
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 24.0f)));
                // Big: as wide as fits, at most half the window's height. Only laid out here: drawLogo draws the
                // layers after ImGui (tints, the additive glow and the Play animation need raylib).
                const float aspect = static_cast<float>(state.logo.height) / static_cast<float>(state.logo.width);
                const float width = std::min({ImGui::GetWindowWidth() * 0.92f, fontSize(state, 1300.0f),
                                              ImGui::GetWindowHeight() * 0.50f / aspect});
                const float height = width * aspect;
                const ImVec2 start((ImGui::GetWindowWidth() - width) * 0.5f, ImGui::GetCursorPosY());
                ImGui::SetCursorPos(start);
                const ImVec2 screenStart = ImGui::GetCursorScreenPos();
                ImGui::Dummy(ImVec2(width, height));
                state.logoMin = screenStart;
                state.logoMax = ImVec2(screenStart.x + width, screenStart.y + height);
                state.logoPlaced = true;
                state.logoThisFrame = true;
                ImGui::SetCursorPos(ImVec2(0.0f, start.y + height));
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 6.0f)));
            }
            else
            {
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 70.0f)));
                pushSize(state, 92.0f);
                centeredText("GOD HAND: REARMED", ImVec4(1.0f, 0.62f, 0.30f, 1.0f));
                ImGui::PopFont();
            }
            pushSize(state, 22.0f);
            centeredText("God Hand\u2122 for PS2 Recompiled, Refined, Rearmed.", kText);
            ImGui::PopFont();
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 40.0f)));
        }

        // ---- Home in the saloon (K, Nov 18): the menu on the bill of fare --------------------------------------------
        // The logo's GOD HAND + plate box in logo.png's canvas (tools/logo: placed in the doorway by that box).
        constexpr float kLogoContent[4] = {0.1225f, 0.3679f, 0.8968f, 0.9002f};

        // The bill's items: Play (Install when not installed), Options, Quit, then Skip Launcher (installed) and Steam's
        // mark (Steam found).
        constexpr size_t kBillOptions = 1;

        void openOptions(State &state, bool sound = true)
        {
            g_hotkeysOpen = false;
            if (sound)
                bookSound(state, true);
            saloon::Scene &scene = state.scene;
            if (!scene.zoomBroken() && !scene.zoomReady())
            {
                state.zoomPending = true; // its frames are still being read (the first moments): it starts when they are
                return;
            }
            if (scene.zoomBroken())
            {
                state.screen = Screen::Options; // straight there, over Home darkened
                state.focusFirst = true;
                state.optionsShown = 0.0f;
                state.optionsLeaving = false;
                return;
            }
            scene.zoomIn(); // the main loop opens Options when it lands
        }

        void drawSaloonHome(State &state)
        {
            if (state.installed && state.firstRunPending)
            {
                state.screen = Screen::FirstRun; // a new install whose disc image was found without the Install screen
                state.focusFirst = true;
                return;
            }
            saloon::Scene &scene = state.scene;
            const ImGuiIO &io = ImGui::GetIO();
            const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
            const bool home = scene.atHome() && state.playTime < 0.0f;
            // Through the zoom only Back counts: it reverses from the frame showing.
            if (scene.zoomingIn() && !popup &&
                (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false)))
            {
                scene.zoomOut();
                bookSound(state, false);
            }
            saloon::Bill &bill = state.bill;
            bill.items = {state.installed ? "Play" : "Install", "Options", "Quit"};
            bill.splash = state.splash;
            bill.showSkip = state.installed;
            bill.skip = !state.ini.getBool("Launcher", "Show", true);
#if KAMITE_LAUNCHER_STEAM
            bill.steam = state.steam.available;
            bill.steamAdded = state.steam.added;
#else
            bill.steam = false;
#endif
            bill.volume = true;
            bill.level = state.volume;
            bill.muted = state.muted;
            const size_t skipItem = bill.showSkip ? bill.items.size() : SIZE_MAX;
            [[maybe_unused]] const size_t steamItem = bill.steam ? bill.items.size() + (bill.showSkip ? 1u : 0u) : SIZE_MAX;
            const size_t muteItem = bill.items.size() + (bill.showSkip ? 1u : 0u) + (bill.steam ? 1u : 0u);
            bool overTarget = false;
            saloon::Quad quad;
            int focus = -1;
            bool billShown = false;
            const Vector2 mouse{io.MousePos.x, io.MousePos.y};
            const bool moved = io.MouseDelta.x != 0.0f || io.MouseDelta.y != 0.0f;
            const bool overBook = home && !popup && scene.bookHit(mouse);
            if (scene.bill(quad))
            {
                billShown = true;
                const saloon::Homography paper = saloon::Homography::rectToQuad(saloon::kPaperW, saloon::kPaperH, quad);
                const std::vector<Rectangle> targets = saloon::billTargets(state.billFonts, bill);
                if (!home)
                    ImGui::BeginDisabled();
                for (size_t i = 0; i < targets.size(); ++i)
                {
                    ImGui::PushID(static_cast<int>(i));
                    bool focused = false;
                    const bool pressed = paperButton("##bill", targets[i], paper, home, &focused);
                    overTarget = overTarget || saloon::inside(paperQuad(targets[i], paper), mouse);
                    if (home && ((state.focusFirst && i == 0) || (state.focusOptions && i == kBillOptions)))
                    {
                        // Play starts focused, its loop showing, so A / Enter plays at once (K Oct 29); back from
                        // Options the loop is on Options again. (Set on the item itself: SetKeyboardFocusHere tabs
                        // to the next text-input item, here the volume slider, and opens it.)
                        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                        ImGui::SetNavCursorVisible(true);
                        focused = true;
                    }
                    // hovering the book is hovering Options (the book lights, the loop goes to Options)
                    if (home && i == kBillOptions && overBook && moved && !focused)
                    {
                        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                        focused = true;
                    }
                    if (focused)
                        focus = static_cast<int>(i);
                    if (i == 0 && state.installed)
                        ownSoundItem(); // Play's own sound
                    else if (i == skipItem || i == muteItem)
                        tickItem(); // the tick box, the mute mark
                    if (pressed && home)
                    {
                        if (i == 0 && state.installed)
                        {
                            if (state.playTime < 0.0f)
                            {
                                state.playTime = 0.0f; // the plate's animation and the fade; the main loop starts the game after
                                state.soundThisFrame = true; // Play's own sound, next frame
                                scene.fling();               // the doors fling open into the fade
                            }
                        }
                        else if (i == 0)
                        {
                            state.screen = Screen::Install;
                            state.focusFirst = true;
                        }
                        else if (i == kBillOptions)
                            openOptions(state);
                        else if (i == 2)
                        {
                            state.result = Result::Quit;
                            state.done = true;
                        }
                        else if (i == skipItem)
                        {
                            bill.skip = !bill.skip;
                            state.ini.setBool("Launcher", "Show", !bill.skip);
                            state.ini.save();
                        }
#if KAMITE_LAUNCHER_STEAM
                        else if (i == steamItem)
                            pressSteam(state);
#endif
                        else if (i == muteItem)
                        {
                            state.muted = !state.muted;
                            state.ini.setBool("Launcher", "Mute", state.muted);
                            state.ini.save();
                        }
                    }
                    ImGui::PopID();
                }
                // the volume ruler down the bill's right edge: an ImGui slider with nothing of its own showing over it
                // (keyboard / gamepad: activate it, then up / down); a mouse drag follows the paperclip through the
                // bill's perspective. Its focus draws no loop (K).
                {
                    const Rectangle r = saloon::billRuler();
                    const Vector2 top = paper.map(r.x + r.width * 0.5f, r.y), bottom = paper.map(r.x + r.width * 0.5f, r.y + r.height);
                    const Vector2 left = paper.map(r.x, r.y + r.height * 0.5f), right = paper.map(r.x + r.width, r.y + r.height * 0.5f);
                    const float x0 = std::min(left.x, right.x), x1 = std::max(left.x, right.x);
                    ImGui::SetCursorScreenPos(ImVec2(x0, std::min(top.y, bottom.y)));
                    const ImGuiCol hidden[] = {ImGuiCol_FrameBg,         ImGuiCol_FrameBgHovered, ImGuiCol_FrameBgActive, ImGuiCol_SliderGrab,
                                               ImGuiCol_SliderGrabActive, ImGuiCol_Text,           ImGuiCol_Border,        ImGuiCol_NavCursor};
                    for (const ImGuiCol col : hidden)
                        ImGui::PushStyleColor(col, ImVec4(0.0f, 0.0f, 0.0f, 0.0f));
                    float percent = state.volume * 100.0f;
                    bool changed = ImGui::VSliderFloat("##volume", ImVec2(std::max(1.0f, x1 - x0), std::max(1.0f, std::fabs(bottom.y - top.y))), &percent,
                                                       0.0f, 100.0f, "", ImGuiSliderFlags_AlwaysClamp | ImGuiSliderFlags_NoInput);
                    ImGui::PopStyleColor(static_cast<int>(std::size(hidden)));
                    state.volumeSliderId = ImGui::GetItemID();
                    if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
                    {
                        percent = saloon::billRulerLevel(paper.inverse().map(mouse).y) * 100.0f;
                        changed = true;
                    }
                    if (changed && home)
                    {
                        state.volume = percent / 100.0f;
                        if (state.muted && state.volume > 0.0f)
                        {
                            state.muted = false;
                            state.ini.setBool("Launcher", "Mute", false);
                        }
                    }
                    if (home && ImGui::IsItemHovered() && moved && !ImGui::IsItemFocused() && !popup)
                        ImGui::SetFocusID(ImGui::GetItemID(), ImGui::GetCurrentWindow());
                    if (ImGui::IsItemDeactivated())
                    {
                        char text[16];
                        std::snprintf(text, sizeof(text), "%.2f", state.volume);
                        state.ini.set("Launcher", "Volume", text);
                        state.ini.save();
                    }
                    overTarget = overTarget || ImGui::IsItemHovered();
                }
                if (!home)
                    ImGui::EndDisabled();
            }
            if (home && billShown)
                state.focusOptions = false;
            // A click on the book opens Options; on a snack, it is eaten (an egg: no hover hint, K).
            if (home && !popup && ImGui::IsMouseClicked(ImGuiMouseButton_Left) && !ImGui::IsAnyItemHovered())
            {
                if (overBook)
                    openOptions(state);
                else if (const saloon::Scene::Bite bite = scene.snackClick(mouse); bite != saloon::Scene::Bite::None)
                    eatSound(state, bite == saloon::Scene::Bite::Drink);
            }
            // The mouse moved off everything (the bill's lines, the book, the slip's controls): nothing stays focused, so
            // no loop lingers and the book goes dark (K, Nov 18). The next key or button focuses the first item.
            if (home && billShown && !popup && moved && !overTarget && !overBook && !ImGui::IsAnyItemHovered())
            {
                ImGuiContext &g = *GImGui;
                if (g.NavId != 0 && g.NavWindow == ImGui::GetCurrentWindow())
                {
                    ImGui::SetNavID(0, ImGuiNavLayer_Main, 0, ImRect());
                    state.navCleared = true;
                    focus = -1;
                }
            }
            // The loop follows the focus; through the zoom it stays where it was (on Options).
            if (home && focus != bill.focus)
            {
                if (focus >= 0)
                    penSound(state);
                bill.focus = focus;
                bill.focusSeed = static_cast<uint32_t>(std::rand()) * 2654435761u + 1u;
                bill.focusAge = 0.0f;
            }
            if (billShown)
                state.focusFirst = false;
        }

        // The saloon's ink before ImGui (so popups cover it), from last frame's focus: the bill, the corner slip.
        void drawSaloonInk(State &state)
        {
            const saloon::Scene &scene = state.scene;
            if (!scene.ready())
                return;
            saloon::Quad quad;
            if (state.screen == Screen::Home && scene.bill(quad))
                saloon::drawBill(state.billFonts, state.bill, saloon::Homography::rectToQuad(saloon::kPaperW, saloon::kPaperH, quad));
            if (state.screen == Screen::Options)
                return;
            const float alpha = scene.overlays();
            const Slip right = slipAt(state, true);
            drawSlipPaper(state, right, alpha);
            drawSlipInk(state, right, true, alpha);
        }

        bool checkImage(State &state, const fs::path &image)
        {
            if (image.empty())
            {
                state.installMessage = "Choose your disc image (USA, SLUS-21503).";
                state.installError = true;
                return false;
            }
            std::error_code error;
            if (!fs::is_regular_file(image, error))
            {
                state.installMessage = "There is no file at " + utf8(image) + ".";
                state.installError = true;
                return false;
            }
            const DiscCheckResult check = verifyDiscAndPrepare(image, state.context->cacheDir);
            if (!check.ok)
            {
                state.installMessage = check.error;
                state.installError = true;
                return false;
            }
            state.ini.set("Game", "Iso", utf8(image));
            state.ini.save();
            state.installed = true;
            loadMenuSounds(state, image);
            loadSnackVoices(state, image);
            startBark(state, image);
            state.installMessage.clear();
            state.installError = false;
            KM_INFO(Boot, "launcher: disc image installed: " << utf8(image));
            return true;
        }

        void drawHome(State &state)
        {
            if (state.saloonOn)
            {
                drawSaloonHome(state);
                return;
            }
            if (state.installed && state.firstRunPending)
            {
                state.screen = Screen::FirstRun; // a new install whose disc image was found without the Install screen
                state.focusFirst = true;
                return;
            }
            title(state);
            const float buttonWidth = fontSize(state, 420.0f);
            if (!state.installed)
            {
                pushSize(state, 22.0f);
                centeredText("To play, point the port to your own copy of the game.", kTextSoft);
                ImGui::PopFont();
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 10.0f)));
                if (bigButton(state, "Install", buttonWidth, true))
                {
                    state.screen = Screen::Install;
                    state.focusFirst = true;
                }
                if (state.focusFirst)
                    ImGui::SetItemDefaultFocus();
            }
            else
            {
                // Play starts selected, its highlight showing, so A / Enter plays at once (SetItemDefaultFocus alone
                // left the cursor hidden until a d-pad press, K Oct 29).
                if (state.focusFirst)
                {
                    ImGui::SetKeyboardFocusHere();
                    ImGui::SetNavCursorVisible(true);
                }
                const bool play = bigButton(state, "Play", buttonWidth, true);
                ownSoundItem();
                if (play && state.playTime < 0.0f)
                {
                    state.playTime = 0.0f; // the plate's animation and the fade; the main loop starts the game after
                    state.soundThisFrame = true; // Play's own sound, next frame
                }
                if (bigButton(state, "Options", buttonWidth, false))
                {
                    state.screen = Screen::Options;
                    state.focusFirst = true;
                    g_hotkeysOpen = false;
                }
                ImGui::Dummy(ImVec2(0.0f, fontSize(state, 6.0f)));
                pushSize(state, 20.0f);
                bool hide = !state.ini.getBool("Launcher", "Show", true);
                const char *label = "Don't show this again (hold Shift while starting to see it)";
                ImGui::SetCursorPosX((ImGui::GetWindowWidth() - ImGui::CalcTextSize(label).x - ImGui::GetFrameHeight() -
                                      ImGui::GetStyle().ItemInnerSpacing.x) *
                                     0.5f);
                const bool toggled = ImGui::Checkbox(label, &hide);
                tickItem();
                if (toggled)
                {
                    state.ini.setBool("Launcher", "Show", !hide);
                    state.ini.save();
                }
                ImGui::PopFont();
            }
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 24.0f)));
            pushSize(state, 22.0f);
            const float quitWidth = fontSize(state, 160.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - quitWidth) * 0.5f);
            if (ImGui::Button("Quit", ImVec2(quitWidth, 0.0f)))
            {
                state.result = Result::Quit;
                state.done = true;
            }
            ImGui::PopFont();
            // The rights notice at the foot of the title screen.
            {
                pushSize(state, 16.0f);
                const char *lines[] = {"God Hand\u2122 is a trademark of CAPCOM CO., LTD. All rights to the game belong to Capcom.",
                                       "God Hand: Rearmed is an unofficial fan-made project, not affiliated with or endorsed by Capcom or Clover Studio. "
                                       "It contains no game data: play your own damn copy!"};
                const float lineHeight = ImGui::GetTextLineHeightWithSpacing();
                const float y = ImGui::GetWindowHeight() - lineHeight * 2.0f - fontSize(state, 18.0f);
                if (ImGui::GetCursorPosY() < y)
                    ImGui::SetCursorPosY(y);
                for (const char *line : lines)
                    centeredText(line, kTextSoft);
                ImGui::PopFont();
            }
            state.focusFirst = false;
        }

        void drawInstall(State &state)
        {
            title(state);
            const float width = fontSize(state, 760.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
            pushSize(state, 22.0f);
            ImGui::BeginChild("install", ImVec2(width, 0.0f), ImGuiChildFlags_AutoResizeY | ImGuiChildFlags_Borders |
                                                                  ImGuiChildFlags_AlwaysUseWindowPadding);
            ImGui::TextWrapped("The port plays from your own disc image (USA, SLUS-21503, the .iso of the disc). "
                               "Nothing is copied: the game reads it while you play, so keep it where it is.");
            ImGui::Spacing();
            if (ImGui::Button("Choose disc image..."))
            {
                const fs::path picked = platform::pick(GetWindowHandle(), false);
                if (!picked.empty())
                {
                    const std::string text = utf8(picked);
                    std::snprintf(state.isoText, sizeof(state.isoText), "%s", text.c_str());
                    checkImage(state, picked);
                }
            }
            if (state.focusFirst)
                ImGui::SetItemDefaultFocus();
            ImGui::TextColored(kTextDim, "or type its path:");
            ImGui::SetNextItemWidth(-1.0f);
            const bool entered = ImGui::InputText("##iso", state.isoText, sizeof(state.isoText), ImGuiInputTextFlags_EnterReturnsTrue);
            if (ImGui::Button("Check and install") || entered)
            {
                std::string text = state.isoText;
                if (text.size() >= 2 && text.front() == '"' && text.back() == '"')
                    text = text.substr(1, text.size() - 2);
                checkImage(state, fromUtf8(text));
            }
            if (!state.installMessage.empty())
            {
                ImGui::Spacing();
                ImGui::PushStyleColor(ImGuiCol_Text, state.installError ? ImVec4(1.0f, 0.45f, 0.35f, 1.0f) : kTextDim);
                ImGui::TextWrapped("%s", state.installMessage.c_str());
                ImGui::PopStyleColor();
            }
            if (state.installed)
            {
                ImGui::Spacing();
                ImGui::TextColored(ImVec4(0.55f, 0.9f, 0.45f, 1.0f), "Installed. Your disc image checks out.");
            }
            ImGui::Spacing();
            const bool leave = ImGui::Button(state.installed ? "Continue" : "Back");
            if (!state.installed)
                ownSoundItem();
            if (leave)
            {
                if (!state.installed)
                    menuSound(state, kCancel);
                state.screen = state.installed && state.firstRunPending ? Screen::FirstRun : Screen::Home;
                state.focusFirst = true;
            }
            ImGui::EndChild();
            ImGui::PopFont();
            state.focusFirst = false;
        }

        // ---- settings tables --------------------------------------------------------------------------------------
        // Every setting is a row (name and help on the left, the control on the right) with a line above it; a
        // setting's own sub-options (subSetting) share its section, with no line between them.
        int g_settingRows = 0;

        void dividerLine()
        {
            ImGuiWindow *window = ImGui::GetCurrentWindow();
            const float y = ImGui::GetCursorScreenPos().y - ImGui::GetStyle().ItemSpacing.y * 0.5f;
            const float x0 = window->InnerRect.Min.x + ImGui::GetStyle().WindowPadding.x;
            const float x1 = window->InnerRect.Max.x - ImGui::GetStyle().WindowPadding.x;
            ImGui::PushClipRect(window->InnerRect.Min, window->InnerRect.Max, false);
            window->DrawList->AddLine(ImVec2(x0, y), ImVec2(x1, y), ImGui::GetColorU32(ImVec4(0.35f, 0.22f, 0.15f, 0.55f)), 1.0f);
            ImGui::PopClipRect();
        }

        void describe(const char *name, const char *help, const char *note)
        {
            ImGui::TextUnformatted(name);
            ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
            if (help && *help)
                ImGui::TextWrapped("%s", help);
            if (note && *note)
                ImGui::TextWrapped("NOTE: %s", note);
            ImGui::PopStyleColor();
        }

        // A setting in a section of its own: a line with some room above and below it, then the name and help on the
        // left (`note`: a line of its own at the bottom of the help, "NOTE: ..."; `below`: more controls under the help,
        // such as Camera's "Show advanced settings"), the control on the right.
        void settingLabel(const char *name, const char *help, const char *note = nullptr, const std::function<void()> &below = {})
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            const bool divided = g_settingRows++ > 0;
            const float gap = std::round(ImGui::GetFontSize() * 0.45f), spacing = ImGui::GetStyle().ItemSpacing.y;
            if (divided)
            {
                ImGui::Dummy(ImVec2(0.0f, gap));
                dividerLine();
                ImGui::Dummy(ImVec2(0.0f, gap));
            }
            describe(name, help, note);
            if (below)
                below();
            ImGui::TableSetColumnIndex(1);
            if (divided)
                ImGui::Dummy(ImVec2(0.0f, 2.0f * gap + spacing)); // the control lines up with the name
        }

        // A sub-option in the section of the setting above it.
        void subSetting(const char *name, const char *help, const char *note = nullptr)
        {
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            ImGui::Indent(ImGui::GetFontSize());
            describe(name, help, note);
            ImGui::Unindent(ImGui::GetFontSize());
            ImGui::TableSetColumnIndex(1);
        }

        bool beginSettings(const char *id)
        {
            if (!ImGui::BeginTable(id, 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX))
                return false;
            g_settingRows = 0;
            ImGui::TableSetupColumn("name", ImGuiTableColumnFlags_WidthStretch, 0.58f);
            ImGui::TableSetupColumn("value", ImGuiTableColumnFlags_WidthStretch, 0.42f);
            return true;
        }

        // A tab's settings come in categories: an orange heading with a rule across the panel (it is also the line above
        // the category's first setting), then the category's own settings table (the columns line up from table to
        // table). Only one tab is drawn a frame, so g_categories counts that tab's headings: room above all but the first.
        int g_categories = 0;

        void categoryGap() { ImGui::Dummy(ImVec2(0.0f, std::round(ImGui::GetFontSize() * (g_categories++ > 0 ? 0.8f : 0.2f)))); }

        void categoryHeading(const char *title)
        {
            categoryGap();
            ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
            ImGui::SeparatorText(title);
            ImGui::PopStyleColor();
        }

        bool beginCategory(const char *title)
        {
            categoryHeading(title);
            return beginSettings(title);
        }

        // A heading that folds its category away: the arrow takes the place of the rule's short left end.
        bool collapsibleHeading(const char *title, bool &open)
        {
            categoryGap();
            const ImVec2 start = ImGui::GetCursorScreenPos();
            const float right = start.x + ImGui::GetContentRegionAvail().x;
            ImGui::SetNextItemOpen(open, ImGuiCond_Always);
            ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
            open = ImGui::TreeNodeEx(title, ImGuiTreeNodeFlags_NoTreePushOnOpen);
            tickItem();
            ImGui::PopStyleColor();
            const ImGuiStyle &style = ImGui::GetStyle();
            const float x0 = start.x + ImGui::GetTreeNodeToLabelSpacing() + ImGui::CalcTextSize(title, nullptr, true).x +
                             style.ItemSpacing.x;
            const float y = std::floor((ImGui::GetItemRectMin().y + ImGui::GetItemRectMax().y) * 0.5f + 0.99f);
            if (right > x0 && style.SeparatorTextBorderSize > 0.0f)
                ImGui::GetWindowDrawList()->AddLine(ImVec2(x0, y), ImVec2(right, y), ImGui::GetColorU32(ImGuiCol_Separator),
                                                    style.SeparatorTextBorderSize);
            return open;
        }

        void iniCombo(State &state, const char *id, const char *section, const char *key, const char *fallback,
                      bool (*combo)(const char *, std::string &))
        {
            std::string value = state.ini.get(section, key, fallback);
            if (value.empty())
                value = fallback;
            if (combo(id, value))
                state.ini.set(section, key, value);
        }

        void iniCheckbox(State &state, const char *id, const char *section, const char *key, bool fallback)
        {
            bool value = state.ini.getBool(section, key, fallback);
            const bool toggled = ImGui::Checkbox(id, &value);
            tickItem();
            if (toggled)
                state.ini.setBool(section, key, value);
        }

        // ---- Controls: player 1's bindings (controls.ini through ps2_host_input's model) ----------
        namespace pc = ps2_host_input::controls;
        namespace gp = ps2_host_gamepads;

        struct ControlRow
        {
            const char *key;    // controls.ini [Pad1] key
            const char *action; // what it does in the game ("" = nothing known)
            const char *button; // the PS2 button
            bool keysOnly = false; // keyboard & mouse only (the Dodge Modifier)
        };
        // The rows: the PS2 inputs in controls.ini's order.
        constexpr ControlRow kControlRows[] = {
            {"Cross", "Attack / OK", "Cross"},
            {"Circle", "Action / prompt", "Circle"},
            {"Square", "Combo", "Square"},
            {"Triangle", "Attack / cancel", "Triangle"},
            {"L1", "Turn around", "L1"},
            {"R1", "God Reel", "R1"},
            {"L2", "Taunt", "L2"},
            {"R2", "Unleash", "R2"},
            {"L3", "", "L3 (left stick click)"},
            {"R3", "", "R3 (right stick click)"},
            {"Start", "Pause menu", "Start"},
            {"Select", "", "Select"},
            {"Up", "Style S / menu up", "D-pad up"},
            {"Down", "Style X / menu down", "D-pad down"},
            {"Left", "Style Y / menu left", "D-pad left"},
            {"Right", "Style E / menu right", "D-pad right"},
            {"LUp", "Move forward", "Left stick up"},
            {"LDown", "Step back", "Left stick down"},
            {"LLeft", "Move left", "Left stick left"},
            {"LRight", "Move right", "Left stick right"},
            {"RUp", "Duck", "Right stick up"},
            {"RDown", "Back flip", "Right stick down"},
            {"RLeft", "Sidestep left", "Right stick left"},
            {"RRight", "Sidestep right", "Right stick right"},
            {"DodgeModifier", "Hold: the move keys dodge", "Dodge modifier", true},
        };
        constexpr int kControlRowCount = static_cast<int>(std::size(kControlRows));
        constexpr int kKeyColumns = 2; // Key 1, Key 2 (a third binding in the file is kept as it is)
        constexpr uint32_t kPlayer = 0u;

        struct Capture
        {
            bool active = false;
            bool controller = false; // false: keys and mouse
            int row = 0;
            int column = 0;          // keys: 0..kKeyColumns-1
            bool waitRelease = true; // the click or button press that started it must end first
            double started = 0.0;
        };
        Capture g_capture;
        bool g_openCapture = false;   // open the popup at drawControls' ID level (cells push their own IDs)
        bool g_captureFrame = false;  // a capture ran this frame: its Esc / B must not also leave Options
        std::string g_controlsStatus;
        bool g_resetArmed = false;

        struct Split
        {
            std::vector<std::string> keys, pads, other;
        };

        Split splitRow(int row)
        {
            Split split;
            for (std::string &binding : pc::bindings(kPlayer, kControlRows[row].key))
            {
                switch (pc::kindOf(binding))
                {
                case pc::Kind::Keys: split.keys.push_back(std::move(binding)); break;
                case pc::Kind::Controller: split.pads.push_back(std::move(binding)); break;
                default: split.other.push_back(std::move(binding)); break;
                }
            }
            return split;
        }

        void storeRow(int row, const Split &split)
        {
            std::vector<std::string> list = split.keys;
            list.insert(list.end(), split.pads.begin(), split.pads.end());
            list.insert(list.end(), split.other.begin(), split.other.end());
            pc::setBindings(kPlayer, kControlRows[row].key, std::move(list));
        }

        std::string rowName(int row)
        {
            const ControlRow &r = kControlRows[row];
            return *r.action ? std::string(r.action) + " (" + r.button + ")" : std::string(r.button);
        }

        // The binding goes into the cell; within one action it appears once; another action may
        // share it (the status line says so).
        void bindCell(int row, bool controller, int column, const std::string &binding)
        {
            Split split = splitRow(row);
            std::vector<std::string> &list = controller ? split.pads : split.keys;
            size_t at = controller ? 0u : static_cast<size_t>(column);
            if (at < list.size())
                list[at] = binding;
            else
            {
                list.push_back(binding);
                at = list.size() - 1u;
            }
            for (size_t i = list.size(); i-- > 0u;)
                if (i != at && pc::sameBinding(list[i], binding))
                {
                    list.erase(list.begin() + static_cast<std::ptrdiff_t>(i));
                    if (i < at)
                        --at;
                }
            storeRow(row, split);
            g_controlsStatus = rowName(row) + ": " + pc::displayName(binding);
            for (int other = 0; other < kControlRowCount; ++other)
            {
                if (other == row)
                    continue;
                bool shared = false;
                for (const std::string &existing : pc::bindings(kPlayer, kControlRows[other].key))
                    shared = shared || pc::sameBinding(existing, binding);
                if (shared)
                {
                    g_controlsStatus += "   (also on " + rowName(other) + ")";
                    break;
                }
            }
            KM_INFO(Boot, "launcher: controls " << kControlRows[row].key << " = " << binding);
        }

        void clearCell(int row, bool controller, int column)
        {
            Split split = splitRow(row);
            std::vector<std::string> &list = controller ? split.pads : split.keys;
            const size_t at = controller ? 0u : static_cast<size_t>(column);
            if (at >= list.size())
                return;
            const std::string removed = list[at];
            list.erase(list.begin() + static_cast<std::ptrdiff_t>(at));
            storeRow(row, split);
            g_controlsStatus = rowName(row) + ": " + pc::displayName(removed) + " removed";
        }

        bool anyPadInput()
        {
            for (int device = 0; device < gp::kMaxDevices; ++device)
            {
                if (!gp::connected(device))
                    continue;
                for (int b = 0; b < gp::ButtonCount; ++b)
                    if (gp::button(device, static_cast<gp::Button>(b)))
                        return true;
                for (int a = 0; a < gp::AxisCount; ++a)
                    if (std::fabs(gp::axis(device, static_cast<gp::Axis>(a))) > 0.3f)
                        return true;
            }
            return false;
        }

        // A key both a game input and a hotkey would hold (K, Nov 14): a modal asks before assigning, whichever side
        // is being set. Plain keys only (a Shift / Ctrl hotkey does not fire on the bare key).
        struct KeyClash
        {
            bool game = false;     // true: a game input (row / column / binding); false: a hotkey (action / key)
            int row = 0, column = 0;
            std::string binding;
            ps2x::hotkeys::Action action = ps2x::hotkeys::Action::Count;
            ps2x::hotkeys::Binding key;
            std::string message;
        };
        KeyClash g_clash;
        bool g_openClash = false;

        void askKeyClash(KeyClash clash)
        {
            g_clash = std::move(clash);
            g_openClash = true;
        }

        std::string lowerFirst(std::string text)
        {
            if (!text.empty())
                text[0] = static_cast<char>(std::tolower(static_cast<unsigned char>(text[0])));
            return text;
        }

        // The game action a controls.ini key ("Cross", "LUp") stands for, as the table names it.
        std::string gameInputName(const std::string &key)
        {
            const auto same = [](std::string_view a, std::string_view b)
            {
                return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin(), [](char x, char y)
                                                          { return std::tolower(static_cast<unsigned char>(x)) == std::tolower(static_cast<unsigned char>(y)); });
            };
            for (int row = 0; row < kControlRowCount; ++row)
                if (same(key, kControlRows[row].key))
                    return rowName(row);
            return key;
        }

        // The capture in progress (every frame while its popup is open): true when it ended.
        bool captureTick()
        {
            if (IsKeyPressed(KEY_ESCAPE) || GetTime() - g_capture.started > 10.0)
            {
                g_controlsStatus = IsKeyPressed(KEY_ESCAPE) ? "Cancelled." : "Nothing pressed for 10 s: cancelled.";
                return true;
            }
            if (g_capture.waitRelease)
            {
                const bool held = g_capture.controller ? anyPadInput()
                                                       : (IsMouseButtonDown(MOUSE_BUTTON_LEFT) || IsMouseButtonDown(MOUSE_BUTTON_RIGHT) ||
                                                          IsMouseButtonDown(MOUSE_BUTTON_MIDDLE) || IsKeyDown(KEY_ENTER) || IsKeyDown(KEY_SPACE));
                if (!held)
                    g_capture.waitRelease = false;
                return false;
            }
            if (!g_capture.controller)
            {
                for (int key : pc::bindableKeys())
                    if (IsKeyPressed(key))
                    {
                        const std::string binding = pc::keyBinding(key);
                        if (!binding.empty())
                        {
                            if (const ps2x::hotkeys::Action holder = ps2x::hotkeys::plainKeyHolder(key);
                                holder != ps2x::hotkeys::Action::Count)
                                askKeyClash(KeyClash{true, g_capture.row, g_capture.column, binding, {}, {},
                                                     pc::displayName(binding) + " is the hotkey for " +
                                                         lowerFirst(ps2x::hotkeys::info(holder).label) +
                                                         ". Pressing it will do both. Assign it anyway?"});
                            else
                                bindCell(g_capture.row, false, g_capture.column, binding);
                            return true;
                        }
                    }
                for (int mouse : {MOUSE_BUTTON_LEFT, MOUSE_BUTTON_RIGHT, MOUSE_BUTTON_MIDDLE, MOUSE_BUTTON_SIDE, MOUSE_BUTTON_EXTRA})
                    if (IsMouseButtonPressed(mouse))
                    {
                        bindCell(g_capture.row, false, g_capture.column, pc::mouseBinding(mouse));
                        return true;
                    }
                return false;
            }
            for (int device = 0; device < gp::kMaxDevices; ++device)
            {
                if (!gp::connected(device))
                    continue;
                for (int b = 0; b < gp::ButtonCount; ++b)
                    if (gp::button(device, static_cast<gp::Button>(b)))
                    {
                        bindCell(g_capture.row, true, 0, pc::padButtonBinding(device, b));
                        return true;
                    }
                for (int a = 0; a < gp::AxisCount; ++a)
                {
                    const float v = gp::axis(device, static_cast<gp::Axis>(a));
                    if (std::fabs(v) > 0.6f)
                    {
                        bindCell(g_capture.row, true, 0, pc::padAxisBinding(device, a, v < 0.0f));
                        return true;
                    }
                }
            }
            return false;
        }

        void startCapture(int row, bool controller, int column)
        {
            g_capture = Capture{true, controller, row, column, true, GetTime()};
            g_openCapture = true;
        }

        // One cell: click (or A/Enter) to bind, right-click (or X / Delete when focused) to clear.
        void bindingCell(int row, bool controller, int column, const std::string &binding, float width)
        {
            ImGui::PushID(row * 8 + (controller ? 7 : column));
            const std::string label = (binding.empty() ? std::string("-") : pc::displayName(binding)) + "##cell";
            if (ImGui::Button(label.c_str(), ImVec2(width, 0.0f)))
                startCapture(row, controller, column);
            if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                clearCell(row, controller, column);
            if (ImGui::IsItemFocused() && (ImGui::IsKeyPressed(ImGuiKey_Delete, false) || ImGui::IsKeyPressed(ImGuiKey_Backspace, false) ||
                                           ImGui::IsKeyPressed(ImGuiKey_GamepadFaceLeft, false)))
                clearCell(row, controller, column);
            ImGui::PopID();
        }

        // The port's shortcut keys (runtime/ps2_hotkeys.h): GHR.ini [Hotkeys], one line per action. The runtime's
        // table is set too, so a key bound here is live when the game starts from this launcher.
        namespace hk = ps2x::hotkeys;
        int g_hotkeyCapture = -1;
        double g_hotkeyStarted = 0.0;
        bool g_hotkeyWaitRelease = false; // the Enter / Space / click that opened the capture must go up first
        bool g_openHotkeyCapture = false;
        std::string g_hotkeyStatus;

        void storeHotkey(State &state, hk::Action action, hk::Binding binding)
        {
            hk::setBinding(action, binding);
            state.ini.set("Hotkeys", hk::info(action).id, binding.bound() ? hk::text(binding) : "none");
        }

        void loadHotkeys(State &state)
        {
            for (int i = 0; i < static_cast<int>(hk::Action::Count); ++i)
            {
                const hk::Action action = static_cast<hk::Action>(i);
                const std::string value = state.ini.get("Hotkeys", hk::info(action).id, "\x01");
                hk::Binding binding;
                if (value == "\x01")
                    continue;
                if (hk::parse(value, binding))
                    hk::setBinding(action, binding);
                else // a controller button from an older build (keyboard-only since Nov 14): the default, written back
                    storeHotkey(state, action, hk::info(action).def);
            }
        }

        void drawHotkeys(State &state)
        {
            static bool loaded = false;
            if (!loaded)
            {
                loaded = true;
                loadHotkeys(state);
            }
            if (collapsibleHeading("Hotkeys", g_hotkeysOpen))
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
                ImGui::TextWrapped("The port's own keys. Click one, then press the key (with Shift or Ctrl held if you want). "
                                   "Right-click clears it. The F2 menu's Controls tab edits the same list.");
                ImGui::PopStyleColor();
                if (ImGui::BeginTable("hotkeys", 2, ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_RowBg))
                {
                    ImGui::TableSetupColumn("Action", ImGuiTableColumnFlags_WidthStretch, 0.6f);
                    ImGui::TableSetupColumn("Key", ImGuiTableColumnFlags_WidthStretch, 0.4f);
                    ImGui::TableHeadersRow();
                    for (int i = 0; i < static_cast<int>(hk::Action::Count); ++i)
                    {
                        const hk::Action action = static_cast<hk::Action>(i);
                        if (!hk::available(action))
                            continue; // the dev keys in a release build
                        const hk::Info &info = hk::info(action);
                        const hk::Binding binding = hk::binding(action);
                        ImGui::TableNextRow();
                        ImGui::TableSetColumnIndex(0);
                        ImGui::AlignTextToFramePadding();
                        ImGui::TextUnformatted(info.label);
                        ImGui::TableSetColumnIndex(1);
                        ImGui::PushID(1000 + i);
                        const std::string label = (binding.bound() ? hk::text(binding) : std::string("-")) + "##hk";
                        if (ImGui::Button(label.c_str(), ImVec2(-1.0f, 0.0f)))
                        {
                            g_hotkeyCapture = i;
                            g_hotkeyStarted = GetTime();
                            g_openHotkeyCapture = true;
                            g_hotkeyWaitRelease = true;
                        }
                        if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(ImGuiMouseButton_Right))
                        {
                            storeHotkey(state, action, hk::Binding{});
                            g_hotkeyStatus = std::string(info.label) + ": no key.";
                        }
                        ImGui::PopID();
                    }
                    ImGui::EndTable();
                }
                if (!g_hotkeyStatus.empty())
                    ImGui::TextColored(kTextDim, "%s", g_hotkeyStatus.c_str());
                if (ImGui::Button("Default hotkeys"))
                {
                    for (int i = 0; i < static_cast<int>(hk::Action::Count); ++i)
                        if (hk::available(static_cast<hk::Action>(i)))
                            storeHotkey(state, static_cast<hk::Action>(i), hk::info(static_cast<hk::Action>(i)).def);
                    g_hotkeyStatus = "Default hotkeys restored.";
                }
            }

            if (g_openHotkeyCapture)
            {
                g_openHotkeyCapture = false;
                ImGui::OpenPopup("##hotkeycapture");
            }
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            if (ImGui::BeginPopupModal("##hotkeycapture", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
            {
                g_captureFrame = true;
                const hk::Action action = static_cast<hk::Action>(g_hotkeyCapture);
                ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 18.0f, ImGui::GetFontSize() * 0.5f));
                ImGui::TextUnformatted(hk::info(action).label);
                ImGui::TextColored(kTextDim, "Press a key (Shift or Ctrl with it if you like)...");
                ImGui::TextColored(kTextDim, "Esc cancels (%d s)", std::max(0, 10 - static_cast<int>(GetTime() - g_hotkeyStarted)));
                ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.5f));
                bool done = false;
                if (g_hotkeyWaitRelease)
                {
                    (void)hk::capturePressed(); // the queue holds the activating key still
                    if (!(IsKeyDown(KEY_ENTER) || IsKeyDown(KEY_KP_ENTER) || IsKeyDown(KEY_SPACE) || IsMouseButtonDown(MOUSE_BUTTON_LEFT)))
                        g_hotkeyWaitRelease = false;
                }
                else if (IsKeyPressed(KEY_ESCAPE) || GetTime() - g_hotkeyStarted > 10.0)
                {
                    g_hotkeyStatus = IsKeyPressed(KEY_ESCAPE) ? "Cancelled." : "Nothing pressed for 10 s: cancelled.";
                    done = true;
                }
                else if (const int key = hk::capturePressed(); key != 0)
                {
                    const hk::Binding binding{key, IsKeyDown(KEY_LEFT_SHIFT) || IsKeyDown(KEY_RIGHT_SHIFT),
                                              IsKeyDown(KEY_LEFT_CONTROL) || IsKeyDown(KEY_RIGHT_CONTROL)};
                    const hk::Action other = hk::conflict(action, binding);
                    if (other != hk::Action::Count)
                        g_hotkeyStatus = hk::text(binding) + " is already " + hk::info(other).label + "; not changed.";
                    else if (const std::string use = pc::keyboardKeyUse(kPlayer, key); !use.empty() && !binding.shift && !binding.ctrl)
                    {
                        KeyClash clash;
                        clash.action = action;
                        clash.key = binding;
                        clash.message = hk::text(binding) + " is assigned to " + gameInputName(use) +
                                        " in the game's controls. Pressing it will do both. Assign it anyway?";
                        askKeyClash(std::move(clash));
                    }
                    else
                    {
                        storeHotkey(state, action, binding);
                        g_hotkeyStatus = std::string(hk::info(action).label) + ": " + hk::text(binding);
                    }
                    done = true;
                }
                if (done)
                {
                    g_hotkeyCapture = -1;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
        }

        // The key-clash question (askKeyClash), opened the frame after the capture that found it closed.
        void drawKeyClash(State &state)
        {
            if (g_openClash)
            {
                g_openClash = false;
                ImGui::OpenPopup("##keyclash");
            }
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            if (!ImGui::BeginPopupModal("##keyclash", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
                return;
            g_captureFrame = true; // its Esc / B must not also leave Options
            const float width = ImGui::GetFontSize() * 22.0f;
            ImGui::Dummy(ImVec2(width, ImGui::GetFontSize() * 0.25f));
            ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + width);
            ImGui::TextUnformatted(g_clash.message.c_str());
            ImGui::PopTextWrapPos();
            ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.5f));
            bool close = false;
            if (ImGui::Button("Assign"))
            {
                if (g_clash.game)
                    bindCell(g_clash.row, false, g_clash.column, g_clash.binding);
                else
                {
                    storeHotkey(state, g_clash.action, g_clash.key);
                    g_hotkeyStatus = std::string(hk::info(g_clash.action).label) + ": " + hk::text(g_clash.key);
                }
                close = true;
            }
            tickItem();
            ImGui::SameLine();
            if (ImGui::Button("Cancel") || ImGui::IsKeyPressed(ImGuiKey_Escape, false) || ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false))
            {
                (g_clash.game ? g_controlsStatus : g_hotkeyStatus) = "Not changed.";
                close = true;
            }
            tickItem();
            if (close)
                ImGui::CloseCurrentPopup();
            ImGui::EndPopup();
        }

        // The Controls tab after its table settings: the bindings and hotkeys (launcher only).
        void drawControls(State &state)
        {
            categoryHeading("Bindings");
            static int device = 0; // 0 keyboard & mouse, 1 controller
            ImGui::TextUnformatted("Bindings for");
            ImGui::SameLine();
            ImGui::RadioButton("Keyboard & mouse", &device, 0);
            tickItem();
            ImGui::SameLine();
            ImGui::RadioButton("Controller", &device, 1);
            tickItem();
            const bool controller = device == 1;
            ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
            ImGui::TextWrapped("%s", controller ? "Click a button, then press the controller button or move the stick or "
                                                  "trigger it should be. Right-click (or X) clears it. Esc cancels."
                                                : "Click a box, then press the key or click the mouse button it should be. "
                                                  "Right-click (or Delete) clears it. Esc cancels; F-keys stay the port's "
                                                  "shortcuts. Menus always take the arrows / WASD, Enter / Space (confirm), "
                                                  "Esc / Backspace (back) and Q / E (tabs).");
            if (pc::envOverride(kPlayer))
                ImGui::TextWrapped("PS2X_PAD1 is set in the environment: it wins over these bindings while it is set.");
            ImGui::PopStyleColor();
            if (ImGui::BeginTable("bindings", controller ? 2 : 1 + kKeyColumns,
                                  ImGuiTableFlags_SizingStretchProp | ImGuiTableFlags_PadOuterX | ImGuiTableFlags_RowBg))
            {
                ImGui::TableSetupColumn("PS2 button", ImGuiTableColumnFlags_WidthStretch, 0.46f);
                if (controller)
                    ImGui::TableSetupColumn("Controller", ImGuiTableColumnFlags_WidthStretch, 0.54f);
                else
                {
                    ImGui::TableSetupColumn("Key 1", ImGuiTableColumnFlags_WidthStretch, 0.27f);
                    ImGui::TableSetupColumn("Key 2", ImGuiTableColumnFlags_WidthStretch, 0.27f);
                }
                ImGui::TableHeadersRow();
                for (int row = 0; row < kControlRowCount; ++row)
                {
                    if (controller && kControlRows[row].keysOnly)
                        continue;
                    const Split split = splitRow(row);
                    ImGui::TableNextRow();
                    ImGui::TableSetColumnIndex(0);
                    ImGui::AlignTextToFramePadding();
                    ImGui::TextUnformatted(kControlRows[row].button);
                    if (*kControlRows[row].action)
                    {
                        ImGui::SameLine();
                        ImGui::TextColored(kTextDim, "%s", kControlRows[row].action);
                    }
                    if (controller)
                    {
                        ImGui::TableSetColumnIndex(1);
                        bindingCell(row, true, 0, split.pads.empty() ? std::string() : split.pads[0], -1.0f);
                    }
                    else
                    {
                        for (int column = 0; column < kKeyColumns; ++column)
                        {
                            ImGui::TableSetColumnIndex(1 + column);
                            bindingCell(row, false, column, column < static_cast<int>(split.keys.size()) ? split.keys[column] : std::string(),
                                        -1.0f);
                        }
                    }
                }
                ImGui::EndTable();
            }
            if (!g_controlsStatus.empty())
                ImGui::TextColored(kTextDim, "%s", g_controlsStatus.c_str());
            if (ImGui::Button(g_resetArmed ? "Click again to reset every binding" : "Reset all bindings to the defaults"))
            {
                if (g_resetArmed)
                {
                    pc::resetDefaults(kPlayer);
                    g_controlsStatus = "Default bindings restored.";
                }
                g_resetArmed = !g_resetArmed;
            }
            ImGui::SameLine();
            if (ImGui::Button("Open controls.ini"))
            {
                std::string message;
                pc::save(message);
                platform::open(state.context->exeDir / "controls.ini");
            }

            drawHotkeys(state);

            // The capture popup.
            if (g_openCapture)
            {
                g_openCapture = false;
                ImGui::OpenPopup("##capture");
            }
            ImGui::SetNextWindowPos(ImGui::GetMainViewport()->GetCenter(), ImGuiCond_Always, ImVec2(0.5f, 0.5f));
            if (ImGui::BeginPopupModal("##capture", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
            {
                g_captureFrame = true;
                ImGui::Dummy(ImVec2(ImGui::GetFontSize() * 18.0f, ImGui::GetFontSize() * 0.5f));
                ImGui::TextUnformatted(rowName(g_capture.row).c_str());
                ImGui::TextColored(kTextDim, "%s", g_capture.controller ? "Press a controller button or move a stick or trigger..."
                                                                       : "Press a key or click a mouse button...");
                ImGui::TextColored(kTextDim, "Esc cancels (%d s)", std::max(0, 10 - static_cast<int>(GetTime() - g_capture.started)));
                ImGui::Dummy(ImVec2(0.0f, ImGui::GetFontSize() * 0.5f));
                if (captureTick())
                {
                    g_capture.active = false;
                    ImGui::CloseCurrentPopup();
                }
                ImGui::EndPopup();
            }
            drawKeyClash(state);
        }

        void saveControls()
        {
            if (!pc::dirty())
                return;
            std::string message;
            if (!pc::save(message))
                KM_WARN(Boot, "launcher: controls not saved: " << message);
        }

        // ---- the settings tabs (settings_table.cpp: the in-game OPTIONS screen draws the same list) ----------------
        // A combo over a setting's choices; a value that is none of them (set by hand) shows as one more entry and is
        // kept unless another is picked. Returns true when changed (value updated).
        bool tableCombo(const char *id, const settings::Setting &setting, std::string &value)
        {
            const int current = settings::choiceIndex(setting, value);
            std::vector<const char *> labels;
            for (int i = 0; i < setting.choiceCount; ++i)
                labels.push_back(setting.choices[i].label);
            const std::string custom = "Custom: " + value;
            int shown = current;
            if (current < 0)
            {
                labels.push_back(custom.c_str());
                shown = setting.choiceCount;
            }
            int picked = shown;
            ImGui::SetNextItemWidth(-1.0f);
            const bool changed = ImGui::Combo(id, &picked, labels.data(), static_cast<int>(labels.size()));
            tickItem();
            if (!changed || picked == shown || picked >= setting.choiceCount)
                return false;
            value = setting.choices[picked].value;
            return true;
        }

        // The settings drawn by hand: Display Mode (two keys), Character, texture packs.
        void customControl(State &state, const settings::Setting &setting)
        {
            const std::string id = setting.id;
            if (id == "DisplayMode")
            {
                std::string mode = state.ini.getBool("Video", "StartFullscreen", true)    ? "fullscreen"
                                   : state.ini.getBool("Video", "StartMaximized", false) ? "maximized"
                                                                                         : "window";
                if (choiceCombo("##window", kWindowModes, mode))
                {
                    state.ini.setBool("Video", "StartFullscreen", mode == "fullscreen");
                    state.ini.setBool("Video", "StartMaximized", mode == "maximized");
                }
            }
            else if (id == "Character")
            {
                static std::vector<characters::Character> list;
                if (list.empty())
                    list = characters::list(state.context->exeDir);
                const std::string current = lower(state.ini.get("Mods", "Character", "gene"));
                int picked = 0;
                std::vector<const char *> names;
                for (size_t i = 0; i < list.size(); ++i)
                {
                    names.push_back(list[i].name.c_str());
                    if (lower(list[i].id) == current)
                        picked = static_cast<int>(i);
                }
                ImGui::SetNextItemWidth(-1.0f);
                const bool pickedNew = ImGui::Combo("##character", &picked, names.data(), static_cast<int>(names.size()));
                tickItem();
                if (pickedNew)
                    state.ini.set("Mods", "Character", list[static_cast<size_t>(picked)].id);
                if (!list.empty() && !list[static_cast<size_t>(picked)].credit.empty())
                    ImGui::TextColored(kTextDim, "%s", list[static_cast<size_t>(picked)].credit.c_str());
            }
            else if (id == "Textures")
            {
                iniCheckbox(state, "Use replacement textures##tex", "Runtime", "PS2X_PCSX2_TEXTURES", true);
                subSetting("Texture Pack Folder", "The folder that holds replacements/. Empty: textures/SLUS-21503 next to "
                                                  "the game.");
                const std::string folder = state.ini.get("Runtime", "PS2X_PCSX2_TEXTURE_DIR");
                ImGui::TextWrapped("%s", folder.empty() ? "(next to the game)" : folder.c_str());
                if (ImGui::Button("Choose folder..."))
                {
                    const fs::path picked = platform::pick(GetWindowHandle(), true);
                    if (!picked.empty())
                        state.ini.set("Runtime", "PS2X_PCSX2_TEXTURE_DIR", utf8(picked));
                }
                if (!folder.empty())
                {
                    ImGui::SameLine();
                    if (ImGui::Button("Use default"))
                        state.ini.set("Runtime", "PS2X_PCSX2_TEXTURE_DIR", "");
                }
            }
        }

        void settingControl(State &state, const settings::Setting &setting)
        {
            const std::string id = std::string("##") + setting.id;
            switch (setting.kind)
            {
            case settings::Kind::Toggle:
                iniCheckbox(state, ("On" + id).c_str(), setting.section, setting.key, std::strcmp(setting.fallback, "1") == 0);
                break;
            case settings::Kind::Choice:
            {
                std::string value = settings::value(state.ini, setting);
                if (tableCombo(id.c_str(), setting, value))
                {
                    if (std::strcmp(setting.key, "CameraPreset") == 0)
                        settings::writeCameraPreset(state.ini, value); // its four values (Custom: from Action's)
                    else
                        state.ini.set(setting.section, setting.key, value);
                }
                break;
            }
            case settings::Kind::Slider:
            {
                const int number = std::atoi(settings::value(state.ini, setting).c_str());
                int index = 0;
                for (int i = 1; i < setting.valueCount; ++i)
                    if (std::abs(setting.values[i] - number) < std::abs(setting.values[index] - number))
                        index = i;
                std::string format = std::to_string(setting.values[index]);
                for (const char *c = setting.unit; *c; ++c)
                    format += *c == '%' ? std::string("%%") : std::string(1, *c);
                ImGui::SetNextItemWidth(-1.0f);
                if (ImGui::SliderInt(id.c_str(), &index, 0, setting.valueCount - 1, format.c_str(), ImGuiSliderFlags_AlwaysClamp))
                    state.ini.set(setting.section, setting.key, std::to_string(setting.values[index]));
                silentItem();
                break;
            }
            case settings::Kind::Custom:
                customControl(state, setting);
                break;
            }
        }

        bool otherCharacters(const State &state)
        {
            static const bool any = characters::list(state.context->exeDir).size() > 1;
            return any;
        }

        // One tab: its settings in the table's order, a heading per category. Sub-options show while their parent
        // allows them (Window Size greyed instead; Camera & FOV's values only with Custom).
        void drawTab(State &state, settings::Tab tab)
        {
            const bool narrow = lower(state.ini.get("Video", "AspectRatio", "16:9")) == "4:3";
            const char *category = nullptr;
            bool table = false;
            for (size_t i = 0; i < settings::kSettingCount; ++i)
            {
                const settings::Setting &s = settings::kSettings[i];
                if (s.tab != tab)
                    continue;
                const bool allowed = settings::parentAllows(state.ini, s);
                if (!allowed && !s.greyed)
                    continue;
                // Character only with a character besides Gene (built from the disc; none in a release yet).
                if (std::strcmp(s.id, "Character") == 0 && !otherCharacters(state))
                    continue;
                if (!category || std::strcmp(category, s.category) != 0)
                {
                    if (table)
                        ImGui::EndTable();
                    category = s.category;
                    table = beginCategory(s.category);
                }
                if (!table)
                    continue;
                if (s.parent)
                    subSetting(s.name, s.description, s.note);
                else
                    settingLabel(s.name, s.description, s.note);
                ImGui::BeginDisabled(!allowed || (s.wideOnly && narrow));
                settingControl(state, s);
                ImGui::EndDisabled();
            }
            if (table)
                ImGui::EndTable();
        }

        // ---- Options > Presets (K, Nov 16; claude/presets-design.md) ------------------------------------------------
        // The pause menu's photos, decoded from the player's disc image (never shipped).
        void readPresetPhotos(State &state)
        {
            if (state.presetPhotosRead || !state.installed)
                return;
            state.presetPhotosRead = true;
            std::string iso = state.ini.get("Game", "Iso");
            const fs::path image = iso.empty() ? state.context->iso : fromUtf8(iso);
            for (int p = 0; p < presets::kCount; ++p)
            {
                std::vector<uint8_t> tm2, rgba;
                int width = 0, height = 0;
                if (!disc_files::readFromImage(image, presets::photoPath(presets::kText[p].photo), tm2) ||
                    !presets::decodePhoto(tm2, width, height, rgba))
                {
                    KM_WARN(Boot, "launcher: the " << presets::kText[p].name << " preset's photo could not be read");
                    continue;
                }
                ::Image picture{rgba.data(), width, height, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                state.presetPhotos[p] = LoadTextureFromImage(picture);
                if (state.presetPhotos[p].id != 0u)
                    SetTextureFilter(state.presetPhotos[p], TEXTURE_FILTER_BILINEAR);
            }
        }

        void usePreset(State &state, int preset)
        {
            presets::write(state.ini, preset);
            state.ini.save();
            KM_INFO(Boot, "launcher: preset " << presets::kText[preset].name);
        }

        // A row per preset (its photo, name, tagline and what it adds, Use / In use); while the settings match none a
        // Custom banner on top, and Use asks before replacing them.
        void drawPresets(State &state)
        {
            readPresetPhotos(state);
            const int now = presets::current(state.ini);
            categoryGap();
            if (now == presets::kCustom)
            {
                ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
                ImGui::SeparatorText("Custom");
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
                ImGui::TextWrapped("%s %s", presets::kCustomText.tagline, presets::kCustomText.band);
                ImGui::PopStyleColor();
                categoryGap();
            }
            const float photoH = std::round(ImGui::GetFontSize() * 5.6f), photoW = std::round(photoH * presets::kPhotoAspect);
            const float buttonWidth = std::round(ImGui::GetFontSize() * 6.0f);
            if (!ImGui::BeginTable("presets", 3, ImGuiTableFlags_SizingFixedFit | ImGuiTableFlags_PadOuterX))
                return;
            ImGui::TableSetupColumn("photo", ImGuiTableColumnFlags_WidthFixed, photoW);
            ImGui::TableSetupColumn("text", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableSetupColumn("use", ImGuiTableColumnFlags_WidthFixed, buttonWidth);
            const float gap = std::round(ImGui::GetFontSize() * 0.45f);
            for (int p = 0; p < presets::kCount; ++p)
            {
                const presets::Text &text = presets::kText[p];
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0);
                if (p > 0)
                {
                    ImGui::Dummy(ImVec2(0.0f, gap));
                    dividerLine();
                    ImGui::Dummy(ImVec2(0.0f, gap));
                }
                if (state.presetPhotos[p].id != 0u)
                    rlImGuiImageSize(&state.presetPhotos[p], static_cast<int>(photoW), static_cast<int>(photoH));
                else
                    ImGui::Dummy(ImVec2(photoW, photoH));
                ImGui::TableSetColumnIndex(1);
                if (p > 0)
                    ImGui::Dummy(ImVec2(0.0f, 2.0f * gap + ImGui::GetStyle().ItemSpacing.y));
                pushSize(state, 26.0f);
                ImGui::TextUnformatted(text.name);
                ImGui::PopFont();
                ImGui::PushStyleColor(ImGuiCol_Text, kTextSoft);
                ImGui::TextWrapped("%s", text.tagline);
                ImGui::PopStyleColor();
                ImGui::PushStyleColor(ImGuiCol_Text, kTextDim);
                ImGui::TextWrapped("%s", text.band);
                ImGui::PopStyleColor();
                ImGui::TableSetColumnIndex(2);
                if (p > 0)
                    ImGui::Dummy(ImVec2(0.0f, 2.0f * gap + ImGui::GetStyle().ItemSpacing.y));
                ImGui::Dummy(ImVec2(0.0f, std::round(ImGui::GetFontSize() * 0.4f)));
                if (p == now)
                {
                    const char *inUse = "In use";
                    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 0.5f * (buttonWidth - ImGui::CalcTextSize(inUse).x));
                    ImGui::TextColored(kHeading, "%s", inUse);
                }
                else
                {
                    ImGui::PushID(p);
                    if (ImGui::Button("Use", ImVec2(buttonWidth, 0.0f)))
                    {
                        if (now == presets::kCustom)
                        {
                            state.presetAsk = p;
                            state.presetOpenPopup = true;
                        }
                        else
                            usePreset(state, p);
                    }
                    ImGui::PopID();
                }
            }
            ImGui::EndTable();

            // "Replace your custom settings?"
            if (state.presetOpenPopup)
            {
                state.presetOpenPopup = false;
                ImGui::OpenPopup("##presetreplace");
            }
            if (!ImGui::BeginPopupModal("##presetreplace", nullptr, ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoTitleBar))
                return;
            const bool back = ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false);
            const float popupButton = std::round(ImGui::GetFontSize() * 7.5f);
            ImGui::TextUnformatted("Replace your custom settings?");
            if (state.presetAsk >= 0 && state.presetAsk < presets::kCount)
                ImGui::TextColored(kTextDim, "%s sets them to its own.", presets::kText[state.presetAsk].name);
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 10.0f)));
            const bool keep = ImGui::Button("Keep mine", ImVec2(popupButton, 0.0f)); // first: the safe one
            ownSoundItem();
            if (ImGui::IsWindowAppearing())
                ImGui::SetItemDefaultFocus();
            ImGui::SameLine();
            const bool replace = ImGui::Button("Replace", ImVec2(popupButton, 0.0f));
            ownSoundItem();
            if (replace && state.presetAsk >= 0 && state.presetAsk < presets::kCount)
            {
                menuSound(state, kConfirm);
                usePreset(state, state.presetAsk);
                state.presetAsk = -1;
                ImGui::CloseCurrentPopup();
            }
            else if (keep || back)
            {
                menuSound(state, kCancel);
                state.presetAsk = -1;
                ImGui::CloseCurrentPopup();
            }
            ImGui::EndPopup();
        }

        // ---- the first run: a wanted poster per preset (K, Nov 16) ---------------------------------------------------
        void loadPosters(State &state)
        {
            if (state.postersLoaded)
                return;
            state.postersLoaded = true;
            const fs::path dir = state.context->exeDir / "resources" / "presets";
            std::ifstream layout(dir / "posters.txt");
            std::string word;
            while (layout >> word)
            {
                if (word == "size")
                    layout >> state.posterW >> state.posterH >> state.posterPad;
                else if (word == "poster")
                {
                    int n = -1;
                    float v[6] = {};
                    layout >> n >> v[0] >> v[1] >> v[2] >> v[3] >> v[4] >> v[5];
                    if (n >= 0 && n < presets::kCount)
                    {
                        std::copy(v, v + 4, state.photoBox[n]);
                        std::copy(v + 4, v + 6, state.stampAt[n]);
                    }
                }
                else
                    std::getline(layout, word); // a comment
            }
            const auto load = [&](Texture2D &texture, const std::string &name) {
                const fs::path path = dir / name;
                std::error_code error;
                if (!fs::is_regular_file(path, error))
                    return;
                texture = LoadTexture(utf8(path).c_str());
                if (texture.id != 0u)
                {
                    GenTextureMipmaps(&texture);
                    SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);
                }
            };
            for (int n = 0; n < presets::kCount; ++n)
            {
                load(state.poster[n], "poster_" + std::to_string(n) + ".png");
                load(state.posterTape[n], "poster_" + std::to_string(n) + "_tape.png");
                load(state.posterShadow[n], "poster_" + std::to_string(n) + "_shadow.png");
            }
            load(state.stamp, "stamp.png");
            readPresetPhotos(state);
        }

        // One layer of a poster: its rectangle (x, y, w, h in the poster's pixels) drawn with the poster at `centre`,
        // `scale` screen pixels per poster pixel, turned `angle` degrees (clockwise) round the poster's centre.
        void posterLayer(const State &state, const Texture2D &texture, float x, float y, float w, float h, Vector2 centre,
                         float scale, float angle, Color tint)
        {
            if (texture.id == 0u)
                return;
            DrawTexturePro(texture, Rectangle{0.0f, 0.0f, static_cast<float>(texture.width), static_cast<float>(texture.height)},
                           Rectangle{centre.x, centre.y, w * scale, h * scale},
                           Vector2{(0.5f * state.posterW - x) * scale, (0.5f * state.posterH - y) * scale}, angle, tint);
        }

        // Where the posters go this frame (screen pixels): the first one's left, their top, width and the gap.
        struct PosterRow
        {
            float left = 0.0f, top = 0.0f, width = 0.0f, height = 0.0f, gap = 0.0f;
        };
        PosterRow posterRow(const State &state)
        {
            PosterRow row;
            const float screenW = static_cast<float>(GetScreenWidth());
            row.gap = fontSize(state, 22.0f);
            const float margin = fontSize(state, 40.0f);
            row.width = std::min(fontSize(state, 267.0f), (screenW - 2.0f * margin - 3.0f * row.gap) / 4.0f);
            row.height = row.width * (state.posterH > 0.0f ? state.posterH / state.posterW : 1.6f);
            row.left = 0.5f * (screenW - 4.0f * row.width - 3.0f * row.gap);
            row.top = fontSize(state, 112.0f);
            return row;
        }

        // The posters (after ImGui: raylib draws them over the launcher's window): the picked one raised and stamped,
        // the others a little lower and darker, each turned a little, on its own soft shadow.
        void drawPosters(State &state)
        {
            if (state.screen != Screen::FirstRun || state.posterW <= 0.0f)
                return;
            constexpr float kTurn[presets::kCount] = {1.6f, -1.1f, 0.8f, -1.4f}; // degrees, clockwise (the mockup's)
            state.pickShown += (static_cast<float>(state.firstRunPick) - state.pickShown) * std::min(1.0f, GetFrameTime() * 14.0f);
            const PosterRow row = posterRow(state);
            const float scale = row.width / state.posterW;
            for (int n = 0; n < presets::kCount; ++n)
            {
                const float lift = std::clamp(1.0f - std::fabs(state.pickShown - static_cast<float>(n)), 0.0f, 1.0f);
                const Vector2 centre{row.left + (row.width + row.gap) * static_cast<float>(n) + 0.5f * row.width,
                                     row.top + 0.5f * row.height + fontSize(state, 9.0f) * (1.0f - lift)};
                const float angle = kTurn[n];
                const unsigned char shade = static_cast<unsigned char>(184.0f + 71.0f * lift);
                const Color tint{shade, shade, shade, 255};
                const Vector2 shadowAt{centre.x + fontSize(state, 3.3f + 3.4f * lift), centre.y + fontSize(state, 5.3f + 5.4f * lift)};
                posterLayer(state, state.posterShadow[n], -state.posterPad, -state.posterPad, state.posterW + 2.0f * state.posterPad,
                            state.posterH + 2.0f * state.posterPad, shadowAt, scale, angle,
                            Color{255, 255, 255, static_cast<unsigned char>(120.0f + 40.0f * lift)});
                posterLayer(state, state.poster[n], 0.0f, 0.0f, state.posterW, state.posterH, centre, scale, angle, tint);
                const float *box = state.photoBox[n];
                posterLayer(state, state.presetPhotos[n], box[0], box[1], box[2] - box[0], box[3] - box[1], centre, scale, angle, tint);
                posterLayer(state, state.posterTape[n], box[0], box[1], box[2] - box[0], box[3] - box[1], centre, scale, angle, tint);
                if (n == state.firstRunPick && state.stamp.id != 0u)
                {
                    const float sw = static_cast<float>(state.stamp.width), sh = static_cast<float>(state.stamp.height);
                    posterLayer(state, state.stamp, state.stampAt[n][0] - 0.5f * sw, state.stampAt[n][1] - 0.5f * sh, sw, sh, centre,
                                scale, angle, WHITE);
                }
            }
        }

        // "How do you want to play?": left / right (or a click) pick a poster, Continue uses its preset.
        void drawFirstRun(State &state)
        {
            loadPosters(state);
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 26.0f)));
            pushSize(state, 40.0f);
            centeredText("How do you want to play?", kHeading);
            ImGui::PopFont();
            pushSize(state, 20.0f);
            centeredText("Pick a preset to start with. You can change it, or any single option, later in Options or in game.",
                         kTextSoft);
            ImGui::PopFont();
            int pick = state.firstRunPick;
            const bool popup = ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId);
            if (!popup)
            {
                const auto pressed = [](std::initializer_list<ImGuiKey> keys) {
                    for (const ImGuiKey key : keys)
                        if (ImGui::IsKeyPressed(key, true))
                            return true;
                    return false;
                };
                if (pressed({ImGuiKey_LeftArrow, ImGuiKey_A, ImGuiKey_GamepadDpadLeft, ImGuiKey_GamepadLStickLeft}))
                    pick = std::max(pick - 1, 0);
                else if (pressed({ImGuiKey_RightArrow, ImGuiKey_D, ImGuiKey_GamepadDpadRight, ImGuiKey_GamepadLStickRight}))
                    pick = std::min(pick + 1, static_cast<int>(presets::kCount) - 1);
                if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
                {
                    const PosterRow row = posterRow(state);
                    const ImVec2 mouse = ImGui::GetMousePos();
                    for (int n = 0; n < presets::kCount; ++n)
                    {
                        const float x = row.left + (row.width + row.gap) * static_cast<float>(n);
                        if (mouse.x >= x && mouse.x < x + row.width && mouse.y >= row.top && mouse.y < row.top + row.height)
                            pick = n;
                    }
                }
            }
            if (pick != state.firstRunPick)
            {
                state.firstRunPick = pick;
                playMenuSound(state, kTick);
            }
            const PosterRow row = posterRow(state);
            ImGui::SetCursorPosY(row.top + row.height + fontSize(state, 40.0f));
            const std::string label = std::string("Continue with ") + presets::kText[state.firstRunPick].name;
            if (state.focusFirst)
            {
                ImGui::SetKeyboardFocusHere();
                ImGui::SetNavCursorVisible(true);
            }
            if (bigButton(state, label.c_str(), fontSize(state, 420.0f), true))
            {
                usePreset(state, state.firstRunPick);
                state.ini.setBool("Launcher", "PresetPicked", true);
                state.ini.save();
                state.firstRunPending = false;
                state.screen = Screen::Home;
                state.focusFirst = true;
            }
            state.focusFirst = false;
        }

        void drawOptions(State &state)
        {
            g_captureFrame = false;
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 30.0f)));
            pushSize(state, 46.0f);
            centeredText("Options", kHeading);
            ImGui::PopFont();
            const float width = std::min(ImGui::GetWindowWidth() - fontSize(state, 60.0f), fontSize(state, 1040.0f));
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - width) * 0.5f);
            pushSize(state, 21.0f);
            const float footer = ImGui::GetFrameHeightWithSpacing() + fontSize(state, 36.0f);
            ImGui::BeginChild("options", ImVec2(width, ImGui::GetContentRegionAvail().y - footer),
                              ImGuiChildFlags_Borders | ImGuiChildFlags_AlwaysUseWindowPadding);
            g_categories = 0;
            if (ImGui::BeginTabBar("tabs"))
            {
                // Presets first (K, Nov 16).
                if (ImGui::BeginTabItem("Presets"))
                {
                    drawPresets(state);
                    ImGui::EndTabItem();
                }
                // The launcher's own order (K, Nov 13: Controls after Sound; the game keeps Controls first).
                constexpr settings::Tab kLauncherTabs[] = {settings::Tab::Display,  settings::Tab::Graphics,
                                                           settings::Tab::Interface, settings::Tab::Gameplay,
                                                           settings::Tab::Sound,    settings::Tab::Controls,
                                                           settings::Tab::Mods};
                for (const settings::Tab tab : kLauncherTabs)
                {
                    if (!ImGui::BeginTabItem(settings::tabName(tab)))
                        continue;
                    drawTab(state, tab);
                    if (tab == settings::Tab::Controls)
                        drawControls(state); // the bindings and hotkeys
                    ImGui::EndTabItem();
                }
                ImGui::EndTabBar();
            }
            ImGui::EndChild();
            ImGui::Dummy(ImVec2(0.0f, fontSize(state, 6.0f)));
            const float backWidth = fontSize(state, 200.0f);
            ImGui::SetCursorPosX((ImGui::GetWindowWidth() - backWidth) * 0.5f);
            const bool back = ImGui::Button("Back", ImVec2(backWidth, 0.0f));
            ownSoundItem();
            if (state.focusFirst)
                ImGui::SetItemDefaultFocus();
            const bool cancel = !g_captureFrame && !ImGui::IsPopupOpen("", ImGuiPopupFlags_AnyPopupId) &&
                                (ImGui::IsKeyPressed(ImGuiKey_GamepadFaceRight, false) || ImGui::IsKeyPressed(ImGuiKey_Escape, false));
            ImGui::PopFont();
            if ((back || cancel) && !state.optionsLeaving)
            {
                if (state.saloonOn)
                    bookSound(state, false); // back out of the book
                else
                    menuSound(state, kCancel);
                state.ini.save();
                saveControls();
                g_resetArmed = false;
                if (state.saloonOn) // the panel fades out, then the zoom plays backwards (the main loop)
                {
                    state.optionsLeaving = true;
                    return;
                }
                state.screen = Screen::Home;
                state.focusFirst = true;
                return;
            }
            state.focusFirst = false;
        }
    }

    bool wanted(bool haveImage)
    {
        if (!haveImage || platform::shiftHeld())
            return true;
        const char *value = std::getenv("KAMITE_LAUNCHER");
        return !(value && *value == '0');
    }

    Result run(const Context &context)
    {
        State state;
        state.context = &context;
        if (!state.ini.load(context.iniPath))
            KM_WARN(Boot, "launcher: cannot read " << utf8(context.iniPath));
        // [Video] CameraDistance (100 = Gene keeps his size) became CameraZoomOut (0 = that) on Oct 30: carry it over.
        if (state.ini.get("Video", "CameraZoomOut").empty() && !state.ini.get("Video", "CameraDistance").empty())
        {
            char text[32];
            std::snprintf(text, sizeof(text), "%g", 100.0 - std::atof(state.ini.get("Video", "CameraDistance").c_str()));
            state.ini.set("Video", "CameraZoomOut", text);
        }
        std::error_code error;
        // A new install (K, Nov 16): no boot ELF checked out yet and no preset picked: the wanted posters once.
        state.firstRunPending = !state.ini.getBool("Launcher", "PresetPicked", false) &&
                                !fs::exists(context.cacheDir / kBootElfPath, error);
        state.installed = !context.iso.empty() && fs::is_regular_file(context.iso, error) && context.message.empty();
        if (!context.iso.empty())
            std::snprintf(state.isoText, sizeof(state.isoText), "%s", utf8(context.iso).c_str());
        if (!context.message.empty())
        {
            state.installMessage = context.message;
            state.installError = true;
            state.screen = Screen::Install;
        }

        ps2_host_display::openHostWindow(kWindowTitle);
        // Without vsync: the monitor's own rate (a 60 cap on a faster screen judders Play's animation; K Nov 10).
        if (!ps2_host_display::vsyncEnabled())
        {
            const int rate = GetMonitorRefreshRate(GetCurrentMonitor());
            SetTargetFPS(rate >= 60 && rate <= 360 ? rate : 60);
        }
        rlImGuiSetup(true);
        ImGuiIO &io = ImGui::GetIO();
        io.IniFilename = nullptr;
        io.ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard | ImGuiConfigFlags_NavEnableGamepad;
        const fs::path fontPath = context.exeDir / "resources" / "fonts" / "Roboto-Regular.ttf";
        if (fs::is_regular_file(fontPath, error))
            state.font = io.Fonts->AddFontFromFileTTF(utf8(fontPath).c_str(), 20.0f);
        applyStyle();
        const auto loadImage = [&](Texture2D &texture, const char *name)
        {
            const fs::path path = context.exeDir / "resources" / "images" / name;
            if (!fs::is_regular_file(path, error))
                return;
            texture = LoadTexture(utf8(path).c_str());
            if (texture.id != 0u)
            {
                GenTextureMipmaps(&texture);
                SetTextureFilter(texture, TEXTURE_FILTER_TRILINEAR);
            }
        };
        loadImage(state.logo, "logo.png");
        loadImage(state.kana, "kana.png");
        loadImage(state.rearmed, "rearmed.png");
        loadImage(state.rearmedText, "rearmed_text.png");
        loadImage(state.rearmedGlow, "rearmed_glow.png");
        if (state.kana.id != 0u && state.logo.id != 0u)
            state.scene.setKana(state.kana, kLogoContent, static_cast<float>(state.logo.height) / static_cast<float>(state.logo.width));
        const fs::path burstPath = context.exeDir / "resources" / "images" / "burst.png";
        if (fs::is_regular_file(burstPath, error))
        {
            state.burst = LoadTexture(utf8(burstPath).c_str());
            if (state.burst.id != 0u)
            {
                GenTextureMipmaps(&state.burst);
                SetTextureFilter(state.burst, TEXTURE_FILTER_TRILINEAR);
            }
        }
        loadSplash(state, context.exeDir / "resources" / "splash");
        // The saloon (resources/saloon): its images decode on worker threads while the launcher starts.
        state.saloonOn = state.scene.open(context.exeDir / "resources" / "saloon");
        if (state.saloonOn)
        {
            if (!saloon::loadBillFonts(state.billFonts, context.exeDir / "resources" / "fonts", state.splash))
            {
                KM_WARN(Boot, "launcher: the bill's fonts are missing from resources/fonts, plain launcher");
                state.scene.close();
                state.saloonOn = false;
            }
        }
        InitAudioDevice();
        state.audio = IsAudioDeviceReady();
        const fs::path musicPath = context.exeDir / "resources" / "music" / "launcher.ogg";
        state.musicPath = musicPath;
        if (state.audio && fs::is_regular_file(musicPath, error))
        {
            state.music = LoadMusicStream(utf8(musicPath).c_str());
            state.hasMusic = IsMusicValid(state.music);
            if (state.hasMusic)
            {
                state.music.looping = true; // the file is cut to loop seamlessly
                SetMusicVolume(state.music, kMusicVolume);
                PlayMusicStream(state.music);
            }
        }
        // The saloon's sounds (K's, resources/sfx/launcher): the bar's murmur (a loop under the music, saloon only), the
        // pen's scribble round a loop, the smoothie's last sip.
        if (state.audio)
        {
            const fs::path sfx = context.exeDir / "resources" / "sfx" / "launcher";
            if (fs::is_regular_file(sfx / "bar_ambience.ogg", error))
            {
                state.ambience = LoadMusicStream(utf8(sfx / "bar_ambience.ogg").c_str());
                state.hasAmbience = IsMusicValid(state.ambience);
                if (state.hasAmbience)
                {
                    state.ambience.looping = true;
                    SetMusicVolume(state.ambience, 0.0f);
                    PlayMusicStream(state.ambience);
                }
            }
            const auto effect = [&](const char *name, Sound &sound) {
                if (!fs::is_regular_file(sfx / name, error))
                    return false;
                sound = LoadSound(utf8(sfx / name).c_str());
                return sound.frameCount > 0;
            };
            state.hasPen = effect("pen_circle.ogg", state.pen);
            state.hasSlurp = effect("smoothie_drink.ogg", state.slurp);
            state.hasBite = effect("eat_fruit.ogg", state.bite);
            state.hasBookOpen = effect("book_open.ogg", state.bookOpen);
            state.hasBookClose = effect("book_close.ogg", state.bookClose);
        }
        state.volume = std::clamp(static_cast<float>(std::atof(state.ini.get("Launcher", "Volume", "0.8").c_str())), 0.0f, 1.0f);
        state.exePath = platform::thisExe();
        update::cleanup(context.exeDir);
        if (g_updateCheck.state.load() == static_cast<int>(update::CheckState::Off))
            update::startCheck(g_updateCheck, state.ini.getBool("Launcher", "UpdateTest", false)); // a dev build's test switch
        KM_INFO(Boot, "launcher: " << update::versionLabel());
#if KAMITE_LAUNCHER_STEAM
        state.steam = steam::probe();
        KM_INFO(Boot, "launcher: steam " << (state.steam.available ? (state.steam.added ? "found, entry there" : "found") : "not found"));
#endif
        state.muted = state.ini.getBool("Launcher", "Mute", false);
        if (state.installed)
        {
            loadMenuSounds(state, context.iso);
            loadSnackVoices(state, context.iso);
            startBark(state, context.iso);
        }
        KM_INFO(Boot, "launcher: shown (" << (state.installed ? "installed" : "not installed") << ")");

        while (!state.done)
        {
            if (WindowShouldClose())
            {
                state.result = Result::Quit;
                break;
            }
            state.unit = std::clamp(static_cast<float>(GetScreenHeight()) / 900.0f, 0.55f, 3.0f);
            feedGamepads(io);
            updateMotion(state, state.screen);
            state.logoThisFrame = false;
            state.soundThisFrame = false;
            state.penThisFrame = false;
            g_tickItems.clear();
            g_ownSoundItems.clear();
            g_silentItems.clear();
            if (state.playTime >= 0.0f)
            {
                if (state.playTime == 0.0f)
                    playBark(state);
                state.playTime += std::min(GetFrameTime(), 0.05f);
                if (state.playTime >= kPlayLength)
                {
                    state.result = Result::Play;
                    state.done = true;
                }
            }
            if (state.hasMusic && !state.musicHandedOff)
            {
                const float fade = state.playTime >= 0.0f ? std::clamp(state.playTime / (kPlayLength - 0.02f), 0.0f, 1.0f) : 0.0f;
                SetMusicVolume(state.music, kMusicVolume * userVolume(state) * (1.0f - fade));
                UpdateMusicStream(state.music);
            }
            if (state.hasAmbience) // out with the music on Play; silent without the saloon
            {
                const float fade = state.playTime >= 0.0f ? std::clamp(state.playTime / (kPlayLength - 0.02f), 0.0f, 1.0f) : 0.0f;
                SetMusicVolume(state.ambience, state.saloonOn ? kEffectGain * state.scene.tuning().ambience * userVolume(state) * (1.0f - fade) : 0.0f);
                UpdateMusicStream(state.ambience);
            }
            if (state.voiceAt >= 0.0f && GetTime() >= state.voiceAt)
            {
                state.voiceAt = -1.0f;
                if (state.screen == Screen::Home && state.playTime < 0.0f)
                    snackVoice(state);
            }
            if (state.saloonOn && state.scene.failed())
            {
                KM_WARN(Boot, "launcher: the saloon failed to load, plain launcher");
                state.scene.close();
                state.saloonOn = false;
            }
            if (state.saloonOn)
            {
                const float dt = std::min(GetFrameTime(), 0.1f);
                saloon::Scene::Frame frame;
                frame.dt = dt;
                frame.parallax = Vector2{state.parallax.x, state.parallax.y};
                frame.volume = userVolume(state); // the curry's steam (muted: cold curry)
                frame.bookLit = state.screen == Screen::Home && state.bill.focus == static_cast<int>(kBillOptions);
                state.scene.update(frame);
                state.bill.focusAge += dt;
                if (state.zoomPending && state.screen == Screen::Home && (state.scene.zoomReady() || state.scene.zoomBroken()))
                {
                    state.zoomPending = false;
                    openOptions(state, false); // (its sound played at the press)
                }
                // The zoom landed: Options' panel fades in over its last frame. Back: it fades out, then the zoom
                // plays backwards (the bill's loop back on Options).
                if (state.screen == Screen::Home && state.scene.landed())
                {
                    state.screen = Screen::Options;
                    state.focusFirst = true;
                    state.optionsShown = 0.0f;
                    state.optionsLeaving = false;
                }
                if (state.screen == Screen::Options)
                {
                    const float target = state.optionsLeaving ? 0.0f : 1.0f;
                    state.optionsShown += std::clamp(target - state.optionsShown, -dt / 0.12f, dt / 0.15f);
                    if (state.optionsLeaving && state.optionsShown <= 0.0f)
                    {
                        state.optionsLeaving = false;
                        state.screen = Screen::Home;
                        state.focusOptions = true;
                        state.scene.zoomOut();
                    }
                }
            }
            const bool playing = state.playTime >= 0.0f;
            BeginDrawing();
            if (state.saloonOn)
            {
                // Install and the first run's posters over Home darkened about halfway (K); Options without its zoom too.
                const float dim = state.screen == Screen::Install || state.screen == Screen::FirstRun                ? 0.5f
                                  : state.screen == Screen::Options && state.scene.zoomFrame() == 0 ? 0.65f
                                                                                                         : 0.0f;
                state.scene.draw(dim);
                // The logo in the doorway's glare (not on Options or the posters), riding the zoom's frames.
                Rectangle at{};
                const bool shown = state.logo.id != 0u && state.screen != Screen::Options && state.screen != Screen::FirstRun &&
                                   state.scene.ready() &&
                                   state.scene.logo(kLogoContent, static_cast<float>(state.logo.height) / static_cast<float>(state.logo.width), at);
                state.logoPlaced = shown;
                state.logoThisFrame = shown;
                if (shown)
                {
                    state.logoMin = ImVec2(at.x, at.y);
                    state.logoMax = ImVec2(at.x + at.width, at.y + at.height);
                }
            }
            else
                drawBackground();
            drawBurst(state);
            if (state.saloonOn)
                drawSaloonInk(state);
            // A popup (Add to Steam's) must sit over the logo: while one is open on Home the logo goes under ImGui
            // (only on Home: the other screens draw no logo).
            const bool logoUnder = state.popupWasOpen && !playing && state.screen == Screen::Home;
            if (logoUnder)
            {
                drawLogo(state, true);
                if (!state.saloonOn)
                    drawSplash(state);
            }
            rlImGuiBegin();
            ImGui::SetNextWindowPos(ImVec2(0.0f, 0.0f));
            ImGui::SetNextWindowSize(io.DisplaySize);
            if (ImGui::Begin("##launcher", nullptr,
                             ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove | ImGuiWindowFlags_NoSavedSettings |
                                 ImGuiWindowFlags_NoBringToFrontOnFocus))
            {
                // While Play's animation runs nothing takes input (and nothing greys out: it is fading anyway).
                if (playing)
                {
                    ImGui::PushStyleVar(ImGuiStyleVar_DisabledAlpha, 1.0f);
                    ImGui::BeginDisabled();
                }
                state.slipFocus = kSlipNone;
                switch (state.screen)
                {
                case Screen::Home:
                    drawHome(state);
                    break;
                case Screen::Install:
                    drawInstall(state);
                    break;
                case Screen::Options:
                    if (state.saloonOn) // over the zoom's last frame, fading in once it lands and out on Back
                    {
                        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, std::clamp(state.optionsShown, 0.0f, 1.0f));
                        const bool leaving = state.optionsLeaving; // Back sets it inside
                        if (leaving)
                            ImGui::BeginDisabled();
                        drawOptions(state);
                        if (leaving)
                            ImGui::EndDisabled();
                        ImGui::PopStyleVar();
                    }
                    else
                        drawOptions(state);
                    break;
                case Screen::FirstRun:
                    drawFirstRun(state);
                    break;
                }
                if (state.screen != Screen::Options)
                    drawVolume(state);
                if (state.screen == Screen::Home)
                    drawUpdate(state);
#if KAMITE_LAUNCHER_STEAM
                if (state.screen == Screen::Home)
                    drawSteam(state);
#endif
                if (playing)
                {
                    ImGui::EndDisabled();
                    ImGui::PopStyleVar();
                }
            }
            ImGui::End();
            if (state.slipFocus != state.lastSlipFocus && state.slipFocus != kSlipNone)
                penSound(state); // the slip's loop
            state.lastSlipFocus = state.slipFocus;
            frameSounds(state);
            rlImGuiEnd();
            if (!logoUnder)
            {
                drawLogo(state);
                if (!state.saloonOn)
                    drawSplash(state);
            }
            drawPosters(state);
            if (playing)
            {
                DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(),
                              Color{0, 0, 0, alphaOf(state.playTime / (kPlayLength - 0.02f))});
                if (state.logoThisFrame)
                    drawPlate(state, state.playTime);
            }
            EndDrawing();
        }
        state.ini.save();
        saveControls();
        if (state.logo.id != 0u)
            UnloadTexture(state.logo);
        for (Texture2D *texture : {&state.kana, &state.rearmed, &state.rearmedText, &state.rearmedGlow})
            if (texture->id != 0u)
                UnloadTexture(*texture);
        if (state.burst.id != 0u)
            UnloadTexture(state.burst);
        for (Texture2D &photo : state.presetPhotos)
            if (photo.id != 0u)
                UnloadTexture(photo);
        for (int n = 0; n < presets::kCount; ++n)
            for (Texture2D *texture : {&state.poster[n], &state.posterTape[n], &state.posterShadow[n]})
                if (texture->id != 0u)
                    UnloadTexture(*texture);
        if (state.stamp.id != 0u)
            UnloadTexture(state.stamp);
        saloon::unloadBillFonts(state.billFonts);
        state.scene.close(); // its threads and textures, before the game has the window
        if (state.splashFont.texture.id != 0u)
            UnloadFont(state.splashFont);
        if (state.barkWorker.joinable())
            state.barkWorker.join();
#if KAMITE_LAUNCHER_STEAM
        if (state.steamWorker.joinable())
            state.steamWorker.join();
#endif
        if (state.hasMusic)
            UnloadMusicStream(state.music);
        if (state.hasAmbience) // (faded out by Play; stopped before the device goes to the game, or it would repeat)
        {
            StopMusicStream(state.ambience);
            UnloadMusicStream(state.ambience);
        }
        // A check still running is left to finish on its own (the game's start does not wait for GitHub).
        if (g_updateCheck.worker.joinable())
            g_updateCheck.worker.detach();
        if (g_updater.worker.joinable())
            g_updater.worker.detach();
        if (state.result == Result::Play && state.audio)
        {
            // The game takes the audio device over (the runtime opens it only when it is not open yet), so Play's
            // sound rings on while the game boots; the sound's few samples are left to the device until then.
            KM_INFO(Boot, "launcher: audio device handed to the game");
        }
        else
        {
            for (int i = 0; i < 4; ++i)
                if (state.hasMenuSound[i])
                    UnloadSound(state.menuSounds[i]);
            for (Sound &voice : state.snackVoices)
                UnloadSound(voice);
            for (const auto &[loaded, sound] : {std::pair{state.hasPen, &state.pen}, std::pair{state.hasSlurp, &state.slurp},
                                                std::pair{state.hasBite, &state.bite}, std::pair{state.hasBookOpen, &state.bookOpen},
                                                std::pair{state.hasBookClose, &state.bookClose}})
                if (loaded)
                    UnloadSound(*sound);
            if (state.audio)
                CloseAudioDevice();
        }
        rlImGuiShutdown(); // the F12 debugger sets ImGui up again later
        // One empty frame, so the window does not keep showing the launcher until the game's first picture.
        BeginDrawing();
        ClearBackground(BLACK);
        EndDrawing();
        KM_INFO(Boot, "launcher: " << (state.result == Result::Play ? "play" : "quit"));
        return state.result;
    }
}
