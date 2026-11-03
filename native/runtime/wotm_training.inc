// Training mode (NATIVO.md, "Runtime: training mode"; WOTM_TRAINING=1 or the menu's TRAINING row): hooks on the EE thread that record what the game tests for
// attacks and the damage of each. Included by wotm_overrides.cpp; shared state in native/wotm_training.h; the body volumes
// are read in wotmCaptureFrame (wotm_live.inc) and the drawing and options panel (pause menu) are in wotm_live.inc. Investigations:
// git show 635fd3c:docs/investigacoes/2026-10-23/treino_hitboxes.md and treino_dano.md.
//
// - Attacks: the game has no hitboxes. Each test is a swept sphere (hdSphereTest, from the bone's point on the previous frame to
//   the current one) against the collision hulls. The hooks mark the "attack test in progress" (punch/combo: collisTestCloseRangeAttack;
//   long-object attack: Pickup::collisTestBatAttack; getup attack: StateGetupAttack::update; projectile: Weapon::WeaponHD; butt slam
//   wave: ShockWave::update) and, only while marked, hdSphereTest (swept and static) records the sphere and whether it hit something.
//   Outside the mark, hdSphereTest belongs to the camera and physics and is left out.
// - Damage: Monster::takeHit keeps the requested damage and the HitEvent's source; Monster::takeDamage measures health before and
//   after (the exact value, with block, rage and difficulty). Fire, shock and hitting the scenery only come through takeDamage.
// - Dummy: AIs (slots >= m_numPlayers) with the pad disabled (Monster +0xF9, setPadEnabled) stand still but still recoil, fall and
//   get grabbed. Refill: full health after 1.5 s without damage, and a killing hit refills health first (nobody dies in training).

#include "wotm_training.h"

namespace wotm_train
{
    using namespace wotm_training;
    constexpr uint32_t kGameObj = wotm::sym::shellGame, kShellObj = wotm::sym::shellInfo;
    constexpr uint32_t kSlots = 0x120380u;             // TheGame::m_monsters (room for 16)
    constexpr uint32_t kMaxHealth = 0x448u, kHealth = 0x44Cu, kPadEnabled = 0xF9u, kGetupState = 0x10C40u;
    constexpr uint32_t kTakeHitInfo = wotm::sym::x_8HitEvent_s_takeHitInfo;   // pos* +0, source +0x10

    constexpr uint32_t kMonsterUpdate = wotm::fn::update__7Monster, kTakeHit = wotm::fn::takeHit__7MonsterP8_fvectorfi,
                       kTakeDamage = wotm::fn::takeDamage__7MonsterfbP7Monster,
                       kPunchTest = wotm::fn::collisTestCloseRangeAttack__20PunchSwipeConfigBaseiP7MonsterR10HitHistoryi,
                       kBatTest = wotm::fn::collisTestBatAttack__6PickupfUi11MonsterAnimR10HitHistoryfff,
                       kGetupUpdate = wotm::fn::update__16StateGetupAttack, kWeaponHD = wotm::fn::WeaponHD__6Weapon,
                       kShockUpdate = wotm::fn::update__9ShockWave, kHookTest = wotm::fn::testCollis__16StateGrappleHook,
                       kHookState = 0x10600u, kSphereSwept = wotm::fn::hdSphereTest__FP8_fvectorT0fP9_hdResultUii,
                       kSphereStatic = wotm::fn::hdSphereTest__FP8_fvectorfP9_hdResultUii, kAiInputs = wotm::fn::updateInputs__2Ai;
    constexpr uint32_t kAiInMonster = 0x4E0u, kGamePad = 0x5024u;   // Ai embedded in the Monster; the Monster's GamePad (one byte per button)
    // attacks without a sphere (fire, cone, body, stomp, grab)
    constexpr uint32_t kFireTest = wotm::fn::TestCollis__10FireBreath, kLos = wotm::fn::hdLosCSTest__FP8_fvectorT0P9_hdResulti,
                       kConeTest = wotm::fn::testConeCollis__14StateSonicRoar, kPunchSearch = wotm::fn::getClosestMonsterToPunch__7Monsterfff,
                       kFovSearch = wotm::fn::getClosestMonsterInFOV__7MonsterR8_fvectorfT1fRf,
                       kEnvTest = wotm::fn::collisTestEnvironmentCollisions__7Monster,
                       kBundle = wotm::fn::hdBundledSphereTest__FP19_hdSphereBundleTestii, kStompUpdate = wotm::fn::update__16StateStompAttack,
                       kStompInto = wotm::fn::transitionInto__16StateStompAttack, kGrabOH = wotm::fn::update__20StateGrappleOHAttack,
                       kMonkeyOH = wotm::fn::update__19StateMonkeyOHAttack;
    constexpr uint32_t kStompIntoSize = 0x54u;          // the stomp's getClosestMonsterToPunch: called from transitionInto
    // the grab's getClosestMonsterInFOV: only StateGrapple::update's (the grab window); transitionFeasible's is the "can I grab?"
    // question the AI and the pad ask every frame
    constexpr uint32_t kGrabUpdate = wotm::fn::update__12StateGrapple, kGrabUpdateSize = 0x390u;
    constexpr uint32_t kRamState = 0xFD40u, kThrownState = 0xE3E0u, kPinTrans = 0x3E30u;

    PS2Runtime::RecompiledFunction g_origUpdate = nullptr, g_origTakeHit = nullptr, g_origTakeDamage = nullptr, g_origPunch = nullptr,
                                   g_origBat = nullptr, g_origGetup = nullptr, g_origWeapon = nullptr, g_origShock = nullptr,
                                   g_origSwept = nullptr, g_origStatic = nullptr, g_origAi = nullptr, g_origFire = nullptr,
                                   g_origLos = nullptr, g_origCone = nullptr, g_origPunchSearch = nullptr, g_origFovSearch = nullptr,
                                   g_origEnv = nullptr, g_origBundle = nullptr, g_origStomp = nullptr, g_origGrabOH = nullptr,
                                   g_origMonkeyOH = nullptr, g_origHook = nullptr;

    // EE thread state
    int g_ctxKind = -1;         // attack test in progress (Kind) or -1
    int g_ctxOwner = -1;
    bool g_ctxHit = false;      // the test in progress led to a takeHit/damage (SonicRoar cone)
    uint32_t g_curMonster = 0;  // Monster::update in progress
    bool g_inStatic = false;
    struct HitCtx
    {
        uint32_t victim = 0;
        float base = 0.f;
        int source = -1;
        int cls = 0;   // class of the victim's state on entry to takeHit (2 = already in a reaction: the hit extends the combo)
    } g_hitCtx;
    uint32_t g_lastDamage[16] = {};   // g_frame of the last damage per slot
    uint32_t g_lastHit[16] = {};      // g_frame of the last takeHit per slot (also counts a blocked hit with no damage)
    uint32_t g_padOff = 0;            // slots whose pad we disabled

    // ---- frame data: Monster states by the offset of m_state (+0x34) within it (table from include/game/game.h)
    // cls: 0 other, 1 attack, 2 reaction (the victim can't act)
    struct StateName { uint32_t off; const char *name; uint8_t cls; };
    constexpr StateName kStates[] = {
        {0x7984, "Idle", 0}, {0x79AC, "Run", 0}, {0x7A90, "Dash", 0}, {0x7AD4, "Jump", 0}, {0x7DA0, "Block", 0},
        {0x7DD8, "Counter", 1}, {0x7E30, "Recoil", 2}, {0x7EA0, "KnockBack", 2}, {0x8450, "Death", 0}, {0x8470, "Punch", 1},
        {0xAE90, "BatSwipe", 1}, {0xDC70, "PickUp", 0}, {0xDCE0, "Grapple", 0}, {0xDD54, "GrappleLift", 1}, {0xDD74, "GrappleThrow", 1},
        {0xDDA0, "GrappleThrow1", 1}, {0xDDCC, "Grappled", 2}, {0xDE30, "BeingSlammed", 2}, {0xE3E0, "BeingThrown", 2},
        {0xE990, "GrappleBreak", 0}, {0xEF40, "Throw", 1}, {0xEFE0, "Climb", 0}, {0xF0B0, "Fly", 0}, {0xF3F0, "CrowdControl", 1},
        {0xF6F4, "SonicRoar", 1}, {0xF974, "FireBreath", 1}, {0xF9D8, "BugAttack", 1}, {0xFA1C, "ButtSlam", 1}, {0xFD40, "RamAttack", 1},
        {0xFE20, "Taunt", 0}, {0xFE38, "Javelin", 1}, {0xFE5C, "RobotSpecial", 1}, {0x102A8, "RockSpecial", 1}, {0x1059C, "ZapAttack", 1},
        {0x10600, "GrappleHook", 1}, {0x106E0, "Impaled", 2}, {0x10714, "Stunned", 2}, {0x1074C, "TwoHandedThrow", 1},
        {0x10790, "AirStrike", 1}, {0x10930, "LavaBlast", 1}, {0x10AE0, "GrappleOHAttack", 1}, {0x10B34, "MonkeyOHAttack", 1},
        {0x10BA0, "Shocked", 2}, {0x10BF0, "Catch", 0}, {0x10C40, "GetupAttack", 1}, {0x10CF0, "StompAttack", 1},
        {0x10D50, "TazerAttack", 1}, {0x10D90, "ShieldAttack", 1}, {0x10E70, "Victory", 0}, {0x10E90, "GunAttack", 1},
        {0x10EBC, "TopSpin", 1}, {0x10F60, "CannonHands", 1}, {0x11100, "BigTakeHit", 2}, {0x11114, "Countered", 2},
        {0x11130, "UltraTazer", 1},
    };
    const StateName *stateName(uint32_t off)
    {
        for (const StateName &s : kStates)
            if (s.off == off)
                return &s;
        return nullptr;
    }
    constexpr uint32_t kPunch = 0x8470, kBat = 0xAE90;

    struct Track
    {
        uint32_t off = 0, start = 0, lastActive = 0, firstActive = 0;
        int chain = -1, step = -1, active = 0, hitVictim = -1;
        bool blocked = false;   // in recoil: the hit was blocked
    } g_track[16];
    constexpr uint32_t kRecoilState = 0x7E30u, kRecoilBlocked = kRecoilState + 0x64u;
    uint32_t g_activeFrame[16] = {};   // last frame with an attack test for the slot
    struct HitMark
    {
        uint32_t frame = 0;
        int8_t victim = -1;   // first damaged victim since the last check
    } g_hitMark[16];

    inline bool ok(uint32_t a, uint32_t n) { return a >= 0x1000u && a + n <= PS2_RAM_SIZE; }
    inline uint32_t rd32(const uint8_t *rdram, uint32_t a) { uint32_t v; std::memcpy(&v, rdram + a, 4); return v; }
    inline float rdf(const uint8_t *rdram, uint32_t a) { float v; std::memcpy(&v, rdram + a, 4); return v; }
    inline void wrf(uint8_t *rdram, uint32_t a, float v) { std::memcpy(rdram + a, &v, 4); }
    inline uint32_t frameNow(const uint8_t *rdram) { return rd32(rdram, wotm::sym::g_frame); }

    int slotOf(const uint8_t *rdram, uint32_t m)
    {
        m &= 0x1FFFFFFFu;
        if (!m)
            return -1;
        for (uint32_t i = 0; i < 16; ++i)
            if ((rd32(rdram, kGameObj + kSlots + 4 * i) & 0x1FFFFFFFu) == m)
                return int(i);
        return -1;
    }

