// Applies per-frame pre-rotations to a motion blob as punish_fit.cpp rewrite() does (mot::preRotate, a key every frame),
// in the file's order: lines "<bone> <frames>" then frames x 9 floats (row-major).
#include "kamite/mot_edit.h"
#include <cstring>
#include <fstream>
#include <iterator>
#include <vector>
using namespace kamite;
int main(int argc, char **argv)
{
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> m((std::istreambuf_iterator<char>(f)), {});
    std::ifstream pf(argv[3]);
    int bone, n;
    while (pf >> bone >> n)
    {
        std::vector<mot::Mat> pre(n);
        for (auto &r : pre)
            for (float &x : r)
                pf >> x;
        mot::preRotate(m, bone, pre, 1);
    }
    std::ofstream o(argv[2], std::ios::binary);
    o.write(reinterpret_cast<const char *>(m.data()), m.size());
}
