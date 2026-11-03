// Elimination and minigames with 3 and 4 players (NATIVO.md, "Elimination and minigames with 3 and 4"). Included by wotm_overrides.cpp after
// wotm_split4.inc (quadrant screen, load and pads for players 3 and 4), whose hooks these wrap. Acts only with 3 or 4
// players in the Shell (m_numPlayers), set by the multiplayer menu (wotm_padsel.inc); with 1 or 2 everything is retail.
//
// Elimination (mode 6). Retail keeps continues and wins in 2-entry fields (Shell::m_continues[2], m_wins[2]) and reads the player's
// by m_index (0 = player 1, anything else = player 2). Here each player has their own (g_cont, g_wins), and whoever runs out of continues is out
// (g_out) while more than one is left in the fight:
//   - StateDeath::transitionInto: retail, with the dead player's continues at 0, joins the screen on the victim and puts the killer in victory (end
//     of the match). During the call both Shell continues read 0 only when the dead player has no continues and one player is left; otherwise
//     1 (the fight goes on). The death credits a win to the killer if it is a player (retail's +1 on m_winsThisGame becomes g_wins).
//   - StateVictory::transitionOK: the victory pose only counts once the others are out (Shell continues as above).
//   - TheGame::gameResolveLifeAndDeath: a dead player (m_unkF6) with continues goes back to the Shell (rtReturnToShell 1, like retail); with no
//     continues and more than one left in the fight, they're out (monster hidden, their camera switches to following someone still in); the last one goes back to the Shell
//     once the killer has finished the victory.
//   - EvaluateMultiPlayerBattleStatusNoAI: each dead player with continues spends one and picks another monster in retail dialog 8
//     (selectNextMultPlayer, with the pad from m_won[1]); then SetPlayerMonster and playerInit like retail. One left: dialog 0xB
//     ("PLAYER N WINS", m_survivor); none: dialog 6. displayDialog is modal (runs frames and yields to the scheduler): it is called as a
//     tail call with ra pointing at a trampoline (kTramp, an address the game never executes) that does the rest and returns to the real ra, both
//     when the dialog returns directly and when the thread was resumed by the dispatcher.
//   - finishedMultPlayElim ("play again"): retail puts players 1 and 2 back on their starting monsters (Shell+0x2B84/0x2B88) and
//     marks them used (resetSelecteds); those of players 3 and 4 come from g_start. InitPlayers resets the state (continues = m_elimination).
//
// Dodgeball (mode 7, quadrant screen): retail counts each player's hits at the start of DodgeBallLevel (2 ints indexed by
// m_index; the 3rd knocks down). Players 3 and 4 would write into the ball's counter: their hits are kept in g_hits (takeDamage with the
// counter and m_index swapped during the call). update(view) for views 2 and 3 only writes "P3"/"P4" (the rest of update already
// runs in views 0 and 1). The ending stays retail's: the third hit knocks down and the hitter wins.
//
// Crush (mode 9, one view): a 4-player scoreboard (P3 and P4 at the top), winner by the highest total of the 4 TokenManagers (TheGame has 4), a tie at
// the top goes to sudden death with everyone. The crush camera frames a pair: during crushUpdate the pair is the two players farthest apart.
//
// Big Shot (mode 8, turn-based): BigShotLevel has two slots (monster +0x6C/+0x70, points +0x9C/+0xA0, name +0x74/+0x84) and the
// turn alternates between them (swapPlayers). Here the players rotate: at the end of each turn the incoming slot gets the next
// player and their points; there are 3 turns per player (retail ends on the 6th) and on the last the slots get the top two, which
// retail compares (tie: the "king of the hill" between them). initForReplay runs once per extra player in slot 2 (hides and prepares
// them like player 2) and the 4-player scoreboard replaces "P1"/"P2".
//
// The minigame end dialogs (finishedDodgeBall, finishedCrush, finishedBigShot) get the "Player 3/4 - scored" lines.

namespace wotm_modes4
{
using wotm_split4::kGame;
using wotm_split4::kShell;
using wotm_split4::rdF;
using wotm_split4::wrF;

constexpr uint32_t kMonsters = kGame + 0x120380, kGameMode = kGame + 0x1203C8, kWon0 = kGame + 0x12043C, kWon1 = kGame + 0x120440;
constexpr uint32_t kNumPlayers = kShell + 0x2BB4, kMonsterSel = kShell + 0x2938, kContinues = kShell + 0x2B40, kElimination = kShell + 0x2B48,
                   kSurvivor = kShell + 0x2B58, kRestart = kShell + 0x2BA8, kInSession = kShell + 0x2BCC, kInMenus = kShell + 0x2BD0,
                   kElimStart = kShell + 0x2B84;
// Monster
constexpr uint32_t kCs = 0xC, kIndex = 0x24, kWins = 0x3C, kDead = 0xE8, kSudden = 0xEC, kOut = 0xF6, kVictorious = 0xF7, kF9 = 0xF9,
                   kHealth = 0x44C, kCrushCam = 0x6728, kIdleState = 0x7984, kVictoryState = 0x10E70;
constexpr uint32_t kStateOwner = 0xC, kDeathKiller = 0x1C;   // MonsterState: owner; StateDeath: killer
constexpr uint32_t kTokens = kGame + 0x11C370, kTokenStride = 0x1004;
constexpr uint32_t kSelectMode = wotm::sym::monsterSelectMode, kSelectCol = 0xB0;   // SelectSlot (column 0xB0, costume 0x2C): taken +0
constexpr uint32_t kDodge = wotm::sym::x_14DodgeBallLevel_instance, kCrush = wotm::sym::x_10CrushLevel_instance,
                   kBigShot = wotm::sym::x_12BigShotLevel_instance;
constexpr uint32_t kScratch = PS2_RAM_SIZE - 0x800;   // text for the font functions (8 x 0x80; split4's ViewDefs sit below)
constexpr uint32_t kTramp = wotm::fn::EvaluateOnlineBattleStatus__5Shelli + 4;   // mode 11 (online) never runs; +4 is the nop after jr ra

namespace fn = wotm::fn;

uint8_t rd8(uint8_t *rdram, uint32_t a) { return FAST_READ8(a & 0x1FFFFFFFu); }
uint32_t rd32(uint8_t *rdram, uint32_t a) { return FAST_READ32(a & 0x1FFFFFFFu); }
void wr32(uint8_t *rdram, uint32_t a, uint32_t v) { FAST_WRITE32(a & 0x1FFFFFFFu, v); }
void wr8(uint8_t *rdram, uint32_t a, uint8_t v) { FAST_WRITE8(a & 0x1FFFFFFFu, v); }
uint32_t monster(uint8_t *rdram, int i) { return rd32(rdram, kMonsters + 4u * uint32_t(i)) & 0x1FFFFFFFu; }

// Players right now (3 or 4; 0 = not applicable)
int players(uint8_t *rdram)
{
    const int32_t n = static_cast<int32_t>(rd32(rdram, kNumPlayers));
    return wotm_split4::g_players && n >= 3 && n <= 4 ? n : 0;
}
int gameMode(uint8_t *rdram) { return static_cast<int32_t>(rd32(rdram, kGameMode)); }
// Player (0..3) owning monster m, or -1
int playerOf(uint8_t *rdram, uint32_t m, int n)
{
    m &= 0x1FFFFFFFu;
    for (int i = 0; i < n && m; ++i)
        if (monster(rdram, i) == m)
            return i;
    return -1;
}

// Game function with up to 6 integer arguments (a0..a3, t0, t1); returns v0. The context is restored as it was.
uint32_t call(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, uint32_t addr, std::initializer_list<uint32_t> args)
{
    const PS2Runtime::RecompiledFunction f = runtime->lookupFunction(addr);
    if (!f)
        return 0;
    const R5900Context saved = *ctx;
    int r = 4;
    for (uint32_t a : args)
        ctx->r[r++] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a)));
    ctx->r[31] = _mm_set_epi64x(0, 0);
    ctx->pc = addr;
    f(rdram, ctx, runtime);
    const uint32_t v0 = GPR_U32(ctx, 2);
    *ctx = saved;
    return v0;
}

