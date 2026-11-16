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
        };
        constexpr GrowRules kOliviaSkirt{"skirt00", "dress", "z_blt00"}, kOliviaSkirt2{"skirt00", "dress2", "z_blt00"};
        constexpr GrowModel kGrowModels[] = {
            {"em/em52.dat", 0x31580u, 0x47640u, 0x97A8C864A29AC1F0ULL, {}},           // Shannon
            {"em/ema0.dat", 0x28060u, 0x47700u, 0xF37EDAD64FF2AE63ULL, kOliviaSkirt},  // Olivia
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

        // ---- Shannon's hair kept behind her seat (K, Nov 16) ------------------------------------------------------------
        // Her two hair chains (cloth kind 0x252 / 0x2A6: bones 33-38, 0.65 m down her back) only collide with her head,
        // neck, back and shoulders (the game's 6 colliders), so in her idle they fell through her seat, at 100% too. After
        // the game's own node collision (func_001067B8 / func_00106BF8, a0 = the node's position), each non-anchor node
        // is moved back (in her pelvis' frame) until the segment from its parent node passes behind her seat: the
        // rearmost surface on a grid (SeatTable, from her body_u1 + BY_00 at rest), grown with Cash, less kSeatMargin,
        // over the hair's half width either side. Position and previous position move together (no added velocity).
        constexpr uint32_t kClothStep = 0x104010u, kCollideA = 0x1067B8u, kCollideB = 0x106BF8u;
        constexpr float kSeatMargin = 0.02f, kHairHalfWidth = 0.05f, kPelvisY = 1.125f;
        SeatTable g_seat;
        uint32_t g_clothOwner = 0, g_clothWork = 0, g_clothNodes = 0; // the cloth step under way (game thread)
        GuestFunction g_origCollideA = nullptr, g_origCollideB = nullptr;

        void onClothStep(uint8_t *, R5900Context *ctx, PS2Runtime *)
        {
            g_clothOwner = GPR_U32(ctx, 4);
            g_clothWork = GPR_U32(ctx, 5);
            g_clothNodes = GPR_U32(ctx, 6);
        }

        float rdf(const uint8_t *rdram, uint32_t a)
        {
            float v;
            std::memcpy(&v, rdram + (a & (PS2_RAM_SIZE - 4u)), 4);
            return v;
        }
        void wrf(uint8_t *rdram, uint32_t a, float v) { std::memcpy(rdram + (a & (PS2_RAM_SIZE - 4u)), &v, 4); }

        // The seat's rear surface (z, bone 17's frame) under a hair segment's point: the deepest cell within the hair's
        // half width at the nearest row; kNone when nothing is there.
        float seatRear(float x, float y, float grow)
        {
            const int iy = static_cast<int>(std::lround((y - SeatTable::kY0) / SeatTable::kStep));
            if (iy < 0 || iy >= SeatTable::kNy)
                return SeatTable::kNone;
            const int x0 = std::max(0, static_cast<int>(std::floor((x - kHairHalfWidth - SeatTable::kX0) / SeatTable::kStep)));
            const int x1 = std::min(SeatTable::kNx - 1, static_cast<int>(std::ceil((x + kHairHalfWidth - SeatTable::kX0) / SeatTable::kStep)));
            float rear = SeatTable::kNone;
            for (int ix = x0; ix <= x1; ++ix)
                if (g_seat.z0[iy][ix] < SeatTable::kNone)
                    rear = std::min(rear, g_seat.z0[iy][ix] + grow * g_seat.gz[iy][ix]);
            return rear;
        }

        uint32_t pelvisPart(const uint8_t *rdram, uint32_t owner) // part 17 (its world matrix at +0x80)
        {
            if (rdram[(owner + 0x2B4u) & PS2_RAM_MASK] <= 17u)
                return 0u;
            const uint32_t parts = rd32(rdram, owner + 0x278u);
            if (!inRam(parts, 0x48u))
                return 0u;
            const uint32_t part = rd32(rdram, parts + 17u * 4u);
            return inRam(part, 0xC0u) ? part : 0u;
        }

        // Keeps one hair node's segment behind the seat; true if it moved the node.
        bool hairBehindSeat(uint8_t *rdram, uint32_t pos)
        {
            const uint32_t owner = g_clothOwner, work = g_clothWork, nodes = g_clothNodes;
            if (!g_seat.ok || !inRam(owner, 0x300u) || !inRam(work, 0x60u) || !inRam(nodes, 0xA0u))
                return false;
            uint16_t kind;
            std::memcpy(&kind, rdram + ((owner + 0x2FEu) & PS2_RAM_MASK), 2);
            if (kind != 0x252u && kind != 0x2A6u)
                return false;
            const uint32_t count = rd32(rdram, work), list = rd32(rdram, work + 4u), parents = rd32(rdram, work + 0x18u);
            if (pos < nodes + 0x40u || (pos - nodes - 0x40u) % 0xA0u != 0u || count > 32u || !inRam(list, 32u) || !inRam(parents, 32u))
                return false;
            const uint32_t idx = (pos - nodes - 0x40u) / 0xA0u;
            if (idx >= count)
                return false;
            const uint8_t parentBone = rdram[(parents + idx) & PS2_RAM_MASK];
            uint32_t pidx = count;
            for (uint32_t i = 0; i < count; ++i)
                if (rdram[(list + i) & PS2_RAM_MASK] == parentBone)
                    pidx = i;
            const uint32_t part = pelvisPart(rdram, owner);
            if (parentBone == 0xFFu || pidx >= count || part == 0u)
                return false;
            float A[3][3], t[3];
            for (int r = 0; r < 3; ++r)
            {
                for (int c = 0; c < 3; ++c)
                    A[r][c] = rdf(rdram, part + 0x80u + 16u * r + 4u * c);
                t[r] = rdf(rdram, part + 0xB0u + 4u * r);
            }
            // world = t + l.x A[0] + l.y A[1] + l.z A[2]: l = (w - t) A^-1
            const float det = A[0][0] * (A[1][1] * A[2][2] - A[1][2] * A[2][1]) - A[0][1] * (A[1][0] * A[2][2] - A[1][2] * A[2][0]) +
                              A[0][2] * (A[1][0] * A[2][1] - A[1][1] * A[2][0]);
            if (std::fabs(det) < 1e-6f)
                return false;
            float I[3][3];
            I[0][0] = (A[1][1] * A[2][2] - A[1][2] * A[2][1]) / det, I[0][1] = (A[0][2] * A[2][1] - A[0][1] * A[2][2]) / det;
            I[0][2] = (A[0][1] * A[1][2] - A[0][2] * A[1][1]) / det, I[1][0] = (A[1][2] * A[2][0] - A[1][0] * A[2][2]) / det;
            I[1][1] = (A[0][0] * A[2][2] - A[0][2] * A[2][0]) / det, I[1][2] = (A[0][2] * A[1][0] - A[0][0] * A[1][2]) / det;
            I[2][0] = (A[1][0] * A[2][1] - A[1][1] * A[2][0]) / det, I[2][1] = (A[0][1] * A[2][0] - A[0][0] * A[2][1]) / det;
            I[2][2] = (A[0][0] * A[1][1] - A[0][1] * A[1][0]) / det;
            const auto local = [&](uint32_t at, float out[3]) {
                const float d[3] = {rdf(rdram, at) - t[0], rdf(rdram, at + 4u) - t[1], rdf(rdram, at + 8u) - t[2]};
                for (int c = 0; c < 3; ++c)
                    out[c] = d[0] * I[0][c] + d[1] * I[1][c] + d[2] * I[2][c];
            };
            float lp[3], ln[3];
            local(nodes + pidx * 0xA0u + 0x40u, lp);
            local(pos, ln);
            const float grow = bone_tweaks::cash() > kOn ? cashGrowthScale(bone_tweaks::cash()) : 0.0f;
            float need = 0.0f;
            for (int s = 3; s <= 10; ++s) // (near the parent the child can't do much: the parent has its own turn)
            {
                const float f = s / 10.0f;
                const float q[3] = {lp[0] + (ln[0] - lp[0]) * f, lp[1] + (ln[1] - lp[1]) * f, lp[2] + (ln[2] - lp[2]) * f};
                if (q[2] >= 0.0f) // in front of the pelvis' middle: not the seat's business
                    continue;
                const float rear = seatRear(q[0], q[1], grow);
                if (rear < SeatTable::kNone)
                    need = std::max(need, (q[2] - (rear - kSeatMargin)) / f);
            }
            need = std::min(need, 0.15f);
            if (need <= 0.0f)
                return false;
            for (int c = 0; c < 3; ++c) // back along the pelvis' z axis, the previous position too
            {
                const float d = -need * A[2][c];
                wrf(rdram, pos + 4u * c, rdf(rdram, pos + 4u * c) + d);
                wrf(rdram, pos + 0x10u + 4u * c, rdf(rdram, pos + 0x10u + 4u * c) + d);
            }
            return true;
        }

        template <int Which>
        void clothCollide(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t pos = GPR_U32(ctx, 4), ra = GPR_U32(ctx, 31);
            (Which == 0 ? g_origCollideA : g_origCollideB)(rdram, ctx, runtime);
            if (ctx->pc != ra) // unwound
                return;
            if (hairBehindSeat(rdram, pos))
                SET_GPR_U32(ctx, 2, 1u);
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
            if (std::strcmp(g.file, "em/em52.dat") == 0) // Shannon: her hair kept behind her seat
            {
                static const char *const kSeatNodes[] = {"body_u1", "BY_00"};
                seatTable(block.data(), block.size(), kSeatNodes, 2, kPelvisY, g_seat);
            }
            for (GrownNode &n : nodes)
            {
                Body body;
                identify(body, block.data() + n.mdb, n.mdbSize);
                body.changes = std::move(n.changes);
                body.sentinel[2] = n.sentinel;
                body.seat = true;
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
        if (g_seat.ok)
        {
            FunctionHooks::instance().observeEntry(kClothStep, "body_detail (cloth step)", &onClothStep);
            const bool a = runtime.hasFunction(kCollideA) && (g_origCollideA = runtime.lookupFunction(kCollideA)) &&
                           FunctionHooks::instance().replace(kCollideA, "cloth node collision (Shannon's hair behind her seat)",
                                                             &clothCollide<0>, ReplacementStatus::Native, "Shannon's hair behind her seat");
            const bool b = runtime.hasFunction(kCollideB) && (g_origCollideB = runtime.lookupFunction(kCollideB)) &&
                           FunctionHooks::instance().replace(kCollideB, "cloth node collision 2 (Shannon's hair behind her seat)",
                                                             &clothCollide<1>, ReplacementStatus::Native, "Shannon's hair behind her seat");
            if (!a || !b)
                KM_WARN(Boot, "body detail: the cloth collision is not hookable (Shannon's hair)");
        }
        const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - start).count();
        KM_INFO(Boot, "body detail: " << models << " women's models rebuilt, " << grown << " grown as they are (" << g_bodies.size() << " bodies, " << ms << " ms"
                                     << (other ? ", " + std::to_string(other) + " listed blocks differ" : std::string()) << ")");
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
