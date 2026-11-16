// The women's detailed bust / seat at run time (see body_detail.h).
#include "kamite/body_detail.h"
#include "kamite/body_detail_build.h"
#include "kamite/bone_tweaks.h"
#include "kamite/disc_files.h"
#include "kamite/function_hooks.h"
#include "kamite/log.h"
#include "runtime/ps2_cd_image.h"
#include "ps2_runtime.h"
#include "ps2_runtime_macros.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <utility>
#include <vector>

namespace kamite::body_detail
{
    namespace
    {
        struct Model
        {
            const char *file;
            uint32_t offset, size;
            uint64_t hash;
            bool bust, seat;
        };
        constexpr Model kModels[] = {
#include "kamite/body_detail_models.inc"
        };
        // Models kept as they are, with Cash's growth on their own seats (growOnly): file, block, size, FNV-1a 64.
        struct GrowModel
        {
            const char *file;
            uint32_t offset, size;
            uint64_t hash;
            GrowRules rules; // clothing layers (skirt, the body under it, a belt plate)
            int jiggle = 0;  // the jiggle bones (kJiggle*) on the nodes below, re-skinned in place (body_detail_build reskin)
            const char *reskin[2] = {nullptr, nullptr};
        };
        constexpr GrowRules kOliviaSkirt{"skirt00", "dress", "z_blt00"}, kOliviaSkirt2{"skirt00", "dress2", "z_blt00"};
        constexpr GrowModel kGrowModels[] = {
            {"em/em52.dat", 0x31580u, 0x47640u, 0x97A8C864A29AC1F0ULL, {}, kJiggleSeat | kJiggleBust, {"body_u1", "BY_00"}}, // Shannon
            {"em/ema0.dat", 0x28060u, 0x47700u, 0xF37EDAD64FF2AE63ULL, kOliviaSkirt, kJiggleBust, {"dress", nullptr}}, // Olivia (skirt: no seat)
            {"el/el0a.dat", 0x577A0u, 0x21640u, 0x7E94290CBD92FD0FULL, kOliviaSkirt2}, // Olivia, cutscenes (dress)
            {"el/elf5.dat", 0x273E0u, 0x29D80u, 0xDBA0ADACA05FA7D8ULL, kOliviaSkirt2}, // Olivia, cutscenes (dress + extras)
            {"el/elf4.dat", 0x25A20u, 0x1E700u, 0x16E4FA36326B5C40ULL, {}},           // Olivia, cutscenes (bunny suit)
            {"el/el15.dat", 0x24EE0u, 0x1C840u, 0x2C2C47653973ABDAULL, {}},           // Shannon, cutscenes
            {"el/el1a.dat", 0xF460u, 0x46200u, 0x045B558E999D846BULL, {}},            // Shannon, cutscenes (whole)
            // not el09 (the bathtub Olivia: no hips)
        };

        constexpr uint32_t kGetMeshPtr = 0x0014B6F8u;    // cModel_getMeshPtr(model, index)
        constexpr uint32_t kTransCallSite = 0x0014A4C0u; // the return address of its first call in cModel_Trans
        constexpr uint32_t kMeshHead = 0x25Cu, kNodeNext = 0x404u, kNodeInfo = 0x414u, kInfoName = 0x8u, kNodeMdb = 0x388u;
        constexpr float kOn = 1.0005f; // Money / Cash above 100% (they are stored as scales)

        // A rebuilt body, shared by every model whose block had the same bytes.
        struct Body
        {
            std::vector<std::pair<uint32_t, std::vector<uint8_t>>> id; // mdb bytes no state changes: what identifies it
            uint32_t size = 0;                                       // the mdb's size
            std::vector<Change> changes;
            int sentinel[3] = {-1, -1, -1};
            bool bust = false, seat = false;
            int jiggle = 0; // kJiggle*: the jiggle bones its records use (bone_tweaks places them every draw)
            JiggleBind bind;
        };
        std::vector<Body> g_bodies; // set at boot, read-only afterwards
        std::vector<std::string> g_nodeNames{"body"}; // the nodes check() looks at (the grown models' too)