// Text through the game's font functions (native drawing picks it up like the HUD's): size of the minigame scoreboards
uint32_t scratchText(uint8_t *rdram, const char *s)
{
    static uint32_t next = 0;
    const uint32_t a = kScratch + 0x80u * (next++ & 7u);
    const size_t n = std::min<size_t>(std::strlen(s), 0x7F);
    std::memcpy(rdram + a, s, n);
    rdram[a + n] = 0;
    return a;
}
enum Align { Left, Right, Center };
void print(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int x, int y, Align al, const char *s)
{
    const uint32_t f = al == Left ? fn::fontSpritePrintXY__FiiiPc : al == Right ? fn::fontSpritePrintRightXY__FiiiPc : fn::fontSpritePrintCenteredXY__FiiiPc;
    call(rdram, ctx, runtime, f, {0u, uint32_t(x), uint32_t(y), scratchText(rdram, s)});
}
void scoreFont(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int r, int g, int b)
{
    call(rdram, ctx, runtime, fn::fontSetCharSizesInSubPixels__Fiiiii, {0u, 0x280u, 0x140u, uint32_t(-0x11), uint32_t(-0x15)});
    call(rdram, ctx, runtime, fn::fontSetColor__Fiiiii, {0u, uint32_t(r), uint32_t(g), uint32_t(b), 0x80u});
}
void defaultFont(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    call(rdram, ctx, runtime, fn::fontSetDefaultSize__Fi, {0u});
    call(rdram, ctx, runtime, fn::fontSetDefaultColor__Fi, {0u});
}
// Scoreboard colors: P1 red and P2 blue (retail's), P3 green, P4 yellow
constexpr int kScoreRgb[4][3] = {{0xFF, 0, 0}, {0, 0, 0xFF}, {0, 0xC0, 0}, {0xE0, 0xE0, 0}};
// "P<i+1> <value>" in player i's corner of a 640-wide view (P1/P3 on the left, P2/P4 on the right; 1 and 2 at the bottom like retail);
// dim: at half color (whoever is waiting for their turn in big shot)
void cornerScore(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int i, const char *value, int yTop, bool dim = false)
{
    char buf[48];
    std::snprintf(buf, sizeof buf, "P%d %s", i + 1, value);
    const int k = dim ? 2 : 1;
    scoreFont(rdram, ctx, runtime, kScoreRgb[i][0] / k, kScoreRgb[i][1] / k, kScoreRgb[i][2] / k);
    print(rdram, ctx, runtime, (i & 1) ? 0x271 : 0x20, i < 2 ? 0xC8 : yTop, (i & 1) ? Right : Left, buf);
}
// "Player N - scored X" lines for players 3 and 4 below retail's (minigame end dialogs)
void extraScoreLines(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int n, const int *score)
{
    call(rdram, ctx, runtime, fn::setTitleFont__Fv, {});
    for (int i = 2; i < n; ++i)
    {
        char buf[48];
        std::snprintf(buf, sizeof buf, "Player %d - scored %d", i + 1, score[i]);
        print(rdram, ctx, runtime, 0x140, 0xAA + 10 * i, Center, buf);
    }
}

// Hooks in general: the original saved at install time (wraps whatever was already registered)
struct Hook
{
    uint32_t addr;
    PS2Runtime::RecompiledFunction orig;
};
void callOrig(const Hook &h, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime) { h.orig(rdram, ctx, runtime); }
// The original of a hook with arguments, like call (without going through the hook itself)
void callOrigWith(const Hook &h, uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, std::initializer_list<uint32_t> args)
{
    const R5900Context saved = *ctx;
    int r = 4;
    for (uint32_t a : args)
        ctx->r[r++] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(a)));
    ctx->r[31] = _mm_set_epi64x(0, 0);
    ctx->pc = h.addr;
    h.orig(rdram, ctx, runtime);
    *ctx = saved;
}

// ------------------------------------------------------------------ elimination

int g_cont[4] = {}, g_wins[4] = {};
bool g_out[4] = {};
uint32_t g_start[4] = {};   // each player's starting monster ("play again" goes back to it)

bool elimActive(uint8_t *rdram) { return gameMode(rdram) == 6 && players(rdram); }

