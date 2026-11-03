// Training mode (NATIVO.md, "Runtime: training mode"): state shared between the EE thread (hooks in native/runtime/wotm_training.inc,
// included by wotm_overrides.cpp; volumes captured in wotmCaptureFrame in wotm_live.inc) and the window (drawn in
// wotm_live.inc). Guide in docs/passos/NATIVO.md, "Runtime: training mode"; investigations in git show 635fd3c:docs/investigacoes/2026-10-23/treino_*.md.
#pragma once
#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <vector>

namespace wotm_training
{
// options (the window writes, the EE thread reads)
constexpr uint32_t kShowAttacks = 1u << 0;   // spheres tested by attacks
constexpr uint32_t kShowHulls = 1u << 1;     // body collision hulls (what attacks hit)
constexpr uint32_t kShowCapsule = 1u << 2;   // monster-vs-monster push capsule
constexpr uint32_t kRefill = 1u << 3;        // health refills after a while without damage, and nobody dies
constexpr uint32_t kPanel = 1u << 4;         // damage panel
constexpr uint32_t kShowInputs = 1u << 6;    // input history (P1 and the dummy)
constexpr uint32_t kTrainHud = 1u << 7;      // dummy's HUD on the right and the numbers over the bars (P1 and dummy)
constexpr uint32_t kMoveTable = 1u << 8;     // P1's punch table (StatePunch configs) and the sphere before the attack window
constexpr uint32_t kDefault = kShowAttacks | kShowHulls | kRefill | kPanel | kShowInputs | kTrainHud | kMoveTable;

// Player 1's HUD cells in GS screen units (640 x 224, 4:3 band), measured from a screenshot (investigation 2026-10-23,
// treino_hud.md): the dummy's is the mirror image (x' = 640 - x), the way the game draws player 2's
namespace hud
{
struct Box
{
    float x0, y0, x1, y1;
};
constexpr Box kPortrait{17.0f, 10.0f, 61.7f, 30.6f};
constexpr Box kHealth{65.3f, 15.1f, 272.0f, 20.5f};
constexpr Box kSpecial{69.7f, 24.7f, 83.0f, 31.1f};
constexpr Box kStamina{18.0f, 32.7f, 29.0f, 88.2f};
constexpr Box mirror(const Box &b) { return Box{640.f - b.x1, b.y0, 640.f - b.x0, b.y1}; }
}

// Dummy (the AIs): standing (pad off), CPU (the game's AI), always blocks, blocks after the 1st hit (until it recovers and goes
// 1 s without being hit). In the two blocking modes the AI doesn't run: the Ai::updateInputs hook holds the GamePad's R2.
// Jump: X pressed in cycles (crouching doesn't exist in the game).
enum Dummy : int { DStand, DCpu, DBlock, DBlockAfter, DJump, DCount };
constexpr uint32_t kJumpPeriod = 50, kJumpHold = 4;   // frames in the jumping dummy's cycle and how many with X held

inline std::atomic<bool> g_on{false};                 // WOTM_TRAINING=1, or the fight that started with training armed
// Armed by the TRAINING line on screen 2 (wotm_mainmenu.inc): the flow skips screens 12 and 15 (wotm_padsel.inc, changeScreenHook), the
// fight's rtMain(first) turns g_on on (wotm_overrides.cpp) and the main menu disarms it and turns it off (menuDisarm)
inline std::atomic<bool> g_menuArmed{false};
inline void menuDisarm(const char *where)
{
    if (!g_menuArmed.exchange(false))
        return;
    g_on = false;
    std::fprintf(stderr, "[training] training mode disarmed (%s)\n", where);
}
inline std::atomic<uint32_t> g_opts{kDefault};
inline std::atomic<int> g_dummy{DStand};
inline std::atomic<int> g_dummyHealth{100};           // % of max health the refill brings the dummy back to
// Meters, on every monster: off, full stamina, rage (stamina at maxLevel); with either of the two on, the special is unlocked
enum Meters : int { MOff, MFull, MRage, MCount };
inline std::atomic<int> g_meters{MFull};
inline std::atomic<bool> g_resetStats{false};

// CPU difficulty (TheGame +0x1203CC, 0 easy, 1 medium, 2 hard; the AI reads it every frame): -1 = the game's. Written during the fight by the
// Monster::update hook (applyDifficulty); the game's value is saved and restored when the option goes back to -1, on "yes" in ARE YOU SURE and in the
// main menu (restoreDifficulty), so it doesn't end up in the save.
constexpr uint32_t kDifficulty = 0x1203CCu;
inline std::atomic<int> g_cpuDiff{-1};
inline std::atomic<int> g_diffSaved{-1};
inline void restoreDifficulty(uint8_t *rdram, uint32_t addr)
{
    const int s = g_diffSaved.exchange(-1);
    if (s >= 0)
    {
        std::memcpy(rdram + addr, &s, 4);
        std::fprintf(stderr, "[training] game difficulty restored (%d)\n", s);
    }
}
inline void applyDifficulty(uint8_t *rdram, uint32_t addr)
{
    const int d = g_cpuDiff.load(std::memory_order_relaxed);
    if (d < 0)
    {
        restoreDifficulty(rdram, addr);
        return;
    }
    if (g_diffSaved.load(std::memory_order_relaxed) < 0)
    {
        int cur;
        std::memcpy(&cur, rdram + addr, 4);
        g_diffSaved = cur >= 0 && cur <= 2 ? cur : 1;
    }
    std::memcpy(rdram + addr, &d, 4);
}

// The game's font atlas (tex0), from the last fontSetTexId (wotm_live.inc): the panel writes with it, since the training fight doesn't draw
// game text (the tex0 of the last frame with text is the menu's until the first pause, and the windows came out empty)
inline std::atomic<uint64_t> g_fontTex0{0};

// Skip the fight intro (SKIP INTRO line in the panel): rtMain(first) arms g_introMs; the Monster::updateCinema hook
// (wotm_training.inc) takes the START path
inline std::atomic<bool> g_skipIntro{true};
inline std::atomic<int64_t> g_introMs{-1};

// Training windows (panel, inputs, spheres and boxes) only while the fight is running: rtMain turns them on (wotm_overrides.cpp), "yes" in ARE YOU
// SURE (QUIT or RESET POSITIONS, wotm_pausemenu.inc) turns them off immediately, and the pause (displayDialog called recently) hides them
inline std::atomic<bool> g_inFight{false};
inline std::atomic<int64_t> g_dialogMs{-100000};
inline int64_t nowMs()
{
    return std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now().time_since_epoch()).count();
}
inline bool windowsHidden() { return !g_inFight.load(std::memory_order_relaxed) || nowMs() - g_dialogMs.load(std::memory_order_relaxed) < 200; }

