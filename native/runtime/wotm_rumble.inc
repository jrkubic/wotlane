// wotm_rumble.inc: gamepad rumble (VIBRATION option on the CONTROLS tab of "Recompiled options", WOTM_VIBRATION, on by
// default). Included by wotm_live.inc before wotm_mainmenu.inc (which calls install).
//  - The game drives the motors through scePadSetActDirect(port, slot, data[6]), with the alignment inputUpdateState sets up: data[0]
//    is the small motor (0/1), data[1] the large one (0..255). In the runtime it's an empty stub; the hook sends both motors to the
//    player's pad over XInput (raylib's GLFW has no rumble on Windows).
//  - The game's per-pad rumble switch (inputUseActuator, which the retail VIBRATION ON/OFF in pause and the options screen used to
//    change; the port removed both lines) stays always on: the option decides. Turning the option off stops the motors immediately.
//  - Player: port + 2 on slot 1 (players 3 and 4, as in scePadRead; wotm_pad.inc). That player's pads: those from wotm_pad::devicesOf.
//  - raylib pad -> XInput index: among raylib gamepads with an XInput name (GLFW calls them "Xbox Controller",
//    "Wireless Xbox Controller" or "XInput ..."), the k-th one is the k-th connected XInput index (GLFW opens them in that order).
//    A pad that Windows reads through DirectInput (DualShock/DualSense without Steam Input) doesn't rumble.
#include <atomic>
#include <chrono>
#include <cstdlib>
#include <cstring>
#include <thread>

#ifdef _WIN32
// No windows.h (it clashes with raylib's names): only what's needed to load XInput
extern "C" __declspec(dllimport) void *__stdcall LoadLibraryA(const char *name);
extern "C" __declspec(dllimport) void *__stdcall GetProcAddress(void *module, const char *name);
#endif

namespace wotm_rumble
{
struct XState { uint32_t packet; uint16_t buttons; uint8_t lt, rt; int16_t lx, ly, rx, ry; };   // XINPUT_STATE
struct XVib { uint16_t left, right; };                                                            // XINPUT_VIBRATION
#ifdef _WIN32
using GetStateFn = unsigned long(__stdcall *)(unsigned long, XState *);
using SetStateFn = unsigned long(__stdcall *)(unsigned long, XVib *);
#else
using GetStateFn = unsigned long (*)(unsigned long, XState *);
using SetStateFn = unsigned long (*)(unsigned long, XVib *);
#endif
static GetStateFn g_get = nullptr;
static SetStateFn g_set = nullptr;
static PS2Runtime::RecompiledFunction g_origUse = nullptr, g_origAct = nullptr;
static uint32_t g_last[4] = {~0u, ~0u, ~0u, ~0u};   // last (left << 16 | right) sent to each XInput index
static uint8_t g_want[4][2] = {};                   // what the game asked for per player (small motor, large)
static bool g_wasOn = true;
static std::atomic<unsigned> g_pulseGen{0};   // feedback pulse in progress (the newest one wins)

static bool loadXInput()
{
#ifdef _WIN32
    for (const char *dll : {"xinput1_4.dll", "xinput9_1_0.dll", "xinput1_3.dll"})
        if (void *m = LoadLibraryA(dll))
        {
            g_get = reinterpret_cast<GetStateFn>(GetProcAddress(m, "XInputGetState"));
            g_set = reinterpret_cast<SetStateFn>(GetProcAddress(m, "XInputSetState"));
            if (g_get && g_set) return true;
        }
#endif
    g_get = nullptr;
    g_set = nullptr;
    return false;
}

static bool isXInputName(const char *n)
{
    return n && (!std::strncmp(n, "Xbox", 4) || !std::strncmp(n, "Wireless Xbox", 13) || !std::strncmp(n, "XInput", 6));
}

// XInput index of raylib gamepad `pad` (-1: not XInput or not found)
static int xinputIndexOf(int pad)
{
    int k = -1, seen = 0;
    for (int i = 0; i < PS2_PAD_MAX_GAMEPADS; ++i)
        if (IsGamepadAvailable(i) && isXInputName(GetGamepadName(i)))
        {
            if (i == pad) { k = seen; break; }
            ++seen;
        }
    if (k < 0) return -1;
    int n = 0;
    for (unsigned long x = 0; x < 4; ++x)
    {
        XState s{};
        if (g_get(x, &s) != 0) continue;   // ERROR_SUCCESS = 0
        if (n == k) return int(x);
        ++n;
    }
    return -1;
}

static void send(int x, uint16_t left, uint16_t right)
{
    const uint32_t v = uint32_t(left) << 16 | right;
    if (x < 0 || x > 3 || g_last[x] == v) return;
    g_last[x] = v;
    XVib vib{left, right};
    g_set(static_cast<unsigned long>(x), &vib);
}

// Sends each pad the request of the player it feeds (the strongest, if the pad feeds more than one)
static void apply()
{
    if (!g_set) return;   // no XInput (Mac, Linux, Windows without the DLL): the game still sees the same pads, nothing rumbles
    const bool on = wotm::settings::on(wotm::settings::Vibration);
    uint16_t left[4] = {}, right[4] = {};
    if (on)
        for (int p = 0; p < 4; ++p)
        {
            if (!g_want[p][0] && !g_want[p][1]) continue;
            const uint32_t devs = wotm_pad::devicesOf(p);
            for (int pad = 0; pad < PS2_PAD_MAX_GAMEPADS; ++pad)
                if (devs & (1u << pad))
                    if (const int x = xinputIndexOf(pad); x >= 0)
                    {
                        left[x] = std::max<uint16_t>(left[x], uint16_t(g_want[p][1] * 257u));
                        right[x] = std::max<uint16_t>(right[x], g_want[p][0] ? 0xFFFF : 0);
                    }
        }
    for (int x = 0; x < 4; ++x) send(x, left[x], right[x]);
}

// scePadSetActDirect(port, slot, data): the runtime's stub returned 1 and did nothing
static void actHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc != wotm::fn::scePadSetActDirect)
    {
        g_origAct(rdram, ctx, runtime);
        return;
    }
    const uint32_t port = GPR_U32(ctx, 4), slot = GPR_U32(ctx, 5), data = GPR_U32(ctx, 6) & 0x1FFFFFFu;
    const int player = int(port) + (slot ? 2 : 0);
    if (player >= 0 && player < 4 && data + 2 <= PS2_RAM_SIZE)
    {
        const bool was = g_want[player][0] || g_want[player][1];
        g_want[player][0] = rdram[data] ? 1 : 0;
        g_want[player][1] = rdram[data + 1];
        static int s_logs = 0;   // log the first few, to check without holding the pad
        if (!was && (g_want[player][0] || g_want[player][1]) && s_logs < 3)
        {
            ++s_logs;
            std::fprintf(stderr, "[rumble] player %d: small motor %d, large %d (option %s)\n", player + 1, g_want[player][0],
                         g_want[player][1], wotm::settings::on(wotm::settings::Vibration) ? "on" : "off");
        }
        apply();
    }
    ctx->r[2] = _mm_set_epi64x(0, 1);
    ctx->pc = GPR_U32(ctx, 31);
}