void refreshWins(uint8_t *rdram, int n)
{
    for (int i = 0; i < n; ++i)
        if (const uint32_t m = monster(rdram, i))
            wr32(rdram, m + kWins, uint32_t(g_wins[i]));
}

int monsterSlot(uint32_t sel)   // monsterIdtomonsterSlot (ShellFinished): selection column of each type
{
    switch (sel)
    {
    case 0x120: return 0;
    case 0x60: return 1;
    case 0xA0: return 2;
    case 0x20: return 3;
    case 0x40: return 4;
    case 0x140: return 5;
    case 0x80: return 6;
    case 0x100: return 7;
    case 0xE0: return 8;
    case 0x160: return 9;
    default: return 10;
    }
}

// The Shell continues (read by m_index in retail) set to `v` during a call
struct ShellContinues
{
    uint8_t *rdram;
    uint32_t saved[2];
    ShellContinues(uint8_t *r, uint32_t v) : rdram(r)
    {
        saved[0] = rd32(rdram, kContinues);
        saved[1] = rd32(rdram, kContinues + 4);
        wr32(rdram, kContinues, v);
        wr32(rdram, kContinues + 4, v);
    }
    ~ShellContinues()
    {
        wr32(rdram, kContinues, saved[0]);
        wr32(rdram, kContinues + 4, saved[1]);
    }
};

// How many are still in the fight besides `except`
int inFight(int n, int except)
{
    int k = 0;
    for (int j = 0; j < n; ++j)
        k += j != except && !g_out[j];
    return k;
}

// Player i out of the match: monster hidden, their camera follows someone still in
void eliminate(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int i, int n)
{
    g_out[i] = true;
    const uint32_t m = monster(rdram, i);
    if (m)
    {
        call(rdram, ctx, runtime, fn::hierSetCsDrawMe__FP3_csUc, {rd32(rdram, m + kCs), 0u});
        call(rdram, ctx, runtime, fn::setShadowOnOff__7Monsterb, {m, 0u});
    }
    for (int j = 0; j < n; ++j)
        if (!g_out[j] && monster(rdram, j))
        {
            call(rdram, ctx, runtime, fn::SetCameraToFollowMonster__7CamerasiP7Monster, {uint32_t(i), monster(rdram, j)});
            break;
        }
    std::fprintf(stderr, "[modes4] elimination: player %d out (%d left)\n", i + 1, inFight(n, -1));
}

Hook g_initPlayers{fn::InitPlayers__5Shell}, g_deathInto{fn::transitionInto__10StateDeath}, g_victoryOk{fn::transitionOK__12StateVictory},
    g_resolve{fn::gameResolveLifeAndDeath__7TheGame}, g_evalNoAI{fn::EvaluateMultiPlayerBattleStatusNoAI__5Shelli},
    g_finishedElim{fn::finishedMultPlayElim__FRi};

void initPlayersHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == g_initPlayers.addr && elimActive(rdram))
    {
        const int32_t c = std::max(0, static_cast<int32_t>(rd32(rdram, kElimination)));
        for (int i = 0; i < 4; ++i)
        {
            g_cont[i] = c;
            g_wins[i] = 0;
            g_out[i] = false;
        }
        std::fprintf(stderr, "[modes4] elimination with %d players, %d continues each\n", players(rdram), c);
    }
    callOrig(g_initPlayers, rdram, ctx, runtime);
}

void deathIntoHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t state = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
    const int n = ctx->pc == g_deathInto.addr && elimActive(rdram) ? players(rdram) : 0;
    const int victim = n ? playerOf(rdram, rd32(rdram, state + kStateOwner), n) : -1;
    if (victim < 0)
    {
        callOrig(g_deathInto, rdram, ctx, runtime);
        return;
    }
    const int killer = playerOf(rdram, rd32(rdram, state + kDeathKiller), n);
    const bool ends = g_cont[victim] == 0 && inFight(n, victim) <= 1;
    const uint32_t ra = GPR_U32(ctx, 31);
    {
        ShellContinues sc(rdram, ends ? 0u : 1u);
        callOrig(g_deathInto, rdram, ctx, runtime);
    }
    if (ctx->pc != ra)
        return;
    if (killer >= 0 && killer != victim)
        ++g_wins[killer];
    refreshWins(rdram, n);
}

void victoryOkHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t self = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
    const int n = ctx->pc == g_victoryOk.addr && elimActive(rdram) ? players(rdram) : 0;
    const int me = n ? playerOf(rdram, rd32(rdram, self + kStateOwner), n) : -1;
    if (me < 0)
    {
        callOrig(g_victoryOk, rdram, ctx, runtime);
        return;
    }
    bool done = true;   // the others are already out (or dead with no continues)
    for (int j = 0; j < n; ++j)
        if (j != me && !g_out[j])
            done = done && g_cont[j] == 0 && !(rdF(rdram, monster(rdram, j) + kHealth) > 0.f);
    ShellContinues sc(rdram, done ? 0u : 1u);
    callOrig(g_victoryOk, rdram, ctx, runtime);
}

void resolveHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc != g_resolve.addr || !elimActive(rdram))
    {
        callOrig(g_resolve, rdram, ctx, runtime);
        return;
    }
    const int n = players(rdram);
    const uint32_t game = GPR_U32(ctx, 4), ra = GPR_U32(ctx, 31);
    for (int i = 0; i < n; ++i)
    {
        const uint32_t m = monster(rdram, i);
        if (!m || g_out[i] || !rd8(rdram, m + kOut))
            continue;
        if (g_cont[i] > 0)
            call(rdram, ctx, runtime, fn::rtReturnToShell__Fii, {1u, 0x1Eu});
        else if (inFight(n, i) >= 2)
            eliminate(rdram, ctx, runtime, i, n);
        else
        {
            const uint32_t k = rd32(rdram, m + wotm_split4::kKiller) & 0x1FFFFFFFu;
            if (!k)
                call(rdram, ctx, runtime, fn::rtReturnToShell__Fii, {1u, 0x1Eu});
            else if (rd8(rdram, k + kVictorious))
            {
                call(rdram, ctx, runtime, fn::rtReturnToShell__Fii, {1u, 0x1Eu});
                call(rdram, ctx, runtime, fn::fadeOut__7TheGamei, {game, 4u});
            }
        }
    }
    ctx->pc = ra;
}

