#pragma once

// Playable Olivia's player files, made from the player's own disc (claude/playable-olivia-handoff.md). Pure file layout,
// no runtime: olivia_player.cpp reads the disc, calls these at boot and serves the results through the file layer.
//
// - Her body file (served for every one of Gene's body files, pl00 / pl02 / ... / pl11, so Gene's costume choice never
//   reaches her): Gene's pl/pl00.dat with its texture pack (entry 0) and model (entry 1) replaced by hers, every other
//   entry (the TXE, the 600 motions and events, the effect pack) kept where the game looks for it (the player's +0x304
//   is the dat itself: entries are used by position).
//   - Her model: her gameplay model's nodes copied as they are (em/ema0.dat #1, or the block body_detail rebuilt from
//     it, so Money / Cash know her body), on her own skeleton (30 bones; Gene's motions touch bones up to 28). Gene's
//     four hand shapes per hand become hers (RH_02 / RH_03 / LH_02 / LH_03 copies of the ones `hands` names); GH_00 and
//     GHA_00 (the God Hand gauntlet and arm) are empty nodes, so unleashing shows the effects only.
//   - The bunny suit: el/elf4.dat's body (the cutscene costume) with her gameplay head, hair, lashes and hands.
//   - Her skin (the face, arms and chest sheets' skin colours) warmed toward cutscene Olivia's by `skin` (0 = as she is);
//     the bunny suit's skin (cutscene Olivia's) brought back to the same warmth.
//   - Gene's spread palm (02): her open hand posed as his goes from 00 to 02, keeping her fingers' shape (hand2 = 2).
//   - Gene's motions on her: what covers ground scaled by her height / Gene's (`ratio`, from the two heads' tops): the
//     hips' and the ankle targets' x / z, the root motion (bone 255); the hips' and ankles' heights moved by the
//     difference of the two skeletons (her legs are Gene's length, so the knees bend as his).
// - Her coat: Gene's coat files (pl01, pl06, pl08, pl0a, pl12) with every vertex at its bone (drawn as nothing; the
//   coat's object and chains run as before).

#include <cstdint>
#include <string>
#include <vector>

namespace kamite::olivia_player
{
    enum class Costume
    {
        Dress,
        Bunny,
    };

    struct Inputs
    {
        std::vector<uint8_t> pl00;   // Gene's pl/pl00.dat
        std::vector<uint8_t> ema0;   // em/ema0.dat
        std::vector<uint8_t> ema0Md; // her model as body_detail rebuilt it (empty: ema0's own)
        std::vector<uint8_t> elf4;   // el/elf4.dat (the bunny suit only)
        std::vector<uint8_t> elf4Md; // its model as body_detail rebuilt it (empty: its own)
        Costume costume = Costume::Dress;
        float skin = 0.0f;           // 0..1: how far toward cutscene Olivia's skin
        int hand2 = 0, hand3 = 1;    // her hand for Gene's shape 02 (spread palm) / 03 (pointing): 0 relaxed, 1 fist,
                                     // 2 (02 only) a spread palm made from her relaxed hand by Gene's 00 -> 02
    };

    struct Built
    {
        std::vector<uint8_t> body;
        float ratio = 1.0f;            // her height / Gene's
        float hipDy = 0.0f, ankleDy = 0.0f;
        int nodes = 0, motions = 0, channels = 0, skinColours = 0, spreadHands = 0;
        uint32_t textureBytes = 0;
        std::string error;
    };

    bool build(const Inputs &in, Built &out);

    // A coat file (pl01 ...) with every model's vertices collapsed onto their bones. False: not a coat dat.
    bool hideCoat(const std::vector<uint8_t> &coat, std::vector<uint8_t> &out);
}
