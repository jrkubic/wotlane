// wotm_pad.inc: which host pads and the keyboard feed each of the game's pads (game pad n = player n + 1, 0 to 3; 2 and 3 are
// multitap slot 1 on ports 0 and 1). Included by wotm_live.inc (global scope of ps2_runtime.cpp). The runtime (ps2_pad.cpp)
// queries g_ps2PadDeviceMap on every read.
// Connected pads are assigned in raylib's order (the first connected is player 1, the second player 2, up to the fourth),
// with no gaps: when a pad is unplugged, the next one moves up to take its place.
//   Default: the keyboard is the same player as the first pad (player 1).
//   WOTM_KBSEPARATE=1: the keyboard is a separate device, after the pads (no pad: player 1; one pad: player 2;
//   two pads: left over, unused).
//   Multiplayer pad selection (wotm_padsel.inc): while g_manual is set, player n gets only the devices that joined
//   that player's column (g_manualDevs[n], same bits as the map); devices left out don't play.
//   Network match (WOTM_NET): one player per PC; game pad 0 gets the keyboard and every host pad.
// Keyboard and mouse receivers that raylib lists as gamepads (Keychron Link) are excluded from the pad list.
// Keys and buttons for each PS2 button: the map from wotm_settings.hpp (CONTROLS tab of the panel), installed into the fork's
// ps2_pad.cpp (g_ps2PadKeyboard, g_ps2PadGamepadButtons); gamepad axes stay fixed.
namespace wotm_pad
{
static std::atomic<bool> g_manual{false};
static std::atomic<uint32_t> g_manualDevs[4] = {};

// Wireless keyboard and mouse receivers that raylib lists as gamepads (the Keychron Link shows up as gamepad 0): they don't count
// as pads, otherwise they would take player 1's place
static bool notAGamepad(int i)
{
    std::string name = GetGamepadName(i) ? GetGamepadName(i) : "";
    for (char &ch : name)
        ch = static_cast<char>(std::tolower(static_cast<unsigned char>(ch)));
    return name.find("keychron") != std::string::npos || name.find("keyboard") != std::string::npos || name.find("mouse") != std::string::npos;
}

// WOTM_NOINPUT=1: no host device reaches the game (determinism tests; the PS2X_PAD script still applies)
static bool noInput()
{
    static const bool on = [] { const char *e = std::getenv("WOTM_NOINPUT"); return e && e[0] == '1'; }();
    return on;
}

// Devices feeding the game's pad `port` (port + 2 * multitap slot: player port + 1)
static uint32_t devicesOf(int port)
{
    const bool kbSeparate = wotm::settings::on(wotm::settings::KbSeparate);   // live option (wotm_settings.hpp)
    // network match (WOTM_NET): one player per PC, fed by the keyboard and every pad (the network carries game pad 1)
    static const bool netplay = [] { const char *e = std::getenv("WOTM_NET"); return e && *e; }();
    if (noInput())
        return 0u;
    int pads[PS2_PAD_MAX_GAMEPADS];
    int n = 0;
    for (int i = 0; i < PS2_PAD_MAX_GAMEPADS; ++i)
        if (IsGamepadAvailable(i) && !notAGamepad(i))
            pads[n++] = i;

    uint32_t devs = 0;
    if (netplay)
    {
        if (port == 0)
        {
            devs = PS2_PAD_DEV_KEYBOARD;
            for (int i = 0; i < n; ++i)
                devs |= 1u << pads[i];
        }
    }
    else if (g_manual.load(std::memory_order_relaxed))
        devs = port >= 0 && port < 4 ? g_manualDevs[port].load(std::memory_order_relaxed) : 0u;
    else
    {
        if (port >= 0 && port < n)
            devs = 1u << pads[port];
        if (kbSeparate ? port == n : port == 0)
            devs |= PS2_PAD_DEV_KEYBOARD;
    }

    // one line per game pad when what feeds it changes (startup, pad plugged in or unplugged)
    static uint32_t last[4] = {~0u, ~0u, ~0u, ~0u};
    if (port >= 0 && port < 4 && devs != last[port])
    {
        last[port] = devs;
        std::string what;
        for (int i = 0; i < PS2_PAD_MAX_GAMEPADS; ++i)
            if (devs & (1u << i))
                what += "pad " + std::to_string(i) + " (" + GetGamepadName(i) + ")";
        if (devs & PS2_PAD_DEV_KEYBOARD)
            what += what.empty() ? "keyboard" : " + keyboard";
        std::fprintf(stderr, "[pad] player %d: %s\n", port + 1, what.empty() ? "no device" : what.c_str());
    }
    return devs;
}

// ------------------------------------------------------------------ key and button map (wotm_settings.hpp, CONTROLS tab)
namespace st = wotm::settings;
// Mouse wheel as a button: each notch (accumulated by the window in mouseFrame) becomes a press lasting one pad read, followed by one
// released read, so the edge counts once per notch. Advances once per keyboard read (keyboard).
static int g_wheelHeld = 0;   // +1 / -1 while pressed, 0 released
static int g_wheelGap = 0;
static void wheelStep()
{
    if (g_wheelHeld)
    {
        g_wheelHeld = 0;
        g_wheelGap = 1;
        return;
    }
    if (g_wheelGap > 0)
    {
        --g_wheelGap;
        return;
    }
    int w = wotm::mouse::g_wheel.load(std::memory_order_relaxed);
    while (w != 0 && !wotm::mouse::g_wheel.compare_exchange_weak(w, w > 0 ? w - 1 : w + 1, std::memory_order_relaxed)) {}
    if (w != 0)
        g_wheelHeld = w > 0 ? 1 : -1;
}
// A key, mouse button (kMouseBase + MouseButton) or wheel notch (kWheelUp/kWheelDown) from the keyboard map
inline bool inputDown(int code)
{
    if (code <= 0) return false;
    if (code == st::kWheelUp) return g_wheelHeld > 0;
    if (code == st::kWheelDown) return g_wheelHeld < 0;
    if (code >= st::kMouseBase && code < st::kMouseBase + 7) return IsMouseButtonDown(code - st::kMouseBase);
    return IsKeyDown(code);
}
inline bool kbDown(st::Bind b)
{
    const uint32_t v = st::kb(b);
    return inputDown(int(v & 0xFFFF)) || inputDown(int(v >> 16));
}
// Player (0..3) that gamepad `pad` feeds, by devicesOf's rule: under manual selection, the column it joined; otherwise, the order
// of connected pads. Anything else (or no gamepad): player 1. Picks that player's button map (wotm_settings.hpp, one per player).
inline int playerOfPad(int pad)
{
    if (pad < 0 || pad >= PS2_PAD_MAX_GAMEPADS)
        return 0;
    if (g_manual.load(std::memory_order_relaxed))
    {
        for (int p = 0; p < 4; ++p)
            if (g_manualDevs[p].load(std::memory_order_relaxed) & (1u << pad)) return std::min(p, st::kPadPlayers - 1);
        return 0;
    }
    int n = 0;
    for (int i = 0; i < PS2_PAD_MAX_GAMEPADS; ++i)
        if (IsGamepadAvailable(i) && !notAGamepad(i))
        {
            if (i == pad) return std::min(n, st::kPadPlayers - 1);
            ++n;
        }
    return 0;
}
inline bool padDown(int pad, st::Bind b, int player)
{
    const int btn = st::pad(b, player);
    return btn > 0 && IsGamepadButtonDown(pad, btn);
}

// In the menus: the screenGetInput hook (wotm_mainmenu.inc) stamps the time on every frame of a menu screen
static std::atomic<double> g_menuTime{-1.0};
inline void markMenuFrame() { g_menuTime.store(GetTime(), std::memory_order_relaxed); }
inline bool inMenus() { return GetTime() - g_menuTime.load(std::memory_order_relaxed) < 0.25; }

// Keyboard -> PS2 buttons (active-low bits in btns) and left stick (data[6] x, data[7] y), on every read of the keyboard player's
// pad. Keyboard actions (wotm_settings.hpp), in the game's control config 0 (CONTROL TYPE A):
//   toggle lock-on on (WOTM_LOCKTOGGLE, the mod swaps the buttons; wotm_overrides.cpp): lock R1, block L1, strafe L2/R2;
//   off (the game's own buttons): lock is L1+R1 held, block R2, strafe L1/R1.
// Target switch: a direct request to the lock-on (wotm_mouse.hpp), on the key's edge; the d-pad isn't used (released, it would walk).
// Esc on Start: in the menus it acts as triangle (back). Walk (Alt): the stick goes to ~40% of its travel.
// In the menus the mouse bypasses the map (step 9): right goes back (triangle), wheel moves up and down (d-pad), left confirms (X),
// except over a native menu that finds the item under the cursor (wotm_mainmenu.inc: the click belongs to it).
static void keyboard(uint16_t &btns, uint8_t *data)
{
    wheelStep();
    const bool menus = inMenus();
    const bool mouseMenu = menus || wotm::mouse::inGameMenu();   // shell menus or an in-game screen with mouse (pause)
    auto down = [mouseMenu](st::Bind b) {
        if (!mouseMenu) return kbDown(b);
        const uint32_t v = st::kb(b);
        const int a = int(v & 0xFFFF), c = int(v >> 16);
        return (a < st::kMouseBase && inputDown(a)) || (c < st::kMouseBase && inputDown(c));
    };
    for (int b = 0; b < st::kBinds; ++b)
        if (st::kBindMask[b] && down(st::Bind(b)))
        {
            uint16_t mask = st::kBindMask[b];
            if (b == st::BStart && menus && IsKeyDown(KEY_ESCAPE))
            {
                const uint32_t v = st::kb(st::BStart);   // Esc on Start: only Esc pressed alone becomes back
                const bool other = (int(v & 0xFFFF) != KEY_ESCAPE && inputDown(int(v & 0xFFFF))) ||
                                   (int(v >> 16) != KEY_ESCAPE && inputDown(int(v >> 16)));
                if (!other) mask = 0x1000;   // triangle
            }
            btns &= static_cast<uint16_t>(~mask);
        }
    if (menus && IsKeyDown(KEY_BACKSPACE))
        btns &= static_cast<uint16_t>(~0x1000u);   // Backspace (the Mac "delete" key) goes back in the menus too, like Esc (triangle)
    if (mouseMenu && !wotm::mouse::g_captured.load(std::memory_order_relaxed))
    {
        uint16_t m = 0;
        // click on an item of an in-game screen (wotm_pausemenu.inc): X for two reads, with the item already selected
        if (wotm::mouse::nowMs() > wotm::mouse::g_crossReqUntil.load(std::memory_order_relaxed))
            wotm::mouse::g_crossReq.store(0, std::memory_order_relaxed);   // stale request (wotm_mouse.hpp, requestCross)
        int req = wotm::mouse::g_crossReq.load(std::memory_order_relaxed);
        while (req > 0 && !wotm::mouse::g_crossReq.compare_exchange_weak(req, req - 1, std::memory_order_relaxed)) {}
        if (req > 0) m |= 0x4000;
        if (IsMouseButtonDown(MOUSE_BUTTON_RIGHT)) m |= 0x1000;                                  // triangle
        if (IsMouseButtonDown(MOUSE_BUTTON_LEFT) && !wotm::mouse::claimed()) m |= 0x4000;        // X
        const bool side = wotm::mouse::sideActive();   // screen that scrolls sideways (wotm_gamescreens.inc)
        if (g_wheelHeld && side && wotm::mouse::g_sideWheel.load(std::memory_order_relaxed))
            m |= g_wheelHeld > 0 ? 0x0080 : 0x0020;                                              // left / right
        else if (g_wheelHeld)
            m |= g_wheelHeld > 0 ? 0x0010 : 0x0040;                                              // up / down
        if (side && IsKeyDown(KEY_Q)) m |= 0x0080;                                               // Q / E: left / right
        if (side && IsKeyDown(KEY_E)) m |= 0x0020;
        btns &= static_cast<uint16_t>(~m);
    }
    constexpr uint16_t kL1 = 0x0400, kR1 = 0x0800, kL2 = 0x0100, kR2 = 0x0200;
    const bool toggle = st::on(st::LockToggle);
    uint16_t act = 0;
    if (down(st::BLock)) act |= toggle ? kR1 : uint16_t(kL1 | kR1);
    if (down(st::BBlock)) act |= toggle ? kL1 : kR2;
    if (down(st::BStrafeL)) act |= toggle ? kL2 : kL1;
    if (down(st::BStrafeR)) act |= toggle ? kR2 : kR1;
    btns &= static_cast<uint16_t>(~act);

    static bool prevNext = false, prevPrev = false;
    const bool next = kbDown(st::BTargetNext), prev = kbDown(st::BTargetPrev);
    const int kp = wotm::mouse::g_kbPlayer.load(std::memory_order_relaxed);
    if (kp >= 0 && kp < 4 && !menus && ((next && !prevNext) || (prev && !prevPrev)))
        wotm::mouse::g_targetReq[kp].fetch_add(next && !prevNext ? 1 : -1, std::memory_order_relaxed);
    prevNext = next;
    prevPrev = prev;

    // left stick (only while one of its keys is pressed; otherwise the gamepad's value or center stays)
    const bool l = down(st::BStickLeft), r = down(st::BStickRight), u = down(st::BStickUp), d = down(st::BStickDown);
    const uint8_t lo = down(st::BWalk) ? 0x4D : 0x00, hi = static_cast<uint8_t>(0xFF - lo);
    if (l != r) data[6] = l ? lo : hi;
    if (u != d) data[7] = u ? lo : hi;

    static const bool log = std::getenv("WOTM_KBLOG") != nullptr;   // one line per change in what the keyboard sends
    static uint16_t lastB = 0xFFFF;
    static uint8_t lastX = 0x80, lastY = 0x80;
    if (log && (btns != lastB || data[6] != lastX || data[7] != lastY))
    {
        std::fprintf(stderr, "[kb] t=%.2f buttons %04x stick %02x,%02x%s\n", GetTime(), unsigned(static_cast<uint16_t>(~btns)), data[6],
                     data[7], menus ? " (menus)" : "");
        lastB = btns;
        lastX = data[6];
        lastY = data[7];
    }
}
// MenuConfirm = 1 in the menus (gamepads): the X button gives triangle (back, in the game), circle gives X (confirm) and
// triangle gives circle. With the default map: the right face button confirms and the bottom one goes back, as on Nintendo consoles.
inline bool menuSwap() { return st::get(st::MenuConfirm) == 1 && inMenus(); }
// PS2 button that the physical button mapped to b produces right now (the menu swap)
inline st::Bind effectiveBind(st::Bind b)
{
    if (!menuSwap()) return b;
    return b == st::BCross ? st::BTriangle : b == st::BCircle ? st::BCross : b == st::BTriangle ? st::BCircle : b;
}
// The inverse: which mapped button gives PS2 button b right now (the icons show that one)
inline st::Bind sourceBind(st::Bind b)
{
    if (!menuSwap()) return b;
    return b == st::BTriangle ? st::BCross : b == st::BCross ? st::BCircle : b == st::BCircle ? st::BTriangle : b;
}
static uint16_t gamepadButtons(int pad)
{
    uint16_t m = 0;
    const int player = playerOfPad(pad);
    for (int b = 0; b < st::kBinds; ++b)
        if (st::bindOnPad(st::Bind(b)) && padDown(pad, st::Bind(b), player))
            m |= st::kBindMask[effectiveBind(st::Bind(b))];
    return m;
}

// Player (0..3) the keyboard feeds, by devicesOf's rule (-1: none, with WOTM_NOINPUT, under manual selection without the keyboard, or
// it's left over). The mouse belongs to the same player: with none, the modern camera never captures the cursor or takes its motion.
inline int keyboardPlayer()
{
    if (noInput())
        return -1;
    if (g_manual.load(std::memory_order_relaxed))
    {
        for (int p = 0; p < 4; ++p)
            if (g_manualDevs[p].load(std::memory_order_relaxed) & PS2_PAD_DEV_KEYBOARD) return p;
        return -1;
    }
    if (!st::on(st::KbSeparate)) return 0;
    int n = 0;
    for (int i = 0; i < PS2_PAD_MAX_GAMEPADS; ++i)
        if (IsGamepadAvailable(i) && !notAGamepad(i)) ++n;
    return n < 4 ? n : -1;
}

// Mouse on the modern camera (wotm_mouse.hpp), on every present on the window thread: cursor captured while the keyboard player's
// orbit is steering (signaled by the EE thread) and the window has focus; when captured, motion goes to the orbit. WOTM_MOUSECAM=0 turns it off.
static void mouseFrame()
{
    namespace ms = wotm::mouse;
    static const bool off = [] { const char *e = std::getenv("WOTM_MOUSECAM"); return e && e[0] == '0'; }();
    ms::g_kbPlayer.store(keyboardPlayer(), std::memory_order_relaxed);
    const bool want = !off && ms::wanted() && !ms::inGameMenu() && IsWindowFocused();   // in-game menu during a stage (pause, "save?"): released
    static bool captured = false;
    static int skip = 0;
    if (want != captured)
    {
        captured = want;
        if (want)
        {
            DisableCursor();
            skip = 2;   // DisableCursor recenters the cursor: the first delta is that jump
        }
        else
            EnableCursor();
        ms::clear();
        ms::g_captured.store(want, std::memory_order_relaxed);
        std::fprintf(stderr, "[mouse] cursor %s (signal %lld ms ago, focus %d)\n", want ? "captured (camera)" : "released",
                     static_cast<long long>(ms::nowMs() - (ms::g_wantUntil.load(std::memory_order_relaxed) - 150)), IsWindowFocused() ? 1 : 0);
    }
    if (!captured)
        return;   // released: wheel and clicks go through pointerFrame (menus)
    if (const float w = GetMouseWheelMove(); w != 0.f)
        ms::g_wheel.fetch_add(w > 0.f ? 1 : -1, std::memory_order_relaxed);
    const Vector2 d = GetMouseDelta();
    if (skip > 0)
    {
        --skip;
        return;
    }
    if (d.x != 0.f || d.y != 0.f)
        ms::add(d.x, d.y);
}

// Mouse in the menus (wotm_mouse.hpp), on every present, with the rectangle `dst` where the native frame (rw x rh) went in the window:
// with the cursor released and the window focused, where it is in the frame, whether it moved, the clicks (left and right) and the wheel.
static void pointerFrame(const Rectangle &dst, int rw, int rh)
{
    namespace ms = wotm::mouse;
    ms::Pointer &p = ms::g_ptr;
    p.rw = rw;
    p.rh = rh;
    if (ms::g_captured.load(std::memory_order_relaxed) || !IsWindowFocused() || dst.width <= 0.f || dst.height <= 0.f)
    {
        static int lastWhy = -1;
        const int why = ms::g_captured.load(std::memory_order_relaxed) ? 1 : !IsWindowFocused() ? 2 : 3;
        if (std::getenv("WOTM_MENUMOUSELOG") && why != lastWhy)
            std::fprintf(stderr, "[menumouse] pointer off (%s)\n", why == 1 ? "cursor captured" : why == 2 ? "window unfocused" : "no frame");
        lastWhy = why;
        p.in = false;
        ms::publishPointer(false, 0.f, 0.f, rw, rh);
        return;
    }
    const Vector2 m = GetMousePosition();
    const float x = (m.x - dst.x) * float(rw) / dst.width, y = (m.y - dst.y) * float(rh) / dst.height;
    const bool in = x >= 0.f && y >= 0.f && x < float(rw) && y < float(rh);
    static const bool log = std::getenv("WOTM_MENUMOUSELOG") != nullptr;
    if (log && in != p.in)
        std::fprintf(stderr, "[menumouse] cursor %s the frame (window %.0f,%.0f, frame %.0f,%.0f of %dx%d)\n", in ? "inside" : "outside", m.x, m.y,
                     x, y, rw, rh);
    if (in && p.in && (std::fabs(x - p.x) > 0.5f || std::fabs(y - p.y) > 0.5f))
        ms::g_moveSeq.fetch_add(1, std::memory_order_relaxed);
    p.x = x;
    p.y = y;
    p.in = in;
    ms::publishPointer(in, x, y, rw, rh);
    if (!in)
        return;
    if (IsMouseButtonPressed(MOUSE_BUTTON_LEFT)) ms::click(ms::g_clickL);
    if (IsMouseButtonPressed(MOUSE_BUTTON_RIGHT)) ms::click(ms::g_clickR);
    if (const float w = GetMouseWheelMove(); w != 0.f)
        ms::g_wheel.fetch_add(w > 0.f ? 1 : -1, std::memory_order_relaxed);
}

static const bool s_installed = (g_ps2PadDeviceMap = &devicesOf, g_ps2PadKeyboard = &keyboard, g_ps2PadGamepadButtons = &gamepadButtons,
                                 true);
}