// selectNextMultPlayer looks for the first free monster (SelectSlot costume 0 with state 1 and taken 1) in an endless loop: with none
// free (match without the menu, or all already used) the 10 elimination monsters become free again, except the ones the others are using.
void ensurePool(uint8_t *rdram, int who, int n)
{
    for (uint32_t c = 0; c <= 10; ++c)
    {
        const uint32_t s = kSelectMode + kSelectCol * c;
        if (rd32(rdram, s + 4) == 1u && rd32(rdram, s) == 1u)
            return;
    }
    for (uint32_t c = 0; c < 10; ++c)
    {
        wr32(rdram, kSelectMode + kSelectCol * c + 4, 1u);
        wr32(rdram, kSelectMode + kSelectCol * c, 1u);
    }
    for (int j = 0; j < n; ++j)
        if (j != who && !g_out[j])
            if (const int c = monsterSlot(rd32(rdram, kMonsterSel + 4u * uint32_t(j))); c < 10)
                wr32(rdram, kSelectMode + kSelectCol * uint32_t(c), 0u);
    std::fprintf(stderr, "[modes4] elimination: monsters free again for dialog 8\n");
}

// Queue of players picking a new monster (dialog 8), one at a time
Hook g_playerInit{fn::playerInit__7Monster};   // only read (not a hook)
int g_queue[4] = {}, g_queued = 0, g_cur = -1;
uint32_t g_contRa = 0;

void elimAfterDialog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int i)
{
    if (!rd32(rdram, kInSession))   // "quit" in the dialog
        return;
    const uint32_t sel = rd32(rdram, kMonsterSel + 4u * uint32_t(i));
    call(rdram, ctx, runtime, fn::SetPlayerMonster__7TheGameiiiii, {kGame, uint32_t(i), sel, 0u, uint32_t(i), i == 0 ? rd32(rdram, kShell + 0x2BE0) : 0u});
    // the game's playerInit, saved at install time: WOTM_AIVSAI's (aiInit) left the monster swapped mid-fight without state
    if (const uint32_t m = monster(rdram, i))
    {
        callOrigWith(g_playerInit, rdram, ctx, runtime, {m});
        wr32(rdram, m + 0x672C, 0);   // type A control scheme (the game's, for players 3 and 4, came from m_vibration; wotm_pausemenu.inc)
    }
    refreshWins(rdram, players(rdram));
    std::fprintf(stderr, "[modes4] elimination: player %d returns with monster %x (%d continues)\n", i + 1, sel, g_cont[i]);
}

void elimNext(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    while (g_queued > 0 && rd32(rdram, kInSession))
    {
        const int i = g_queue[0];
        for (int k = 1; k < g_queued; ++k)
            g_queue[k - 1] = g_queue[k];
        --g_queued;
        g_cur = i;
        --g_cont[i];
        ensurePool(rdram, i, players(rdram));
        wr32(rdram, kWon1, uint32_t(i));   // selectNextMultPlayer reads the pad and the "Player N" from here
        ctx->r[4] = _mm_set_epi64x(0, 8);
        ctx->r[31] = _mm_set_epi64x(0, kTramp);
        ctx->pc = fn::displayDialog__Fi;
        runtime->lookupFunction(fn::displayDialog__Fi)(rdram, ctx, runtime);
        if (ctx->pc != kTramp)
            return;   // the thread yielded in the middle of the dialog: the trampoline continues from here when it finishes
        g_cur = -1;
        elimAfterDialog(rdram, ctx, runtime, i);
    }
    g_queued = 0;
    g_cur = -1;
    wr32(rdram, kGame + 0x120460, 1u);   // like retail at the end of mode 6
    wr32(rdram, kGame + 0x120458, 0u);
    ctx->pc = g_contRa;
}

void trampoline(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (g_cur >= 0)
    {
        const int i = g_cur;
        g_cur = -1;
        elimAfterDialog(rdram, ctx, runtime, i);
    }
    elimNext(rdram, ctx, runtime);
}

// Modal dialog as a tail call (returns to the ra of whoever called Evaluate)
void tailDialog(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int which, uint32_t ra)
{
    ctx->r[4] = _mm_set_epi64x(0, which);
    ctx->r[31] = _mm_set_epi64x(0, ra);
    ctx->pc = fn::displayDialog__Fi;
    runtime->lookupFunction(fn::displayDialog__Fi)(rdram, ctx, runtime);
}

void evalNoAIHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc != g_evalNoAI.addr || !elimActive(rdram))
    {
        callOrig(g_evalNoAI, rdram, ctx, runtime);
        return;
    }
    const int n = players(rdram);
    const int32_t r = static_cast<int32_t>(GPR_U32(ctx, 5));
    const uint32_t ra = GPR_U32(ctx, 31);
    ctx->pc = ra;
    if (r == 3)
    {
        wr32(rdram, kInMenus, 0u);
        wr32(rdram, kInSession, 0u);
        return;
    }
    if (r != 1)
        return;
    // dead players with no continues are out; the other dead ones come back
    g_queued = 0;
    for (int i = 0; i < n; ++i)
    {
        const uint32_t m = monster(rdram, i);
        if (!m || g_out[i] || rdF(rdram, m + kHealth) > 0.f)
            continue;
        if (g_cont[i] == 0)
            eliminate(rdram, ctx, runtime, i, n);
        else
            g_queue[g_queued++] = i;
    }
    int survivor = -1, left = 0;
    for (int i = 0; i < n; ++i)
        if (!g_out[i])
        {
            survivor = i;
            ++left;
        }
    if (left <= 1)
    {
        g_queued = 0;
        std::fprintf(stderr, "[modes4] elimination: over, %s\n", left ? "one player left" : "nobody left");
        if (left == 1)
            wr32(rdram, kSurvivor, uint32_t(survivor));
        tailDialog(rdram, ctx, runtime, left == 1 ? 0xB : 6, ra);
        return;
    }
    if (!g_queued)
        return;
    g_contRa = ra;
    elimNext(rdram, ctx, runtime);
}

