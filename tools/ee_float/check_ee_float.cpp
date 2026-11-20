// Checks runtime/ps2xRuntime's exact EE float maths (ps2_ee_float.h) against GT3 Revisited's pinned results: their
// platform hash over 200,000 random operand triples (tests/test_ee_float.cpp, ee_float_platform_hash h[0..2]) and their
// known PS2 results (the multiplier one below the chopped product, the divider one above). Any host, any compiler,
// from the repo root (one line):
//   g++ -std=c++20 -O2 -Iruntime/ps2xRuntime/include tools/ee_float/check_ee_float.cpp
//       runtime/ps2xRuntime/src/lib/ps2_ee_float.cpp -o check_ee_float && ./check_ee_float
#include "ps2_ee_float.h"

#include <cstdio>
#include <random>

namespace
{
    // GT3 Revisited's operand mix: any bit pattern, exponents near each other, near the ends of the range, zeros.
    uint32_t operand(std::mt19937 &rng, uint32_t near)
    {
        const uint32_t r = rng();
        switch (rng() % 6)
        {
        case 0:
            return r;
        case 1:
            return (r & 0x807FFFFFu) | (((near >> 23) & 0xFF) + (rng() % 60) - 30) % 256 << 23;
        case 2:
            return (r & 0x807FFFFFu) | ((rng() % 8) << 23);
        case 3:
            return (r & 0x807FFFFFu) | ((248 + rng() % 8) << 23);
        case 4:
            return r & 0x807FFFFFu;
        default:
            return (r & 0x80000000u) | (near & 0x7FFFFFFFu) | (rng() & 1);
        }
    }
    int g_failures = 0;
    void check(bool ok, const char *what)
    {
        if (!ok)
        {
            std::printf("FAIL: %s\n", what);
            ++g_failures;
        }
    }
} // namespace

int main()
{
    using namespace ps2ee;
    std::mt19937 rng(7);
    uint64_t h[3] = {1469598103934665603ull, 1469598103934665603ull, 1469598103934665603ull};
    auto mix = [&](int k, uint32_t v) { h[k] = (h[k] ^ v) * 1099511628211ull; };
    for (int i = 0; i < 200000; ++i)
    {
        const uint32_t a = operand(rng, rng()), b = operand(rng, a), c = operand(rng, b);
        mix(0, add(a, b) ^ sub(a, c));
        mix(1, mul(a, b) ^ madd(c, a, b));
        mix(2, div(a, b) ^ ps2ee::sqrt(a));
    }
    check(h[0] == 0x534ba27d4f41d126ull, "add / sub hash");
    check(h[1] == 0xd1182518a327deccull, "mul / madd hash");
    check(h[2] == 0x8b0a9ed8cf1ef085ull, "div / sqrt hash");

    const uint32_t m[][3] = {
        {0x42867EF9u, 0x44E2CAB1u, 0x47EE4D29u}, {0xC23EADDCu, 0x390B27B6u, 0xBBCF4BF4u},
        {0xC67EABFEu, 0xC7199C76u, 0x4E18D070u}, {0x43574BC8u, 0xC41DC545u, 0xC804AF60u},
        {0x4AFD0AAFu, 0xB383B509u, 0xBF022F68u}, {0x3C0F1FDBu, 0x3F7C1EA4u, 0x3C0CF47Cu},
        {0x3F800000u, 0x3F7FFFFFu, 0x3F7FFFFEu}, {0x3F7FFFFFu, 0x3F800000u, 0x3F7FFFFFu}}; // 1.0 * m low, m * 1.0 exact
    const uint32_t d[][3] = {
        {0xBF6F746Eu, 0xBBD8AF44u, 0x430D7368u}, {0xC2F683A1u, 0x453D2682u, 0xBD26D192u},
        {0xBD357760u, 0xB79071B7u, 0x4520CEAEu}, {0xB7992194u, 0xC4AB79DFu, 0x32649CD0u},
        {0x3D83815Eu, 0x456DF726u, 0x378D78C0u}, {0xB7DE9689u, 0x3A5574FFu, 0xBD0579B6u},
        {0x3F800000u, 0x40400000u, 0x3EAAAAABu}, {0x40A00000u, 0x41200000u, 0x3F000000u}}; // 1 / 3, 5 / 10
    const uint32_t s[][2] = {{0x36CC3609u, 0x3B21ACF8u}, {0x3F5C248Cu, 0x3F6D6535u}, {0x495F7BE1u, 0x446F30A7u},
                             {0x371B6734u, 0x3B477510u}, {0x3FF73693u, 0x3FB1E2B7u}, {0x40ADA8C8u, 0x40151786u},
                             {0x40000000u, 0x3FB504F3u}, {0x40400000u, 0x3FDDB3D7u}};
    for (const auto &t : m)
        check(mul(t[0], t[1]) == t[2], "known PS2 product");
    for (const auto &t : d)
        check(div(t[0], t[1]) == t[2], "known PS2 quotient");
    for (const auto &t : s)
        check(ps2ee::sqrt(t[0]) == t[1] && ps2ee::sqrt(t[0] | kSign) == t[1] &&
                  rsqrt(0x3F800000u, t[0]) == div(0x3F800000u, t[1]),
              "known PS2 root");
    check(div(0xC0000000u, 0u) == 0xFFFFFFFFu && div(0u, kSign) == 0xFFFFFFFFu && div(kSign, 0x3F800000u) == kSign,
          "division by zero / of zero");

    std::printf("%s\n", g_failures ? "ps2_ee_float: FAILED" : "ps2_ee_float: matches GT3 Revisited's pinned results");
    return g_failures != 0;
}
