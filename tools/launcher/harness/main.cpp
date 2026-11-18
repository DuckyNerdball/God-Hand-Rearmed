#include "kamite/launcher.h"
#include <cstdio>
#include <cstdlib>
int main() {
  kamite::launcher::Context c;
  c.exeDir = "/tmp/lh/exe"; c.iniPath = "/tmp/lh/exe/GHR.ini"; c.cacheDir = "/tmp/lh/exe/game_data"; c.iso = std::getenv("LH_NOISO") ? "" : "/tmp/lh/exe/fake.iso";
  auto r = kamite::launcher::run(c);
  std::printf("result %d\n", (int)r);
}
