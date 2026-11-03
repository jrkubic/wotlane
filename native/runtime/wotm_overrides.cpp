// Fixes specific to War of the Monsters (SCUS_971.97), applied by replacing functions of the recompiled code.
//
// updateRate floor: `timerGetUpdateRate` (0x2236D8) returns D_006F8DB8, how many frames per second the game's last frame
// "was worth" (60 = 1 vsync). Physics integrates with Euler using `dt = 1 / rate` (fur spring in BoneSpring::update,
// monster turning in MonsterDynamics::updatePosition, vehicles, debris...). On retail the rate is 60 or 30; in this
// runtime the game runs at ~20 logic frames/s and the rate swings frame to frame between 60 and 8, so dt is up to 7x larger
// than intended. With that dt the integrators go unstable: Congar's fur stretches across the whole scene and the turn control
// (damping ~ -40.5 * rotVel, stable only with dt < 2/40.5, rate >= 21) flips sign and amplifies the velocity every frame
// (the monster spins on its own and the camera behind it spins along).
// The fix returns at least WOTM_RATEMIN (default 30; 0 turns it off) for the whole game: on slow frames the game runs
// a little slower instead of skipping, and every integrator stays in the range it was designed for. Nothing changes when the
// rate is already >= 30 (retail). Replaces the earlier targeted floors (WOTM_SPRINGMIN on the spring, WOTM_TURNMIN on turning).
#include "ps2_runtime.h"
#include "wotm_menucpus.h"   // CPUs 4..7 of menu screen 12
#include "wotm_syms.h"   // wotm::sym / wotm::fn: ELF addresses by name (tools/native_syms.py --gen)
#include "ps2_runtime_macros.h"
#include "runtime/ee_scheduler.h"

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <string>

#include <memory>
#include <thread>

#include "net/wotm_lockstep.hpp"     // input timeline for deterministic mode and the netcode (wotm_sync.inc)
#include "net/wotm_monsters.hpp"     // monsters and costumes present on the disc (WOTM_MATCH)
#include "net/wotm_net_client.hpp"   // network match client (wotm_sync.inc)
#include "wotm_netstatus.h"          // on-screen network message and disabled debug keys (wotm_live.inc)

#ifdef _WIN32
extern "C" __declspec(dllimport) unsigned long __stdcall GetModuleFileNameA(void *module, char *path, unsigned long size);
#else
unsigned long GetModuleFileNameA(void *module, char *path, unsigned long size);   // wotm_posix.cpp: the same contract, for netBuildId
#endif

namespace ps2_stubs   // Kernel/Stubs/Pad.cpp: pad state that scePadRead returns instead of the devices'
{
    void setPadOverrideState(uint16_t buttons, uint8_t lx, uint8_t ly, uint8_t rx, uint8_t ry);
    void clearPadOverrideState();
}

extern std::atomic<unsigned long long> g_wotmIdleUs;
void wotmHostProfWatchThisThread();                            // ps2_hostprof.cpp (PS2X_HOSTPROF)
void wotmHostProfDump(long long t0, long long t1, const char *tag);
double wotmThreadCpuMs();
void wotmHostProfFrameStart();
extern bool g_wotmCycleProf;
extern uint64_t g_wotmEeCycle, g_wotmIdleCycles;
extern uint32_t g_wotmProfPc[4096];
extern uint64_t g_wotmProfCyc[4096];
extern uint32_t g_wotmFreeSpinPc;
extern void (*g_wotmCallTrace)(const R5900Context *ctx, uint32_t targetPc);   // ps2_runtime.cpp (dispatchGuestBranch), WOTM_CALLTRACE
void wotmCaptureFrame(const uint8_t *rdram);   // wotm_live.inc: copies the finished frame for native drawing
void wotmCaptureScene(const uint8_t *rdram, int view);   // wotm_live.inc: scene (objects, palettes, camera) on exit from hier(v, 0)
void wotmVu1SceneExit(const uint8_t *rdram);   // wotm_vu1check.inc: the snapshot's 'S' record, at the same point
void wotmFlareCapture(const uint8_t *rdram);   // wotm_live.inc: lens flare, projected and occlusion-tested
void wotmThreeMileFlash(const uint8_t *rdram);   // wotm_live.inc: white flash from the 3-MILE power plant
void wotmFadeView(const uint8_t *rdram, int view, uint32_t alpha, bool queued);   // wotm_live.inc: black fade of the whole view, from the emitter's arguments
void wotmTheGameFade(const uint8_t *rdram, uint32_t self);   // wotm_live.inc: exit of TheGame's fades
void wotmLetterbox(const uint8_t *rdram, float alpha);   // wotm_live.inc: menu black bars, from letterboxDmaData's alpha
void wotmGsPktQueueSent();   // wotm_live.inc: the gsPkt queue was dispatched (DmaGsPktQueue)
void wotmFontSent();         // wotm_live.inc: the frame's text was dispatched (fontDmaFontData)
// wotm_live.inc: text from the font calls (wotm::FontText), on entry to each one
void wotmFontInitPacket(uint32_t vram);
void wotmFontSetTexId(const uint8_t *rdram, uint32_t node);
void wotmFontColor(const uint8_t *rdram, int font);
void wotmFontGlyph(const uint8_t *rdram, int font, int ch);
void wotmFontDma(const uint8_t *rdram);
void wotmShellFade(const uint8_t *rdram, int view, uint32_t r, uint32_t g, uint32_t b, int start, int end, int step, bool redraw);
void wotmShellFadeStep();       // wotm_live.inc: entry of one sceGsSyncV in Shell::FadeScreen's loop
void wotmShellFadeStepDone();   // wotm_live.inc: its return
void wotmCaptureFadeStep(const uint8_t *rdram);   // wotm_live.inc: scene of one Shell::FadeScreen step that redraws
void wotmLoadFreeze(bool on);   // wotm_live.inc: stage load (LoadLevelFiles until rtMain), the screen repeats the last frame
bool wotmPauseBlock(PS2Runtime *rt);   // wotm_foto.inc: F7/F6, holds the EE thread while paused
bool wotmPauseTookFrame();             // wotm_foto.inc: the frame that just ended had a stop (pause or snap)
void wotmPickupHighlights(const uint8_t *rdram);   // wotm_live.inc: colorQuad of highlighted pick-ups, on exit from enableHighlights
void wotmScreenPoly(const uint8_t *rdram, int src, int x, int y, uint32_t pd);   // wotm_live.inc: one ScreenPolys quad, on entry to polyBuildPrim
bool wotmScreenPolyBox(const uint8_t *rdram, int src, int x, int y, uint32_t pd, float box[4]);   // wotm_live.inc: the quad's box, without building it
void wotmBarRect(float x0, float y0, float x1, float y1, uint8_t r, uint8_t g, uint8_t b, uint8_t a);   // wotm_live.inc: bar rectangle
void wotmBarTex(float x0, float y0, float x1, float y1, uint64_t tex0, float u0, float v0, float u1, float v1);   // wotm_live.inc: portrait
void wotmBarView(int view);   // wotm_live.inc: view of the bars that follow
void wotmSoundFrame(unsigned fields);   // wotm_snd.inc: sound advances one game frame (deterministic mode)
void wotmHudAnchor(const uint8_t *rdram, float cx, float cy, const float *world, bool bars);   // wotm_live.inc: health icon (photo mode)
void wotmHudAnchorEnd();
float wotmHealthIconShift(const uint8_t *rdram, float rx);   // wotm_live.inc: bar x clamped to the widescreen view's edge
void wotmDamagePopup(const uint8_t *rdram, const float p[3], float amount, int kind);   // wotm_live.inc: damage number (0 hit, 1 throw, 2 special)
void wotmPadSelInstall(PS2Runtime &runtime);   // wotm_padsel.inc: 2-player side selection on the menu's big screen
void wotmLevelSelInstall(PS2Runtime &runtime);   // wotm_levelsel.inc: hidden stages and RANDOM in stage select
void wotmMainMenuInstall(PS2Runtime &runtime);   // wotm_mainmenu.inc: text main menu

// Debug cheats: the window (F1 menu, wotm_live.inc) writes, the EE thread (monsterUpdateCheats) reads. Bits in wotm_cheats.h.
#include "wotm_cheats.h"
#include "wotm_settings.hpp"   // live options ("recompiled options" panel and launcher)
std::atomic<uint32_t> g_wotmCheats{0};
std::atomic<bool> g_wotmCheatWin{false};

// Debug free camera (F5): the window (wotm_freecam.inc) moves the pose, the EE thread puts it into view 0's _cs (freeCamWrite).
#include "wotm_freecam.h"
#include "wotm_mouse.hpp"   // mouse for the modern camera (wotm_orbit.inc consumes it, the window accumulates it)
#include "wotm_detmath.hpp"   // trig with the same bits on every machine, for results that reach guest RAM (wotm_orbit.inc, lock-on)
std::atomic<bool> g_wotmFreeCam{false};
std::atomic<bool> g_wotmFreeCamPad{false};
std::mutex g_wotmFreeCamM;
WotmFreeCamPose g_wotmFreeCamPose;

// JSON save next to mc0/ (NATIVO.md, "Save em JSON"): hooks on MonsterMc::stdInit/stdExit.
#include "wotm_save.inc"
// Main menu "Press start": X counts as Start, and the button already works during the transition.
#include "wotm_menu.inc"
// 3- and 4-player split screen (NATIVO.md, "Tela dividida de 3 e 4 jogadores"; WOTM_PLAYERS=3 or 4): views, cameras, load and pads 3 and 4.
#include "wotm_split4.inc"
// Elimination and minigames with 3 and 4 players (on top of the wotm_split4.inc hooks)
#include "wotm_modes4.inc"
// mods/ folder that overrides disc files (MODS.md): fileCdSearchFile and sceCdRead.
#include "wotm_mods.inc"
// Mod skins in a fight (item 8): base costume's sounds and AIs randomly given a skin
#include "wotm_skins.inc"
// Custom monsters (item 8, part 2): playable Goliath Prime (visible to AIs, takes damage from them)
#include "wotm_custom.inc"
// Training mode (item 9): attack spheres, exact damage and dummy (WOTM_TRAINING, F2 menu)
#include "wotm_training.inc"
// The game's sine and cosine against retail, bit for bit (WOTM_TRIGCHECK): what a Windows vs Mac match needs
#include "wotm_trigcheck.inc"

namespace
{
    constexpr uint32_t kTimerGetUpdateRate = wotm::fn::timerGetUpdateRate__Fv;
    constexpr uint32_t kUpdateRateVar = 0x6F8DB8u;   // D_006F8DB8, read through gp

    int g_minRate = 30;
    unsigned long long g_clamped = 0;

    // Frame fields = vblanks that have passed since the frame started (WotM clock, g_wotmFreeSpinPc != 0).
    // The game measures the frame with timer 1: timer1Handler adds ceil(T1 / 9619) to the field counter (D_006F8DBC) and waits for
    // vsync; timerWaitForMinUpdateRate computes fields = D_006F8DBC - D_006F8DC0 and updateRate = 60 / fields. On the PS2 the frame
    // starts right after a vblank, so "the logic crossed n vblanks" and "T1 >= n fields" are the same thing. Here the timer runs
    // on the virtual clock and the frame starts a few (host) ms after the vblank: the logic crosses a vblank with T1 < 1 field,
    // the game counts 1 field for a 2-vsync frame and runs slower than the clock (18% of frames, ~11% slowdown
    // in a fight). The fix: on entry to timerWaitForMinUpdateRate the counter becomes at least the vblanks since the
    // frame started (marked by timerGetCurTics at the end of timerWaitForMinUpdateRate / in timerSetFrameStart).
    constexpr uint32_t kTimerWaitForMinUpdateRate = wotm::fn::timerWaitForMinUpdateRate__FPUi;
    constexpr uint32_t kTimerGetCurTics = wotm::fn::timerGetCurTics__Fv;
    constexpr uint32_t kFrameStartRa[2] = {0x22369Cu, 0x223480u};   // return address of the jal in timerWaitForMinUpdateRate / timerSetFrameStart
    constexpr uint32_t kFieldCounter = 0x6F8DBCu, kFieldCounterAtStart = 0x6F8DC0u;
    PS2Runtime::RecompiledFunction g_origWaitMinRate = nullptr, g_origGetCurTics = nullptr;
    uint64_t g_frameStartVsync = 0;
    bool g_frameStartValid = false;
    std::chrono::steady_clock::time_point g_frameStartHost;   // [field] sv=: frame start on the host clock
    double g_svAtMs = -1.0;                                   // [field] sv=: 1st call to sceGsSyncV (ms since the frame started)
    int g_svVblanks = -1;                                     // vblanks delivered between the frame start and that call

    void getCurTicsMarked(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = GPR_U32(ctx, 31);
        g_origGetCurTics(rdram, ctx, runtime);
        if (ra == kFrameStartRa[0] || ra == kFrameStartRa[1])
        {
            g_frameStartVsync = runtime->eeScheduler().currentVSyncTick();
            g_frameStartValid = true;
        }
    }

    // Native timer clock (FRONTEIRA.md, step 4; WOTM_TIMER_EMU=1 goes back to the emulated timer 1). The game measures time with timer 1
    // (timerInit: mode 0x82, BUSCLK/256 = 576 kHz; 9619 ticks per field, timerGetFrameTime divides by 576 to get ms):
    // timerGetCurTics returns D_006F8DC4 + T1, and timer1Handler adds ceil(T1 / 9619) fields (up to 16) to the counter D_006F8DBC, T1 being
    // the time since the last reset (end of timerWaitForMinUpdateRate, timerSetFrameStart or the previous timer1Handler). In the runtime
    // T1 runs on the EE's virtual clock, which only tracks wall time through scheduler patches. Here both read the
    // host clock instead: timerGetCurTics is replaced entirely, and timer1Handler still runs (it waits for vsync), but the fields it
    // adds come from host time. The rest of the timer code (timerSetFrameStart, timerEndOfRealTimeProcessing, timerGetFrameTime...) only sees T1
    // through timerGetCurTics and moves to the host with no changes. Vsync (sceGsSyncV) still comes from the runtime's scheduler.
    constexpr uint32_t kTimer1Handler = wotm::fn::timer1Handler__FPUi;
    constexpr uint32_t kFieldsForAverage = 0x6F8DE4u;   // D_006F8DE4: fields since timerSetFrameStart (timerGetAverageFrameTime)
    constexpr int32_t kTicsPerField = 9619;
    constexpr uint32_t kT1Count = 0x10000800u;   // T1_COUNT: timer1Handler reads it and resets it
    PS2Runtime::RecompiledFunction g_origTimer1 = nullptr;
    bool g_nativeTimer = true;
    const auto g_hostT0 = std::chrono::steady_clock::now();
    uint32_t g_t1Zero = 0;   // host ticks at the last T1 reset

    uint32_t hostTics()
    {
        const auto us = std::chrono::duration_cast<std::chrono::microseconds>(std::chrono::steady_clock::now() - g_hostT0).count();
        return static_cast<uint32_t>(static_cast<uint64_t>(us) * 576u / 1000u);   // wraps around in ~2 h, like the game's counter
    }

    // Offline turbo (wotm_modifiers.inc): during the fight the host clock the game sees runs 2x, continuous across each switch
    uint32_t g_clockHost0 = 0, g_clockGame0 = 0, g_clockScale = 1;
    uint32_t gameHostTics() { return wotm::modifiers::scaledClock(hostTics(), g_clockHost0, g_clockGame0, g_clockScale); }
    void setGameClockScale(uint32_t scale)
    {
        const uint32_t host = hostTics();
        g_clockGame0 = wotm::modifiers::scaledClock(host, g_clockHost0, g_clockGame0, g_clockScale);
        g_clockHost0 = host;
        g_clockScale = scale;
    }

#include "wotm_sync.inc"
    namespace wotm_modifiers   // wotm_modifiers.inc (below, after the RAM helpers it uses)
    {
        void fightStart();
        void statsFrame(uint8_t *rdram);
    }

    // The clock the game sees: the host's, or the fixed-step virtual one (WOTM_FIXEDSTEP; wotm_sync::callTics on each query from the game)
    uint32_t gameTics() { return wotm_sync::g_fixedStep ? wotm_sync::g_virtualTics : gameHostTics(); }

    void getCurTicsNative(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = GPR_U32(ctx, 31), now = wotm_sync::g_fixedStep ? wotm_sync::callTics() : gameHostTics();
        SET_GPR_U32(ctx, 2, now);
        ctx->pc = ra;
        if (ra == kFrameStartRa[0] || ra == kFrameStartRa[1])   // the game resets T1 there: start of the frame
        {
            g_t1Zero = now;
            g_frameStartVsync = runtime->eeScheduler().currentVSyncTick();
            g_frameStartValid = true;
            g_frameStartHost = std::chrono::steady_clock::now();
        }
    }