// "play again" from dialog 0xB: players 3 and 4 also go back to their starting monster, marked as used
void finishedElimHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = ctx->pc == g_finishedElim.addr ? GPR_U32(ctx, 31) : 0u;
    callOrig(g_finishedElim, rdram, ctx, runtime);
    const int n = players(rdram);
    if (!ra || ctx->pc != ra || !n || GPR_U32(ctx, 2) != 1u || rd32(rdram, kRestart) != 1u || rd32(rdram, kInSession) != 0u)
        return;
    for (int i = 2; i < n; ++i)
        if (g_start[i])
        {
            wr32(rdram, kMonsterSel + 4u * uint32_t(i), g_start[i]);
            wr32(rdram, kSelectMode + kSelectCol * uint32_t(monsterSlot(g_start[i])), 0u);
        }
    std::fprintf(stderr, "[modes4] elimination: play again with the starting monsters\n");
}

// ------------------------------------------------------------------ dodgeball

int g_hits[4] = {};
Hook g_takeDamage{fn::takeDamage__7MonsterfbP7Monster}, g_dodgeUpdate{fn::update__14DodgeBallLeveli},
    g_dodgeInit{fn::initAfterDbLoad__14DodgeBallLevel}, g_finishedDodge{fn::finishedDodgeBall__FRi};

void takeDamageHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t src = GPR_U32(ctx, 5) & 0x1FFFFFFFu;
    const int n = ctx->pc == g_takeDamage.addr && gameMode(rdram) == 7 && src ? players(rdram) : 0;
    const int32_t idx = n ? static_cast<int32_t>(rd32(rdram, src + kIndex)) : 0;
    if (idx < 2 || idx >= n)
    {
        callOrig(g_takeDamage, rdram, ctx, runtime);
        return;
    }
    // player 1's counter (start of DodgeBallLevel) holds this player's hits and m_index is 0 during the call
    const uint32_t ra = GPR_U32(ctx, 31), hits0 = rd32(rdram, kDodge);
    wr32(rdram, kDodge, uint32_t(g_hits[idx]));
    wr32(rdram, src + kIndex, 0u);
    callOrig(g_takeDamage, rdram, ctx, runtime);
    if (ctx->pc != ra)
        std::fprintf(stderr, "[modes4] dodgeball: takeDamage didn't return to the caller\n");
    g_hits[idx] = static_cast<int32_t>(rd32(rdram, kDodge));
    wr32(rdram, kDodge, hits0);
    wr32(rdram, src + kIndex, uint32_t(idx));
}

void dodgeUpdateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t view = GPR_U32(ctx, 5);
    if (ctx->pc != g_dodgeUpdate.addr || view < 2u || !players(rdram))
    {
        callOrig(g_dodgeUpdate, rdram, ctx, runtime);
        return;
    }
    ctx->pc = GPR_U32(ctx, 31);
    if (int(view) >= players(rdram))
        return;
    // like retail in views 0 and 1: P3 on the left, P4 on the right (x in a 320-wide view)
    char buf[24];
    std::snprintf(buf, sizeof buf, "P%u %d", view + 1u, g_hits[view]);
    scoreFont(rdram, ctx, runtime, kScoreRgb[view][0], kScoreRgb[view][1], kScoreRgb[view][2]);
    print(rdram, ctx, runtime, (view & 1u) ? 0x12C : 0x20, 0xC8, (view & 1u) ? Right : Left, buf);
    defaultFont(rdram, ctx, runtime);
}

void dodgeInitHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = ctx->pc == g_dodgeInit.addr ? GPR_U32(ctx, 31) : 0u;
    callOrig(g_dodgeInit, rdram, ctx, runtime);
    const int n = players(rdram);
    if (!ra || ctx->pc != ra || !n)
        return;
    for (int i = 0; i < 4; ++i)
        g_hits[i] = 0;
    for (int i = 2; i < n; ++i)
        if (const uint32_t m = monster(rdram, i))
            call(rdram, ctx, runtime, fn::drainSpecial__7Monster, {m});
}

void finishedDodgeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int n = players(rdram);
    callOrig(g_finishedDodge, rdram, ctx, runtime);
    if (n)
    {
        const R5900Context saved = *ctx;
        extraScoreLines(rdram, ctx, runtime, n, g_hits);
        *ctx = saved;
    }
}

// ------------------------------------------------------------------ crush

int g_crushTotal[4] = {};
int g_crushLoser = -1;
Hook g_crushUpdate{fn::updateNormal__10CrushLevel}, g_crushInit{fn::initAfterDbLoad__10CrushLevel},
    g_crushSudden{fn::initSuddenDeath__10CrushLevel}, g_crushCam{fn::crushUpdate__6Camera}, g_finishedCrush{fn::finishedCrush__FRi};

int tokens(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int i)
{
    return static_cast<int32_t>(call(rdram, ctx, runtime, fn::grandTotal__12TokenManager, {kTokens + kTokenStride * uint32_t(i)}));
}

void winsText(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime, int winner)
{
    const uint32_t m = monster(rdram, winner);
    const uint32_t type = m ? (rd32(rdram, m + 0x14) >> 5) : 0u;
    char name[16] = {};
    if (type < 17)
        std::memcpy(name, rdram + wotm::sym::MonsterNewNames_006E8D90 + 12u * type, 12);
    char buf[40];
    std::snprintf(buf, sizeof buf, "%s WINS\n", name);
    call(rdram, ctx, runtime, fn::fontSetColor__Fiiiii, {0u, 0x5Au, 0x5Au, 0x5Au, 0x80u});
    call(rdram, ctx, runtime, fn::fontSetCharSizesInSubPixels__Fiiiii, {0u, 0x280u, 0x140u, uint32_t(-0x11), uint32_t(-0x15)});
    print(rdram, ctx, runtime, 0x140, 0xE, Center, buf);
    defaultFont(rdram, ctx, runtime);
}

