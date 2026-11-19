// Gameplay Olivia from her cutscene parts (see olivia_model.h). The formats: tools/models/gamemodel.py.
#include "kamite/olivia_model.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <tuple>
#include <utility>

namespace kamite::olivia_model
{
    namespace
    {
        uint32_t r32(const std::vector<uint8_t> &b, size_t o)
        {
            uint32_t v = 0;
            std::memcpy(&v, b.data() + o, 4);
            return v;
        }
        uint16_t r16(const std::vector<uint8_t> &b, size_t o)
        {
            uint16_t v = 0;
            std::memcpy(&v, b.data() + o, 2);
            return v;
        }
        float rf(const std::vector<uint8_t> &b, size_t o)
        {
            float v = 0.0f;
            std::memcpy(&v, b.data() + o, 4);
            return v;
        }
        void put32(std::vector<uint8_t> &b, size_t o, uint32_t v) { std::memcpy(b.data() + o, &v, 4); }
        void pad(std::vector<uint8_t> &b, size_t align)
        {
            while (b.size() % align)
                b.push_back(0);
        }
        bool inside(const std::vector<uint8_t> &b, size_t at, size_t bytes) { return at <= b.size() && bytes <= b.size() - at; }

        // A dat's entry (type, bytes from its offset to the next entry's).
        bool datEntry(const std::vector<uint8_t> &dat, uint32_t index, const char *type, std::vector<uint8_t> &out)
        {
            if (dat.size() < 4)
                return false;
            const uint32_t n = r32(dat, 0);
            if (index >= n || !inside(dat, 4, 8u * n))
                return false;
            const uint32_t at = r32(dat, 4 + 4 * index);
            if (at == 0 || at >= dat.size() || std::strncmp(reinterpret_cast<const char *>(dat.data() + 4 + 4 * n + 4 * index), type, 4) != 0)
                return false;
            uint32_t end = static_cast<uint32_t>(dat.size());
            for (uint32_t i = 0; i < n; ++i)
                if (const uint32_t o = r32(dat, 4 + 4 * i); o > at && o < end)
                    end = o;
            out.assign(dat.begin() + at, dat.begin() + end);
            return true;
        }

        struct Vb
        {
            uint32_t count = 0;
            uint16_t mat = 0;
            uint8_t extra[8] = {};
            std::vector<float> pos;     // 3 per record
            std::vector<int32_t> flag;  // the strip flag
            std::vector<int8_t> nrm;    // 3 per record (empty: none)
            std::vector<int16_t> uv;    // 2 per record
            std::vector<uint8_t> col;   // 4 per record (empty: none)
            std::vector<uint8_t> wt;    // 8 per record: 0, bone x 4 (three), percent (three), 0 (empty: none)
        };
        struct Node
        {
            std::string name;
            uint8_t hdr[20] = {};
            float srt[9] = {};
            uint8_t tail[60] = {};
            std::vector<Vb> vbs;
        };
        struct Model
        {
            uint32_t kind = 0, anchor = 0;
            std::vector<uint8_t> bones; // 16 per bone, the first node's
            std::vector<Node> nodes;
        };