    // Counting happens on entry: timer1Handler almost always blocks in sceGsSyncV, and resuming jumps straight into the middle of the
    // recompiled function without going through this wrapper (an "after" here never runs). The emulated T1 is reset so the original adds 0 fields
    // (it rereads D_006F8DBC/D_006F8DE4 after the vsync and writes back what we added).
    void timer1HandlerNative(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kTimer1Handler)
        {
            const uint32_t now = gameTics();
            const int32_t elapsed = static_cast<int32_t>(now - g_t1Zero);
            g_t1Zero = now;
            // fixed step: the virtual clock only moves at the frame boundary; 1 field per vsync lets the frame wait finish
            const uint32_t add = wotm_sync::g_fixedStep ? 1u
                               : elapsed <= 0 ? 0u : static_cast<uint32_t>(std::min((elapsed + kTicsPerField - 1) / kTicsPerField, 16));
            WRITE32(kFieldCounter, READ32(kFieldCounter) + add);
            if (!wotm_sync::g_fixedStep)   // fixed step: wotm_sync::frame adds the step at the frame boundary
                WRITE32(kFieldsForAverage, READ32(kFieldsForAverage) + add);
            runtime->memory().write32(kT1Count, 0u);
        }
        g_origTimer1(rdram, ctx, runtime);
    }


    // Direct match, WOTM_MATCH="<player monster>[,<AI>...up to 7],<stage>" (indices into MonsterLongNames and GameLevelNames;
    // "3,1,1" = lizard vs monkey in central; "3,1,2,5,1" = lizard vs monkey, robot and mantis): userintMain (0x1D9868) doesn't open
    // the menu and returns 0 (start the session) with the shell set up the way the menu leaves it for "1 player vs CPU" (measured with dumps at
    // the end of the -Auto script's menu): challenge (m_mode 2), 1 player + 0 to 7 AIs (m_monsterSel[4 + i], m_costume[2 + i]), 3 lives. Each monster
    // accepts a costume as "<monster>:<0-3>" ("3:2,1:1,1"); without ":" or with a costume already taken by the same monster, it gets the lowest
    // free one (the menu doesn't allow two of the same in the same costume). The one difference is m_killTarget 0 (the menu sets 1): with no
    // win target, EvaluateOnePlayerChallengeStatus just revives the knocked-down monster and the fight goes on, without the "PLAY AGAIN" dialog
    // that would wait for the pad. The opening movie (uiMain, 0x1D93D8) becomes a no-op. Each round end (entry of
    // Shell::EvaluateGameStatus, 0x1A7960) is logged as [match] with both monsters' health and wins.
    constexpr uint32_t kUserintMain = wotm::fn::userintMain__Fv, kUiMain = wotm::fn::uiMain__Fi, kEvaluateGameStatus = wotm::fn::EvaluateGameStatus__5Shelli;
    PS2Runtime::RecompiledFunction g_origEvaluate = nullptr;
    unsigned g_rounds = 0;
    constexpr uint32_t kShell = wotm::sym::shellInfo, kGame = wotm::sym::shellGame;   // shellInfo, shellGame
    // Up to 7 AIs (8 monsters): m_monsterSel[4 + i] runs past the header's [8] and lands in pad2958 (unused; retail Endurance already writes
    // m_monsterSel[8] with 5 AIs), m_costume[2 + i] goes up to index 8 (before m_mode at 0x2C10), m_monsters[4 + i] up to 10 (16 fit),
    // and stage + 8 monsters take 9 of fileStatus's 13 files. The practical ceiling is RAM: see wotmCheckLoadEnd.
    constexpr int kMatchMaxAIs = 7;
    int g_matchMonsters[kMatchMaxAIs + 1] = {-1, -1, -1, -1, -1, -1, -1, -1};   // player, then the AIs
    int g_matchCostumes[kMatchMaxAIs + 1] = {-1, -1, -1, -1, -1, -1, -1, -1};   // -1: the lowest free costume
    int g_matchAIs = 0, g_matchLevel = -1;
    // WOTM_PLAYERS=2: the first two WOTM_MATCH monsters are players 1 and 2 (m_monsterSel[1], m_costume[1], m_numPlayers 2:
    // split screen, like the menu's 2-player free for all); with WOTM_AIVSAI=1 both are AI-controlled. WOTM_PLAYERS=3 or 4: the
    // first three or four, with the screen in quadrants (wotm_split4.inc).
    // WOTM_PLAYERS=5..8: players 1..4 in the quadrants and 5..8 in the first AIs (m_monsters[4..7]), which read pads 4..7
    // instead of the AI (g_padAIs; wotm_multi.inc, aiUpdateInputsFromPad).
    int g_matchPlayers = 1;
    int g_padAIs = 0;
    int g_matchMode = 0, g_matchContinues = 2;   // WOTM_MODE=<mode>[:<continues>] (6 elimination, 7 dodgeball, 8 big shot, 9 crush)
    int g_matchKillTarget = 0;                   // WOTM_KILLTARGET=<n>: wins to end the match (0: never ends)
    unsigned g_matchCount = 0;
    bool g_aiVsAi = false;

    void userintMainDirect(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // network match: after the first match the game only comes back here through QUIT (end of fight or pause, ARE YOU SURE: YES). The
        // direct match would restart with a second stage load, which desyncs; instead it ends here and the game closes on every PC
        // on the same frame (the QUIT came from input exchanged over the network) and the launcher goes back to the lobby
        if (g_matchCount > 0 && wotm_sync::g_net)
        {
            std::fprintf(stderr, "[net] match ended by QUIT\n");
            runtime->requestStop();   // the same path as the window's X button
            SET_GPR_S32(ctx, 2, 0);
            ctx->pc = GPR_U32(ctx, 31);
            return;
        }
        for (uint32_t i = 0; i < 16; ++i)
            WRITE32(kShell + 0x2938 + 4 * i, 0);                         // m_monsterSel (4 players + up to 12 AIs, up to 0x2978)
        for (uint32_t i = 0; i < 12; ++i)
            WRITE32(kShell + 0x2BE0 + 4 * i, 0);                         // m_costume (up to 0x2C10, where m_mode lives)
        // costumes: requested ones first (the first to ask gets it), then the automatic ones in the lowest free slot for the same monster
        const int total = g_matchPlayers + g_matchAIs;
        int costumes[kMatchMaxAIs + 1];
        auto taken = [&](int i, int c) {
            for (int j = 0; j < total; ++j)
                if (j != i && costumes[j] == c && g_matchMonsters[j] == g_matchMonsters[i])
                    return true;
            return false;
        };
        for (int i = 0; i < total; ++i)
            costumes[i] = -1;
        for (int i = 0; i < total; ++i)
            if (g_matchCostumes[i] >= 0 && !taken(i, g_matchCostumes[i]))
                costumes[i] = g_matchCostumes[i];   // 4 or more: mod costume (MON/<PREFIX><n>.* served from mods/)
        for (int i = 0; i < total; ++i)
            for (int c = g_matchMonsters[i] == 13 ? 1 : 0; costumes[i] < 0; ++c)   // Goliath: costume 0 is a prototype (wotm_custom.inc)
                if (c > 3 || !taken(i, c))
                    costumes[i] = c & 3;   // more than 4 of the same: repeats (the menu never gets there)
        for (int i = 0; i < total; ++i)
        {
            const uint32_t costume = static_cast<uint32_t>(costumes[i]);
            const bool player = i < g_matchPlayers;
            const uint32_t ai = static_cast<uint32_t>(i - g_matchPlayers);
            const uint32_t slot = player ? uint32_t(i) : 4u + ai, costumeSlot = player ? uint32_t(i) : 2u + ai;
            WRITE32(kShell + 0x2938 + 4 * slot, static_cast<uint32_t>(g_matchMonsters[i] << 5));   // (monster << 5) | variant
            if (player && i >= 2)   // players 3 and 4: m_costume[2..] belongs to the AIs (wotm_split4.inc loads them through the AI path)
                wotm_split4::g_extraCostume[i - 2] = costume;
            else
                WRITE32(kShell + 0x2BE0 + 4 * costumeSlot, costume);
        }
        WRITE32(kShell + 0x2A44, static_cast<uint32_t>(g_matchKillTarget));   // m_killTarget
        WRITE32(kShell + 0x2BB0, static_cast<uint32_t>(g_matchLevel));   // m_levelNum
        WRITE32(kShell + 0x2BB4, static_cast<uint32_t>(g_matchPlayers));  // m_numPlayers
        WRITE32(kShell + 0x2BB8, static_cast<uint32_t>(g_matchAIs));     // m_numAIs
        for (uint32_t i = 0; i < 4; ++i)
            WRITE32(kShell + 0x2BBC + 4 * i, 3);                         // m_lives
        // m_mode: 1-player free for all (2) or multiplayer battle (3: TheGame::gameReestablishViews creates the two split-screen views
        // in modes 3, 6 and 7; with 2 the fight stayed in a single view and the cutscenes, which toggle the split, turned it on)
        // WOTM_MODE (with 2 or more players): elimination (6, continues in m_elimination and both m_continues, starting monsters
        // marked used the way retail's select screen leaves them) or a minigame (7 dodgeball, 8 big shot, 9 crush; the stage is 25, 26 or 27)
        const uint32_t mode = g_matchPlayers > 1 && g_matchMode ? uint32_t(g_matchMode) : g_matchPlayers > 1 ? 3u : 2u;
        WRITE32(kShell + 0x2C10, mode);
        WRITE32(kShell + 0x2B54, mode);                                   // m_battleMode
        WRITE32(kGame + 0x1203C8, mode);                                  // TheGame::m_gameMode
        if (mode == 6u)
        {
            WRITE32(kShell + 0x2B48, static_cast<uint32_t>(g_matchContinues));   // m_elimination
            WRITE32(kShell + 0x2B40, static_cast<uint32_t>(g_matchContinues));   // m_continues[0..1]
            WRITE32(kShell + 0x2B44, static_cast<uint32_t>(g_matchContinues));
            WRITE32(kShell + 0x2B4C, 0);                                          // m_wins[0..1]
            WRITE32(kShell + 0x2B50, 0);
            for (uint32_t c = 0; c < 10; ++c)   // no menu: the 10 elimination monsters marked free (SelectSlot costume 0: taken 1, state 1)
            {
                WRITE32(wotm::sym::monsterSelectMode + 0xB0u * c, 1);
                WRITE32(wotm::sym::monsterSelectMode + 0xB0u * c + 4, 1);
            }
            for (int i = 0; i < g_matchPlayers; ++i)
            {
                const uint32_t sel = static_cast<uint32_t>(g_matchMonsters[i] << 5);
                if (i < 2)
                    WRITE32(kShell + 0x2B84 + 4 * i, sel);
                wotmElimSetStart(i, sel);
                WRITE32(wotm::sym::monsterSelectMode + 0xB0u * static_cast<uint32_t>(wotm_modes4::monsterSlot(sel)), 0);   // taken: in use
                if (i < 2)
                    WRITE32(kShell + 0x2BE0 + 4 * i, 0);   // costume 0, like retail in elimination
                else
                    wotm_split4::g_extraCostume[i - 2] = 0;
            }
        }
        std::fprintf(stderr, "[match] %u: monster %d:%d (player%s)", ++g_matchCount, g_matchMonsters[0], costumes[0], g_aiVsAi ? " under AI" : "");
        for (int i = 1; i < g_matchPlayers; ++i)
            std::fprintf(stderr, " and %d:%d (player %d)", g_matchMonsters[i], costumes[i], i + 1);
        std::fprintf(stderr, " %s", g_matchAIs ? "vs" : (g_matchPlayers > 1 ? "no AI" : "alone"));
        for (int i = g_matchPlayers; i < g_matchPlayers + g_matchAIs; ++i)
            std::fprintf(stderr, " %d:%d", g_matchMonsters[i], costumes[i]);
        std::fprintf(stderr, "%s, stage %d\n", g_matchAIs ? " (AI)" : "", g_matchLevel);
        SET_GPR_S32(ctx, 2, 0);
        ctx->pc = GPR_U32(ctx, 31);
    }

    void uiMainSkipped(uint8_t *, R5900Context *ctx, PS2Runtime *) { ctx->pc = GPR_U32(ctx, 31); }

    void evaluateGameStatusLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kEvaluateGameStatus)
        {
            std::fprintf(stderr, "[match] round %u ended (rtMain returned %d):", ++g_rounds, static_cast<int32_t>(GPR_U32(ctx, 5)));
            for (uint32_t slot = 0; slot < 4u + static_cast<uint32_t>(kMatchMaxAIs); ++slot)   // m_monsters[0..] (players) and [4..] (AIs)
            {
                if (slot < 4u && slot >= static_cast<uint32_t>(std::max(g_matchPlayers, 1)))
                    continue;
                const uint32_t m = READ32(kGame + 0x120380 + 4 * slot);
                if (!m)
                    continue;
                const uint32_t hw = READ32(m + 0x44C);   // m_health
                float health;
                std::memcpy(&health, &hw, 4);
                std::fprintf(stderr, " monster %u health %.0f wins %d;", READ32(m + 0x14) >> 5, health, static_cast<int32_t>(READ32(m + 0x3C)));
            }
            std::fprintf(stderr, "\n");
        }
        g_origEvaluate(rdram, ctx, runtime);
    }

    // WOTM_AIVSAI=1: the player is also controlled by the game's AI. Monster::playerInit (0x165F10) jumps to Monster::aiInit
    // (0x166038) with the same registers, like a `j`: m_playerNum stays 2 and Monster::update reads inputs from Ai::updateInputs.
    // Camera and HUD stay on the player (SetPlayerMonster). Resuming mid-function lands directly in aiInit (the jump doesn't come back here).
    // TheGame::SetAIMonster (0x137B10) recomputes m_numAIs (0x1203E0) by counting slots with m_playerNum 2, which now includes the
    // player; m_monsters[4 + i] would be read past the last AI (sound banks, pickups). After it runs, the players are subtracted from the count.
    constexpr uint32_t kPlayerInit = wotm::fn::playerInit__7Monster, kAiInit = wotm::fn::aiInit__7Monster, kSetAIMonster = wotm::fn::SetAIMonster__7TheGameiiii;
    PS2Runtime::RecompiledFunction g_aiInit = nullptr, g_origSetAIMonster = nullptr;

    void playerInitAsAi(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        ctx->pc = kAiInit;
        g_aiInit(rdram, ctx, runtime);
    }

    void setAIMonsterCounted(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool entry = ctx->pc == kSetAIMonster;
        g_origSetAIMonster(rdram, ctx, runtime);
        if (!entry)
            return;
        const int32_t players = static_cast<int32_t>(READ32(kShell + 0x2BB4));
        int32_t n = static_cast<int32_t>(READ32(kGame + 0x1203E0));
        for (int32_t i = 0; i < players && i < 4; ++i)
        {
            const uint32_t m = READ32(kGame + 0x120380 + 4 * i);   // m_monsters[i]
            if (m && READ32(m + 0x18) == 2u)                       // m_playerNum
                --n;
        }
        WRITE32(kGame + 0x1203E0, static_cast<uint32_t>(n < 0 ? 0 : n));
    }