// CrushLevel::updateNormal (before sudden death) with 3 and 4: the original draws the time and P1/P2 while there is time left; the ending is ours
void crushUpdateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int n = ctx->pc == g_crushUpdate.addr ? players(rdram) : 0;
    if (!n)
    {
        callOrig(g_crushUpdate, rdram, ctx, runtime);
        return;
    }
    const uint32_t ra = GPR_U32(ctx, 31);
    const int32_t timer = static_cast<int32_t>(rd32(rdram, kCrush)), finished = static_cast<int32_t>(rd32(rdram, kCrush + 4));
    const int32_t dt = static_cast<int32_t>(call(rdram, ctx, runtime, fn::timerGetFieldsLastFrame__Fv, {}));
    if (!finished && timer - dt > 0)
    {
        callOrig(g_crushUpdate, rdram, ctx, runtime);
        if (ctx->pc != ra)
            return;
        for (int i = 2; i < n; ++i)
        {
            char v[16];
            std::snprintf(v, sizeof v, "%d", tokens(rdram, ctx, runtime, i));
            cornerScore(rdram, ctx, runtime, i, v, 0x1E);
        }
        defaultFont(rdram, ctx, runtime);
        return;
    }
    ctx->pc = ra;
    wr32(rdram, kCrush, uint32_t(timer - dt));
    if (!finished)
    {
        int best = 0, worst = 0, ties = 0;
        for (int i = 0; i < n; ++i)
        {
            g_crushTotal[i] = tokens(rdram, ctx, runtime, i);
            if (g_crushTotal[i] > g_crushTotal[best])
                best = i;
            if (g_crushTotal[i] < g_crushTotal[worst])
                worst = i;
        }
        for (int i = 0; i < n; ++i)
            ties += g_crushTotal[i] == g_crushTotal[best];
        wr32(rdram, kCrush + 8, uint32_t(g_crushTotal[0]));   // retail's m_total (the end dialog shows P1 and P2 from here)
        wr32(rdram, kCrush + 0xC, uint32_t(g_crushTotal[1]));
        std::fprintf(stderr, "[modes4] crush: time up, %d/%d/%d/%d\n", g_crushTotal[0], g_crushTotal[1], g_crushTotal[2],
                     n > 3 ? g_crushTotal[3] : 0);
        if (ties > 1)
        {
            call(rdram, ctx, runtime, fn::initSuddenDeath__10CrushLevel, {kCrush});
            return;
        }
        for (int i = 0; i < n; ++i)
            if (i != best)
                wr8(rdram, monster(rdram, i) + kF9, 0);
        const uint32_t w = monster(rdram, best);
        wr8(rdram, w + kVictorious, 0);
        call(rdram, ctx, runtime, fn::enterNewState__7MonsterP12MonsterState, {w, w + kVictoryState});
        wr32(rdram, kWon0, uint32_t(best));
        wr32(rdram, kWon1, uint32_t(worst));
        g_crushLoser = worst;
        wr32(rdram, kCrush + 4, 1u);
    }
    winsText(rdram, ctx, runtime, static_cast<int32_t>(rd32(rdram, kWon0)));
}

void crushInitHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = ctx->pc == g_crushInit.addr ? GPR_U32(ctx, 31) : 0u;
    callOrig(g_crushInit, rdram, ctx, runtime);
    const int n = players(rdram);
    if (!ra || ctx->pc != ra || !n)
        return;
    g_crushLoser = -1;
    for (int i = 2; i < n; ++i)
        if (const uint32_t m = monster(rdram, i))
        {
            wr32(rdram, m + kCrushCam, 1u);   // as InitCrushMonsters does with the two
            call(rdram, ctx, runtime, fn::drainSpecial__7Monster, {m});
        }
}

void crushSuddenHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t ra = ctx->pc == g_crushSudden.addr ? GPR_U32(ctx, 31) : 0u;
    callOrig(g_crushSudden, rdram, ctx, runtime);
    const int n = players(rdram);
    if (!ra || ctx->pc != ra || !n)
        return;
    for (int i = 2; i < n; ++i)
        if (const uint32_t m = monster(rdram, i))
        {
            wr8(rdram, m + kSudden, 1);
            call(rdram, ctx, runtime, fn::drainSpecial__7Monster, {m});
        }
}

// Camera::crushUpdate frames the pair at +0xE0C/+0xE10: with 3 and 4, the two players farthest apart during the call
void crushCamHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t cam = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
    const int n = ctx->pc == g_crushCam.addr ? players(rdram) : 0;
    if (!n || gameMode(rdram) != 9)
    {
        callOrig(g_crushCam, rdram, ctx, runtime);
        return;
    }
    int a = 0, b = 1;
    float far = -1.f;
    for (int i = 0; i < n; ++i)
        for (int j = i + 1; j < n; ++j)
        {
            const uint32_t ci = rd32(rdram, monster(rdram, i) + kCs) & 0x1FFFFFFFu, cj = rd32(rdram, monster(rdram, j) + kCs) & 0x1FFFFFFFu;
            if (ci < 0x1000u || cj < 0x1000u)
                continue;
            const float dx = rdF(rdram, ci + 0x10) - rdF(rdram, cj + 0x10), dy = rdF(rdram, ci + 0x14) - rdF(rdram, cj + 0x14),
                        dz = rdF(rdram, ci + 0x18) - rdF(rdram, cj + 0x18);
            const float d = dx * dx + dy * dy + dz * dz;
            if (d > far)
            {
                far = d;
                a = i;
                b = j;
            }
        }
    const uint32_t pa = rd32(rdram, cam + 0xE0C), pb = rd32(rdram, cam + 0xE10), ra = GPR_U32(ctx, 31);
    wr32(rdram, cam + 0xE0C, monster(rdram, a));
    wr32(rdram, cam + 0xE10, monster(rdram, b));
    callOrig(g_crushCam, rdram, ctx, runtime);
    if (ctx->pc != ra)
        return;
    wr32(rdram, cam + 0xE0C, pa);
    wr32(rdram, cam + 0xE10, pb);
}

void finishedCrushHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int n = players(rdram);
    if (n && g_crushLoser < 0)   // sudden death: whoever fell (gameResolveLifeAndDeath sets "the other of player 1")
        for (int i = 0; i < n && g_crushLoser < 0; ++i)
            if (const uint32_t m = monster(rdram, i); m && !(rdF(rdram, m + kHealth) > 0.f))
                g_crushLoser = i;
    if (n && g_crushLoser >= 0)
        wr32(rdram, kWon1, uint32_t(g_crushLoser));
    callOrig(g_finishedCrush, rdram, ctx, runtime);
    if (n)
    {
        const R5900Context saved = *ctx;
        extraScoreLines(rdram, ctx, runtime, n, g_crushTotal);
        *ctx = saved;
    }
}

