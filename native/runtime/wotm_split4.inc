// 3- and 4-player split screen (NATIVO.md, "3 and 4 player split screen"). Included by wotm_overrides.cpp. Always installed: acts when the Shell
// has 3 or 4 players (m_numPlayers), set by the multiplayer menu (wotm_padsel.inc) or by WOTM_MATCH with WOTM_PLAYERS=3 or 4.
//
// The engine already has almost everything for 4 views: viewInfo, worldCtx and viewDb with 5 entries (4 is the shadow's), 4 cameras
// (Cameras::m_cameras), 4 HUDs, the particles' viewPrevMat and the sight points with 5, and the cinematics' viewToggleSplitScreen
// does nothing with 3 or more views. What's missing, by hook:
//   - Views (exit of TheGame::gameReestablishViews in modes 3, 6 and 7 with 3 or 4 players): 320 x 112 GS quadrants, view
//     v in column v & 1 and row v >> 1. With 3 players view 2 is the whole bottom row (640 x 112). Retail has 8 viewDefs
//     (full, halves, menus, unified, shadow, load); the quadrant ones live at the end of upper RAM (kQuadDefs),
//     copied from the half on the same side with the row's height, origin and center, and half the fullscreen screen distance (the FOV of the
//     640 x 224). The 640 x 112 strip comes from viewDef[0] with the same height math and screen distance kStripFov (between the
//     quadrants', which would give ~118 degrees horizontally, and fullscreen's, which would cut the monster in half vertically).
//     viewSetFov rebuilds planes and matrices; the viewDb scissor goes along (only the emulated GS uses it). viewInfo[v].viewport stays 1 or 2:
//     viewGetScreenDisplay keeps saying "split screen" (the cameras use its POV table). m_numCameras = players.
//   - viewGrappleConfig (end of rtMain and unified view switch) resets view 0 to viewDef[viewport]; on exit the quadrant comes back.
//   - No unified view: SetOkToUnify at the start of each frame clears everyone's m_camUnify (InitCCCamera* returns without unifying;
//     camera 2 is player 3's). The victory pose (StateVictory) still turns on gUseUnifiedView with camera 2 on the winner; when the
//     screen splits again, cameras 2 and 3 go back to players 3 and 4 on POV 0. ResetLevel puts camera 2 on POV 3 (retail's
//     unified one); here it stays on the POV it had.
//   - 4 players: SetAIMonster puts each AI on camera 3 (gameInitCamera(3, ai), a camera with no view in retail). Here camera 3 is
//     player 4's: the AI only gets m_cameraView 3, without the camera (m_cameraFollows stays 0, HUD 3 doesn't get its messages).
//   - Load: Shell::LoadMonstersDB builds players 3 and 4's path with player 2's monster, without costume (a retail
//     quirk). From LoadLevelFiles to the entry of InitPlayers they go through the AI path (m_monsterSel[4..], m_costume[2..],
//     the same file) and become players again in InitPlayers; the load order is that of its duplicate count (players, then AIs).
//     Players 3 and 4's costumes have no place in the Shell: they live in g_extraCostume, and SetPlayerMonster gets them instead of
//     the m_costume[1] that InitPlayers sends to every player after player 1.
//   - Motion blur: no hook; MotionBlurAA's bss has packets for 4 views ([2][2][4][0x100] at 0x7319F0, NATIVO.md, "3 and 4 player split screen").
//   - HUD: Hud::initAfter and Hud::update return immediately with index >= 2. HUD 2/3 becomes a copy of the HUD in the same column and runs
//     update like it, with its own view's monster, camera and bar (hudInitAfter, hudUpdate).
//   - Camera on POV 3 (parked): cameras 2 and 3 had no park position and went to the origin (at the start and on death).
//   - End of round in mode 3: retail only respawns with 1 or 0 alive (2 players); here each dead monster respawns as what it is. The win
//     target opens dialog 5 (modal) as a tail call of Evaluate, with its caller's ra.
//   - End of victory (LeaveUnifiedView): the 2-player path (camera 0 doesn't jump to the victory camera).
//   - Respawning more than 4 monsters (list 3 with 4 points, "play again"): whoever lands on top of another goes to list 2.
//   - End of an AI throw (StateThrowBack::handlePreemption): doesn't touch camera 3, which is player 4's.
//   - Pads: inputInit opens 2 ports x 4 slots (pad = port + 2 * slot: player 3 is slot 1 of port 0, player 4 slot 1 of port
//     1); the runtime's pad stub has the multitap slots (tools/runtime_patch/wotm-runtime.patch), no hook here.
//   - 5 to 8 players (WOTM_PLAYERS=5..8): the first 4 in the quadrants, the rest as AIs driven by pads 4..7 (wotm_multi.inc).

float wotmScreenWide();              // wotm_live.inc: window width over the width of the game's 4:3 screen

#include "wotm_training.h"   // training mode: dummy's HUD (trainingHud)

namespace wotm_split4
{
    static_assert(PS2_RAM_SIZE > PS2_GUEST_RAM_SIZE, "the quadrant viewDefs live above the game RAM");

    constexpr uint32_t kShell = wotm::sym::shellInfo, kGame = wotm::sym::shellGame;
    constexpr uint32_t kMonsterSel = kShell + 0x2938, kNumPlayers = kShell + 0x2BB4, kNumAIs = kShell + 0x2BB8, kCostume = kShell + 0x2BE0;
    constexpr uint32_t kSlots = kGame + 0xB80, kSlotStride = 0x11190, kMonsters = kGame + 0x120380, kGameMode = kGame + 0x1203C8,
                       kNumSlots = kGame + 0x1203D4;
    constexpr uint32_t kPlayerNum = 0x18, kNumInits = 0x2C, kCameraView = 0x6CD8, kCamUnify = 0x7970;   // Monster
    constexpr uint32_t kViewInfo = wotm::sym::viewInfo, kViewDef = wotm::sym::viewDef, kViewDb = wotm::sym::viewDb,
                       kNumViews = wotm::sym::viewNumViews, kUnified = wotm::sym::gUseUnifiedView;
    constexpr uint32_t kCameras = wotm::sym::x_7Cameras_m_cameras, kNumCameras = wotm::sym::x_7Cameras_m_numCameras, kCamStride = 0xEB0,
                       kCamCs = 0xCF0, kCamMonster = 0xCF4, kCamPov = 0xCF8;
    constexpr uint32_t kDefSize = 0xC0, kDbSize = 0x360;
    constexpr uint32_t kDbScissor[4] = {0xC0, 0x140, 0x230, 0x2B0};
    constexpr uint32_t kQuadDefs = PS2_RAM_SIZE - 0x1000;   // the 4 quadrant ViewDefs

    constexpr uint32_t kReestablish = wotm::fn::gameReestablishViews__7TheGame, kGrapple = wotm::fn::viewGrappleConfig__Fv,
                       kOkToUnify = wotm::fn::SetOkToUnify__7TheGame, kResetLevel = wotm::fn::ResetLevel__7TheGame,
                       kInitCamera = wotm::fn::gameInitCamera__7TheGameii, kLoadLevelFiles = wotm::fn::LoadLevelFiles__5Shell,
                       kInitPlayers = wotm::fn::InitPlayers__5Shell, kViewCreate = wotm::fn::viewCreate__F10_viewportsi,
                       kViewSetFov = wotm::fn::viewSetFov__Fiff, kFollow = wotm::fn::SetCameraToFollowMonster__7CamerasiP7Monster,
                       kSetPov = wotm::fn::SetCameraPOV__7CamerasiQ26Camera9CameraPOV;
    PS2Runtime::RecompiledFunction g_origReestablish = nullptr, g_origGrapple = nullptr, g_origResetLevel = nullptr, g_origInitCamera = nullptr,
                                   g_origLoadLevelFiles = nullptr, g_origInitPlayers = nullptr, g_viewCreate = nullptr, g_viewSetFov = nullptr,
                                   g_follow = nullptr, g_setPov = nullptr;

    int g_players = 0;              // player cap with the hooks installed (4); 0 = off
    int g_views = 0;                // quadrant views built (0 = none: retail fullscreen or halves)
    bool g_wasUnified = false;      // the unified view (victory) was on in the previous frame
    bool g_cinemaFull = false;      // fullscreen cinematic (cinemaViews)
    unsigned g_hudCalls[4] = {};    // diagnostic (WOTM_SPLITLOG): Hud::update of each HUD since the last line
    unsigned g_hudPaused = 0;       // diagnostic (WOTM_SPLITLOG): HUD 2/3 updates paused midway since the last line
    int g_loadExtra = 0;            // players 3 and 4 on the AI path (from LoadLevelFiles to InitPlayers)
    uint32_t g_extraCostume[2] = {0, 0};   // players 3 and 4's costumes (the Shell has no place for them)