#include "wotm_multi.inc"

    // Pad aside, the game still treats player slots as human wherever m_playerNum decides something other than input:
    // TheGame::gameGetStartPoint (0x138D68; with m_playerNum 2 the player spawned at an AI point and the AI at the player's) and
    // Cameras::Update (0x11C770; it only calls Camera::checkCollision, which builds the sight points for the "ghost" buildings, for a
    // monster camera with m_playerNum 1). During those two functions the players go back to 1. Neither one waits.
    constexpr uint32_t kGameGetStartPoint = wotm::fn::gameGetStartPoint__7TheGameP7Monster, kCamerasUpdate = wotm::fn::Update__7Cameras;
    PS2Runtime::RecompiledFunction g_origStartPoint = nullptr, g_origCamerasUpdate = nullptr;

    void callAsHumans(PS2Runtime::RecompiledFunction orig, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        uint32_t flipped[4];
        int n = 0;
        const int32_t players = static_cast<int32_t>(READ32(kShell + 0x2BB4));
        for (int32_t i = 0; i < players && i < 4; ++i)
        {
            const uint32_t m = READ32(kGame + 0x120380 + 4 * i);   // m_monsters[i]
            if (m && READ32(m + 0x18) == 2u)
            {
                WRITE32(m + 0x18, 1u);
                flipped[n++] = m;
            }
        }
        orig(rdram, ctx, runtime);
        while (n > 0)
            WRITE32(flipped[--n] + 0x18, 2u);
    }

    void startPointAsHumans(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { callAsHumans(g_origStartPoint, rdram, ctx, runtime); }
    void camerasUpdateAsHumans(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { callAsHumans(g_origCamerasUpdate, rdram, ctx, runtime); }

    // 1-player free for all with 0 to 7 AIs (mod, always on): screenFreeForAllOptions1P (screen 12) only moves the cursor over 1..3
    // AIs. Retail already handles item 0 on confirm (m_numAIs 0, skips "first to" and goes straight to monster select, screen 3, which with no
    // AI moves on to the stage; in the fight EvaluateOnePlayerChallengeStatus just revives the player), and the number switch (shell +0x43C) has
    // 11 children, the game's own digits 0 to 10. Here the cursor wraps over 0..7: after the original runs, the step it took (it
    // only knows 1..3) becomes a step of 1 within 0..7, and the switch gets the same value (whichChild, as hierSetSwitch would do). With the
    // cursor on 4..7 the original runs with 3 (it accepts no more and just logs an error on confirm): confirming writes 3 to m_numAIs
    // and goes to "first to"; the requested number stays in wotm_menucpus::g_want, and the menu carries on as with 3 CPUs until the stage is confirmed
    // (changeScreenHook in wotm_padsel.inc).
    constexpr uint32_t kFfaOptions1P = wotm::fn::screenFreeForAllOptions1P__Fv;
    constexpr uint32_t kFfaCursor = wotm::sym::currentSelection + 12 * 4;   // currentSelection[12]
    constexpr uint32_t kFfaNumAIs = kShell + 0x2BB8;                         // m_numAIs
    PS2Runtime::RecompiledFunction g_origFfaOptions1P = nullptr;
    uint32_t g_ffaRa = 0, g_ffaBefore = 0;
    bool g_ffaPending = false, g_ffaFake = false;

    void ffaOptions1PWithZero(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        constexpr int kMax = wotm_menucpus::kMax;
        if (ctx->pc == kFfaOptions1P)
        {
            g_ffaRa = GPR_U32(ctx, 31);
            g_ffaBefore = READ32(kFfaCursor);
            g_ffaPending = true;
            g_ffaFake = g_ffaBefore > 3u && g_ffaBefore <= uint32_t(kMax);
            if (g_ffaFake)
            {
                WRITE32(kFfaCursor, 3);
                WRITE32(kFfaNumAIs, 0);   // confirming sets it to 3 (the screen doesn't read m_numAIs; returning resets it)
            }
        }
        g_origFfaOptions1P(rdram, ctx, runtime);
        if (!g_ffaPending || ctx->pc != g_ffaRa)
            return;   // stopped midway (resuming lands here again and finishes the job)
        g_ffaPending = false;
        const int before = int(g_ffaBefore), eff = g_ffaFake ? 3 : before, after = int(READ32(kFfaCursor));
        if (g_ffaFake && READ32(kFfaNumAIs) == 3u)
        {
            wotm_menucpus::g_want.store(before);
            std::fprintf(stderr, "[wotm] screen 12: %d CPUs (the menu continues with 3; confirming the stage writes %d)\n", before, before);
        }
        else if (READ32(kFfaNumAIs) != 0u || after != eff)
            wotm_menucpus::g_want.store(0);   // confirmed 1..3 (or moved the cursor)
        // the original's step (up: >= 2 goes down 1, otherwise goes to 3; down: < 3 goes up 1, otherwise goes to 1)
        const bool dec = (eff >= 2 && after == eff - 1) || (eff <= 1 && after == 3);
        const bool inc = (eff < 3 && after == eff + 1) || (eff >= 3 && after == 1);
        const int want = dec ? (before + kMax) % (kMax + 1) : inc ? (before + 1) % (kMax + 1) : before;
        if (want == after)
            return;
        WRITE32(kFfaCursor, uint32_t(want));
        const uint32_t sw = READ32(kShell + 0x43C);
        if (sw && (READ8(sw) & 0x3F) == 6 && READ8(sw + 0xB) > want)   // SWITCH_NODE, numKids
            WRITE8(sw + 8, uint8_t(want));                              // whichChild
    }

    // 2- to 4-player free for all with 0 to 8 - N AIs (8 monsters): the same as screen 12, for screenFreeForAllOptions2P (screen 13), which
    // moves the cursor over 0..2 (left: > 0 goes down 1, otherwise goes to 2; right: < 2 goes up 1, otherwise goes to 0) and confirms 0..2 into m_numAIs,
    // going to "first to" (15). The number switch (shell +0x464) has the same 11 children. With the cursor above what the game knows
    // (2 with 2 players; none with 3 and 4, which retail doesn't have: wotm_padsel.inc's 3- and 4-player select sets the CPUs) the original runs
    // with that maximum, and confirming stores the number in wotm_menucpus::g_want.
    constexpr uint32_t kFfaOptions2P = wotm::fn::screenFreeForAllOptions2P__Fv;
    constexpr uint32_t kFfa2Cursor = wotm::sym::currentSelection + 13 * 4;   // currentSelection[13]
    PS2Runtime::RecompiledFunction g_origFfaOptions2P = nullptr;
    uint32_t g_ffa2Ra = 0, g_ffa2Before = 0;
    bool g_ffa2Pending = false, g_ffa2Fake = false;
    int g_ffa2Game = 2;

    void ffaOptions2PMore(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        constexpr int kGame = 2;   // the original's step moves within 0..2
        const int players = std::clamp(wotm_menucpus::g_menuPlayers.load(), 2, 4), kMax = wotm_menucpus::maxFor(players);
        if (ctx->pc == kFfaOptions2P)
        {
            g_ffa2Ra = GPR_U32(ctx, 31);
            g_ffa2Before = std::min(READ32(kFfa2Cursor), uint32_t(kMax));   // a cursor left over from a different player count
            WRITE32(kFfa2Cursor, g_ffa2Before);
            g_ffa2Pending = true;
            g_ffa2Game = players == 2 ? kGame : 0;   // the highest number the game confirms on its own
            g_ffa2Fake = g_ffa2Before > uint32_t(g_ffa2Game) && g_ffa2Before <= uint32_t(kMax);
            if (g_ffa2Fake)
                WRITE32(kFfa2Cursor, uint32_t(g_ffa2Game));
        }
        g_origFfaOptions2P(rdram, ctx, runtime);
        if (!g_ffa2Pending || ctx->pc != g_ffa2Ra)
            return;
        g_ffa2Pending = false;
        const int before = int(g_ffa2Before), eff = g_ffa2Fake ? g_ffa2Game : before, after = int(READ32(kFfa2Cursor));
        const bool confirmed = READ32(wotm::sym::currScreen) == 15u;
        if (g_ffa2Fake && confirmed)
        {
            wotm_menucpus::g_want.store(before);
            std::fprintf(stderr, "[wotm] screen 13: %d players and %d CPUs (the menu continues with %d)\n", players, before, g_ffa2Game);
        }
        else if (confirmed || after != eff)
            wotm_menucpus::g_want.store(0);
        const bool dec = (eff >= 1 && after == eff - 1) || (eff == 0 && after == kGame);
        const bool inc = (eff < kGame && after == eff + 1) || (eff == kGame && after == 0);
        const int want = dec ? (before + kMax) % (kMax + 1) : inc ? (before + 1) % (kMax + 1) : before;
        if (want == after)
            return;
        WRITE32(kFfa2Cursor, uint32_t(want));
        const uint32_t sw = READ32(kShell + 0x464);
        if (sw && (READ8(sw) & 0x3F) == 6 && READ8(sw + 0xB) > want)
            WRITE8(sw + 8, uint8_t(want));
    }

    // Debug cheats (wotm_live.inc's F1 menu, or WOTM_CHEATS at startup): applied on the EE thread, on entry to Monster::update
    // (0x15B488) for each player (m_monsters[0 .. m_numPlayers-1]), before that player's frame logic. The window only writes g_wotmCheats.
    //   invincible: Monster::takeDamage (0x15FE40) returns on entry when the victim is a player (not even forced damage gets through).
    //   stamina:    1 = full (cur = max 100, no exhaustion); 2 = at rage level (cur = maxLevel, 130).
    //   special:    m_specialWeapon (+0xF3) = 1, the unlock that drainSpecial clears and isSpecialAvailable reads.
    //   cloak:      without CLOAK (m_cloaked, +0xEF), calls Monster::setCloakOn (0x162CD0), like the power-up; when it runs out, turns it on again.
    //   god:        m_godMode (+0xE9), the game's own mode: no damage, health, stamina and special refilled every frame.
    //   win:        one-shot action: takeDamage(100000, force, player) on every living AI (m_monsters[4 + i], i < m_numAIs).
    // WOTM_CLOAK=1 (CLOAK test in the VU1 check) is the same as WOTM_CHEATS=cloak.
    constexpr uint32_t kMonsterUpdate = wotm::fn::update__7Monster, kSetCloakOn = wotm::fn::setCloakOn__7Monster,
                       kTakeDamage = wotm::fn::takeDamage__7MonsterfbP7Monster;
    PS2Runtime::RecompiledFunction g_origMonsterUpdate = nullptr, g_setCloakOn = nullptr, g_origTakeDamage = nullptr;
    uint32_t g_godApplied = 0;
    int g_autoWinSecs = 0;   // WOTM_CHEATS=vencer=N   // bits of the player slots where m_godMode was turned on from here (so only those get turned off)

    bool isPlayerMonster(uint8_t *rdram, uint32_t m, int *slot = nullptr)
    {
        int32_t players = static_cast<int32_t>(FAST_READ32(kShell + 0x2BB4));   // m_numPlayers
        if (players < 1 || players > 4)
            players = 1;
        for (int32_t i = 0; i < players; ++i)
            if (m && FAST_READ32(kGame + 0x120380 + 4 * i) == m)
            {
                if (slot)
                    *slot = i;
                return true;
            }
        return false;
    }

    void callTakeDamage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t victim, float damage, uint32_t source)
    {
        const R5900Context saved = *ctx;
        ctx->r[4] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(victim)));
        ctx->r[5] = _mm_set_epi64x(0, 1);   // force
        ctx->r[6] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(source)));
        ctx->r[31] = _mm_set_epi64x(0, 0);
        ctx->f[12] = damage;
        ctx->pc = kTakeDamage;
        g_origTakeDamage(rdram, ctx, runtime);
        *ctx = saved;
    }

    void takeDamageCheat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kTakeDamage && (g_wotmCheats.load(std::memory_order_relaxed) & kCheatInvincible) && isPlayerMonster(rdram, GPR_U32(ctx, 4)))
        {
            static uint32_t blocked = 0;
            if (++blocked <= 3 || blocked % 100 == 0)
                std::fprintf(stderr, "[debug] invincible: damage blocked on player (%u)\n", blocked);
            ctx->pc = GPR_U32(ctx, 31);
            return;
        }
        g_origTakeDamage(rdram, ctx, runtime);
    }

    // God mode with no hit reaction: for a player with kCheatGod, Monster::takeHit (0x15F048, the virtual entry for every hit),
    // knockBack (0x1601C8) and takeAdditiveRecoil (0x160058) return on entry, and enterNewState (0x1621C8) rejects the
    // reaction states, as the game itself does in mode 7 (explosion and collision pushes call knockBack or enter the state from outside).
    constexpr uint32_t kTakeHit = wotm::fn::takeHit__7MonsterP8_fvectorfi, kKnockBack = wotm::fn::knockBack__7MonsterR8_fvectorff,
                       kAdditiveRecoil = wotm::fn::takeAdditiveRecoil__7MonsterR8_fvectorf, kEnterNewState = wotm::fn::enterNewState__7MonsterP12MonsterState;
    PS2Runtime::RecompiledFunction g_origTakeHit = nullptr, g_origKnockBack = nullptr, g_origAdditiveRecoil = nullptr, g_origEnterNewState = nullptr;
    // offsets of the reaction states inside Monster (game.h): Recoil, KnockBack, Grappled, BeingSlammed, BeingThrown, Impaled,
    // Stunned, Shocked, BigTakeHit, Countered
    constexpr uint32_t kHitStates[] = {0x7E30, 0x7EA0, 0xDDCC, 0xDE30, 0xE3E0, 0x106E0, 0x10714, 0x10BA0, 0x11100, 0x11114};

    bool godPlayer(uint8_t *rdram, uint32_t m)
    {
        return (g_wotmCheats.load(std::memory_order_relaxed) & kCheatGod) && isPlayerMonster(rdram, m);
    }

    void godBlocked(const char *what)
    {
        static uint32_t n = 0;
        if (++n <= 3 || n % 100 == 0)
            std::fprintf(stderr, "[debug] god: %s rejected on player (%u)\n", what, n);
    }

    void takeHitCheat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kTakeHit && godPlayer(rdram, GPR_U32(ctx, 4)))
        {
            godBlocked("takeHit");
            ctx->pc = GPR_U32(ctx, 31);
            return;
        }
        g_origTakeHit(rdram, ctx, runtime);
    }

    void knockBackCheat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kKnockBack && godPlayer(rdram, GPR_U32(ctx, 4)))
        {
            godBlocked("knockBack");
            ctx->pc = GPR_U32(ctx, 31);
            return;
        }
        g_origKnockBack(rdram, ctx, runtime);
    }

    void additiveRecoilCheat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kAdditiveRecoil && godPlayer(rdram, GPR_U32(ctx, 4)))
        {
            ctx->pc = GPR_U32(ctx, 31);
            return;
        }
        g_origAdditiveRecoil(rdram, ctx, runtime);
    }

    void enterNewStateCheat(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = GPR_U32(ctx, 4);
        if (ctx->pc == kEnterNewState && godPlayer(rdram, m))
        {
            const uint32_t off = GPR_U32(ctx, 5) - m;
            for (uint32_t s : kHitStates)
                if (off == s)
                {
                    char what[48];
                    std::snprintf(what, sizeof what, "state +0x%X", s);
                    godBlocked(what);
                    ctx->pc = GPR_U32(ctx, 31);
                    return;
                }
        }
        g_origEnterNewState(rdram, ctx, runtime);
    }

    void monsterUpdateCheats(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = GPR_U32(ctx, 4);
        int slot = 0;
        if (ctx->pc == kMonsterUpdate && isPlayerMonster(rdram, m, &slot))
        {
            const uint32_t c = g_wotmCheats.load(std::memory_order_relaxed);
            const uint32_t st = (c & kCheatStaminaMask) / kCheatStamina1;
            if (st == 1u)
            {
                WRITE32(m + 0x470, READ32(m + 0x460));   // StaminaMeter (+0x460): cur = max
                WRITE32(m + 0x474, 0);                    // recoverPool
                WRITE32(m + 0x478, 0);                    // exhaustTimer
                WRITE32(m + 0x480, 0);                    // exhausted
            }
            else if (st == 2u)
            {
                WRITE32(m + 0x470, READ32(m + 0x468));   // cur = maxLevel (rage)
                WRITE32(m + 0x474, 0);
                WRITE32(m + 0x478, 0);
                WRITE32(m + 0x480, 0);
            }
            if (c & kCheatSpecial)
                WRITE8(m + 0xF3, 1);   // m_specialWeapon
            const uint32_t bit = 1u << slot;
            if (c & kCheatGod)
            {
                WRITE8(m + 0xE9, 1);   // m_godMode
                g_godApplied |= bit;
            }
            else if (g_godApplied & bit)
            {
                WRITE8(m + 0xE9, 0);
                g_godApplied &= ~bit;
            }
            if ((c & kCheatCloak) && g_setCloakOn && !READ8(m + 0xEF))   // m_cloaked
            {
                const R5900Context saved = *ctx;
                ctx->pc = kSetCloakOn;
                g_setCloakOn(rdram, ctx, runtime);
                *ctx = saved;
            }
            if (slot == 0 && g_autoWinSecs > 0)
            {   // WOTM_CHEATS=vencer=N: every N seconds of game time (60 g_frame frames per second), the same request as key 6
                static uint32_t last = 0;
                const uint32_t f = READ32(wotm::sym::g_frame);
                if (last == 0 || f < last)
                    last = f;
                else if (f - last >= static_cast<uint32_t>(g_autoWinSecs) * 60u)
                {
                    last = f;
                    g_wotmCheatWin = true;
                }
            }
            if (slot == 0 && g_wotmCheatWin.exchange(false))
            {
                const int32_t ais = static_cast<int32_t>(READ32(kGame + 0x1203E0));   // m_numAIs
                int killed = 0;
                for (int32_t i = 0; i < ais && i < 4; ++i)
                {
                    const uint32_t ai = READ32(kGame + 0x120390 + 4 * i);   // m_monsters[4 + i]
                    if (ai && !READ8(ai + 0xE8))                           // m_dead
                    {
                        callTakeDamage(rdram, ctx, runtime, ai, 100000.0f, m);
                        ++killed;
                    }
                }
                for (int i = 1; i < g_matchPlayers; ++i)   // 2+ players (WOTM_PLAYERS): the other players go down too
                {
                    const uint32_t other = READ32(kGame + 0x120380 + 4 * i);   // m_monsters[i]
                    if (other && !READ8(other + 0xE8))
                    {
                        callTakeDamage(rdram, ctx, runtime, other, 100000.0f, m);
                        ++killed;
                    }
                }
                std::fprintf(stderr, "[debug] win the fight: %d AI(s) knocked down out of %d\n", killed, ais);
            }
        }
        g_origMonsterUpdate(rdram, ctx, runtime);
    }

    // WOTM_TURNLOG: one [turn] line per MonsterDynamics::updatePosition (each monster, each game frame):
    // frame (g_frame), monster, rate, rotAccel (+0x110, zeroed by the function), rotVel (+0x100) before/after and the angles
    // (Monster+0xC -> +0x60, in radians) after integration. The rate shown is the variable's (without the floor).
    constexpr uint32_t kMonsterDynamicsUpdatePosition = wotm::fn::updatePosition__15MonsterDynamics;
    PS2Runtime::RecompiledFunction g_origUpdatePosition = nullptr;

    void updatePositionLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        auto readF = [&](uint8_t *, uint32_t addr)
        {
            const uint32_t w = READ32(addr);
            float f;
            std::memcpy(&f, &w, 4);
            return f;
        };
        const uint32_t dyn = GPR_U32(ctx, 4);
        float acc[3], v0[3];
        for (int i = 0; i < 3; ++i)
        {
            acc[i] = readF(rdram, dyn + 0x110 + 4 * i);
            v0[i] = readF(rdram, dyn + 0x100 + 4 * i);
        }
        const int32_t rate = static_cast<int32_t>(READ32(kUpdateRateVar));
        g_origUpdatePosition(rdram, ctx, runtime);
        const uint32_t mon = READ32(dyn);
        const uint32_t cs = READ32(mon + 0xC);
        std::fprintf(stderr, "[turn] g=%u mon=%06x rate=%d acc=%+.3f,%+.3f,%+.3f vel=%+.3f,%+.3f,%+.3f -> %+.3f,%+.3f,%+.3f ang=%+.3f,%+.3f,%+.3f\n",
                     READ32(wotm::sym::g_frame), mon, rate, acc[0], acc[1], acc[2], v0[0], v0[1], v0[2], readF(rdram, dyn + 0x100), readF(rdram, dyn + 0x104),
                     readF(rdram, dyn + 0x108), readF(rdram, cs + 0x60), readF(rdram, cs + 0x64), readF(rdram, cs + 0x68));
    }

    // WOTM_FIELDLOG: one [field] line per game frame, detected when timerWaitForMinUpdateRate updates
    // D_006F8DC0 (field count at frame start). Shows the fields the game measured with timer 1 (D_006F8DB4),
    // the virtual EE cycles spent (in fields: total, skipped while idle, and the 3 checkpoint pcs that spent the most) and the
    // real time (total, idle waiting for an event, and waiting for the wall clock to release a deadline).
    bool g_fieldLog = false;
    double g_hierMs[4] = {}, g_captureMs[4] = {};   // per view: hier (both passes) and native capture; see hierTapped
    unsigned g_hierViews = 0;                        // bits: views with hier(v, 0) this frame
    // Calls to sceGsSyncV during the frame (each one waits 1 vsync) and the $ra of the first 4: shows who spends the fields.
    constexpr uint32_t kSceGsSyncV = wotm::fn::sceGsSyncV;
    PS2Runtime::RecompiledFunction g_origSyncV = nullptr;
    uint32_t g_syncVCalls = 0, g_syncVRa[4];
    // displayDialog's loop (pause menu, areYouSure, multiplayer end of fight...): builds the frame by itself (hier, particleDraw,
    // Hud::print, the dialog's function, fontDmaFontData) and waits for vsync in the sceGsSyncV at the end of the loop, without going through
    // timerWaitForMinUpdateRate. With no capture there, the screen repeated the last frame from before the dialog, and the pause frame (the _cs
    // shell->m_cs610, which pauseLevelOverlay turns on) never showed up. $ra of that jal: displayDialog + 0x7C4 (jal at 0x1ACB9C).
    // On the last pass ($s1 != 0; the beqz right after loops back with 0) Shell::FadeScreen has just darkened the screen: capturing
    // there would turn off the fade (wotmCaptureFrame) and show the bright scene until the next frame. It stays without a capture, as before.
    constexpr uint32_t kDialogLoopSyncVRa = wotm::fn::displayDialog__Fi + 0x7C4u;
    // Shell::FadeScreen's two loops (a step with hier(0, 0) when b = true; jal sceGsSyncV at +0x2A4 and +0x40C): wotmCaptureFadeStep
    // publishes the step's scene, if there was a hier, without turning off the fade.
    constexpr uint32_t kFadeStepSyncVRa[2] = {wotm::fn::FadeScreen__5ShellibRUiT3UcUcUcUcUcUc + 0x2ACu,
                                              wotm::fn::FadeScreen__5ShellibRUiT3UcUcUcUcUcUc + 0x414u};
    void syncVTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t fadeRa = 0;
        if (ctx->pc == kSceGsSyncV)
        {
            wotm_save::poll(rdram);   // tokens and unlocks that changed outside a save go to save.json
            const uint32_t ra = GPR_U32(ctx, 31);
            if (g_fieldLog)
            {
                if (g_syncVCalls < 4)
                    g_syncVRa[g_syncVCalls] = ra;
                if (g_syncVCalls == 0 && g_frameStartValid)
                {
                    const auto t = std::chrono::steady_clock::now();
                    g_svAtMs = std::chrono::duration<double, std::milli>(t - g_frameStartHost).count();
                    g_svVblanks = static_cast<int>(runtime->eeScheduler().currentVSyncTick() - g_frameStartVsync);
                }
                ++g_syncVCalls;
            }
            if (ra == kDialogLoopSyncVRa && GPR_U32(ctx, 17) == 0u)
                wotmCaptureFrame(rdram);
            // each pass of the dialog loop is one fixed-step frame: it reads the pad (inputUpdate) and waits for vsync without going through
            // timerWaitForMinUpdateRate. Without this the frame index stopped and, over the network, every PC read forever the input of the
            // frame where the dialog opened (the menu didn't move). Here the index advances, local input is exchanged and the state goes into the hash
            if (ra == kDialogLoopSyncVRa && wotm_sync::g_fixedStep)
                wotm_sync::frame(rdram, kFieldCounter, kFieldCounterAtStart, kFieldsForAverage);
            else if (ra == kFadeStepSyncVRa[0] || ra == kFadeStepSyncVRa[1])
            {
                wotmCaptureFadeStep(rdram);
                wotmShellFadeStep();
                fadeRa = ra;
            }
        }
        g_origSyncV(rdram, ctx, runtime);
        if (fadeRa && ctx->pc == fadeRa)
        {
            fadeRa = 0;
            wotmShellFadeStepDone();
        }
    }
    uint32_t g_lastFieldStart = 0;
    uint64_t g_lastCycle = 0, g_lastIdleUs = 0;
    std::chrono::steady_clock::time_point g_lastHost;
    bool g_hostProfWatch = false;

    void fieldLogFrame(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t start = READ32(0x6F8DC0u);
        if (start == g_lastFieldStart)
            return;
        g_lastFieldStart = start;
        const auto now = std::chrono::steady_clock::now();
        const uint64_t idleUs = g_wotmIdleUs.load(std::memory_order_relaxed);
        int top[3] = {-1, -1, -1};
        for (int i = 0; i < 4096; ++i)
        {
            if (!g_wotmProfCyc[i])
                continue;
            for (int k = 0; k < 3; ++k)
            {
                if (top[k] < 0 || g_wotmProfCyc[i] > g_wotmProfCyc[top[k]])
                {
                    for (int m = 2; m > k; --m)
                        top[m] = top[m - 1];
                    top[k] = i;
                    break;
                }
            }
        }
        const double field = 294912000.0 / 59.94;
        static uint32_t lastTics = 0;   // host ticks (native clock) or D_006F8DC4, the emulated timer 1's (9619 per field)
        const uint32_t tics = g_nativeTimer ? hostTics() : READ32(0x6F8DC4u);
        static double lastCpuMs = 0.0;   // EE thread CPU (user + kernel): a slow frame with little CPU = blocked outside idle
        const double cpuMs = wotmThreadCpuMs();
        // the line in a single write: stderr is unbuffered, and each fprintf would be a system call on the EE thread
        char line[512];
        int len = 0;
        auto add = [&](const char *fmt, auto... a) {
            if (len < int(sizeof(line)))
                len += std::snprintf(line + len, sizeof(line) - size_t(len), fmt, a...);
        };
        add("[field] g=%u fields=%d rate=%d tics=%.2f ee=%.2f idle=%.2f host=%.1fms hostidle=%.1fms cpu=%.1fms top",
                     READ32(wotm::sym::g_frame), static_cast<int32_t>(READ32(0x6F8DB4u)), static_cast<int32_t>(READ32(kUpdateRateVar)),
                     (tics - lastTics) / 9619.0,
                     (g_wotmEeCycle - g_lastCycle) / field, g_wotmIdleCycles / field,
                     std::chrono::duration<double, std::milli>(now - g_lastHost).count(), (idleUs - g_lastIdleUs) / 1000.0,
                     cpuMs - lastCpuMs);
        lastCpuMs = cpuMs;
        for (int k = 0; k < 3; ++k)
            if (top[k] >= 0)
                add(" %06x=%.2f", g_wotmProfPc[top[k]], g_wotmProfCyc[top[k]] / field);
        add(" syncv=%u", g_syncVCalls);
        if (g_svAtMs >= 0.0)   // sv=<ms to the 1st call>@<vblanks already delivered>/<ms to here, the end of the frame>
            add(" sv=%.1f@%d/%.1f", g_svAtMs, g_svVblanks,
                         std::chrono::duration<double, std::milli>(now - g_frameStartHost).count());
        g_svAtMs = -1.0;
        for (uint32_t k = 0; k < g_syncVCalls && k < 4; ++k)
            add("%c%06x", k ? ',' : ':', g_syncVRa[k]);
        g_syncVCalls = 0;
        unsigned views = 0;
        for (unsigned v = 0; v < 4; ++v)
            views += (g_hierViews >> v) & 1u;
        add(" views=%u hier=%.2f/%.2f cap=%.2f/%.2f", views, g_hierMs[0], g_hierMs[1], g_captureMs[0], g_captureMs[1]);
        if (g_hierViews & 0xCu)   // 3 and 4 players (wotm_split4.inc): views 2 and 3
            add(" hier23=%.2f/%.2f cap23=%.2f/%.2f", g_hierMs[2], g_hierMs[3], g_captureMs[2], g_captureMs[3]);
        len = std::min(len, int(sizeof(line)) - 2);
        line[len++] = 10;
        std::fwrite(line, 1, size_t(len), stderr);
        g_hierViews = 0;
        for (int v = 0; v < 4; ++v)
            g_hierMs[v] = g_captureMs[v] = 0.0;
        // with PS2X_HOSTPROF: where the EE thread was during a frame longer than 100 ms (ps2_hostprof.cpp)
        if (!g_hostProfWatch && std::getenv("PS2X_HOSTPROF") && !std::getenv("PS2X_HOSTPROF_WIN"))
        {
            g_hostProfWatch = true;
            wotmHostProfWatchThisThread();
        }
        if (g_hostProfWatch && now - g_lastHost > std::chrono::milliseconds(100))
        {
            char tag[48];
            std::snprintf(tag, sizeof(tag), "frame g=%u", READ32(wotm::sym::g_frame));
            wotmHostProfDump(g_lastHost.time_since_epoch().count(), now.time_since_epoch().count(), tag);
        }
        if (g_hostProfWatch)
            wotmHostProfFrameStart();
        std::memset(g_wotmProfPc, 0, sizeof(g_wotmProfPc));
        std::memset(g_wotmProfCyc, 0, sizeof(g_wotmProfCyc));
        g_wotmIdleCycles = 0;
        g_lastCycle = g_wotmEeCycle;
        g_lastHost = now;
        g_lastIdleUs = idleUs;
        lastTics = tics;
    }


    void emitChaseHud(uint8_t *rdram);   // HUD for players 5..8 in the chase view, further below (next to the bars)
    // 3 and 4 players at 60: the game turns on a "30 Hz mode" with 3+ players (Shell::Use30HzMode: m_numPlayers >= 3), and
    // Shell::BootInitGame calls timerInit(30), which sets the updateRate ceiling (D_006F8DD8) to 30: timerWaitForMinUpdateRate waits
    // one extra vsync on every frame that fit in 1 field. Here timerInit at that call site gets 60; the rest of the mode (the VRAM layout
    // in getVramAddr) stays. WOTM_30HZ=1 goes back to the original.
    constexpr uint32_t kTimerInit = wotm::fn::timerInit__Fi;
    constexpr uint32_t kBootInitGameTimerRa = wotm::fn::BootInitGame__5Shell + 0x24u;   // return address of the jal timerInit
    PS2Runtime::RecompiledFunction g_origTimerInit = nullptr;
    void timerInit60(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kTimerInit && GPR_U32(ctx, 31) == kBootInitGameTimerRa && GPR_U32(ctx, 4) == 30u)
            SET_GPR_U32(ctx, 4, 60u);
        g_origTimerInit(rdram, ctx, runtime);
    }

    void waitMinRateByVblanks(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        // only on entry: resuming mid-function (after a sceGsSyncV in the loop) already has D_006F8DC0 updated
        if (ctx->pc == kTimerWaitForMinUpdateRate)
        {
            // [field] for the previous frame: this runs every frame (timerGetUpdateRate doesn't: in the menu and at the start of a stage
            // there are frames with no call, and the line added up two frames)
            if (g_fieldLog)
                fieldLogFrame(rdram, ctx, runtime);
            wotm_modifiers::statsFrame(rdram);
            // frame ready (update, hier, particleDraw) and the next one hasn't started: native drawing copies the scene here
            emitChaseHud(rdram);
            wotm_split4::trainingHudLate(rdram, ctx, runtime);   // training mode: the dummy's HUD if HUD 0's update was paused
            wotmCaptureFrame(rdram);
            // pause (F7/F6/F8, wotm_foto.inc): the finished frame stays on screen and the next one doesn't start. A frame with a stop counts as 1 field:
            // the stopped time doesn't become dt (without this updateRate dropped to the floor and the game took a 1/30 step on resume)
            wotmPauseBlock(runtime);
            if (wotmPauseTookFrame())
                WRITE32(kFieldCounter, READ32(kFieldCounterAtStart) + 1u);
            else if ((g_nativeTimer || g_wotmFreeSpinPc != 0u) && g_frameStartValid)
            {
                const uint64_t vblanks = (runtime->eeScheduler().currentVSyncTick() - g_frameStartVsync) * g_clockScale;   // turbo: 2 fields per vsync
                const int32_t start = static_cast<int32_t>(READ32(kFieldCounterAtStart));
                const int32_t measured = static_cast<int32_t>(READ32(kFieldCounter)) - start;
                if (vblanks > 0u && vblanks < 16u && static_cast<int32_t>(vblanks) > measured)
                    WRITE32(kFieldCounter, static_cast<uint32_t>(start + static_cast<int32_t>(vblanks)));
            }
#if !PS2X_HW_EMU
            // Fixed step n > 1 (the 30 Hz mode of 3+ player network matches): frame() makes the frame exactly n fields long, which also
            // satisfies the game's own wait below, so without this a fast PC ran each 2-field frame in one vsync, at up to twice the
            // speed. The wait is in place on the vblank clock, not through the game's sceGsSyncV, whose fixed-step parity counts calls
            // and must be the same on every PC. A turbo fight is 1 vsync per 2-field frame on purpose (wotm::modifiers::vsyncsPerFrame).
            const int frameVsyncs = wotm::modifiers::vsyncsPerFrame(wotm_sync::g_fixedStep, wotm_sync::g_mods);
            if (frameVsyncs > 1 && g_frameStartValid)
            {
                EeScheduler &ee = runtime->eeScheduler();
                const uint64_t until = g_frameStartVsync + static_cast<uint64_t>(frameVsyncs);
                while (ee.currentVSyncTick() < until && !runtime->isStopRequested())
                    ee.waitVSyncHere(ee.currentVSyncTick(), 0);
            }
#endif
            wotm_sync::frame(rdram, kFieldCounter, kFieldCounterAtStart, kFieldsForAverage);   // fixed step and state hash (WOTM_FIXEDSTEP, WOTM_STATEHASH)
        }
        g_origWaitMinRate(rdram, ctx, runtime);
    }

    // Fades for native drawing (wotm_live.inc, Scene::drawScreenRects): the game darkens the screen with a black GS SPRITE built on
    // the fly. Native redoes it from each emitter's arguments, without reading the packet: rtFadeScreen (fight start) and TheGame::updateFadeOut/
    // updateFadeOutAndIn (end of round, cutscenes) go through the gsPkt queue; uiFadeScreen (menus) and letterboxDmaData send directly;
    // Shell::FadeScreen is a loop that writes one alpha per vsync (wotmShellFade on entry, wotmShellFadeStep on the loop's sceGsSyncV).
    constexpr uint32_t kUiFadeScreen = wotm::fn::uiFadeScreen__FUi, kShellFadeScreen = wotm::fn::FadeScreen__5ShellibRUiT3UcUcUcUcUcUc;
    PS2Runtime::RecompiledFunction g_origUiFade = nullptr, g_origShellFade = nullptr;

    constexpr uint32_t kRtFadeScreen = wotm::fn::rtFadeScreen__FiUi, kTheGameFadeOut = wotm::fn::updateFadeOut__7TheGame,
                       kTheGameFadeOutAndIn = wotm::fn::updateFadeOutAndIn__7TheGame;
    PS2Runtime::RecompiledFunction g_origRtFade = nullptr, g_origGameFadeOut = nullptr, g_origGameFadeOutAndIn = nullptr;

    void rtFadeScreenTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kRtFadeScreen)   // rtFadeScreen(view, amount): alpha 0x80 - amount, through the gsPkt queue
            wotmFadeView(rdram, static_cast<int>(GPR_U32(ctx, 4)), 0x80u - GPR_U32(ctx, 5), true);
        g_origRtFade(rdram, ctx, runtime);
    }
    // The fade level is only right after the function updates it: read it on return.
    template <uint32_t Entry, PS2Runtime::RecompiledFunction *Orig> void theGameFadeTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        static uint32_t ra = 0, sp = 0, self = 0;
        if (ctx->pc == Entry)
        {
            ra = GPR_U32(ctx, 31);
            sp = GPR_U32(ctx, 29);
            self = GPR_U32(ctx, 4);
        }
        (*Orig)(rdram, ctx, runtime);
        if (ra && ctx->pc == ra && GPR_U32(ctx, 29) == sp)
        {
            ra = 0;
            wotmTheGameFade(rdram, self);
        }
    }

    // Queue vs text order: rtMain sends the text (fontDmaFontData) and then the gsPkt queue (DmaGsPktQueue), and the fade covers the text;
    // ShellRtLoop::endField (loop of the end-of-stage dialogs, wonLevelOverlay and friends) sends the queue before the text, and the
    // tokens dialog shows up over the screen darkened by TheGame::updateFadeOut. Marked on entry to each one (ScreenRect::underText).
    constexpr uint32_t kDmaGsPktQueue = wotm::fn::DmaGsPktQueue__5GsPkt, kFontDmaFontData = wotm::fn::fontDmaFontData__Fv;
    PS2Runtime::RecompiledFunction g_origDmaGsPktQueue = nullptr, g_origFontDma = nullptr;

    void dmaGsPktQueueTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kDmaGsPktQueue)
            wotmGsPktQueueSent();
        g_origDmaGsPktQueue(rdram, ctx, runtime);
    }

    void fontDmaTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kFontDmaFontData)
        {
            wotmFontDma(rdram);
            wotmFontSent();
        }
        g_origFontDma(rdram, ctx, runtime);
    }

    // Text from the font calls (wotm::FontText): native drawing redoes the glyphs without reading fontPacket.
    constexpr uint32_t kFontInitPacket = wotm::fn::fontInitPacket__FP6QwData10_vramAddrs, kFontSetTexId = wotm::fn::fontSetTexId__FP9_hierhead,
                       kFontSetColorGifTag = wotm::fn::fontSetColorGifTag__Fi, kFontBuildPrim = wotm::fn::fontBuildPrim__FicP6QwData;
    PS2Runtime::RecompiledFunction g_origFontInitPacket = nullptr, g_origFontSetTexId = nullptr, g_origFontColor = nullptr, g_origFontGlyph = nullptr;

    void fontInitPacketTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kFontInitPacket)
            wotmFontInitPacket(GPR_U32(ctx, 5));
        g_origFontInitPacket(rdram, ctx, runtime);
    }
    void fontSetTexIdTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kFontSetTexId)
            wotmFontSetTexId(rdram, GPR_U32(ctx, 4));
        g_origFontSetTexId(rdram, ctx, runtime);
    }
    void fontColorTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kFontSetColorGifTag)
            wotmFontColor(rdram, static_cast<int>(GPR_U32(ctx, 4)));
        g_origFontColor(rdram, ctx, runtime);
    }
    void fontGlyphTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kFontBuildPrim)
            wotmFontGlyph(rdram, static_cast<int>(GPR_U32(ctx, 4)), static_cast<int>(GPR_U32(ctx, 5) & 0xFF));
        g_origFontGlyph(rdram, ctx, runtime);
    }

    void uiFadeScreenTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kUiFadeScreen)   // uiFadeScreen(amount): view 0, alpha 0x80 - amount, directly
            wotmFadeView(rdram, 0, 0x80u - GPR_U32(ctx, 4), false);
        g_origUiFade(rdram, ctx, runtime);
    }

    // 3-MILE white flash: threeMileDrawFlash builds a full-screen SPRITE and puts it on the gsPkt queue (with no hook, native never saw it).
    constexpr uint32_t kThreeMileDrawFlash = wotm::fn::threeMileDrawFlash__Fv;
    PS2Runtime::RecompiledFunction g_origThreeMileFlash = nullptr;
    void threeMileFlashTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kThreeMileDrawFlash)
            wotmThreeMileFlash(rdram);
        g_origThreeMileFlash(rdram, ctx, runtime);
    }

    // Menu black bars: letterboxDmaData(alpha) builds the packet at D_00731850 and sends it directly; read after it returns.
    constexpr uint32_t kLetterboxDmaData = wotm::fn::letterboxDmaData__Ff;
    PS2Runtime::RecompiledFunction g_origLetterbox = nullptr;

    void letterboxTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kLetterboxDmaData)   // letterboxDmaData(alpha in $f12)
            wotmLetterbox(rdram, ctx->f[12]);
        g_origLetterbox(rdram, ctx, runtime);
    }

    void shellFadeScreenTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kShellFadeScreen)
        {
            // a0 = this, a1 = view, a2 = redraw, t1-t3 = r, g, b; on the stack start (+0), end (+8) and step (+0x10)
            const uint32_t sp = GPR_U32(ctx, 29);
            wotmShellFade(rdram, static_cast<int>(GPR_U32(ctx, 5)), GPR_U32(ctx, 9), GPR_U32(ctx, 10), GPR_U32(ctx, 11), READ8(sp), READ8(sp + 8),
                          READ8(sp + 0x10), (GPR_U32(ctx, 6) & 0xFF) != 0);
        }
        g_origShellFade(rdram, ctx, runtime);
    }

    // Stage load: from entry of Shell::LoadLevelFiles (the new level starts overwriting RAM; the LOAD card stays in the
    // hierarchy) until entry of rtMain, the native screen stays on the last frame, with only Shell::FadeScreen on top (wotmLoadFreeze).
    constexpr uint32_t kLoadLevelFiles = wotm::fn::LoadLevelFiles__5Shell, kRtMain = wotm::fn::rtMain__Fb;
    PS2Runtime::RecompiledFunction g_origLoadLevelFiles = nullptr, g_origRtMain = nullptr;

    unsigned g_loadGen = 0;   // stage loads (Shell::LoadLevelFiles) since boot: whatever is kept per loaded file starts over

    void loadLevelFilesTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kLoadLevelFiles)
        {
            wotmLoadFreeze(true);
            ++g_loadGen;
            wotm_sync::levelLoad();
            wotm_modifiers::fightStart();
        }
        g_origLoadLevelFiles(rdram, ctx, runtime);
    }
    // End of stage load: the stage and monsters go in contiguously starting at 0xA00000 (NGP, then RTX and TEX, fileStatus) and nothing
    // checks the ceiling. Above ~0x1F10000 there is live data (and the zip buffer at 0x1F7F840); measured on 2026-10-18, the block at 0x1B70000
    // is left over from the menu and can be overwritten (Metro with 2 monsters goes up to 0x1D0C380). Logs when it changes and warns near the ceiling.
    constexpr uint32_t kFileStatus = wotm::sym::fileStatus, kLoadBase = 0xA00000u, kLoadCeiling = 0x1F10000u;
    // RAM above 32 MB (NATIVO.md, "RAM de 64 MB"): the runtime has 64 MB (PS2_RAM_SIZE) and the game sees 32 (PS2_GUEST_RAM_SIZE: stack, heap and
    // GetMemorySize the same as on the PS2). On entry to Shell::LoadMonstersDB the next load address (fileStatus.ngpAddr[ngpLoaded],
    // right after the stage's NGP) jumps to 0x2000000: the monsters' NGPs and, behind them, the RTX and TEX of the stage and monsters live in the
    // upper area, and only the stage's NGP stays below. Relocation (dbsRelocateViaPtrListFile) adds "address - 0xA00000" in 32 bits, so
    // it works at any address. ngpAddr[1] (the stage's end) becomes 0x2000000; nobody reads that value as a size. WOTM_HIGHRAM=0 turns it off.
    constexpr uint32_t kLoadMonstersDB = wotm::fn::LoadMonstersDB__5Shell, kHighBase = PS2_GUEST_RAM_SIZE;
    PS2Runtime::RecompiledFunction g_origLoadMonstersDB = nullptr;

    void loadMonstersDBHigh(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kLoadMonstersDB)
        {
            const uint32_t ngpN = READ8(kFileStatus + 0x74);
            const uint32_t slot = kFileStatus + 0x3C + 4 * ngpN;
            if (ngpN >= 1 && ngpN <= 13 && READ32(slot) < kHighBase)
                WRITE32(slot, kHighBase);
        }
        g_origLoadMonstersDB(rdram, ctx, runtime);
    }

    uint32_t g_lastLoadEnd = 0;

    void wotmCheckLoadEnd(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t texN = READ8(kFileStatus + 0x38), ngpN = READ8(kFileStatus + 0x74), resN = READ8(kFileStatus + 0xB0);
        if (texN > 13 || ngpN > 13 || resN > 13)
            return;
        const uint32_t end = std::max({READ32(kFileStatus + 4 * texN), READ32(kFileStatus + 0x3C + 4 * ngpN), READ32(kFileStatus + 0x78 + 4 * resN)});
        if (end == g_lastLoadEnd || end < kLoadBase)
            return;
        g_lastLoadEnd = end;
        {
            // Ceilings that don't depend on RAM (item 8: mod skins and files bigger than the disc's): texInfo has 1000 entries and the
            // game only prints "Texture count" when it goes past (it writes beyond the array); overflowing resident VRAM (.RTX) already exits the game
            // with "TRYING TO LOAD TOO MUCH TEXTURE". fileStatus.maxTexId (0x130) is the highest texId of each file, accumulated.
            uint32_t maxTex = 0;
            for (uint32_t i = 0; i < std::max(texN, resN); ++i)
                maxTex = std::max<uint32_t>(maxTex, FAST_READ16(kFileStatus + 0x130 + 2 * i));
            std::fprintf(stderr, "[wotm] stage load: %u of 1000 texIds%s\n", maxTex,
                         maxTex >= 1000 ? " -- PAST texInfo, expect corruption" : maxTex >= 900 ? " -- near the texInfo ceiling" : "");
        }
        if (end > kHighBase)
        {   // upper area: only the stage's NGP stays below (fileStatus.ngpAddr[0] to the end of the file, kept in the load log)
            const double used = (end - kHighBase) / 1048576.0, left = (int64_t(PS2_RAM_SIZE) - end) / 1048576.0;
            std::fprintf(stderr, "[wotm] stage load: %u files, monsters and textures end at 0x%07X (%.1f MB above 32 MB, %.1f MB to the end of RAM)%s\n",
                         ngpN, end, used, left, left < 0.5 ? " -- NEAR OR PAST THE END, expect corruption" : "");
            return;
        }
        const double used = (end - kLoadBase) / 1048576.0, left = (int64_t(kLoadCeiling) - end) / 1048576.0;
        std::fprintf(stderr, "[wotm] stage load: %u files, ends at 0x%07X (%.1f MB used, %.1f MB to the ceiling 0x%07X)%s\n", ngpN, end,
                     used, left, kLoadCeiling, left < 0.5 ? " -- NEAR OR PAST THE CEILING, expect corruption" : "");
    }

    bool g_damageNewFight = true;   // rtMain(first): damage numbers forget the previous fight's monsters (monsterUpdateDamage)

    void rtMainTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kRtMain)
        {
            wotmLoadFreeze(false);
            wotmCheckLoadEnd(rdram, ctx, runtime);
            if (GPR_U32(ctx, 4) && wotm_training::g_menuArmed.load() && !wotm_training::g_on.exchange(true))   // rtMain(first): the fight started
                std::fprintf(stderr, "[training] training mode on (TRAINING fight)\n");
            wotm_training::g_inFight = true;   // also the return from RESET POSITIONS (rtMain(0))
            if (GPR_U32(ctx, 4))
            {
                wotm_training::g_introMs = wotm_training::nowMs();   // this fight's intro can be skipped (wotm_training.inc)
                g_damageNewFight = true;
            }
        }
        g_origRtMain(rdram, ctx, runtime);
    }

    // ScreenPolys HUD (Hud::drawHealthIcon's health icon): one quad per polyBuildPrim(src, x, y, pd, pkt), rebuilt by
    // native code from the arguments (wotm::screenPolyFromCall), without reading s_polyBuffer.
    constexpr uint32_t kPolyBuildPrim = wotm::fn::polyBuildPrim__FiiiRQ211ScreenPolys8PolyDataP6QwData;
    PS2Runtime::RecompiledFunction g_origPolyBuild = nullptr;

    void polyBuildPrimTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kPolyBuildPrim)
            wotmScreenPoly(rdram, static_cast<int>(GPR_U32(ctx, 4)), static_cast<int>(GPR_U32(ctx, 5)), static_cast<int>(GPR_U32(ctx, 6)), GPR_U32(ctx, 7));
        g_origPolyBuild(rdram, ctx, runtime);
    }

    // Exit of hier(0, 0): native drawing copies the scene here (wotmCaptureScene), with the state the traversal just sent to the
    // VU1; rtMain's gameUpdate and animationRunGlobal, right after, already write the next frame. In split screen view 1 is captured
    // the same way on exit from hier(1, 0) (fallback: entry of TheGame::Update2, which rtMain calls right after, only for view 1).
    constexpr uint32_t kHier = wotm::fn::hier__Fii;
    PS2Runtime::RecompiledFunction g_origHier = nullptr;

    // The function can return midway (thread switch) and be resumed later without coming through here: only capture when the pc reaches
    // the call's $ra. When the return escapes (60% of fight frames), the capture happens on entry to TheGame::Update, which rtMain calls
    // right after (only RtLoopView::end in between). Without that fallback the scene came from the end of the frame, one frame ahead of the lens flare.
    uint32_t g_hierSceneRa = 0;   // $ra of the hier(v, 0) in progress (0 = none)
    int g_hierSceneView = 0;      // its view
    void hierSceneExit(uint8_t *rdram)
    {
        g_hierSceneRa = 0;
        wotmCaptureScene(rdram, g_hierSceneView);
        if (g_hierSceneView == 0)
            wotmVu1SceneExit(rdram);
    }
    // Per-view cost for [field] (WOTM_FIELDLOG): host time inside hier(v, pass) per view, over both passes, and of the native
    // capture (wotmCaptureScene) on exit from hier(v, 0). Only counts calls that return through the wrapper (one that blocks and is
    // resumed midway doesn't come through here).
    // Views 2 and 3 (3 and 4 players, wotm_split4.inc): rtMain doesn't run an update between their hier(v, 0) and hier(v, 1), so the
    // capture fallback is the entry of hier(v, 1).
    void hierTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool entry = ctx->pc == kHier;
        const uint32_t view = GPR_U32(ctx, 4), ra = GPR_U32(ctx, 31);
        if (entry && GPR_U32(ctx, 5) != 0u && g_hierSceneRa && g_hierSceneView >= 2)
            hierSceneExit(rdram);
        if (entry && view < 4u && GPR_U32(ctx, 5) == 0u)
        {
            if (g_hierSceneRa)   // the previous one's exit escaped with no fallback: capture before starting this one
                hierSceneExit(rdram);
            g_hierSceneRa = ra;
            g_hierSceneView = static_cast<int>(view);
            g_hierViews |= 1u << view;
        }
        const auto t0 = std::chrono::steady_clock::now();
        g_origHier(rdram, ctx, runtime);
        const auto t1 = std::chrono::steady_clock::now();
        if (entry && view < 4u && ctx->pc == ra)
            g_hierMs[view] += std::chrono::duration<double, std::milli>(t1 - t0).count();
        if (g_hierSceneRa && ctx->pc == g_hierSceneRa)
        {
            const int v = g_hierSceneView;
            hierSceneExit(rdram);
            if (v >= 0 && v < 4)
                g_captureMs[v] += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
        }
    }

    // Lens flare: the list is ready (projected and tested) on exit from particleLensFlareRender. main.cpp copies it after
    // particleDraw, but on the frame armed by the check (F8, WOTM_VU1CHECK) the DMA really runs, particleDraw waits and returns
    // midway, and that copy picked up the list still in world coordinates (the snapshot frame came out with no flare). This one comes later and wins.
    constexpr uint32_t kLensFlareRender = wotm::fn::particleLensFlareRender__FP6QwDataiPPUi;
    PS2Runtime::RecompiledFunction g_origLensFlareRender = nullptr;
    void lensFlareRenderTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = ctx->pc == kLensFlareRender ? GPR_U32(ctx, 31) : 0u;
        g_origLensFlareRender(rdram, ctx, runtime);
        if (ra && ctx->pc == ra)
            wotmFlareCapture(rdram);
    }

    constexpr uint32_t kTheGameUpdate = wotm::fn::Update__7TheGame;
    PS2Runtime::RecompiledFunction g_origTheGameUpdate = nullptr;
    // WOTM_LOGICDELAY=<ms>: host delay of 0 to <ms> (random, fixed sequence) on entry to TheGame::Update, to test the
    // clock and vsync with 1- and 2-field frames (FRONTEIRA.md, step 4). Diagnostic; never on by default.
    int g_logicDelayMs = 0;
    uint32_t g_logicDelaySeed = 12345u;
    void theGameUpdateTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kTheGameUpdate && g_hierSceneRa)
            hierSceneExit(rdram);
        if (ctx->pc == kTheGameUpdate && g_logicDelayMs > 0)
        {
            g_logicDelaySeed = g_logicDelaySeed * 1103515245u + 12345u;
            const uint32_t us = (g_logicDelaySeed >> 8) % (static_cast<uint32_t>(g_logicDelayMs) * 1000u + 1u);
            const auto until = std::chrono::steady_clock::now() + std::chrono::microseconds(us);
            while (std::chrono::steady_clock::now() < until)   // busy wait: Windows sleep rounds to ~1 ms or more
                ;
        }
        g_origTheGameUpdate(rdram, ctx, runtime);
    }
    constexpr uint32_t kTheGameUpdate2 = wotm::fn::Update2__7TheGame;
    PS2Runtime::RecompiledFunction g_origTheGameUpdate2 = nullptr;
    void theGameUpdate2Tapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kTheGameUpdate2 && g_hierSceneRa)
            hierSceneExit(rdram);
        g_origTheGameUpdate2(rdram, ctx, runtime);
    }

    // Green tint on a pick-up the monster can grab: enableHighlights writes it into the _cs's colorQuad before hier(v,0), and
    // computeHighlight (Monster::update, after hier) sets it back to 1 before the capture. On exit, wotmPickupHighlights records the value
    // (Scene::csColorAtDraw).
    constexpr uint32_t kEnableHighlights = wotm::fn::enableHighlights__12LevelPickupsi;
    PS2Runtime::RecompiledFunction g_origEnableHighlights = nullptr;

    void enableHighlightsTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool entry = ctx->pc == kEnableHighlights;
        g_origEnableHighlights(rdram, ctx, runtime);
        if (entry)
            wotmPickupHighlights(rdram);
    }

    // Same behavior as the original (`jr $ra; lw $v0, gp_rel(D_006F8DB8)`), except for the floor above.
    void timerGetUpdateRate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        int32_t rate = static_cast<int32_t>(READ32(kUpdateRateVar));
        if (rate > 0 && rate < g_minRate)
        {
            rate = g_minRate;
            if ((++g_clamped % 20000ull) == 0 && std::getenv("PS2X_STATS"))
                std::fprintf(stderr, "[wotm] updateRate clamped to %d in %llu calls\n", g_minRate, g_clamped);
        }
        SET_GPR_S32(ctx, 2, rate);
        ctx->pc = GPR_U32(ctx, 31);
    }
}


