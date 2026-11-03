// wotm_settings.hpp: player options that apply live (the "recompiled options" panel in the main menu, wotm_mainmenu.inc). The launcher
// doesn't have them: it only removes the environment variables when it opens the game.
//
// File: settings.ini next to save.json (the folder above mc0/, the ELF's folder: discroot\ in run_live), one KEY=value line per
// option, using each option's environment variable name. Order at startup: default (the old one, with no variable), then the file,
// then whichever environment variables are set (tests and batches); WOTM_SETTINGS=0 ignores the file (neither reads nor writes it).
// The panel writes the whole file with the current values on every change.
//
// Each value is a std::atomic<int>: the EE thread (mod hooks, panel) and the window thread (video) read it on every use.
#pragma once

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>

#include "net/wotm_modifiers.hpp"

namespace wotm::settings
{
enum Key
{
    Res,          // native frame height (lines); 0 = the window's
    Aspect,       // 0 = dynamic (follows the window); 1.. = kAspectNames[i]
    Display,      // 0 windowed, 1 borderless fullscreen, 2 exclusive fullscreen
    Rate,         // presentation Hz; 0 = the monitor's
    Vsync,        // 0/1
    LockToggle,   // QoL: lock-on toggled with R1
    EnemyBars,    // QoL: health and energy bars over enemies
    DamagePop,    // QoL: damage numbers
    KbSeparate,   // keyboard is a separate player
    Prompts,      // button icons: kPromptNames (0 = automatic, from the last device used; 1 = the game's 3D ones)
    MenuConfirm,  // in menus, with a gamepad: 0 = bottom button confirms (the game's), 1 = right button confirms and bottom goes back
    Camera,       // 0 = classic (the game's), 1 = modern (orbital: right stick, screen-relative controls; wotm_orbit.inc)
    CamRecenter,  // modern camera: slowly swings back behind the monster after ~1.5 s untouched, while it's walking
    CloseCam,     // 1 = the game's close-up camera (unified view); 0 = never switches to it
    CamSens,      // modern camera: right stick speed, 1..10 (5 = the old speed; each step is 20%)
    CamInvertY,   // modern camera: 1 = inverts the vertical axis of the right stick and mouse (default: up looks up, the camera moves down)
    RenderScale,  // native frame scale over Res, in %: 50..200 (above 100 = supersampling)
    Msaa,         // native frame MSAA samples: 0 (off), 2, 4, 8
    TexFilter,    // game texture filter: 0 bilinear (the GS's), 1 trilinear (mipmaps), 2/4/8/16 anisotropic
    Announcer,    // select-screen announcer (wotm_narrator.inc): 0 off, 1 the voice (MP3 with the name), 2 the monster's own roar
    SkipIntro,    // skips the two boot movies (wotm_fmv.hpp); read once, on the first movie: takes effect on the next boot
    Vibration,    // gamepad vibration (wotm_rumble.inc)
    Interp,       // frame interpolation (wotm_live.inc, "interpolacao"), on by default: above 60 Hz (g_presentHz) or with the game at 30
    SuperJump,    // gameplay modifiers (net/wotm_modifiers.hpp, wotm_modifiers.inc): 0/1 each, latched at fight start
    FastMonsters,
    Turbo,
    kKeys
};

inline constexpr const char *kEnv[kKeys] = {"WOTM_RES",        "WOTM_ASPECT",    "WOTM_FULLSCREEN", "WOTM_HZ",        "WOTM_VSYNC",
                                            "WOTM_LOCKTOGGLE", "WOTM_ENEMYBARS", "WOTM_DAMAGEPOP",  "WOTM_KBSEPARATE",
                                            "WOTM_PROMPTS",    "WOTM_MENUCONFIRM", "WOTM_CAMERA",    "WOTM_CAMRECENTER", "WOTM_CLOSECAM",
                                            "WOTM_CAMSENS",    "WOTM_CAMINVERT",   "WOTM_RENDERSCALE", "WOTM_MSAA",      "WOTM_TEXFILTER",
                                            "WOTM_ANNOUNCER",  "WOTM_SKIPINTRO",   "WOTM_VIBRATION",   "WOTM_INTERP",
                                            "WOTM_SUPERJUMP",  "WOTM_FASTMONSTERS", "WOTM_TURBO"};
inline constexpr int kDefault[kKeys] = {720, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 5, 0, 100, 0, 0, 1, 0, 1, 1, 0, 0, 0};
inline constexpr const char *kPromptNames[] = {"auto", "original", "keyboard", "xbox", "playstation", "nintendo"};
inline constexpr int kPromptStyles = 6;
enum PromptStyle { PromptAuto, PromptOriginal, PromptKeyboard, PromptXbox, PromptPs, PromptNintendo };
inline constexpr const char *kConfirmNames[] = {"bottom", "right"};

// Choices the panel cycles through
inline constexpr int kResolutions[] = {0, 448, 720, 1080, 1440, 2160};   // 0 = the window's
inline constexpr int kRenderScales[] = {50, 75, 100, 125, 150, 200};
inline constexpr int kMsaaLevels[] = {0, 2, 4, 8};
inline constexpr int kTexFilters[] = {0, 1, 2, 4, 8, 16};
inline constexpr int kRates[] = {0, 60, 120, 144, 165, 180, 240};
inline constexpr const char *kAspectNames[] = {"dynamic", "4:3", "16:9", "16:10", "21:9", "32:9"};
inline constexpr float kAspectRatio[] = {0.f, 4.f / 3.f, 16.f / 9.f, 16.f / 10.f, 21.f / 9.f, 32.f / 9.f};
inline constexpr int kAspects = 6;

inline std::atomic<int> g_val[kKeys] = {720, 0, 0, 0, 1, 0, 0, 0, 0, 0, 0, 1, 1, 1, 5, 0, 100, 0, 0, 1, 0, 1, 1, 0, 0, 0};
inline std::filesystem::path g_path;   // empty: no file (WOTM_SETTINGS=0 or before load)
inline std::mutex g_fileMutex;
inline bool g_pathPending = false;   // load found keys from an older layout: rewrites the file at the end of load
// Presentation rate in effect (initPresentRate, wotm_live.inc; 0 = not opened yet): the Interp option only applies above 60
inline std::atomic<int> g_presentHz{0};

// Network match (WOTM_NET; turned on in load): options that change game state take a fixed profile, the same on every PC. The
// modern camera writes the game camera using this PC's mouse, sensitivity and inversion, and toggled lock-on changes what the
// buttons do. The file and the panel keep the player's values (the panel shows them locked), which come back outside network play.
inline constexpr Key kNetKeys[] = {LockToggle, Camera, CamRecenter, CloseCam, CamSens, CamInvertY};
inline constexpr int kNetValues[] = {0, 0, 1, 1, 5, 0};
inline std::atomic<bool> g_netProfile{false};
// The lobby's modifiers (WOTM_MODIFIERS, wotm::modifiers bits): forced on the three modifier keys in a network match
inline std::atomic<int> g_netMods{0};
inline constexpr Key kModKeys[wotm::modifiers::kCount] = {SuperJump, FastMonsters, Turbo};   // bit i = kModKeys[i]

// Network profile value for `k`, or -1 if the option is free
inline int netValue(Key k)
{
    for (int i = 0; i < wotm::modifiers::kCount; ++i)
        if (kModKeys[i] == k)
            return (g_netMods.load(std::memory_order_relaxed) >> i) & 1;
    for (size_t i = 0; i < sizeof kNetKeys / sizeof kNetKeys[0]; ++i)
        if (kNetKeys[i] == k)
            return kNetValues[i];
    return -1;
}
inline bool forced(Key k) { return g_netProfile.load(std::memory_order_relaxed) && netValue(k) >= 0; }

inline int get(Key k) { return forced(k) ? netValue(k) : g_val[k].load(std::memory_order_relaxed); }
inline bool on(Key k) { return get(k) != 0; }

// Active modifiers as wotm::modifiers bits (the lobby's in a network match)
inline int modifiers()
{
    int bits = 0;
    for (int i = 0; i < wotm::modifiers::kCount; ++i)
        bits |= on(kModKeys[i]) ? 1 << i : 0;
    return bits;
}

// WOTM_MODIFIERS=<list> (launcher, lobby): sets the three keys; false (nothing changes) on an unknown name
inline bool applyModifierList(const char *list)
{
    int bits = 0;
    if (!wotm::modifiers::parse(list ? list : "", bits))
        return false;
    for (int i = 0; i < wotm::modifiers::kCount; ++i)
        g_val[kModKeys[i]].store((bits >> i) & 1);
    return true;
}

// Fixed aspect ratio from the Aspect option (0 = dynamic)
inline float aspectRatio()
{
    const int a = get(Aspect);
    return a > 0 && a < kAspects ? kAspectRatio[a] : 0.f;
}

// Text -> value, with each old variable's rules (same meaning in the file and in the environment)
inline bool parse(Key k, const char *s, int &out)
{
    if (!s) return false;
    while (*s == ' ') ++s;
    switch (k)
    {
    case Res:
    {
        if (!std::strcmp(s, "window")) { out = 0; return true; }
        if (*s < '0' || *s > '9') return false;
        const int v = std::atoi(s);
        out = v <= 0 ? 0 : std::clamp(v, 224, 4320);
        return true;
    }
    case RenderScale:
    {
        const int v = std::atoi(s);
        if (v <= 0) return false;
        out = std::clamp(v, 25, 400);
        return true;
    }
    case Msaa:
    {
        const int v = std::atoi(s);
        out = v >= 8 ? 8 : v >= 4 ? 4 : v >= 2 ? 2 : 0;
        return true;
    }
    case TexFilter:
    {
        if (!std::strcmp(s, "bilinear")) { out = 0; return true; }
        if (!std::strcmp(s, "trilinear")) { out = 1; return true; }
        const int v = std::atoi(s);   // "16x" = 16
        out = v >= 16 ? 16 : v >= 8 ? 8 : v >= 4 ? 4 : v >= 2 ? 2 : v == 1 ? 1 : 0;
        return true;
    }
    case Aspect:
    {
        if (!*s || !std::strcmp(s, "dynamic") || !std::strcmp(s, "0")) { out = 0; return true; }
        float w = 0.f, h = 0.f;
        if (std::sscanf(s, "%f:%f", &w, &h) != 2 || w <= 0.f || h <= 0.f) return false;
        // the nearest one in the list
        const float r = w / h;
        int best = 1;
        for (int i = 2; i < kAspects; ++i)
            if (std::fabs(kAspectRatio[i] - r) < std::fabs(kAspectRatio[best] - r)) best = i;
        out = best;
        return true;
    }
    case Display: out = *s == '2' ? 2 : (*s && *s != '0') ? 1 : 0; return true;
    case Rate: out = std::max(0, std::atoi(s)); return true;
    case CamSens:
    {
        const int v = std::atoi(s);
        if (v <= 0) return false;
        out = std::clamp(v, 1, 10);
        return true;
    }
    case Vsync: out = *s == '0' ? 0 : 1; return true;
    case Announcer: out = std::clamp(std::atoi(s), 0, 2); return true;
    case Interp: out = std::atoi(s) > 0 || !std::strcmp(s, "on") ? 1 : 0; return true;   // "redraw" (stage-0 diagnostic) doesn't turn it on
    case KbSeparate: out = *s == '1' ? 1 : 0; return true;
    case Prompts:
    case MenuConfirm:
    {
        const int n = k == Prompts ? kPromptStyles : 2;
        for (int i = 0; i < n; ++i)
            if (!std::strcmp(s, k == Prompts ? kPromptNames[i] : kConfirmNames[i]))
            {
                // "playstation" was dropped from the list (it was the same as original, with the game's face buttons): becomes "original"
                out = k == Prompts && i == PromptPs ? int(PromptOriginal) : i;
                return true;
            }
        if (*s < '0' || *s > '9') return false;
        out = std::clamp(std::atoi(s), 0, n - 1);
        return true;
    }
    default: out = (*s && std::strcmp(s, "0") != 0) ? 1 : 0; return true;
    }
}

inline std::string format(Key k, int v)
{
    if (k == Aspect) return kAspectNames[v > 0 && v < kAspects ? v : 0];
    if (k == Prompts) return kPromptNames[std::clamp(v, 0, kPromptStyles - 1)];
    if (k == MenuConfirm) return kConfirmNames[v ? 1 : 0];
    if (k == Res && v <= 0) return "window";
    if (k == TexFilter) return v <= 0 ? "bilinear" : v == 1 ? "trilinear" : std::to_string(v) + "x";
    return std::to_string(v);
}

// ------------------------------------------------------------------ key and button map (panel's CONTROLS tab)
// Each PS2 button gets up to two keyboard keys and one gamepad button. raylib codes (KeyboardKey, GamepadButton); 0 = nothing.
// Gamepads have one map per player (kPadPlayers): a gamepad uses the map of the player it feeds (wotm_pad::playerOfPad); the keyboard
// has only one. In the file: KB_<BUTTON>=KEY[,KEY], PAD_<BUTTON>=BUTTON (player 1) and PAD2_/PAD3_/PAD4_<BUTTON>=BUTTON, with the names from
// kKeyNames/kPadButtonNames. No environment variable.
// Keyboard-only actions (step 8 of the orbital camera, 75f5be1:docs/investigacoes/2026-10-22/camera_orbital_plano.md): they become PS2 buttons according to
// toggled lock-on (WOTM_LOCKTOGGLE changes which buttons lock, block and strafe; wotm_pad.inc, keyboard) or request a target switch
// directly from lock-on (wotm_mouse.hpp). They assume the game's CONTROL TYPE A (configuration 0).
enum Bind
{
    BUp, BDown, BLeft, BRight,                 // D-pad
    BCross, BCircle, BSquare, BTriangle,
    BL1, BR1, BL2, BR2,
    BL1R1,                                     // keyboard only: L1 and R1 together
    BStart, BSelect, BL3, BR3,
    BStickUp, BStickDown, BStickLeft, BStickRight,   // keyboard only: left stick
    BLock, BBlock, BStrafeL, BStrafeR,         // keyboard only: lock on, block, strafe (buttons depend on toggled lock-on)
    BTargetNext, BTargetPrev,                  // keyboard only: next / previous target (with a target locked)
    BTriCircle, BCrossSquare,                  // keyboard only: triangle and circle together, X and square together (specials)
    BWalk,                                     // keyboard only: while held, the stick only goes part of the way (walking)
    kBinds
};
inline constexpr const char *kBindNames[kBinds] = {"UP",     "DOWN",   "LEFT",   "RIGHT",    "CROSS",     "CIRCLE",     "SQUARE",
                                                   "TRIANGLE", "L1",   "R1",     "L2",       "R2",        "L1_R1",      "START",
                                                   "SELECT", "L3",     "R3",     "STICK_UP", "STICK_DOWN", "STICK_LEFT", "STICK_RIGHT",
                                                   "LOCK",   "BLOCK",  "STRAFE_LEFT", "STRAFE_RIGHT", "TARGET_NEXT", "TARGET_PREV",
                                                   "TRIANGLE_CIRCLE", "CROSS_SQUARE", "WALK"};
// scePadRead bits (1 = the button)
inline constexpr uint16_t kBindMask[kBinds] = {0x0010, 0x0040, 0x0080, 0x0020, 0x4000, 0x2000, 0x8000, 0x1000, 0x0400, 0x0800, 0x0100,
                                               0x0200, 0x0C00, 0x0008, 0x0001, 0x0002, 0x0004, 0,      0,      0,      0,
                                               0,      0,      0,      0,      0,      0,      0x3000, 0xC000, 0};
inline constexpr bool bindOnPad(Bind b) { return b != BL1R1 && b < BStickUp; }

struct KeyName { int code; const char *name; };
// Mouse codes in the keyboard map (outside raylib's KeyboardKey): buttons 1000 + MouseButton, wheel 1010 (up) and 1011 (down)
inline constexpr int kMouseBase = 1000, kWheelUp = 1010, kWheelDown = 1011;
// Keys that can be picked (Esc cancels picking, but works as a default; F1..F12 belong to the runtime: debug menu, free camera,
// screenshots, fullscreen). The mouse and Esc names are the ones from the icon atlas (wotm_prompts.h).
inline constexpr KeyName kKeyNames[] = {
    {256, "ESC"},
    {1000, "MOUSE_LEFT"}, {1001, "MOUSE_RIGHT"}, {1002, "MOUSE_MIDDLE"}, {1003, "MOUSE_SIDE"}, {1004, "MOUSE_EXTRA"},
    {1010, "WHEEL_UP"}, {1011, "WHEEL_DOWN"},
    {32, "SPACE"}, {39, "APOSTROPHE"}, {44, "COMMA"}, {45, "MINUS"}, {46, "PERIOD"}, {47, "SLASH"},
    {48, "0"}, {49, "1"}, {50, "2"}, {51, "3"}, {52, "4"}, {53, "5"}, {54, "6"}, {55, "7"}, {56, "8"}, {57, "9"},
    {59, "SEMICOLON"}, {61, "EQUAL"},
    {65, "A"}, {66, "B"}, {67, "C"}, {68, "D"}, {69, "E"}, {70, "F"}, {71, "G"}, {72, "H"}, {73, "I"}, {74, "J"}, {75, "K"}, {76, "L"},
    {77, "M"}, {78, "N"}, {79, "O"}, {80, "P"}, {81, "Q"}, {82, "R"}, {83, "S"}, {84, "T"}, {85, "U"}, {86, "V"}, {87, "W"}, {88, "X"},
    {89, "Y"}, {90, "Z"},
    {91, "LBRACKET"}, {92, "BACKSLASH"}, {93, "RBRACKET"}, {96, "GRAVE"},
    {257, "ENTER"}, {258, "TAB"}, {259, "BACKSPACE"}, {260, "INSERT"}, {261, "DELETE"}, {262, "RIGHT"}, {263, "LEFT"}, {264, "DOWN"},
    {265, "UP"}, {266, "PGUP"}, {267, "PGDN"}, {268, "HOME"}, {269, "END"}, {280, "CAPSLOCK"},
    {320, "KP0"}, {321, "KP1"}, {322, "KP2"}, {323, "KP3"}, {324, "KP4"}, {325, "KP5"}, {326, "KP6"}, {327, "KP7"}, {328, "KP8"},
    {329, "KP9"}, {330, "KPDECIMAL"}, {331, "KPDIVIDE"}, {332, "KPMULTIPLY"}, {333, "KPMINUS"}, {334, "KPPLUS"}, {335, "KPENTER"},
    {340, "LSHIFT"}, {341, "LCTRL"}, {342, "LALT"}, {344, "RSHIFT"}, {345, "RCTRL"}, {346, "RALT"},
};
// raylib GamepadButton 1..17, by position on the controller (the per-style icons come later)
inline constexpr const char *kPadButtonNames[18] = {"",        "DPAD_UP", "DPAD_RIGHT", "DPAD_DOWN", "DPAD_LEFT", "FACE_UP",
                                                    "FACE_RIGHT", "FACE_DOWN", "FACE_LEFT", "L1",    "L2",        "R1",
                                                    "R2",      "BACK",    "GUIDE",      "START",     "L3",        "R3"};
inline constexpr int kPadButtons = 18;

// Default keyboard (layout 3): WASD moves (left stick), the mouse turns the camera, left button light attack (square), right button
// heavy (triangle), Space jumps (X), E or F grabs (circle), Shift blocks, Q or the middle button locks on, Tab/R or the wheel switch
// targets, Z/C strafe, 1 and 2 the specials (triangle+circle, X+square), Alt walks slowly, Enter or Esc pause (Esc and Backspace go
// back in the menus, wotm_pad.inc), G or Ctrl taunts (Select), arrows on the D-pad. L1, R1, L2, R2 and L1+R1 have no key: the actions
// above drive them.
inline constexpr uint32_t kbPair(int a, int b = 0) { return uint32_t(a) | (uint32_t(b) << 16); }
inline constexpr uint32_t kDefaultKb[kBinds] = {kbPair(265), kbPair(264), kbPair(263), kbPair(262),           // D-pad: arrows
                                                kbPair(32), kbPair(69, 70), kbPair(1000), kbPair(1001),      // X, circle, square, triangle
                                                0, 0, 0, 0, 0,                                               // L1, R1, L2, R2, L1+R1
                                                kbPair(257, 256), kbPair(71, 341), 0, 0,                     // Start, Select, L3, R3
                                                kbPair(87), kbPair(83), kbPair(65), kbPair(68),              // left stick: WASD
                                                kbPair(81, 1002), kbPair(340), kbPair(90), kbPair(67),       // lock-on, block, strafe
                                                kbPair(258, 1011), kbPair(82, 1010),                         // next, previous target
                                                kbPair(49), kbPair(50), kbPair(342)};                        // specials, walk
// KB_LAYOUT in settings.ini: the KB_ lines of a file from an older layout are ignored (the player gets the new defaults)
inline constexpr int kKbLayout = 3;
inline constexpr int kDefaultPad[kBinds] = {1, 3, 4, 2, 7, 6, 8, 5, 9, 11, 10, 12, 0, 15, 13, 16, 17, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

inline std::atomic<uint32_t> g_kb[kBinds];
inline constexpr int kPadPlayers = 4;
inline std::atomic<int> g_pad[kPadPlayers][kBinds];
inline const bool g_bindsInit = [] {
    for (int b = 0; b < kBinds; ++b)
    {
        g_kb[b].store(kDefaultKb[b]);
        for (int p = 0; p < kPadPlayers; ++p) g_pad[p][b].store(kDefaultPad[b]);
    }
    return true;
}();
// Prefix of player p's (0..3) gamepad map in the file
inline std::string padPrefix(int p) { return p == 0 ? std::string("PAD_") : "PAD" + std::to_string(p + 1) + "_"; }

inline uint32_t kb(Bind b) { return g_kb[b].load(std::memory_order_relaxed); }
inline int pad(Bind b, int player = 0) { return g_pad[std::clamp(player, 0, kPadPlayers - 1)][b].load(std::memory_order_relaxed); }

inline const char *keyName(int code)
{
    for (const KeyName &k : kKeyNames)
        if (k.code == code) return k.name;
    return nullptr;
}
inline int keyCode(const std::string &name)
{
    for (const KeyName &k : kKeyNames)
        if (name == k.name) return k.code;
    return 0;
}
inline std::string formatKb(uint32_t v)
{
    std::string s;
    for (int i = 0; i < 2; ++i)
        if (const char *n = keyName(int((v >> (16 * i)) & 0xFFFF)))
            s += (s.empty() ? "" : ",") + std::string(n);
    return s;
}
inline uint32_t parseKb(const std::string &s)
{
    const size_t c = s.find(',');
    const int a = keyCode(s.substr(0, c)), b = c == std::string::npos ? 0 : keyCode(s.substr(c + 1));
    return a ? kbPair(a, b != a ? b : 0) : kbPair(b);
}
inline int parsePad(const std::string &s)
{
    for (int i = 1; i < kPadButtons; ++i)
        if (s == kPadButtonNames[i]) return i;
    return 0;
}

inline void save();
// New key for button b (the previous one is removed); if it belonged to another button, that button gets the one b had (swap)
inline void bindKey(Bind b, int code)
{
    const uint32_t old = kb(b);
    for (int o = 0; o < kBinds; ++o)
    {
        if (o == b) continue;
        uint32_t v = kb(Bind(o));
        const int k1 = int(v & 0xFFFF), k2 = int(v >> 16);
        if (k1 != code && k2 != code) continue;
        const int give = int(old & 0xFFFF);
        const int n1 = k1 == code ? give : k1, n2 = k2 == code ? (k1 == code ? 0 : give) : k2;
        g_kb[o].store(n1 ? kbPair(n1, n2 != n1 ? n2 : 0) : kbPair(n2));
    }
    g_kb[b].store(kbPair(code));
    save();
}
// New gamepad button for button b in player `player`'s map (swaps with whoever had it, like the keyboard)
inline void bindPad(Bind b, int button, int player)
{
    player = std::clamp(player, 0, kPadPlayers - 1);
    const int old = pad(b, player);
    for (int o = 0; o < kBinds; ++o)
        if (o != b && pad(Bind(o), player) == button)
            g_pad[player][o].store(old);
    g_pad[player][b].store(button);
    save();
}
// `map`: -1 = keyboard, 0..3 = player gamepads
inline void clearBind(Bind b, int map)
{
    (map < 0 ? (void)g_kb[b].store(0) : (void)g_pad[std::clamp(map, 0, kPadPlayers - 1)][b].store(0));
    save();
}
inline void resetBinds(int map)
{
    for (int b = 0; b < kBinds; ++b)
        map < 0 ? g_kb[b].store(kDefaultKb[b]) : g_pad[std::clamp(map, 0, kPadPlayers - 1)][b].store(kDefaultPad[b]);
    save();
}

inline void save()
{
    std::lock_guard<std::mutex> lk(g_fileMutex);
    if (g_path.empty()) return;
    std::ofstream f(g_path, std::ios::trunc);
    if (!f)
    {
        std::fprintf(stderr, "[settings] could not write %s\n", g_path.string().c_str());
        return;
    }
    f << "# Recompiled options (main menu); same keys as the environment variables\n";
    for (int k = 0; k < kKeys; ++k)
        f << kEnv[k] << '=' << format(Key(k), get(Key(k))) << '\n';
    f << "# Keys (KB_) and gamepad buttons for each PS2 button (PAD_ = player 1, PAD2_..PAD4_ = players 2 to 4): CONTROLS tab\n";
    f << "KB_LAYOUT=" << kKbLayout << '\n';
    for (int b = 0; b < kBinds; ++b)
        f << "KB_" << kBindNames[b] << '=' << formatKb(kb(Bind(b))) << '\n';
    for (int p = 0; p < kPadPlayers; ++p)
        for (int b = 0; b < kBinds; ++b)
            if (bindOnPad(Bind(b)))
                f << padPrefix(p) << kBindNames[b] << '=' << kPadButtonNames[std::clamp(pad(Bind(b), p), 0, kPadButtons - 1)] << '\n';
}

inline void set(Key k, int v, bool write = true)
{
    g_val[k].store(v, std::memory_order_relaxed);
    if (write) save();
}

// Environment variables over the current values; returns how many took effect
inline int applyEnv()
{
    int n = 0;
    for (int k = 0; k < kKeys; ++k)
        if (const char *e = std::getenv(kEnv[k]))
            if (int v; parse(Key(k), e, v))
            {
                g_val[k].store(v);
                ++n;
            }
    return n;
}
// Already at static initialization: whatever doesn't go through load (replay --vu1cmp, without the hooks) gets the environment, as before
inline const int g_envAtStart = applyEnv();

// Once, when the hooks are installed (before the first frame). `dir` = save.json's folder.
inline void load(const std::filesystem::path &dir)
{
    static std::once_flag once;
    std::call_once(once, [&] {
        const char *off = std::getenv("WOTM_SETTINGS");
        const bool useFile = !(off && off[0] == '0');
        int fromFile = 0, fromEnv = 0, kbLayout = 0;
        uint32_t fileKb[kBinds] = {};
        bool kbSeen[kBinds] = {};
        if (useFile)
        {
            g_path = dir / "settings.ini";
            std::ifstream f(g_path);
            std::string line;
            while (f && std::getline(f, line))
            {
                if (!line.empty() && line.back() == '\r') line.pop_back();
                const size_t eq = line.find('=');
                if (line.empty() || line[0] == '#' || eq == std::string::npos) continue;
                const std::string key = line.substr(0, eq), val = line.substr(eq + 1);
                for (int k = 0; k < kKeys; ++k)
                    if (key == kEnv[k])
                        if (int v; parse(Key(k), val.c_str(), v))
                        {
                            g_val[k].store(v);
                            ++fromFile;
                        }
                if (key == "KB_LAYOUT")
                    kbLayout = std::atoi(val.c_str());
                for (int b = 0; b < kBinds; ++b)
                {
                    if (key == std::string("KB_") + kBindNames[b])
                        fileKb[b] = parseKb(val), kbSeen[b] = true;
                    else if (bindOnPad(Bind(b)))
                        for (int p = 0; p < kPadPlayers; ++p)
                            if (key == padPrefix(p) + kBindNames[b])
                                g_pad[p][b].store(parsePad(val)), ++fromFile;
                }
            }
            // keys from the file only for the current layout; from an older one (the old I/J/K/L) the new default stays, and the file is rewritten
            int kbFromFile = 0;
            for (int b = 0; b < kBinds; ++b)
                kbFromFile += kbSeen[b];
            if (kbLayout == kKbLayout)
            {
                for (int b = 0; b < kBinds; ++b)
                    if (kbSeen[b])
                        g_kb[b].store(fileKb[b]);
                fromFile += kbFromFile;
            }
            else if (kbFromFile)
            {
                std::fprintf(stderr, "[settings] keys from layout %d ignored: keyboard on layout %d (WASD and mouse)\n", kbLayout, kKbLayout);
                g_pathPending = true;
            }
        }
        fromEnv = applyEnv();
        std::fprintf(stderr, "[settings] %s: %d from file, %d from environment\n", useFile ? g_path.string().c_str() : "file ignored (WOTM_SETTINGS=0)",
                     fromFile, fromEnv);
        // WOTM_MODIFIERS offline: over the three keys, like any variable; in a network match: forced, the player's own values stay
        const char *modList = std::getenv("WOTM_MODIFIERS");
        const char *net = std::getenv("WOTM_NET");
        const bool netMatch = net && *net;
        if (modList && !netMatch && !applyModifierList(modList))
            std::fprintf(stderr, "[settings] WOTM_MODIFIERS=%s: unknown modifier (known: jump, fast, turbo); ignored\n", modList);
        if (netMatch)
        {
            // the lobby's list, unset = none (never this PC's own choice: every PC must play the same modifiers)
            int bits = 0;
            wotm::modifiers::parse(modList ? modList : "", bits);
            g_netMods.store(bits);
            std::fprintf(stderr, "[settings] network match: modifiers \"%s\"\n", wotm::modifiers::format(bits).c_str());
            g_netProfile.store(true);
            std::fprintf(stderr, "[settings] network match: fixed profile (classic camera, the game's lock-on, close camera, no mouse)\n");
        }
        if (g_pathPending)
            save();
    });
}
} // namespace wotm::settings