        uint64_t fnv(const std::vector<uint8_t> &b)
        {
            uint64_t h = 0xCBF29CE484222325ull;
            for (const uint8_t x : b)
                h = (h ^ x) * 0x100000001B3ull;
            return h;
        }

        uint32_t rd32(const uint8_t *rdram, uint32_t a)
        {
            uint32_t v;
            std::memcpy(&v, rdram + (a & (PS2_RAM_SIZE - 4u)), 4);
            return v;
        }
        bool inRam(uint32_t a, uint32_t size)
        {
            a &= PS2_RAM_MASK;
            return a >= 0x100000u && size <= PS2_RAM_SIZE - a;
        }

        // The bytes of a body's mdb that neither state touches: the header's first 12 (with the buffer and bone
        // counts), the buffer table, buffer 0's header and its first UVs.
        void identify(Body &body, const uint8_t *m, uint32_t size)
        {
            uint16_t nv;
            std::memcpy(&nv, m + 10, 2);
            uint32_t vb0, uv;
            uint16_t count;
            std::memcpy(&vb0, m + 32, 4);
            std::memcpy(&uv, m + vb0 + 8, 4);
            std::memcpy(&count, m + vb0 + 20, 2);
            auto take = [&](uint32_t at, uint32_t n) { body.id.emplace_back(at, std::vector<uint8_t>(m + at, m + at + n)); };
            take(0, 12);
            take(32, 4u * nv);
            take(vb0, 32);
            take(vb0 + uv, std::min<uint32_t>(64u, 4u * count));
            body.size = size;
        }

        // The rebuilt body an mdb is (its bytes no state changes), or nullptr.
        const Body *bodyOf(const uint8_t *rdram, uint32_t mdb)
        {
            for (const Body &b : g_bodies)
            {
                if (!inRam(mdb, b.size))
                    continue;
                const uint8_t *p = rdram + (mdb & PS2_RAM_MASK);
                bool same = true;
                for (const auto &[at, bytes] : b.id)
                    if (std::memcmp(p + at, bytes.data(), bytes.size()) != 0)
                    {
                        same = false;
                        break;
                    }
                if (same)
                    return &b;
            }
            return nullptr;
        }

        // The model's "body" node's mdb, or 0.
        uint32_t bodyMdb(const uint8_t *rdram, uint32_t model)
        {
            if (!inRam(model, 0x2E4u))
                return 0u;
            uint32_t node = rd32(rdram, model + kMeshHead);
            for (int i = 0; i < 64 && inRam(node, 0x418u); ++i)
            {
                const uint32_t info = rd32(rdram, node + kNodeInfo);
                if (inRam(info, 16u) && std::memcmp(rdram + ((info + kInfoName) & PS2_RAM_MASK), "body\0", 5) == 0)
                    return rd32(rdram, node + kNodeMdb);
                node = rd32(rdram, node + kNodeNext);
            }
            return 0u;
        }

        // A body node drawn: its bust / seat to the state Money / Cash ask for.
        void check(uint8_t *rdram, uint32_t mdb)
        {
            const Body *b = bodyOf(rdram, mdb);
            if (!b)
                return;
            uint8_t *p = rdram + (mdb & PS2_RAM_MASK);
            const float cash = bone_tweaks::cash(); // (the reads are relaxed atomics: cheap per node)
            const bool want[3] = {false, b->bust && bone_tweaks::money() > kOn, b->seat && cash > kOn};
            const float grow[3] = {0.0f, 0.0f, want[2] ? cashGrowthScale(cash) : 0.0f};
            for (int region = 1; region <= 2; ++region)
            {
                const int s = b->sentinel[region];
                if (s < 0)
                    continue;
                float at[3];
                wantedPosition(b->changes[static_cast<size_t>(s)], want[region], grow[region], at);
                if (std::memcmp(p + b->changes[static_cast<size_t>(s)].pos, at, 12) != 0)
                    apply(p, b->changes, region, want[region], grow[region]);
            }
        }