namespace
{
    // Particle emitter that "teleports" (since 2026-10-21; found with 8 monsters). An emitter with EmitMotionSense (0x1000000; the fire
    // on Agamo's head, fxId 141 IdolHeadFlame) adds to cumulativeRate the distance it moved since the previous frame:
    // |ParticleToEmitVec - prevParticleToEmitVec| * 4 / maxSize. On one frame the position came from very far away (Agamo with 7 other
    // monster types in Center and Airport), the accumulator went to ~5e18 and stayed there (n = cvt.w.s saturates at 0x7FFFFFFF and the rest never goes down);
    // with live particles the numParticles + n in the maxParticles cap goes past 2^31, turns negative, the cap doesn't catch it and the emit loop
    // runs 2^31 times: the game hangs on frame 3. On the PS2 there was no way to have that many monsters. Fix: on exit from
    // utilUpdateEmitterMatrices(e, wind), a jump of more than kJump units (a stage is ~3000) counts as standing still (previous = current); and,
    // on entry to particleEmit, an accumulator outside [0, 2^16) goes back to 0 and numParticles outside [0, maxParticles] goes back to the limit.
    // Outside those cases nothing changes. One [wotm] line per fix (up to 8).
    constexpr uint32_t kEmitMatrices = wotm::fn::utilUpdateEmitterMatrices__FP11EmitterTypeP8_fvector,
                       kParticleEmit = wotm::fn::particleEmit__FP13_ParticleTypeP8_fvectorT1;
    constexpr uint32_t kEmitVec = 0x0, kEmitPrevVec = 0x50, kEmitCumRate = 0x138, kEmitFxId = 0x13C;   // EmitterType
    constexpr uint32_t kPtEmit = 0x2CA0, kPtNum = 0x2ED0, kPtMax = 0x2ED4;                            // ParticleType
    constexpr float kJump = 100000.f;
    PS2Runtime::RecompiledFunction g_origEmitMatrices = nullptr, g_origParticleEmit = nullptr;
    int g_emitFixes = 0;