        bool readMdb(const std::vector<uint8_t> &s, uint32_t m, Node &node, std::vector<uint8_t> *bones)
        {
            if (!inside(s, m, 32) || std::memcmp(s.data() + m, "mdb", 3) != 0)
                return false;
            const uint32_t boneAt = m + r32(s, m + 4);
            const uint16_t nb = r16(s, m + 8), nv = r16(s, m + 10);
            if (!inside(s, m + 32, 4u * nv) || !inside(s, boneAt, 16u * nb))
                return false;
            std::memcpy(node.hdr, s.data() + m + 12, 20);
            if (bones)
                bones->assign(s.begin() + boneAt, s.begin() + boneAt + 16u * nb);
            for (uint32_t i = 0; i < nv; ++i)
            {
                const uint32_t v = m + r32(s, m + 32 + 4 * i);
                if (!inside(s, v, 32))
                    return false;
                const uint32_t po = r32(s, v), no = r32(s, v + 4), uo = r32(s, v + 8), co = r32(s, v + 12), wo = r32(s, v + 16);
                Vb vb;
                vb.count = r16(s, v + 20);
                vb.mat = r16(s, v + 22);
                std::memcpy(vb.extra, s.data() + v + 24, 8);
                const uint32_t n = vb.count;
                if (!inside(s, v + po, 16u * n) || !inside(s, v + uo, 4u * n) || (no && !inside(s, v + no, 4u * n)) ||
                    (co && !inside(s, v + co, 4u * n)) || (wo && !inside(s, v + wo, 8u * n)))
                    return false;
                for (uint32_t k = 0; k < n; ++k)
                {
                    for (int a = 0; a < 3; ++a)
                        vb.pos.push_back(rf(s, v + po + 16 * k + 4 * a));
                    vb.flag.push_back(static_cast<int32_t>(r32(s, v + po + 16 * k + 12)));
                    if (no)
                        for (int a = 0; a < 3; ++a)
                            vb.nrm.push_back(static_cast<int8_t>(s[v + no + 4 * k + a]));
                    for (int a = 0; a < 2; ++a)
                        vb.uv.push_back(static_cast<int16_t>(r16(s, v + uo + 4 * k + 2 * a)));
                    if (co)
                        vb.col.insert(vb.col.end(), s.begin() + v + co + 4 * k, s.begin() + v + co + 4 * k + 4);
                    if (wo)
                        vb.wt.insert(vb.wt.end(), s.begin() + v + wo + 8 * k, s.begin() + v + wo + 8 * k + 8);
                }
                node.vbs.push_back(std::move(vb));
            }
            return true;
        }

        // A scr (gamemodel.scr_read): header, node table, mdbs, then the 112-byte node records.
        bool readModel(const std::vector<uint8_t> &s, Model &out)
        {
            if (!inside(s, 0, 16) || std::memcmp(s.data(), "scr", 3) != 0)
                return false;
            out.kind = r32(s, 4);
            const uint32_t n = r32(s, 8);
            if (n == 0 || n > 256 || !inside(s, 16, 4u * n))
                return false;
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t no = r32(s, 16 + 4 * i);
                if (!inside(s, no, 112))
                    return false;
                Node node;
                const char *name = reinterpret_cast<const char *>(s.data() + no + 8);
                node.name.assign(name, strnlen(name, 8));
                for (int k = 0; k < 9; ++k)
                    node.srt[k] = rf(s, no + 16 + 4 * k);
                std::memcpy(node.tail, s.data() + no + 52, 60);
                if (i == 0)
                    out.anchor = no + r32(s, no + 4);
                int32_t rel;
                std::memcpy(&rel, s.data() + no, 4);
                if (!readMdb(s, static_cast<uint32_t>(static_cast<int64_t>(no) + rel), node, i == 0 ? &out.bones : nullptr))
                    return false;
                out.nodes.push_back(std::move(node));
            }
            return !out.bones.empty();
        }

        void writeVb(std::vector<uint8_t> &o, const Vb &vb)
        {
            const size_t v = o.size(), n = vb.count;
            o.resize(v + 32, 0);
            const auto rel = [&] { return static_cast<uint32_t>(o.size() - v); };
            const uint32_t po = rel();
            for (size_t k = 0; k < n; ++k)
            {
                const size_t at = o.size();
                o.resize(at + 16);
                std::memcpy(o.data() + at, &vb.pos[3 * k], 12);
                std::memcpy(o.data() + at + 12, &vb.flag[k], 4);
            }
            pad(o, 16);
            uint32_t no = 0, co = 0, wo = 0;
            if (!vb.nrm.empty())
            {
                no = rel();
                for (size_t k = 0; k < n; ++k)
                    o.insert(o.end(), {static_cast<uint8_t>(vb.nrm[3 * k]), static_cast<uint8_t>(vb.nrm[3 * k + 1]),
                                       static_cast<uint8_t>(vb.nrm[3 * k + 2]), 0});
                pad(o, 16);
            }
            const uint32_t uo = rel();
            for (size_t k = 0; k < n; ++k)
            {
                const size_t at = o.size();
                o.resize(at + 4);
                std::memcpy(o.data() + at, &vb.uv[2 * k], 4);
            }
            pad(o, 16);
            if (!vb.col.empty())
            {
                co = rel();
                o.insert(o.end(), vb.col.begin(), vb.col.end());
                pad(o, 16);
            }
            if (!vb.wt.empty())
            {
                wo = rel();
                o.insert(o.end(), vb.wt.begin(), vb.wt.end());
                pad(o, 16);
            }
            put32(o, v, po);
            put32(o, v + 4, no);
            put32(o, v + 8, uo);
            put32(o, v + 12, co);
            put32(o, v + 16, wo);
            const uint16_t c16 = static_cast<uint16_t>(n);
            std::memcpy(o.data() + v + 20, &c16, 2);
            std::memcpy(o.data() + v + 22, &vb.mat, 2);
            std::memcpy(o.data() + v + 24, vb.extra, 8);
        }

