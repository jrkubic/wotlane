// Custom monsters (MODS.md, "Custom monsters"): monsters the game doesn't offer at selection, played in a regular fight. The bosses
// that are Monsters: Goliath Prime (type 13, "assboss", MON/ASSBOSS1.*), the adventure boss in canyon2, and Cerebulon's last two
// forms in the final stage (14 "jelly", MON/JELLY0.*, and 15 "final", MON/FINAL0.*; the first form, the Mecha, is a piece of the
// stage, not a Monster). Included by wotm_overrides.cpp, after wotm_skins.inc. Investigation: git show
// 18bb1d2:docs/investigacoes/2026-10-23/goliath_codigo.md (and goliath_selecao.md).
//
// - TheGame::SetAIMonster: every time an AI joins, retail disables every slot of types 0x1A0, 0x1C0 and 0x1E0 (disableMonster:
//   m_playerNum 0 and drawMe 0 on m_cs, on the shadow and on the _cs at +0x71F8/+0x71FC; in the adventure this is what hides the
//   boss until its cutscene). With a boss as a player it ended up invisible and frozen. Here whatever disableMonster clears on the
//   players (m_monsters[0..3]) is restored after the call; the adventure boss is an AI (m_monsters[4..]) and stays as in retail.
//   Retail also counts every 0x1C0/0x1E0 slot in m_numAIs (both wait hidden in the final stage): a player of those types is taken
//   out of the count, otherwise the loops over m_monsters[4 + i] would read one AI too many (null).
// - Monster::takeHit: on 0x1A0, damage only goes through when the boss's attackState is 4 or 10 or it comes from player 1
//   (attackerId == m_monsters[0]->m_id). With Goliath as a player, the AIs couldn't hurt it. During the call, player 1's m_id is set
//   to the attacker's (the only read of m_monsters[0] in takeHit is that test); the rest of takeHit follows the normal path
//   (blocking, knockback). Only when the Goliath being hit is a player.

#include "wotm_disc.hpp"   // .CHR of the base monster for the camera (camTable)

namespace wotm_custom
{
    constexpr uint32_t kSetAI = wotm::fn::SetAIMonster__7TheGameiiii, kTakeHit = wotm::fn::takeHit__7MonsterP8_fvectorfi;
    constexpr uint32_t kGameObj = wotm::sym::shellGame;
    constexpr uint32_t kMonsters = 0x120380;          // TheGame::m_monsters[8]
    constexpr uint32_t kTypeBits = 0x14, kPlayerNum = 0x18, kId = 0x20, kCs = 0xC, kShadow = 0x1A3C, kCsA = 0x71F8, kCsB = 0x71FC;
    constexpr uint32_t kDrawMe = 0xC;                 // _cs::drawMe (byte)
    constexpr uint32_t kNumAIs = 0x1203E0, kGameMode = 0x1203C8;   // TheGame::m_numAIs, m_gameMode
    constexpr uint32_t kSlots = 0xB80, kSlotStride = 0x11190, kNumSlots = 0x1203D4;   // TheGame::m_slots[16], m_numSlots
    constexpr uint32_t kGoliath = 13u << 5, kJelly = 14u << 5, kFinal = 15u << 5;
    // A boss that is a Monster (selection doesn't offer it; SetAIMonster hides it)
    bool bossType(uint32_t bits) { return bits == kGoliath || bits == kJelly || bits == kFinal; }
    PS2Runtime::RecompiledFunction g_origSetAI = nullptr, g_origTakeHit = nullptr;

    bool isPlayerSlot(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t m)
    {
        for (uint32_t i = 0; i < 4; ++i)
            if (READ32(kGameObj + kMonsters + 4 * i) == m)
                return true;
        return false;
    }

    struct Saved
    {
        uint32_t m = 0, playerNum = 0;
        uint32_t cs[4] = {};
        uint8_t draw[4] = {};
    };