    float rdF(uint8_t *rdram, uint32_t a) { float v; std::memcpy(&v, rdram + a, 4); return v; }
    void wrF(uint8_t *rdram, uint32_t a, float v) { std::memcpy(rdram + a, &v, 4); }
    uint32_t sel(int i) { return kMonsterSel + 4u * uint32_t(i); }
    uint32_t costume(int i) { return kCostume + 4u * uint32_t(i); }
    uint32_t camera(int v) { return kCameras + kCamStride * uint32_t(v); }

    // Calls a game function with the context saved (like callTakeDamage): a0, a1 integers, f12 and f13 float
    void callGuest(PS2Runtime::RecompiledFunction fn, uint32_t addr, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t a0,
                   uint32_t a1 = 0, float f12 = 0.f, float f13 = 0.f)
    {
        if (!fn)
            return;
        const R5900Context saved = *ctx;
        ctx->r[4] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a0)));
        ctx->r[5] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a1)));
        ctx->r[31] = _mm_set_epi64x(0, 0);
        ctx->f[12] = f12;
        ctx->f[13] = f13;
        ctx->pc = addr;
        fn(rdram, ctx, runtime);
        *ctx = saved;
    }

    // Players on the quadrant screen right now (0 = not applicable): 3 or 4 in the Shell, in one of the split-screen modes (3, 6 and 7)
    int splitPlayers(uint8_t *rdram)
    {
        const int32_t n = static_cast<int32_t>(FAST_READ32(kNumPlayers)), mode = static_cast<int32_t>(FAST_READ32(kGameMode));
        return g_players && n >= 3 && n <= 4 && (mode == 3 || mode == 6 || mode == 7) ? n : 0;
    }

    constexpr float kStripFov = 2.f / 3.f;   // screen distance of the 640 x 112 strip (3 players), as a fraction of fullscreen's

    void writeQuadDefs(uint8_t *rdram, int n)
    {
        const float full = rdF(rdram, kViewDef);   // viewDef[0]: 384 at 640 wide; the quadrant is 320
        for (uint32_t v = 0; v < 4; ++v)
        {
            const bool strip = n == 3 && v == 2;   // 3 players: view 2 takes the whole bottom row
            const uint32_t src = strip ? kViewDef : kViewDef + kDefSize * (1u + (v & 1u)), dst = kQuadDefs + kDefSize * v;   // left or right half
            std::memcpy(rdram + dst, rdram + src, kDefSize);
            const int32_t h = FAST_READ16(src + 6) / 2, sign = (v >> 1) ? 1 : -1;
            FAST_WRITE16(dst + 6, static_cast<uint16_t>(h));                          // h
            FAST_WRITE16(dst + 0xE, static_cast<uint16_t>((v >> 1) * uint32_t(h)));   // scissor y
            wrF(rdram, dst + 0x14, rdF(rdram, src + 0x14) + float(sign * h / 2));    // offY: 2048 -/+ 56
            FAST_WRITE32(dst + 0x1C, static_cast<uint32_t>(static_cast<int32_t>(FAST_READ32(src + 0x1C)) + sign * h / 2));   // cy
            wrF(rdram, dst, strip ? full * kStripFov : full * 0.5f);
        }
    }

    // View v with viewDef `def`: planes and matrices (viewSetFov) and the viewDb scissor
    void applyDef(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t v, uint32_t def)
    {
        FAST_WRITE32(kViewInfo + 0x14u * v + 4, def);
        callGuest(g_viewSetFov, kViewSetFov, rdram, ctx, runtime, v, 0, rdF(rdram, def), rdF(rdram, def + 0x20));
        const uint64_t x = FAST_READ16(def + 0xC), y = FAST_READ16(def + 0xE), w = FAST_READ16(def + 4), h = FAST_READ16(def + 6);
        const uint64_t s = x | ((x + w - 1) << 16) | (y << 32) | ((y + h - 1) << 48);
        for (uint32_t off : kDbScissor)
            FAST_WRITE64(kViewDb + kDbSize * v + off, s);
    }
    // View v in its quadrant
    void applyQuad(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t v)
    {
        applyDef(rdram, ctx, runtime, v, kQuadDefs + kDefSize * v);
    }

    // Cameras 2 and 3 back to players 3 and 4, on POV 0 (the victory left them on the winner, POV 7)
    void refollow(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        for (int v = 2; v < g_views; ++v)
        {
            const uint32_t m = FAST_READ32(kMonsters + 4u * uint32_t(v));
            if (!m)
                continue;
            if (FAST_READ32(camera(v) + kCamMonster) == m)
            {
                // LeaveUnifiedView puts camera 2 on POV 3 (unified rest): with player 3 as the winner it already follows them
                if (FAST_READ32(camera(v) + kCamPov) == 3u)
                    callGuest(g_setPov, kSetPov, rdram, ctx, runtime, uint32_t(v), 0u);
                continue;
            }
            FAST_WRITE32(camera(v) + kCamPov, 0);   // SetCameraToFollowMonster reloads the current POV
            callGuest(g_follow, kFollow, rdram, ctx, runtime, uint32_t(v), m);
        }
    }

    void reestablishViews(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = ctx->pc == kReestablish ? GPR_U32(ctx, 31) : 0u;
        g_origReestablish(rdram, ctx, runtime);
        if (!ra || ctx->pc != ra)
            return;
        const int n = splitPlayers(rdram);
        if (!n)
        {
            g_views = 0;
            return;
        }
        writeQuadDefs(rdram, n);
        FAST_WRITE32(kNumViews, 4u);   // viewCreate paints the background of viewNumViews viewDbs
        callGuest(g_viewCreate, kViewCreate, rdram, ctx, runtime, 1u, 2u);   // view 2: retail leaves it on viewport 5 (unified)
        if (n == 4)
            callGuest(g_viewCreate, kViewCreate, rdram, ctx, runtime, 2u, 3u);   // view 3: _cs and viewDb (retail doesn't create it)
        for (int v = 0; v < n; ++v)
            applyQuad(rdram, ctx, runtime, uint32_t(v));
        FAST_WRITE32(kNumViews, uint32_t(n));
        FAST_WRITE32(kNumCameras, uint32_t(n));
        if (n == 4)
            callGuest(g_setPov, kSetPov, rdram, ctx, runtime, 3u, FAST_READ32(camera(3) + kCamPov));   // the original's loop went up to 3
        if (g_views != n)
            std::fprintf(stderr, "[split] %d players: views in 320 x 112 quadrants, %d cameras\n", n, n);
        g_views = n;
        g_cinemaFull = false;
        g_wasUnified = false;   // the players' cameras already come from InitPlayers (gameInitCamera(i, slot))
        // camera 2 leaves InitBeforeDbLoad on POV 3 (unified rest); with a player to follow, it goes to POV 0 like the others
        for (int v = 2; v < n; ++v)
            if (FAST_READ32(camera(v) + kCamPov) == 3u && FAST_READ32(camera(v) + kCamMonster))
                callGuest(g_setPov, kSetPov, rdram, ctx, runtime, uint32_t(v), 0u);
    }

    // Cameras::SetCameraPOV(view, pov): POV 3 is the camera parked at +0xD20 (DetachCamera; TogglePOV toggles 0 and 3). Retail
    // cameras 2 and 3 never get that position (2 is the unified one, which InitBeforeDbLoad and ResetLevel put on POV 3 only as a
    // rest): on POV 3 they went to the map origin (a quadrant of nothing but fog, at the start and on the player's death). With quadrants,
    // POV 3 on one of them parks where it is (_cs->trans), like DetachCamera, which also puts the position in the _cs first.
    constexpr uint32_t kCamPark = 0xD20;
    PS2Runtime::RecompiledFunction g_origSetPov = nullptr;
    bool g_skipPov3 = false;   // an AI's throwPreemption in progress: SetCameraPOV for camera 3 (player 4's) doesn't happen
    void setCameraPov(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t v = GPR_U32(ctx, 4);
        if (ctx->pc == kSetPov && v == 3u && g_skipPov3)
        {
            ctx->pc = GPR_U32(ctx, 31);
            return;
        }
        if (ctx->pc == kSetPov && GPR_U32(ctx, 5) == 3u && v >= 2u && v < 4u && g_views > int(v))
        {
            const uint32_t c = camera(int(v)), cs = FAST_READ32(c + kCamCs) & 0x1FFFFFFFu;
            if (cs >= 0x1000u && cs + 0x20u <= PS2_RAM_SIZE)
                std::memcpy(rdram + c + kCamPark, rdram + cs + 0x10, 16);   // _cs->trans
        }
        g_origSetPov(rdram, ctx, runtime);
    }

    // Cameras::LeaveUnifiedView (end of the victory pose, "play again", ResetLevel): with m_numMonsters 2 (two players) retail only
    // returns camera 2 to POV 3 and fades out (CCFadeOut); with any other number (1 player) camera 0 takes the place of 2 (the victory one) and
    // redoes the transition from there: with quadrants (m_numMonsters = players and AIs) player 1's camera would jump to where
    // the victory camera was. The call sees 2 and takes the split screen path; refollow returns cameras 2 and 3.
    constexpr uint32_t kLeaveUnified = wotm::fn::LeaveUnifiedView__7CamerasUi, kNumMonsters = kGame + 0x1203D8;
    PS2Runtime::RecompiledFunction g_origLeaveUnified = nullptr;
    void leaveUnified(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != kLeaveUnified || !g_views || !splitPlayers(rdram))
        {
            g_origLeaveUnified(rdram, ctx, runtime);
            return;
        }
        const uint32_t n = FAST_READ32(kNumMonsters), ra = GPR_U32(ctx, 31);
        FAST_WRITE32(kNumMonsters, 2u);
        g_origLeaveUnified(rdram, ctx, runtime);
        if (ctx->pc == ra && FAST_READ32(kNumMonsters) == 2u)
            FAST_WRITE32(kNumMonsters, n);
    }

    // StateThrowBack::handlePreemption (end of being thrown/knocked down): puts the owner's m_cameraView camera on POV 0 and clears its +0xD98 and
    // +0xD9C, without checking whether the owner is human. With 4 players the AI has m_cameraView 3 (initCamera) and touched player 4's camera
    // (in retail camera 3 has no view). Here, with an AI owner on view 3: SetCameraPOV(3) doesn't happen and both fields are restored.
    constexpr uint32_t kThrowPreempt = wotm::fn::handlePreemption__14StateThrowBackP12MonsterState, kStateOwner = 0xC;
    PS2Runtime::RecompiledFunction g_origThrowPreempt = nullptr;
    void throwPreemption(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
        uint32_t owner = 0;
        if (ctx->pc == kThrowPreempt && self >= 0x1000u && self + 0x10u <= PS2_RAM_SIZE && splitPlayers(rdram) == 4)
            owner = FAST_READ32(self + kStateOwner) & 0x1FFFFFFFu;
        bool ai = owner >= 0x1000u && owner + kCameraView + 4u <= PS2_RAM_SIZE && FAST_READ32(owner + kCameraView) == 3u;
        for (uint32_t i = 0; i < 4 && ai; ++i)
            ai = (FAST_READ32(kMonsters + 4u * i) & 0x1FFFFFFFu) != owner;
        if (!ai)
        {
            g_origThrowPreempt(rdram, ctx, runtime);
            return;
        }
        const uint32_t c = camera(3), d98 = FAST_READ32(c + 0xD98), d9c = FAST_READ32(c + 0xD9C);
        g_skipPov3 = true;
        g_origThrowPreempt(rdram, ctx, runtime);
        g_skipPov3 = false;
        FAST_WRITE32(c + 0xD98, d98);
        FAST_WRITE32(c + 0xD9C, d9c);
    }

    // Cinematic (ActionDispatch::doCinema, moviePlaying; the monster intro before the fight): with 2 players doCinema calls
    // viewToggleSplitScreen and the scene plays fullscreen; with 3 or more views the toggle does nothing and only view 0 followed the cinematic, in
    // its quadrant. Here, at the start of each frame (okToUnify), a playing cinematic puts the screen in a single view (viewNumViews 1, view 0 with
    // viewDef[0], 640 x 224) and when the cinematic ends it goes back to quadrants; rtMain reads the number of views on the next frame. The cameras
    // of the other views keep following the monsters underneath.
    constexpr uint32_t kMoviePlaying = wotm::sym::moviePlaying;
    void cinemaViews(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool movie = FAST_READ32(kMoviePlaying) != 0 && !FAST_READ32(kUnified);
        if (movie == g_cinemaFull)
            return;
        g_cinemaFull = movie;
        if (movie)
        {
            FAST_WRITE32(kNumViews, 1u);
            applyDef(rdram, ctx, runtime, 0u, kViewDef);
        }
        else
        {
            // endCinema calls viewToggleSplitScreen, which with 1 view recreates 0 and 1 as halves (viewCreate): all of them back
            FAST_WRITE32(kNumViews, uint32_t(g_views));
            for (int v = 0; v < g_views; ++v)
            {
                FAST_WRITE32(kViewInfo + 0x14u * uint32_t(v) + 8, 1u + (uint32_t(v) & 1u));   // viewport: half on the same side
                applyQuad(rdram, ctx, runtime, uint32_t(v));
            }
        }
    }

    void grappleConfig(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = ctx->pc == kGrapple ? GPR_U32(ctx, 31) : 0u;
        g_origGrapple(rdram, ctx, runtime);
        if (ra && ctx->pc == ra && g_views && !FAST_READ32(kUnified) && splitPlayers(rdram))
        {
            if (g_cinemaFull)
                applyDef(rdram, ctx, runtime, 0u, kViewDef);
            else
                applyQuad(rdram, ctx, runtime, 0u);
        }
    }

    // Start of each rtMain frame (gameSetOkToUnify): no unifying; the camera goes back to the players when the victory ends
    PS2Runtime::RecompiledFunction g_origOkToUnify = nullptr;
    void okToUnify(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc != kOkToUnify || !g_views || !splitPlayers(rdram))
        {
            g_origOkToUnify(rdram, ctx, runtime);
            return;
        }
        cinemaViews(rdram, ctx, runtime);
        static const bool log = std::getenv("WOTM_SPLITLOG") != nullptr;   // one line per second: each view's camera
        static unsigned frames = 0;
        if (log && ++frames % 60 == 0)
        {
            std::fprintf(stderr, "[split] cameras:");
            for (int v = 0; v < g_views; ++v)
            {
                const uint32_t cs = FAST_READ32(camera(v) + kCamCs), vcs = FAST_READ32(kViewInfo + 0x14u * uint32_t(v));
                std::fprintf(stderr, " %d: monster %x (player %x) pov %d cs %x view %x at %.0f,%.0f,%.0f;", v, FAST_READ32(camera(v) + kCamMonster),
                             FAST_READ32(kMonsters + 4u * uint32_t(v)), static_cast<int32_t>(FAST_READ32(camera(v) + kCamPov)), cs, vcs,
                             rdF(rdram, (vcs & 0x1FFFFFFFu) + 0x10), rdF(rdram, (vcs & 0x1FFFFFFFu) + 0x14), rdF(rdram, (vcs & 0x1FFFFFFFu) + 0x18));
            }
            std::fprintf(stderr, " hud.update %u/%u/%u/%u (paused %u)\n", g_hudCalls[0], g_hudCalls[1], g_hudCalls[2], g_hudCalls[3], g_hudPaused);
            g_hudPaused = 0;
            for (unsigned &n : g_hudCalls)
                n = 0;
        }
        const uint32_t slots = std::min<uint32_t>(FAST_READ32(kNumSlots), 16u);
        for (uint32_t i = 0; i < slots; ++i)
            FAST_WRITE32(kSlots + kSlotStride * i + kCamUnify, 0);
        if (FAST_READ32(kUnified))
            g_wasUnified = true;
        else if (g_wasUnified)
        {
            g_wasUnified = false;
            refollow(rdram, ctx, runtime);
        }
        ctx->pc = GPR_U32(ctx, 31);
    }

    void resetLevel(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool keep = ctx->pc == kResetLevel && g_views && splitPlayers(rdram);
        const uint32_t ra = GPR_U32(ctx, 31), pov = FAST_READ32(camera(2) + kCamPov);
        g_origResetLevel(rdram, ctx, runtime);
        if (keep && ctx->pc == ra && FAST_READ32(camera(2) + kCamPov) != pov)
            callGuest(g_setPov, kSetPov, rdram, ctx, runtime, 2u, pov);
    }

    // TheGame::gameInitCamera(view, slot): with 4 players camera 3 stays with player 4 (the AI doesn't take it)
    void initCamera(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kInitCamera && GPR_U32(ctx, 5) == 3u && splitPlayers(rdram) == 4)
        {
            // player = one of m_monsters[0..3] (SetPlayerMonster already stored it); m_playerNum is no use: with WOTM_AIVSAI the player
            // also has 2
            const uint32_t slot = GPR_U32(ctx, 6);
            const uint32_t m = kSlots + kSlotStride * slot;
            bool player = false;
            for (uint32_t i = 0; i < 4; ++i)
                player = player || FAST_READ32(kMonsters + 4u * i) == m;
            if (slot < 16u && !player)
            {
                FAST_WRITE32(m + kCameraView, 3u);
                ctx->pc = GPR_U32(ctx, 31);
                return;
            }
        }
        g_origInitCamera(rdram, ctx, runtime);
    }

    // Players 3 and 4 onto the AI path (load) and back (InitPlayers). The AIs shift by extra positions; m_costume[2 + j] goes up to
    // index 11 (0x2C0C, before m_mode): at most 10 between extra players and AIs.
    void toLoadLayout(uint8_t *rdram)
    {
        const int n = static_cast<int32_t>(FAST_READ32(kNumPlayers)), extra = n - 2;
        int ais = std::clamp(static_cast<int32_t>(FAST_READ32(kNumAIs)), 0, 10 - extra);
        for (int j = ais - 1; j >= 0; --j)
        {
            FAST_WRITE32(sel(4 + j + extra), FAST_READ32(sel(4 + j)));
            FAST_WRITE32(costume(2 + j + extra), FAST_READ32(costume(2 + j)));
        }
        for (int k = 0; k < extra; ++k)
        {
            FAST_WRITE32(sel(4 + k), FAST_READ32(sel(2 + k)));
            FAST_WRITE32(costume(2 + k), g_extraCostume[k]);
        }
        FAST_WRITE32(kNumPlayers, 2u);
        FAST_WRITE32(kNumAIs, uint32_t(ais + extra));
        g_loadExtra = extra;
    }

    void toPlayLayout(uint8_t *rdram)
    {
        const int extra = g_loadExtra, ais = std::max(0, static_cast<int32_t>(FAST_READ32(kNumAIs)) - extra);
        for (int k = 0; k < extra; ++k)
            FAST_WRITE32(sel(2 + k), FAST_READ32(sel(4 + k)));
        for (int j = 0; j < ais; ++j)
        {
            FAST_WRITE32(sel(4 + j), FAST_READ32(sel(4 + j + extra)));
            FAST_WRITE32(costume(2 + j), FAST_READ32(costume(2 + j + extra)));
        }
        for (int k = 0; k < extra; ++k)
        {
            FAST_WRITE32(sel(4 + ais + k), 0u);
            FAST_WRITE32(costume(2 + ais + k), 0u);
        }
        FAST_WRITE32(kNumPlayers, uint32_t(2 + extra));
        FAST_WRITE32(kNumAIs, uint32_t(ais));
        g_loadExtra = 0;
    }

    void loadLevelFiles(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kLoadLevelFiles)
        {
            if (g_loadExtra)   // the previous load didn't reach InitPlayers
                toPlayLayout(rdram);
            const int32_t n = static_cast<int32_t>(FAST_READ32(kNumPlayers));
            if (n >= 3 && n <= 4 && FAST_READ32(kShell + 0x2C10) != 6u)   // m_mode 6 loads the 10 monsters on its own
                toLoadLayout(rdram);
        }
        g_origLoadLevelFiles(rdram, ctx, runtime);
    }

    void initPlayers(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kInitPlayers && g_loadExtra)
            toPlayLayout(rdram);
        g_origInitPlayers(rdram, ctx, runtime);
    }

    // TheGame::SetPlayerMonster(player, type, dup, view, costume): InitPlayers sends m_costume[1] to every player after player 1;
    // players 3 and 4 get their own (the one from the file the load read)
    constexpr uint32_t kSetPlayer = wotm::fn::SetPlayerMonster__7TheGameiiiii;
    PS2Runtime::RecompiledFunction g_origSetPlayer = nullptr;
    void setPlayerMonster(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t player = GPR_U32(ctx, 5);
        if (ctx->pc == kSetPlayer && player >= 2u && player < 4u && splitPlayers(rdram))
            ctx->r[9] = _mm_set_epi64x(0, static_cast<int64_t>(g_extraCostume[player - 2u]));   // t1
        g_origSetPlayer(rdram, ctx, runtime);
    }

    // Hud::update(hud): retail returns right at the start with index (+0) >= 2. The rest uses the index for the monster (m_viewSlot[index]), the
    // camera (m_cameras[index], _cs at +0xCF0), the text side (0 on the left, the others on the right) and the drawMe of the health bars (the
    // _cs at +0xF4 of HUDs 0 and 1: turns on its own index's and turns off the other's; the HUD's own bar is placed in front of the camera). With
    // quadrants, HUD v = 2 or 3 runs as HUD p = v & 1 (same column): during the call its index becomes p and m_viewSlot[p],
    // camera p's _cs and HUD p's bar become view v's; everything is restored on exit. Afterwards, only this HUD's bar stays on
    // (rtMain calls each view's update right before its hier). With m_numMonsters != 2 the bar position is the fullscreen one
    // (+0x100 in the HUD), which is the quadrant's shape.
    constexpr uint32_t kHudUpdate = wotm::fn::update__3Hud, kHudStride = 0x2E0, kHudBarCs = 0xF4, kViewSlot = kGame + 0x1203E8;
    PS2Runtime::RecompiledFunction g_origHudUpdate = nullptr;
    uint32_t hudBar(uint8_t *rdram, uint32_t i)
    {
        const uint32_t cs = FAST_READ32(kGame + kHudStride * i + kHudBarCs) & 0x1FFFFFFFu;
        return cs >= 0x1000u && cs + 0x10u <= PS2_RAM_SIZE ? cs : 0u;
    }
    // Per-view HUD animations. HUD 2/3 is a copy of the column's HUD: the _animHandles (+0x130..+0x1F0) point to the same
    // _animCharInstances, so what one starts the other shows (player 3's h180 rage flash, set by updateBoostAndRage, and its
    // end appearing and disappearing on player 1's HUD). Each HUD keeps here the state of the column's instances: of the control nodes
    // (animCtx[k] +0x14..+0x34: start, time, speed, active, loop, iterations, key cache), the AnimPlayer's main tree
    // (+0x8, only a control node or nothing; animationStart only swaps that pointer) and the output (animOutput.val). Only one is in RAM per
    // column (g_hudLive); hudUse swaps, marking dirty (animOutput.dirty) every channel whose value changed, so hier rebuilds the
    // matrices it cached from the other. The bars (AnimBlend in the active list) stay shared: each update reapplies the percentage.
    // Order within the frame: update 0 (restores state 0), hier 0, animationRunGlobal (evaluates 0), update 1, hier 1, update 2 (saves 0,
    // loads 2 and evaluates it with UpdateAnimations, as animationRunGlobal would), hier 2, update 3, hier 3.
    // Rage bar (AnimBlend at +0x268, over handle +0x170): the blend node lives inside the HUD. update does rampIn when the node
    // isn't active (+0x274) and reapplies its time (toTime, +0x2A0: rage percentage, or speed 1 when full). In the HUD 2/3
    // copy the node is a different one, but the handle is the column HUD's: its rampIn put a second node in the active list of the same AnimPlayer, and
    // both views showed the top layer (player 3's rage on player 1's bar and vice versa). The copy's node stays marked active
    // (never enters the list); its toTime goes to the column's node before evaluation, and the column node's goes into the per-HUD state.
    constexpr uint32_t kHudHandles = 0x130, kHudHandleN = 13, kCtlState = 0x14, kCtlStateLen = 0x20, kCtlSize = 0x3C;
    constexpr uint32_t kHudRageActive = 0x268 + 0x8 + 0x4, kHudRageTime = 0x268 + 0x8 + 0x30, kAnimTimeLen = 0x10;
    constexpr uint32_t kUpdateAnims = wotm::fn::UpdateAnimations__10AnimPlayer;
    struct HudAnimState
    {
        uint32_t ci[4] = {};
        int n = 0;
        std::vector<uint8_t> data;
        uint8_t rageTime[kAnimTimeLen] = {};   // toTime of the column's rage bar blend node
    };
    HudAnimState g_hudAnim[4];
    uint32_t g_hudLive[2] = {0, 1};   // HUD whose state is in RAM, per column
    PS2Runtime::RecompiledFunction g_updateAnims = nullptr;

    uint32_t ciOk(uint8_t *rdram, uint32_t ci)
    {
        ci &= 0x1FFFFFFFu;
        return ci >= 0x1000u && ci + 0x30u <= PS2_RAM_SIZE ? ci : 0u;
    }
    // Distinct instances among HUD i's handles
    int hudInstances(uint8_t *rdram, uint32_t hud, uint32_t out[4])
    {
        int n = 0;
        for (uint32_t k = 0; k < kHudHandleN; ++k)
        {
            const uint32_t ci = ciOk(rdram, FAST_READ32(hud + kHudHandles + 0x10u * k));
            if (!ci || std::find(out, out + n, ci) != out + n)
                continue;
            if (n == 4)
                break;
            out[n++] = ci;
        }
        return n;
    }
    // An instance's fields: AnimPlayer, animCtx, val, dirty, animations and channels (0 in everything if something doesn't make sense)
    struct CiFields { uint32_t player = 0, ctx = 0, val = 0, dirty = 0, anims = 0, chans = 0; };
    CiFields ciFields(uint8_t *rdram, uint32_t ci)
    {
        CiFields f;
        const uint32_t chr = ciOk(rdram, FAST_READ32(ci + 0x20));
        f.player = ciOk(rdram, FAST_READ32(ci + 0x24));
        f.ctx = ciOk(rdram, FAST_READ32(ci + 0x10));
        f.val = ciOk(rdram, FAST_READ32(ci + 4));
        f.dirty = ciOk(rdram, FAST_READ32(ci + 8));
        f.anims = chr ? FAST_READ16(chr) : 0u;
        f.chans = chr ? FAST_READ16(chr + 2) : 0u;
        if (!chr || !f.player || !f.ctx || !f.val || !f.dirty || f.anims > 64u || f.chans > 512u ||
            f.ctx + kCtlSize * f.anims > PS2_RAM_SIZE || f.val + 4u * f.chans > PS2_RAM_SIZE)
            f = CiFields{};
        return f;
    }
    size_t ciBytes(const CiFields &f) { return kCtlStateLen * f.anims + 4u + 4u * f.chans; }

    void hudAnimSave(uint8_t *rdram, uint32_t hud, HudAnimState &s)
    {
        s.n = hudInstances(rdram, hud, s.ci);
        std::memcpy(s.rageTime, rdram + hud + kHudRageTime, kAnimTimeLen);
        s.data.clear();
        for (int i = 0; i < s.n; ++i)
        {
            const CiFields f = ciFields(rdram, s.ci[i]);
            const size_t at = s.data.size();
            s.data.resize(at + ciBytes(f));
            uint8_t *d = s.data.data() + at;
            for (uint32_t k = 0; k < f.anims; ++k, d += kCtlStateLen)
                std::memcpy(d, rdram + f.ctx + kCtlSize * k + kCtlState, kCtlStateLen);
            if (f.player)
                std::memcpy(d, rdram + f.player + 8, 4);
            std::memcpy(d + 4, rdram + f.val, 4u * f.chans);
        }
    }
    // false: the instances changed (another stage); the saved state is invalid
    bool hudAnimLoad(uint8_t *rdram, uint32_t hud, const HudAnimState &s)
    {
        uint32_t ci[4] = {};
        const int n = hudInstances(rdram, hud, ci);
        if (n != s.n || !std::equal(ci, ci + n, s.ci))
            return false;
        std::memcpy(rdram + hud + kHudRageTime, s.rageTime, kAnimTimeLen);
        const uint8_t *d = s.data.data(), *end = d + s.data.size();
        for (int i = 0; i < n; ++i)
        {
            const CiFields f = ciFields(rdram, ci[i]);
            if (d + ciBytes(f) > end)
                return false;
            for (uint32_t k = 0; k < f.anims; ++k, d += kCtlStateLen)
                std::memcpy(rdram + f.ctx + kCtlSize * k + kCtlState, d, kCtlStateLen);
            if (!f.player)
            {
                d += 4u;
                continue;
            }
            // main tree: only a control node of this instance, or nothing (a blend node from the pool would end up in two trees)
            uint32_t main;
            std::memcpy(&main, d, 4);
            const uint32_t m = main & 0x1FFFFFFFu, cur = FAST_READ32(f.player + 8) & 0x1FFFFFFFu;
            const auto ctlNode = [&](uint32_t a) { return !a || (a >= f.ctx && a < f.ctx + kCtlSize * f.anims && (a - f.ctx) % kCtlSize == 0); };
            if (ctlNode(m) && ctlNode(cur))
                FAST_WRITE32(f.player + 8, main);
            for (uint32_t c = 0; c < f.chans; ++c)
            {
                const uint32_t a = f.val + 4u * c;
                if (std::memcmp(rdram + a, d + 4 + 4u * c, 4) != 0)
                {
                    std::memcpy(rdram + a, d + 4 + 4u * c, 4);
                    rdram[f.dirty + c / 8u] |= uint8_t(1u << (c & 7u));
                }
            }
            d += 4u + 4u * f.chans;
        }
        return true;
    }
    // Puts the animation state of HUD `who` (column who & 1) into RAM. First time for HUD 2/3: starts from the column's current state.
    void hudUse(uint8_t *rdram, uint32_t who)
    {
        const uint32_t col = who & 1u, hud = kGame + kHudStride * col, live = g_hudLive[col];
        if (live == who)
            return;
        hudAnimSave(rdram, hud, g_hudAnim[live]);
        HudAnimState &s = g_hudAnim[who];
        if (s.n == 0 || !hudAnimLoad(rdram, hud, s))
            s = g_hudAnim[live];   // new state = the column's (already in RAM)
        g_hudLive[col] = who;
    }
    void hudAnimReset()
    {
        for (HudAnimState &s : g_hudAnim)
            s = HudAnimState{};
        g_hudLive[0] = 0;
        g_hudLive[1] = 1;
    }
    // Evaluates the column's instances with the state in use (animationRunGlobal only runs after view 0's hier)
    void hudEvaluate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t hud)
    {
        uint32_t ci[4] = {};
        const int n = hudInstances(rdram, hud, ci);
        for (int i = 0; i < n; ++i)
            if (const CiFields f = ciFields(rdram, ci[i]); f.player && !(FAST_READ32(ci[i] + 0x1C) & 1u))   // lazyEvaluate
                callGuest(g_updateAnims, kUpdateAnims, rdram, ctx, runtime, f.player);
    }

    // Training mode (wotm_training.h) in a 1-player fight: the dummy's HUD is the game's own HUD 1 (player 2's, mirrored on the
    // right, which the HUD bank already has and initAfter already turned on). On exit from HUD 0's Hud::update, HUD 1's update runs with
    // m_viewSlot[1] swapped for the dummy's slot (the first live m_monsters[i >= 1]) during the call. Outside the unified view and in the
    // versus entry mode update ties HUD i to camera i's _cs (m_cameras[i]); camera 1 follows the dummy, so the call uses camera 0's _cs.
    void trainingHud(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!wotm_training::g_on.load(std::memory_order_relaxed) || !(wotm_training::g_opts.load(std::memory_order_relaxed) & wotm_training::kTrainHud) ||
            FAST_READ32(kNumPlayers) != 1u || splitPlayers(rdram))
            return;
        uint32_t slot = ~0u;
        for (uint32_t i = 1; i < 16 && slot == ~0u; ++i)
        {
            const uint32_t m = FAST_READ32(kMonsters + 4u * i) & 0x1FFFFFFFu;
            if (m >= kSlots && (m - kSlots) % kSlotStride == 0 && (m - kSlots) / kSlotStride < 16u)
                slot = (m - kSlots) / kSlotStride;
        }
        if (slot == ~0u)
            return;
        static bool s_logged = false;
        if (!s_logged)
        {
            s_logged = true;
            std::fprintf(stderr, "[training] dummy HUD: game HUD 1 in slot %u (cs %08X)\n", slot, FAST_READ32(kGame + kHudStride + kHudBarCs));
        }
        // HUD i stays tied to camera i's _cs; with 1 player camera 1 isn't the screen's (it follows the dummy), so in this call
        // HUD 1 uses camera 0's _cs, like HUDs 2 and 3 on the quadrant screen (hudUpdate)
        const uint32_t saved = FAST_READ32(kViewSlot + 4u), savedCs = FAST_READ32(camera(1) + kCamCs);
        FAST_WRITE32(kViewSlot + 4u, slot);
        FAST_WRITE32(camera(1) + kCamCs, FAST_READ32(camera(0) + kCamCs));
        // outside the unified view HUD 1's update acts as in split screen: turns its bar on and HUD 0's off (+0xC of the bar's
        // _cs, the drawMe); HUD 0's is restored to how its update left it
        const uint32_t bar0 = FAST_READ32(kGame + kHudBarCs) & 0x1FFFFFFFu;
        const uint8_t draw0 = bar0 ? FAST_READ8(bar0 + 0xC) : 0;
        callGuest(g_origHudUpdate, kHudUpdate, rdram, ctx, runtime, kGame + kHudStride);
        if (bar0)
            FAST_WRITE8(bar0 + 0xC, draw0);
        FAST_WRITE32(kViewSlot + 4u, saved);
        FAST_WRITE32(camera(1) + kCamCs, savedCs);
    }

    // HUD bar in the bottom strip with 3 players (view 2, 640 x 112). Hud::update puts the bar's _cs in front of the camera with a
    // fixed offset along its axes (right, forward, up: HUD +0x100), made for the quadrant projection; with the strip's focal length
    // (kStripFov) and width the bar came out bigger, centered and cut off at the top. Here it moves to depth times fs / fq (the
    // on-screen size goes back to the quadrant's) and 160 GS pixels times the widescreen width to the left (the 4:3 band of the whole
    // window starts 160 * (wide - 1) GS pixels after the quadrant's): it ends up where it would be in the left quadrant.
    // The bar is the same as HUD 0's (hudInitAfter), which its update rebuilds before view 0's hier; g_stripBarPos avoids adding twice
    // if update didn't reposition it.
    float g_stripBarPos[3] = {};
    void stripHudBar(uint8_t *rdram, uint32_t bar)
    {
        const uint32_t cs = FAST_READ32(camera(2) + kCamCs) & 0x1FFFFFFFu;
        const float fq = rdF(rdram, kQuadDefs), fs = rdF(rdram, kQuadDefs + 2u * kDefSize);
        if (!bar || cs < 0x1000u || cs + 0x50u > PS2_RAM_SIZE || !(fq > 1.f) || !(fs > 1.f))
            return;
        float p[3], t[3], ax[3][3];
        for (uint32_t i = 0; i < 3; ++i)
        {
            p[i] = rdF(rdram, bar + 0x10u + 4u * i);
            t[i] = rdF(rdram, cs + 0x10u + 4u * i);
            for (uint32_t r = 0; r < 3; ++r)
                ax[r][i] = rdF(rdram, cs + 0x20u + 0x10u * r + 4u * i);
        }
        if (std::memcmp(p, g_stripBarPos, sizeof p) == 0)
            return;
        float l[3] = {};   // right, forward, up
        for (int r = 0; r < 3; ++r)
            for (int i = 0; i < 3; ++i)
                l[r] += ax[r][i] * (p[i] - t[i]);
        if (!(l[1] > 0.f))
            return;
        const float right = l[0] - 160.f * wotmScreenWide() / fq * l[1], depth = l[1] * fs / fq;
        for (int i = 0; i < 3; ++i)
        {
            g_stripBarPos[i] = t[i] + ax[0][i] * right + ax[1][i] * depth + ax[2][i] * l[2];
            wrF(rdram, bar + 0x10u + 4u * uint32_t(i), g_stripBarPos[i]);
        }
    }

    // HUD 2/3 update paused midway (the scheduler's time slice ran out inside it): the swap of the index, m_viewSlot, the camera's
    // _cs and the bar stays in effect until the resume finishes (pc at ra). Undone on exit from the first call, the rest of update
    // ran with index 2: updateMessage writes HUD 0's "WINS" with fontSpritePrintXY and the others' with
    // fontSpritePrintRightXY, so player 3's jumped to the right for one frame (player 4's is already on the right side).
    struct HudPending
    {
        bool on = false;
        uint32_t ra = 0, self = 0, me = 0, saved[4] = {};
    } g_hudPending;

    void hudUpdateDone(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t self, uint32_t me, const uint32_t saved[4]);

    // Training mode: HUD 0's update paused midway (the time slice ran out inside it). The resume doesn't go through this hook and,
    // at its end, HUD 0 turns off HUD 1's bar: the dummy's (trainingHud) is left for trainingHudLate, before the frame capture.
    // Without this the dummy's bar disappeared on those frames (it flickered).
    bool g_trainPending = false;

    // Hook on timerWaitForMinUpdateRate (wotm_overrides.cpp), before wotmCaptureFrame: the frame's update has already finished
    void trainingHudLate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (!g_trainPending)
            return;
        g_trainPending = false;
        trainingHud(rdram, ctx, runtime);
    }

    void hudUpdate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (g_hudPending.on && ctx->pc != kHudUpdate)
        {
            // resuming the paused update
            const HudPending pend = g_hudPending;
            g_origHudUpdate(rdram, ctx, runtime);
            if (ctx->pc == pend.ra)
            {
                g_hudPending.on = false;
                hudUpdateDone(rdram, ctx, runtime, pend.self, pend.me, pend.saved);
            }
            return;
        }
        const uint32_t self = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
        const bool split = ctx->pc == kHudUpdate && g_views && splitPlayers(rdram) && self >= kGame && (self - kGame) % kHudStride == 0 &&
                           (self - kGame) / kHudStride < 4u;
        if (!split)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            const bool entry = ctx->pc == kHudUpdate;
            g_origHudUpdate(rdram, ctx, runtime);
            if (entry && self == kGame)
            {
                g_trainPending = ctx->pc != ra;
                if (!g_trainPending)
                    trainingHud(rdram, ctx, runtime);
            }
            return;
        }
        if (g_hudPending.on)
        {
            // another update started before the paused one finished resuming: undo its swap first
            g_hudPending.on = false;
            hudUpdateDone(rdram, ctx, runtime, g_hudPending.self, g_hudPending.me, g_hudPending.saved);
        }
        const uint32_t me = (self - kGame) / kHudStride, p = me & 1u, hudP = kGame + kHudStride * p;
        const uint32_t ra = GPR_U32(ctx, 31);
        ++g_hudCalls[me];
        hudUse(rdram, me);
        uint32_t saved[4] = {};
        if (me >= 2u)
        {
            saved[0] = FAST_READ32(self);
            saved[1] = FAST_READ32(kViewSlot + 4u * p);
            saved[2] = FAST_READ32(camera(int(p)) + kCamCs);
            saved[3] = FAST_READ32(hudP + kHudBarCs);
            FAST_WRITE32(self, p);
            FAST_WRITE32(kViewSlot + 4u * p, FAST_READ32(kViewSlot + 4u * me));
            FAST_WRITE32(camera(int(p)) + kCamCs, FAST_READ32(camera(int(me)) + kCamCs));
            FAST_WRITE32(hudP + kHudBarCs, FAST_READ32(self + kHudBarCs));
            FAST_WRITE8(self + kHudRageActive, 1);   // no rampIn: the column's node is the only one in the active list
        }
        g_origHudUpdate(rdram, ctx, runtime);
        if (ctx->pc != ra)
        {
            ++g_hudPaused;
            g_hudPending.on = true;
            g_hudPending.ra = ra;
            g_hudPending.self = self;
            g_hudPending.me = me;
            std::memcpy(g_hudPending.saved, saved, sizeof saved);
            return;
        }
        hudUpdateDone(rdram, ctx, runtime, self, me, saved);
    }

    void hudUpdateDone(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t self, uint32_t me, const uint32_t saved[4])
    {
        const uint32_t p = me & 1u, hudP = kGame + kHudStride * p;
        if (me >= 2u)
        {
            FAST_WRITE32(self, saved[0]);
            FAST_WRITE32(kViewSlot + 4u * p, saved[1]);
            FAST_WRITE32(camera(int(p)) + kCamCs, saved[2]);
            FAST_WRITE32(hudP + kHudBarCs, saved[3]);
            std::memcpy(rdram + hudP + kHudRageTime, rdram + self + kHudRageTime, kAnimTimeLen);
            hudEvaluate(rdram, ctx, runtime, hudP);
        }
        // this HUD's bar stays as the game left it; the others (by _cs: HUD 2's and 3's are the same as 0's and 1's) go off
        const uint32_t own = hudBar(rdram, me);
        const uint8_t on = own ? FAST_READ8(own + 0xC) : 0;
        for (uint32_t i = 0; i < 4u; ++i)
            if (const uint32_t b = hudBar(rdram, i); b && b != own)
                FAST_WRITE8(b + 0xC, 0);
        if (own)
            FAST_WRITE8(own + 0xC, on);
        if (me == 2u && g_views == 3 && !g_cinemaFull && !FAST_READ32(kUnified))
            stripHudBar(rdram, own);
    }

    // Hud::initAfter(i) also does nothing with i >= 2 (no bar _cs or animations; the HUD node at +0xE4 only exists in HUDs 0 and
    // 1). On exit from HUD 2's or 3's initAfter it becomes a copy of the HUD in the same column (i & 1), already initialized (main calls them from
    // 0 to 3), with its index restored: both share the bar's _cs and animations, which each view rebuilds in its Hud::update
    // right before its hier.
    constexpr uint32_t kHudInitAfter = wotm::fn::initAfter__3Hudi;
    PS2Runtime::RecompiledFunction g_origHudInitAfter = nullptr;
    void hudInitAfter(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t self = GPR_U32(ctx, 4) & 0x1FFFFFFFu, i = GPR_U32(ctx, 5);
        const uint32_t ra = ctx->pc == kHudInitAfter ? GPR_U32(ctx, 31) : 0u;
        g_origHudInitAfter(rdram, ctx, runtime);
        if (ra && ctx->pc == ra && i == 0u)
            hudAnimReset();   // new HUDs (main initializes them from 0 to 3): the saved animation state is no longer valid
        if (!ra || ctx->pc != ra || i < 2u || i >= 4u || self != kGame + kHudStride * i || !g_players)
            return;
        const int32_t n = static_cast<int32_t>(FAST_READ32(kNumPlayers));
        if (n < 3 || static_cast<int32_t>(i) >= n)
            return;
        std::memcpy(rdram + self, rdram + kGame + kHudStride * (i & 1u), kHudStride);
        FAST_WRITE32(self, i);
    }

    // Monster::updateBoostAndRage: with full rage the camera-followed monster starts the flash (h180) on HUD m_cameraView. During
    // logic (gameUpdate) RAM holds the column HUD's animation state; the monster of HUDs 2 and 3 uses its own during the call.
    constexpr uint32_t kBoostRage = wotm::fn::updateBoostAndRage__7Monster;
    PS2Runtime::RecompiledFunction g_origBoostRage = nullptr;
    void boostAndRage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
        const uint32_t view = ctx->pc == kBoostRage && m >= 0x1000u && m + kCameraView + 4u <= PS2_RAM_SIZE ? FAST_READ32(m + kCameraView) : 0u;
        if (view < 2u || view >= 4u || !g_views || !splitPlayers(rdram))
        {
            g_origBoostRage(rdram, ctx, runtime);
            return;
        }
        const uint32_t before = g_hudLive[view & 1u];
        hudUse(rdram, view);
        g_origBoostRage(rdram, ctx, runtime);
        hudUse(rdram, before);
    }

    // End of round in mode 3 (Shell::EvaluateGameStatus -> EvaluateMultiPlayerBattleStatusNoAI or ...AI, with r = 1 when someone
    // dies, 3 when they quit). Retail NoAI only handles "1 left alive" and "none alive": with 3 or 4 players a death leaves 2 or 3
    // alive, nobody respawns and the next round ends right away (the dead stay dead). AI respawns slots 0 and 1 as players and the
    // others as AI (players 3 and 4 would become AI). Here, with quadrants, both follow the AI rule by m_monsters: with a win
    // target (m_killTarget) and a living killer who reached it, dialog 5 (winner in m_won[0]); otherwise each dead monster respawns as
    // what it is (players 0..3 through playerInit, AIs through aiInit) and the session continues.
    constexpr uint32_t kEvalNoAI = wotm::fn::EvaluateMultiPlayerBattleStatusNoAI__5Shelli,
                       kEvalAI = wotm::fn::EvaluateMultiPlayerBattleStatusAI__5Shelli, kPlayerInit = wotm::fn::playerInit__7Monster,
                       kAiInit = wotm::fn::aiInit__7Monster, kDisplayDialog = wotm::fn::displayDialog__Fi;
    constexpr uint32_t kHealth = 0x44C, kDead = 0xE8, kKiller = 0x846C, kMonsterNum = 0x28, kWins = 0x3C;   // Monster
    constexpr uint32_t kInSession = kShell + 0x2BCC, kInMenus = kShell + 0x2BD0;
    PS2Runtime::RecompiledFunction g_origEvalNoAI = nullptr, g_origEvalAI = nullptr;
    void evaluateBattle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool entry = ctx->pc == kEvalNoAI || ctx->pc == kEvalAI;
        if (!entry || !g_views || !splitPlayers(rdram) || FAST_READ32(kGameMode) != 3u)
        {
            (ctx->pc == kEvalAI ? g_origEvalAI : g_origEvalNoAI)(rdram, ctx, runtime);
            return;
        }
        const int32_t r = static_cast<int32_t>(GPR_U32(ctx, 5));
        const uint32_t ra = GPR_U32(ctx, 31);
        ctx->pc = ra;
        if (r == 3)
        {
            FAST_WRITE32(kInMenus, 0u);
            FAST_WRITE32(kInSession, 0u);
            return;
        }
        int32_t killer = -1;
        const uint32_t slots = std::min<uint32_t>(FAST_READ32(kNumSlots), 16u);
        for (uint32_t i = 0; i < slots && killer < 0; ++i)
        {
            const uint32_t s = kSlots + kSlotStride * i;
            const uint32_t by = FAST_READ32(s + kKiller) & 0x1FFFFFFFu;
            if (FAST_READ8(s + kDead) && by && !FAST_READ8(by + kDead))
                killer = static_cast<int32_t>(FAST_READ32(by + kMonsterNum));
        }
        const int32_t target = static_cast<int32_t>(FAST_READ32(kShell + 0x2A44));   // m_killTarget
        if (target > 0 && killer >= 0 && killer < 16 &&
            static_cast<int32_t>(FAST_READ32(kSlots + kSlotStride * uint32_t(killer) + kWins)) >= target)
        {
            // displayDialog is modal (runs frames and yields to the scheduler until the choice): called as a tail call, with the ra of whoever called
            // Evaluate. Through callGuest (ra 0) its resume went back to pc 0 at the end and the game thread died ("play again")
            FAST_WRITE32(kGame + 0x12043C, uint32_t(killer));
            ctx->r[4] = _mm_set_epi64x(0, 5);
            ctx->pc = kDisplayDialog;
            runtime->lookupFunction(kDisplayDialog)(rdram, ctx, runtime);
            return;
        }
        const int players = splitPlayers(rdram), ais = static_cast<int32_t>(FAST_READ32(kGame + 0x1203E0));
        for (int i = 0; i < 4 + ais; ++i)
        {
            if (i >= players && i < 4)
                continue;
            const uint32_t m = FAST_READ32(kMonsters + 4u * uint32_t(i)) & 0x1FFFFFFFu;
            if (!m || !(rdF(rdram, m + kHealth) <= 0.f))
                continue;
            const uint32_t fn = i < 4 ? kPlayerInit : kAiInit;   // WOTM_AIVSAI's (playerInit -> aiInit) applies: looked up each time
            callGuest(runtime->lookupFunction(fn), fn, rdram, ctx, runtime, m);
        }
        FAST_WRITE32(kInMenus, 0u);
        FAST_WRITE32(kInSession, 1u);
    }

    // TheGame::gameGetStartPoint(m): in modes 2 to 6, a human's first spawn (m_playerNum 1) uses StartPoints list 1
    // and an AI's list 2, in rotation (getNextPoint), rejecting a point with something within 120; respawns use list 3.
    // List 1 has retail's 2 player points (in Centro: 2; 2 and 3 have 4, one in each corner): players 3 and 4 took
    // player 1's point (occupied) and fell to player 2's, all three in the same place. With quadrants, a player beyond the number of points in list 1
    // spawns as an AI (m_playerNum 2 during the call): list 2. Diagnostics with WOTM_SPLITLOG (the lists only once).
    constexpr uint32_t kStartPoint = wotm::fn::gameGetStartPoint__7TheGameP7Monster, kPointTypes = wotm::sym::x_11StartPoints_s_pointTypes;
    PS2Runtime::RecompiledFunction g_origStartPoint = nullptr;
    void logPointLists(uint8_t *rdram)
    {
        for (uint32_t t = 0; t < 4; ++t)
        {
            const uint32_t pts = FAST_READ32(kPointTypes + 16u * t) & 0x1FFFFFFFu, n = FAST_READ32(kPointTypes + 16u * t + 4);
            std::fprintf(stderr, "[split] points type %u: %u (next %u):", t, n, FAST_READ32(kPointTypes + 16u * t + 12));
            for (uint32_t i = 0; i < n && i < 32 && pts >= 0x1000u; ++i)
                std::fprintf(stderr, " (%.0f,%.0f,%.0f)", rdF(rdram, pts + 0x20u * i), rdF(rdram, pts + 0x20u * i + 4), rdF(rdram, pts + 0x20u * i + 8));
            std::fprintf(stderr, "\n");
        }
    }
    // Some other m_monsters entry within 120 (on the ground plane) of m_monsters[who]
    bool nearOther(uint8_t *rdram, int who)
    {
        const uint32_t m = FAST_READ32(kMonsters + 4u * uint32_t(who)) & 0x1FFFFFFFu, cs = m ? FAST_READ32(m + 0xC) & 0x1FFFFFFFu : 0u;
        for (int j = 0; j < 12 && cs >= 0x1000u; ++j)
        {
            const uint32_t o = FAST_READ32(kMonsters + 4u * uint32_t(j)) & 0x1FFFFFFFu, ocs = o ? FAST_READ32(o + 0xC) & 0x1FFFFFFFu : 0u;
            if (j == who || ocs < 0x1000u)
                continue;
            const float dx = rdF(rdram, ocs + 0x10) - rdF(rdram, cs + 0x10), dy = rdF(rdram, ocs + 0x14) - rdF(rdram, cs + 0x14);
            if (dx * dx + dy * dy < 120.f * 120.f)
                return true;
        }
        return false;
    }
    // Still on top of another after list 2 (dodgeball, mode 7, only uses list 1, with 2 points, and the arena has no others): the
    // point where it landed rotated around the center of list 1's points, 90 degrees for player 3 and -90 for player 4 (or 180), a little
    // higher; Monster::setTrans and hdReparentCsGrid as gameGetStartPoint does, and gravity settles it.
    constexpr uint32_t kStartVec = PS2_RAM_SIZE - 0x400;   // _fvector for setTrans (below wotm_modes4.inc's text)
    void rotatedStart(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t m, int who, bool log)
    {
        const uint32_t pts = FAST_READ32(kPointTypes + 16u) & 0x1FFFFFFFu, n = FAST_READ32(kPointTypes + 16u + 4u);
        const uint32_t cs = FAST_READ32(m + 0xC) & 0x1FFFFFFFu;
        if (pts < 0x1000u || n == 0 || n > 32 || cs < 0x1000u)
            return;
        float cx = 0.f, cy = 0.f;
        for (uint32_t i = 0; i < n; ++i)
        {
            cx += rdF(rdram, pts + 0x20u * i);
            cy += rdF(rdram, pts + 0x20u * i + 4);
        }
        cx /= float(n);
        cy /= float(n);
        for (int k = 0; k < 3; ++k)   // 90, -90, 180 degrees: the first free one
        {
            const float s = k == 0 ? (who & 1 ? -1.f : 1.f) : k == 1 ? (who & 1 ? 1.f : -1.f) : 0.f, c = k == 2 ? -1.f : 0.f;
            const float x = rdF(rdram, cs + 0x10) - cx, y = rdF(rdram, cs + 0x14) - cy;
            wrF(rdram, kStartVec, cx + x * c - y * s);
            wrF(rdram, kStartVec + 4, cy + x * s + y * c);
            wrF(rdram, kStartVec + 8, rdF(rdram, cs + 0x18) + 50.f);
            wrF(rdram, kStartVec + 12, 0.f);
            callGuest(runtime->lookupFunction(wotm::fn::setTrans__7MonsterR8_fvector), wotm::fn::setTrans__7MonsterR8_fvector, rdram, ctx, runtime,
                      m, kStartVec);
            callGuest(runtime->lookupFunction(wotm::fn::hdReparentCsGrid__FP3_cs), wotm::fn::hdReparentCsGrid__FP3_cs, rdram, ctx, runtime, cs);
            if (!nearOther(rdram, who))
                break;
        }
        if (log)
            std::fprintf(stderr, "[split] spawn: m_monsters[%d] at the point rotated around %.0f,%.0f: %.0f,%.0f\n", who, cx, cy,
                         rdF(rdram, cs + 0x10), rdF(rdram, cs + 0x14));
    }

    void startPoint(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static const bool log = std::getenv("WOTM_SPLITLOG") != nullptr;
        const uint32_t m = GPR_U32(ctx, 5) & 0x1FFFFFFFu, ra = ctx->pc == kStartPoint ? GPR_U32(ctx, 31) : 0u, self = GPR_U32(ctx, 4);
        static bool listed = false;
        if (log && ra && !listed)
        {
            listed = true;
            logPointLists(rdram);
        }
        int who = -1;
        for (int i = 0; i < 12 && who < 0; ++i)
            if ((FAST_READ32(kMonsters + 4u * uint32_t(i)) & 0x1FFFFFFFu) == m)
                who = i;
        const int32_t humanPoints = static_cast<int32_t>(FAST_READ32(kPointTypes + 16u + 4u));   // list 1
        const bool asAi = ra && who >= humanPoints && who < 4 && splitPlayers(rdram) && FAST_READ32(m + kPlayerNum) == 1u;
        if (asAi)
            FAST_WRITE32(m + kPlayerNum, 2u);
        g_origStartPoint(rdram, ctx, runtime);
        if (asAi)
            FAST_WRITE32(m + kPlayerNum, 1u);
        // Respawn (list 3, 4 points) with more than 4 monsters: on "play again" everyone respawns together and the fifth and sixth land on
        // another's point (all occupied: the last one tried is kept). Two monsters in the same place: the push between them gave NaN in the position
        // (and the player's camera went along). A monster within 120 of another: again like an AI's first spawn (list 2, the
        // corners, with the same free-point test).
        if (ra && ctx->pc == ra && who >= 0 && splitPlayers(rdram))
        {
            const uint32_t cs = FAST_READ32(m + 0xC) & 0x1FFFFFFFu;
            bool near = false;
            for (int j = 0; j < 12 && cs >= 0x1000u && !near; ++j)
            {
                const uint32_t o = FAST_READ32(kMonsters + 4u * uint32_t(j)) & 0x1FFFFFFFu, ocs = o ? FAST_READ32(o + 0xC) & 0x1FFFFFFFu : 0u;
                if (j == who || ocs < 0x1000u)
                    continue;
                const float dx = rdF(rdram, ocs + 0x10) - rdF(rdram, cs + 0x10), dy = rdF(rdram, ocs + 0x14) - rdF(rdram, cs + 0x14);
                near = dx * dx + dy * dy < 120.f * 120.f;
            }
            if (near)
            {
                const uint32_t inits = FAST_READ32(m + kNumInits), pn = FAST_READ32(m + kPlayerNum);
                FAST_WRITE32(m + kNumInits, 0u);
                FAST_WRITE32(m + kPlayerNum, 2u);
                callGuest(g_origStartPoint, kStartPoint, rdram, ctx, runtime, self, m);
                FAST_WRITE32(m + kNumInits, inits);
                FAST_WRITE32(m + kPlayerNum, pn);
                if (log)
                    std::fprintf(stderr, "[split] spawn: m_monsters[%d] on top of another, again through list 2\n", who);
                if (nearOther(rdram, who))
                    rotatedStart(rdram, ctx, runtime, m, who, log);
            }
        }
        if (log && ra && ctx->pc == ra)
        {
            const uint32_t cs = FAST_READ32(m + 0xC) & 0x1FFFFFFFu;
            std::fprintf(stderr, "[split] spawn: m_monsters[%d] (playerNum %d) at %.0f,%.0f,%.0f\n", who,
                         static_cast<int32_t>(FAST_READ32(m + kPlayerNum)), rdF(rdram, cs + 0x10), rdF(rdram, cs + 0x14), rdF(rdram, cs + 0x18));
        }
    }

    // Quadrants for players 1 to 4 (with WOTM_PLAYERS=5..8, the rest are AIs, wotm_multi.inc)
    void install(PS2Runtime &runtime)
    {
        constexpr int n = 4;   // player cap (with 1 or 2 in the Shell the hooks leave the game as in retail)
        g_viewCreate = runtime.lookupFunction(kViewCreate);
        g_viewSetFov = runtime.lookupFunction(kViewSetFov);
        g_follow = runtime.lookupFunction(kFollow);
        g_setPov = g_origSetPov = runtime.lookupFunction(kSetPov);
        g_origReestablish = runtime.lookupFunction(kReestablish);
        g_origGrapple = runtime.lookupFunction(kGrapple);
        g_origOkToUnify = runtime.lookupFunction(kOkToUnify);
        g_origResetLevel = runtime.lookupFunction(kResetLevel);
        g_origInitCamera = runtime.lookupFunction(kInitCamera);
        g_origLoadLevelFiles = runtime.lookupFunction(kLoadLevelFiles);
        g_origInitPlayers = runtime.lookupFunction(kInitPlayers);
        g_origSetPlayer = runtime.lookupFunction(kSetPlayer);
        g_origHudUpdate = runtime.lookupFunction(kHudUpdate);
        g_origStartPoint = runtime.lookupFunction(kStartPoint);
        g_origHudInitAfter = runtime.lookupFunction(kHudInitAfter);
        g_origEvalNoAI = runtime.lookupFunction(kEvalNoAI);
        g_origEvalAI = runtime.lookupFunction(kEvalAI);
        g_origBoostRage = runtime.lookupFunction(kBoostRage);
        g_origThrowPreempt = runtime.lookupFunction(kThrowPreempt);
        g_origLeaveUnified = runtime.lookupFunction(kLeaveUnified);
        g_updateAnims = runtime.lookupFunction(kUpdateAnims);
        if (!g_viewCreate || !g_viewSetFov || !g_follow || !g_setPov || !g_origReestablish || !g_origGrapple || !g_origOkToUnify ||
            !g_origResetLevel || !g_origInitCamera || !g_origLoadLevelFiles || !g_origInitPlayers || !g_origSetPlayer || !g_origHudUpdate || !g_origStartPoint || !g_origHudInitAfter || !g_origEvalNoAI || !g_origEvalAI ||
            !g_origBoostRage || !g_origThrowPreempt || !g_origLeaveUnified || !g_updateAnims)
        {
            std::fprintf(stderr, "[split] %d-player split screen off: game function missing from the runtime\n", n);
            return;
        }
        g_players = n;
        runtime.registerFunction(kReestablish, &reestablishViews);
        runtime.registerFunction(kGrapple, &grappleConfig);
        runtime.registerFunction(kSetPov, &setCameraPov);
        runtime.registerFunction(kOkToUnify, &okToUnify);
        runtime.registerFunction(kResetLevel, &resetLevel);
        runtime.registerFunction(kInitCamera, &initCamera);
        runtime.registerFunction(kLoadLevelFiles, &loadLevelFiles);
        runtime.registerFunction(kInitPlayers, &initPlayers);
        runtime.registerFunction(kSetPlayer, &setPlayerMonster);
        runtime.registerFunction(kHudUpdate, &hudUpdate);
        runtime.registerFunction(kStartPoint, &startPoint);
        runtime.registerFunction(kHudInitAfter, &hudInitAfter);
        runtime.registerFunction(kEvalNoAI, &evaluateBattle);
        runtime.registerFunction(kEvalAI, &evaluateBattle);
        runtime.registerFunction(kBoostRage, &boostAndRage);
        runtime.registerFunction(kThrowPreempt, &throwPreemption);
        runtime.registerFunction(kLeaveUnified, &leaveUnified);
            std::fprintf(stderr, "[split] up to %d players: quadrant screen, pads 3 and 4 on slot 1 of the ports\n", n);
    }
}

// Multiplayer menu monster selection (wotm_padsel.inc): costume for players 3 and 4 (the Shell only stores players 1 and 2's)
void wotmMultiSetCostume(int player, uint32_t costume)
{
    if (player >= 2 && player < 4)
        wotm_split4::g_extraCostume[player - 2] = costume;
}