        // gamemodel.mdb_write: header, the buffer table, the bone table, the buffers (each 16-aligned).
        void writeMdb(std::vector<uint8_t> &o, const Node &node, const std::vector<uint8_t> &bones)
        {
            const size_t m = o.size();
            const uint16_t nb = static_cast<uint16_t>(bones.size() / 16), nv = static_cast<uint16_t>(node.vbs.size());
            o.insert(o.end(), {'m', 'd', 'b', 0, 0, 0, 0, 0});
            o.insert(o.end(), {static_cast<uint8_t>(nb), static_cast<uint8_t>(nb >> 8), static_cast<uint8_t>(nv), static_cast<uint8_t>(nv >> 8)});
            o.insert(o.end(), node.hdr, node.hdr + 20);
            const size_t table = o.size();
            o.resize(table + 4u * nv, 0);
            pad(o, 16);
            put32(o, m + 4, static_cast<uint32_t>(o.size() - m));
            o.insert(o.end(), bones.begin(), bones.end());
            pad(o, 16);
            for (size_t i = 0; i < node.vbs.size(); ++i)
            {
                put32(o, table + 4 * i, static_cast<uint32_t>(o.size() - m));
                writeVb(o, node.vbs[i]);
                pad(o, 16);
            }
        }

        // gamemodel.scr_write.
        std::vector<uint8_t> writeModel(const Model &model)
        {
            std::vector<uint8_t> o{'s', 'c', 'r', 0};
            const uint32_t n = static_cast<uint32_t>(model.nodes.size());
            o.resize(16, 0);
            put32(o, 4, model.kind);
            put32(o, 8, n);
            o.resize(16 + 4u * n, 0);
            pad(o, 16);
            std::vector<uint32_t> mdbs;
            for (const Node &node : model.nodes)
            {
                mdbs.push_back(static_cast<uint32_t>(o.size()));
                writeMdb(o, node, model.bones);
                pad(o, 16);
            }
            for (uint32_t i = 0; i < n; ++i)
            {
                const uint32_t no = static_cast<uint32_t>(o.size());
                put32(o, 16 + 4 * i, no);
                o.resize(no + 112, 0);
                put32(o, no, mdbs[i] - no);
                put32(o, no + 4, model.anchor - no);
                std::memcpy(o.data() + no + 8, model.nodes[i].name.c_str(), std::min<size_t>(8, model.nodes[i].name.size()));
                std::memcpy(o.data() + no + 16, model.nodes[i].srt, 36);
                std::memcpy(o.data() + no + 52, model.nodes[i].tail, 60);
            }
            return o;
        }