    void setAI(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != kSetAI)
        {
            g_origSetAI(rdram, ctx, runtime);
            return;
        }
        // outside the adventure, every boss slot (a CPU boss stays visible too); in the adventure, only the players' slots
        const bool adventure = READ32(kGameObj + kGameMode) == 1u;
        const uint32_t slots = std::min(16u, READ32(kGameObj + kNumSlots));
        Saved saved[16];
        int n = 0;
        for (uint32_t i = 0; i < slots; ++i)
        {
            const uint32_t m = kGameObj + kSlots + i * kSlotStride;
            if (!bossType(READ32(m + kTypeBits)) || (adventure && !isPlayerSlot(rdram, ctx, runtime, m)))
                continue;
            Saved &s = saved[n++];
            s.m = m;
            s.playerNum = READ32(m + kPlayerNum);
            const uint32_t at[4] = {kCs, kShadow, kCsA, kCsB};
            for (int k = 0; k < 4; ++k)
                if ((s.cs[k] = READ32(m + at[k])))
                    s.draw[k] = READ8(s.cs[k] + kDrawMe);
        }
        g_origSetAI(rdram, ctx, runtime);
        int32_t ais = static_cast<int32_t>(READ32(kGameObj + kNumAIs));
        bool players = false;
        for (int j = 0; j < n; ++j)
        {
            const Saved &s = saved[j];
            // An AI in this fight (m_monsters[4..]): aiInit gave it m_playerNum 2, which disableMonster cleared. Only Jelly and
            // Cerebulon: Goliath's AI (Ai::updateAssBoss) needs the points of its own stage
            bool cpu = false;
            const uint32_t bits = READ32(s.m + kTypeBits);
            for (uint32_t i = 0; i < 12 && !cpu && (bits == kJelly || bits == kFinal); ++i)
                cpu = READ32(kGameObj + kMonsters + 4 * (4 + i)) == s.m;
            const bool player = isPlayerSlot(rdram, ctx, runtime, s.m);
            if (!player && !cpu)
                continue;   // loaded but with no AI yet: retail hides it until its own SetAIMonster
            WRITE32(s.m + kPlayerNum, player ? s.playerNum : 2u);
            // the player goes back to how it was; the CPU gets body and shadow on, like a regular AI after SetAIMonster
            // (before it all four are off), and the two extra _cs off, like the player's
            for (int k = 0; k < 4; ++k)
                if (s.cs[k])
                    WRITE8(s.cs[k] + kDrawMe, player ? s.draw[k] : uint8_t(k < 2));
            if (!player)
                continue;
            // counted by retail (m_playerNum 2 before disableMonster, or type 0x1C0/0x1E0); WOTM_AIVSAI's setAIMonsterCounted
            // doesn't subtract it, because it already reads it as zero
            const uint32_t type = READ32(s.m + kTypeBits);
            if (s.playerNum == 2u || type == kJelly || type == kFinal)
                --ais;
            players = true;
        }
        if (players)
            WRITE32(kGameObj + kNumAIs, static_cast<uint32_t>(ais < 0 ? 0 : ais));
    }

    void takeHit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = GPR_U32(ctx, 4), attacker = GPR_U32(ctx, 6);
        const uint32_t p1 = ctx->pc == kTakeHit && READ32(self + kTypeBits) == kGoliath && isPlayerSlot(rdram, ctx, runtime, self)
                                ? READ32(kGameObj + kMonsters) : 0;
        const uint32_t oldId = p1 ? READ32(p1 + kId) : 0;
        if (p1)
            WRITE32(p1 + kId, attacker);
        g_origTakeHit(rdram, ctx, runtime);
        if (p1)
            WRITE32(p1 + kId, oldId);
    }

    // Sound: SoundManager::loadMonsterBank (and the low-res one) has no case for the bosses. Goliath's sounds are in the boss bank
    // (gSoundBanks[BANK_MECH], Assemb_s.bnk), which retail only loads in the boss cutscene in canyon2 (canyon2BossIntro); Jelly's
    // and the final form's are in Final_s.bnk (gSoundBanks[BANK_FINAL]), which loadRTSoundBanks only loads in the adventure's final
    // stage. Outside the adventure, the bank request for 0x1A0 becomes SoundManager::loadMechBossSoundBank (same this), as a
    // jump, and the one for 0x1C0/0x1E0 becomes m_banks.loadSoundBank(&gSoundBanks[BANK_FINAL], "Final_s.bnk", autoUnload) (which
    // does nothing when the bank is already loaded: both types in the same fight share one).
    constexpr uint32_t kLoadBank = wotm::fn::loadMonsterBank__12SoundManagerii, kLoadLoBank = wotm::fn::loadLoResMonsterBank__12SoundManagerii,
                       kLoadMech = wotm::fn::loadMechBossSoundBank__12SoundManager,
                       kLoadSoundBank = wotm::fn::loadSoundBank__16SoundBankManagerPPvPci;
    constexpr uint32_t kSndBanks = 0x210, kBankFinal = 24;   // SoundManager::m_banks, BANK_FINAL
    PS2Runtime::RecompiledFunction g_origLoadBank = nullptr, g_origLoadLoBank = nullptr, g_loadMech = nullptr;

    // On-screen name ("%s CRUSHED", "%s WINS"): the type's entry in the 5 copies of MonsterNewNames[17][12] ("assboss", "jelly",
    // "final"); gets monster.json's "name" (up to 11 letters, lowercase like the game's) or `fallback`
    void setShownName(uint8_t *rdram, uint32_t type, const char *fallback)
    {
        std::string shown = fallback;
        if (const wotm::mods::Custom *cu = wotm::mods::Registry::get().customByType(int(type)))
        {
            std::string n = cu->name;
            if (n.size() > 11)   // "GOLIATH PRIME" -> "goliath": cut at the last word that fits
            {
                const size_t sp = n.rfind(' ', 11);
                n = n.substr(0, sp != std::string::npos && sp > 0 ? sp : 11);
            }
            shown.clear();
            for (char ch : n)
                shown += char(std::tolower(static_cast<unsigned char>(ch)));
        }
        for (const uint32_t names : {wotm::sym::MonsterNewNames_006E8958, wotm::sym::MonsterNewNames_006E8D90, wotm::sym::MonsterNewNames_006EAA78,
                                     wotm::sym::MonsterNewNames_006ED318, wotm::sym::MonsterNewNames_006EFEC8})
        {
            char *e = reinterpret_cast<char *>(rdram + ((names + type * 12) & 0x01FFFFFFu));
            std::memset(e, 0, 12);
            std::memcpy(e, shown.data(), shown.size());
        }
    }

    void loadBankCommon(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t entry, PS2Runtime::RecompiledFunction orig)
    {
        const uint32_t bits = ctx->pc == entry ? GPR_U32(ctx, 5) : 0;
        if ((bits == kJelly || bits == kFinal) && READ32(kGameObj + kGameMode) != 1u)
        {
            const uint32_t self = GPR_U32(ctx, 4), autoUnload = GPR_U32(ctx, 6);
            std::fprintf(stderr, "[custom] sound bank for %s (Final_s.bnk)\n", bits == kJelly ? "Jelly" : "Cerebulon");
            setShownName(rdram, bits >> 5, bits == kJelly ? "jelly" : "cerebulon");
            wotm_modes4::call(rdram, ctx, runtime, kLoadSoundBank,
                              {self + kSndBanks, wotm::sym::gSoundBanks + 4 * kBankFinal, wotm_modes4::scratchText(rdram, "Final_s.bnk"), autoUnload});
            ctx->pc = GPR_U32(ctx, 31);
            return;
        }
        if (bits == kGoliath && READ32(kGameObj + kGameMode) != 1u && g_loadMech)
        {
            std::fprintf(stderr, "[custom] sound bank for Goliath (Assemb_s.bnk, loadMechBossSoundBank)\n");
            setShownName(rdram, 13, "goliath");
            ctx->pc = kLoadMech;
            g_loadMech(rdram, ctx, runtime);
            return;
        }
        orig(rdram, ctx, runtime);
    }
    void loadBank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { loadBankCommon(rdram, ctx, runtime, kLoadBank, g_origLoadBank); }
    void loadLoBank(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { loadBankCommon(rdram, ctx, runtime, kLoadLoBank, g_origLoadLoBank); }

    // Camera: RST/ASSBOSS.CHR carries camera parameters retail never used (at POV 0 the camera sits in front and low:
    // m_00 = (-70, 336, 90) versus Togera's (0, -255, 150)), and RST/JELLY.CHR's sits at Jelly's legs. The monster's camera table
    // (+0x6CF0: 10 POVs of 0xA0, plus 10 for split screen) comes from its .CHR (MonsterTweaks::setActiveMonster) and
    // Cameras::SetCameraPOV copies the requested POV into the camera. A custom with "camera" gets the table from the base monster's
    // .CHR, with the three distances of each POV (m_00, m_10, m_20) scaled: on entry to SetCameraPOV for a camera following the
    // custom, the computed table is written into the monster before the copy.
    // The base's table comes from the .CHR read on the host (mods/ or disc), from the camera key as charreset::readKey reads it
    // (type 0xFC6: after 0x10 bytes, 2 screens of 9 POVs of 0xA0, which go to POVs 0..8 of each screen; the rest of the table stays
    // the monster's). Never read the .CHR through the game inside a hook: calling setActiveMonster twice sometimes left the
    // .CHR's own table on top of the borrowed one.
    constexpr uint32_t kSetPov = wotm::fn::SetCameraPOV__7CamerasiQ26Camera9CameraPOV;
    constexpr uint32_t kCamTable = 0x6CF0, kCamPovs = 20, kCamPov = 0xA0, kCamSplit = 0x640;
    constexpr uint32_t kCameras = wotm::sym::x_7Cameras_m_cameras, kCamStride = 0xEB0, kCamMonster = 0xCF4;
    constexpr uint32_t kChrCameraKey = 0xFC6, kChrKeyData = 0x10, kChrPovs = 9, kChrSplit = kChrPovs * kCamPov, kChrHeader = 12;
    // Without monster.json (WOTM_MATCH=13): Togera's, doubled. With it, the mod's "camera" ({"from", "scale"}).
    constexpr const char *kCamBaseDefault = "lizard";
    constexpr float kCamScaleDefault = 2.0f;
    PS2Runtime::RecompiledFunction g_origSetPov = nullptr;
    std::map<std::string, std::vector<uint8_t>> g_camTables;   // "<base>*<scale>" -> finished table (empty = failed)

    // .CHR camera key -> table (on top of `cam`, the monster's own); false without the key
    bool chrCamera(const std::vector<uint8_t> &file, std::vector<uint8_t> &cam)
    {
        auto rd = [&](size_t o) { uint32_t v = 0; if (o + 4 <= file.size()) std::memcpy(&v, &file[o], 4); return v; };
        for (size_t o = kChrHeader; o + 8 <= file.size();)
        {
            const uint32_t type = rd(o), size = rd(o + 4);
            if (type == 1 || size < 8)
                break;
            if (type == kChrCameraKey && o + kChrKeyData + 2 * kChrSplit <= file.size())
            {
                for (uint32_t s = 0; s < 2; ++s)
                    for (uint32_t p = 0; p < kChrPovs; ++p)
                    {
                        const uint8_t *src = &file[o + kChrKeyData + s * kChrSplit + p * kCamPov];
                        uint8_t *dst = &cam[s * kCamSplit + p * kCamPov];
                        for (uint32_t v = 0; v < 3; ++v)   // m_00, m_10, m_20 (without w)
                            std::memcpy(dst + v * 16, src + v * 16, 12);
                        std::memcpy(dst + 0x30, src + 0x30, 0x94 - 0x30);   // 3 PitchChange and the scalars
                        std::memcpy(dst + 0x94, src + 0x98, 4);             // 0x94 and 0x98 are swapped in readKey
                        std::memcpy(dst + 0x98, src + 0x94, 4);
                        std::memcpy(dst + 0x9C, src + 0x9C, 4);
                    }
                return true;
            }
            o += size;
        }
        return false;
    }

    const std::vector<uint8_t> &camTable(uint8_t *rdram, uint32_t m, const std::string &base, float scale, const char *who)
    {
        const std::string key = base + "*" + std::to_string(scale);
        if (const auto it = g_camTables.find(key); it != g_camTables.end())
            return it->second;
        std::vector<uint8_t> &cam = g_camTables[key];
        std::string up = base;
        for (char &ch : up)
            ch = char(std::toupper(static_cast<unsigned char>(ch)));
        const std::string rel = "RST/" + up + ".CHR";
        std::vector<uint8_t> file;
        const std::string host = wotm::mods::Registry::get().hostPath(rel);
        if (!host.empty())
        {
            std::ifstream in(std::filesystem::u8path(host), std::ios::binary);
            file.assign(std::istreambuf_iterator<char>(in), {});
        }
        else
        {
            const PS2Runtime::IoPaths &io = PS2Runtime::getIoPaths();
            wotm::disc::readFile(io.cdRoot, io.cdImage.string(), rel, file);
        }
        const uint8_t *const mon = rdram + ((m + kCamTable) & 0x01FFFFFFu);
        std::vector<uint8_t> table(mon, mon + kCamPovs * kCamPov);
        if (!chrCamera(file, table))
        {
            std::fprintf(stderr, "[custom] %s: camera from %s did not load (%s); keeping its own .CHR's\n", who, rel.c_str(),
                         file.empty() ? "file not found" : "no camera key");
            return cam;
        }
        cam = std::move(table);
        for (uint32_t p = 0; p < kCamPovs; ++p)
            for (uint32_t v = 0; v < 3; ++v)
                for (uint32_t c = 0; c < 3; ++c)
                {
                    float f;
                    std::memcpy(&f, &cam[p * kCamPov + v * 16 + c * 4], 4);
                    f *= scale;
                    std::memcpy(&cam[p * kCamPov + v * 16 + c * 4], &f, 4);
                }
        float d10[3];
        std::memcpy(d10, cam.data(), 12);
        std::fprintf(stderr, "[custom] %s: camera from %s, distances x%.2f (POV 0: %.0f, %.0f, %.0f)\n", who, rel.c_str(), scale, d10[0], d10[1], d10[2]);
        return cam;
    }

    void setPov(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t view = GPR_U32(ctx, 4);
        const uint32_t m = ctx->pc == kSetPov && view < 4 ? READ32(kCameras + view * kCamStride + kCamMonster) : 0;
        const uint32_t type = m ? READ32(m + kTypeBits) >> 5 : 0;
        const wotm::mods::Custom *cu = type ? wotm::mods::Registry::get().customByType(int(type)) : nullptr;
        const char *base = cu && !cu->cameraFrom.empty() ? cu->cameraFrom.c_str() : type == 13 && !cu ? kCamBaseDefault : nullptr;
        if (base)
        {
            const float scale = cu && !cu->cameraFrom.empty() ? cu->cameraScale : kCamScaleDefault;
            const std::vector<uint8_t> &cam = camTable(rdram, m, base, scale, cu ? cu->id.c_str() : "assboss");
            if (!cam.empty())
                std::memcpy(rdram + ((m + kCamTable) & 0x01FFFFFFu), cam.data(), cam.size());
        }
        g_origSetPov(rdram, ctx, runtime);
    }
    // Javelin (StateJavelin): Weapon::DetonateWeapon calls Monster::attachPickupImpaler on the victim, which enters StateImpaled via
    // enterNewState without transitionOK (which would have blocked anyone lacking anims 0x8E..0x90). StateImpaled::update only exits
    // when anim 0x90 ends; without it (ASSBOSS0/1) the victim was stuck forever. Without those anims, the hook does what retail does
    // with an invalid pickup: killPickup(it, 1) and return. The object's hit (dispatchHit) lands as a regular one. Investigation: 18bb1d2:docs/investigacoes/2026-10-23/goliath_impale.md.
    constexpr uint32_t kImpale = wotm::fn::attachPickupImpaler__7MonsterGQ2t10LinkedList1ZP6Pickup8IteratorP8_fvector,
                       kKillPickup = wotm::fn::killPickup__12LevelPickupsGQ2t10LinkedList1ZP6Pickup8Iteratori;
    constexpr uint32_t kAnims = 0x1CF0, kAnimStride = 0x10;   // Monster::m_anims[id].ci
    PS2Runtime::RecompiledFunction g_origImpale = nullptr, g_killPickup = nullptr;

    void impale(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = GPR_U32(ctx, 4);
        bool has = true;
        if (ctx->pc == kImpale && g_killPickup)
            for (uint32_t id = 0x8E; id <= 0x90; ++id)
                has = has && READ32(m + kAnims + id * kAnimStride) != 0;
        if (has)
        {
            g_origImpale(rdram, ctx, runtime);
            return;
        }
        std::fprintf(stderr, "[custom] javelin: monster type %u has no impaled anims; becomes a regular hit\n", READ32(m + kTypeBits) >> 5);
        ctx->r[4] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(GPR_U32(ctx, 5))));
        ctx->r[5] = _mm_set_epi64x(0, 1);
        ctx->pc = kKillPickup;
        g_killPickup(rdram, ctx, runtime);
    }
    // Spin (StateTopSpin, Monster+0x10EBC; investigation 18bb1d2:docs/investigacoes/2026-10-23/goliath_topspin.md). On regular monsters X+Square (PadFlags +0x38) is
    // StateCrowdControl (+0xF3F0), which never starts on Goliath (it asks for the type's effect anim, which Goliath lacks). Only the
    // boss AI entered TopSpin, via setAssBossState(12). With Goliath as a player:
    //  - StateCrowdControl::transitionOK: X+Square on the ground (Idle or Run), with attacks enabled, not holding a large object and
    //    TopSpin's transitionOK accepted;
    //  - Monster::enterNewState: CrowdControl becomes TopSpin;
    //  - StateTopSpin::update: the full hit (damage, push, sound) only lands on m_monsters[0], which is Goliath itself; during the call
    //    it is set to the nearest living AI;
    //  - Ai::setAssBossState (called by update on every hit): only acts for the adventure boss (whose owner is assBoss).
    constexpr uint32_t kCrowdOK = wotm::fn::transitionOK__17StateCrowdControl, kEnterState = wotm::fn::enterNewState__7MonsterP12MonsterState,
                       kSpinUpdate = wotm::fn::update__12StateTopSpin, kSpinOK = wotm::fn::transitionOK__12StateTopSpin,
                       kSetBossState = wotm::fn::setAssBossState__2Aii, kHoldingLarge = wotm::fn::isHoldingLarge__7Monster,
                       kPadFrame = wotm::fn::x__vc__8PadFlagsi;
    constexpr uint32_t kStateOwner = 0xC, kCurState = 0x34, kIdle = 0x7984, kRun = 0x79AC, kCrowd = 0xF3F0, kSpin = 0x10EBC, kTazer = 0x11130;
    constexpr uint32_t kAttacksOn = 0x4A, kPad = 0x5040, kPadSpin = 0x38, kDead = 0xE8, kNumMonsters = 0x1203D8, kTrans = 0x10;
    PS2Runtime::RecompiledFunction g_origCrowdOK = nullptr, g_origEnterState = nullptr, g_origSpinUpdate = nullptr, g_origSetBossState = nullptr;

    bool goliathPlayer(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t m)
    {
        return m && READ32(m + kTypeBits) == kGoliath && isPlayerSlot(rdram, ctx, runtime, m);
    }
    void ret(R5900Context *ctx, uint32_t v0)
    {
        ctx->r[2] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(v0)));
        ctx->pc = GPR_U32(ctx, 31);
    }

    void crowdOK(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = ctx->pc == kCrowdOK ? READ32(GPR_U32(ctx, 4) + kStateOwner) : 0;
        if (!goliathPlayer(rdram, ctx, runtime, m))
        {
            g_origCrowdOK(rdram, ctx, runtime);
            return;
        }
        using wotm_modes4::call;
        const uint32_t st = READ32(m + kCurState);
        bool ok = READ8(m + kAttacksOn) != 0 && (st == m + kIdle || st == m + kRun);
        ok = ok && READ16(call(rdram, ctx, runtime, kPadFrame, {m + kPad, 0u}) + kPadSpin) != 0;
        ok = ok && call(rdram, ctx, runtime, kHoldingLarge, {m}) == 0 && call(rdram, ctx, runtime, kSpinOK, {m + kSpin}) != 0;
        ret(ctx, ok ? 1u : 0u);
    }

    void enterState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = GPR_U32(ctx, 4);
        if (ctx->pc == kEnterState && GPR_U32(ctx, 5) == m + kCrowd && goliathPlayer(rdram, ctx, runtime, m))
        {
            ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(m + kSpin)));
            std::fprintf(stderr, "[custom] Goliath: spin (X+Square)\n");
        }
        if (ctx->pc == kEnterState && GPR_U32(ctx, 5) == m + kTazer && READ32(m + kTypeBits) == kJelly && isPlayerSlot(rdram, ctx, runtime, m))
            std::fprintf(stderr, "[custom] Jelly: shock (%s)\n", READ32(m + kPlayerNum) == 2u ? "AI" : "Triangle+Circle");
        g_origEnterState(rdram, ctx, runtime);
    }

    void spinUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = ctx->pc == kSpinUpdate ? READ32(GPR_U32(ctx, 4) + kStateOwner) : 0;
        uint32_t near = 0;
        if (goliathPlayer(rdram, ctx, runtime, m))
        {
            const uint32_t cs = READ32(m + kCs), n = std::min(8u, READ32(kGameObj + kNumMonsters));
            float best = 1e30f, p[3], q[3];
            std::memcpy(p, rdram + ((cs + kTrans) & 0x01FFFFFFu), 12);
            for (uint32_t i = 0; i < n && cs; ++i)
            {
                const uint32_t o = READ32(kGameObj + kMonsters + 4 * i), ocs = o ? READ32(o + kCs) : 0;
                if (!ocs || o == m || READ32(o + kPlayerNum) != 2u || READ8(o + kDead))
                    continue;
                std::memcpy(q, rdram + ((ocs + kTrans) & 0x01FFFFFFu), 12);
                const float d = (q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) + (q[2] - p[2]) * (q[2] - p[2]);
                if (d < best)
                    best = d, near = o;
            }
        }
        const uint32_t p1 = near ? READ32(kGameObj + kMonsters) : 0;
        if (near)
            WRITE32(kGameObj + kMonsters, near);
        g_origSpinUpdate(rdram, ctx, runtime);
        if (near)
            WRITE32(kGameObj + kMonsters, p1);
    }

    void setBossState(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kSetBossState)
        {
            const uint32_t boss = READ32(wotm::sym::assBoss);
            if (!boss || READ32(GPR_U32(ctx, 4)) != boss)
            {
                ctx->pc = GPR_U32(ctx, 31);   // void: only the adventure boss (or WOTM_AIVSAI, which runs initAssBoss)
                return;
            }
        }
        g_origSetBossState(rdram, ctx, runtime);
    }
    // Look-at (18bb1d2:docs/investigacoes/2026-10-23/goliath_lockon.md): Monster::updateLookAt turns the head toward the nearest enemy
    // (getLookAtTarget) through anims 154/155. On ASSBOSS they rotate the whole turret (28 of the 30 pieces) by up to 90 degrees,
    // which looks like a permanent lock-on. For Goliath as a player, no look-at target: the turret stays centered; with lock-on
    // (both triggers) it aims via getTarget, like a regular monster. The adventure boss and Goliath under AI follow retail.
    constexpr uint32_t kLookAt = wotm::fn::getLookAtTarget__7Monster;
    PS2Runtime::RecompiledFunction g_origLookAt = nullptr;

    void lookAt(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = GPR_U32(ctx, 4);
        if (ctx->pc == kLookAt && goliathPlayer(rdram, ctx, runtime, m) && READ32(m + kPlayerNum) != 2u)
        {
            ret(ctx, 0);
            return;
        }
        g_origLookAt(rdram, ctx, runtime);
    }
    // Jelly's shock (StateUltraTazer, Monster+0x11130): retail's transitionOK only checks the anims and isSpecialAvailable
    // (m_specialWeapon), with no button; in the final stage FinalBoss::updateJelly's script decides. With Jelly as a player the shock
    // fired on its own: here it only starts with Triangle+Circle held on the player's pad (inputGetPadButtons for the pad at the
    // position in m_monsters[0..3]; it is the special-move combo of regular monsters in layout A, PadEntry +0x36, so Jelly
    // stays still and transitionOK
    // keeps being polled). Jelly under AI (m_playerNum 2) and the adventure one follow retail.
    constexpr uint32_t kTazerOK = wotm::fn::transitionOK__15StateUltraTazer, kPadButtons = wotm::fn::inputGetPadButtons__Fi;
    constexpr uint32_t kBtnTriangle = 0x1000, kBtnCircle = 0x2000;   // libpad (inputGetPadButtons already inverts the active-low bits)
    constexpr uint32_t kTazerUpdate = wotm::fn::update__15StateUltraTazer;
    constexpr float kTazerReach = 1200.f, kTazerKeep = 1500.f;   // nearest enemy distance to start and to keep the shock going
    PS2Runtime::RecompiledFunction g_origTazerOK = nullptr, g_origTazerUpdate = nullptr;
    bool g_tazerHeld[4] = {};   // Triangle+Circle already held on the previous targetless transitionOK call (per player)

    // Nearest living, active monster (m_playerNum 1 or 2) in another slot to `m`, at most `maxDist` away; 0 if none
    uint32_t nearestEnemy(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t m, float maxDist, float *dist)
    {
        const uint32_t cs = READ32(m + kCs), slots = std::min(16u, READ32(kGameObj + kNumSlots));
        if (!cs)
            return 0;
        float p[3], q[3], best = maxDist * maxDist;
        uint32_t near = 0;
        std::memcpy(p, rdram + ((cs + kTrans) & 0x01FFFFFFu), 12);
        for (uint32_t i = 0; i < slots; ++i)
        {
            const uint32_t o = kGameObj + kSlots + i * kSlotStride, ocs = READ32(o + kCs), pn = READ32(o + kPlayerNum);
            if (o == m || !ocs || (pn != 1u && pn != 2u) || READ8(o + kDead))
                continue;
            std::memcpy(q, rdram + ((ocs + kTrans) & 0x01FFFFFFu), 12);
            const float d = (q[0] - p[0]) * (q[0] - p[0]) + (q[1] - p[1]) * (q[1] - p[1]) + (q[2] - p[2]) * (q[2] - p[2]);
            if (d < best)
                best = d, near = o;
        }
        if (dist)
            *dist = std::sqrt(best);
        return near;
    }

    void tazerOK(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = ctx->pc == kTazerOK ? READ32(GPR_U32(ctx, 4) + kStateOwner) : 0;
        int player = -1;
        for (uint32_t i = 0; m && i < 4 && player < 0; ++i)
            if (READ32(kGameObj + kMonsters + 4 * i) == m)
                player = int(i);
        if (player >= 0 && READ32(m + kTypeBits) == kJelly && READ32(m + kPlayerNum) != 2u)
        {
            const uint32_t b = wotm_modes4::call(rdram, ctx, runtime, kPadButtons, {uint32_t(player)});
            float d = 0.f;
            const uint32_t target = (b & (kBtnTriangle | kBtnCircle)) == (kBtnTriangle | kBtnCircle) ? nearestEnemy(rdram, ctx, runtime, m, kTazerReach, &d) : 0;
            if (!target)
            {
                const bool pressed = (b & (kBtnTriangle | kBtnCircle)) == (kBtnTriangle | kBtnCircle);
                if (pressed && !g_tazerHeld[player])   // one notice per press (transitionOK runs every frame)
                {
                    float far = 0.f;
                    nearestEnemy(rdram, ctx, runtime, m, 1e6f, &far);
                    std::fprintf(stderr, "[custom] Jelly: shock with no enemy within %.0f (nearest at %.0f)\n", kTazerReach, far);
                }
                g_tazerHeld[player] = pressed;
                ret(ctx, 0);
                return;
            }
            g_origTazerOK(rdram, ctx, runtime);
            if (GPR_U32(ctx, 2))
                std::fprintf(stderr, "[custom] Jelly: shock target type %u at %.0f\n", READ32(target + kTypeBits) >> 5, d);
            return;
        }
        g_origTazerOK(rdram, ctx, runtime);
    }

    // Shock target: StateUltraTazer::update aims at game->m_monsters[0] (in the final stage, the player), with no distance limit:
    // beams from Jelly's locators to the target's, push and hit (dispatchHit). With Jelly as a player the target was itself. During
    // the call, m_monsters[0] is set to the nearest living enemy (any active slot, player or AI); with nobody within kTazerKeep, it is
    // Jelly itself with m_dead set, and update exits to Idle as it does when the target is dead.
    void tazerUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = ctx->pc == kTazerUpdate ? READ32(GPR_U32(ctx, 4) + kStateOwner) : 0;
        if (!m || READ32(m + kTypeBits) != kJelly || !isPlayerSlot(rdram, ctx, runtime, m) || READ32(m + kPlayerNum) == 2u)
        {
            g_origTazerUpdate(rdram, ctx, runtime);
            return;
        }
        const uint32_t target = nearestEnemy(rdram, ctx, runtime, m, kTazerKeep, nullptr), p1 = READ32(kGameObj + kMonsters);
        const uint8_t dead = READ8(m + kDead);
        WRITE32(kGameObj + kMonsters, target ? target : m);
        if (!target)
            WRITE8(m + kDead, 1);
        g_origTazerUpdate(rdram, ctx, runtime);
        WRITE32(kGameObj + kMonsters, p1);
        if (!target)
            WRITE8(m + kDead, dead);
    }
    // The shock uses up the special (StateUltraTazer::update clears m_specialWeapon), and PowerUps::TakeHit doesn't let types
    // 0x1C0/0x1E0 pick up the special item (in the final stage the script turns it on). With Jelly as a player, during the call its
    // type is set to Togera's (0x60): it picks up the item like a regular monster. The function only reads the type in those tests
    // (health, stamina, special).
    constexpr uint32_t kPowerUpHit = wotm::fn::TakeHit__8PowerUpsP7MonsterP3_cs, kTypeStandIn = 3u << 5;
    PS2Runtime::RecompiledFunction g_origPowerUpHit = nullptr;

    void powerUpHit(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = ctx->pc == kPowerUpHit ? GPR_U32(ctx, 4) : 0;
        const bool jelly = m && READ32(m + kTypeBits) == kJelly && isPlayerSlot(rdram, ctx, runtime, m) && READ32(m + kPlayerNum) != 2u;
        if (jelly)
            WRITE32(m + kTypeBits, kTypeStandIn);
        g_origPowerUpHit(rdram, ctx, runtime);
        if (jelly)
            WRITE32(m + kTypeBits, kJelly);
    }
} // namespace wotm_custom

