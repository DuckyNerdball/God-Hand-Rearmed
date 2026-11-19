#pragma once

// Gameplay Olivia (em/ema0.dat) rebuilt from her cutscene parts on the player's disc (claude/limit-break-handoff.md,
// decision 10): the body (el/el0a.dat: dress, skirt, belt, boots), the head (el/el0b.dat: face, mouth, eyelids, eyes,
// hair) and the hands (el/el0c.dat), on her gameplay skeleton (30 bones, the same as el0a's) so every gameplay motion
// plays as before. Pure file layout, no runtime: body_detail.cpp builds it at boot and serves the file.
//
// - The head's 42 bones fold onto the gameplay ones: its chest / neck / head (0-2) onto 2-4, the jaw and lower lip
//   (3-6) onto the gameplay jaw (26, which the motions open when she talks), every other face and hair bone onto the
//   head (a neutral face and still hair, as the gameplay model has).
// - The hands' finger bones fold onto the hands (10 / 16) in two fixed poses, as the gameplay model keeps them: RH_00 /
//   LH_00 relaxed (half curled, as the gameplay model's open hands), RH_01 / LH_01 a fist; the game shows one of each
//   pair.
// - Node names the game looks up stay (HD_00, RH_00 / RH_01 / LH_00 / LH_01; the body stays "dress" for Cash's rules);
//   the gameplay model's own head, hands, hair and lashes go; the fur round her armholes stays (the cutscene dress
//   el0a has none; its larger sheet keeps the fur where the gameplay sheet has it).
// - Textures: the cutscene parts' sheets (the gameplay model's drop out), about three times the gameplay set in video
//   memory.

#include <cstddef>
#include <cstdint>
#include <string>
#include <vector>

namespace kamite::olivia_model
{
    struct Built
    {
        std::vector<uint8_t> dat; // the new em/ema0.dat (TM3 at entry 0, MD at entry 1)
        uint32_t md = 0, mdSize = 0;
        uint32_t textureBytes = 0, gameTextureBytes = 0; // the TM3s' sizes, new and the game's
        int nodes = 0, vertices = 0;
        std::string error;
    };

    // ema0: the game's em/ema0.dat; el0a / el0b / el0c: the cutscene parts.
    bool build(const std::vector<uint8_t> &ema0, const std::vector<uint8_t> &el0a, const std::vector<uint8_t> &el0b,
               const std::vector<uint8_t> &el0c, Built &out);
}
