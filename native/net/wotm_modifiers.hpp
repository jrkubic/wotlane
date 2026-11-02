// ABOUTME: Gameplay modifiers (Super Jump, Fast Monsters, Turbo): names, bits, the list text the lobby, launcher and WOTM_MODIFIERS
// ABOUTME: use, their fixed strengths, and turbo's frame and clock math. Shared by the runtime, the settings and wotm_server.
#pragma once

#include <cstdint>
#include <string>

namespace wotm::modifiers
{
enum Bit
{
    Jump = 1,    // Super Jump: moon gravity (TheGame::m_gravity x kJumpGravity)
    Fast = 2,    // Fast Monsters: walk, run, dash and ram x kFastSpeed
    Turbo = 4,   // the whole fight at 2x: 2 fields per frame, each frame paced at 1 vsync
};
inline constexpr int kCount = 3;
inline constexpr const char *kNames[kCount] = {"jump", "fast", "turbo"};   // canonical order: bit i = 1 << i
inline constexpr float kJumpGravity = 0.45f;
inline constexpr float kFastSpeed = 1.5f;

// "turbo,jump" -> Jump | Turbo: comma list in any order, spaces around names ignored, repeats allowed, "" = none. An unknown or
// empty name returns false and leaves `bits` alone.
inline bool parse(const std::string &list, int &bits)
{
    int out = 0;
    if (list.find_first_not_of(' ') == std::string::npos)
    {
        bits = 0;
        return true;
    }
    size_t at = 0;
    while (true)
    {
        const size_t comma = list.find(',', at);
        std::string name = list.substr(at, comma == std::string::npos ? std::string::npos : comma - at);
        name.erase(0, name.find_first_not_of(' '));
        name.erase(name.find_last_not_of(' ') + 1);
        int bit = 0;
        for (int i = 0; i < kCount; ++i)
            if (name == kNames[i])
                bit = 1 << i;
        if (!bit)
            return false;
        out |= bit;
        if (comma == std::string::npos)
            break;
        at = comma + 1;
    }
    bits = out;
    return true;
}

// Bits -> list in canonical order ("jump,turbo"); 0 = ""
inline std::string format(int bits)
{
    std::string s;
    for (int i = 0; i < kCount; ++i)
        if (bits & (1 << i))
            s += (s.empty() ? "" : ",") + std::string(kNames[i]);
    return s;
}

// Fixed-step frame shape: game fields each frame advances, and host vsyncs it lasts (`step` = WOTM_FIXEDSTEP)
inline int fieldsPerFrame(int step, int bits) { return bits & Turbo ? 2 : step; }
inline int vsyncsPerFrame(int step, int bits) { return bits & Turbo ? 1 : step; }

// Offline turbo clock: the host clock seen by the game runs `scale` times faster from the switch (host0 -> game0), so the game's
// time stays continuous across the switch. Unsigned arithmetic: the 32-bit tick counter wraps the same way the game's does.
inline uint32_t scaledClock(uint32_t host, uint32_t host0, uint32_t game0, uint32_t scale) { return game0 + (host - host0) * scale; }
} // namespace wotm::modifiers