        void onGetMeshPtr(uint8_t *rdram, R5900Context *ctx, PS2Runtime *)
        {
            if (GPR_U32(ctx, 31) != kTransCallSite)
                return;
            const uint32_t model = GPR_U32(ctx, 4);
            if (!inRam(model, 0x2E4u))
                return;
            uint32_t node = rd32(rdram, model + kMeshHead);
            for (int i = 0; i < 64 && inRam(node, 0x418u); ++i)
            {
                const uint32_t info = rd32(rdram, node + kNodeInfo);
                if (inRam(info, 16u))
                {
                    const char *name = reinterpret_cast<const char *>(rdram + ((info + kInfoName) & PS2_RAM_MASK));
                    for (const std::string &n : g_nodeNames)
                        if (std::strncmp(name, n.c_str(), 8) == 0)
                        {
                            check(rdram, rd32(rdram, node + kNodeMdb));
                            break;
                        }
                }
                node = rd32(rdram, node + kNodeNext);
            }
        }

        bool readDonor(const char *path, const char *node, Mesh &out)
        {
            const auto file = disc_files::find(path);
            std::vector<uint8_t> data;
            return file && disc_files::read(*file, data) && loadDonor(data.data(), data.size(), node, out);
        }
    }

    void install(PS2Runtime &runtime)
    {
        if (!runtime.hasFunction(kGetMeshPtr))
            return;
        if (!disc_files::open())
        {
            KM_WARN(Boot, "body detail: the disc image has no file archive; the women keep their models");
            return;
        }
        const auto start = std::chrono::steady_clock::now();
        Mesh bunny, shannon;
        if (!readDonor("el/elf4.dat", "body", bunny) || !readDonor("el/el15.dat", "body_u1", shannon))
        {
            KM_WARN(Boot, "body detail: the donor models (el/elf4.dat, el/el15.dat) could not be read");
            return;
        }
        std::map<uint64_t, std::pair<int, std::vector<uint8_t>>> built; // block hash -> body (-1: failed), its scr
        std::vector<PS2DiscOverlay::Patch> patches;
        int models = 0, other = 0;
        for (const Model &m : kModels)
        {
            const auto file = disc_files::find(m.file);
            std::vector<uint8_t> block;
            if (!file || !disc_files::readRange(*file, m.offset, m.size, block) || block.size() != m.size || fnv(block) != m.hash)
            {
                ++other;
                continue;
            }
            auto it = built.find(m.hash);
            if (it == built.end())
            {
                Built b;
                if (!build(block.data(), block.size(), bunny, shannon, m.bust, m.seat, b))
                {
                    KM_WARN(Boot, "body detail: " << m.file << " @0x" << std::hex << m.offset << std::dec << ": " << b.error);
                    built.emplace(m.hash, std::make_pair(-1, std::vector<uint8_t>()));
                    continue;
                }
                if (!b.error.empty())
                    KM_INFO(Boot, "body detail: " << m.file << " @0x" << std::hex << m.offset << std::dec << ": " << b.error);
                Body body;
                identify(body, b.scr.data() + b.bodyMdb, b.bodyMdbSize);
                body.changes = std::move(b.changes);
                std::memcpy(body.sentinel, b.sentinel, sizeof body.sentinel);
                body.bust = m.bust && b.sentinel[1] >= 0;
                body.seat = m.seat && b.sentinel[2] >= 0; // the detailed seat and Cash's growth on it
                body.jiggle = b.jiggle;
                body.bind = b.bind;
                g_bodies.push_back(std::move(body));
                it = built.emplace(m.hash, std::make_pair(static_cast<int>(g_bodies.size()) - 1, std::move(b.scr))).first;
            }
            if (it->second.first < 0)
                continue;
            patches.emplace_back(file->imageOffset + m.offset, it->second.second);
            ++models;
        }
        // Shannon and Olivia keep their models; Cash grows their own seats (and what hangs over them).
        int grown = 0;
        for (const GrowModel &g : kGrowModels)
        {
            const auto file = disc_files::find(g.file);
            std::vector<uint8_t> block;
            std::vector<GrownNode> nodes;
            if (!file || !disc_files::readRange(*file, g.offset, g.size, block) || block.size() != g.size || fnv(block) != g.hash ||
                !growOnly(block.data(), block.size(), g.rules, nodes))
            {
                ++other;
                continue;
            }
            // The jiggle bones on her own nodes: only when every node to re-skin is one the runtime knows (a grown
            // node), since a re-skinned node drawn without its bones placed would fall apart.
            const int named = (g.reskin[0] ? 1 : 0) + (g.reskin[1] ? 1 : 0);
            const auto isReskinned = [&g](const char *name) {
                for (const char *r : g.reskin)
                    if (r && std::strncmp(name, r, 8) == 0)
                        return true;
                return false;
            };
            int known = 0;
            for (const GrownNode &n : nodes)
                known += isReskinned(n.name) ? 1 : 0;
            int jiggle = 0;
            if (g.jiggle && known == named && (jiggle = reskin(block.data(), block.size(), g.reskin, named, g.jiggle)) != 0)
                patches.emplace_back(file->imageOffset + g.offset, block);
            else if (g.jiggle)
                KM_WARN(Boot, "body detail: " << g.file << ": no jiggle bones (" << known << " of " << named << " nodes known)");
            for (GrownNode &n : nodes)
            {
                Body body;
                identify(body, block.data() + n.mdb, n.mdbSize);
                body.changes = std::move(n.changes);
                body.sentinel[2] = n.sentinel;
                body.seat = true;
                if (jiggle && isReskinned(n.name) && jiggleSpare(block.data(), block.size(), n.mdb, &body.bind))
                    body.jiggle = jiggle;
                g_bodies.push_back(std::move(body));
                if (std::find(g_nodeNames.begin(), g_nodeNames.end(), std::string(n.name)) == g_nodeNames.end())
                    g_nodeNames.emplace_back(n.name);
            }
            ++grown;
        }
        if (patches.empty() && grown == 0)
        {
            KM_WARN(Boot, "body detail: no model matched (" << other << " listed blocks differ on this disc)");
            return;
        }
        if (!patches.empty())
            PS2DiscOverlay::add(std::move(patches));
        FunctionHooks::instance().observeEntry(kGetMeshPtr, "body_detail", &onGetMeshPtr);
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        KM_INFO(Boot, "body detail: " << models << " women's models rebuilt, " << grown << " grown as they are (" << g_bodies.size() << " bodies, " << ms << " ms"
                                     << (other ? ", " + std::to_string(other) + " listed blocks differ" : std::string()) << ")");
    }