        // A TM3 (gamemodel.tm3_read / tm3_write): "TM3", count, 4, 0, offsets, names (8 bytes), TIM3s 64-aligned.
        bool readTm3(const std::vector<uint8_t> &t, std::vector<std::pair<std::string, std::vector<uint8_t>>> &out)
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
                out.emplace_back(std::string(name, strnlen(name, 8)), std::vector<uint8_t>(t.begin() + at, t.begin() + end));
            }
            return true;
        }
        std::vector<uint8_t> writeTm3(const std::vector<std::pair<std::string, std::vector<uint8_t>>> &texs)
        {
            const uint32_t n = static_cast<uint32_t>(texs.size());
            std::vector<uint8_t> o{'T', 'M', '3', 0};
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

        // A node's bones mapped (map[source] = gameplay bone) and its materials shifted; duplicate bones merged.
        void remap(Node &node, const std::vector<int> &map, int matBase)
        {
            for (Vb &vb : node.vbs)
            {
                vb.mat = static_cast<uint16_t>(vb.mat + matBase);
                for (size_t k = 0; k < vb.wt.size(); k += 8)
                {
                    std::array<std::pair<int, int>, 3> mix{};
                    int used = 0;
                    for (int s = 0; s < 3; ++s)
                    {
                        const int w = vb.wt[k + 4 + s];
                        if (w == 0)
                            continue;
                        const int src = vb.wt[k + 1 + s] / 4;
                        const int b = src >= 0 && src < static_cast<int>(map.size()) ? map[static_cast<size_t>(src)] : 4;
                        int at = 0;
                        while (at < used && mix[static_cast<size_t>(at)].first != b)
                            ++at;
                        if (at == used)
                            mix[static_cast<size_t>(used++)] = {b, 0};
                        mix[static_cast<size_t>(at)].second += w;
                    }
                    std::sort(mix.begin(), mix.begin() + used, [](const auto &a, const auto &b) { return a.second > b.second; });
                    for (int s = 0; s < 3; ++s)
                    {
                        vb.wt[k + 1 + s] = static_cast<uint8_t>(s < used ? mix[static_cast<size_t>(s)].first * 4 : 0);
                        vb.wt[k + 4 + s] = static_cast<uint8_t>(s < used ? mix[static_cast<size_t>(s)].second : 0);
                    }
                    vb.wt[k] = 0;
                    vb.wt[k + 7] = 0;
                }
            }
        }

        struct M3
        {
            double m[3][3] = {{1, 0, 0}, {0, 1, 0}, {0, 0, 1}};
            M3 operator*(const M3 &b) const
            {
                M3 r;
                for (int i = 0; i < 3; ++i)
                    for (int j = 0; j < 3; ++j)
                        r.m[i][j] = m[i][0] * b.m[0][j] + m[i][1] * b.m[1][j] + m[i][2] * b.m[2][j];
                return r;
            }
            std::array<double, 3> apply(const std::array<double, 3> &v) const
            {
                return {m[0][0] * v[0] + m[0][1] * v[1] + m[0][2] * v[2], m[1][0] * v[0] + m[1][1] * v[1] + m[1][2] * v[2],
                        m[2][0] * v[0] + m[2][1] * v[1] + m[2][2] * v[2]};
            }
        };
        M3 rot(int axis, double deg)
        {
            const double a = deg * 3.14159265358979323846 / 180.0, c = std::cos(a), s = std::sin(a);
            M3 r;
            const int i = (axis + 1) % 3, j = (axis + 2) % 3;
            r.m[i][i] = c;
            r.m[i][j] = -s;
            r.m[j][i] = s;
            r.m[j][j] = c;
            return r;
        }

        // A hand pose (degrees): the finger joints' curl towards the palm (knuckle, middle, tip) and the thumb's swing under
        // the fingers (about y) and curl (about z), at its root and its two joints.
        struct HandPose
        {
            double knuckle, middle, tip, thumbSwing, thumbCurl, thumbJointSwing, thumbJointCurl;
        };
        constexpr HandPose kRelaxed{35.0, 45.0, 30.0, 10.0, 5.0, 0.0, 10.0}; // as the gameplay model's open hands
        constexpr HandPose kFist{80.0, 88.0, 64.0, 48.0, -16.0, 32.0, 0.0};

        // The hands (el0c, 34 bones: 0 / 1 the left wrist / hand, 2-4 its thumb, 5-7 / 8-10 / 11-13 / 14-16 its fingers;
        // 17-33 the right the same) posed: the finger joints curled about z towards the palm (the palm faces down in the
        // rest pose), the thumb swung under the fingers about y and curled. Positions skinned with the hand's own
        // weights, normals turned with the record's heaviest bone.
        void poseHand(Node &node, const std::vector<uint8_t> &bones, const HandPose &pose)
        {
            const size_t nb = bones.size() / 16;
            std::vector<std::array<double, 3>> t(nb), bind(nb), at(nb);
            std::vector<int> parent(nb);
            std::vector<M3> local(nb), world(nb);
            for (size_t i = 0; i < nb; ++i)
            {
                for (int a = 0; a < 3; ++a)
                    t[i][static_cast<size_t>(a)] = rf(bones, 16 * i + 4 * a);
                int16_t p;
                std::memcpy(&p, bones.data() + 16 * i + 14, 2);
                parent[i] = p;
            }
            for (size_t i = 0; i < nb; ++i)
            {
                const size_t side = i >= 17 ? 17 : 0, k = i - side; // k: the bone within its hand
                const double s = side ? 1.0 : -1.0;                 // the left hand's fingers point along +x
                if (k >= 5 && k <= 16)
                {
                    const size_t joint = (k - 5) % 3;
                    local[i] = rot(2, s * (joint == 0 ? pose.knuckle : joint == 1 ? pose.middle : pose.tip));
                }
                else if (k == 2)
                    local[i] = rot(1, -s * pose.thumbSwing) * rot(2, s * pose.thumbCurl);
                else if (k == 3 || k == 4)
                    local[i] = rot(1, -s * pose.thumbJointSwing) * rot(2, s * pose.thumbJointCurl);
            }
            for (size_t i = 0; i < nb; ++i)
            {
                const int p = parent[i];
                if (p < 0 || static_cast<size_t>(p) >= i)
                {
                    bind[i] = at[i] = t[i];
                    world[i] = local[i];
                    continue;
                }
                const size_t q = static_cast<size_t>(p);
                for (int a = 0; a < 3; ++a)
                    bind[i][static_cast<size_t>(a)] = bind[q][static_cast<size_t>(a)] + t[i][static_cast<size_t>(a)];
                const auto d = world[q].apply(t[i]);
                for (int a = 0; a < 3; ++a)
                    at[i][static_cast<size_t>(a)] = at[q][static_cast<size_t>(a)] + d[static_cast<size_t>(a)];
                world[i] = world[q] * local[i];
            }
            for (Vb &vb : node.vbs)
            {
                if (vb.wt.empty())
                    continue;
                for (size_t r = 0; r < vb.count; ++r)
                {
                    const std::array<double, 3> v{vb.pos[3 * r], vb.pos[3 * r + 1], vb.pos[3 * r + 2]};
                    std::array<double, 3> sum{};
                    double total = 0.0;
                    int heavy = -1, heavyW = -1;
                    for (int s = 0; s < 3; ++s)
                    {
                        const int w = vb.wt[8 * r + 4 + static_cast<size_t>(s)];
                        const size_t b = vb.wt[8 * r + 1 + static_cast<size_t>(s)] / 4u;
                        if (w == 0 || b >= nb)
                            continue;
                        const auto d = world[b].apply({v[0] - bind[b][0], v[1] - bind[b][1], v[2] - bind[b][2]});
                        for (int a = 0; a < 3; ++a)
                            sum[static_cast<size_t>(a)] += w * (d[static_cast<size_t>(a)] + at[b][static_cast<size_t>(a)]);
                        total += w;
                        if (w > heavyW)
                            heavyW = w, heavy = static_cast<int>(b);
                    }
                    if (total <= 0.0)
                        continue;
                    for (int a = 0; a < 3; ++a)
                        vb.pos[3 * r + static_cast<size_t>(a)] = static_cast<float>(sum[static_cast<size_t>(a)] / total);
                    if (!vb.nrm.empty() && heavy >= 0)
                    {
                        auto n = world[static_cast<size_t>(heavy)].apply({static_cast<double>(vb.nrm[3 * r]), static_cast<double>(vb.nrm[3 * r + 1]),
                                                                          static_cast<double>(vb.nrm[3 * r + 2])});
                        const double len = std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
                        for (int a = 0; a < 3 && len > 0.0; ++a)
                            vb.nrm[3 * r + static_cast<size_t>(a)] =
                                static_cast<int8_t>(std::lround(std::clamp(n[static_cast<size_t>(a)] / len * 127.0, -127.0, 127.0)));
                    }
                }
            }
        }

        const Node *find(const Model &m, const char *name)
        {
            for (const Node &n : m.nodes)
                if (n.name == name)
                    return &n;
            return nullptr;
        }
        size_t countVertices(const Model &m)
        {
            size_t n = 0;
            for (const Node &node : m.nodes)
                for (const Vb &vb : node.vbs)
                    n += vb.count;
            return n;
        }
    }

    bool build(const std::vector<uint8_t> &ema0, const std::vector<uint8_t> &el0a, const std::vector<uint8_t> &el0b,
               const std::vector<uint8_t> &el0c, Built &out)
    {
        std::vector<uint8_t> gameTm3, gameMd, aTm3, aMd, bTm3, bMd, cTm3, cMd;
        if (!datEntry(ema0, 0, "TM3", gameTm3) || !datEntry(ema0, 1, "MD", gameMd) || !datEntry(el0a, 0, "TM3", aTm3) ||
            !datEntry(el0a, 1, "MD", aMd) || !datEntry(el0b, 0, "TM3", bTm3) || !datEntry(el0b, 1, "MD", bMd) ||
            !datEntry(el0c, 0, "TM3", cTm3) || !datEntry(el0c, 1, "MD", cMd))
        {
            out.error = "a file is not the expected dat (TM3, MD)";
            return false;
        }
        Model game, body, head, hands;
        if (!readModel(gameMd, game) || !readModel(aMd, body) || !readModel(bMd, head) || !readModel(cMd, hands))
        {
            out.error = "a model could not be read";
            return false;
        }
        std::vector<std::pair<std::string, std::vector<uint8_t>>> texs, bTex, cTex;
        if (!readTm3(aTm3, texs) || !readTm3(bTm3, bTex) || !readTm3(cTm3, cTex) || cTex.empty())
        {
            out.error = "a texture pack could not be read";
            return false;
        }
        if (game.bones.size() != 30 * 16 || body.bones != game.bones || head.bones.size() != 42 * 16 || hands.bones.size() != 34 * 16)
        {
            out.error = "the skeletons are not the ones this was made for";
            return false;
        }
        // Textures: the body's sheets, the head's after them. The hands' one sheet (el0c_03) is the body's own (the arms
        // and the hands share it; the two copies differ only in their palettes' rounding): only a disc without it there
        // gets a second copy.
        const int headBase = static_cast<int>(texs.size());
        texs.insert(texs.end(), bTex.begin(), bTex.end());
        int handBase = 0;
        while (handBase < headBase && texs[static_cast<size_t>(handBase)].first != cTex[0].first)
            ++handBase;
        if (handBase == headBase || cTex.size() != 1)
        {
            handBase = static_cast<int>(texs.size());
            texs.insert(texs.end(), cTex.begin(), cTex.end());
        }

        std::vector<int> bodyMap(30), headMap(42, 4), handMap(34);
        for (int i = 0; i < 30; ++i)
            bodyMap[static_cast<size_t>(i)] = i;
        headMap[0] = 2, headMap[1] = 3, headMap[2] = 4;
        for (int i = 3; i <= 6; ++i)
            headMap[static_cast<size_t>(i)] = 26; // the jaw and the lower lip
        for (int i = 0; i < 34; ++i)
            handMap[static_cast<size_t>(i)] = i < 17 ? 16 : 10;
        handMap[0] = 15, handMap[17] = 9; // the wrists' twist bones

        Model made;
        made.kind = game.kind;
        made.anchor = game.anchor;
        made.bones = game.bones;
        const auto take = [&](const Model &from, const char *name, const char *as, const std::vector<int> &map, int matBase) {
            const Node *n = find(from, name);
            if (!n)
                return false;
            Node copy = *n;
            copy.name = as;
            remap(copy, map, matBase);
            made.nodes.push_back(std::move(copy));
            return true;
        };
        bool ok = take(body, "z_blt00", "z_blt00", bodyMap, 0);
        for (const Node &n : head.nodes) // the hair cards and the eyelids, mouth (the head's own order)
            if (n.name != "hd" && n.name != "eye_L6")
                ok = take(head, n.name.c_str(), n.name.c_str(), headMap, headBase) && ok;
        // The fur round her armholes (w_ / x_): the gameplay model's own (el0a has none; its larger ema0_06 keeps the fur
        // in the same place, so the UVs hold).
        std::vector<std::pair<std::string, std::vector<uint8_t>>> gameTex;
        if (!readTm3(gameTm3, gameTex))
        {
            out.error = "the gameplay texture pack could not be read";
            return false;
        }
        int fur = 0;
        for (const Node &n : game.nodes)
        {
            if (n.name.rfind("w_", 0) != 0 && n.name.rfind("x_", 0) != 0)
                continue;
            Node copy = n;
            for (Vb &vb : copy.vbs)
            {
                const std::string &name = vb.mat < gameTex.size() ? gameTex[vb.mat].first : std::string();
                size_t to = 0;
                while (to < texs.size() && texs[to].first != name)
                    ++to;
                if (to == texs.size())
                    ok = false;
                vb.mat = static_cast<uint16_t>(to);
            }
            made.nodes.push_back(std::move(copy));
            ++fur;
        }
        ok = fur > 0 && take(body, "skirt00", "skirt00", bodyMap, 0) && take(body, "dress2", "dress", bodyMap, 0) &&
             take(body, "boots", "boots", bodyMap, 0) && ok;
        // The hands: the fist then the open hand, right then left, as the gameplay model orders them.
        for (const auto &[src, fistName, openName] : {std::tuple{"hand_R", "RH_01", "RH_00"}, std::tuple{"hand_R1", "LH_01", "LH_00"}})
        {
            const Node *n = find(hands, src);
            if (!n)
            {
                ok = false;
                continue;
            }
            for (const bool closed : {true, false})
            {
                Node copy = *n;
                copy.name = closed ? fistName : openName;
                poseHand(copy, hands.bones, closed ? kFist : kRelaxed);
                remap(copy, handMap, handBase);
                made.nodes.push_back(std::move(copy));
            }
        }
        ok = take(head, "hd", "HD_00", headMap, headBase) && take(head, "eye_L6", "eye_L6", headMap, headBase) && ok;
        if (!ok)
        {
            out.error = "a part is missing from the cutscene models";
            return false;
        }
        // The node records the game reads (flags in the tail) follow the gameplay model's where the name is the same.
        for (Node &n : made.nodes)
            if (const Node *g = find(game, n.name.c_str()))
                std::memcpy(n.tail, g->tail, sizeof n.tail), std::memcpy(n.srt, g->srt, sizeof n.srt);

        const std::vector<uint8_t> md = writeModel(made), tm3 = writeTm3(texs);
        std::vector<uint8_t> &dat = out.dat;
        dat.assign(4 + 8 + 8, 0);
        put32(dat, 0, 2);
        std::memcpy(dat.data() + 12, "TM3\0MD\0\0", 8);
        pad(dat, 0x20);
        put32(dat, 4, static_cast<uint32_t>(dat.size()));
        dat.insert(dat.end(), tm3.begin(), tm3.end());
        pad(dat, 0x20);
        out.md = static_cast<uint32_t>(dat.size());
        out.mdSize = static_cast<uint32_t>(md.size());
        put32(dat, 8, out.md);
        dat.insert(dat.end(), md.begin(), md.end());
        pad(dat, 0x20);
        out.textureBytes = static_cast<uint32_t>(tm3.size());
        out.gameTextureBytes = static_cast<uint32_t>(gameTm3.size());
        out.nodes = static_cast<int>(made.nodes.size());
        out.vertices = static_cast<int>(countVertices(made));
        return true;
    }
}
