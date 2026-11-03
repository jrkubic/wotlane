// wotm_modifiers.inc: gameplay modifiers (net/wotm_modifiers.hpp) inside the runtime. Included by wotm_overrides.cpp.
//
// The active set is latched once per fight (wotm_sync::g_mods, set by wotm_sync::levelLoad at Shell::LoadLevelFiles) and cleared at
// rtReturnToShell, so a change in the options panel only reaches the next fight and post-fight menus run at normal speed. In a
// network match the latch happens at the same frame index on every PC and the set is in the config hash.
//  - Super Jump: TheGame::m_gravity x0.45. Its only writer is TheGame::InitBeforeDbLoad (one swc1), so the scale goes on entry of
//    InitAfterDbLoad, which follows it. Global gravity keeps the game's own jump-height predictions (StateJump/StateFly::
//    getJumpHeightGain) and throw aims consistent with the physics; per-monster m_gravityScale would not.
//  - Fast Monsters: walk/run take their target speed and acceleration in MonsterDynamics::updateFootMotion($f12, $f13); dash and
//    ram attack compare against MonsterDynamics::getSpeedModifier. Both x1.5.
//  - Turbo: fixed step -> wotm_sync::frame and the frame wait (wotm_overrides.cpp) use fieldsPerFrame/vsyncsPerFrame; offline ->
//    the host clock the game sees runs 2x (setGameClockScale).
//  - WOTM_MODSTATS=1: player 1's highest rise (cs trans z from where its vertical velocity turned positive to where it stopped
//    rising: a jump's height, not the stage's) and peak ground speed (|vel.xy|) since the fight start, every 300 frames; and per
//    rise its height and the gravity it felt (vertical velocity lost per second over frames 1-6 after takeoff: releasing the jump
//    button damps the rise later on, so the height depends on the input but this does not). For scripts/modcheck.sh (coord).
namespace wotm_modifiers
{
    namespace md = wotm::modifiers;
    constexpr uint32_t kInitAfterDbLoad = wotm::fn::InitAfterDbLoad__7TheGame, kRtReturnToShell = wotm::fn::rtReturnToShell__Fii;
    constexpr uint32_t kFootMotion = wotm::fn::updateFootMotion__15MonsterDynamicsfff;
    constexpr uint32_t kSpeedModifier = wotm::fn::getSpeedModifier__15MonsterDynamics;
    constexpr uint32_t kGameGravity = 0x1203C4u, kGameMonsters = 0x120380u;   // TheGame::m_gravity, m_monsters[0] (include/game/game.h)
    constexpr uint32_t kMonsterCs = 0xCu, kCsTrans = 0x10u, kMonsterVel = 0x1E0u;   // Monster::m_cs, _cs::trans, m_dynamics.m_body.vel
    PS2Runtime::RecompiledFunction g_origInitAfterDbLoad = nullptr, g_origRtReturn = nullptr, g_origFootMotion = nullptr,
                                   g_origSpeedModifier = nullptr;
    uint32_t g_gravityWritten = 0;   // bits of the last gravity written: a second InitAfterDbLoad without a new base never scales twice
    bool g_stats = false;
    float g_takeoffZ = 0.f, g_maxRise = 0.f, g_maxSpeed = 0.f;
    bool g_rising = false;
    uint32_t g_riseFrames = 0;   // frames since takeoff
    float g_vz1 = 0.f, g_riseAccel = 0.f;

    bool on(int bit) { return (wotm_sync::g_mods & bit) != 0; }

    // Shell::LoadLevelFiles entry (wotm_sync::levelLoad): latch the panel's (or the lobby's) set for this fight
    void fightStart()
    {
        wotm_sync::g_mods = wotm::settings::modifiers();
        g_rising = false, g_maxRise = 0.f, g_maxSpeed = 0.f;
        if (!wotm_sync::g_fixedStep)
            setGameClockScale(on(md::Turbo) ? 2u : 1u);
        std::fprintf(stderr, "[modifiers] fight: \"%s\"\n", md::format(wotm_sync::g_mods).c_str());
    }