// After wotmSkinsInstall: wraps whatever SetAIMonster hooks are already registered (WOTM_AIVSAI, skins).
void wotmCustomInstall(PS2Runtime &runtime)
{
    using namespace wotm_custom;
    if ((g_origSetAI = runtime.lookupFunction(kSetAI)))
        runtime.registerFunction(kSetAI, &setAI);
    if ((g_origTakeHit = runtime.lookupFunction(kTakeHit)))
        runtime.registerFunction(kTakeHit, &takeHit);
    if ((g_origSetPov = runtime.lookupFunction(kSetPov)))
        runtime.registerFunction(kSetPov, &setPov);
    g_loadMech = runtime.lookupFunction(kLoadMech);
    g_killPickup = runtime.lookupFunction(kKillPickup);
    if ((g_origImpale = runtime.lookupFunction(kImpale)))
        runtime.registerFunction(kImpale, &impale);
    if ((g_origTazerOK = runtime.lookupFunction(kTazerOK)))
        runtime.registerFunction(kTazerOK, &tazerOK);
    if ((g_origTazerUpdate = runtime.lookupFunction(kTazerUpdate)))
        runtime.registerFunction(kTazerUpdate, &tazerUpdate);
    if ((g_origPowerUpHit = runtime.lookupFunction(kPowerUpHit)))
        runtime.registerFunction(kPowerUpHit, &powerUpHit);
    if ((g_origLookAt = runtime.lookupFunction(kLookAt)))
        runtime.registerFunction(kLookAt, &lookAt);
    if ((g_origCrowdOK = runtime.lookupFunction(kCrowdOK)))
        runtime.registerFunction(kCrowdOK, &crowdOK);
    if ((g_origEnterState = runtime.lookupFunction(kEnterState)))
        runtime.registerFunction(kEnterState, &enterState);
    if ((g_origSpinUpdate = runtime.lookupFunction(kSpinUpdate)))
        runtime.registerFunction(kSpinUpdate, &spinUpdate);
    if ((g_origSetBossState = runtime.lookupFunction(kSetBossState)))
        runtime.registerFunction(kSetBossState, &setBossState);
    if ((g_origLoadBank = runtime.lookupFunction(kLoadBank)))
        runtime.registerFunction(kLoadBank, &loadBank);
    if ((g_origLoadLoBank = runtime.lookupFunction(kLoadLoBank)))
        runtime.registerFunction(kLoadLoBank, &loadLoBank);
    std::fprintf(stderr, "[custom] playable bosses (Goliath, Jelly, Cerebulon): %s, %s\n", g_origSetAI ? "visible with AIs" : "NO SetAIMonster hook",
                 g_origTakeHit ? "takes damage from AIs" : "NO takeHit hook");
}
