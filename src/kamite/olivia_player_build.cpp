// Playable Olivia's player files (see olivia_player_build.h). The formats: tools/models/gamemodel.py, mot.py.
#include "kamite/olivia_player_build.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <functional>

namespace kamite::olivia_player
{
    namespace
    {
        using Bytes = std::vector<uint8_t>;

        uint32_t r32(const Bytes &b, size_t o)
        {
            uint32_t v = 0;
            std::memcpy(&v, b.data() + o, 4);
            return v;
        }
        uint16_t r16(const Bytes &b, size_t o)
        {
            uint16_t v = 0;
            std::memcpy(&v, b.data() + o, 2);
            return v;
        }
        float rf(const Bytes &b, size_t o)
        {
            float v = 0.0f;
            std::memcpy(&v, b.data() + o, 4);
            return v;
        }
        void put32(Bytes &b, size_t o, uint32_t v) { std::memcpy(b.data() + o, &v, 4); }
        void put16(Bytes &b, size_t o, uint16_t v) { std::memcpy(b.data() + o, &v, 2); }
        void putf(Bytes &b, size_t o, float v) { std::memcpy(b.data() + o, &v, 4); }
        void pad(Bytes &b, size_t align)
        {
            while (b.size() % align)
                b.push_back(0);
        }
        bool inside(const Bytes &b, size_t at, size_t bytes) { return at <= b.size() && bytes <= b.size() - at; }

        // ---- dat: u32 count, u32 offsets[count] (0 = none), char type[4][count]; entries 32-byte aligned ------------
        struct Entry
        {
            char type[4] = {};
            Bytes bytes;
            bool present = false;
        };
        bool readDat(const Bytes &dat, std::vector<Entry> &out)
        {
            if (dat.size() < 4)
                return false;
            const uint32_t n = r32(dat, 0);
            if (n == 0 || n > 4096 || !inside(dat, 4, 8u * n))
                return false;
            std::vector<uint32_t> starts;
            for (uint32_t i = 0; i < n; ++i)
                if (const uint32_t o = r32(dat, 4 + 4 * i); o)
                    starts.push_back(o);
            std::sort(starts.begin(), starts.end());
            out.assign(n, Entry{});
            for (uint32_t i = 0; i < n; ++i)
            {
                Entry &e = out[i];
                std::memcpy(e.type, dat.data() + 4 + 4 * n + 4 * i, 4);
                const uint32_t at = r32(dat, 4 + 4 * i);
                if (at == 0)
                    continue;
                if (at >= dat.size())
                    return false;
                const auto next = std::upper_bound(starts.begin(), starts.end(), at);
                const uint32_t end = next == starts.end() ? static_cast<uint32_t>(dat.size()) : *next;
                e.bytes.assign(dat.begin() + at, dat.begin() + end);
                e.present = true;
            }
            return true;
        }
        Bytes writeDat(const std::vector<Entry> &entries)
        {
            const uint32_t n = static_cast<uint32_t>(entries.size());
            Bytes o(4 + 8u * n, 0);
            put32(o, 0, n);
            for (uint32_t i = 0; i < n; ++i)
                std::memcpy(o.data() + 4 + 4 * n + 4 * i, entries[i].type, 4);
            for (uint32_t i = 0; i < n; ++i)
            {
                if (!entries[i].present)
                    continue;
                pad(o, 0x20);
                put32(o, 4 + 4 * i, static_cast<uint32_t>(o.size()));
                o.insert(o.end(), entries[i].bytes.begin(), entries[i].bytes.end());
            }
            pad(o, 0x20);
            return o;
        }
        bool entryIs(const Entry &e, const char *type) { return e.present && std::strncmp(e.type, type, 4) == 0; }