    Jiggle jiggleOf(const uint8_t *rdram, uint32_t model)
    {
        Jiggle out;
        if (!inRam(model, 0x2E4u))
            return out;
        uint32_t node = rd32(rdram, model + kMeshHead);
        for (int i = 0; i < 64 && inRam(node, 0x418u); ++i)
        {
            const uint32_t info = rd32(rdram, node + kNodeInfo);
            if (inRam(info, 16u))
            {
                const char *name = reinterpret_cast<const char *>(rdram + ((info + kInfoName) & PS2_RAM_MASK));
                for (const std::string &n : g_nodeNames)
                    if (std::strncmp(name, n.c_str(), 8) == 0)
                    {
                        if (const Body *b = bodyOf(rdram, rd32(rdram, node + kNodeMdb)); b && b->jiggle)
                        {
                            out.flags |= b->jiggle;
                            std::memcpy(out.pelvis, b->bind.pelvis, sizeof out.pelvis);
                            std::memcpy(out.bust, b->bind.bust, sizeof out.bust);
                        }
                        break;
                    }
            }
            node = rd32(rdram, node + kNodeNext);
        }
        return out;
    }

    bool bustDetailed(const uint8_t *rdram, uint32_t model)
    {
        const uint32_t mdb = bodyMdb(rdram, model & PS2_RAM_MASK);
        const Body *b = mdb ? bodyOf(rdram, mdb) : nullptr;
        if (!b || !b->bust || b->sentinel[1] < 0)
            return false;
        const Change &c = b->changes[static_cast<size_t>(b->sentinel[1])];
        return std::memcmp(rdram + (mdb & PS2_RAM_MASK) + c.pos, c.detail, 12) == 0;
    }
}