    float emitF(uint8_t *rdram, uint32_t a)
    {
        float v;
        std::memcpy(&v, rdram + a, 4);
        return v;
    }

    void emitMatricesGuarded(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t e = GPR_U32(ctx, 4) & 0x1FFFFFFFu, ra = ctx->pc == kEmitMatrices ? GPR_U32(ctx, 31) : 0u;
        g_origEmitMatrices(rdram, ctx, runtime);
        if (!ra || ctx->pc != ra || e < 0x1000u || e + 0x140u > PS2_RAM_SIZE)
            return;
        float d2 = 0.f;
        for (uint32_t k = 0; k < 3; ++k)
        {
            const float d = emitF(rdram, e + kEmitVec + 4 * k) - emitF(rdram, e + kEmitPrevVec + 4 * k);
            d2 += d * d;
        }
        if (d2 > kJump * kJump || !std::isfinite(d2))
        {
            std::memcpy(rdram + e + kEmitPrevVec, rdram + e + kEmitVec, 16);
            if (g_emitFixes < 8 && ++g_emitFixes)
                std::fprintf(stderr, "[wotm] particle emitter fxId %u jumped %.3g units: counted as standing still\n", FAST_READ32(e + kEmitFxId),
                             std::sqrt(d2));
        }
    }

    void particleEmitGuarded(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t p = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
        if (ctx->pc == kParticleEmit && p >= 0x1000u && p + kPtMax + 4 <= PS2_RAM_SIZE)
        {
            const float cum = emitF(rdram, p + kPtEmit + kEmitCumRate);
            const int32_t num = static_cast<int32_t>(FAST_READ32(p + kPtNum)), mx = static_cast<int32_t>(FAST_READ32(p + kPtMax));
            const bool badCum = !(cum >= 0.f && cum < 65536.f), badNum = num < 0 || (mx >= 0 && num > mx);
            if (badCum)
            {
                const float zero = 0.f;
                std::memcpy(rdram + p + kPtEmit + kEmitCumRate, &zero, 4);
            }
            if (badNum)
                FAST_WRITE32(p + kPtNum, static_cast<uint32_t>(num < 0 ? 0 : mx));
            if ((badCum || badNum) && g_emitFixes < 8 && ++g_emitFixes)
                std::fprintf(stderr, "[wotm] particleEmit fxId %u: accumulator %g and %d/%d particles corrected\n", FAST_READ32(p + kPtEmit + kEmitFxId), cum,
                             num, mx);
        }
        g_origParticleEmit(rdram, ctx, runtime);
    }

    // Kineticlops chain lightning that hangs with more than 4 monsters (since 2026-10-21; Metro with 8). In StateZapAttack::update, the
    // loop that passes the lightning from the target to the other monsters (i from 0 to numMonsters) and the loop that draws each one's arcs (0 to
    // numArcs, float @0x50 of the state) use the same variable ($s4) in retail: after chaining to monster k, i ends up at numArcs + 1.
    // With 4 monsters that only shortens the loop (i goes past the end; the lightning hits one extra monster per frame). With 8 and k >= numArcs + 1,
    // i goes backwards, the loop chains to the same k again and never ends. Fix: save i at the loop's hdLosCSTest call
    // (start of each monster's test) and, at the first call after the arcs loop (mathfRand), set i = max(i, saved). Where
    // retail doesn't hang (i doesn't go backwards) the result is the same. One [wotm] line per fix (up to 8).
    constexpr uint32_t kZapUpdate = wotm::fn::update__14StateZapAttack, kLosCs = wotm::fn::hdLosCSTest__FP8_fvectorT0P9_hdResulti,
                       kMathfRand = wotm::fn::mathfRand__Fii;
    constexpr uint32_t kZapRaLos = kZapUpdate + 0xB60, kZapRaAfterArcs = kZapUpdate + 0xC4C;   // return addresses of the two calls in the loop
    constexpr int kRegS4 = 20;
    PS2Runtime::RecompiledFunction g_origLosCs = nullptr, g_origMathfRand = nullptr;
    int32_t g_zapIndex = -1;
    int g_zapFixes = 0;

    void losCsTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kLosCs && GPR_U32(ctx, 31) == kZapRaLos)
            g_zapIndex = static_cast<int32_t>(GPR_U32(ctx, kRegS4));
        g_origLosCs(rdram, ctx, runtime);
    }

    void mathfRandTapped(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kMathfRand && GPR_U32(ctx, 31) == kZapRaAfterArcs && g_zapIndex >= 0)
        {
            const int32_t i = static_cast<int32_t>(GPR_U32(ctx, kRegS4));
            if (i < g_zapIndex)
            {
                SET_GPR_U32(ctx, kRegS4, static_cast<uint32_t>(g_zapIndex));
                if (g_zapFixes < 8 && ++g_zapFixes)
                    std::fprintf(stderr, "[wotm] chain lightning (StateZapAttack): the arcs loop moved the monster index from %d to %d; kept at %d\n",
                                 g_zapIndex, i, g_zapIndex);
            }
            g_zapIndex = -1;
        }
        g_origMathfRand(rdram, ctx, runtime);
    }
}
#include "wotm_orbit.inc"   // modern (orbit) camera: hooks called by the free camera and lock-on ones, below
namespace
{
    // Free camera (F5, wotm_freecam.h). View 0's _cs (viewInfo[0], the same one the game camera moves) gets the pose at two
    // points: after Cameras::Update (the rest of the frame's logic, like the stage scripts, sound and AI, already sees the new camera) and on
    // entry to viewUpdate(0), which builds worldToScreenMat and worldCtx[0] from cs->trans and cs->mat (it wins over anyone who touches the
    // _cs between the two, like the fight intro camera). The game camera keeps running underneath: when turned off, it leaves the
    // free pose and returns to the monster by itself. While it's on (and without Tab), scePadRead returns an idle pad.
    constexpr uint32_t kViewUpdate = wotm::fn::viewUpdate__Fi, kScePadRead = wotm::fn::scePadRead;
    constexpr uint32_t kViewInfo = wotm::sym::viewInfo;   // 5 x 0x14: [0] = the view's _cs*
    PS2Runtime::RecompiledFunction g_origViewUpdate = nullptr, g_origCamerasUpdateFree = nullptr, g_origPadRead = nullptr;

    void putF(uint8_t *rdram, uint32_t a, float v) { std::memcpy(rdram + a, &v, 4); }

    void freeCamWrite(uint8_t *rdram)
    {
        if (!g_wotmFreeCam.load(std::memory_order_relaxed))
            return;
        uint32_t cs = 0;
        std::memcpy(&cs, rdram + kViewInfo, 4);
        cs &= 0x1FFFFFFFu;
        if (cs < 0x1000u || cs + 0x70u > PS2_RAM_SIZE)
            return;
        WotmFreeCamPose p;
        {
            std::lock_guard<std::mutex> lk(g_wotmFreeCamM);
            p = g_wotmFreeCamPose;
        }
        float ax[3][3];
        wotmFreeCamAxes(p, ax[0], ax[1], ax[2]);
        for (int i = 0; i < 3; ++i)
        {
            putF(rdram, cs + 0x10 + 4 * i, p.pos[i]);   // trans
            for (int r = 0; r < 3; ++r)
                putF(rdram, cs + 0x20 + 0x10 * r + 4 * i, ax[r][i]);   // mat: right, forward, up
        }
        for (int r = 0; r < 3; ++r)
            putF(rdram, cs + 0x20 + 0x10 * r + 0xC, 0.f);
        putF(rdram, cs + 0x60, p.pitch);   // rot (RPH): the game camera starts from here when turned off
        putF(rdram, cs + 0x64, 0.f);
        putF(rdram, cs + 0x68, p.heading);
    }

    void camerasUpdateFree(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = GPR_U32(ctx, 31);
        orbit::pre(rdram);   // modern camera (wotm_orbit.inc)
        g_origCamerasUpdateFree(rdram, ctx, runtime);
        if (ctx->pc == ra)
            orbit::post(rdram, ctx, runtime);
        freeCamWrite(rdram);
    }

    void viewUpdateFree(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (GPR_U32(ctx, 4) == 0u)
            freeCamWrite(rdram);
        g_origViewUpdate(rdram, ctx, runtime);
    }

    void padReadFree(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t data = GPR_U32(ctx, 6) & 0x1FFFFFFFu;   // scePadRead(port, slot, rdata)
        g_origPadRead(rdram, ctx, runtime);
        if (!g_wotmFreeCam.load(std::memory_order_relaxed) || g_wotmFreeCamPad.load(std::memory_order_relaxed))
            return;
        if (GPR_U32(ctx, 2) == 0u || data < 0x1000u || data + 32u > PS2_RAM_SIZE)
            return;
        uint8_t *d = rdram + data;
        d[2] = d[3] = 0xFF;                // buttons (active low)
        std::memset(d + 4, 0x80, 4);       // sticks centered
        std::memset(d + 8, 0, 12);         // button pressure
    }
}

namespace
{
    // Toggle lock-on (mod, WOTM_LOCKTOGGLE=1; launcher's "QoL features" group). The frame's lock-on (PadEntry +0x32) is decided in
    // updateInput{0,1,2}Config{CR,SR} from the held-frame counters that the same config's updateHistory{0,1,2}
    // just updated (PadFlags +0x1658 and +0x165E: ++ held, 0 released). Config 0 locks with both
    // above PadFlags +0x16C4 (just one only strafes, PadEntry +0x1A/+0x1C); configs 1 and 2, with +0x1658 nonzero. The config
    // itself reads +0x32 right afterwards to pick the stick and d-pad mapping; that's why rewriting only
    // +0x32 after interpretInputs left the character locked but with released-button movement. Here, on exit from updateHistory, a
    // new press of lock-on toggles a state per human player (owner in PadFlags +0x1810), and the counters are set to
    // read as "holding" (on) or "not holding" (off): the rest of the config, including the lock-on combos,
    // sees exactly what it would with the button held or released. The real counters are saved and restored before the next
    // updateHistory, which counts from them. A dead locked target turns the state off (while held, updateLock would jump to the
    // nearest monster). The AI is unchanged.
    //
    // Mod buttons in config 0 (the default; PadFlags +0x16EC): the GamePad that loadPadInputs builds has one byte per button
    // (0 L1, 1 L2, 3 R1, 4 R2, 0xC/0xD left/right on the d-pad or stick). In config 0 the game uses L1+R1 for
    // lock-on, L1 or R1 alone to strafe (the byte's value goes to PadEntry +0x1A/+0x1C), R2 for block (and
    // counter) and L2 for the camera. On entry to interpretInputs the bytes are swapped: physical L2 -> byte 0 and R2 -> byte 3
    // (strafe), L1 -> byte 4 (block), the game's L2 stays released; physical R1 only toggles lock-on (read here, not by the game).
    // With lock-on on, d-pad left/right (read raw in scePadRead, port = player) switch the target
    // (m_pinTarget: updateLock keeps the target while it stays targetable) to the next living monster to the left or
    // right on screen, and don't move the character; L2/R2 come in as left/right, which while locked the game already turns into
    // strafing (PadEntry +0x16/+0x18 -> +0x1A/+0x1C, only without the game's L2/R2, byte 4 = block). The original bytes come back on exit. Configs 1 and 2 keep the game's buttons.
    constexpr uint32_t kUpdateHistory[3] = {wotm::fn::updateHistory0__8PadFlagsR7GamePad, wotm::fn::updateHistory1__8PadFlagsR7GamePad,
                                            wotm::fn::updateHistory2__8PadFlagsR7GamePad};
    constexpr uint32_t kInterpretInputs = wotm::fn::interpretInputs__8PadFlagsR7GamePad;
    constexpr uint32_t kPadFlagsLockA = 0x1658u, kPadFlagsLockB = 0x165Eu, kPadFlagsHoldTweak = 0x16C4u, kPadFlagsConfig = 0x16ECu,
                       kPadFlagsOwner = 0x1810u, kPadFlagsScreenRel = 0x16E4u;
    // m_monsters (TheGame +0x120380): 4 player slots and the AIs from [4] on; with fights of up to 8, AI 7 is at [10], and
    // TheGame has room for 16. Walking only 8 left AIs 5 to 7 out of lock-on.
    constexpr uint32_t kMonsterSlots = 16u;
    // m_playerNum (+0x18) 0 = slot allocated but out of the fight: in Endurance the queued AIs sit in m_monsters from the start,
    // idle and "alive", and only the one that came in has m_playerNum; the game's getClosestMonster skips the 0 ones (lock-on used to lock onto them).
    constexpr uint32_t kMonsterCs = 0xCu, kMonsterPlayerNum = 0x18u, kMonsterPinTarget = 0x6C04u, kMonsterDead = 0xE8u,
                       kMonsterDying = 0xEFu, kMonsterCameraView = 0x6CD8u;
    constexpr uint32_t kPadL1 = 0x0u, kPadL2 = 0x1u, kPadR1 = 0x3u, kPadR2 = 0x4u, kPadLeft = 0xCu, kPadRight = 0xDu, kPadUp = 0xEu,
                       kPadDown = 0xFu;
    PS2Runtime::RecompiledFunction g_origUpdateHistory[3] = {}, g_origInterpretInputs = nullptr, g_origPadReadLock = nullptr;
    struct LockToggle
    {
        uint32_t pf = 0;         // the player's PadFlags (0 = free)
        uint32_t owner = 0;      // owning monster (a different owner on the same PadFlags resets the state)
        bool held = false;       // lock-on physically held on the previous frame (edge)
        bool on = false;         // toggled state
        bool forged = false;     // counters rewritten (realA/realB are restored before the next updateHistory)
        bool remap = false;      // mod buttons this frame (config 0): lock-on is physical R1 (lockHeld)
        bool lockHeld = false;
        bool dirHeld[2] = {};    // d-pad left/right on the previous frame (edge for target switching)
        bool screenRel = false;  // PadFlags +0x16E4 this frame: updateInput0ConfigSR (the game already makes the stick camera-relative)
        uint16_t realA = 0, realB = 0, fakeA = 0, fakeB = 0;
    } g_lockToggle[4];
    uint8_t g_padDir[4] = {};    // per pad (port + 2 * slot, the player's): bit 0 left, bit 1 right on the d-pad (last scePadRead)

    bool ramOk(uint32_t a, uint32_t n) { return a >= 0x1000u && a + n <= PS2_RAM_SIZE; }
    uint16_t rd16(uint8_t *rdram, uint32_t a) { uint16_t v; std::memcpy(&v, rdram + a, 2); return v; }
    void wr16(uint8_t *rdram, uint32_t a, uint16_t v) { std::memcpy(rdram + a, &v, 2); }

    LockToggle *lockToggleFind(uint32_t pf, bool create)
    {
        for (LockToggle &e : g_lockToggle)
            if (e.pf == pf)
                return &e;
        if (create)
            for (LockToggle &e : g_lockToggle)
                if (!e.pf)
                {
                    e = LockToggle{};
                    e.pf = pf;
                    return &e;
                }
        return nullptr;
    }

    // Is the locked target (m_pinTarget, Monster +0x6C04, from the previous frame) one of TheGame's m_monsters with m_dead (+0xE8) or that
    // left the fight (m_playerNum 0)?
    bool lockTargetDead(uint8_t *rdram, uint32_t owner)
    {
        const uint32_t target = FAST_READ32(owner + kMonsterPinTarget);
        if (!target)
            return false;
        for (uint32_t i = 0; i < kMonsterSlots; ++i)
            if (FAST_READ32(kGame + 0x120380 + 4 * i) == target)
                return ramOk(target, kMonsterDead + 1u) &&
                       (static_cast<int8_t>(rdram[target + kMonsterDead]) != 0 || FAST_READ32(target + kMonsterPlayerNum) == 0u);
        return false;   // building, boss part, etc.
    }

    // State of the human player who owns this PadFlags (nullptr for the AI)
    LockToggle *lockToggleFor(uint8_t *rdram, uint32_t pf, int *slot)
    {
        const uint32_t owner = FAST_READ32(pf + kPadFlagsOwner);
        if (!isPlayerMonster(rdram, owner, slot))
            return nullptr;   // AI (its m_playerNum isn't 0 either)
        LockToggle *t = lockToggleFind(pf, true);
        if (t && t->owner != owner)
        {
            *t = LockToggle{};
            t->pf = pf;
            t->owner = owner;
        }
        return t;
    }

    float rdF(uint8_t *rdram, uint32_t a) { float v; std::memcpy(&v, rdram + a, 4); return v; }

    // Position (cs->trans) of a monster or another targetable (the _cs is at +0xC in both)
    bool targetPos(uint8_t *rdram, uint32_t obj, float p[3])
    {
        if (!ramOk(obj, 0x10u))
            return false;
        const uint32_t cs = FAST_READ32(obj + kMonsterCs) & 0x1FFFFFFFu;
        if (!ramOk(cs, 0x20u))
            return false;
        for (int i = 0; i < 3; ++i)
            p[i] = rdF(rdram, cs + 0x10 + 4 * i);
        return true;
    }

    // m_monsters[i] works as a target for the owner: alive and targetable as in getClosestTargetable(6); position in tp
    bool lockCandidate(uint8_t *rdram, uint32_t m, uint32_t owner, float tp[3])
    {
        if (!m || m == owner || !ramOk(m, kMonsterDying + 1u) || rdram[m + kMonsterDead] || rdram[m + kMonsterDying] ||
            FAST_READ32(m + kMonsterPlayerNum) == 0u)
            return false;
        return (rd16(rdram, m + 4) & 6u) == 6u && targetPos(rdram, m, tp);
    }

    // On lock: the target is the nearest living monster (3D distance). updateLock's getClosestTargetable also weighs the direction
    // the monster is facing; with m_pinTarget already filled in and targetable, updateLock keeps it.
    void lockNearestTarget(uint8_t *rdram, uint32_t owner)
    {
        float me[3], tp[3];
        if (!targetPos(rdram, owner, me))
            return;
        uint32_t best = 0;
        float bestD = 0.f;
        for (uint32_t i = 0; i < kMonsterSlots; ++i)
        {
            const uint32_t m = FAST_READ32(kGame + 0x120380 + 4 * i);   // m_monsters[i]
            if (!lockCandidate(rdram, m, owner, tp))
                continue;
            const float d = (tp[0] - me[0]) * (tp[0] - me[0]) + (tp[1] - me[1]) * (tp[1] - me[1]) + (tp[2] - me[2]) * (tp[2] - me[2]);            if (!best || d < bestD)
            {
                best = m;
                bestD = d;
            }
        }
        FAST_WRITE32(owner + kMonsterPinTarget, best);   // no candidate, 0: updateLock picks (previously the old target stayed)
    }

    // Switches the locked target to the nearest living monster to the left (dir < 0) or right (dir > 0) of the current one on screen: horizontal
    // angle seen from the player's camera (the view _cs's right and forward axes), wrapping around at the end.
    // Right and forward axes (in the XY plane) of the player's camera: the _cs of their view (viewInfo[view], mat at +0x20/+0x30)
    bool lockCameraAxes(uint8_t *rdram, uint32_t owner, float right[2], float fwd[2])
    {
        int32_t players = static_cast<int32_t>(FAST_READ32(kShell + 0x2BB4));   // m_numPlayers
        const int32_t view = players > 1 ? std::clamp<int32_t>(static_cast<int32_t>(FAST_READ32(owner + kMonsterCameraView)), 0, 4) : 0;
        const uint32_t cam = FAST_READ32(kViewInfo + 0x14u * view) & 0x1FFFFFFFu;   // the view's _cs
        if (!ramOk(cam, 0x40u))
            return false;
        for (int i = 0; i < 2; ++i)
        {
            right[i] = rdF(rdram, cam + 0x20 + 4 * i);
            fwd[i] = rdF(rdram, cam + 0x30 + 4 * i);
        }
        // looking almost straight down, forward on the ground vanishes: the camera's "up" (+0x40) points where the screen goes up
        if (fwd[0] * fwd[0] + fwd[1] * fwd[1] < 0.09f)
            for (int i = 0; i < 2; ++i)
                fwd[i] = rdF(rdram, cam + 0x40 + 4 * i);
        return std::isfinite(right[0] + right[1] + fwd[0] + fwd[1]);
    }