    int numPlayers(const uint8_t *rdram)
    {
        const int32_t p = static_cast<int32_t>(rd32(rdram, kShellObj + 0x2BB4));   // m_numPlayers
        return p >= 1 && p <= 4 ? p : 1;
    }

    // the function finished (wasn't interrupted midway by the scheduler): pc is back at the entry ra
    inline bool returned(R5900Context *ctx, uint32_t ra) { return ctx->pc == ra; }

    // ---- attacks

    uint32_t g_kindLog = 0;   // kinds already announced in the log (one notice per kind)
    void logKind(int kind)
    {
        static const char *const names[KKinds] = {"punch", "object", "getup", "projectile", "wave", "hook", "other", "fire", "body", "stomp", "grab", "cone"};
        if (kind < 0 || kind >= KKinds || (g_kindLog & (1u << kind)))
            return;
        g_kindLog |= 1u << kind;
        std::fprintf(stderr, "[training] first attack test of kind %s\n", names[kind]);
    }

    void pushSweepK(const uint8_t *rdram, const float a[3], const float b[3], float r, uint8_t hit, int kind, int owner)
    {
        if (!std::isfinite(a[0] + a[1] + a[2] + b[0] + b[1] + b[2] + r) || r < 0.f || r > 5000.f)
            return;
        Sweep s{};
        std::memcpy(s.a, a, sizeof s.a);
        std::memcpy(s.b, b, sizeof s.b);
        s.r = r;
        s.frame = frameNow(rdram);
        s.kind = uint8_t(kind);
        s.owner = int8_t(owner);
        s.hit = hit;
        if (owner >= 0 && owner < 16)
            g_activeFrame[owner] = s.frame;   // counted in the state the monster ends the frame in (trackState)
        logKind(kind);
        Shared &sh = shared();
        std::lock_guard<std::mutex> lk(sh.m);
        if (sh.sweeps.size() < 4096)
            sh.sweeps.push_back(s);
    }
    void pushSweep(const uint8_t *rdram, const float a[3], const float b[3], float r, uint8_t hit)
    {
        pushSweepK(rdram, a, b, r, hit, g_ctxKind, g_ctxOwner);
    }

    void pushCone(const uint8_t *rdram, const float apex[3], const float axis[3], const float up[3], float len, float cosHalf, bool flat,
                  uint8_t hit, int kind, int owner)
    {
        const float al = std::sqrt(axis[0] * axis[0] + axis[1] * axis[1] + axis[2] * axis[2]);
        if (!std::isfinite(apex[0] + apex[1] + apex[2] + al + len + cosHalf) || al < 1e-4f || len <= 0.f || len > 20000.f)
            return;
        Cone c{};
        std::memcpy(c.apex, apex, sizeof c.apex);
        for (int k = 0; k < 3; ++k)
        {
            c.axis[k] = axis[k] / al;
            c.up[k] = up[k];
        }
        c.len = len;
        c.cosHalf = std::clamp(cosHalf, -1.f, 1.f);
        c.frame = frameNow(rdram);
        c.kind = uint8_t(kind);
        c.owner = int8_t(owner);
        c.hit = hit;
        c.flat = flat;
        if (owner >= 0 && owner < 16)
            g_activeFrame[owner] = c.frame;
        logKind(kind);
        Shared &sh = shared();
        std::lock_guard<std::mutex> lk(sh.m);
        if (sh.cones.size() < 512)
            sh.cones.push_back(c);
    }
    // only sphere attack tests record through hdSphereTest (cone, body and search tests record their own shape)
    inline bool sphereCtx() { return g_ctxKind >= 0 && g_ctxKind <= KOther; }

    uint8_t classify(const uint8_t *rdram, uint32_t res, int n)
    {
        uint8_t h = 0;
        res &= 0x1FFFFFFFu;
        for (int i = 0; i < n && i < 100 && ok(res + 0x30u * uint32_t(i), 0x30u); ++i)
        {
            const uint32_t r = res + 0x30u * uint32_t(i);
            if (!rdram[r])   // _hdResult::hit
                continue;
            h = std::max<uint8_t>(h, (rd32(rdram, r + 4) & 0x7FFu) ? 2 : 1);
        }
        return h;
    }

    void readVec(const uint8_t *rdram, uint32_t p, float out[3])
    {
        p &= 0x1FFFFFFFu;
        if (!ok(p, 12)) { out[0] = out[1] = out[2] = NAN; return; }
        for (int i = 0; i < 3; ++i)
            out[i] = rdf(rdram, p + 4 * i);
    }

