// The native VU1 programs (src/kamite/render) on their own, without the runtime: replays the test vectors that
// "vu1_replay nativedump" wrote (each run's entry state and data memory, and the interpreter's results) and compares
// every field. Plain C++ and the program sources only, so it builds for any target, e.g. AArch64 for the NEON path:
//
//   aarch64-linux-gnu-g++ -std=c++20 -O2 -ffp-contract=off -static -I<repo>/src -I<repo>/runtime/ps2xRuntime/include \
//       tools/vu/vu1_native_check.cpp src/kamite/render/*.cpp -o check
//   qemu-aarch64 ./check vectors.bin
//
// (KAMITE_VU_SCALAR: the plain C++ path.) Programs are found by their code, as the runtime does (vu1_programs.cpp).
#include "runtime/ps2_vu1.h"
#include "runtime/ps2_vu1_native.h"

#include <cstdio>
#include <cstring>
#include <map>
#include <string>
#include <vector>

static std::vector<Vu1NativeEntry> g_entries;
void registerVu1NativeProgram(const Vu1NativeEntry &entry) { g_entries.push_back(entry); }
uint64_t vu1NativeCodeHash(const uint8_t *code, const Vu1NativeEntry &entry)
{
    uint64_t hash = 0xCBF29CE484222325ull;
    for (uint32_t at = entry.codeBegin; at < entry.codeEnd; ++at)
    {
        uint8_t byte = code[at];
        for (uint32_t v = 0; v < entry.variableCount; ++v)
            if (at - entry.variable[v] < 4u)
                byte = 0u;
        hash ^= byte;
        hash *= 0x100000001B3ull;
    }
    return hash;
}
namespace kamite::render
{
    void registerNativeVu1Programs();
}

struct Kicked
{
    std::vector<std::vector<uint8_t>> packets;
    const uint8_t *data;
};

// The packet XGKICK sends: GIF tags until the one with EOP.
static void kick(void *user, uint32_t qwordAddress, uint32_t)
{
    Kicked &k = *static_cast<Kicked *>(user);
    std::vector<uint8_t> packet;
    uint32_t at = (qwordAddress * 16u) & 0x3FFFu;
    for (;;)
    {
        uint64_t tag;
        std::memcpy(&tag, k.data + at, 8);
        const uint32_t nloop = tag & 0x7FFFu, format = (tag >> 58) & 3u;
        uint32_t nreg = (tag >> 60) & 0xFu;
        if (nreg == 0u)
            nreg = 16u;
        const uint32_t bytes = 16u + (format == 0u ? nloop * nreg * 16u : format == 1u ? ((nloop * nreg + 1u) & ~1u) * 8u : nloop * 16u);
        for (uint32_t b = 0; b < bytes; ++b)
            packet.push_back(k.data[(at + b) & 0x3FFFu]);
        at = (at + bytes) & 0x3FFFu;
        if ((tag >> 15) & 1u)
            break;
    }
    k.packets.push_back(std::move(packet));
}

int main(int argc, char **argv)
{
    if (argc < 2)
    {
        std::fprintf(stderr, "usage: vu1_native_check <vectors>\n");
        return 2;
    }
    kamite::render::registerNativeVu1Programs();
    FILE *in = std::fopen(argv[1], "rb");
    if (!in)
        return 2;
    std::map<std::string, uint64_t> fields;
    uint64_t runs = 0, differ = 0, declined = 0;
    std::vector<uint8_t> code(0x4000), dataIn(0x4000), dataOut(0x4000), data(0x4000);
    for (;;)
    {
        uint32_t magic = 0;
        if (std::fread(&magic, 4, 1, in) != 1)
            break;
        if (magic != 0x4E315556u)
        {
            std::fprintf(stderr, "bad vector (VU1State %zu bytes here)\n", sizeof(VU1State));
            return 1;
        }
        VU1State stIn{}, stOut{};
        uint64_t cycles = 0;
        uint32_t packetCount = 0;
        bool ok = std::fread(code.data(), 1, code.size(), in) == code.size() && std::fread(&stIn, sizeof(stIn), 1, in) == 1 &&
                  std::fread(dataIn.data(), 1, dataIn.size(), in) == dataIn.size() && std::fread(&stOut, sizeof(stOut), 1, in) == 1 &&
                  std::fread(dataOut.data(), 1, dataOut.size(), in) == dataOut.size() && std::fread(&cycles, 8, 1, in) == 1 &&
                  std::fread(&packetCount, 4, 1, in) == 1;
        std::vector<std::vector<uint8_t>> packets(packetCount);
        for (auto &pk : packets)
        {
            uint32_t size = 0;
            ok = ok && std::fread(&size, 4, 1, in) == 1;
            pk.resize(size);
            ok = ok && std::fread(pk.data(), 1, size, in) == size;
        }
        if (!ok)
        {
            std::fprintf(stderr, "truncated vector\n");
            return 1;
        }
        Vu1NativeProgram program = nullptr;
        for (const Vu1NativeEntry &e : g_entries)
            if (vu1NativeCodeHash(code.data(), e) == e.codeHash)
                for (uint32_t i = 0; i < e.entryCount; ++i)
                    if (e.entryPcs[i] == stIn.pc)
                        program = e.program;
        if (!program)
            continue;
        ++runs;
        VU1State st = stIn;
        data = dataIn;
        Kicked kicked{{}, data.data()};
        Vu1NativeRun run;
        run.state = &st;
        run.code = code.data();
        run.data = data.data();
        run.dataSize = 0x4000u;
        run.budget = ~0ull;
        run.kick = &kick;
        run.user = &kicked;
        if (!program(run))
        {
            ++declined;
            continue;
        }
        std::vector<std::string> notes;
        const auto note = [&](const std::string &field) {
            ++fields[field];
            notes.push_back(field);
        };
        for (int r = 1; r < 32; ++r)
            if (std::memcmp(st.vf[r], stOut.vf[r], 16) != 0)
                note("vf" + std::to_string(r));
        for (int r = 1; r < 16; ++r)
            if (st.vi[r] != stOut.vi[r])
                note("vi" + std::to_string(r));
        if (std::memcmp(st.acc, stOut.acc, 16) != 0)
            note("acc");
        if (std::memcmp(&st.q, &stOut.q, 4) != 0)
            note("q");
        if (std::memcmp(&st.p, &stOut.p, 4) != 0 || std::memcmp(&st.i, &stOut.i, 4) != 0)
            note("p/i");
        if (st.clip != stOut.clip)
            note("clip");
        if (st.mac != stOut.mac)
            note("mac");
        if (st.status != stOut.status)
            note("status");
        if (st.pc != stOut.pc)
            note("pc");
        if (run.cycles != cycles)
            note("cycles");
        if (data != dataOut)
            note("data");
        if (kicked.packets != packets)
            note("packets");
        if (!notes.empty() && differ++ < 10)
        {
            std::printf("run %llu pc %04x:", static_cast<unsigned long long>(runs), stIn.pc);
            for (const std::string &n : notes)
                std::printf(" %s", n.c_str());
            std::printf("\n");
        }
    }
    std::printf("%llu runs (%llu declined), %llu differ\n", static_cast<unsigned long long>(runs),
                static_cast<unsigned long long>(declined), static_cast<unsigned long long>(differ));
    for (const auto &[f, n] : fields)
        std::printf("  %s: %llu\n", f.c_str(), static_cast<unsigned long long>(n));
    return differ != 0 ? 1 : 0;
}
