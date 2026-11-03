// More CPUs in free for all than the game's menu allows (NATIVO.md, "More CPUs from the menu"): up to 8 monsters in the fight, 0 to 7 CPUs
// with 1 player (screen 12) and 0 to 8 - N with N = 2 to 4 (screen 13). The screens' hooks (wotm_overrides.cpp) store the chosen number;
// the menu carries on with what the game knows (3 CPUs with 1 player, 2 with 2, none with 3 and 4), and wotm_padsel.inc puts the extra
// CPUs into the selection and writes the real number when the stage is confirmed (1 and 2 players) or on leaving the selection (3 and 4).
#pragma once
#include <atomic>

namespace wotm_menucpus
{
constexpr int kMonsters = 8;          // the WOTM_MATCH ceiling (8 monsters in the fight)
constexpr int kMax = kMonsters - 1;   // 1 player + 7 AIs
constexpr int maxFor(int players) { return kMonsters - players; }
inline std::atomic<int> g_want{0};          // CPUs requested beyond what the game knows; 0: m_numAIs applies
inline std::atomic<int> g_menuPlayers{2};   // multiplayer players (2 to 4, from pad selection): the maximum for screen 13
}
