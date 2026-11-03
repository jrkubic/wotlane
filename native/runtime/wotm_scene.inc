// wotm_scene.inc (included by ps2_runtime.cpp; raylib and ps2_runtime.h are already included)
#include <atomic>
#include "wotm_savepics.hpp"   // save list images (wotm_live.inc draws them)
static std::atomic<bool> g_wotmShotKey{false};   // F8 in the window (wotm_foto.inc) -> one frame of the VU1 vs native check now (wotm_vu1check.inc)
#include "wotm_live.inc"
#include "wotm_foto.inc"   // F7 pause, F6 advance one frame, F8 snapshot with a note (WOTM_FOTOS)
#include "wotm_syms.h"   // wotm::sym / wotm::fn: ELF addresses by name (tools/native_syms.py --gen)
#include "wotm_saveicon.hpp"   // autosave icon

extern bool g_wotmVu1Tap;   // ps2_memory.cpp (VU1 vs native check)
static void wotmDrawScene(const uint8_t *rdram)
{
    if (!rdram) return;
    wotm_live::initWindowAspect();   // window at the widescreen aspect ratio (once, before the intro)
    wotm_live::fullscreenKey();      // F11: fullscreen <-> windowed
    {
        // Without VU1 nobody completes the packet: the game waits for `objsInPacket | objsInAlphaPacket` to reach zero (rtWaitForVu1, and a loop
        // inlined in particleDraw). Each present stands in for the VU1 completion. Not on the frame sampled by the VU1 vs native
        // check: there hierFlushObjQ kicks off the real DMA, and zeroing here threw away the alpha OT that hadn't been dispatched yet (the
        // 0x8000/0x4000 objects showed up as "only native draws", clue 7 of VU1CHECK).
        // In fixed step (WOTM_FIXEDSTEP, netcode) the window doesn't write to game RAM: the moment of the write depended on the host. The hooks
        // on rtWaitForVu1/hierFlushObjQ/particleDraw zero the same counters on the EE thread.
        static const bool s_noVu1 = std::getenv("PS2X_NO_VU1") != nullptr;
        static const bool s_lockstep = [] { const char *e = std::getenv("WOTM_FIXEDSTEP"); return e && *e; }();
        if (s_noVu1 && !g_wotmVu1Tap && !s_lockstep)
        {
            const uint32_t zero = 0;
            std::memcpy(const_cast<uint8_t *>(rdram) + wotm::sym::objsInPacket, &zero, 4);
            std::memcpy(const_cast<uint8_t *>(rdram) + wotm::sym::objsInAlphaPacket, &zero, 4);
        }
    }
    wotmFotoKeys();
    const bool drew = wotmDrawNative(rdram); // native layer (native/wotm_native.hpp): stage read from RAM, no VU1/GS
    {
        // With no native scene (loading, screen change) the window kept the GS frame. With PS2X_NO_GS the GS draws nothing: the
        // displayed area only has the texture uploads, and between the intro skip and the menu raw VRAM showed up (scrambled texture
        // blocks; also between fights). Here it stays black; WOTM_GSFRAME=1 shows the GS frame again (diagnostic).
        // A rectangle rather than ClearBackground: the GS frame is still in the raylib batch, and the immediate glClear came before it.
        static const bool s_blackGap = std::getenv("PS2X_NO_GS") != nullptr && std::getenv("WOTM_GSFRAME") == nullptr;
        if (!drew && s_blackGap)
            DrawRectangle(0, 0, GetScreenWidth(), GetScreenHeight(), BLACK);
    }
    {
        // Save icon (NATIVO.md, "Save in JSON"): 1 s in the corner each time save.json is written (wotm_save.inc)
        static const PS2Runtime::IoPaths &io = PS2Runtime::getIoPaths();
        static const std::filesystem::path card =
            (io.mcRoot.empty() ? (io.elfDirectory.empty() ? std::filesystem::current_path() : io.elfDirectory) / "mc0" : io.mcRoot) / "BASCUS-97197";
        wotm::saveicon::presenter().present(io.cdRoot, io.cdImage.string(), card);
    }
    wotmFotoOverlay(drew, rdram);
    {
        // WOTM_WINSHOTS=<s>[,<s>...]: the whole window (with whatever is drawn over the native frame) to win_<s>.png when the clock passes
        // each <s> seconds. WOTM_SHOTS only writes when the native scene is redrawn; this one also works in the Shell loops (dialog
        // after the fight, cinematics) and on the black "no scene" screen.
        static std::vector<double> shots = [] {
            std::vector<double> v;
            if (const char *s = std::getenv("WOTM_WINSHOTS"))
                for (const char *p = s; *p;)
                {
                    char *e = nullptr;
                    const double t = std::strtod(p, &e);
                    if (e == p) break;
                    v.push_back(t);
                    p = (*e == ',') ? e + 1 : e;
                }
            return v;
        }();
        static size_t next = 0;
        if (next < shots.size() && GetTime() >= shots[next])
        {
            char name[64];
            std::snprintf(name, sizeof name, "win_%g.png", shots[next]);
            rlDrawRenderBatchActive();   // the raylib batch hasn't reached the frame yet
            Image img = LoadImageFromScreen();
            if (ExportImage(img, name))
                std::fprintf(stderr, "[shot] %s (window, native scene %s)\n", name, drew ? "drawn" : "missing: black");
            UnloadImage(img);
            ++next;
        }
    }
}
#include "wotm_vu1check.inc"