    void lockCycleTarget(uint8_t *rdram, uint32_t owner, int slot, int dir)
    {
        const uint32_t cur = FAST_READ32(owner + kMonsterPinTarget);
        float me[3], tp[3], right[2], fwd[2];
        if (!cur || !targetPos(rdram, owner, me) || !lockCameraAxes(rdram, owner, right, fwd))
            return;
        auto screenAngle = [&](const float p[3]) {
            const float dx = p[0] - me[0], dy = p[1] - me[1];
            return wotm::detmath::atan2f(dx * right[0] + dy * right[1], dx * fwd[0] + dy * fwd[1]);   // picks game state: same bits everywhere
        };
        const float curA = targetPos(rdram, cur, tp) ? screenAngle(tp) : 0.f;
        uint32_t best = 0, wrap = 0;
        float bestD = 1e9f, wrapA = 0.f;
        for (uint32_t i = 0; i < kMonsterSlots; ++i)
        {
            const uint32_t m = FAST_READ32(kGame + 0x120380 + 4 * i);   // m_monsters[i]
            if (m == cur || !lockCandidate(rdram, m, owner, tp))
                continue;
            const float a = screenAngle(tp), d = (a - curA) * static_cast<float>(dir);
            if (d > 0.f && d < bestD)
            {
                bestD = d;
                best = m;
            }
            if (!wrap || a * -static_cast<float>(dir) > wrapA * -static_cast<float>(dir))
            {
                wrap = m;   // the farthest on the opposite side: the wraparound
                wrapA = a;
            }
        }
        if (!best)
            best = wrap;
        if (!best)
            return;
        FAST_WRITE32(owner + kMonsterPinTarget, best);
        std::fprintf(stderr, "[lockon] player %d: target to the %s (0x%x)\n", slot + 1, dir < 0 ? "left" : "right", best);
    }

    void lockCameraStick(uint8_t *rdram, uint32_t owner, uint8_t *b);

    void interpretInputsLockToggle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pf = GPR_U32(ctx, 4) & 0x1FFFFFFFu, gp = GPR_U32(ctx, 5) & 0x1FFFFFFFu;
        // modern camera (wotm_orbit.inc): screen-relative config and right stick for orbiting, before lock-on reads +0x16E4
        const int orbitSlot = ctx->pc == kInterpretInputs ? orbit::inputEnter(rdram, pf, gp) : -1;
        // target switch requested from the keyboard (Tab, R, mouse wheel; wotm_mouse.hpp): with a locked target, with or without toggle lock-on. Not in a
        // network match: the request comes from this PC's window, outside the input exchanged over the network (switching stays on the pad's d-pad)
        if (int ps = -1; ctx->pc == kInterpretInputs && !wotm::settings::g_netProfile.load(std::memory_order_relaxed) && ramOk(pf, kPadFlagsOwner + 4u) &&
                         isPlayerMonster(rdram, FAST_READ32(pf + kPadFlagsOwner), &ps) && ps >= 0 && ps < 4)
            if (const int req = wotm::mouse::g_targetReq[ps].exchange(0, std::memory_order_relaxed); req != 0)
            {
                const uint32_t owner = FAST_READ32(pf + kPadFlagsOwner);
                if (FAST_READ32(owner + kMonsterPinTarget))
                    lockCycleTarget(rdram, owner, ps, req > 0 ? 1 : -1);   // next = the one to the right on screen
            }
        LockToggle *t = nullptr;
        int slot = 0;
        if (ctx->pc == kInterpretInputs && wotm::settings::on(wotm::settings::LockToggle) && ramOk(pf, kPadFlagsOwner + 4u) && ramOk(gp, 0x18u))
            t = lockToggleFor(rdram, pf, &slot);
        if (t)
        {
            t->remap = FAST_READ32(pf + kPadFlagsConfig) == 0u;
            t->screenRel = FAST_READ32(pf + kPadFlagsScreenRel) != 0u;
        }
        if (!t || !t->remap)
        {
            const uint32_t ra0 = GPR_U32(ctx, 31);
            g_origInterpretInputs(rdram, ctx, runtime);
            if (ctx->pc == ra0)
                orbit::inputLeave(rdram, orbitSlot);
            return;
        }
        uint8_t *b = rdram + gp, saved[0x10];
        std::memcpy(saved, b, sizeof saved);
        t->lockHeld = saved[kPadR1] != 0;
        b[kPadL1] = saved[kPadL2];                // strafe: L2 and R2
        b[kPadR1] = saved[kPadR2];
        b[kPadR2] = saved[kPadL1] ? 0xFF : 0;     // block: L1
        b[kPadL2] = 0;
        const uint8_t dirs = slot < 4 ? g_padDir[slot] : 0;
        for (int i = 0; i < 2; ++i)
        {
            const bool held = (dirs >> i) & 1u;
            if (t->on && held)
            {
                b[kPadLeft] = b[kPadRight] = 0;   // while locked, the arrows switch target and don't move
                if (!t->dirHeld[i])
                    lockCycleTarget(rdram, t->owner, slot, i ? 1 : -1);
            }
            t->dirHeld[i] = held;
        }
        if (t->on)
        {
            if (!t->screenRel)
                lockCameraStick(rdram, t->owner, b);
            // while locked, the game strafes with left/right (PadEntry +0x16/+0x18 -> +0x1A/+0x1C) and ignores bytes 0/3
            if (saved[kPadL2])
                b[kPadLeft] = 0xFF;
            if (saved[kPadR2])
                b[kPadRight] = 0xFF;
        }
        const uint32_t ra = GPR_U32(ctx, 31);
        g_origInterpretInputs(rdram, ctx, runtime);
        if (ctx->pc == ra)   // the time slice ran out midway: the rest still reads the swapped bytes (loadPadInputs rebuilds them all)
        {
            std::memcpy(b, saved, sizeof saved);
            orbit::inputLeave(rdram, orbitSlot);
        }
    }

    // Only in the monster-relative config (updateInput0ConfigCR, PadFlags +0x16E4 = 0). In the screen-relative one (ConfigSR, +0x16E4 != 0;
    // only in the unified view and in Crush: in the normal view a human stays on CR, 75f5be1:docs/investigacoes/2026-10-22/camera_controle_cr_sr.md) the game itself, while locked, already projects the
    // camera stick (PadFlags +0x17E0) onto the target direction and its perpendicular; converting here first did the conversion twice, and the error
    // was the angle between the camera and the target direction (up could go backwards or sideways with the camera off the monster's back).
    // While locked, the game moves the monster in its own frame of reference, facing the target (up/down = toward the target and back, left/right =
    // sideways). To make the stick (and d-pad) camera-relative, as when unlocked: the direction it points on screen becomes a vector
    // on the ground via the camera axes and is rewritten into the GamePad's up/down/left/right bytes in the monster's frame (forward =
    // toward the target; right with the same handedness as the camera's right relative to its forward). The magnitude is preserved.
    void lockCameraStick(uint8_t *rdram, uint32_t owner, uint8_t *b)
    {
        const float sx = (float(b[kPadRight]) - float(b[kPadLeft])) / 255.f, sy = (float(b[kPadUp]) - float(b[kPadDown])) / 255.f;
        if (std::fabs(sx) < 0.01f && std::fabs(sy) < 0.01f)
            return;
        const uint32_t target = FAST_READ32(owner + kMonsterPinTarget);
        float me[3], tp[3], cr[2], cf[2];
        if (!target || !targetPos(rdram, owner, me) || !targetPos(rdram, target, tp) || !lockCameraAxes(rdram, owner, cr, cf))
            return;
        auto norm = [](float v[2]) {
            const float l = std::sqrt(v[0] * v[0] + v[1] * v[1]);
            if (l < 1e-4f) return false;
            v[0] /= l;
            v[1] /= l;
            return true;
        };
        float mf[2] = {tp[0] - me[0], tp[1] - me[1]};
        if (!norm(cr) || !norm(cf) || !norm(mf))
            return;
        // perp(v) = (v.y, -v.x); the camera's right is +perp(forward) or -perp(forward) depending on the world's handedness
        const float hand = cr[0] * cf[1] - cr[1] * cf[0] >= 0.f ? 1.f : -1.f;
        const float mr[2] = {hand * mf[1], -hand * mf[0]};
        const float w[2] = {sx * cr[0] + sy * cf[0], sx * cr[1] + sy * cf[1]};
        const float lx = std::clamp(w[0] * mr[0] + w[1] * mr[1], -1.f, 1.f), ly = std::clamp(w[0] * mf[0] + w[1] * mf[1], -1.f, 1.f);
        b[kPadRight] = static_cast<uint8_t>(std::lround(std::max(lx, 0.f) * 255.f));
        b[kPadLeft] = static_cast<uint8_t>(std::lround(std::max(-lx, 0.f) * 255.f));
        b[kPadUp] = static_cast<uint8_t>(std::lround(std::max(ly, 0.f) * 255.f));
        b[kPadDown] = static_cast<uint8_t>(std::lround(std::max(-ly, 0.f) * 255.f));
    }

    void padReadLockToggle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t port = GPR_U32(ctx, 4), slot = GPR_U32(ctx, 5), data = GPR_U32(ctx, 6) & 0x1FFFFFFFu;   // scePadRead(port, slot, rdata)
        g_origPadReadLock(rdram, ctx, runtime);
        if (port < 2 && slot < 2 && GPR_U32(ctx, 2) != 0u && ramOk(data, 32u))
        {
            const uint8_t btn = static_cast<uint8_t>(~rdram[data + 2]);   // active low: 0x80 left, 0x20 right
            g_padDir[port + 2 * slot] = static_cast<uint8_t>(((btn >> 7) & 1u) | (((btn >> 5) & 1u) << 1));
        }
    }

    void lockToggleApply(uint8_t *rdram, uint32_t pf, int config)
    {
        int slot = 0;
        LockToggle *t = lockToggleFor(rdram, pf, &slot);
        if (!t)
            return;
        const uint32_t owner = t->owner;
        const uint16_t a = rd16(rdram, pf + kPadFlagsLockA), b = rd16(rdram, pf + kPadFlagsLockB);
        const int32_t tweak = static_cast<int32_t>(FAST_READ32(pf + kPadFlagsHoldTweak));
        const uint16_t over = static_cast<uint16_t>(std::clamp<int32_t>(tweak + 1, 1, 0xFFFF));   // above the threshold: locks
        const uint16_t under = static_cast<uint16_t>(std::clamp<int32_t>(tweak, 0, 0xFFFF));
        const bool remap = config == 0 && t->remap;
        const bool held = remap ? t->lockHeld : config == 0 ? (a > tweak && b > tweak) : a != 0;
        if (t->on && lockTargetDead(rdram, owner))
        {
            t->on = false;   // when held, updateLock would move on to the nearest one; when toggled, release
            std::fprintf(stderr, "[lockon] player %d: released (target died)\n", slot + 1);
        }
        if (held && !t->held)
        {
            t->on = !t->on;
            if (t->on)
                lockNearestTarget(rdram, owner);
            std::fprintf(stderr, "[lockon] player %d: %s (target 0x%x)\n", slot + 1, t->on ? "locked" : "released", FAST_READ32(owner + kMonsterPinTarget));
        }
        t->held = held;
        t->realA = a;
        t->realB = b;
        if (config == 0)
        {
            if (t->on)
            {
                wr16(rdram, pf + kPadFlagsLockA, std::max(a, over));
                wr16(rdram, pf + kPadFlagsLockB, std::max(b, over));
            }
            else if (remap ? (a > tweak && b > tweak) : held)   // with the mod buttons: L2+R2 together don't lock either
            {
                wr16(rdram, pf + kPadFlagsLockA, std::min(a, under));   // both together, below the threshold: neither locks nor strafes
                wr16(rdram, pf + kPadFlagsLockB, std::min(b, under));
            }
        }
        else if (t->on)
            wr16(rdram, pf + kPadFlagsLockA, std::max<uint16_t>(a, 1));
        else
            wr16(rdram, pf + kPadFlagsLockA, 0);
        t->fakeA = rd16(rdram, pf + kPadFlagsLockA);
        t->fakeB = rd16(rdram, pf + kPadFlagsLockB);
        t->forged = t->fakeA != a || t->fakeB != b;
    }

    template <int N>
    void updateHistoryLockToggle(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t pf = GPR_U32(ctx, 4) & 0x1FFFFFFFu, ra = GPR_U32(ctx, 31);
        const bool ok = ctx->pc == kUpdateHistory[N] && ramOk(pf, kPadFlagsOwner + 4u);
        if (ok)
            if (LockToggle *t = lockToggleFind(pf, false); t && t->forged)
            {
                // updateHistory counts from the real counters (if someone reset them midway, what it wrote stays)
                if (rd16(rdram, pf + kPadFlagsLockA) == t->fakeA && rd16(rdram, pf + kPadFlagsLockB) == t->fakeB)
                {
                    wr16(rdram, pf + kPadFlagsLockA, t->realA);
                    wr16(rdram, pf + kPadFlagsLockB, t->realB);
                }
                t->forged = false;
            }
        g_origUpdateHistory[N](rdram, ctx, runtime);
        if (ok && ctx->pc == ra)
        {
            if (wotm::settings::on(wotm::settings::LockToggle))
                lockToggleApply(rdram, pf, N);
            else if (LockToggle *t = lockToggleFind(pf, false); t && (t->on || t->held))
            {
                t->on = t->held = false;   // option turned off mid-fight: release and go back to the game's buttons
                t->remap = false;
            }
        }
    }

#include "wotm_modifiers.inc"   // gameplay modifiers: Super Jump, Fast Monsters, Turbo
}

namespace
{
    // Health and energy bars over enemies (mod, WOTM_ENEMYBARS=1; launcher's "QoL features" group), replacing the health icon (the little ball
    // with the player number that changes color with health). Hud::drawHealthIcon(monster, n) projects the monster's head and sends two
    // quads to ScreenPolys::addPoly at the same point: the number (white) and the ring (color from +0x686C/+0x6870). Here the hook on
    // drawHealthIcon saves the monster, and the one on addPoly, when called from inside it, replaces the two quads with two untextured
    // bars (wotmBarRect, drawn with the screen rectangles) centered where the first would be: health (m_health/m_maxHealth,
    // +0x44C/+0x448, with recent damage in red) and, below it, energy (StaminaMeter at +0x460: cur/max; gray while exhausted),
    // in the style of the player's HUD bars. The rest of the HUD doesn't change.
    constexpr uint32_t kDrawHealthIcon = wotm::fn::drawHealthIcon__3HudP7Monsteri, kDrawHealthIconSize = 0x4F8u,
                       kAddPoly = wotm::fn::addPoly__11ScreenPolysiiiRQ211ScreenPolys8PolyData;
    constexpr uint32_t kMonsterMaxHealth = 0x448u, kMonsterHealth = 0x44Cu, kMonsterStamina = 0x460u;   // StaminaMeter: max +0, cur +0x10, exhausted +0x20
    constexpr uint32_t kXform2d = wotm::fn::mathfTransform3dTo2d__FiP8_fvectorT1;
    PS2Runtime::RecompiledFunction g_origDrawHealthIcon = nullptr, g_origAddPolyBars = nullptr, g_origXform2dBars = nullptr;
    uint32_t g_barMonster = 0;   // monster of the last drawHealthIcon (0 = bars off: the game's icon stays)
    bool g_barDone = false;      // its bars are already out (the second quad just disappears)
    float g_barRawX = 0.f;       // projected head x (GS pixels), before drawHealthIcon's clamp
    bool g_barRawOk = false;
    float g_barWorld[3] = {};    // the world point that drawHealthIcon projected (photo mode: bars and icon follow the camera)
    bool g_anchorDone = true;    // this drawHealthIcon's icon has already been recorded (wotmHudAnchor)

    float barFrac(uint8_t *rdram, uint32_t cur, uint32_t max)
    {
        float c, m;
        std::memcpy(&c, rdram + cur, 4);
        std::memcpy(&m, rdram + max, 4);
        return m > 0.f && std::isfinite(c) ? std::clamp(c / m, 0.f, 1.f) : 0.f;
    }

    // Player HUD style (measured from a 720p screenshot): black, gray (94) and black frame; inside, color bands across the
    // thickness (dark at the edges, light highlight near the middle). Green health with recent damage in red (highlight at the bottom), blue
    // energy; the empty part darkens the background.
    constexpr uint8_t kHealthProf[][3] = {{20, 58, 2},    {43, 101, 16},  {81, 170, 39},  {96, 181, 53},  {169, 191, 148},
                                          {146, 191, 109}, {85, 178, 42},  {68, 146, 31},  {31, 79, 9}};
    constexpr uint8_t kLagProf[][3] = {{80, 28, 26}, {118, 40, 38}, {160, 56, 53}, {160, 56, 53}, {187, 103, 104}, {184, 95, 95}};
    constexpr uint8_t kEnergyProf[][3] = {{13, 23, 67},    {37, 55, 133},  {46, 67, 153},  {112, 127, 153}, {146, 150, 153},
                                          {85, 105, 153},  {46, 67, 153},  {28, 43, 112},  {7, 15, 51}};
    // Energy above the maximum (up to maxLevel, rage): pink over the blue (profile measured from a screenshot of the player's bar); with rage
    // full (m_boostFull) the whole bar blinks between purple and pink. Rhythm measured on the player's bar (60 frames/s recording with
    // WOTM_CHEATS=furia): a cycle of ~6 frames, ~2 of them pink.
    constexpr uint8_t kOverProf[][3] = {{33, 4, 19},     {108, 19, 70},  {182, 33, 118}, {223, 40, 145}, {255, 56, 201},
                                        {255, 50, 175},  {182, 33, 118}, {119, 21, 77},  {38, 6, 24}};
    constexpr uint8_t kFuryDimProf[][3] = {{16, 10, 28},  {42, 22, 91},  {59, 35, 116}, {98, 43, 157}, {98, 55, 197},
                                           {87, 42, 178}, {51, 31, 115}, {39, 28, 82},  {14, 11, 30}};
    constexpr uint32_t kFuryBlinkPeriod = 6u, kFuryBlinkOn = 2u;   // frames
    constexpr uint8_t kExhaustedProf[][3] = {{30, 30, 34},  {60, 60, 68},   {80, 80, 90},   {120, 120, 128}, {140, 140, 146},
                                             {100, 100, 110}, {80, 80, 90},  {55, 55, 62},   {25, 25, 30}};

    template <size_t N>
    void gradBar(float x0, float y0, float x1, float y1, const uint8_t (&prof)[N][3])
    {
        if (x1 <= x0)
            return;
        const float h = (y1 - y0) / static_cast<float>(N);
        for (size_t i = 0; i < N; ++i)
            wotmBarRect(x0, y0 + h * i, x1, y0 + h * (i + 1), prof[i][0], prof[i][1], prof[i][2], 0x80);
    }

    // A bar's frame around the inside (x0, y0)-(x1, y1); 1 unit = 1.5 px in x and 3.2 px in y at 720p
    // Ring of thickness (tx, ty) outside (x0, y0)-(x1, y1)
    void barRing(float x0, float y0, float x1, float y1, float tx, float ty, uint8_t c)
    {
        wotmBarRect(x0 - tx, y0 - ty, x1 + tx, y0, c, c, c, 0x80);
        wotmBarRect(x0 - tx, y1, x1 + tx, y1 + ty, c, c, c, 0x80);
        wotmBarRect(x0 - tx, y0, x0, y1, c, c, c, 0x80);
        wotmBarRect(x1, y0, x1 + tx, y1, c, c, c, 0x80);
    }

    // Recent damage (the red section): holds ~0.5 s and drains down to current health, per monster
    struct BarLag
    {
        uint32_t m = 0;
        float lag = 0.f;
        int hold = 0;
    } g_barLag[16];

    float barLag(uint32_t m, float health)
    {
        BarLag *e = nullptr;
        for (BarLag &b : g_barLag)
            if (b.m == m)
                e = &b;
        if (!e)
            for (BarLag &b : g_barLag)
                if (!b.m)
                {
                    e = &b;
                    *e = BarLag{m, health, 0};
                    break;
                }
        if (!e)
            return health;
        if (health >= e->lag)
        {
            e->lag = health;
            e->hold = 0;
        }
        else if (++e->hold > 30)
            e->lag = std::max(health, e->lag - 0.006f);
        return e->lag;
    }

    // HUD portrait of each monster: the last entry of its .RTX (T8H 128x64; the normal face on the left half, the rage face on the
    // right). The already-relocated TEX0 lives in the monster's own NGP (portrait objects: TBP0 = the previous file's fileStatus.maxResAddr,
    // CBP = previous maxTexAddr + the portrait's palette, which moves around per monster). The monster's file comes from its root node
    // (Monster::m_cs -> _cs::epNode), which sits inside the NGP; the TEX0 is searched for once per loaded file.
    struct PortraitCache
    {
        uint32_t lo = 0, hi = 0;
        uint64_t tex0 = 0;
    } g_portraits[16];
    unsigned g_portraitGen = 0;