    void rtReturnModsOff(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kRtReturnToShell && wotm_sync::g_mods)
        {
            wotm_sync::g_mods = 0;
            if (!wotm_sync::g_fixedStep)
                setGameClockScale(1u);
            std::fprintf(stderr, "[modifiers] off (back to the shell)\n");
        }
        g_origRtReturn(rdram, ctx, runtime);
    }

    void initAfterDbLoadGravity(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kInitAfterDbLoad && on(md::Jump))
        {
            const uint32_t game = READ32(wotm::sym::game) & 0x1FFFFFFFu;
            if (ramOk(game + kGameGravity, 4u))
            {
                uint32_t bits = READ32(game + kGameGravity);
                if (bits != g_gravityWritten)
                {
                    float g;
                    std::memcpy(&g, &bits, 4);
                    const float moon = g * md::kJumpGravity;
                    std::memcpy(&g_gravityWritten, &moon, 4);
                    WRITE32(game + kGameGravity, g_gravityWritten);
                    std::fprintf(stderr, "[modifiers] super jump: gravity %.1f -> %.1f\n", g, moon);
                }
            }
        }
        g_origInitAfterDbLoad(rdram, ctx, runtime);
    }

    void footMotionFast(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kFootMotion && on(md::Fast))
        {
            ctx->f[12] *= md::kFastSpeed;   // target speed
            ctx->f[13] *= md::kFastSpeed;   // acceleration: the same time to reach the faster speed
        }
        g_origFootMotion(rdram, ctx, runtime);
    }

    // a leaf function (no waits): it always returns here, so its $f0 can be scaled after the call
    void speedModifierFast(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool entry = ctx->pc == kSpeedModifier;
        const uint32_t ra = GPR_U32(ctx, 31);
        g_origSpeedModifier(rdram, ctx, runtime);
        if (entry && ctx->pc == ra && on(md::Fast))
            ctx->f[0] *= md::kFastSpeed;
    }

    // WOTM_MODSTATS: once per frame (waitMinRateByVblanks)
    void statsFrame(uint8_t *rdram)
    {
        if (!g_stats || !wotm_sync::g_levelLoaded)
            return;
        const uint32_t game = FAST_READ32(wotm::sym::game) & 0x1FFFFFFFu;
        if (!ramOk(game + kGameMonsters, 4u))
            return;
        const uint32_t mon = FAST_READ32(game + kGameMonsters) & 0x1FFFFFFFu;
        if (!ramOk(mon, kMonsterVel + 12u))
            return;
        const uint32_t cs = FAST_READ32(mon + kMonsterCs) & 0x1FFFFFFFu;
        if (!ramOk(cs + kCsTrans, 12u))
            return;
        const float z = rdF(rdram, cs + kCsTrans + 8u), vx = rdF(rdram, mon + kMonsterVel), vy = rdF(rdram, mon + kMonsterVel + 4u),
                    vz = rdF(rdram, mon + kMonsterVel + 8u);
        if (vz > 0.f && !g_rising)
            g_takeoffZ = z, g_rising = true, g_riseFrames = 0, g_riseAccel = 0.f;
        else if (vz <= 0.f && g_rising)
        {
            g_rising = false;
            if (z - g_takeoffZ > 20.f)   // each real rise (not the ground's jitter)
                std::fprintf(stderr, "[modstats] rise %.1f gravity %.1f at frame %u\n", z - g_takeoffZ, g_riseAccel, wotm_sync::g_index);
        }
        if (g_rising)
        {
            ++g_riseFrames;
            if (g_riseFrames == 1u)
                g_vz1 = vz;
            else if (g_riseFrames == 6u)
                g_riseAccel = (vz - g_vz1) / 5.f * 60.f / static_cast<float>(wotm::modifiers::fieldsPerFrame(std::max(wotm_sync::g_fixedStep, 1), wotm_sync::g_mods));
        }
        if (g_rising)
            g_maxRise = std::max(g_maxRise, z - g_takeoffZ);
        g_maxSpeed = std::max(g_maxSpeed, std::sqrt(vx * vx + vy * vy));
        if (wotm_sync::g_index % 300u == 0u)
            std::fprintf(stderr, "[modstats] frame %u peak_height %.1f peak_speed %.1f\n", wotm_sync::g_index, g_maxRise, g_maxSpeed);
    }

    void install(PS2Runtime &runtime)
    {
        g_stats = std::getenv("WOTM_MODSTATS") && std::getenv("WOTM_MODSTATS")[0] == '1';
        if ((g_origInitAfterDbLoad = runtime.lookupFunction(kInitAfterDbLoad)))
            runtime.registerFunction(kInitAfterDbLoad, &initAfterDbLoadGravity);
        if ((g_origRtReturn = runtime.lookupFunction(kRtReturnToShell)))   // after wotm_sync::init, whose wrapper it chains
            runtime.registerFunction(kRtReturnToShell, &rtReturnModsOff);
        if ((g_origFootMotion = runtime.lookupFunction(kFootMotion)))
            runtime.registerFunction(kFootMotion, &footMotionFast);
        if ((g_origSpeedModifier = runtime.lookupFunction(kSpeedModifier)))
            runtime.registerFunction(kSpeedModifier, &speedModifierFast);
    }
}