// Type of attack test that recorded the sphere (or cone). KFire: FireBreath::TestCollis's line (hdLosCSTest); KBody: the body
// spheres in the charge and when thrown (hdBundledSphereTest from collisTestEnvironmentCollisions); KStomp and KGrab: the victim search
// area (getClosestMonsterToPunch for the stomp, getClosestMonsterInFOV for the grab) and the mark on the victim when the attack deals damage;
// KCone: StateSonicRoar::testConeCollis's cone; KHook: Ultra-V's grapple hook line (StateGrappleHook::testCollis, radius-0 hdSphereTest
// from the tip's previous point to its current one)
enum Kind : uint8_t { KPunch, KBat, KGetup, KWeapon, KShock, KHook, KOther, KFire, KBody, KStomp, KGrab, KCone, KKinds };

// A swept sphere (from a to b, radius r) the game tested in one frame; a == b for a stationary sphere
struct Sweep
{
    float a[3], b[3];
    float r;
    uint32_t frame;   // g_frame
    uint8_t kind;
    int8_t owner;     // slot in m_monsters (-1 unknown)
    uint8_t hit;      // 0 nothing, 1 scenery, 2 object or monster
};

// A cone tested in one frame: apex, axis (unit), reach, cosine of the half-angle. flat: sector in the plane perpendicular to `up`
// (the stomp: getClosestMonsterToPunch searches in the ground plane)
struct Cone
{
    float apex[3], axis[3], up[3];
    float len, cosHalf;
    uint32_t frame;
    uint8_t kind;
    int8_t owner;
    uint8_t hit;   // 2: the test dealt damage or found the victim
    bool flat;
};

// One applied damage (Monster::takeDamage): health before and after
struct Hit
{
    uint32_t frame;
    int8_t victim, attacker;   // slots (-1 unknown)
    float base;                // damage requested from takeHit (before blocking and modifiers); < 0 without takeHit (fire, fall...)
    float applied;             // health before - after
    float before, max;
    int source;                // takeHit's HitEvent::source (-1 none)
};