// ------------------------------------------------------------------ big shot

constexpr uint32_t kBsSlot = kBigShot + 0x6C, kBsName = kBigShot + 0x74, kBsTurn = kBigShot + 0x94, kBsScore = kBigShot + 0x9C,
                   kBsPoints = kBigShot + 0xA4, kBsEnd = kBigShot + 0xC4, kBsWinnerSet = kBigShot + 0xC8, kBsThrows = kBigShot + 0xD0,
                   kBsTimer = kBigShot + 0xD4, kBsFade = kBigShot + 0xDC, kBsState = kBigShot + 0xE8;
constexpr int kBsTurnsEach = 3;   // retail ends on the 6th turn (3 each)
int g_bsOcc[2] = {0, 1}, g_bsNext = 1, g_bsTurns = 0;
float g_bsScore[4] = {};
int g_bsWinner = -1, g_bsLoser = -1;
uint8_t g_bsWait[16] = {};                          // waiting position (player 2's after initForReplay)
constexpr uint32_t kBsVec = PS2_RAM_SIZE - 0x3E0;   // _fvector for setTrans (split4's is at -0x400)
Hook g_bsReplay{fn::initForReplay__12BigShotLevel}, g_bsTakeTurns{fn::takeTurns__12BigShotLevel}, g_bsUpdate{fn::updateNormal__12BigShotLevel},
    g_finishedBs{fn::finishedBigShot__FRi};

void bsSetSlot(uint8_t *rdram, int slot, int player, float score)
{
    g_bsOcc[slot] = player;
    const uint32_t m = monster(rdram, player);
    wr32(rdram, kBsSlot + 4u * uint32_t(slot), m);
    wrF(rdram, kBsScore + 4u * uint32_t(slot), score);
    const uint32_t type = m ? (rd32(rdram, m + 0x14) >> 5) : 0u;   // name (MonsterNewNames, like initAfterDbLoad)
    std::memset(rdram + kBsName + 0x10u * uint32_t(slot), 0, 0x10);
    if (type < 17)
        std::memcpy(rdram + kBsName + 0x10u * uint32_t(slot), rdram + wotm::sym::MonsterNewNames_006E8958 + 12u * type, 12);
}

void bsReplayHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int n = ctx->pc == g_bsReplay.addr ? players(rdram) : 0;
    if (!n)
    {
        callOrig(g_bsReplay, rdram, ctx, runtime);
        return;
    }
    // the original prepares slot 1 hidden: once with each extra player, the last one (the one that stays) with player 2
    const uint32_t self = GPR_U32(ctx, 4);
    for (int i = 0; i < n; ++i)
        if (const uint32_t m = monster(rdram, i))
            call(rdram, ctx, runtime, fn::hierSetCsDrawMe__FP3_csUc, {rd32(rdram, m + kCs), 1u});
    for (int i = 2; i < n; ++i)
    {
        wr32(rdram, kBsSlot + 4, monster(rdram, i));
        callOrigWith(g_bsReplay, rdram, ctx, runtime, {self});
    }
    wr32(rdram, kBsSlot + 4, monster(rdram, 1));
    callOrig(g_bsReplay, rdram, ctx, runtime);
    if (const uint32_t m = monster(rdram, 1), cs = m ? rd32(rdram, m + kCs) & 0x1FFFFFFFu : 0u; cs >= 0x1000u)
        std::memcpy(g_bsWait, rdram + cs + 0x10, 16);   // where the original leaves whoever is waiting for their turn
    g_bsOcc[0] = 0;
    g_bsOcc[1] = 1;
    g_bsNext = 1;
    g_bsTurns = 0;
    for (float &s : g_bsScore)
        s = 0.f;
    g_bsWinner = g_bsLoser = -1;
    std::fprintf(stderr, "[modes4] big shot with %d players: %d turns each\n", n, kBsTurnsEach);
}

void bsTakeTurnsHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int n = ctx->pc == g_bsTakeTurns.addr ? players(rdram) : 0;
    if (!n || static_cast<int32_t>(rd32(rdram, kBsTimer)) != 1 || rd32(rdram, kBsState) != 0u)
    {
        callOrig(g_bsTakeTurns, rdram, ctx, runtime);
        return;
    }
    // end of the turn on this frame (the original swaps the slots in swapPlayers, or compares on the last one)
    const int t = rd32(rdram, kBsTurn) ? 1 : 0, p = g_bsOcc[t];
    const bool last = ++g_bsTurns >= kBsTurnsEach * n;
    if (!last)
    {
        const int q = g_bsNext;
        g_bsNext = (g_bsNext + 1) % n;
        bsSetSlot(rdram, t ^ 1, q, g_bsScore[q]);
        wr32(rdram, kBsThrows, 0u);   // only the last turn has 6 (the end in the original)
        const uint32_t ra = GPR_U32(ctx, 31);
        callOrig(g_bsTakeTurns, rdram, ctx, runtime);
        if (ctx->pc == ra)
            g_bsScore[p] = rdF(rdram, kBsScore + 4u * uint32_t(t));
        return;
    }
    // last turn: this frame's point (the original adds it with points still counting and an odd timer) and the top two in the slots
    g_bsScore[p] = rdF(rdram, kBsScore + 4u * uint32_t(t)) + (rdF(rdram, kBsPoints) > 0.f ? 1.f : 0.f);
    wrF(rdram, kBsPoints, 0.f);
    int order[4] = {0, 1, 2, 3};
    std::stable_sort(order, order + n, [](int a, int b) { return g_bsScore[a] > g_bsScore[b]; });
    bsSetSlot(rdram, 0, order[0], g_bsScore[order[0]]);
    bsSetSlot(rdram, 1, order[1], g_bsScore[order[1]]);
    g_bsLoser = order[n - 1];
    // out of the contest: at the waiting spot, idle and hidden (the last to play kept falling, and a fall in "king of the hill"
    // counts as a loss: it decided the sudden death)
    for (int i = 0; i < n; ++i)
        if (i != order[0] && i != order[1])
            if (const uint32_t m = monster(rdram, i))
            {
                std::memcpy(rdram + kBsVec, g_bsWait, 16);
                call(rdram, ctx, runtime, fn::setTrans__7MonsterR8_fvector, {m, kBsVec});
                call(rdram, ctx, runtime, fn::enterNewState__7MonsterP12MonsterState, {m, m + kIdleState});
                call(rdram, ctx, runtime, fn::setShadowOnOff__7Monsterb, {m, 0u});
                wr8(rdram, m + kF9, 0);
                call(rdram, ctx, runtime, fn::hierSetCsDrawMe__FP3_csUc, {rd32(rdram, m + kCs), 0u});
            }
    wr32(rdram, kBsThrows, 6u);
    std::fprintf(stderr, "[modes4] big shot: over, %.0f/%.0f/%.0f/%.0f\n", g_bsScore[0], g_bsScore[1], g_bsScore[2], n > 3 ? g_bsScore[3] : 0.f);
    callOrig(g_bsTakeTurns, rdram, ctx, runtime);
}

void bsUpdateHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int n = ctx->pc == g_bsUpdate.addr ? players(rdram) : 0;
    if (!n)
    {
        callOrig(g_bsUpdate, rdram, ctx, runtime);
        return;
    }
    const uint32_t ra = GPR_U32(ctx, 31);
    const bool playing = rd32(rdram, kBsEnd) == 0u, full = rd32(rdram, kBsFade) == 0x80u, decided = rd32(rdram, kBsWinnerSet) != 0u;
    if (playing && full)
        wr32(rdram, kBsFade, 0x7Fu);   // without the original's "P1"/"P2" (with the timer at 0 it redoes the fade at 1/128 and goes back to 0x80)
    callOrig(g_bsUpdate, rdram, ctx, runtime);
    if (ctx->pc != ra)
        return;
    if (playing && full && rd32(rdram, kBsFade) == 0x7Fu)
        wr32(rdram, kBsFade, 0x80u);
    if (!decided && rd32(rdram, kBsWinnerSet) != 0u)   // the original picked the winning slot (m_won by slot)
    {
        g_bsWinner = g_bsOcc[rd32(rdram, kWon0) ? 1 : 0];
        if (g_bsLoser == g_bsWinner || g_bsLoser < 0)
            g_bsLoser = g_bsOcc[rd32(rdram, kWon0) ? 0 : 1];
    }
    if (!playing || !full)
        return;
    const int active = g_bsOcc[rd32(rdram, kBsTurn) ? 1 : 0];
    for (int i = 0; i < n; ++i)
    {
        float s = g_bsScore[i];
        for (int k = 0; k < 2; ++k)
            if (g_bsOcc[k] == i)
                s = rdF(rdram, kBsScore + 4u * uint32_t(k));
        char v[24];
        std::snprintf(v, sizeof v, "%.0f", s);
        cornerScore(rdram, ctx, runtime, i, v, 0xB4, i != active);
    }
    defaultFont(rdram, ctx, runtime);
}

void finishedBsHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const int n = players(rdram);
    if (n && g_bsWinner >= 0)   // ended on points: m_won and the P1/P2 points by player (sudden death already sets them that way)
    {
        wr32(rdram, kWon0, uint32_t(g_bsWinner));
        wr32(rdram, kWon1, uint32_t(g_bsLoser));
        wrF(rdram, kBsScore, g_bsScore[0]);
        wrF(rdram, kBsScore + 4, g_bsScore[1]);
    }
    callOrig(g_finishedBs, rdram, ctx, runtime);
    if (n)
    {
        int s[4] = {};
        for (int i = 0; i < 4; ++i)
            s[i] = int(g_bsScore[i] + 0.5f);
        const R5900Context saved = *ctx;
        extraScoreLines(rdram, ctx, runtime, n, s);
        *ctx = saved;
    }
}

// ------------------------------------------------------------------ install

void install(PS2Runtime &runtime)
{
    if (!wotm_split4::g_players)
        return;
    struct H { Hook *h; PS2Runtime::RecompiledFunction fn; };
    const H hooks[] = {
        {&g_initPlayers, &initPlayersHook},   {&g_deathInto, &deathIntoHook},     {&g_victoryOk, &victoryOkHook},
        {&g_resolve, &resolveHook},           {&g_evalNoAI, &evalNoAIHook},       {&g_finishedElim, &finishedElimHook},
        {&g_takeDamage, &takeDamageHook},     {&g_dodgeUpdate, &dodgeUpdateHook}, {&g_dodgeInit, &dodgeInitHook},
        {&g_finishedDodge, &finishedDodgeHook}, {&g_crushUpdate, &crushUpdateHook}, {&g_crushInit, &crushInitHook},
        {&g_crushSudden, &crushSuddenHook},   {&g_crushCam, &crushCamHook},       {&g_finishedCrush, &finishedCrushHook},
        {&g_bsReplay, &bsReplayHook},         {&g_bsTakeTurns, &bsTakeTurnsHook}, {&g_bsUpdate, &bsUpdateHook},
        {&g_finishedBs, &finishedBsHook},
    };
    for (const H &h : hooks)
        if (!(h.h->orig = runtime.lookupFunction(h.h->addr)))
        {
            std::fprintf(stderr, "[modes4] off: game function %x not found in the runtime\n", h.h->addr);
            return;
        }
    if (!(g_playerInit.orig = runtime.lookupFunction(g_playerInit.addr)))
        return;
    for (const H &h : hooks)
        runtime.registerFunction(h.h->addr, h.fn);
    runtime.registerFunction(kTramp, &trampoline);
    std::fprintf(stderr, "[modes4] elimination and minigames for 3 and 4 players\n");
}
} // namespace wotm_modes4

// Elimination monster selection (wotm_padsel.inc): each player's starting monster ("play again" goes back to it)
void wotmElimSetStart(int player, uint32_t sel)
{
    if (player >= 0 && player < 4)
        wotm_modes4::g_start[player] = sel;
}