    uint64_t monsterPortrait(uint8_t *rdram, uint32_t m)
    {
        if (g_portraitGen != g_loadGen)
        {
            for (PortraitCache &c : g_portraits)
                c = PortraitCache{};
            g_portraitGen = g_loadGen;
        }
        if (!ramOk(m, 0x10u))
            return 0;
        const uint32_t cs = FAST_READ32(m + 0xCu) & 0x1FFFFFFFu;
        if (!ramOk(cs, 4u))
            return 0;
        const uint32_t ep = FAST_READ32(cs) & 0x1FFFFFFFu;
        const uint32_t n = FAST_READ8(kFileStatus + 0x74);
        for (uint32_t i = 1; i < n && i < 13; ++i)
        {
            const uint32_t lo = FAST_READ32(kFileStatus + 0x3C + 4 * i), hi = FAST_READ32(kFileStatus + 0x3C + 4 * (i + 1));
            if (ep < lo || ep >= hi || !ramOk(lo, hi - lo))
                continue;
            for (const PortraitCache &c : g_portraits)
                if (c.lo == lo && c.hi == hi)
                    return c.tex0;
            const uint32_t resBase = FAST_READ16(kFileStatus + 0x168 + 2 * (i - 1));
            uint64_t found = 0;
            for (uint32_t a = (lo + 7u) & ~7u; a + 8u <= hi && !found; a += 8u)
            {
                uint64_t v;
                std::memcpy(&v, rdram + a, 8);
                const uint32_t psm = uint32_t(v >> 20) & 0x3F, tw = uint32_t(v >> 26) & 15, th = uint32_t(v >> 30) & 15;
                if (psm == 0x1B && tw == 7 && th == 6 && ((v >> 51) & 15) == 0 && (v & 0x3FFF) == resBase)
                    found = v;
            }
            PortraitCache *slot = &g_portraits[0];
            for (PortraitCache &c : g_portraits)
                if (!c.lo || c.lo >= c.hi)
                {
                    slot = &c;
                    break;
                }
            *slot = PortraitCache{lo, hi, found};
            std::fprintf(stderr, "[wotm] portrait for file %u (NGP 0x%07X): %s\n", i, lo, found ? "found" : "not found");
            return found;
        }
        return 0;
    }

    // Special button lit: the red of the player's HUD icon (measured from a screenshot with WOTM_CHEATS=esp), in bands like the bars
    constexpr uint8_t kSpecialProf[][3] = {{106, 36, 34}, {156, 53, 50}, {196, 68, 65}, {222, 91, 88}, {235, 112, 111},
                                           {222, 91, 88}, {196, 68, 65}, {156, 53, 50}, {106, 36, 34}};

    // One block per enemy: portrait on the left at full height; on the right, health on top and, below, the special and the energy (thinner and
    // shorter). Each cell has its own black outline; the gray band between two cells is shared (the gap between them is exactly
    // black + gray + black), and a black outline closes the block. Game screen units, 640 x 224 (1 in y = 2.14 in x on screen).
    // s: block scale (1 = over the enemy; the chase view's HUD uses a bigger one, emitChaseHud)
    void emitBars(uint8_t *rdram, uint32_t m, const float box[4], float s = 1.f)
    {
        if (!ramOk(m, kMonsterStamina + 0x24u))
            return;
        const float health = barFrac(rdram, m + kMonsterHealth, m + kMonsterMaxHealth);
        const float energy = barFrac(rdram, m + kMonsterStamina + 0x10u, m + kMonsterStamina);
        const bool exhausted = FAST_READ32(m + kMonsterStamina + 0x20u) != 0u;
        const bool fury = static_cast<int8_t>(FAST_READ8(m + 0xF4u)) != 0;   // m_boostFull: Hud::update swaps the portrait's face based on it
        const bool special = FAST_READ8(m + 0xF3u) != 0 && FAST_READ32((FAST_READ32(wotm::sym::game) & 0x1FFFFFFFu) + 0x1203C8u) != 9u;   // Monster::isSpecialAvailable (TheGame::m_gameMode)
        const float lag = barLag(m, health);
        const float kOx = 0.7f * s, kOy = 0.3f * s, kGx = 1.3f * s, kGy = 0.6f * s, kIx = 1.0f * s, kIy = 0.45f * s;   // outline: black, gray, black
        const float kDx = 2.f * kIx + kGx, kDy = 2.f * kIy + kGy;                                                      // gap between two cells
        constexpr float kAspect = 480.f / 224.f;                                                                        // 1 unit in y in x units
        const float kW = 44.f * s, kHealthH = 2.6f * s, kEnergyH = 1.6f * s;
        const float kRowsH = kHealthH + kDy + kEnergyH, kPortraitW = kRowsH * kAspect, kSpecialW = kEnergyH * kAspect;
        const float cx = 0.5f * (box[0] + box[2]), cy = 0.5f * (box[1] + box[3]);
        const float bx0 = cx - 0.5f * (kPortraitW + kDx + kW), ty0 = cy - 0.5f * kRowsH;
        struct Cell { float x0, y0, x1, y1; };
        const float hx0 = bx0 + kPortraitW + kDx, sy0 = ty0 + kHealthH + kDy;
        const Cell portrait{bx0, ty0, bx0 + kPortraitW, ty0 + kRowsH};
        const Cell hp{hx0, ty0, hx0 + kW, ty0 + kHealthH};
        const Cell sp{hx0, sy0, hx0 + kSpecialW, sy0 + kEnergyH};
        const Cell st{hx0 + kSpecialW + kDx, sy0, hx0 + kW, sy0 + kEnergyH};
        const Cell cells[4] = {portrait, hp, sp, st};
        barRing(bx0 - kIx - kGx, ty0 - kIy - kGy, hx0 + kW + kIx + kGx, ty0 + kRowsH + kIy + kGy, kOx, kOy, 0);
        for (const Cell &c : cells)   // the gray of all of them before the black: one cell's gray doesn't cover its neighbor's outline
            barRing(c.x0 - kIx, c.y0 - kIy, c.x1 + kIx, c.y1 + kIy, kGx, kGy, 94);
        for (const Cell &c : cells)
        {
            barRing(c.x0, c.y0, c.x1, c.y1, kIx, kIy, 0);
            wotmBarRect(c.x0, c.y0, c.x1, c.y1, 0, 0, 0, 0x40);   // empty: the darkened background
        }
        if (const uint64_t t = monsterPortrait(rdram, m))
            wotmBarTex(portrait.x0, portrait.y0, portrait.x1, portrait.y1, t, fury ? 64.f : 0.f, 64.f, fury ? 128.f : 64.f, 0.f);   // V bottom to top, like polyBuildPrim (0x80 - v)
        gradBar(hp.x0 + kW * health, hp.y0, hp.x0 + kW * lag, hp.y1, kLagProf);
        gradBar(hp.x0, hp.y0, hp.x0 + kW * health, hp.y1, kHealthProf);
        if (special)
            gradBar(sp.x0, sp.y0, sp.x1, sp.y1, kSpecialProf);
        const float stW = st.x1 - st.x0;
        if (exhausted)
            gradBar(st.x0, st.y0, st.x0 + stW * energy, st.y1, kExhaustedProf);
        else
        {
            gradBar(st.x0, st.y0, st.x0 + stW * energy, st.y1, kEnergyProf);
            // whatever goes past the maximum, as in Hud::update: (cur - max) / (maxLevel - max)
            float cur, max, top;
            std::memcpy(&max, rdram + m + kMonsterStamina, 4);
            std::memcpy(&top, rdram + m + kMonsterStamina + 0x8u, 4);
            std::memcpy(&cur, rdram + m + kMonsterStamina + 0x10u, 4);
            const float over = top > max && std::isfinite(cur) ? std::clamp((cur - max) / (top - max), 0.f, 1.f) : 0.f;
            if (fury)
                gradBar(st.x0, st.y0, st.x1, st.y1,
                        FAST_READ32(wotm::sym::g_frame) % kFuryBlinkPeriod < kFuryBlinkOn ? kOverProf : kFuryDimProf);
            else if (over > 0.f)
                gradBar(st.x0, st.y0, st.x0 + stW * over, st.y1, kOverProf);
        }
    }

    // HUD for player 5..8 on their own PC (WOTM_LOCALPLAYER): the game has no HUD for the AIs, so the chase view (bars 4 + k) shows
    // the emitBars block (portrait, health, special, energy) of the monster itself (AI m_monsters[4 + k]), bigger, in the top-left
    // corner, like player 1's. Once per frame, at the frame boundary (before the capture).
    void emitChaseHud(uint8_t *rdram)
    {
        static const int local = [] { const char *e = std::getenv("WOTM_LOCALPLAYER"); return e ? std::atoi(e) : 0; }();
        if (local < 5 || local > 4 + g_padAIs)
            return;
        constexpr float kScale = 3.f, kLeft = 18.f, kTop = 10.f, kHalfW = 30.f * kScale, kHalfH = 3.6f * kScale;
        const float box[4] = {kLeft + kHalfW - 1.f, kTop + kHalfH - 1.f, kLeft + kHalfW + 1.f, kTop + kHalfH + 1.f};
        if (const uint32_t m = FAST_READ32(kGame + 0x120380 + 4 * (local - 1)))
        {
            wotmBarView(local - 1);
            emitBars(rdram, m, box, kScale);
            wotmBarView(0);
        }
    }

    void drawHealthIconBars(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kDrawHealthIcon)
        {
            // with WOTM_ENEMYBARS always; without it, only in 3- or 4-player fights (screen in quadrants, wotm_split4.inc)
            // option on: bars in every fight (without it, only in 3- and 4-player multiplayer)
            const bool on = wotm::settings::on(wotm::settings::EnemyBars) || wotm_split4::splitPlayers(rdram);
            g_barMonster = on ? GPR_U32(ctx, 5) & 0x1FFFFFFFu : 0u;
            g_barDone = false;
            g_barRawOk = false;
            g_anchorDone = false;
        }
        g_origDrawHealthIcon(rdram, ctx, runtime);
        g_anchorDone = true;
        wotmHudAnchorEnd();
    }

    // mathfTransform3dTo2d(view, point, out) called from drawHealthIcon: saves x before the 4:3 view's clamp
    void xform2dBars(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = GPR_U32(ctx, 31), out = GPR_U32(ctx, 6) & 0x1FFFFFFFu, in = GPR_U32(ctx, 5) & 0x1FFFFFFFu;
        const bool mine = ctx->pc == kXform2d && ra >= kDrawHealthIcon && ra < kDrawHealthIcon + kDrawHealthIconSize && ramOk(out, 8u) &&
                          ramOk(in, 12u);
        float world[3] = {};
        if (mine)
            std::memcpy(world, rdram + in, 12);   // before the call: the output may be the same vector
        g_origXform2dBars(rdram, ctx, runtime);
        if (mine && ctx->pc == ra)
        {
            std::memcpy(&g_barRawX, rdram + out, 4);
            std::memcpy(g_barWorld, world, 12);
            g_barRawOk = true;
        }
    }

    void addPolyBars(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = GPR_U32(ctx, 31);
        const bool fromIcon = ctx->pc == kAddPoly && ra >= kDrawHealthIcon && ra < kDrawHealthIcon + kDrawHealthIconSize;
        if (!fromIcon || !g_barMonster)
        {
            // game icon (no bars): record the center and the world point, so photo mode can carry it along with the camera
            float box[4];
            if (fromIcon && !g_anchorDone && g_barRawOk &&
                wotmScreenPolyBox(rdram, static_cast<int>(GPR_U32(ctx, 4)), static_cast<int>(GPR_U32(ctx, 5)), static_cast<int>(GPR_U32(ctx, 6)),
                                  GPR_U32(ctx, 7), box))
                wotmHudAnchor(rdram, 0.5f * (box[0] + box[2]), 0.5f * (box[1] + box[3]), g_barWorld, false);
            if (fromIcon) g_anchorDone = true;
            g_origAddPolyBars(rdram, ctx, runtime);
            return;
        }
        float box[4];
        if (!g_barDone && wotmScreenPolyBox(rdram, static_cast<int>(GPR_U32(ctx, 4)), static_cast<int>(GPR_U32(ctx, 5)),
                                            static_cast<int>(GPR_U32(ctx, 6)), GPR_U32(ctx, 7), box))
        {
            // clamp x to the view's widescreen width (that's the rectangles' view); without the raw x, the game's x stays. In quadrants the raw x
            // comes in outside the view (below) and the offset came out wrong: only the rectangles' view is kept (native drawing adds the column)
            const float shift = wotmHealthIconShift(rdram, g_barRawOk ? g_barRawX : std::nanf(""));
            const float dx = wotm_split4::splitPlayers(rdram) ? 0.f : shift;
            box[0] += dx;
            box[2] += dx;
            // screen in quadrants: in the bottom views the icon's y comes with the quadrant offset applied twice (like view 1's x, which
            // native drawing compensates for), below the screen (~228 of 224): bring it back to the quadrant's height. Then the block (~4.2 lines to
            // each side of the center) stays inside the view's band (viewDef: y +0xE, height +6), like the game's icon at the edge
            if (wotm_split4::splitPlayers(rdram))
            {
                const uint32_t view = std::min<uint32_t>(FAST_READ32(wotm::sym::viewCurView), 3u);
                const uint32_t def = FAST_READ32(wotm::sym::viewInfo + 0x14u * view + 4u) & 0x1FFFFFFFu;
                if (ramOk(def, 0x10u))
                {
                    constexpr float kHalf = 5.f;
                    const float y0 = FAST_READ16(def + 0xE), y1 = y0 + FAST_READ16(def + 6);
                    const float cy = 0.5f * (box[1] + box[3]) - y0;   // y0 = 0 at the top, the quadrant's height at the bottom
                    const float dy = std::clamp(cy, y0 + kHalf, y1 - kHalf) - 0.5f * (box[1] + box[3]);
                    box[1] += dy;
                    box[3] += dy;
                    // the right column's x also comes with the column's 320 applied twice (~805): bring it back to the column (view x, +0xC)
                    const float x0 = FAST_READ16(def + 0xC);
                    box[0] -= x0;
                    box[2] -= x0;
                }
            }
            wotmHudAnchor(rdram, 0.5f * (box[0] + box[2]), 0.5f * (box[1] + box[3]), g_barRawOk ? g_barWorld : nullptr, true);
            g_anchorDone = true;
            emitBars(rdram, g_barMonster, box);
            wotmHudAnchorEnd();
        }
        g_barDone = true;
        ctx->pc = ra;   // the icon's quad is not built
    }
}

namespace
{
    // Damage numbers (mod, WOTM_DAMAGEPOP=1; launcher's "QoL features" group). Each monster's health (m_health, +0x44C) is checked on
    // entry to Monster::update: whatever dropped since the last check becomes a number (wotmDamagePopup), which also catches fire, shock and
    // falls, which don't go through takeHit. The type comes from the victim's last Monster::takeHit (up to 2 frames earlier), via the
    // HitEvent::s_takeHitInfo the attacker built: source 1, 5, 6, 12, 14 and 0x14 = special (Weapon, FireBreath, StateZapAttack,
    // StateSonicRoar, StateGrappleHook, StateUltraTazer, StateTazerAttack, StateTopSpin; 6 and 0x14 are the ones StateBlock handles along with
    // 5, and Togera's special arrives with 6), 8 and 13 = throw (MonsterState::handleCollis for a flying object, StateThrowBack for a
    // thrown monster; counts for up to 45 frames, since the damage from hitting the scenery comes later), the rest = hit. Placement: the hit point
    // (s_takeHitInfo.pos) when there is one, otherwise a little above the monster. Nothing shows during a cutscene (ActCinema: shell->m_inCinema,
    // moviePlaying) nor during each monster's first 30 updates in the fight, while the game is still setting the initial health; rtMain(first)
    // forgets the previous fight's monsters (g_frame doesn't work: rtMain also resets it when returning from pause).
    constexpr uint32_t kMonsterUpdateDmg = wotm::fn::update__7Monster, kTakeHitDmg = wotm::fn::takeHit__7MonsterP8_fvectorfi;
    constexpr uint32_t kTakeHitInfo = wotm::sym::x_8HitEvent_s_takeHitInfo;   // HitEvent: pos* +0, source +0x10
    PS2Runtime::RecompiledFunction g_origMonsterUpdateDmg = nullptr, g_origTakeHitDmg = nullptr;
    struct DamageTrack
    {
        uint32_t m = 0;
        float last = 0.f;
        int kind = 0;
        uint32_t hitFrame = 0;
        bool havePos = false;
        float pos[3] = {};
        uint32_t seen = 0;   // updates since the monster entered the stage
    } g_damageTrack[16];
    std::atomic<int64_t> g_damageUpdateMs{0};   // clock (steady_clock) of the last Monster::update: stopped = game paused

    // One of TheGame's m_monsters (room for 16)
    bool isMonsterSlot(uint8_t *rdram, uint32_t m)
    {
        for (uint32_t i = 0; i < 16; ++i)
            if (m && (FAST_READ32(kGame + 0x120380 + 4 * i) & 0x1FFFFFFFu) == m)
                return true;
        return false;
    }

    DamageTrack *damageTrack(uint8_t *rdram, uint32_t m)
    {
        for (DamageTrack &d : g_damageTrack)
            if (d.m == m)
                return &d;
        for (DamageTrack &d : g_damageTrack)
            if (!d.m || !isMonsterSlot(rdram, d.m))
            {
                d = DamageTrack{};
                d.m = m;
                std::memcpy(&d.last, rdram + m + 0x44Cu, 4);
                return &d;
            }
        return nullptr;
    }

    int damageKind(int source)
    {
        switch (source)
        {
        case 1: case 5: case 6: case 12: case 14: case 20: return 2;
        case 8: case 13: return 1;
        default: return 0;
        }
    }

    void takeHitDamage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
        if (ctx->pc == kTakeHitDmg && ramOk(m, 0x450u) && isMonsterSlot(rdram, m))
            if (DamageTrack *d = damageTrack(rdram, m))
            {
                d->kind = damageKind(static_cast<int32_t>(FAST_READ32(kTakeHitInfo + 0x10u)));
                d->hitFrame = FAST_READ32(wotm::sym::g_frame);
                const uint32_t p = FAST_READ32(kTakeHitInfo) & 0x1FFFFFFFu;
                d->havePos = ramOk(p, 0x10u);
                if (d->havePos)
                    for (int i = 0; i < 3; ++i)
                        d->pos[i] = rdF(rdram, p + 4 * i);
                d->havePos = d->havePos && std::isfinite(d->pos[0]) && std::isfinite(d->pos[1]) && std::isfinite(d->pos[2]);
            }
        g_origTakeHitDmg(rdram, ctx, runtime);
    }

    void monsterUpdateDamage(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t m = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
        if (ctx->pc == kMonsterUpdateDmg && ramOk(m, 0x450u) && isMonsterSlot(rdram, m))
        {
            if (g_damageNewFight)
            {
                g_damageNewFight = false;
                for (DamageTrack &t : g_damageTrack)
                    t = DamageTrack{};
            }
            g_damageUpdateMs.store(std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count(),
                                   std::memory_order_relaxed);
            if (DamageTrack *d = damageTrack(rdram, m))
            {
                const uint32_t sh = FAST_READ32(wotm::sym::shell) & 0x1FFFFFFFu;
                const bool cinema = FAST_READ32(wotm::sym::moviePlaying) != 0u || (ramOk(sh, 0x2BA8u) && FAST_READ32(sh + 0x2BA4u) != 0u);
                const bool quiet = cinema || d->seen < 30u;
                if (d->seen < 30u)
                    ++d->seen;
                float h;
                std::memcpy(&h, rdram + m + 0x44Cu, 4);
                if (std::isfinite(h))
                {
                    const float lost = d->last - h;
                    if (lost >= 0.05f)
                    {
                        const uint32_t g = FAST_READ32(wotm::sym::g_frame);
                        // a thrown monster takes the damage when it hits the scenery, a few frames after the takeHit that launched it
                        const bool recent = d->hitFrame && g - d->hitFrame <= (d->kind == 1 ? 45u : 2u);
                        float p[3];
                        if (recent && d->havePos)
                            std::memcpy(p, d->pos, sizeof p);
                        else if (targetPos(rdram, m, p))
                            p[2] += 60.f;   // no hit point: a little above the monster's origin
                        else
                            p[0] = p[1] = p[2] = 0.f;
                        if (!quiet && (p[0] != 0.f || p[1] != 0.f || p[2] != 0.f) &&
                            (wotm::settings::on(wotm::settings::DamagePop) || wotm_training::g_on.load(std::memory_order_relaxed)))
                            wotmDamagePopup(rdram, p, lost, recent ? d->kind : 0);
                    }
                    d->last = h;
                }
            }
        }
        g_origMonsterUpdateDmg(rdram, ctx, runtime);
    }
}

// Window (drawDamagePopups): how long since the game last ran a Monster::update. They stop during fight pause, dialogs and F7.
double wotmDamageIdleSeconds()
{
    const int64_t now = std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
    return double(now - g_damageUpdateMs.load(std::memory_order_relaxed)) / 1000.0;
}