    // hdSphereTest(from, to, r, res, skipId, max): swept sphere
    void sphereSwept(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != kSphereSwept || !sphereCtx() || g_inStatic || !g_on.load(std::memory_order_relaxed))
        {
            g_origSwept(rdram, ctx, runtime);
            return;
        }
        float a[3], b[3];
        readVec(rdram, GPR_U32(ctx, 4), a);
        readVec(rdram, GPR_U32(ctx, 5), b);
        const float r = ctx->f[12];
        const uint32_t res = GPR_U32(ctx, 6), ra = GPR_U32(ctx, 31);
        g_origSwept(rdram, ctx, runtime);
        const int n = returned(ctx, ra) ? int(GPR_S32(ctx, 2)) : 0;
        pushSweep(rdram, a, b, r, classify(rdram, res, n));
    }

    // hdSphereTest(pos, r, res, skipId, max): static sphere
    void sphereStatic(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != kSphereStatic || !sphereCtx() || !g_on.load(std::memory_order_relaxed))
        {
            g_origStatic(rdram, ctx, runtime);
            return;
        }
        float a[3];
        readVec(rdram, GPR_U32(ctx, 4), a);
        const float r = ctx->f[12];
        const uint32_t res = GPR_U32(ctx, 5), ra = GPR_U32(ctx, 31);
        g_inStatic = true;
        g_origStatic(rdram, ctx, runtime);
        g_inStatic = false;
        const int n = returned(ctx, ra) ? int(GPR_S32(ctx, 2)) : 0;
        pushSweep(rdram, a, a, r, classify(rdram, res, n));
    }

    // marks the attack test in progress during the original call
    void withCtx(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, PS2Runtime::RecompiledFunction orig, int kind, int owner)
    {
        const int savedKind = g_ctxKind, savedOwner = g_ctxOwner;
        g_ctxKind = kind;
        g_ctxOwner = owner;
        orig(rdram, ctx, runtime);
        g_ctxKind = savedKind;
        g_ctxOwner = savedOwner;
    }

    void punchTest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)   // a2 = attacking Monster
    {
        if (ctx->pc != kPunchTest || !g_on.load(std::memory_order_relaxed)) { g_origPunch(rdram, ctx, runtime); return; }
        withCtx(rdram, ctx, runtime, g_origPunch, KPunch, slotOf(rdram, GPR_U32(ctx, 6)));
    }
    void batTest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)     // inside the holder's Monster::update
    {
        if (ctx->pc != kBatTest || !g_on.load(std::memory_order_relaxed)) { g_origBat(rdram, ctx, runtime); return; }
        withCtx(rdram, ctx, runtime, g_origBat, KBat, slotOf(rdram, g_curMonster));
    }
    void getupUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)  // a0 = the state, inside the Monster
    {
        if (ctx->pc != kGetupUpdate || !g_on.load(std::memory_order_relaxed)) { g_origGetup(rdram, ctx, runtime); return; }
        withCtx(rdram, ctx, runtime, g_origGetup, KGetup, slotOf(rdram, GPR_U32(ctx, 4) - kGetupState));
    }
    void weaponHD(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != kWeaponHD || !g_on.load(std::memory_order_relaxed)) { g_origWeapon(rdram, ctx, runtime); return; }
        withCtx(rdram, ctx, runtime, g_origWeapon, KWeapon, -1);
    }
    // StateGrappleHook::testCollis (a0 = the state, Monster +0x10600): the only hdSphereTest of Ultra-V's hook, radius 0 (a line) from the
    // tip's previous point to its current one (+0x90 -> +0xA0)
    void hookTest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != kHookTest || !g_on.load(std::memory_order_relaxed)) { g_origHook(rdram, ctx, runtime); return; }
        withCtx(rdram, ctx, runtime, g_origHook, KHook, slotOf(rdram, (GPR_U32(ctx, 4) & 0x1FFFFFFFu) - kHookState));
    }
    void shockUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)  // +0x20: the owner (Monster), when it is one of the slots
    {
        if (ctx->pc != kShockUpdate || !g_on.load(std::memory_order_relaxed)) { g_origShock(rdram, ctx, runtime); return; }
        const uint32_t self = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
        withCtx(rdram, ctx, runtime, g_origShock, KShock, ok(self, 0x24) ? slotOf(rdram, rd32(rdram, self + 0x20)) : -1);
    }

    // ---- attacks without a sphere: fire, cone, body, stomp and grab

    // offset of the current state (m_state, +0x34) within the Monster; 0 if outside
    uint32_t stateOff(const uint8_t *rdram, uint32_t m)
    {
        const uint32_t st = rd32(rdram, m + 0x34) & 0x1FFFFFFFu;
        return st > m && st - m < 0x11190u ? st - m : 0;
    }
    // the monster's _cs (+0xC): translation +0x10, matrix rows +0x20 (x), +0x30 (forward), +0x40 (up)
    bool monsterCs(const uint8_t *rdram, uint32_t m, float trans[3], float fwd[3], float up[3])
    {
        const uint32_t cs = rd32(rdram, m + 0xC) & 0x1FFFFFFFu;
        if (!ok(cs, 0x50))
            return false;
        readVec(rdram, cs + 0x10, trans);
        readVec(rdram, cs + 0x30, fwd);
        readVec(rdram, cs + 0x40, up);
        return true;
    }

    // FireBreath::TestCollis (a0 = the FireBreath, +4 the Monster): a line of sight from the mouth out to the reach (hdLosCSTest)
    void fireTest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != kFireTest || !g_on.load(std::memory_order_relaxed)) { g_origFire(rdram, ctx, runtime); return; }
        const uint32_t self = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
        withCtx(rdram, ctx, runtime, g_origFire, KFire, ok(self, 8) ? slotOf(rdram, rd32(rdram, self + 4)) : -1);
    }
    // hdLosCSTest(from, to, res, skipId): only for fire and cone, the line (radius 0) and whether it hit scenery or a monster
    void losTest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != kLos || (g_ctxKind != KFire && g_ctxKind != KCone) || !g_on.load(std::memory_order_relaxed))
        {
            g_origLos(rdram, ctx, runtime);
            return;
        }
        float a[3], b[3];
        readVec(rdram, GPR_U32(ctx, 4), a);
        readVec(rdram, GPR_U32(ctx, 5), b);
        const uint32_t res = GPR_U32(ctx, 6), ra = GPR_U32(ctx, 31);
        g_origLos(rdram, ctx, runtime);
        pushSweep(rdram, a, b, 0.f, returned(ctx, ra) ? classify(rdram, res, 1) : 0);
    }

    // StateSonicRoar::testConeCollis (a0 = the state, +0xC the Monster): cone from the mouth (+0x3E50) along axis +0x33D0, reach +0x264
    // (1000 on Monster Island with the crater swapped, like the game), cosine of the half-angle +0x268; red if someone was hit
    void coneTest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
        if (ctx->pc != kConeTest || !g_on.load(std::memory_order_relaxed) || !ok(self, 0x270))
        {
            g_origCone(rdram, ctx, runtime);
            return;
        }
        const uint32_t m = rd32(rdram, self + 0xC) & 0x1FFFFFFFu;
        float apex[3], axis[3], trans[3], fwd[3], up[3] = {0.f, 0.f, 1.f};
        const bool have = ok(m, 0x3E60) && monsterCs(rdram, m, trans, fwd, up);
        readVec(rdram, m + 0x3E50, apex);
        readVec(rdram, m + 0x33D0, axis);
        float len = rdf(rdram, self + 0x264);
        if (rd32(rdram, kGameObj + 0x1203C8u) == 1 && rd32(rdram, kGameObj + 0x1203D0u) == 6 && rd32(rdram, wotm::sym::craterSwitched))
            len = 1000.f;
        const float cosHalf = rdf(rdram, self + 0x268);
        const int owner = slotOf(rdram, m);
        const bool savedHit = g_ctxHit;
        g_ctxHit = false;
        withCtx(rdram, ctx, runtime, g_origCone, KCone, owner);
        const bool hit = g_ctxHit;
        g_ctxHit = savedHit;
        if (have)
            pushCone(rdram, apex, axis, up, len, cosHalf, false, hit ? 2 : 0, KCone, owner);
    }

    // getClosestMonsterToPunch(maxDist f12, angle f13, reach f14): for the stomp (called from StateStompAttack::transitionInto) the
    // victim search, a sector on the ground from the monster, in front of it, half-angle angle/2, radius reach
    void punchSearch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = GPR_U32(ctx, 31), m = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
        if (ctx->pc != kPunchSearch || !g_on.load(std::memory_order_relaxed) || ra < kStompInto || ra >= kStompInto + kStompIntoSize ||
            !ok(m, 0x450))
        {
            g_origPunchSearch(rdram, ctx, runtime);
            return;
        }
        const float angle = ctx->f[13], reach = ctx->f[14];
        float trans[3], fwd[3], up[3];
        const bool have = monsterCs(rdram, m, trans, fwd, up);
        g_origPunchSearch(rdram, ctx, runtime);
        if (have && returned(ctx, ra))
            pushCone(rdram, trans, fwd, up, reach, std::cos(angle * 0.5f), true, GPR_U32(ctx, 2) ? 2 : 0, KStomp, slotOf(rdram, m));
    }

    // getClosestMonsterInFOV(from a1, maxDist f12, eye a2, cosAngle f13, distSq a3), only from StateGrapple::update: the victim's aim
    // point within maxDist of `from` (the sphere) and inside the cone seen from `eye` around the monster's forward (the cone, to the
    // sphere's far end)
    void fovSearch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = GPR_U32(ctx, 31), m = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
        if (ctx->pc != kFovSearch || !g_on.load(std::memory_order_relaxed) || !ok(m, 0x450) || ra < kGrabUpdate ||
            ra >= kGrabUpdate + kGrabUpdateSize)
        {
            g_origFovSearch(rdram, ctx, runtime);
            return;
        }
        float from[3], eye[3], trans[3], fwd[3], up[3];
        readVec(rdram, GPR_U32(ctx, 5), from);
        readVec(rdram, GPR_U32(ctx, 6), eye);
        const float maxDist = ctx->f[12], cosAngle = ctx->f[13];
        const bool have = monsterCs(rdram, m, trans, fwd, up);
        g_origFovSearch(rdram, ctx, runtime);
        if (!have || !returned(ctx, ra))
            return;
        const uint8_t hit = GPR_U32(ctx, 2) ? 2 : 0;
        const int owner = slotOf(rdram, m);
        const float dx = from[0] - eye[0], dy = from[1] - eye[1], dz = from[2] - eye[2];
        pushCone(rdram, eye, fwd, up, std::sqrt(dx * dx + dy * dy + dz * dz) + maxDist, cosAngle, false, hit, KGrab, owner);
        pushSweepK(rdram, from, from, maxDist, hit, KGrab, owner);
    }

    // collisTestEnvironmentCollisions (a0 = Monster): during a ram attack and for a thrown monster, the body spheres are the attack
    void envTest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
        const uint32_t st = ctx->pc == kEnvTest && g_on.load(std::memory_order_relaxed) && ok(m, 0x450) ? stateOff(rdram, m) : 0;
        if (st != kRamState && st != kThrownState)
        {
            g_origEnv(rdram, ctx, runtime);
            return;
        }
        withCtx(rdram, ctx, runtime, g_origEnv, KBody, slotOf(rdram, m));
    }
    // hdBundledSphereTest(tests a0, n a1, skipId a2): 0x18-byte tests {from*, to*, radius, max, results*, count}
    void bundleTest(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t tests = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
        const int n = int(GPR_S32(ctx, 5));
        if (ctx->pc != kBundle || g_ctxKind != KBody || n <= 0 || n > 16 || !ok(tests, 0x18u * uint32_t(n)))
        {
            g_origBundle(rdram, ctx, runtime);
            return;
        }
        float a[16][3], b[16][3], r[16];
        for (int i = 0; i < n; ++i)
        {
            const uint32_t t = tests + 0x18u * uint32_t(i);
            readVec(rdram, rd32(rdram, t), a[i]);
            readVec(rdram, rd32(rdram, t + 4), b[i]);
            r[i] = rdf(rdram, t + 8);
        }
        const uint32_t ra = GPR_U32(ctx, 31);
        g_origBundle(rdram, ctx, runtime);
        const bool done = returned(ctx, ra);
        for (int i = 0; i < n; ++i)
        {
            const uint32_t t = tests + 0x18u * uint32_t(i);
            pushSweep(rdram, a[i], b[i], r[i], done ? classify(rdram, rd32(rdram, t + 0x10), int(rd32(rdram, t + 0x14))) : 0);
        }
    }

    // StateStompAttack::update, StateGrappleOHAttack::update, StateMonkeyOHAttack::update (inside the attacker's Monster::update): the
    // damage goes straight to the victim's takeDamage, which gets a marker (takeDamage, below)
    void stompUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != kStompUpdate || !g_on.load(std::memory_order_relaxed)) { g_origStomp(rdram, ctx, runtime); return; }
        withCtx(rdram, ctx, runtime, g_origStomp, KStomp, slotOf(rdram, g_curMonster));
    }
    void grabOHUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != kGrabOH || !g_on.load(std::memory_order_relaxed)) { g_origGrabOH(rdram, ctx, runtime); return; }
        withCtx(rdram, ctx, runtime, g_origGrabOH, KGrab, slotOf(rdram, g_curMonster));
    }
    void monkeyOHUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != kMonkeyOH || !g_on.load(std::memory_order_relaxed)) { g_origMonkeyOH(rdram, ctx, runtime); return; }
        withCtx(rdram, ctx, runtime, g_origMonkeyOH, KGrab, slotOf(rdram, g_curMonster));
    }

    // ---- damage and dummy

    // class of the monster's current state (kStates' cls; 0 if unnamed)
    int stateCls(const uint8_t *rdram, uint32_t m)
    {
        const uint32_t st = rd32(rdram, m + 0x34) & 0x1FFFFFFFu;
        const StateName *sn = st > m && st - m < 0x11190u ? stateName(st - m) : nullptr;
        return sn ? sn->cls : 0;
    }

    void takeHit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != kTakeHit || !g_on.load(std::memory_order_relaxed)) { g_origTakeHit(rdram, ctx, runtime); return; }
        const HitCtx saved = g_hitCtx;
        g_hitCtx.victim = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
        g_hitCtx.base = ctx->f[12];
        g_hitCtx.source = static_cast<int32_t>(rd32(rdram, kTakeHitInfo + 0x10u));
        g_hitCtx.cls = ok(g_hitCtx.victim, 0x450u) ? stateCls(rdram, g_hitCtx.victim) : 0;
        if (const int vs = slotOf(rdram, g_hitCtx.victim); vs >= 0)
            g_lastHit[vs] = frameNow(rdram);
        if (g_ctxKind == KCone)
            g_ctxHit = true;
        // frame data: a hit that connected counts even with no damage (blocked), so the advantage on block shows up. The
        // attacker: the owner of the attack test in progress, or the monster whose update is running
        if (const int vs = slotOf(rdram, g_hitCtx.victim); vs >= 0)
        {
            const int att = g_ctxOwner >= 0 ? g_ctxOwner : slotOf(rdram, g_curMonster);
            if (att >= 0 && att < 16 && att != vs && g_hitMark[att].victim < 0)
                g_hitMark[att] = HitMark{frameNow(rdram), int8_t(vs)};
        }
        g_origTakeHit(rdram, ctx, runtime);
        g_hitCtx = saved;
    }

    void takeDamage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)   // a0 victim, f12 damage, a1 force, a2 source (Monster*)
    {
        const uint32_t m = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
        const int slot = ctx->pc == kTakeDamage && g_on.load(std::memory_order_relaxed) && ok(m, 0x450u) ? slotOf(rdram, m) : -1;
        if (slot < 0)
        {
            g_origTakeDamage(rdram, ctx, runtime);
            return;
        }
        const float dmg = ctx->f[12], maxH = rdf(rdram, m + kMaxHealth);
        float before = rdf(rdram, m + kHealth);
        // refill: a hit that would kill refills health first (F1's "win the fight", 100000, goes through); 1.35 covers hard difficulty
        if ((g_opts.load(std::memory_order_relaxed) & kRefill) && dmg < 10000.f && maxH > 0.f && before <= dmg * 1.35f + 0.5f)
        {
            wrf(rdram, m + kHealth, maxH);
            before = maxH;
        }
        const uint32_t ra = GPR_U32(ctx, 31), src = GPR_U32(ctx, 6);
        const int clsBefore = stateCls(rdram, m);
        g_origTakeDamage(rdram, ctx, runtime);
        if (!returned(ctx, ra))
            return;
        const float after = rdf(rdram, m + kHealth);
        if (!std::isfinite(before) || !std::isfinite(after) || before - after < 0.001f)
            return;
        if (g_ctxKind == KCone)
            g_ctxHit = true;
        if (g_ctxKind == KStomp || g_ctxKind == KGrab)
        {   // stomp and grab have no geometry: a red sphere at the victim's aim point (+0x3E30) marks the hit that took health
            float p[3];
            readVec(rdram, m + kPinTrans, p);
            pushSweep(rdram, p, p, 25.f, 2);
        }
        Hit h{};
        h.frame = frameNow(rdram);
        h.victim = int8_t(slot);
        h.attacker = int8_t(slotOf(rdram, src));
        const bool inHit = g_hitCtx.victim == m;
        h.base = inHit ? g_hitCtx.base : -1.f;
        h.source = inHit ? g_hitCtx.source : -1;
        h.applied = before - after;
        h.before = before;
        h.max = maxH;
        g_lastDamage[slot] = h.frame;
        if (h.attacker >= 0 && h.attacker != slot && g_hitMark[h.attacker].victim < 0)
            g_hitMark[h.attacker] = HitMark{h.frame, int8_t(slot)};
        static uint32_t logged = 0;
        if (++logged <= 30 || logged % 100 == 0)
            std::fprintf(stderr, "[training] damage %.2f on slot %d (from %d; requested %.2f, source %d), health %.1f -> %.1f of %.0f (%u)\n", h.applied,
                         slot, h.attacker, h.base, h.source, before, after, maxH, logged);
        Shared &sh = shared();
        std::lock_guard<std::mutex> lk(sh.m);
        // continuous damage (fire, shock: a little per frame, no takeHit) becomes a single record while it continues
        bool merged = false;
        if (!inHit && !sh.hits.empty())
        {
            Hit &p = sh.hits.back();
            if (p.source < 0 && p.victim == h.victim && p.attacker == h.attacker && h.frame - p.frame <= 3u)
            {
                p.applied += h.applied;
                p.frame = h.frame;
                merged = true;
            }
        }
        // combo: continues if the victim was still in a reaction when the hit landed (or the damage came within 3 frames of the previous)
        Combo &c = sh.combo;
        const int cls = inHit ? g_hitCtx.cls : clsBefore;
        if (!(c.victim == h.victim && c.hits > 0 && (cls == 2 || h.frame - c.last <= 3u)))
        {
            c = Combo{};
            c.victim = h.victim;
            c.attacker = h.attacker;
            c.start = h.frame;
        }
        if (!merged)
            ++c.hits;
        c.damage += h.applied;
        c.last = h.frame;
        if (c.damage > sh.bestCombo)
        {
            sh.bestCombo = c.damage;
            sh.bestHits = c.hits;
        }
        if (!merged && sh.hits.size() < 1024)
            sh.hits.push_back(h);
    }

    // On exit from Monster::update: the state the monster ends frame f in. A change (or another combo step) closes the previous
    // state's record (which lasted until frame f - 1) and opens the new one at f; the frame's attack test and damage count for the new one.
    void trackState(const uint8_t *rdram, uint32_t m, int slot)
    {
        const uint32_t st = rd32(rdram, m + 0x34) & 0x1FFFFFFFu;
        const uint32_t off = st > m && st - m < 0x11190u ? st - m : 0;
        int chain = -1, step = -1;
        if (off == kPunch)
        {
            chain = int(rd32(rdram, m + kPunch + 0x2988));
            step = int(rd32(rdram, m + kPunch + 0x298C));
        }
        else if (off == kBat)
        {
            chain = int(rd32(rdram, m + kBat + 0x2D48));
            step = int(rd32(rdram, m + kBat + 0x2D4C));
        }
        Track &t = g_track[slot];
        const uint32_t f = frameNow(rdram);
        // the frame's activity (spheres, damage dealt) in the current state
        auto absorb = [&](Track &tr) {
            if (off == kRecoilState && rd32(rdram, m + kRecoilBlocked))
                tr.blocked = true;
            if (g_activeFrame[slot] == f && tr.lastActive != f)
            {
                ++tr.active;
                tr.lastActive = f;
                if (!tr.firstActive)
                    tr.firstActive = f;
            }
            if (g_hitMark[slot].victim >= 0)
            {
                if (tr.hitVictim < 0)
                    tr.hitVictim = g_hitMark[slot].victim;
                g_hitMark[slot] = HitMark{};
            }
        };
        if (t.off == off && t.chain == chain && t.step == step && t.start && f >= t.start)
        {
            absorb(t);
            return;
        }
        if (t.start && f > t.start)
        {
            const StateName *sn = stateName(t.off);
            const bool attack = (sn && sn->cls == 1) || t.active > 0;
            Shared &sh = shared();
            std::lock_guard<std::mutex> lk(sh.m);
            if (attack)
            {
                MoveRec r{};
                r.owner = int8_t(slot);
                r.name = sn ? sn->name : "?";
                r.chain = int8_t(t.chain);
                r.step = int8_t(t.step);
                r.start = t.start;
                r.end = f;
                r.startup = t.firstActive ? int(t.firstActive - t.start) + 1 : 0;
                r.active = t.active;
                r.hitVictim = int8_t(t.hitVictim);
                static uint32_t nlog = 0;
                if (++nlog <= 40)
                    std::fprintf(stderr, "[training] attack slot %d %s c%d s%d: startup %d, active %d, total %u, victim %d\n", slot, r.name, r.chain,
                                 r.step, r.startup, r.active, r.end - r.start, r.hitVictim);
                sh.moves.push_back(r);
                if (sh.moves.size() > 64)
                    sh.moves.erase(sh.moves.begin());
            }
            else if (sn && sn->cls == 2)
            {
                static uint32_t nlog = 0;
                if (++nlog <= 40)
                    std::fprintf(stderr, "[training] reaction slot %d %s%s: %u frames (%u..%u)\n", slot, sn->name,
                                 t.blocked ? " (blocked)" : "", f - t.start, t.start, f);
                sh.stuns.push_back(StunRec{int8_t(slot), t.blocked ? "BLOCKSTUN" : sn->name, t.start, f, t.blocked});
                if (sh.stuns.size() > 64)
                    sh.stuns.erase(sh.stuns.begin());
            }
        }
        t = Track{};
        t.off = off;
        t.chain = chain;
        t.step = step;
        t.start = f;
        absorb(t);
    }

    // The frame's inputs, on exit from Monster::update: the GamePad (+0x5024) still holds what interpretInputs read (clearInputs runs at
    // the start of the next update; toggled lock-on restores the original bytes on exit). D-pad/left stick in 0xC..0xF
    // (left, right, forward, back; relative to the monster under normal controls).
    void recordPad(const uint8_t *rdram, uint32_t m, int idx)
    {
        const uint8_t *gp = rdram + m + kGamePad;
        constexpr uint8_t kOn = 0x50;
        const int x = (gp[0xD] > kOn) - (gp[0xC] > kOn), y = (gp[0xE] > kOn) - (gp[0xF] > kOn);
        const uint8_t dir = uint8_t(5 + x + 3 * y);
        uint16_t btn = 0;
        if (gp[0] > kOn) btn |= kBtnL1;
        if (gp[1] > kOn) btn |= kBtnL2;
        if (gp[3] > kOn) btn |= kBtnR1;
        if (gp[4] > kOn) btn |= kBtnR2;
        if (gp[6] > kOn) btn |= kBtnX;
        if (gp[7] > kOn) btn |= kBtnCircle;
        if (gp[8] > kOn) btn |= kBtnTriangle;
        if (gp[9] > kOn) btn |= kBtnSquare;
        Shared &sh = shared();
        std::lock_guard<std::mutex> lk(sh.m);
        std::vector<PadRec> &h = sh.pad[idx];
        if (!h.empty() && h.back().dir == dir && h.back().btn == btn)
        {
            if (h.back().frames < 999u)
                ++h.back().frames;
            return;
        }
        h.push_back(PadRec{dir, btn, 1});
        if (h.size() > 24)
            h.erase(h.begin());
    }

    // ---- punch table (StatePunch configs, the .CHR values already in RAM)
    constexpr uint32_t kAnims = 0x1CF0u, kNumAnims = 0x12Cu;   // Monster::m_anims (one 0x10 _animHandle per MonsterAnim)
    constexpr uint32_t kPunchCfgs = 0x20u, kTierStride = 0x420u, kTypeStride = 0xB0u, kFistDefault = 0x2980u, kPunchTier = 0x2988u,
                       kPunchType = 0x298Cu, kPunchInWindow = 0x2A00u;

    PunchCfg readPunchCfg(const uint8_t *rdram, uint32_t m, int tier, int type)
    {
        PunchCfg p;
        if (tier < 0 || tier >= kPunchTiers || type < 0 || type >= kPunchTypes)
            return p;
        const uint32_t c = m + kPunch + kPunchCfgs + uint32_t(tier) * kTierStride + uint32_t(type) * kTypeStride;
        p.anim = int(rd32(rdram, c));
        p.ini = rdf(rdram, c + 0x0C);
        p.fim = rdf(rdram, c + 0x10);
        p.kb = rdf(rdram, c + 0x64);
        p.stun = rdf(rdram, c + 0x6C);
        p.radius = rdf(rdram, c + 0x74);
        if (!(p.radius > 0.f))
            p.radius = rdf(rdram, m + kPunch + kFistDefault);
        p.loc[0] = int(rd32(rdram, c + 0x7C));
        p.loc[1] = int(rd32(rdram, c + 0x80));
        p.dmg = rdf(rdram, c + 0x90);
        // the same rule as chr_dump.py: no animation and no damage = empty cell
        p.valid = std::isfinite(p.ini + p.fim + p.dmg + p.radius) && !(p.anim <= 0 && p.dmg == 0.f);
        return p;
    }

    // Object attacks (StateBatSwipe, Monster +0xAE90): configs at +0x20 + pos*0x480 + type*0xC0 (0xC0 SwipeConfig, sharing the start
    // of PunchConfig's layout), pos +0x2D48, type +0x2D4C, inCollisionWindow +0x2DC0. The type comes from the same curMappedAction as
    // the punch (StateBatSwipe::transitionOK). The damage is a multiplier on the object's damage (LevelPickups::s_info): +0x90 when the
    // object is a bat (Pickup +0xA0 == 8, collisTestBatAttack), +0xB0 for the others (getDamageMod, via collisTestCloseRangeAttack).
    constexpr uint32_t kSwipeCfgs = 0x20u, kSwipeTierStride = 0x480u, kSwipeTypeStride = 0xC0u, kSwipeTier = 0x2D48u, kSwipeType = 0x2D4Cu,
                       kSwipeInWindow = 0x2DC0u, kMonsterPickup = 0x68A4u, kPickupKind = 0xA0u, kPickupKindBat = 8u;

    // the Pickup the monster holds (m_pickup points to the list node whose first field is the Pickup*), or 0
    uint32_t heldPickup(const uint8_t *rdram, uint32_t m)
    {
        const uint32_t node = rd32(rdram, m + kMonsterPickup) & 0x1FFFFFFFu;
        if (!ok(node, 4u))
            return 0;
        const uint32_t p = rd32(rdram, node) & 0x1FFFFFFFu;
        return ok(p, kPickupKind + 4u) ? p : 0;
    }

    PunchCfg readSwipeCfg(const uint8_t *rdram, uint32_t m, int tier, int type, bool bat)
    {
        PunchCfg p;
        if (tier < 0 || tier >= kPunchTiers || type < 0 || type >= kPunchTypes)
            return p;
        const uint32_t c = m + kBat + kSwipeCfgs + uint32_t(tier) * kSwipeTierStride + uint32_t(type) * kSwipeTypeStride;
        p.weapon = true;
        p.anim = int(rd32(rdram, c));
        p.ini = rdf(rdram, c + 0x0C);
        p.fim = rdf(rdram, c + 0x10);
        p.kb = rdf(rdram, c + 0x64);
        p.stun = rdf(rdram, c + 0x6C);
        p.radius = rdf(rdram, c + 0x74);
        p.loc[0] = int(rd32(rdram, c + 0x7C));
        p.dmg = rdf(rdram, c + (bat ? 0x90u : 0xB0u));
        p.valid = std::isfinite(p.ini + p.fim + p.dmg + p.radius) && !(p.anim <= 0 && p.dmg == 0.f);
        return p;
    }

    // animationGetCurrentPercent(&m_anims[anim]) (a0 is the _animHandle's address; returns in $f0), restoring the context afterwards
    float animPercent(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t m, int anim)
    {
        static const PS2Runtime::RecompiledFunction f = runtime->lookupFunction(wotm::fn::animationGetCurrentPercent__FG11_animHandle);
        if (!f || anim < 0 || uint32_t(anim) >= kNumAnims)
            return -1.f;
        const uint32_t h = m + kAnims + uint32_t(anim) * 0x10u;
        if (!(rd32(rdram, h) & 0x1FFFFFFFu))   // null ci: the monster lacks this animation
            return -1.f;
        const R5900Context saved = *ctx;
        ctx->r[4] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(h)));
        ctx->r[31] = _mm_set_epi64x(0, 0);
        ctx->pc = wotm::fn::animationGetCurrentPercent__FG11_animHandle;
        f(rdram, ctx, runtime);
        const float pct = ctx->f[0];
        *ctx = saved;
        return std::isfinite(pct) ? pct : -1.f;
    }

    // ---- MOVE LIST (wotm_mainmenu.inc): P1's monster's list and its favorites. Favorites file: training_moves.ini next to
    // settings.ini (one "<type>=<id>,<id>..." line per monster; without settings.ini, in memory only)
    constexpr uint32_t kPunchNext = 0x94u;   // PunchConfig: 6 floats, > 0 = the next type is allowed
    std::filesystem::path favPath()
    {
        const std::filesystem::path &p = wotm::settings::g_path;
        return p.empty() ? std::filesystem::path() : p.parent_path() / "training_moves.ini";
    }
    std::vector<std::string> loadFav(int type)
    {
        std::vector<std::string> out;
        const std::filesystem::path p = favPath();
        if (p.empty()) return out;
        std::ifstream f(p);
        std::string line;
        const std::string key = std::to_string(type) + "=";
        while (f && std::getline(f, line))
        {
            if (!line.empty() && line.back() == '\r') line.pop_back();
            if (line.rfind(key, 0) != 0) continue;
            std::string rest = line.substr(key.size()), id;
            for (char c : rest + ",")
            {
                if (c == ',') { if (!id.empty() && int(out.size()) < kFavMax) out.push_back(id); id.clear(); }
                else if (c != ' ') id += c;
            }
        }
        return out;
    }
    void saveFav(int type, const std::vector<std::string> &fav)
    {
        const std::filesystem::path p = favPath();
        if (p.empty()) return;
        std::vector<std::string> lines;
        {
            std::ifstream f(p);
            std::string line;
            const std::string key = std::to_string(type) + "=";
            while (f && std::getline(f, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                if (!line.empty() && line.rfind(key, 0) != 0) lines.push_back(line);
            }
        }
        std::string mine = std::to_string(type) + "=";
        for (size_t i = 0; i < fav.size(); ++i) mine += (i ? "," : "") + fav[i];
        lines.push_back(mine);
        std::ofstream f(p, std::ios::trunc);
        f << "# Training mode MOVE LIST: favorites per monster type (m_typeBits >> 5); ids P<type>, C<type>x<n>, W<type>\n";
        for (const std::string &l : lines)
            if (l[0] != '#') f << l << '\n';
    }
    // builds monster m's list (position 1 punches, same-button sequences, object attacks) and publishes it with its favorites
    void buildMoves(const uint8_t *rdram, uint32_t m, int type, bool bat, uint32_t pickup)
    {
        std::vector<Move> list;
        auto fill = [](Move &mv, const PunchCfg &c) {
            mv.ini = c.ini; mv.fim = c.fim; mv.radius = c.radius; mv.stun = c.stun; mv.kb = c.kb;
        };
        for (int k = 0; k < kPunchTypes; ++k)
        {
            const PunchCfg c = readPunchCfg(rdram, m, 0, k);
            if (!c.valid) continue;
            Move mv;
            mv.kind = MkPunch; mv.type = int8_t(k); mv.len = 1; mv.dmg = c.dmg;
            fill(mv, c);
            std::snprintf(mv.id, sizeof mv.id, "P%d", k);
            list.push_back(mv);
        }
        for (int k = 0; k < kPunchTypes; ++k)
        {
            PunchCfg c = readPunchCfg(rdram, m, 0, k);
            if (!c.valid) continue;
            float total = c.dmg;
            int n = 1;
            for (int t = 0; t + 1 < kPunchTiers; ++t)
            {
                const uint32_t cfg = m + kPunch + kPunchCfgs + uint32_t(t) * kTierStride + uint32_t(k) * kTypeStride;
                if (!(rdf(rdram, cfg + kPunchNext + uint32_t(k) * 4u) > 0.f)) break;
                const PunchCfg nx = readPunchCfg(rdram, m, t + 1, k);
                if (!nx.valid) break;
                c = nx;
                total += c.dmg;
                ++n;
            }
            if (n < 2) continue;
            Move mv;
            mv.kind = MkCombo; mv.type = int8_t(k); mv.len = uint8_t(n); mv.dmg = total;
            fill(mv, c);
            std::snprintf(mv.id, sizeof mv.id, "C%dx%d", k, n);
            list.push_back(mv);
        }
        for (int k = 0; k < kPunchTypes; ++k)
        {
            const PunchCfg c = readSwipeCfg(rdram, m, 0, k, bat);
            if (!c.valid) continue;
            Move mv;
            mv.kind = MkWeapon; mv.type = int8_t(k); mv.len = 1; mv.dmg = c.dmg;
            fill(mv, c);
            std::snprintf(mv.id, sizeof mv.id, "W%d", k);
            list.push_back(mv);
        }
        // the other common moves (80b3802:docs/investigacoes/2026-10-25/move_list_comuns.md): numbers from the monster's own RAM
        // btns and dir: the command (the panel row's icons): kBtn* bits and the numpad-notation direction (8 forward/up, 2 back, 5 none)
        auto other = [&](const char *id, const char *name, uint32_t state, float dmg, const char *hint, uint8_t btns, int dir = 5) {
            Move mv;
            mv.kind = MkOther; mv.state = state; mv.dmg = std::isfinite(dmg) ? dmg : 0.f;
            mv.btns = btns; mv.dir = int8_t(dir);
            std::snprintf(mv.id, sizeof mv.id, "%s", id);
            std::snprintf(mv.name, sizeof mv.name, "%s", name);
            std::snprintf(mv.hint, sizeof mv.hint, "%s", hint);
            list.push_back(mv);
            return list.size() - 1;
        };
        constexpr uint8_t kSq = uint8_t(kBtnSquare), kTri = uint8_t(kBtnTriangle), kCir = uint8_t(kBtnCircle), kX = uint8_t(kBtnX), kR2 = uint8_t(kBtnR2), kSpecialBtn = uint8_t(kCir | kTri);
        char h[200];
        {   // butt slam: ring 0 directly within reach[0], then the wave out to reach[n-1]
            const int n = std::clamp(int(rd32(rdram, m + 0xFA84u)), 0, 8);
            const float d0 = rdf(rdram, m + 0xFAA8u), r0 = rdf(rdram, m + 0xFCD4u), rn = n > 0 ? rdf(rdram, m + 0xFCD4u + 4u * uint32_t(n - 1)) : 0.f;
            const float cost = rdf(rdram, m + 0xFA68u);
            std::snprintf(h, sizeof h, "IN THE AIR: UP + HEAVY. %.0f DAMAGE WITHIN %.0f, THEN A SHOCKWAVE UP TO %.0f (%d RINGS). STAMINA %.0f. KNOCKS DOWN", d0, r0, rn, n, cost);
            const size_t i = other("Obutt", "BUTT SLAM", 0xFA1Cu, d0, h, kTri, 8);
            list[i].kb = rdf(rdram, m + 0xFAB0u); list[i].stun = rdf(rdram, m + 0xFAB8u); list[i].radius = r0;
        }
        {   // ram attack: base damage + speed x factor
            const float base = rdf(rdram, m + 0xFD80u), per = rdf(rdram, m + 0xFD84u), cost = rdf(rdram, m + 0xFD88u);
            char sp[40] = "";
            if (per > 0.f) std::snprintf(sp, sizeof sp, " + %.3f PER UNIT OF SPEED", per);   // factor 0 on every monster seen
            std::snprintf(h, sizeof h, "RUN (TAP FORWARD TWICE) + HEAVY, HOLD TO CHARGE. %.0f DAMAGE%s. STAMINA %.0f. KNOCKS DOWN", base, sp, cost);
            const size_t i = other("Oram", "RAM ATTACK", 0xFD40u, base, h, kTri, 8);
            list[i].kb = 1.f;
        }
        {   // grab: the cone's reach; attacks while grabbing (Congar uses MonkeyOHAttack); throw (damage from the victim's ground impact)
            const float range = rdf(rdram, m + 0xDD04u);
            std::snprintf(h, sizeof h, "GRAB NEAR THE OPPONENT (RANGE %.0f). THEN LIGHT OR HEAVY TO HIT, GRAB OR UP + GRAB TO THROW", range);
            const size_t g = other("Ograb", "GRAB", 0xDCE0u, 0.f, h, kCir);
            list[g].radius = range;
            const bool monkey = type == 1;   // Congar (kMonsterNames: type = m_typeBits >> 5)
            const uint32_t st = monkey ? 0x10B34u : 0x10AE0u;
            const float dSq = rdf(rdram, m + (monkey ? 0x10B5Cu : 0x10B08u)), cSq = rdf(rdram, m + (monkey ? 0x10B60u : 0x10B0Cu));
            const float dTri = rdf(rdram, m + (monkey ? 0x10B70u : 0x10B1Cu)), cTri = rdf(rdram, m + (monkey ? 0x10B74u : 0x10B20u));
            std::snprintf(h, sizeof h, "LIGHT WHILE GRABBING. %.0f DAMAGE, STAMINA %.0f. STOPS THE OPPONENT FROM STRUGGLING FOR A MOMENT", dSq, cSq);
            list[other("Ograbsq", "GRAB: LIGHT", st, dSq, h, kSq)].variant = 1;   // the same state: the animation at +0x50 (0xBA = special) tells them apart
            std::snprintf(h, sizeof h, "HEAVY WHILE GRABBING. %.0f DAMAGE, STAMINA %.0f. LETS THE OPPONENT GO", dTri, cTri);
            list[other("Ograbtri", "GRAB: HEAVY", st, dTri, h, kTri)].variant = 2;
            const float ground = rdf(rdram, m + 0xE430u), perV = rdf(rdram, m + 0xE434u), wall = rdf(rdram, m + 0xE400u);
            char sp[40] = "";
            if (perV > 0.f) std::snprintf(sp, sizeof sp, " + %.3f PER UNIT OF SPEED", perV);
            std::snprintf(h, sizeof h, "GRAB AGAIN WHILE GRABBING (UP + GRAB THROWS UP). LANDING %.0f%s, WALL %.0f; WHOEVER IT HITS TAKES THE SAME", ground, sp, wall);
            const size_t t = other("Ograbthrow", "GRAB: THROW", 0xDD74u, ground, h, kCir);
            list[t].kb = 1.f;
        }
        // getup attack (b4e810b:docs/investigacoes/2026-10-26/move_list_getup_taunt.md): while down, every face button press rewrites
        // getupAttack (ThrowBack +0x558) with "LIGHT or HEAVY pressed"; entry 0 or 1 by the orientation of locator 0x809 (+0x3358)
        for (int i = 0; i < 2; ++i)   // 0x24-byte entries
        {
            const uint32_t e = m + 0x10C40u + uint32_t(i) * 0x24u;
            const int anim = int(rd32(rdram, e + 0x20u));
            const float d = rdf(rdram, e + 0x34u);
            if (anim <= 0 || !(d > 0.f)) continue;
            char id[12], nm[16];
            std::snprintf(id, sizeof id, "Ogetup%d", i);
            std::snprintf(nm, sizeof nm, "GETUP %d", i + 1);
            std::snprintf(h, sizeof h, "WHILE KNOCKED DOWN, PRESS LIGHT OR HEAVY (THE LAST BUTTON MASHED DECIDES; HOLDING IT AS YOU LAND COUNTS) TO GET UP SWINGING. %.0f DAMAGE, WINDOW %.0f-%.0f", d, rdf(rdram, e + 0x14u) * 100.f, rdf(rdram, e + 0x18u) * 100.f);
            const size_t k = other(id, nm, 0x10C40u, d, h, uint8_t(kSq | kTri));
            list[k].ini = rdf(rdram, e + 0x14u); list[k].fim = rdf(rdram, e + 0x18u);
            list[k].kb = rdf(rdram, e + 0x28u); list[k].stun = rdf(rdram, e + 0x30u);
        }
        {   // crowd control: a wave using the special meter
            const int n = std::clamp(int(rd32(rdram, m + 0xF42Cu)), 0, 8);
            const float d0 = rdf(rdram, m + 0xF450u), rn = n > 0 ? rdf(rdram, m + 0xF67Cu + 4u * uint32_t(n - 1)) : 0.f;
            // a ring with no force (+0xC and +0x10 zero) only staggers (HitEvent type 1; b4e810b:docs/investigacoes/2026-10-26/move_list_campos_especiais.md): the last one, in crowd control
            const uint32_t last = m + 0xF44Cu + uint32_t(n > 0 ? n - 1 : 0) * 0x1Cu;
            const bool edge = n > 1 && rdf(rdram, last + 0xCu) == 0.f && rdf(rdram, last + 0x10u) == 0.f;
            std::snprintf(h, sizeof h, "LIGHT + JUMP TOGETHER WITH THE SPECIAL METER. SHOCKWAVE UP TO %.0f (%d RINGS), %.0f DAMAGE ON THE FIRST%s. USES THE WHOLE METER", rn, n, d0,
                          edge ? "; THE OUTER EDGE ONLY STAGGERS" : "");
            const size_t i = other("Ocrowd", "CROWD CTRL", 0xF3F0u, d0, h, uint8_t(kSq | kX));
            list[i].radius = rn; list[i].kb = rdf(rdram, m + 0xF458u); list[i].stun = rdf(rdram, m + 0xF460u);
        }
        {   // taunt (b4e810b:docs/investigacoes/2026-10-26/move_list_getup_taunt.md): Select while standing still (StateIdle) or on landing; with the special meter full (+0xF4) it starts
            // rage at 95% of the animation: damage x +0x48C x +0x49C (each only if > 0), speed x +0x4A0, drains the meter in +0x494 s (20 if 0)
            const float a = rdf(rdram, m + 0x48Cu), b = rdf(rdram, m + 0x49Cu), spd = rdf(rdram, m + 0x4A0u), dur0 = rdf(rdram, m + 0x494u);
            const float dmgX = (a > 0.f ? a : 1.f) * (b > 0.f ? b : 1.f), dur = dur0 > 0.f ? dur0 : 20.f;
            std::snprintf(h, sizeof h, "SELECT, STANDING STILL. WITH THE SPECIAL METER FULL IT STARTS RAGE: DAMAGE X%.2f, SPEED X%.2f FOR %.0f SECONDS, DRAINING THE METER. OTHERWISE IT IS JUST A TAUNT",
                          dmgX, spd > 0.f ? spd : 1.f, dur);
            other("Otaunt", "TAUNT", 0xFE20u, 0.f, h, 0);
        }
        {   // Counter (80b3802:docs/investigacoes/2026-10-25/move_list_comuns.md, 7): from within Block, R2 + square/triangle; no damage (counterSuccess only cancels the
            // attack and sends the victim to Countered; collisTestBatAttack returns 0); window in fields +0x3C/+0x40, steals the object +0x4C
            const float f0 = rdf(rdram, m + 0x7E14u), f1 = rdf(rdram, m + 0x7E18u);
            const bool steals = rd32(rdram, m + 0x7E24u) != 0;
            std::snprintf(h, sizeof h, "WHILE BLOCKING, LIGHT OR HEAVY. NO DAMAGE: CANCELS THE HIT AND LEAVES THE OPPONENT OPEN%s. WORKS FROM FIELD %.0f TO %.0f",
                          steals ? ", TAKING THEIR OBJECT" : "", f0, f1);
            const size_t i = other("Ocounter", "COUNTER", 0x7DD8u, 0.f, h, uint8_t(kR2 | kSq | kTri));
            list[i].ini = f0; list[i].fim = f1;
        }
        {   // object throw (80b3802:docs/investigacoes/2026-10-25/move_list_comuns.md, 4): Throw (one hand) or TwoHandedThrow (two hands, bit 1 of Pickup +0x50); the damage is
            // the object's (s_info[type] +0x08, type = Pickup +0xA0) times the monster's multiplier (+0x6A38 + 4*type), radius +0x0C,
            // speed +0x34 (mph), stamina on hit +0x30. With no object in hand, the hint only explains.
            const int t = pickup ? int(rd32(rdram, pickup + kPickupKind)) : -1;
            const uint32_t flags = pickup ? rd32(rdram, pickup + 0x50u) : 0;
            const bool twoHanded = (flags & 2u) != 0, rigid = (flags & 4u) != 0;
            if (t >= 0 && t < 28)
            {
                const uint32_t info = wotm::sym::x_12LevelPickups_s_info + uint32_t(t) * 0x54u;
                const float od = rdf(rdram, info + 0x08u), orad = rdf(rdram, info + 0x0Cu), ospd = rdf(rdram, info + 0x34u), ogain = rdf(rdram, info + 0x30u);
                const float mod = rdf(rdram, m + 0x6A38u + 4u * uint32_t(t));
                const float d = od * (std::isfinite(mod) ? mod : 1.f);
                std::snprintf(h, sizeof h, "GRAB WHILE HOLDING THIS OBJECT (TYPE %d%s). %.0f DAMAGE (%.0f X %.2f), RADIUS %.0f, %.0f MPH. STAMINA +%.0f ON A HIT",
                              t, twoHanded ? ", TWO-HANDED" : "", d, od, mod, orad, ospd, ogain);
                const size_t i = other("Othrow", "THROW OBJ", twoHanded ? 0x1074Cu : 0xEF40u, d, h, kCir);
                list[i].radius = orad;
                if (rigid)
                {   // up + circle with a rigid-body object: LevelPickups::throwRBPickup, TwoHandedThrow's angle +0x2C and speed +0x30; the
                    // damage comes from the body's own collision (RigidPickup::createCollisEvents): +0x444 + |contact speed| x +0x448 + the
                    // object's damage (s_info +0x08), HitEvent source 0xB; the thrower gains s_info +0x30 stamina
                    const float base = rdf(rdram, pickup + 0x444u), per = rdf(rdram, pickup + 0x448u);
                    const float ang = rdf(rdram, m + 0x10778u), spd = rdf(rdram, m + 0x1077Cu);
                    std::snprintf(h, sizeof h, "UP + GRAB WITH THIS OBJECT: HURLS IT AT %.0f DEGREES, %.0f MPH. ON IMPACT %.0f + %.2f PER UNIT OF SPEED + THE OBJECT'S %.0f",
                                  ang, spd, base, per, od);
                    const size_t r = other("Othrowrb", "HURL OBJ", 0x1074Cu, base + od, h, kCir, 8);
                    list[r].kb = 1.f;
                }
            }
            else
                other("Othrow", "THROW OBJ", 0xEF40u, 0.f, "GRAB WHILE HOLDING AN OBJECT. THE DAMAGE IS THE OBJECT'S, TIMES YOUR MULTIPLIER FOR IT: PICK ONE UP TO SEE THE NUMBERS", kCir);
        }
        {   // the monster's special (80b3802:docs/investigacoes/2026-10-25/move_list_especiais.md): Monster +0x7980 points to the state; circle + triangle with the meter
            const uint32_t sp = rd32(rdram, m + 0x7980u) & 0x1FFFFFFFu;
            const uint32_t so = sp > m && sp - m < 0x11190u ? sp - m : 0;
            // shockwave (ShockWaveConfig): n at +0, rings at +0x20 (0x1C each, damage +4), reaches at +0x250 + 4i
            auto wave = [&](uint32_t cfg, float &d0, float &rn, int &n) {
                n = std::clamp(int(rd32(rdram, cfg)), 0, 8);
                d0 = rdf(rdram, cfg + 0x24u);
                rn = n > 0 ? rdf(rdram, cfg + 0x250u + 4u * uint32_t(n - 1)) : 0.f;
            };
            float d0 = 0.f, rn = 0.f;
            int n = 0;
            const char *cmd = "GRAB + HEAVY TOGETHER WITH THE SPECIAL METER";
            switch (so)
            {
            case 0xF6F4u:   // SonicRoar (Congar)
            {
                const float d = rdf(rdram, m + so + 0x3Cu), rng = rdf(rdram, m + so + 0x34u), loop = rdf(rdram, m + so + 0x2Cu);
                std::snprintf(h, sizeof h, "%s. SONIC ROAR: %.0f DAMAGE PER TARGET IN THE CONE, RANGE %.0f, %.0f FRAMES. KNOCKS DOWN", cmd, d, rng, loop);
                const size_t i = other("Ospecial", "SONIC ROAR", so, d, h, kSpecialBtn);
                list[i].radius = rng; list[i].kb = 1.f;
                break;
            }
            case 0xF974u:   // FireBreath (Togera, Raptros)
            {
                const float dps = rdf(rdram, m + so + 0x24u), scen = rdf(rdram, m + so + 0x28u), rng = rdf(rdram, m + so + 0x20u), fr = rdf(rdram, m + so + 0x1Cu);
                std::snprintf(h, sizeof h, "%s. FIRE BREATH: %.0f DAMAGE PER SECOND TO MONSTERS (%.0f TO BUILDINGS), RANGE %.0f, %.0f FRAMES", cmd, dps, scen, rng, fr);
                const size_t i = other("Ospecial", "FIRE BREATH", so, dps, h, kSpecialBtn);
                list[i].radius = rng;
                break;
            }
            case 0xF9D8u:   // BugAttack (Preytor): HomingBug statics (.sbss, MANTIS.CHR key 0xFAA), read by symbol
            {
                using namespace wotm::sym;
                const float dps = rdf(rdram, x_9HomingBug_s_damagePerSecond), maxD = rdf(rdram, x_9HomingBug_s_maxMonsterDamage);
                const float fields = rdf(rdram, x_9HomingBug_s_attachFields), lock = rdf(rdram, x_9HomingBug_s_targetLockMaxRange);
                const float latch = rdf(rdram, x_9HomingBug_s_attachRange), hp = rdf(rdram, x_9HomingBug_s_initialHealth);
                std::snprintf(h, sizeof h, "%s. HOMING BUG: LATCHES ON AND DRAINS %.1f PER SECOND, UP TO %.0f OR %.0f FIELDS. LOCKS ON WITHIN %.0f, LATCHES AT %.0f. %.0f HP",
                              cmd, dps, maxD, fields, lock, latch, hp);
                const size_t i = other("Ospecial", "BUG ATTACK", so, maxD, h, kSpecialBtn);
                list[i].radius = lock;
                break;
            }
            case 0xFE5Cu:   // RobotSpecial (Robo-47): a missile that explodes into a wave
            {
                wave(m + so + 0x58u, d0, rn, n);
                std::snprintf(h, sizeof h, "%s. HOLD TO AIM, RELEASE TO FIRE A HOMING MISSILE: SHOCKWAVE UP TO %.0f (%d RINGS), %.0f DAMAGE AT THE CENTER. KNOCKS DOWN", cmd, rn, n, d0);
                const size_t i = other("Ospecial", "MISSILE", so, d0, h, kSpecialBtn);
                list[i].radius = rn; list[i].kb = 1.f;
                break;
            }
            case 0x102A8u:   // RockSpecial (Agamo): head bomb
            {
                wave(m + so + 0x44u, d0, rn, n);
                std::snprintf(h, sizeof h, "%s. THROWS THE HEAD; PRESS AGAIN TO DETONATE: SHOCKWAVE UP TO %.0f (%d RINGS), %.0f DAMAGE AT THE CENTER. KNOCKS DOWN", cmd, rn, n, d0);
                const size_t i = other("Ospecial", "ROCK BOMB", so, d0, h, kSpecialBtn);
                list[i].radius = rn; list[i].kb = 1.f;
                break;
            }
            case 0x1059Cu:   // ZapAttack (Kineticlops)
            {
                const float dps = rdf(rdram, m + so + 0x38u), rng = rdf(rdram, m + so + 0x34u), loop = rdf(rdram, m + so + 0x2Cu);
                std::snprintf(h, sizeof h, "%s. LIGHTNING ON THE LOCKED TARGET: %.1f DAMAGE PER SECOND PLUS SHOCK, RANGE %.0f, %.0f FRAMES, CHAINS TO OTHERS. KNOCKS DOWN AT THE END", cmd, dps, rng, loop);
                const size_t i = other("Ospecial", "LIGHTNING", so, dps, h, kSpecialBtn);
                list[i].radius = rng; list[i].kb = 1.f;
                break;
            }
            case 0x10600u:   // GrappleHook (Ultra-V)
            {
                const float d = rdf(rdram, m + so + 0x34u), rng = rdf(rdram, m + so + 0x30u);
                std::snprintf(h, sizeof h, "%s. GRAPPLE HOOK: %.0f DAMAGE PLUS THE HELD OBJECT'S, RANGE %.0f, THEN PULLS THE TARGET IN", cmd, d, rng);
                const size_t i = other("Ospecial", "HOOK", so, d, h, kSpecialBtn);
                list[i].radius = rng;
                break;
            }
            case 0x10790u:   // AirStrike (Zorgulon)
            {
                const float d = rdf(rdram, m + so + 0x174u);
                const int fr = int(rd32(rdram, m + so + 0x178u));   // integer (720 in ALIEN.CHR)
                std::snprintf(h, sizeof h, "%s. FLARE, THEN SAUCERS STRAFE THE TARGET FOR %d FIELDS, %.1f DAMAGE PER SHOT", cmd, fr, d);
                other("Ospecial", "AIR STRIKE", so, d, h, kSpecialBtn);
                break;
            }
            case 0x10930u:   // LavaBlast (Magmo)
            {
                const float d = rdf(rdram, m + so + 0x19Cu);
                const int balls = int(rd32(rdram, m + so + 0x190u));
                std::snprintf(h, sizeof h, "%s. LAVA BLAST: %d BALLS OF %.1f DAMAGE EACH, PLUS FIRE. KNOCKS DOWN", cmd, balls, d);
                const size_t i = other("Ospecial", "LAVA BLAST", so, d, h, kSpecialBtn);
                list[i].kb = 1.f;
                break;
            }
            case 0x10F60u:   // CannonHands (Goliath)
            {
                const float d = rdf(rdram, m + 0x69ACu);
                std::snprintf(h, sizeof h, "%s. CANNON HANDS: 36 HOMING SHOTS OF %.0f DAMAGE", cmd, d);
                other("Ospecial", "CANNON", so, d, h, kSpecialBtn);
                break;
            }
            default:
                if (so) other("Ospecial", "SPECIAL", so, 0.f, cmd, kSpecialBtn);
                break;
            }
        }
        Shared &sh = shared();
        std::lock_guard<std::mutex> lk(sh.m);
        sh.moveList = std::move(list);
        if (sh.movesType != type)   // new monster: its favorites come from the file (ones changed in the panel were already saved)
        {
            sh.fav = loadFav(type);
            sh.favDirty = false;
            std::fprintf(stderr, "[training] move list for type %d: %zu moves, %zu favorite(s)\n", type, sh.moveList.size(), sh.fav.size());
            for (const Move &mv : sh.moveList)
                if (mv.kind == MkOther)
                    std::fprintf(stderr, "[training]   %-11s damage %.1f radius %.0f kb %.0f stun %.0f: %s\n", mv.name, mv.dmg, mv.radius, mv.kb, mv.stun, mv.hint);
        }
        sh.movesType = type;
    }
    // every 64 frames on P1: a new list when the monster changes; favorites changed in the panel go to the file
    void updateMoves(const uint8_t *rdram, uint32_t m, bool bat, uint32_t pickup)
    {
        static uint32_t lastPickup = 0, lastLogged = 0;   // the object in hand changes the throw row: rebuild the list right away
        const int type = int(rd32(rdram, m + 0x14) >> 5);
        bool rebuild = pickup != lastPickup, save = false;
        lastPickup = pickup;
        std::vector<std::string> fav;
        {
            Shared &sh = shared();
            std::lock_guard<std::mutex> lk(sh.m);
            rebuild = rebuild || sh.movesType != type;
            if (sh.favDirty) { save = true; fav = sh.fav; sh.favDirty = false; }
        }
        if (save) saveFav(type, fav);
        if (rebuild || (frameNow(rdram) % 64u) == 0) buildMoves(rdram, m, type, bat, pickup);
        if (pickup && pickup != lastLogged)
        {   // new object in hand: the throw rows with its numbers (to check without opening the panel)
            lastLogged = pickup;
            Shared &sh = shared();
            std::lock_guard<std::mutex> lk(sh.m);
            for (const Move &mv : sh.moveList)
                if (mv.kind == MkOther && !std::strncmp(mv.id, "Othrow", 6))
                    std::fprintf(stderr, "[training]   %-11s damage %.1f radius %.0f: %s\n", mv.name, mv.dmg, mv.radius, mv.hint);
        }
    }

    // On exit from Monster::update: the attack in progress (punch or object attack: config, % of animation, locators) and, for P1,
    // the 6 types at the combo position (the SwipeConfigs while holding an object, the PunchConfigs otherwise)
    void trackPunch(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t m, int slot)
    {
        const uint32_t st = rd32(rdram, m + 0x34) & 0x1FFFFFFFu;
        const bool swiping = st == m + kBat;
        const bool punching = st == m + kPunch || swiping;
        if (!punching && slot != 0)
            return;
        const uint32_t pickup = heldPickup(rdram, m);
        const bool weapon = swiping || (pickup != 0 && st != m + kPunch);
        const bool bat = pickup != 0 && rd32(rdram, pickup + kPickupKind) == kPickupKindBat;
        const uint32_t base = swiping ? kBat : kPunch, offTier = swiping ? kSwipeTier : kPunchTier, offType = swiping ? kSwipeType : kPunchType;
        const int tier = punching ? int(rd32(rdram, m + base + offTier)) : 0;
        const int type = punching ? int(rd32(rdram, m + base + offType)) : -1;
        PunchLive live;
        if (punching)
        {
            live.frame = frameNow(rdram);
            live.tier = int8_t(tier);
            live.type = int8_t(type);
            live.cfg = swiping ? readSwipeCfg(rdram, m, tier, type, bat) : readPunchCfg(rdram, m, tier, type);
            live.inWindow = rd32(rdram, m + base + (swiping ? kSwipeInWindow : kPunchInWindow)) != 0;
            live.pct = live.cfg.valid ? animPercent(rdram, ctx, runtime, m, live.cfg.anim) : -1.f;
            for (int id : live.cfg.loc)
            {
                if (id < 0x802 || id > 0x832)
                    continue;
                float *p = live.loc[live.nloc];
                readVec(rdram, m + uint32_t(id) * 16u - 0x4260u, p);   // getLocatorTrans: current world translation
                if (std::isfinite(p[0] + p[1] + p[2]))
                    ++live.nloc;
            }
        }
        if (punching)
        {
            static uint32_t nlog = 0;
            if (nlog < 12 && live.cfg.valid && !live.inWindow)
            {
                ++nlog;
                std::fprintf(stderr, "[training] %s slot %d pos %d type %d: %.0f%% of animation, window %.0f-%.0f, %d locator(s), radius %.1f%s%.1f\n",
                             swiping ? "object attack" : "punch", slot, tier, type, live.pct * 100.f, live.cfg.ini * 100.f, live.cfg.fim * 100.f,
                             live.nloc, live.cfg.radius, swiping ? (bat ? ", bat x" : ", object x") : ", damage ", live.cfg.dmg);
            }
        }
        PunchCfg table[kPunchTypes];
        if (slot == 0)
        {
            for (int k = 0; k < kPunchTypes; ++k)
                table[k] = weapon ? readSwipeCfg(rdram, m, tier, k, bat) : readPunchCfg(rdram, m, tier, k);
            updateMoves(rdram, m, bat, pickup);
        }
        Shared &sh = shared();
        std::lock_guard<std::mutex> lk(sh.m);
        if (punching)
            sh.live[slot] = live;
        if (slot == 0)
        {
            std::copy(std::begin(table), std::end(table), std::begin(sh.table));
            sh.tableTier = int8_t(tier);
            sh.tableWeapon = weapon;
            sh.p1State = st > m && st - m < 0x11190u ? st - m : 0;
            sh.p1StateAnim = sh.p1State ? rd32(rdram, st + 0x50u) : 0;
        }
    }

    void monsterUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
        if (ctx->pc != kMonsterUpdate || !ok(m, 0x450u))
        {
            g_origUpdate(rdram, ctx, runtime);
            return;
        }
        const int slot = slotOf(rdram, m);
        const bool on = g_on.load(std::memory_order_relaxed);
        if (on)
            applyDifficulty(rdram, kGameObj + kDifficulty);   // CPU difficulty chosen in the panel (or the game's restored)
        if (slot >= 0)
        {
            const uint32_t bit = 1u << slot;
            const bool isDummy = slot >= numPlayers(rdram);
            const bool still = on && isDummy && g_dummy.load(std::memory_order_relaxed) == DStand;
            if (still)
            {
                rdram[m + kPadEnabled] = 0;   // no inputs: the AI doesn't act; states (recoil, fall, grab) still run
                g_padOff |= bit;
            }
            else if (g_padOff & bit)
            {
                rdram[m + kPadEnabled] = 1;
                g_padOff &= ~bit;
            }
            const uint32_t opts = g_opts.load(std::memory_order_relaxed);
            if (on && (opts & kRefill))
            {
                // the player refills to full; the dummy, to the chosen % (health above it is lowered too, so the % applies right away)
                const float h = rdf(rdram, m + kHealth), mx = rdf(rdram, m + kMaxHealth);
                const float pct = isDummy ? std::clamp(g_dummyHealth.load(std::memory_order_relaxed), 1, 100) / 100.f : 1.f;
                const float target = mx * pct;
                if (std::isfinite(h) && mx > 0.f && h > 0.f && std::fabs(h - target) > 0.01f && frameNow(rdram) - g_lastDamage[slot] > 90u)
                    wrf(rdram, m + kHealth, target);
            }
            if (const int meters = g_meters.load(std::memory_order_relaxed); on && meters != MOff)
            {   // like the F1 cheats (wotm_overrides.cpp, monsterUpdateCheats), on everyone: stamina full or in rage, special available
                std::memcpy(rdram + m + 0x470, rdram + m + (meters == MRage ? 0x468 : 0x460), 4);   // StaminaMeter: cur = max or maxLevel
                std::memset(rdram + m + 0x474, 0, 8);                    // recoverPool, exhaustTimer
                std::memset(rdram + m + 0x480, 0, 4);                    // exhausted
                rdram[m + 0xF3] = 1;                                     // m_specialWeapon
            }
        }
        const uint32_t saved = g_curMonster, ra = GPR_U32(ctx, 31);
        g_curMonster = m;
        g_origUpdate(rdram, ctx, runtime);
        g_curMonster = saved;
        if (on && slot >= 0 && returned(ctx, ra))
        {
            trackState(rdram, m, slot);
            if (g_opts.load(std::memory_order_relaxed) & kMoveTable)
                trackPunch(rdram, ctx, runtime, m, slot);
            if (slot == 0 || slot == 4)
                recordPad(rdram, m, slot == 0 ? 0 : 1);
        }
    }


    // Skip the intro (635fd3c:docs/investigacoes/2026-10-24/treino_intro.md): the same path as START in Monster::updateCinema (movieAborted,
    // fadeOutAndIn(4) and ActionDispatch::stopAllActiveActions through TaskManager::global), only during the intro: armed by the fight's
    // rtMain(first) (g_introMs), valid for 10 s and disarmed on first use. RESET POSITIONS (rtMain(0)) doesn't arm it.
    PS2Runtime::RecompiledFunction g_origCinema = nullptr;
    void callFn(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, uint32_t fn, uint32_t a0, uint32_t a1, uint32_t a2)
    {
        const PS2Runtime::RecompiledFunction f = rt->lookupFunction(fn);
        if (!f) return;
        const R5900Context saved = *ctx;
        const uint32_t a[3] = {a0, a1, a2};
        for (int i = 0; i < 3; ++i)
            ctx->r[4 + i] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a[i])));
        ctx->r[31] = _mm_set_epi64x(0, 0);
        ctx->pc = fn;
        f(rdram, ctx, rt);
        *ctx = saved;
    }
    void cinemaUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        g_origCinema(rdram, ctx, runtime);
        if (!g_on.load(std::memory_order_relaxed) || !g_skipIntro.load(std::memory_order_relaxed))
            return;
        const int64_t armed = g_introMs.load(std::memory_order_relaxed);
        if (armed < 0 || nowMs() - armed > 10000)
            return;
        if (!rd32(rdram, wotm::sym::moviePlaying) || rd32(rdram, wotm::sym::movieAborted) || rd32(rdram, kGameObj + 0x120454u) ||
            rd32(rdram, kGameObj + 0x120458u))
            return;
        g_introMs = -1;
        const uint32_t one = 1;
        std::memcpy(rdram + wotm::sym::movieAborted, &one, 4);
        callFn(rdram, ctx, runtime, wotm::fn::fadeOutAndIn__7TheGamei, kGameObj, 4u, 0u);
        callFn(rdram, ctx, runtime, wotm::fn::add__11TaskManagerPFv_Uii, wotm::sym::x_11TaskManager_global,
               wotm::fn::stopAllActiveActions__14ActionDispatchv, 10u);
        std::fprintf(stderr, "[training] intro skipped\n");
    }

    // Hud::update's "WINS n" (m_gameMode 2..6, training's versus): in training it counts nothing and the dummy's lands under the panel.
    // Hud sprintfs into two buffers in Hud's .rodata (no symbol in the ELF: the left one and the right one) and passes them to
    // fontSpritePrint(Right)XY; with training on, those two are not printed
    constexpr uint32_t kWinsLeft = 0x006EA920u, kWinsRight = 0x006EA938u;
    PS2Runtime::RecompiledFunction g_origPrint = nullptr, g_origPrintRight = nullptr;
    bool skipWins(R5900Context *ctx)
    {
        const uint32_t s = GPR_U32(ctx, 7) & 0x1FFFFFFFu;
        if (!g_on.load(std::memory_order_relaxed) || (s != kWinsLeft && s != kWinsRight))
            return false;
        ctx->pc = GPR_U32(ctx, 31);
        return true;
    }
    void printXY(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != wotm::fn::fontSpritePrintXY__FiiiPc || !skipWins(ctx))
            g_origPrint(rdram, ctx, runtime);
    }
    void printRightXY(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != wotm::fn::fontSpritePrintRightXY__FiiiPc || !skipWins(ctx))
            g_origPrintRight(rdram, ctx, runtime);
    }
    // A blocking dummy: in place of the AI (Ai::updateInputs only runs with the pad enabled, after the GamePad's clearInputs), holds
    // L1+R1 (lock-on: keeps facing the target) and R2 (block). "After the 1st hit": while in a reaction and up to 1 s after the
    // last hit taken (blocked or not).
    void aiInputs(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)   // a0 = Monster + 0x4E0
    {
        const int mode = g_dummy.load(std::memory_order_relaxed);
        if (ctx->pc != kAiInputs || !g_on.load(std::memory_order_relaxed) || (mode != DBlock && mode != DBlockAfter && mode != DJump))
        {
            g_origAi(rdram, ctx, runtime);
            return;
        }
        const uint32_t m = (GPR_U32(ctx, 4) & 0x1FFFFFFFu) - kAiInMonster;
        const int slot = ok(m, kGamePad + 0x1C) ? slotOf(rdram, m) : -1;
        if (slot < numPlayers(rdram))
        {
            g_origAi(rdram, ctx, runtime);
            return;
        }
        const uint32_t f = frameNow(rdram);
        const bool block = mode != DJump && (mode == DBlock || stateCls(rdram, m) == 2 || (g_lastHit[slot] && f - g_lastHit[slot] <= 60u) ||
                           (g_lastDamage[slot] && f - g_lastDamage[slot] <= 60u));
        static uint32_t nlog = 0;
        if (++nlog == 300 || nlog == 1500)
        {
            const uint32_t st = rd32(rdram, m + 0x34) & 0x1FFFFFFFu;
            const StateName *sn = st > m && st - m < 0x11190u ? stateName(st - m) : nullptr;
            std::fprintf(stderr, "[training] dummy slot %d mode %d: state %s, holding block %d\n", slot, mode, sn ? sn->name : "?", int(block));
        }
        if (mode == DJump && (f % kJumpPeriod) < kJumpHold)
            rdram[m + kGamePad + 6] = 0xFF;   // X: jump
        if (block)
        {
            rdram[m + kGamePad + 0] = 0xFF;   // L1
            rdram[m + kGamePad + 3] = 0xFF;   // R1
            rdram[m + kGamePad + 4] = 0xFF;   // R2
        }
        ctx->pc = GPR_U32(ctx, 31);
    }
}