// A body volume in world space: segments (pairs of points) of a hull, or a capsule
struct Hull
{
    int8_t owner;
    std::vector<float> seg;   // x0 y0 z0 x1 y1 z1 ...
};
struct Capsule
{
    int8_t owner;
    float a[3], b[3], r;
};

// Measured frame data (g_frame): one attack by the attacker (an attack state, or one step of a StatePunch combo)
struct MoveRec
{
    int8_t owner;
    const char *name;     // state name (table in game.h)
    int8_t chain, step;   // StatePunch/BatSwipe: chain and step (.CHR configs); -1 for the others
    uint32_t start, end;  // state frames (end exclusive)
    int startup;          // frame of the first attack test, counting the state's first frame as 1 (0 = never tested)
    int active;           // frames with an attack test
    int8_t hitVictim;     // slot of the first victim damaged (-1 none)
};
// A victim reaction: frames unable to act (recoil, stunned, thrown...)
struct StunRec
{
    int8_t victim;
    const char *name;
    uint32_t start, end;
    bool blocked = false;   // recoil from a blocked attack (StateRecoil +0x64, the recoil's isBlocking)
};

// Combo: the hits a victim took without recovering (each one landed while it was still in a reaction, or up to 3 frames after the previous one)
struct Combo
{
    int8_t victim = -1, attacker = -1;
    int hits = 0;
    float damage = 0.f;
    uint32_t start = 0, last = 0;
};

// Inputs: one history entry (direction and buttons unchanged for `frames` frames)
constexpr uint16_t kBtnL1 = 1, kBtnL2 = 2, kBtnR1 = 4, kBtnR2 = 8, kBtnX = 16, kBtnCircle = 32, kBtnTriangle = 64, kBtnSquare = 128;
struct PadRec
{
    uint8_t dir;      // numpad notation, relative to the monster: 8 forward, 2 back, 4 left, 6 right, 5 neutral
    uint16_t btn;     // kBtn*
    uint32_t frames;
};

// Punch table: a StatePunch PunchConfig (Monster +0x8470 +0x20 + pos*0x420 + type*0xB0; the values from RST/<MONSTER>.CHR,
// key 0xFCC, which tools/chr_dump.py also reads). pos = position in the combo (StatePunch +0x2988), type = which attack (+0x298C).
// Holding an object, a StateBatSwipe SwipeConfig (Monster +0xAE90 +0x20 + pos*0x480 + type*0xC0; pos +0x2D48, type +0x2D4C):
// same layout at the start, and the damage is a multiplier on the object's damage (weapon = true, dmg = the multiplier)
struct PunchCfg
{
    bool valid = false;
    bool weapon = false;         // SwipeConfig: dmg multiplies the object's damage (+0x90 with a bat, +0xB0 for the others)
    int anim = -1;               // MonsterAnim (+0x00)
    float ini = 0.f, fim = 0.f;  // attack test window, fraction of the animation (+0x0C, +0x10)
    float dmg = 0.f;             // requested damage (+0x90)
    float radius = 0.f;          // fist sphere radius (+0x74; 0 = the default, StatePunch +0x2980)
    float stun = 0.f;            // stun (+0x6C; > 100 stuns)
    float kb = 0.f;              // knockback force (+0x64; != 0 knocks down)
    int loc[2] = {-1, -1};       // locators tested (+0x7C, +0x80; < 0x7D0 none; SwipeConfig only has +0x7C)
};
// A monster's punch in progress, at the exit of Monster::update: config, point in the animation and where the locators are now
struct PunchLive
{
    uint32_t frame = 0;          // g_frame of the reading (0 = never)
    int8_t tier = -1, type = -1;
    float pct = -1.f;            // animationGetCurrentPercent of the attack's animation
    bool inWindow = false;       // StatePunch +0x2A00 (inCollisionWindow; StateBatSwipe +0x2DC0)
    uint8_t nloc = 0;
    float loc[2][3] = {};
    PunchCfg cfg;
};
constexpr int kPunchTiers = 10, kPunchTypes = 6;
// name of each punch / object-attack type (step), in .CHR config order
inline const char *const kMoveTypeNames[kPunchTypes] = {"HEAVY", "FORWARD + HEAVY", "BACK + HEAVY", "FORWARD + LIGHT", "BACK + LIGHT", "LIGHT"};