void wotmInstallOverrides(PS2Runtime &runtime, const std::string &elfPath)
{
    const size_t slash = elfPath.find_last_of("/\\");
    const std::string name = slash == std::string::npos ? elfPath : elfPath.substr(slash + 1);
    if (name.find("SCUS_971.97") == std::string::npos)
        return;

    wotmModsInstall(runtime, elfPath);
    if (const char *e = std::getenv("WOTM_RATEMIN"))
        g_minRate = std::atoi(e);
    if (std::getenv("WOTM_TURNLOG") && (g_origUpdatePosition = runtime.lookupFunction(kMonsterDynamicsUpdatePosition)))
    {
        runtime.registerFunction(kMonsterDynamicsUpdatePosition, &updatePositionLogged);
        std::fprintf(stderr, "[wotm] monster rotation log on (WOTM_TURNLOG)\n");
    }
    if (std::getenv("WOTM_FIELDLOG"))
    {
        g_fieldLog = g_wotmCycleProf = true;
        g_lastHost = std::chrono::steady_clock::now();
        std::fprintf(stderr, "[wotm] per-frame fields log on (WOTM_FIELDLOG)\n");
    }
    if (g_minRate > 0 || g_fieldLog)
        runtime.registerFunction(kTimerGetUpdateRate, &timerGetUpdateRate);
    if ((g_origSyncV = runtime.lookupFunction(kSceGsSyncV)))
        runtime.registerFunction(kSceGsSyncV, &syncVTapped);
    g_nativeTimer = std::getenv("WOTM_TIMER_EMU") == nullptr;
#if !PS2X_HW_EMU
    if (!g_nativeTimer)   // wotm.exe has no EE timers (FRONTEIRA.md, step 8): the emulated T1 would stay frozen
    {
        std::fprintf(stderr, "[wotm] WOTM_TIMER_EMU ignored: wotm.exe has no EE timers (use ps2EntryRunner)\n");
        g_nativeTimer = true;
    }
#endif
    if (std::getenv("WOTM_30HZ") == nullptr && (g_origTimerInit = runtime.lookupFunction(kTimerInit)))
        runtime.registerFunction(kTimerInit, &timerInit60);
    if ((g_origWaitMinRate = runtime.lookupFunction(kTimerWaitForMinUpdateRate)) &&
        (g_origGetCurTics = runtime.lookupFunction(kTimerGetCurTics)) &&
        (g_origTimer1 = runtime.lookupFunction(kTimer1Handler)))
    {
        runtime.registerFunction(kTimerWaitForMinUpdateRate, &waitMinRateByVblanks);
        if (g_nativeTimer)
        {
            runtime.registerFunction(kTimerGetCurTics, &getCurTicsNative);
            runtime.registerFunction(kTimer1Handler, &timer1HandlerNative);
            std::fprintf(stderr, "[wotm] game timer on the host clock (WOTM_TIMER_EMU=1 goes back to the emulated timer 1)\n");
        }
        else
            runtime.registerFunction(kTimerGetCurTics, &getCurTicsMarked);
    }
    else
        g_nativeTimer = false;
    if ((g_origRtFade = runtime.lookupFunction(kRtFadeScreen)))
        runtime.registerFunction(kRtFadeScreen, &rtFadeScreenTapped);
    if ((g_origGameFadeOut = runtime.lookupFunction(kTheGameFadeOut)))
        runtime.registerFunction(kTheGameFadeOut, &theGameFadeTapped<kTheGameFadeOut, &g_origGameFadeOut>);
    if ((g_origGameFadeOutAndIn = runtime.lookupFunction(kTheGameFadeOutAndIn)))
        runtime.registerFunction(kTheGameFadeOutAndIn, &theGameFadeTapped<kTheGameFadeOutAndIn, &g_origGameFadeOutAndIn>);
    if ((g_origDmaGsPktQueue = runtime.lookupFunction(kDmaGsPktQueue)))
        runtime.registerFunction(kDmaGsPktQueue, &dmaGsPktQueueTapped);
    if ((g_origFontDma = runtime.lookupFunction(kFontDmaFontData)))
        runtime.registerFunction(kFontDmaFontData, &fontDmaTapped);
    if ((g_origFontInitPacket = runtime.lookupFunction(kFontInitPacket)))
        runtime.registerFunction(kFontInitPacket, &fontInitPacketTapped);
    if ((g_origFontSetTexId = runtime.lookupFunction(kFontSetTexId)))
        runtime.registerFunction(kFontSetTexId, &fontSetTexIdTapped);
    if ((g_origFontColor = runtime.lookupFunction(kFontSetColorGifTag)))
        runtime.registerFunction(kFontSetColorGifTag, &fontColorTapped);
    if ((g_origFontGlyph = runtime.lookupFunction(kFontBuildPrim)))
        runtime.registerFunction(kFontBuildPrim, &fontGlyphTapped);
    if ((g_origUiFade = runtime.lookupFunction(kUiFadeScreen)))
        runtime.registerFunction(kUiFadeScreen, &uiFadeScreenTapped);
    if ((g_origThreeMileFlash = runtime.lookupFunction(kThreeMileDrawFlash)))
        runtime.registerFunction(kThreeMileDrawFlash, &threeMileFlashTapped);
    if ((g_origLetterbox = runtime.lookupFunction(kLetterboxDmaData)))
        runtime.registerFunction(kLetterboxDmaData, &letterboxTapped);
    if ((g_origShellFade = runtime.lookupFunction(kShellFadeScreen)))
        runtime.registerFunction(kShellFadeScreen, &shellFadeScreenTapped);
    if ((g_origLoadLevelFiles = runtime.lookupFunction(kLoadLevelFiles)) && (g_origRtMain = runtime.lookupFunction(kRtMain)))
    {
        runtime.registerFunction(kLoadLevelFiles, &loadLevelFilesTapped);
        runtime.registerFunction(kRtMain, &rtMainTapped);
    }
    {
        const char *h = std::getenv("WOTM_HIGHRAM");
        if (!(h && h[0] == '0') && (g_origLoadMonstersDB = runtime.lookupFunction(kLoadMonstersDB)))
            runtime.registerFunction(kLoadMonstersDB, &loadMonstersDBHigh);
        else
            std::fprintf(stderr, "[wotm] monsters loaded below 32 MB, as on the PS2 (WOTM_HIGHRAM=0)\n");
    }
    {   // player options (wotm_settings.hpp): settings.ini next to save.json (the same folder wotm_save::initPaths finds)
        const PS2Runtime::IoPaths &io = PS2Runtime::getIoPaths();
        std::filesystem::path root = io.mcRoot;
        if (root.empty())
            root = !io.elfDirectory.empty() ? io.elfDirectory / "mc0" : std::filesystem::current_path() / "mc0";
        wotm::settings::load(root.lexically_normal().parent_path());
    }
    wotm_save::install(runtime);
    if ((g_origEmitMatrices = runtime.lookupFunction(kEmitMatrices)))   // emitter that teleports (8 monsters with Agamo)
        runtime.registerFunction(kEmitMatrices, &emitMatricesGuarded);
    if ((g_origParticleEmit = runtime.lookupFunction(kParticleEmit)))
        runtime.registerFunction(kParticleEmit, &particleEmitGuarded);
    if ((g_origLosCs = runtime.lookupFunction(kLosCs)) && (g_origMathfRand = runtime.lookupFunction(kMathfRand)))   // chain lightning with 8
    {
        runtime.registerFunction(kLosCs, &losCsTapped);
        runtime.registerFunction(kMathfRand, &mathfRandTapped);
    }
    wotm_split4::install(runtime);   // before the scePadRead hooks (free camera, lock-on): they wrap its hook
    wotm_modes4::install(runtime);   // wraps split4's end-of-round and InitPlayers hooks; the cheats' takeDamage wraps its own
    if ((g_origPolyBuild = runtime.lookupFunction(kPolyBuildPrim)))
        runtime.registerFunction(kPolyBuildPrim, &polyBuildPrimTapped);
    if ((g_origHier = runtime.lookupFunction(kHier)))
        runtime.registerFunction(kHier, &hierTapped);
    if (const char *d = std::getenv("WOTM_LOGICDELAY"))
    {
        g_logicDelayMs = std::max(0, std::atoi(d));
        std::fprintf(stderr, "[wotm] delay of 0 to %d ms on entry to TheGame::Update (WOTM_LOGICDELAY)\n", g_logicDelayMs);
    }
    if (g_origHier && (g_origTheGameUpdate = runtime.lookupFunction(kTheGameUpdate)))
        runtime.registerFunction(kTheGameUpdate, &theGameUpdateTapped);
    if (g_origHier && (g_origTheGameUpdate2 = runtime.lookupFunction(kTheGameUpdate2)))
        runtime.registerFunction(kTheGameUpdate2, &theGameUpdate2Tapped);
    if ((g_origLensFlareRender = runtime.lookupFunction(kLensFlareRender)))
        runtime.registerFunction(kLensFlareRender, &lensFlareRenderTapped);
    wotmPadSelInstall(runtime);
    wotmLevelSelInstall(runtime);
    wotmMainMenuInstall(runtime);
    wotm_menu::install(runtime);
    if ((g_origEnableHighlights = runtime.lookupFunction(kEnableHighlights)))
        runtime.registerFunction(kEnableHighlights, &enableHighlightsTapped);
    if (const char *e = std::getenv("WOTM_MATCH"))
    {
        int v[kMatchMaxAIs + 2], c[kMatchMaxAIs + 2], n = 0;
        const char *p = e;
        for (char *end; n < kMatchMaxAIs + 2; p = end + 1)
        {
            c[n] = -1;
            v[n++] = static_cast<int>(std::strtol(p, &end, 10));
            if (end == p && std::isalpha(static_cast<unsigned char>(*p)))   // "<custom monster id>" (mods/<mod>/monsters/, wotm_custom.inc)
            {
                const char *t = p;
                while (*t && *t != ',' && *t != ':')
                    ++t;
                const std::string id(p, t);
                if (const wotm::mods::Custom *cu = wotm::mods::Registry::get().custom(id))
                {
                    v[n - 1] = cu->type;
                    c[n - 1] = cu->costume;
                    end = const_cast<char *>(t);
                }
                else
                    std::fprintf(stderr, "[wotm] WOTM_MATCH: custom monster \"%s\" not found in mods/\n", id.c_str());
            }
            if (end != p && *end == ':')   // "<monster>:<costume>"
            {
                const char *q = end + 1;
                if (*q && !std::isdigit(static_cast<unsigned char>(*q)))   // "<monster>:<skin>": mod costume by id (mods/<mod>/skins/)
                {
                    const char *t = q;
                    while (*t && *t != ',')
                        ++t;
                    const std::string id(q, t);
                    const wotm::mods::Skin *sk = wotm::mods::Registry::get().skin(v[n - 1], id);
                    if (!sk)
                        std::fprintf(stderr, "[wotm] WOTM_MATCH: skin \"%s\" for monster %d not found in mods/; costume 0\n", id.c_str(), v[n - 1]);
                    c[n - 1] = sk ? sk->index : 0;
                    end = const_cast<char *>(t);
                }
                else
                    c[n - 1] = static_cast<int>(std::strtol(q, &end, 10));
                if (end == q || c[n - 1] < 0 || c[n - 1] > 99)   // 4..99: mod costumes (file name up to 8 letters)
                {
                    p = nullptr;
                    break;
                }
            }
            if (end == p || *end != ',')
            {
                p = end == p ? nullptr : end;
                break;
            }
        }
        if (const char *pl = std::getenv("WOTM_PLAYERS"); pl && std::atoi(pl) >= 2 && std::atoi(pl) <= 8)
        {
            g_matchPlayers = std::min(std::atoi(pl), 4);
            g_padAIs = std::atoi(pl) - g_matchPlayers;
        }
        if (const char *md = std::getenv("WOTM_MODE"))   // "<mode>[:<continues>]" (wotm_modes4.inc)
        {
            const int m = std::atoi(md);
            g_matchMode = m >= 6 && m <= 9 ? m : 0;
            if (const char *k = std::strchr(md, ':'))
                g_matchContinues = std::clamp(std::atoi(k + 1), 0, 9);
        }
        if (const char *kt = std::getenv("WOTM_KILLTARGET"))
            g_matchKillTarget = std::clamp(std::atoi(kt), 0, 99);
        int bad = -1;
        if (p && *p == '\0' && n >= 2 && !wotm_monsters::fits(v, c, n - 1, &bad))   // monster with no file on the disc: the load would hang
            std::fprintf(stderr, "[wotm] WOTM_MATCH rejected: monster %d (list item %d) has no file on the disc (costume %s); valid: 1-5, 7-11, "
                                 "13 (costume 1; 0 only if requested), 14 and 15 (1 costume)\n", v[bad], bad + 1,
                         wotm_monsters::costumes(v[bad]) ? "repeated too often or requested without a file" : "none");
        else if (p && *p == '\0' && n >= 1 + g_matchPlayers + g_padAIs)   // "<monster>,<stage>" = alone on the stage, no AI (like the menu's free for all with 0 AIs)
        {
            g_matchAIs = n - 1 - g_matchPlayers;
            for (int i = 0; i < n - 1; ++i)
                g_matchMonsters[i] = v[i], g_matchCostumes[i] = c[i];
            g_matchLevel = v[n - 1];
        }
        if (g_matchLevel >= 0)
        {
            runtime.registerFunction(kUserintMain, &userintMainDirect);
            runtime.registerFunction(kUiMain, &uiMainSkipped);
            if ((g_origEvaluate = runtime.lookupFunction(kEvaluateGameStatus)))
                runtime.registerFunction(kEvaluateGameStatus, &evaluateGameStatusLogged);
            std::fprintf(stderr, "[wotm] direct match: %d player(s), monster %d, %d AI(s) (%d on player pads), stage %d (WOTM_MATCH)\n",
                         g_matchPlayers + g_padAIs, g_matchMonsters[0], g_matchAIs, g_padAIs, g_matchLevel);
        }
        else
            std::fprintf(stderr, "[wotm] invalid WOTM_MATCH (\"%s\"; use <monster>[:<costume 0-3>][,<AI>[:<costume>]...up to 7],<stage>)\n", e);
    }
    if (std::getenv("WOTM_AIVSAI") && (g_aiInit = runtime.lookupFunction(kAiInit)) &&
        (g_origSetAIMonster = runtime.lookupFunction(kSetAIMonster)))
    {
        g_aiVsAi = true;
        runtime.registerFunction(kPlayerInit, &playerInitAsAi);
        runtime.registerFunction(kSetAIMonster, &setAIMonsterCounted);
        if ((g_origStartPoint = runtime.lookupFunction(kGameGetStartPoint)))
            runtime.registerFunction(kGameGetStartPoint, &startPointAsHumans);
        if ((g_origCamerasUpdate = runtime.lookupFunction(kCamerasUpdate)))
            runtime.registerFunction(kCamerasUpdate, &camerasUpdateAsHumans);
        std::fprintf(stderr, "[wotm] player controlled by the AI (WOTM_AIVSAI)\n");
    }
    wotm_multi::install(runtime);   // after the LoadLevelFiles hook (stage load), which it wraps
    wotm_sync::init(runtime);
    wotm_modifiers::install(runtime);   // after the direct match's userintMain, which it chains
    if ((g_origFfaOptions1P = runtime.lookupFunction(kFfaOptions1P)))
        runtime.registerFunction(kFfaOptions1P, &ffaOptions1PWithZero);
    if ((g_origFfaOptions2P = runtime.lookupFunction(kFfaOptions2P)))
        runtime.registerFunction(kFfaOptions2P, &ffaOptions2PMore);
    // Debug cheats (F1 menu): always installed; WOTM_CHEATS=inv,stam,furia,esp,cloak,deus turns them on from startup
    if ((g_origMonsterUpdate = runtime.lookupFunction(kMonsterUpdate)) && (g_origTakeDamage = runtime.lookupFunction(kTakeDamage)))
    {
        g_setCloakOn = runtime.lookupFunction(kSetCloakOn);
        runtime.registerFunction(kMonsterUpdate, &monsterUpdateCheats);
        runtime.registerFunction(kTakeDamage, &takeDamageCheat);
        if ((g_origTakeHit = runtime.lookupFunction(kTakeHit)))
            runtime.registerFunction(kTakeHit, &takeHitCheat);
        if ((g_origKnockBack = runtime.lookupFunction(kKnockBack)))
            runtime.registerFunction(kKnockBack, &knockBackCheat);
        if ((g_origAdditiveRecoil = runtime.lookupFunction(kAdditiveRecoil)))
            runtime.registerFunction(kAdditiveRecoil, &additiveRecoilCheat);
        if ((g_origEnterNewState = runtime.lookupFunction(kEnterNewState)))
            runtime.registerFunction(kEnterNewState, &enterNewStateCheat);        uint32_t c = std::getenv("WOTM_CLOAK") ? kCheatCloak : 0u;
        if (const char *e = std::getenv("WOTM_CHEATS"))
        {
            const std::string s = std::string(",") + e + ",";
            if (s.find(",inv,") != std::string::npos) c |= kCheatInvincible;
            if (s.find(",stam,") != std::string::npos) c = (c & ~kCheatStaminaMask) | kCheatStamina1;
            if (s.find(",furia,") != std::string::npos) c = (c & ~kCheatStaminaMask) | 2u * kCheatStamina1;
            if (s.find(",esp,") != std::string::npos) c |= kCheatSpecial;
            if (s.find(",cloak,") != std::string::npos) c |= kCheatCloak;
            if (s.find(",deus,") != std::string::npos) c |= kCheatGod;
            if (const size_t w = s.find(",vencer="); w != std::string::npos)
                g_autoWinSecs = std::atoi(s.c_str() + w + 8);
        }
        g_wotmCheats = c;
        if (c || g_autoWinSecs > 0)
            std::fprintf(stderr, "[wotm] debug cheats on from startup: 0x%x, win every %d s (WOTM_CHEATS/WOTM_CLOAK)\n", c, g_autoWinSecs);
    }
    // Free camera (F5): after the other hooks, so the Cameras::Update one wraps the WOTM_AIVSAI one when it exists
    if ((g_origCamerasUpdateFree = runtime.lookupFunction(kCamerasUpdate)) && (g_origViewUpdate = runtime.lookupFunction(kViewUpdate)) &&
        (g_origPadRead = runtime.lookupFunction(kScePadRead)))
    {
        runtime.registerFunction(kCamerasUpdate, &camerasUpdateFree);
        runtime.registerFunction(kViewUpdate, &viewUpdateFree);
        runtime.registerFunction(kScePadRead, &padReadFree);
    }
    else
        std::fprintf(stderr, "[wotm] free camera unavailable: Cameras::Update, viewUpdate or scePadRead has no function in the runtime\n");
    // Toggle lock-on (mod, WOTM_LOCKTOGGLE=1, "QoL features"): one press locks, another releases. Hooks always installed; the option
    // (wotm_settings.hpp) applies live, and when it's off they just pass through
    {
        constexpr PS2Runtime::RecompiledFunction kHooks[3] = {&updateHistoryLockToggle<0>, &updateHistoryLockToggle<1>,
                                                              &updateHistoryLockToggle<2>};
        for (int i = 0; i < 3; i++)
            g_origUpdateHistory[i] = runtime.lookupFunction(kUpdateHistory[i]);
        if (g_origUpdateHistory[0] && g_origUpdateHistory[1] && g_origUpdateHistory[2])
        {
            for (int i = 0; i < 3; i++)
                runtime.registerFunction(kUpdateHistory[i], kHooks[i]);
            std::fprintf(stderr, "[wotm] toggle lock-on (WOTM_LOCKTOGGLE, now %s): one press locks, another releases\n",
                         wotm::settings::on(wotm::settings::LockToggle) ? "on" : "off");
            // mod buttons in config 0 (after the free camera: the scePadRead hook here wraps its one)
            if ((g_origInterpretInputs = runtime.lookupFunction(kInterpretInputs)) && (g_origPadReadLock = runtime.lookupFunction(kScePadRead)))
            {
                runtime.registerFunction(kInterpretInputs, &interpretInputsLockToggle);
                runtime.registerFunction(kScePadRead, &padReadLockToggle);
                std::fprintf(stderr, "[wotm] lock-on on R1, block on L1, strafe on L2/R2, arrows switch target (config 0)\n");
            }
        }
        else
            std::fprintf(stderr, "[wotm] toggle lock-on unavailable: PadFlags::updateHistory{0,1,2} has no function in the runtime\n");
    }
    // Modern camera (wotm_orbit.inc): Cameras::Update and interpretInputs are already wrapped above; here shakeCamera and
    // SetOkToUnify (after wotm_split4, whose hook this one wraps)
    orbit::install(runtime);
    // Damage numbers (mod, WOTM_DAMAGEPOP=1): after the cheats, which also wrap Monster::update and takeHit. Always
    // installed (tracking health is cheap); the option only decides whether the number shows
    {
        if ((g_origMonsterUpdateDmg = runtime.lookupFunction(kMonsterUpdateDmg)) && (g_origTakeHitDmg = runtime.lookupFunction(kTakeHitDmg)))
        {
            runtime.registerFunction(kMonsterUpdateDmg, &monsterUpdateDamage);
            runtime.registerFunction(kTakeHitDmg, &takeHitDamage);
            std::fprintf(stderr, "[wotm] damage numbers (WOTM_DAMAGEPOP, now %s): hit white, throw orange, special cyan\n",
                         wotm::settings::on(wotm::settings::DamagePop) ? "on" : "off");
        }
        else
            std::fprintf(stderr, "[wotm] damage numbers unavailable: Monster::update or Monster::takeHit has no function in the runtime\n");
    }
    // Health and energy bars over enemies (mod, WOTM_ENEMYBARS=1): replacing Hud::drawHealthIcon's health icon. Hooks
    // always installed: without the variable, the bars only apply in 3- or 4-player fights (the multiplayer HUD)
    {
        if ((g_origDrawHealthIcon = runtime.lookupFunction(kDrawHealthIcon)) && (g_origAddPolyBars = runtime.lookupFunction(kAddPoly)))
        {
            runtime.registerFunction(kDrawHealthIcon, &drawHealthIconBars);
            runtime.registerFunction(kAddPoly, &addPolyBars);
            if ((g_origXform2dBars = runtime.lookupFunction(kXform2d)))
                runtime.registerFunction(kXform2d, &xform2dBars);
            if (wotm::settings::on(wotm::settings::EnemyBars))
                std::fprintf(stderr, "[wotm] health and energy bars over enemies (WOTM_ENEMYBARS)\n");
        }
        else
            std::fprintf(stderr, "[wotm] bars over enemies unavailable: Hud::drawHealthIcon or ScreenPolys::addPoly has no function in the runtime\n");
    }
    extern void wotmVu1CheckInstall(PS2Runtime &);   // wotm_vu1check.inc (WOTM_VU1CHECK: VU1 vs native per object)
    wotmVu1CheckInstall(runtime);
    extern void wotmVu0UcodeInstall(PS2Runtime &);   // wotm_live.inc: VU0 microcode without VIF0 in the game executable
    wotmVu0UcodeInstall(runtime);
    wotmSkinsInstall(runtime);   // last: wraps the SetPlayerMonster/SetAIMonster hooks (split4, WOTM_AIVSAI)
    wotmCustomInstall(runtime);  // after the skins: wraps their SetAIMonster
    wotmTrainingInstall(runtime);   // last: the outermost hook on update/takeHit/takeDamage (sees the damage the cheats let through)
    wotmTrigCheckInstall(runtime);
    if (g_minRate > 0)
        std::fprintf(stderr, "[wotm] updateRate >= %d (dt <= 1/%d) for the whole game (WOTM_RATEMIN)\n", g_minRate, g_minRate);
    else
        std::fprintf(stderr, "[wotm] updateRate unclamped (WOTM_RATEMIN=0)\n");
}