        // ---- scr: "scr\0", kind, node count, 0, node offsets; the mdbs; 112-byte node records (+0 mdb, +4 the anchor,
        // +8 name, +16 srt, +52 the rest). The nodes' mdbs are copied as they are (every offset in one is its own).
        struct Node
        {
            std::string name;
            uint8_t rec[112] = {};
            Bytes mdb;
        };
        struct Scr
        {
            uint32_t kind = 0, anchor = 0;
            std::vector<Node> nodes;
        };
        bool readScr(const Bytes &s, Scr &out)
        {
            if (!inside(s, 0, 16) || std::memcmp(s.data(), "scr", 3) != 0)
                return false;
            out.kind = r32(s, 4);
            const uint32_t n = r32(s, 8);
            if (n == 0 || n > 256 || !inside(s, 16, 4u * n))
                return false;
            std::vector<uint32_t> mdbs, recs;
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t no = r32(s, 16 + 4 * i);
                if (!inside(s, no, 112))
                    return false;
                int32_t rel;
                std::memcpy(&rel, s.data() + no, 4);
                const int64_t m = static_cast<int64_t>(no) + rel;
                if (m < 16 || m >= static_cast<int64_t>(s.size()) || std::memcmp(s.data() + m, "mdb", 3) != 0)
                    return false;
                mdbs.push_back(static_cast<uint32_t>(m));
                recs.push_back(no);
            }
            std::vector<uint32_t> bounds = mdbs;
            bounds.insert(bounds.end(), recs.begin(), recs.end());
            bounds.push_back(static_cast<uint32_t>(s.size()));
            std::sort(bounds.begin(), bounds.end());
            int32_t anchorRel;
            std::memcpy(&anchorRel, s.data() + recs[0] + 4, 4);
            out.anchor = static_cast<uint32_t>(static_cast<int64_t>(recs[0]) + anchorRel);
            for (uint32_t i = 0; i < n; ++i)
            {
                Node node;
                std::memcpy(node.rec, s.data() + recs[i], 112);
                const char *name = reinterpret_cast<const char *>(node.rec + 8);
                node.name.assign(name, strnlen(name, 8));
                const uint32_t end = *std::upper_bound(bounds.begin(), bounds.end(), mdbs[i]);
                node.mdb.assign(s.begin() + mdbs[i], s.begin() + end);
                out.nodes.push_back(std::move(node));
            }
            return true;
        }
        Bytes writeScr(const Scr &scr)
        {
            const uint32_t n = static_cast<uint32_t>(scr.nodes.size());
            Bytes o{'s', 'c', 'r', 0};
            o.resize(16, 0);
            put32(o, 4, scr.kind);
            put32(o, 8, n);
            o.resize(16 + 4u * n, 0);
            pad(o, 16);
            std::vector<uint32_t> mdbs;
            for (const Node &node : scr.nodes)
            {
                mdbs.push_back(static_cast<uint32_t>(o.size()));
                o.insert(o.end(), node.mdb.begin(), node.mdb.end());
                pad(o, 16);
            }
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t no = static_cast<uint32_t>(o.size());
                put32(o, 16 + 4 * i, no);
                o.insert(o.end(), scr.nodes[i].rec, scr.nodes[i].rec + 112);
                put32(o, no, mdbs[i] - no);
                put32(o, no + 4, scr.anchor - no);
                std::memset(o.data() + no + 8, 0, 8);
                std::memcpy(o.data() + no + 8, scr.nodes[i].name.c_str(), std::min<size_t>(8, scr.nodes[i].name.size()));
            }
            return o;
        }
        const Node *find(const Scr &s, const std::string &name)
        {
            for (const Node &n : s.nodes)
                if (n.name == name)
                    return &n;
            return nullptr;
        }

        // ---- an mdb: "mdb\0", +4 bone table (rel), +8 u16 bones, +10 u16 buffers, +32 buffer offsets (rel); a buffer:
        // +0 positions (16 bytes each: xyz, strip flag), +4 normals, +8 UVs, +12 colours, +16 weights (8 bytes: 0, bone
        // x 4 three times, percent three times, 0), +20 u16 count, +22 u16 material.
        bool mdbOk(const Bytes &m)
        {
            if (!inside(m, 0, 32) || std::memcmp(m.data(), "mdb", 3) != 0)
                return false;
            const uint16_t nv = r16(m, 10);
            if (!inside(m, 32, 4u * nv))
                return false;
            for (uint16_t i = 0; i < nv; ++i)
            {
                const uint32_t v = r32(m, 32 + 4 * i);
                if (!inside(m, v, 32))
                    return false;
                const uint32_t n = r16(m, v + 20), po = r32(m, v), wo = r32(m, v + 16);
                if (!inside(m, v + po, 16u * n) || (wo && !inside(m, v + wo, 8u * n)))
                    return false;
            }
            return inside(m, r32(m, 4), 16u * r16(m, 8));
        }
        // f(the buffer's offset in b) for each buffer of the mdb at b[base].
        void forEachBuffer(Bytes &b, uint32_t base, const std::function<void(uint32_t)> &f)
        {
            const uint16_t nv = r16(b, base + 10);
            for (uint16_t i = 0; i < nv; ++i)
                f(base + r32(b, base + 32 + 4 * i));
        }
        void forEachBuffer(Bytes &m, const std::function<void(uint32_t)> &f) { forEachBuffer(m, 0, f); }
        void shiftMaterials(Bytes &m, int by)
        {
            forEachBuffer(m, [&](uint32_t v) { put16(m, v + 22, static_cast<uint16_t>(r16(m, v + 22) + by)); });
        }
        // Every vertex at the model's origin on bone 0 alone: whatever the bones do, each triangle is a point.
        void collapse(Bytes &m, uint32_t base = 0)
        {
            forEachBuffer(m, base, [&](uint32_t v) {
                const uint32_t n = r16(m, v + 20), po = r32(m, v), wo = r32(m, v + 16);
                for (uint32_t k = 0; k < n; ++k)
                {
                    std::memset(m.data() + v + po + 16 * k, 0, 12);
                    if (wo)
                    {
                        uint8_t *w = m.data() + v + wo + 8 * k;
                        std::memset(w, 0, 8);
                        w[4] = 100;
                    }
                }
            });
        }
        float maxY(const Bytes &m)
        {
            float top = -1e9f;
            Bytes copy = m;
            forEachBuffer(copy, [&](uint32_t v) {
                const uint32_t n = r16(copy, v + 20), po = r32(copy, v);
                for (uint32_t k = 0; k < n; ++k)
                    top = std::max(top, rf(copy, v + po + 16 * k + 4));
            });
            return top;
        }
        // The bind position of a bone (its offsets summed up the chain), from an mdb's bone table.
        bool bindOf(const Bytes &m, int bone, float out[3])
        {
            const uint32_t table = r32(m, 4), nb = r16(m, 8);
            out[0] = out[1] = out[2] = 0.0f;
            for (int i = bone, guard = 0; i >= 0 && guard < 64; ++guard)
            {
                if (static_cast<uint32_t>(i) >= nb)
                    return false;
                for (int a = 0; a < 3; ++a)
                    out[a] += rf(m, table + 16u * static_cast<uint32_t>(i) + 4u * static_cast<uint32_t>(a));
                int16_t parent;
                std::memcpy(&parent, m.data() + table + 16u * static_cast<uint32_t>(i) + 14, 2);
                i = parent;
            }
            return true;
        }

        // ---- TM3: "TM3", count, 4, 0, offsets, names (8 bytes), TIM3s 64-aligned --------------------------------------
        using Textures = std::vector<std::pair<std::string, Bytes>>;
        bool readTm3(const Bytes &t, Textures &out)
        {
            if (!inside(t, 0, 16) || std::memcmp(t.data(), "TM3", 3) != 0)
                return false;
            const uint32_t n = r32(t, 4);
            const size_t names = 16 + 4u * n + ((n & 1) ? 4 : 0);
            if (n == 0 || n > 64 || !inside(t, names, 8u * n))
                return false;
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t at = r32(t, 16 + 4 * i);
                uint32_t end = static_cast<uint32_t>(t.size());
                for (uint32_t k = 0; k < n; ++k)
                    if (const uint32_t o = r32(t, 16 + 4 * k); o > at && o < end)
                        end = o;
                if (at >= end)
                    return false;
                const char *name = reinterpret_cast<const char *>(t.data() + names + 8 * i);
                out.emplace_back(std::string(name, strnlen(name, 8)), Bytes(t.begin() + at, t.begin() + end));
            }
            return true;
        }
        Bytes writeTm3(const Textures &texs)
        {
            const uint32_t n = static_cast<uint32_t>(texs.size());
            Bytes o{'T', 'M', '3', 0};
            o.resize(16, 0);
            put32(o, 4, n);
            put32(o, 8, 4);
            o.resize(16 + 4u * n + ((n & 1) ? 4 : 0), 0);
            for (const auto &[name, tim] : texs)
            {
                const size_t at = o.size();
                o.resize(at + 8, 0);
                std::memcpy(o.data() + at, name.c_str(), std::min<size_t>(8, name.size()));
            }
            for (uint32_t i = 0; i < n; ++i)
            {
                pad(o, 64);
                put32(o, 16 + 4 * i, static_cast<uint32_t>(o.size()));
                o.insert(o.end(), texs[i].second.begin(), texs[i].second.end());
            }
            pad(o, 64);
            return o;
        }

        // ---- skin: a TIM3's palette (+16: picture header; total, CLUT bytes, image bytes, header size; the CLUT after
        // the image, RGBA, alpha 0x80 = opaque). Skin-range colours (hue 8..50 deg or 345+, saturation 0.07..0.45,
        // value 0.28+: the range the previews used, olivia_skin_variants.png) turned toward cutscene Olivia's
        // (measured Nov 19, median HSV of the arms: hue -9.6 deg, saturation x1.36, value x1.16) by `t`.
        int warmSkin(Bytes &tim, float t)
        {
            constexpr size_t p = 16;
            if (!inside(tim, p, 32))
                return 0;
            const uint32_t clut = r32(tim, p + 4), img = r32(tim, p + 8);
            const uint16_t hs = r16(tim, p + 12);
            const size_t at = p + hs + img;
            if (clut == 0 || !inside(tim, at, clut))
                return 0;
            int changed = 0;
            for (size_t e = at; e + 4 <= at + clut; e += 4)
            {
                if (tim[e + 3] == 0)
                    continue;
                const float r = tim[e] / 255.0f, g = tim[e + 1] / 255.0f, b = tim[e + 2] / 255.0f;
                const float mx = std::max({r, g, b}), mn = std::min({r, g, b}), d = mx - mn;
                if (mx <= 0.0f || d <= 0.0f)
                    continue;
                float h = mx == r ? (g - b) / d : mx == g ? 2.0f + (b - r) / d : 4.0f + (r - g) / d;
                h = std::fmod(h / 6.0f + 1.0f, 1.0f);
                float s = d / mx, v = mx;
                const float deg = h * 360.0f;
                if (!((deg >= 8.0f && deg <= 50.0f) || deg >= 345.0f) || s < 0.07f || s > 0.45f || v < 0.28f)
                    continue;
                h = std::fmod(h - 0.0267f * t + 1.0f, 1.0f);
                s = std::min(1.0f, s * (1.0f + 0.36f * t));
                v = std::min(1.0f, v * (1.0f + 0.16f * t));
                const float hh = h * 6.0f, f = hh - std::floor(hh), pp = v * (1 - s), q = v * (1 - s * f), u = v * (1 - s * (1 - f));
                float o[3];
                switch (static_cast<int>(hh) % 6)
                {
                case 0: o[0] = v, o[1] = u, o[2] = pp; break;
                case 1: o[0] = q, o[1] = v, o[2] = pp; break;
                case 2: o[0] = pp, o[1] = v, o[2] = u; break;
                case 3: o[0] = pp, o[1] = q, o[2] = v; break;
                case 4: o[0] = u, o[1] = pp, o[2] = v; break;
                default: o[0] = v, o[1] = pp, o[2] = q; break;
                }
                for (int c = 0; c < 3; ++c)
                    tim[e + static_cast<size_t>(c)] = static_cast<uint8_t>(std::lround(std::clamp(o[c], 0.0f, 1.0f) * 255.0f));
                ++changed;
            }
            return changed;
        }

        // ---- motions: "mtb3", u16 frames, u8 records, u8 loop; 12-byte records (u8 bone, u8 channel, u16 keys, u32
        // flags, u32 payload); channel: 0x00 constant (payload an f32), 0x10 Hermite (payload: the curve block's offset
        // from the motion's start; six half floats: value, value step, slope in, its step, slope out, its step, then the
        // keys' u8 steps). value = a * value + c: the curve's values move with it exactly (up to the halves' rounding).
        float halfToFloat(uint16_t v)
        {
            const int expo = (v >> 9) & 0x3F, mant = v & 0x1FF;
            if (expo == 0 && mant == 0)
                return 0.0f;
            const float value = std::ldexp(1.0f + mant / 512.0f, expo - 47);
            return (v & 0x8000u) ? -value : value;
        }
        uint16_t floatToHalf(float x)
        {
            if (x == 0.0f || !std::isfinite(x))
                return 0;
            const uint16_t sign = x < 0.0f ? 0x8000u : 0u;
            const float a = std::fabs(x);
            int e = static_cast<int>(std::floor(std::log2(a)));
            int m = static_cast<int>(std::lround((a / std::ldexp(1.0f, e) - 1.0f) * 512.0f));
            if (m >= 512)
                m = 0, ++e;
            if (e + 47 < 1)
                return 0;
            const int expo = std::min(e + 47, 62);
            return static_cast<uint16_t>(sign | (expo << 9) | (m & 0x1FF));
        }
        struct Affine
        {
            float a = 1.0f, c = 0.0f;
        };
        // affine[bone 0, 20, 24, 255][component 0..2]
        int editMotion(Bytes &m, const std::function<const Affine *(int bone, int component)> &edit)
        {
            if (!inside(m, 0, 8) || std::memcmp(m.data(), "mtb3", 4) != 0)
                return 0;
            const int records = m[6];
            int changed = 0;
            for (int i = 0; i < records && inside(m, 8 + 12u * static_cast<uint32_t>(i), 12); ++i)
            {
                const size_t r = 8 + 12u * static_cast<uint32_t>(i);
                const uint8_t bone = m[r], channel = m[r + 1];
                if (channel == 0xFF)
                    break;
                const int component = channel & 0x0F;
                if (component > 2)
                    continue;
                const Affine *f = edit(bone, component);
                if (!f)
                    continue;
                const uint32_t payload = r32(m, r + 8);
                if ((channel & 0xF0) == 0x00)
                {
                    float v;
                    std::memcpy(&v, m.data() + r + 8, 4);
                    putf(m, r + 8, f->a * v + f->c);
                    ++changed;
                }
                else if ((channel & 0xF0) == 0x10 && inside(m, payload, 12))
                {
                    uint16_t h[6];
                    std::memcpy(h, m.data() + payload, 12);
                    h[0] = floatToHalf(f->a * halfToFloat(h[0]) + f->c);
                    for (int k = 1; k < 6; ++k)
                        h[k] = floatToHalf(f->a * halfToFloat(h[k]));
                    std::memcpy(m.data() + payload, h, 12);
                    ++changed;
                }
            }
            return changed;
        }

        bool fail(Built &out, const char *why)
        {
            out.error = why;
            return false;
        }
    }

    bool build(const Inputs &in, Built &out)
    {
        std::vector<Entry> pl, ema0, elf4;
        if (!readDat(in.pl00, pl) || pl.size() < 3 || !entryIs(pl[0], "TM3") || !entryIs(pl[1], "MD") || !entryIs(pl[2], "TXE"))
            return fail(out, "pl/pl00.dat is not the expected dat (TM3, MD, TXE, motions)");
        if (!readDat(in.ema0, ema0) || ema0.size() < 2 || !entryIs(ema0[0], "TM3") || !entryIs(ema0[1], "MD"))
            return fail(out, "em/ema0.dat is not the expected dat (TM3, MD)");
        const bool bunny = in.costume == Costume::Bunny;
        if (bunny && (!readDat(in.elf4, elf4) || elf4.size() < 2 || !entryIs(elf4[0], "TM3") || !entryIs(elf4[1], "MD")))
            return fail(out, "el/elf4.dat is not the expected dat (TM3, MD)");

        Scr gene, her, suit;
        if (!readScr(pl[1].bytes, gene) || !readScr(in.ema0Md.empty() ? ema0[1].bytes : in.ema0Md, her) ||
            (bunny && !readScr(in.elf4Md.empty() ? elf4[1].bytes : in.elf4Md, suit)))
            return fail(out, "a model could not be read");
        for (const Scr *s : {&gene, &her, &suit})
            for (const Node &n : s->nodes)
                if (!mdbOk(n.mdb))
                    return fail(out, "a model's mesh could not be read");
        const Node *geneHead = find(gene, "HD_00"), *herHead = find(her, "HD_00");
        if (!geneHead || !herHead || !find(her, "RH_00") || !find(her, "RH_01") || !find(her, "LH_00") || !find(her, "LH_01") ||
            !find(her, "z_00") || r16(herHead->mdb, 8) != 30 || r16(geneHead->mdb, 8) < 29)
            return fail(out, "the models are not the ones this was made for");
        if (bunny && (!find(suit, "body") || r16(find(suit, "body")->mdb, 8) != 30))
            return fail(out, "the bunny suit is not the one this was made for");

        // Textures: hers (the skin warmed); the bunny suit's own sheets first, hers after them.
        Textures herTex, suitTex, texs;
        if (!readTm3(ema0[0].bytes, herTex) || (bunny && !readTm3(elf4[0].bytes, suitTex)))
            return fail(out, "a texture pack could not be read");
        if (in.skin > 0.0f)
            for (auto &[name, tim] : herTex)
                if (name == "ema0_01" || name == "ema0_03" || name == "ema0_06")
                    out.skinColours += warmSkin(tim, std::clamp(in.skin, 0.0f, 1.0f));
        texs = suitTex;
        texs.insert(texs.end(), herTex.begin(), herTex.end());
        const int herBase = static_cast<int>(suitTex.size());

        // The model: her nodes (or the suit's body and her head, hair, lashes, hands), Gene's four hand shapes, and the
        // God Hand's two nodes empty.
        Scr made;
        made.kind = her.kind;
        made.anchor = her.anchor;
        const auto take = [&](const Node &n, const std::string &as, int matBase) {
            Node copy = n;
            copy.name = as;
            if (matBase)
                shiftMaterials(copy.mdb, matBase);
            made.nodes.push_back(std::move(copy));
        };
        if (bunny)
        {
            for (const Node &n : suit.nodes)
                take(n, n.name, 0);
            for (const Node &n : her.nodes)
            {
                const bool head = n.name == "HD_00" || n.name == "y_Eyl00" || (n.name.rfind("z_", 0) == 0 && n.name != "z_blt00");
                const bool hand = n.name.rfind("RH_", 0) == 0 || n.name.rfind("LH_", 0) == 0;
                if (head || hand)
                    take(n, n.name, herBase);
            }
        }
        else
            for (const Node &n : her.nodes)
                take(n, n.name, 0);
        const auto handOf = [&](const char *side, int which) { return find(her, std::string(side) + (which == 1 ? "_01" : "_00")); };
        take(*handOf("RH", in.hand2), "RH_02", herBase);
        take(*handOf("RH", in.hand3), "RH_03", herBase);
        take(*handOf("LH", in.hand2), "LH_02", herBase);
        take(*handOf("LH", in.hand3), "LH_03", herBase);
        for (const char *name : {"GH_00", "GHA_00"})
        {
            take(*find(her, "z_00"), name, herBase);
            collapse(made.nodes.back().mdb);
        }
        out.nodes = static_cast<int>(made.nodes.size());

        // Gene's motions on her skeleton.
        float geneHip[3], herHip[3], geneAnkle[3], herAnkle[3];
        if (!bindOf(geneHead->mdb, 0, geneHip) || !bindOf(herHead->mdb, 0, herHip) || !bindOf(geneHead->mdb, 20, geneAnkle) ||
            !bindOf(herHead->mdb, 20, herAnkle))
            return fail(out, "the skeletons could not be read");
        const float geneTop = maxY(geneHead->mdb), herTop = maxY(herHead->mdb);
        if (!(geneTop > 1.0f) || !(herTop > 1.0f))
            return fail(out, "the heads' heights could not be read");
        out.ratio = herTop / geneTop;
        out.hipDy = herHip[1] - geneHip[1];
        out.ankleDy = herAnkle[1] - geneAnkle[1];
        const Affine flat{out.ratio, 0.0f}, hipY{1.0f, out.hipDy}, ankleY{1.0f, out.ankleDy};
        const auto edit = [&](int bone, int component) -> const Affine * {
            if (bone == 255)
                return &flat;
            if (bone == 0)
                return component == 1 ? &hipY : &flat;
            if (bone == 20 || bone == 24)
                return component == 1 ? &ankleY : &flat;
            return nullptr;
        };
        for (size_t i = 3; i < pl.size(); ++i)
            if (entryIs(pl[i], "MOT"))
                if (const int n = editMotion(pl[i].bytes, edit))
                    ++out.motions, out.channels += n;

        pl[0].bytes = writeTm3(texs);
        pl[1].bytes = writeScr(made);
        out.textureBytes = static_cast<uint32_t>(pl[0].bytes.size());
        out.body = writeDat(pl);
        return true;
    }

    bool hideCoat(const std::vector<uint8_t> &coat, std::vector<uint8_t> &out)
    {
        std::vector<Entry> entries;
        if (!readDat(coat, entries))
            return false;
        int models = 0;
        for (Entry &e : entries)
        {
            if (!entryIs(e, "MD"))
                continue;
            Scr s;
            if (!readScr(e.bytes, s))
                return false;
            for (const Node &n : s.nodes)
                if (!mdbOk(n.mdb))
                    return false;
            // In place: the file keeps its layout.
            const uint32_t n = r32(e.bytes, 8);
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t no = r32(e.bytes, 16 + 4 * i);
                int32_t rel;
                std::memcpy(&rel, e.bytes.data() + no, 4);
                collapse(e.bytes, static_cast<uint32_t>(static_cast<int64_t>(no) + rel));
            }
            ++models;
        }
        if (models == 0)
            return false;
        out = writeDat(entries);
        return true;
    }
}
