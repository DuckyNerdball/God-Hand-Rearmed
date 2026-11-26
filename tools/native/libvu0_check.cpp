// The libvu0 natives' maths (src/kamite/native/libvu0_core.h) on its own, without the runtime: replays the vectors
// shadow_test wrote (a matrix, a vector, and the recompiled sceVu0ApplyMatrix's ACC and result under the game thread's
// float rules) and compares bit for bit. Plain C++ and the core header only, so it builds for any target (README.md):
// SSE, plain C++ (-DKAMITE_VU_SCALAR) and AArch64 NEON under qemu.
#include "kamite/native/libvu0_core.h"
#include "kamite/native/motion_core.h"

#include <cstdio>
#include <cstring>
#include <string>

int main(int argc, char **argv)
{
    using namespace kamite::native;
    if (argc < 2)
    {
        std::printf("usage: libvu0_check <vectors.bin>\n");
        return 2;
    }
    std::FILE *f = std::fopen(argv[1], "rb");
    if (!f)
        return 2;
    VuFloatEnv env; // round toward zero, denormals flushed: the game thread's MXCSR
    unsigned char record[112];
    int count = 0, bad = 0;
    while (std::fread(record, 1, sizeof(record), f) == sizeof(record))
    {
        const Vec4 m0 = load(record), m1 = load(record + 16), m2 = load(record + 32), m3 = load(record + 48);
        const Vec4 b = load(record + 64);
        if (!libvu0::finiteInputs(m0) || !libvu0::finiteInputs(m1) || !libvu0::finiteInputs(m2) ||
            !libvu0::finiteInputs(m3) || !libvu0::finiteInputs(b))
            continue;
        const libvu0::RowResult row = libvu0::mulRow(m0, m1, m2, m3, b);
        unsigned char acc[16], r[16];
        store(acc, row.acc);
        store(r, row.r);
        ++count;
        if (std::memcmp(acc, record + 80, 16) != 0 || std::memcmp(r, record + 96, 16) != 0)
        {
            if (++bad <= 5)
                std::printf("vector %d differs\n", count);
        }
    }
    std::fclose(f);

    // RotMatrixX / Y / Z (<vectors>.rot): ecossin, the rotation rows and the four rows of the product.
    int rotCount = 0, rotBad = 0;
    if (std::FILE *r = std::fopen((std::string(argv[1]) + ".rot").c_str(), "rb"))
    {
        unsigned char in[4 + 16 * 7 + 12 + 64], out[64 + 96 + 16 + 8];
        while (std::fread(in, 1, sizeof(in), r) == sizeof(in) && std::fread(out, 1, sizeof(out), r) == sizeof(out))
        {
            uint32_t axis, status, negative;
            float q;
            std::memcpy(&axis, in, 4);
            const unsigned char *p = in + 4;
            const Vec4 vf0 = load(p), coefficients = load(p + 16);
            libvu0::VuRegs v{load(p + 32), load(p + 48), load(p + 64), load(p + 80), load(p + 96), 0.0f, 0};
            std::memcpy(&q, p + 112, 4), std::memcpy(&status, p + 116, 4), std::memcpy(&negative, p + 120, 4);
            v.q = q;
            v.status = static_cast<uint16_t>(status);
            const unsigned char *matrix = p + 124;
            libvu0::ecossin(vf0, coefficients, negative != 0u, v);
            Vec4 vf4 = v.vf4;
            const libvu0::RotationRows m = axis == 0   ? libvu0::rotationRows<0>(vf0, v.vf5, vf4)
                                           : axis == 1 ? libvu0::rotationRows<1>(vf0, v.vf5, vf4)
                                                       : libvu0::rotationRows<2>(vf0, v.vf5, vf4);
            unsigned char got[sizeof(out)] = {};
            libvu0::RowResult row{};
            Vec4 last{};
            for (int k = 0; k < 4; ++k)
            {
                last = load(matrix + 16 * k);
                row = libvu0::mulRow(m.r0, m.r1, m.r2, m.r3, last);
                store(got + 16 * k, row.r);
            }
            store(got + 64, last);    // vf4
            store(got + 80, row.r);   // vf5
            store(got + 96, m.r0);    // vf6-vf9
            store(got + 112, m.r1);
            store(got + 128, m.r2);
            store(got + 144, m.r3);
            store(got + 160, row.acc);
            const uint32_t statusOut = v.status;
            std::memcpy(got + 176, &v.q, 4);
            std::memcpy(got + 180, &statusOut, 4);
            ++rotCount;
            if (std::memcmp(got, out, sizeof(out)) != 0 && ++rotBad <= 5)
            {
                for (size_t i = 0; i < sizeof(out); ++i)
                    if (got[i] != out[i])
                    {
                        std::printf("rot vector %d (axis %u) differs at byte %zu\n", rotCount, axis, i);
                        break;
                    }
            }
        }
        std::fclose(r);
    }
    bad += rotBad;

    // Rung 3's batch 1 (<vectors>.ops, written by shadow_test from the recompiled functions): OuterProduct, Normalize,
    // capVu0Length, InversMatrix, ScaleVector, MulVector, TransMatrix; records the natives would decline are skipped.
    int opsCount = 0, opsBad = 0;
    if (std::FILE *o = std::fopen((std::string(argv[1]) + ".ops").c_str(), "rb"))
    {
        unsigned char rec[256];
        const Vec4 vf0 = bitsOf(0u, 0u, 0u, 0x3F800000u);
        while (std::fread(rec, 1, sizeof(rec), o) == sizeof(rec))
        {
            uint32_t kind;
            uint16_t status;
            float s;
            std::memcpy(&kind, rec, 4), std::memcpy(&s, rec + 132, 4), std::memcpy(&status, rec + 136, 2);
            const Vec4 a = load(rec + 4), b = load(rec + 68), vf5 = load(rec + 84), vf6 = load(rec + 100), acc = load(rec + 116);
            unsigned char got[64], want[64];
            size_t size = 0;
            auto put = [&](const void *mine, const void *theirs, size_t n) {
                std::memcpy(got + size, mine, n), std::memcpy(want + size, theirs, n), size += n;
            };
            unsigned char t[16], u[16];
            if (kind == 1)
            {
                if (!libvu0::finiteXyz(a) || !libvu0::finiteXyz(b) || libvu0::exponent255(laneBits<3>(vf6)))
                    continue;
                const libvu0::OuterResult r = libvu0::outerProduct(a, b);
                store(t, blend<8>(r.r, sub(vf6, vf6))), put(t, rec + 140, 16);
                store(u, blend<7>(acc, r.acc)), put(u, rec + 220, 16);
            }
            else if (kind == 2)
            {
                if (!libvu0::finiteXyz(a) || !libvu0::ordinaryLength(libvu0::squaresSummed(a, a)))
                    continue;
                const libvu0::NormalizeResult n = libvu0::normalize(vf0, a, vf5, status);
                store(t, n.vf6), put(t, rec + 140, 16);
                store(u, n.vf5), put(u, rec + 242, 14);
                put(&n.q, rec + 236, 4), put(&n.status, rec + 240, 2);
            }
            else if (kind == 3)
            {
                if (!libvu0::finiteXyz(a))
                    continue;
                const libvu0::LengthResult l = libvu0::length(vf0, a, status);
                store(t, l.vf4), put(t, rec + 204, 16);
                put(&l.q, rec + 236, 4), put(&l.status, rec + 240, 2);
            }
            else if (kind == 4)
            {
                libvu0::Words r[4];
                bool ok = true;
                for (int k = 0; k < 4; ++k)
                    std::memcpy(r[k].w, rec + 4 + 16 * k, 16), ok = ok && libvu0::finiteXyz(load(r[k].w));
                if (!ok)
                    continue;
                const Vec4 v5 = load(r[3].w), v9 = blend<7>(v5, sub(v5, v5));
                store(r[3].w, v9);
                const libvu0::Words t4 = libvu0::pextlw(r[1], r[0]), t5 = libvu0::pextuw(r[1], r[0]);
                const libvu0::Words t6 = libvu0::pextlw(r[3], r[2]), t7 = libvu0::pextuw(r[3], r[2]);
                const libvu0::Words c0 = libvu0::pcpyld(t6, t4), c1 = libvu0::pcpyud(t4, t6), c2 = libvu0::pcpyld(t7, t5);
                const libvu0::InversResult x = libvu0::inversRows(load(c0.w), load(c1.w), load(c2.w), v5, v9, acc);
                unsigned char m[64];
                std::memcpy(m, c0.w, 16), std::memcpy(m + 16, c1.w, 16), std::memcpy(m + 32, c2.w, 16), store(m + 48, x.vf4);
                put(m, rec + 140, 48);
                store(t, x.vf4), put(t, rec + 188, 16);
            }
            else if (kind == 5)
            {
                if (!libvu0::finiteInputs(a) || (floatBits(s) & 0x7FFFFFFFu) >= 0x5E800000u)
                    continue;
                store(t, mul(a, splat(s))), put(t, rec + 140, 16);
            }
            else if (kind == 6)
            {
                if (!libvu0::finiteInputs(a) || !libvu0::finiteInputs(b))
                    continue;
                store(t, mul(a, b)), put(t, rec + 140, 16);
            }
            else
            {
                const Vec4 row3 = load(rec + 52);
                if (!libvu0::finiteXyz(row3) || !libvu0::finiteXyz(b))
                    continue;
                put(rec + 4, rec + 140, 48);
                store(t, blend<7>(row3, add(row3, b))), put(t, rec + 188, 16);
            }
            ++opsCount;
            if (std::memcmp(got, want, size) != 0 && ++opsBad <= 5)
                std::printf("ops vector %d (kind %u) differs\n", opsCount, kind);
        }
        std::fclose(o);
    }
    bad += opsBad;
#if defined(KAMITE_VU_SSE)
    const char *path = "SSE";
#elif defined(KAMITE_VU_NEON)
    const char *path = "NEON";
#else
    const char *path = "plain C++";
#endif
    // Rung 3's batch 2 (<vectors>.b2): the FPU scalar ops, VU0 max / min, cvt.w.s, mul by 1 / add, in the natives'
    // domains (as their entry checks keep them), against the runtime's results.
    int b2Count = 0, b2Bad = 0;
    if (std::FILE *o = std::fopen((std::string(argv[1]) + ".b2").c_str(), "rb"))
    {
        uint32_t rec[16];
        auto fl = [](uint32_t b) { float x; std::memcpy(&x, &b, 4); return x; };
        while (std::fread(rec, 4, 16, o) == 16)
        {
            uint32_t got[8] = {}, want[8] = {};
            int words = 0;
            switch (rec[0])
            {
            case 1:
            {
                constexpr uint32_t kLimit = 0x7E800000u;
                if ((rec[1] & 0x7FFFFFFFu) >= kLimit || (rec[2] & 0x7FFFFFFFu) >= kLimit || (rec[3] & 0x7FFFFFFFu) >= kLimit)
                    continue;
                const float d = subScalar(fl(rec[2]), fl(rec[1]));
                const uint32_t ed = (floatBits(d) >> 23) & 0xFFu;
                if (ed == 0u || ed == 255u)
                    continue;
                const float q0 = divScalar(fl(rec[1]), d), q1 = divScalar(fl(rec[3]), d);
                const float s0 = addScalar(q0, 1.0f), s1 = subScalar(s0, q1), m = mulScalar(s1, 255.0f);
                if (fpuPastMax(q0) || fpuPastMax(q1) || fpuPastMax(s0) || fpuPastMax(s1) || fpuPastMax(m))
                    continue;
                const float r[6] = {d, q0, q1, s0, s1, m};
                for (int k = 0; k < 6; ++k)
                    got[k] = floatBits(r[k]), want[k] = rec[4 + k];
                words = 6;
                break;
            }
            case 2:
            {
                const Vec4 a = load(rec + 1), b = load(rec + 5);
                store(got, vuMax(a, b));
                store(got + 4, vuMin(a, b));
                std::memcpy(want, rec + 9, 28);
                words = 7;
                break;
            }
            case 3:
                got[0] = static_cast<uint32_t>(fpuToWord(fl(rec[1])));
                want[0] = rec[2];
                words = 1;
                break;
            case 4:
            {
                const Vec4 a = load(rec + 1), b = load(rec + 5);
                if (!magnitudesBelow(blend<8>(a, splat(0.0f)), 0x7F7FFFFFu))
                    continue;
                volatile float one = 1.0f; // as the game's: from a register (a constant folds x * 1 to x)
                store(got, mul(a, splat(one)));
                std::memcpy(want, rec + 9, 12);
                words = 3;
                if (libvu0::finiteInputs(a) && libvu0::finiteInputs(b)) // the box test's add (inputs below 2^62)
                {
                    store(got + 3, add(a, b));
                    std::memcpy(want + 3, rec + 12, 16);
                    words = 7;
                }
                break;
            }
            default:
            {
                if ((rec[1] & 0x7FFFFFFFu) >= 0x7E800000u)
                    continue;
                const float k = fl(0x437F0000u);
                const float q0 = divScalar(fpuFromSmallWord(static_cast<int32_t>(rec[2])), k);
                const float q1 = divScalar(fpuFromSmallWord(static_cast<int32_t>(rec[3])), k);
                const float q2 = divScalar(fpuFromSmallWord(static_cast<int32_t>(rec[4])), k);
                const float pr = mulScalar(mulScalar(mulScalar(fl(rec[1]), q0), q1), q2);
                got[0] = floatBits(q0), got[1] = floatBits(q1), got[2] = floatBits(q2), got[3] = floatBits(pr);
                std::memcpy(want, rec + 5, 16);
                words = 4;
                break;
            }
            }
            ++b2Count;
            if (std::memcmp(got, want, 4u * static_cast<unsigned>(words)) != 0 && ++b2Bad <= 5)
                std::printf("batch-2 vector %d (kind %u) differs\n", b2Count, rec[0]);
        }
        std::fclose(o);
    }
    bad += b2Bad;

    // Rung 4 (<vectors>.motion, motion_core.h): Adjust_theta_vec, MtxInitRotVec, MtxMulRotVec, MtxMulScaleVec, MtxInverse against
    // the recompiled functions' results; records the natives would decline are skipped.
    int motionCount = 0, motionBad = 0;
    if (std::FILE *o = std::fopen((std::string(argv[1]) + ".motion").c_str(), "rb"))
    {
        unsigned char rec[4 + 4 + 16 + 48 + 16 + 64 + 64];
        while (std::fread(rec, 1, sizeof(rec), o) == sizeof(rec))
        {
            uint32_t kind, order;
            std::memcpy(&kind, rec, 4), std::memcpy(&order, rec + 4, 4);
            unsigned char got[64] = {};
            if (kind == 4u)
            {
                const motion::Mat4 a{{load(rec + 88), load(rec + 104), load(rec + 120), load(rec + 136)}};
                motion::Mat4 m;
                if (!motion::inverseInputs(a) || !motion::inverse(a, m))
                    continue;
                for (int k = 0; k < 4; ++k)
                    store(got + 16 * k, m.r[k]);
            }
            else if (kind == 0u)
            {
                motion::ThetaTable t;
                t.c = load(rec + 24);
                std::memcpy(t.one, rec + 40, 16), std::memcpy(t.mask, rec + 56, 16);
                const Vec4 v = load(rec + 8);
                if (!motion::thetaInputs(v, t))
                    continue;
                store(got, motion::adjustTheta(v, t));
            }
            else
            {
                motion::Mat4 b = motion::unit();
                if (kind == 3u)
                {
                    uint32_t s3[3];
                    std::memcpy(s3, rec + 8, 12);
                    b.r[0] = bitsOf(s3[0], 0u, 0u, 0u), b.r[1] = bitsOf(0u, s3[1], 0u, 0u), b.r[2] = bitsOf(0u, 0u, s3[2], 0u);
                }
                else
                {
                    float angles[3];
                    std::memcpy(angles, rec + 8, 12);
                    const Vec4 coefficients = load(rec + 72);
                    if ((order <= 5u && !libvu0::finiteInputs(coefficients)) ||
                        !motion::rotationByOrder(b, order, angles, motion::vf0Constant(), coefficients))
                        continue;
                }
                if (kind != 1u)
                {
                    const motion::Mat4 a{{load(rec + 88), load(rec + 104), load(rec + 120), load(rec + 136)}};
                    if (!motion::productInputs(a, b))
                        continue;
                    b = motion::product(a, b);
                }
                for (int k = 0; k < 4; ++k)
                    store(got + 16 * k, b.r[k]);
            }
            ++motionCount;
            if (std::memcmp(got, rec + 152, kind == 0u ? 16u : 64u) != 0 && ++motionBad <= 5)
                std::printf("motion vector %d (kind %u, order %u) differs\n", motionCount, kind, order);
        }
        std::fclose(o);
    }
    bad += motionBad;

    // The blend (<vectors>.blend: A, B, t, the atan table, next -> func_00156D70's out), motion_core.h's blendTrs.
    int blendCount = 0, blendBad = 0;
    if (std::FILE *o = std::fopen((std::string(argv[1]) + ".blend").c_str(), "rb"))
    {
        unsigned char rec[64 + 64 + 4 + 36 + 12 + 64];
        while (std::fread(rec, 1, sizeof(rec), o) == sizeof(rec))
        {
            const motion::Mat4 a{{load(rec), load(rec + 16), load(rec + 32), load(rec + 48)}};
            const motion::Mat4 b{{load(rec + 64), load(rec + 80), load(rec + 96), load(rec + 112)}};
            float t;
            motion::BlendTables tb;
            std::memcpy(&t, rec + 128, 4), std::memcpy(tb.atan, rec + 132, 36), std::memcpy(tb.next, rec + 168, 12);
            motion::Exact x;
            int calls = 0;
            const motion::Mat4 m = motion::blendTrs(x, a, b, t, tb, calls);
            if (!x.ok)
                continue;
            unsigned char got[64];
            for (int k = 0; k < 4; ++k)
                store(got + 16 * k, m.r[k]);
            ++blendCount;
            if (std::memcmp(got, rec + 180, 64) != 0 && ++blendBad <= 5)
                std::printf("blend vector %d differs\n", blendCount);
        }
        std::fclose(o);
    }
    bad += blendBad;

    // The twist (<vectors>.twist: parent world, local, axis, angle, the cos table -> func_001501D0's world with partFlags
    // 0x800 on its scaled path), motion_core.h's product + twist.
    int twistCount = 0, twistBad = 0;
    if (std::FILE *o = std::fopen((std::string(argv[1]) + ".twist").c_str(), "rb"))
    {
        unsigned char rec[64 + 64 + 16 + 4 + 16 + 64];
        while (std::fread(rec, 1, sizeof(rec), o) == sizeof(rec))
        {
            const motion::Mat4 pw{{load(rec), load(rec + 16), load(rec + 32), load(rec + 48)}};
            const motion::Mat4 local{{load(rec + 64), load(rec + 80), load(rec + 96), load(rec + 112)}};
            float axis[4], angle, c[4];
            std::memcpy(axis, rec + 128, 16), std::memcpy(&angle, rec + 144, 4), std::memcpy(c, rec + 148, 16);
            if (!motion::productInputs(pw, local))
                continue;
            motion::Mat4 w = motion::product(pw, local);
            motion::Exact x;
            int calls = 0;
            if (!motion::twist(x, w, axis, angle, c, calls))
                continue;
            unsigned char got[64];
            for (int k = 0; k < 4; ++k)
                store(got + 16 * k, w.r[k]);
            ++twistCount;
            if (std::memcmp(got, rec + 164, 64) != 0 && ++twistBad <= 5)
                std::printf("twist vector %d differs\n", twistCount);
        }
        std::fclose(o);
    }
    bad += twistBad;

    // The track (<vectors>.track, M3: n, t, the 0x156550 table, the track -> func_00158CD0's f0), motion_core's
    // trackSample.
    int trackCount = 0, trackBad = 0;
    if (std::FILE *o = std::fopen((std::string(argv[1]) + ".track").c_str(), "rb"))
    {
        unsigned char rec[4 + 4 + 64 + 0xC + 4 * 64 + 4];
        while (std::fread(rec, 1, sizeof(rec), o) == sizeof(rec))
        {
            uint32_t n, table[16], want;
            float t;
            std::memcpy(&n, rec, 4), std::memcpy(&t, rec + 4, 4), std::memcpy(table, rec + 8, 64);
            std::memcpy(&want, rec + 72 + 0xC + 4 * 64, 4);
            motion::Exact x;
            int calls = 0, loops = 0;
            const float v = motion::trackSample(x, rec + 72, n, t, table, calls, loops);
            if (!x.ok)
                continue;
            ++trackCount;
            uint32_t got;
            std::memcpy(&got, &v, 4);
            if (got != want && ++trackBad <= 5)
                std::printf("track vector %d differs\n", trackCount);
        }
        std::fclose(o);
    }
    bad += trackBad;

    // The decode (<vectors>.decode, M3 batch 2: the block, the cursor, t, the 0x156550 table -> func_00159AD0's f0 and
    // the cursor after), motion_core's RecDecoder over the block and the table alone (a read elsewhere declines).
    int decodeCount = 0, decodeBad = 0, decodeDeclined = 0;
    if (std::FILE *o = std::fopen((std::string(argv[1]) + ".decode").c_str(), "rb"))
    {
        static unsigned char rec[16 + 64 + 0x20 + 0x800 + 4 + 0x20];
        struct Mem
        {
            uint32_t block, cursor;
            const unsigned char *bytes, *table;
            const uint8_t *read(uint32_t address, uint32_t size)
            {
                if (address < cursor + 0x20u && cursor < address + size)
                    return nullptr;
                if (address >= block && address + size <= block + 0x800u && address + size > address)
                    return bytes + (address - block);
                if (address >= 0x41EDF0u && address + size <= 0x41EE30u)
                    return table + (address - 0x41EDF0u);
                return nullptr;
            }
        };
        while (std::fread(rec, 1, sizeof(rec), o) == sizeof(rec))
        {
            uint32_t head[4], want;
            std::memcpy(head, rec, 16);
            std::memcpy(&want, rec + 112 + 0x800, 4);
            float t;
            std::memcpy(&t, &head[3], 4);
            Mem mem{head[0], head[1], rec + 112, rec + 16};
            motion::Exact x;
            motion::RecDecoder<Mem> d{mem, x};
            std::memcpy(d.o, rec + 80, 0x20);
            const uint32_t got = d.decode(head[0], head[2], t);
            if (!d.ok || !x.ok)
            {
                ++decodeDeclined;
                continue;
            }
            ++decodeCount;
            if ((got != want || std::memcmp(d.o, rec + 116 + 0x800, 0x20) != 0) && ++decodeBad <= 5)
                std::printf("decode vector %d differs: f0 %08X want %08X\n", decodeCount, got, want);
        }
        std::fclose(o);
    }
    bad += decodeBad;
    std::printf("%s: %d vectors, %d differ; RotMatrix %d vectors, %d differ; batch 1 maths %d vectors, %d differ; batch 2 "
                "maths %d vectors, %d differ; motion %d vectors, %d differ; blend %d vectors, %d differ; twist %d vectors, "
                "%d differ; track %d vectors, %d differ; decode %d vectors (%d declined), %d differ\n", path, count,
                bad - rotBad - opsBad - b2Bad - motionBad - blendBad - twistBad - trackBad - decodeBad, rotCount, rotBad,
                opsCount, opsBad, b2Count, b2Bad, motionCount, motionBad, blendCount, blendBad, twistCount, twistBad,
                trackCount, trackBad, decodeCount, decodeDeclined, decodeBad);
    return bad ? 1 : 0;
}
