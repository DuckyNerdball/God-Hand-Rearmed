// Bakes edits into a motion blob as punish_fit.cpp rewrite() does: ops file lines
// "curve <bone> <comp> <n> v..." (addCurve every 1) / "pre <bone> <n> 9n floats" (preRotate every 1).
#include "kamite/mot_edit.h"
#include <fstream>
#include <iterator>
#include <string>
#include <vector>
using namespace kamite;
int main(int argc, char **argv)
{
    std::ifstream f(argv[1], std::ios::binary);
    std::vector<uint8_t> m((std::istreambuf_iterator<char>(f)), {});
    std::ifstream of(argv[3]);
    std::string op;
    while (of >> op)
    {
        int bone, n, comp = 0;
        if (op == "curve") { of >> bone >> comp >> n; std::vector<float> v(n); for (float &x : v) of >> x; mot::addCurve(m, bone, comp, v, 1); }
        else { of >> bone >> n; std::vector<mot::Mat> v(n); for (auto &r : v) for (float &x : r) of >> x; mot::preRotate(m, bone, v, 1); }
    }
    std::ofstream o(argv[2], std::ios::binary);
    o.write(reinterpret_cast<const char *>(m.data()), m.size());
}
