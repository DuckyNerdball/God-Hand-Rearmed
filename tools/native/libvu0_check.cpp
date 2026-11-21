// The libvu0 natives' maths (src/kamite/native/libvu0_core.h) on its own, without the runtime: replays the vectors
// shadow_test wrote (a matrix, a vector, and the recompiled sceVu0ApplyMatrix's ACC and result under the game thread's
// float rules) and compares bit for bit. Plain C++ and the core header only, so it builds for any target (README.md):
// SSE, plain C++ (-DKAMITE_VU_SCALAR) and AArch64 NEON under qemu.
#include "kamite/native/libvu0_core.h"

#include <cstdio>
#include <cstring>

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
#if defined(KAMITE_VU_SSE)
    const char *path = "SSE";
#elif defined(KAMITE_VU_NEON)
    const char *path = "NEON";
#else
    const char *path = "plain C++";
#endif
    std::printf("%s: %d vectors, %d differ\n", path, count, bad);
    return bad ? 1 : 0;
}