// inputUseActuator(pad, use): always on (the option decides in actHook)
static void useHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == wotm::fn::inputUseActuator__Fib) ctx->r[5] = _mm_set_epi64x(0, 1);
    g_origUse(rdram, ctx, runtime);
}

// Turning the option on in the panel: a short buzz on every connected XInput pad, so the player feels it's on. After
// 0.25 s it goes back to what the game asks for (apply). The pulse bypasses send's cache.
static void pulse()
{
    const unsigned gen = ++g_pulseGen;
    for (unsigned long x = 0; x < 4; ++x)
    {
        XState st{};
        if (g_get(x, &st) != 0) continue;
        XVib vib{0x6000, 0x6000};
        g_set(x, &vib);
        g_last[x] = ~0u;
    }
    std::thread([gen] {
        std::this_thread::sleep_for(std::chrono::milliseconds(250));
        if (g_pulseGen.load() != gen) return;
        for (int x = 0; x < 4; ++x) g_last[x] = ~0u;
        apply();
    }).detach();
}

// After the option changes in the panel (wotm_mainmenu.inc): off stops the motors immediately; on gives the feedback pulse
static void poll()
{
    if (!g_set) return;
    const bool on = wotm::settings::on(wotm::settings::Vibration);
    if (on != g_wasOn)
    {
        g_wasOn = on;
        if (on)
            pulse();
        else
            apply();
    }
}

static void stopAll()
{
    if (!g_set) return;
    for (int x = 0; x < 4; ++x)
    {
        XVib vib{0, 0};
        g_set(static_cast<unsigned long>(x), &vib);
    }
}

static void install(PS2Runtime &runtime)
{
    // The hooks go in on every machine, with or without XInput: useHook changes what the game stores for each pad (its rumble
    // switch at 0x77A0C0), and that state must be the same on every PC of a network match. Only the motors need XInput.
    const bool xinput = loadXInput();
    if (!xinput)
        std::fprintf(stderr, "[rumble] XInput unavailable: no rumble\n");
    g_wasOn = wotm::settings::on(wotm::settings::Vibration);
    if ((g_origAct = runtime.lookupFunction(wotm::fn::scePadSetActDirect)))
        runtime.registerFunction(wotm::fn::scePadSetActDirect, &actHook);
    if ((g_origUse = runtime.lookupFunction(wotm::fn::inputUseActuator__Fib)))
        runtime.registerFunction(wotm::fn::inputUseActuator__Fib, &useHook);
    if (!xinput)
        return;
    std::atexit(stopAll);   // so no pad keeps rumbling after the game closes
    std::fprintf(stderr, "[rumble] rumble over XInput (%s; VIBRATION on the CONTROLS tab, WOTM_VIBRATION)\n", g_wasOn ? "on" : "off");
}
} // namespace wotm_rumble
