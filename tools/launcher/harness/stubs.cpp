// Stubs: everything launcher.cpp calls outside the launcher's own modules (a Linux harness only).
#include "kamite/character_mods.h"
#include "kamite/disc_files.h"
#include "kamite/game_disc.h"
#include "kamite/launcher_platform.h"
#include "kamite/launcher_update.h"
#include "kamite/log.h"
#include "ps2_host_gamepads.h"
#include "ps2_host_input.h"
#include "runtime/ps2_display_options.h"
#include "runtime/ps2_hotkeys.h"
#include "raylib.h"
#include <cstdio>
#include <cstdlib>
#include <fstream>
namespace ps2_host_input::controls {
static std::string empty;
const std::string &path() { return empty; }
bool dirty() { return false; }
const std::string &fileError() { return empty; }
bool envOverride(uint32_t) { return false; }
std::vector<std::string> bindings(uint32_t, std::string_view) { return {}; }
void setBindings(uint32_t, std::string_view, std::vector<std::string>) {}
void resetDefaults(uint32_t) {}
bool save(std::string &) { return true; }
Kind kindOf(std::string_view) { return Kind::Keys; }
bool sameBinding(std::string_view a, std::string_view b) { return a == b; }
std::string displayName(std::string_view b) { return std::string(b); }
std::string keyBinding(int) { return {}; }
const std::vector<int> &bindableKeys() { static std::vector<int> v; return v; }
std::string mouseBinding(int) { return {}; }
std::string padButtonBinding(int, int) { return {}; }
std::string padAxisBinding(int, int, bool) { return {}; }
std::string keyboardKeyUse(uint32_t, int) { return {}; }
}
namespace ps2_host_display {
void openHostWindow(const char *title) {
  const char *w = std::getenv("LH_W"), *h = std::getenv("LH_H");
  InitWindow(w ? std::atoi(w) : 1600, h ? std::atoi(h) : 900, title);
  SetExitKey(KEY_NULL);
}
bool vsyncEnabled() { return false; }
}
namespace ps2_host_gamepads {
void update() {}
bool connected(int) { return false; }
bool button(int, Button) { return false; }
float axis(int, Axis) { return 0.0f; }
}
namespace ps2x::hotkeys {
const Info &info(Action) { static Info i{}; return i; }
Binding binding(Action) { return Binding{}; }
bool available(Action) { return false; }
void setBinding(Action, Binding) {}
Action conflict(Action a, Binding) { return a; }
Action plainKeyHolder(int) { return Action{}; }
std::string text(Binding) { return {}; }
bool parse(std::string_view, Binding &) { return false; }
int capturePressed() { return 0; }
}
namespace kamite {
namespace characters { std::vector<Character> list(const std::filesystem::path &) { return {}; } }
namespace disc_files {
bool readFromImage(const std::filesystem::path &, std::string_view path, std::vector<uint8_t> &out) {
  std::ifstream in("/mnt/user-data/uploads/Claude/God Hand Extract/" + std::string(path), std::ios::binary);
  if (!in) return false; out.assign(std::istreambuf_iterator<char>(in), {}); return true;
}
}
DiscCheckResult verifyDiscAndPrepare(const std::filesystem::path &, const std::filesystem::path &) { DiscCheckResult r; r.ok = true; return r; }
namespace log {
void write(Category, Level level, std::string_view message) { std::fprintf(stderr, "[%d] %.*s\n", (int)level, (int)message.size(), message.data()); }
bool traceEnabled(Category) { return false; }
}
namespace launcher::update {
std::string versionLabel() { return "v0.6.9 Beta (dev abc1234)"; }
void startCheck(Checker &c, bool) { c.state.store(std::getenv("LH_UPDATE") ? (int)CheckState::Available : (int)CheckState::Off); if (std::getenv("LH_UPDATE")) c.release.tag = "v0.7.0"; }
void startInstall(Installer &, const Release &, const std::filesystem::path &) {}
void cleanup(const std::filesystem::path &) {}
}
namespace launcher::platform {
std::filesystem::path pick(void *, bool) { return {}; }
void open(const std::filesystem::path &) {}
bool shiftHeld() { return false; }
bool relaunch(const std::filesystem::path &) { return false; }
std::filesystem::path thisExe() { return "/tmp/lh/GHR"; }
}
}
#include "kamite/launcher_steam.h"
namespace kamite::launcher::steam {
Status probe() { Status s; s.available = true; return s; }
bool running() { return false; }
Result addOrUpdate(const std::filesystem::path &, std::atomic<int> &stage) { stage = (int)Stage::Done; Result r; r.ok = true; r.message = "Added."; return r; }
}