void wotmTrainingInstall(PS2Runtime &runtime)
{
    using namespace wotm_train;
    if (const char *e = std::getenv("WOTM_TRAINING"); e && *e && *e != '0')
        g_on = true;
    if (const char *e = std::getenv("WOTM_TRAINING_DUMMY"); e && *e)   // 0 stand still, 1 CPU, 2 always block, 3 block after the 1st hit, 4 jump
        g_dummy = std::clamp(std::atoi(e), 0, DCount - 1);
    if (const char *e = std::getenv("WOTM_TRAINING_HEALTH"); e && *e)  // dummy's health % on refill
        g_dummyHealth = std::clamp(std::atoi(e), 1, 100);
    struct H { uint32_t at; PS2Runtime::RecompiledFunction *orig; PS2Runtime::RecompiledFunction fn; const char *name; };
    const H hooks[] = {
        {kMonsterUpdate, &g_origUpdate, &monsterUpdate, "Monster::update"},
        {kTakeHit, &g_origTakeHit, &takeHit, "Monster::takeHit"},
        {kTakeDamage, &g_origTakeDamage, &takeDamage, "Monster::takeDamage"},
        {kPunchTest, &g_origPunch, &punchTest, "collisTestCloseRangeAttack"},
        {kBatTest, &g_origBat, &batTest, "Pickup::collisTestBatAttack"},
        {kGetupUpdate, &g_origGetup, &getupUpdate, "StateGetupAttack::update"},
        {kWeaponHD, &g_origWeapon, &weaponHD, "Weapon::WeaponHD"},
        {kShockUpdate, &g_origShock, &shockUpdate, "ShockWave::update"},
        {kSphereSwept, &g_origSwept, &sphereSwept, "hdSphereTest (swept)"},
        {kSphereStatic, &g_origStatic, &sphereStatic, "hdSphereTest (static)"},
        {kAiInputs, &g_origAi, &aiInputs, "Ai::updateInputs"},
        {kFireTest, &g_origFire, &fireTest, "FireBreath::TestCollis"},
        {kLos, &g_origLos, &losTest, "hdLosCSTest"},
        {kConeTest, &g_origCone, &coneTest, "StateSonicRoar::testConeCollis"},
        {kPunchSearch, &g_origPunchSearch, &punchSearch, "getClosestMonsterToPunch"},
        {kFovSearch, &g_origFovSearch, &fovSearch, "getClosestMonsterInFOV"},
        {kEnvTest, &g_origEnv, &envTest, "collisTestEnvironmentCollisions"},
        {kBundle, &g_origBundle, &bundleTest, "hdBundledSphereTest"},
        {kStompUpdate, &g_origStomp, &stompUpdate, "StateStompAttack::update"},
        {kGrabOH, &g_origGrabOH, &grabOHUpdate, "StateGrappleOHAttack::update"},
        {kMonkeyOH, &g_origMonkeyOH, &monkeyOHUpdate, "StateMonkeyOHAttack::update"},
        {kHookTest, &g_origHook, &hookTest, "StateGrappleHook::testCollis"},
        {wotm::fn::updateCinema__7Monster, &g_origCinema, &cinemaUpdate, "Monster::updateCinema"},
        {wotm::fn::fontSpritePrintXY__FiiiPc, &g_origPrint, &printXY, "fontSpritePrintXY (WINS)"},
        {wotm::fn::fontSpritePrintRightXY__FiiiPc, &g_origPrintRight, &printRightXY, "fontSpritePrintRightXY (WINS)"},
    };
    int n = 0;
    for (const H &h : hooks)
        if ((*h.orig = runtime.lookupFunction(h.at)))
        {
            runtime.registerFunction(h.at, h.fn);
            ++n;
        }
        else
            std::fprintf(stderr, "[training] no hook: %s\n", h.name);
    std::fprintf(stderr, "[training] %d of %zu hooks; training mode %s (WOTM_TRAINING or TRAINING in the menu)\n", n, sizeof hooks / sizeof hooks[0],
                 g_on.load() ? "ON" : "off");
}
