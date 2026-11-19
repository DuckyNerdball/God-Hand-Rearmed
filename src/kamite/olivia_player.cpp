// Playable Olivia at run time (see olivia_player.h).
#include "kamite/olivia_player.h"
#include "kamite/body_detail.h"
#include "kamite/disc_files.h"
#include "kamite/file_layer.h"
#include "kamite/function_hooks.h"
#include "kamite/game_layout.h"
#include "kamite/log.h"
#include "kamite/olivia_player_build.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <atomic>
#include <cctype>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <string>

namespace kamite::olivia_player
{
    namespace
    {
        std::atomic<bool> g_active{false};
        std::atomic<float> g_ratio{1.0f};

        // Gene's body files (func_0030D8E0: 0x100 + his costume) and coat files (the table at 0x450590).
        constexpr const char *kBodies[] = {"pl/pl00.dat", "pl/pl02.dat", "pl/pl05.dat", "pl/pl07.dat", "pl/pl09.dat",
                                           "pl/pl0e.dat", "pl/pl0f.dat", "pl/pl10.dat", "pl/pl11.dat"};
        constexpr const char *kCoats[] = {"pl/pl01.dat", "pl/pl06.dat", "pl/pl08.dat", "pl/pl0a.dat", "pl/pl12.dat"};

        // K's picks (Nov 19): the skin's warmth toward cutscene Olivia's (0..1: medium; all the way also reddens her eyes,
        // which share the face sheet's palette), a spread palm made from her open hand for Gene's 02, her fist for 03.
        constexpr float kSkin = 0.65f;
        constexpr int kHand2 = 2, kHand3 = 1;

        std::string lower(const char *s)
        {
            std::string out = s ? s : "";
            for (char &c : out)
                c = static_cast<char>(std::tolower(static_cast<unsigned char>(c)));
            return out;
        }

        bool readDisc(const char *path, std::vector<uint8_t> &out)
        {
            const auto file = disc_files::find(path);
            return file && disc_files::read(*file, out) && !out.empty();
        }

        // Gene's coat (the player's +0x68C, pl01 ...): every mesh node hidden each time it is packed for drawing. Her
        // coat files are empty already, but a coat built from Gene's file stays (K, Nov 19: the dress run showed his coat
        // after a save state and a memory card load), so it is hidden where it is drawn too.
        constexpr uint32_t kGetMeshPtr = 0x0014B6F8u, kTransCallSite = 0x0014A4C0u, kCoat = 0x68Cu;
        constexpr uint32_t kMeshHead = 0x25Cu, kNodeNext = 0x404u, kNodeFlags = 0x380u;
        uint32_t rd32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (a & (PS2_RAM_MASK & ~3u)), 4);
            return v;
        }
        bool inRam(uint32_t a, uint32_t size)
        {
            a &= PS2_RAM_MASK;
            return a >= 0x100000u && size <= PS2_RDRAM_SIZE - a;
        }
        void hideCoatNodes(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (GPR_U32(ctx, 31) != kTransCallSite)
                return;
            const uint32_t player = rd32(rdram, game::GameWork::address(game::GameWork::kPlayerObj));
            if (!inRam(player, kCoat + 4u))
                return;
            const uint32_t coat = rd32(rdram, player + kCoat);
            if (coat == 0u || GPR_U32(ctx, 4) != coat || !inRam(coat, 0x2E4u))
                return;
            uint32_t node = rd32(rdram, coat + kMeshHead);
            for (int i = 0; i < 64 && inRam(node, 0x418u); ++i)
            {
                uint32_t flags = rd32(rdram, node + kNodeFlags);
                flags |= 1u;
                std::memcpy(rdram + ((node + kNodeFlags) & PS2_RAM_MASK), &flags, 4);
                node = rd32(rdram, node + kNodeNext);
            }
        }

        // lwc1 $f1,0x404($s1) where the camera adds its look height to Gene's feet (func_00139FF8 0x13A3E4,
        // func_0013AD90 0x13B25C; s1 = cPlCamera): her ratio times it.
        void lookHeight(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            float v;
            std::memcpy(&v, rdram + ((GPR_U32(ctx, 17) + 0x404u) & PS2_RAM_MASK), 4);
            ctx->f[1] = v * g_ratio.load(std::memory_order_relaxed);
        }
    }

    bool active() { return g_active.load(std::memory_order_relaxed); }
    float ratio() { return g_ratio.load(std::memory_order_relaxed); }

    void install(PS2Runtime &)
    {
        ps2xRegisterHostHook("km_cam_look_height", &lookHeight); // flag "fov" (always on): Gene's ratio is 1
        if (lower(std::getenv("KAMITE_CHARACTER")) != "olivia")
            return;
        const auto start = std::chrono::steady_clock::now();
        if (!disc_files::open())
        {
            KM_WARN(Boot, "olivia: the disc image has no file archive; playing as Gene");
            return;
        }
        Inputs in;
        in.costume = lower(std::getenv("KAMITE_COSTUME")) == "bunny" ? Costume::Bunny : Costume::Dress;
        in.skin = kSkin;
        in.hand2 = kHand2;
        in.hand3 = kHand3;
        if (!readDisc("pl/pl00.dat", in.pl00) || !readDisc("em/ema0.dat", in.ema0) ||
            (in.costume == Costume::Bunny && !readDisc("el/elf4.dat", in.elf4)))
        {
            KM_WARN(Boot, "olivia: her files could not be read from the disc; playing as Gene");
            return;
        }
        in.ema0Md = body_detail::grownBlock("em/ema0.dat");
        in.elf4Md = body_detail::grownBlock("el/elf4.dat");
        Built built;
        if (!build(in, built))
        {
            KM_WARN(Boot, "olivia: " << built.error << "; playing as Gene");
            return;
        }
        int bodies = 0, coats = 0;
        for (const char *path : kBodies)
            if (disc_files::find(path) && file_layer::replace(path, built.body))
                ++bodies;
        for (const char *path : kCoats)
        {
            std::vector<uint8_t> coat, hidden;
            if (readDisc(path, coat) && hideCoat(coat, hidden) && file_layer::replace(path, std::move(hidden)))
                ++coats;
        }
        if (bodies == 0)
        {
            KM_WARN(Boot, "olivia: none of Gene's body files could be replaced; playing as Gene");
            return;
        }
        g_ratio = built.ratio;
        g_active = true;
        FunctionHooks::instance().observeEntry(kGetMeshPtr, "olivia_coat", &hideCoatNodes);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        KM_INFO(Boot, "olivia: playing as Olivia (" << (in.costume == Costume::Bunny ? "bunny suit" : "dress") << "; " << built.nodes
                                                   << " nodes, " << built.body.size() << " bytes, textures " << built.textureBytes / 1024u
                                                   << " KB; her height / Gene's " << built.ratio << ", hips " << built.hipDy << " m, ankles "
                                                   << built.ankleDy << " m; " << built.channels << " channels in " << built.motions
                                                   << " motions; " << built.skinColours << " skin colours; " << built.spreadHands << " spread hands; " << bodies << " body and "
                                                   << coats << " coat files; Money / Cash "
                                                   << ((in.costume == Costume::Bunny ? in.elf4Md : in.ema0Md).empty() ? "off (no rebuilt body)" : "on") << "; " << ms << " ms)");
    }
}