// Move list (the pause's MOVE LIST) for P1's monster, built from the configs in RAM: the 6 punches of the first position, the same-button
// sequences (the next type is allowed by the config's float +0x94 + type*4 and the next slot exists) with summed damage, and the 6
// object attacks. id: "P<type>", "C<type>x<n>", "W<type>" (what the favorites file stores)
// MkOther: the other moves (butt slam, charge, grab, getting up, crowd control, throw, specials), with name, ready-made hint and the
// state's offset in Monster (the line lights up while P1 is in it); numbers read from RAM at the offsets in
// 80b3802:docs/investigacoes/2026-10-25/move_list_comuns.md and move_list_especiais.md
enum MoveKind : uint8_t { MkPunch, MkCombo, MkWeapon, MkOther };
struct Move
{
    uint8_t kind = MkPunch;
    int8_t type = -1;            // PadFlags type (0 triangle ... 5 square), the button for each attack in the sequence
    uint8_t len = 1;             // attacks in the sequence
    char id[12] = {};
    float dmg = 0.f;             // sum (combo); multiplier on the object's damage (object)
    float ini = 0.f, fim = 0.f;  // window of the last attack
    float radius = 0.f, stun = 0.f, kb = 0.f;   // of the last attack
    uint32_t state = 0;          // MkOther: state's offset in Monster
    char name[16] = {};          // MkOther: short name (in-game box and panel)
    char hint[200] = {};         // MkOther: input and numbers (panel hint)
    uint8_t btns = 0;            // MkOther: input buttons (kBtn* bits; the icons on the panel line)
    int8_t dir = 5;              // MkOther: input direction (numpad: 5 none, 8 forward/up, 2 back)
    int8_t variant = 0;          // MkOther: 0 lights up by state; 1 only with the state's animation (+0x50) != 0xBA (normal grab attack), 2 only == 0xBA (special)
};
// direction and button for each punch type (the rows of the in-game table and the panel): 1 and 3 are forward + button, 2 and 4 back + button
inline int moveTypeDir(int type) { return type == 1 || type == 3 ? 8 : type == 2 || type == 4 ? 2 : 5; }
inline uint8_t moveTypeBtn(int type) { return type <= 2 ? uint8_t(kBtnTriangle) : uint8_t(kBtnSquare); }
constexpr int kFavMax = 8;

struct Shared
{
    std::vector<Move> moveList;  // the list for P1's monster (movesType = m_typeBits >> 5; -1 = not built yet)
    int movesType = -1;
    std::vector<std::string> fav;   // favorite ids for monster movesType (up to kFavMax); the in-game box shows these
    bool favDirty = false;          // the panel changed the favorites: wotm_training.inc writes the file
    uint32_t p1State = 0;           // P1's state offset in Monster (lights up the box's MkOther line)
    uint32_t p1StateAnim = 0;       // the word at +0x50 of P1's state (the animation, for grab attacks: Move::variant)
    std::mutex m;
    PunchLive live[16];          // the punch in progress per slot (stale frame = not punching)
    PunchCfg table[kPunchTypes]; // the 6 types for the combo position P1 is at (0 outside a punch)
    int8_t tableTier = 0;
    bool tableWeapon = false;    // the table is the object-attack one (P1 is holding one; StateBatSwipe)
    std::vector<Sweep> sweeps;   // accumulated; the window discards old ones
    std::vector<Cone> cones;     // same
    std::vector<Hit> hits;       // same
    std::vector<MoveRec> moves;  // last finished attacks
    std::vector<StunRec> stuns;  // last finished reactions
    std::vector<Hull> hulls;     // from the last captured frame
    std::vector<Capsule> caps;
    uint32_t frame = 0;          // g_frame of the last capture
    float health[16] = {}, maxHealth[16] = {};
    int8_t players = 1;          // m_numPlayers (slots 0..players-1 are players)
    Combo combo;                 // the last one (still open or finished)
    float bestCombo = 0.f;       // highest combo damage since the last "clear"
    int bestHits = 0;
    std::vector<PadRec> pad[2];  // input history: [0] P1, [1] the first dummy (CPU1); newest at the end
    float stamina[16] = {}, staminaMax[16] = {};
    bool hudShown = false;       // the game's HUD is on screen (gHudEnable, no letterbox): the bar numbers are drawn
};
inline Shared &shared()
{
    static Shared s;
    return s;
}
}
