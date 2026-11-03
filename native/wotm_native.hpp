// WotM native layer: reads the scene straight from EE RAM and draws it with raylib (no VU1/GS).
//
// Used two ways: by the recompiled runtime (wotm_scene.inc, every frame, on the live RDRAM)
// and by the standalone viewer (native/viewer.cpp, on a `ram_<frame>.bin` dump).
// Formats: docs/FORMATOS.md. This file contains no game data.
//
// Game conventions (PS2): row vectors (p' = p * M), world is Z-up, float32 little-endian.
#pragma once
#include "wotm_syms.h"   // wotm::sym / wotm::fn: ELF addresses by name (tools/native_syms.py --gen)

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cctype>
#include <map>
#include <set>
#include <cstring>
#include <string>
#include <memory>
#include <unordered_map>
#include <vector>
#if defined(_M_X64) || defined(__SSE4_1__)
#include <smmintrin.h>   // dustTexture: SSE4.1 rasterization
#endif

#include "raylib.h"
#include "rlgl.h"
#include "wotm_gs.hpp"
#include "wotm_interp.hpp"   // M4, GameCamera and frame interpolation (no raylib, tested in native/tests)
#include "wotm_mods.hpp"   // .RTX palettes: mods/ wins over the disc (loadResources)
#include "wotm_texmods.hpp"   // mod textures as PNG (textureId, residentTexture)

namespace wotm {

// ---------------------------------------------------------------- RAM

struct Ram {
    const uint8_t *p = nullptr;
    size_t size = 0;

    bool ok(uint32_t a, uint32_t n = 4) const { return p && a >= 0x1000 && uint64_t(a) + n <= size; }
    uint32_t u32(uint32_t a) const { uint32_t v = 0; if (ok(a)) std::memcpy(&v, p + a, 4); return v; }
    uint16_t u16(uint32_t a) const { uint16_t v = 0; if (ok(a, 2)) std::memcpy(&v, p + a, 2); return v; }
    int16_t s16(uint32_t a) const { return int16_t(u16(a)); }
    uint8_t u8(uint32_t a) const { return ok(a, 1) ? p[a] : 0; }
    int8_t s8(uint32_t a) const { return int8_t(u8(a)); }
    float f32(uint32_t a) const { float v = 0; if (ok(a)) std::memcpy(&v, p + a, 4); return v; }
};

// Fixed addresses of the NTSC-U retail (SCUS_971.97): symbol_addrs.txt
namespace addr {
constexpr uint32_t world = wotm::sym::world;            // _worldctx *world (ep@0, skyCs@4, eo@0x20, weMat@0x30)
constexpr uint32_t worldCtx = wotm::sym::worldCtx;         // _worldctx worldCtx[5] (0x1F0 each)
constexpr uint32_t g_doFOVGroups = wotm::sym::g_doFOVGroups; // int: hierTraverseAsm culls GROUPs by sphere (Scene::outOfView)
constexpr uint32_t viewInfo = wotm::sym::viewInfo;         // 5 x 0x14: [0] = the camera's _cs*, [1] = viewport*
constexpr uint32_t worldToScreenMat = wotm::sym::worldToScreenMat; // 5 x 0x40
constexpr uint32_t paraL = wotm::sym::paraL;            // float[4][4]: rows x, y, z; columns = parallel lights 0-2 and the specular half-vector, x32
constexpr uint32_t vu1FogParams = 0x0025B280;     // float[4] (D_0025B280, VU1 qword 21): fogFA, fogMaxVal, 1, fogFB (fogUpdate)
constexpr uint32_t fogColorCtx = wotm::sym::fogColorCtx;      // u64[6]: the FOGCOL packet fogUpdate sends to the GS; [4] = R | G << 8 | B << 16
constexpr uint32_t vu1LightColor = 0x0025B238;    // float[4][4] (D_0025B238): color/128 of lights 0-2 (rows); row 3 is ambient/specular
constexpr uint32_t fieldsAtFrameStart = 0x006F8DC0;   // u32 (D_006F8DC0): field count at the start of the frame (timerWaitForMinUpdateRate)
constexpr uint32_t csActiveList = wotm::sym::x_6CsPool_m_activeList;     // CsPool::m_activeList (CsNode: cs@0, next@4)
constexpr uint32_t csHPActiveList = wotm::sym::x_6CsPool_m_HPActiveList;   // CsPool::m_HPActiveList
constexpr uint32_t tempVramTexAddr = wotm::sym::tempVramTexAddr;  // end of the resources (.RTX) in VRAM, in words; >> 6 = palette base block
constexpr uint32_t fileStatus = wotm::sym::fileStatus;      // FileStatus: resAddr@0x78, resLoaded@0xB0, names[14][9]@0xB1, maxTexAddr[14]@0x14C, maxResAddr[14]@0x168
constexpr uint32_t texInfo = wotm::sym::texInfo;         // texInfo[1000] x 16: [0] = the texture's upload packet (RAM), [4] = residency/VRAM
constexpr uint32_t fontPacket = 0x00735900;      // GIF packet with the frame's text (fontBuildPrim/fontDmaFontData): A+D header, then sprite runs
constexpr uint32_t psBlkLst = wotm::sym::psBlkLst;        // _pshadowblocker[10] x 0x30: cs@0x10, cs2@0x18, &origin@0x1C, &radius@0x20, factor@0x24, inUse u16@0x2E
constexpr uint32_t psLitDirWs = wotm::sym::psLitDirWs;      // shadow light direction (from the scene toward the light), world space
constexpr uint32_t psShadowIntens = wotm::sym::psShadowIntens;  // float, 80 = 0.625 (GS scale)
constexpr uint32_t psShadowOn = 0x006F8D7C;      // psSetShadowOnOff
}

// Game view that the collection and drawing in progress follow (split screen: views 0 and 1, the left and right 320 x 224 halves of
// the GS; src/common/view.cpp; with 3 and 4 players, views 0 to 3 in 320 x 112 quadrants, wotm_split4.inc). Per thread: collection runs on the EE thread and drawing on the window thread, each binds its own with ViewScope. Without
// a ViewScope, view 0 with the full screen applies (640 x 224, center 2048), as before split screen. cx/cy/w/h: center and size
// of the view in GS pixels (the view's viewDef: cx/cy at +0x18/+0x1C, w/h at +4/+6).
// 2 players with WOTM_LOCALPLAYER (wotm_multi.inc): both views use viewport 0 (full screen, 4:3) and presentation shows only the
// local player's. `grid` marks a frame with 2+ views in viewport 0 (without the unified view); the retail split-screen halves
// (viewport 1/2) and the 3- and 4-player quadrants don't count.
struct ViewCtx {
    int view = 0;
    float cx = 2048.f, cy = 2048.f, w = 640.f, h = 224.f;
    bool grid = false;
    // Aspect ratio the view has on screen (640 x 224 = 4:3; the 320 x 224 half = 2:3)
    float aspect() const { return (w / 640.f) * (4.f / 3.f) / (h / 224.f); }
    bool full() const { return w >= 640.f; }
    // The frame has other views (split screen, quadrants or one view per player): what the game keeps per view beats what
    // exists once per frame
    bool shared() const { return !full() || grid; }
    // Top-left corner of the view on the game screen (640 x 224)
    float left() const { return cx - 2048.f + 320.f - 0.5f * w; }
    float top() const { return cy - 2048.f + 112.f - 0.5f * h; }
    // WOTM_LOCALPLAYER (only the local player's view, full screen): a point projected in the view mapped to the whole screen (the enlarged view)
    void toFull(float &x, float &y) const {
        x = (x - left()) * 640.f / w;
        y = (y - top()) * 224.f / h;
    }
    // Fixed-size rectangle on screen (icon over the monster): the center moves like the projected point, the size stays
    void boxToFull(float &x0, float &y0, float &x1, float &y1) const {
        float cx2 = 0.5f * (x0 + x1), cy2 = 0.5f * (y0 + y1);
        const float ox = cx2, oy = cy2;
        toFull(cx2, cy2);
        x0 += cx2 - ox; x1 += cx2 - ox;
        y0 += cy2 - oy; y1 += cy2 - oy;
    }
    // HUD text: the game lays it out across the view's width (centered in it) and in 224 lines from its top, at the usual size; on
    // the whole screen it is centered and starts from the top
    void textToFull(float &x, float &y) const {
        x -= cx - 2048.f;
        y -= top();
    }
};
inline ViewCtx &curView() { static thread_local ViewCtx v; return v; }
struct ViewScope {
    ViewCtx prev;
    explicit ViewScope(const ViewCtx &v) : prev(curView()) { curView() = v; }
    ~ViewScope() { curView() = prev; }
    ViewScope(const ViewScope &) = delete;
    ViewScope &operator=(const ViewScope &) = delete;
};

// Draw profiling (WOTM_DRAWSTATS=1, with PS2X_STATS): time of each Scene::draw phase, batches flushed and vertices emitted,
// summed on the window thread; wotm_live.inc prints the average per drawn view on the [native] line and resets
struct DrawProf {
    enum Phase { Sky, Trav, AlphaOT, Shadows, ShadowTex, Sprites, Fx, Hud, Flush, Jobs,   // Flush and Jobs (vertices on the helper threads): inside the others
                 RShow, RView, RFlares, RFrame, kN };   // per redraw (wotmDrawNative): Scene::show, the whole drawView, deferred flares, the whole redraw
    long redraws = 0;
    double ms[kN] = {};
    long batches = 0, verts = 0, views = 0;   // batches: flushes (one vertex upload each)
    long draws = 0;                           // draws (glDrawArrays) from the VtxStream flushes
    long shadowHit = 0, shadowMiss = 0;   // silhouettes reused and redone (Scene::shadowTexture)
    long emitted = 0;                     // vertices actually emitted by drawList (after face culling and slots)
    long resident = 0, resUp = 0;         // vertices drawn from the resident arena and uploaded to it (Scene::residentDraw)
};
inline DrawProf &drawProf() { static DrawProf p; return p; }
inline bool drawProfOn() { static const bool on = std::getenv("WOTM_DRAWSTATS") != nullptr; return on; }
inline double profNowMs() { return std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now().time_since_epoch()).count(); }
// Adds the scope's time to phase `ph` (only with WOTM_DRAWSTATS)
struct ProfScope {
    DrawProf::Phase ph;
    double t0;
    explicit ProfScope(DrawProf::Phase p) : ph(p), t0(drawProfOn() ? profNowMs() : 0.0) {}
    ~ProfScope() { if (drawProfOn()) drawProf().ms[ph] += profNowMs() - t0; }
};
// Addresses for the current view: worldToScreenMat[v], worldCtx[v], viewInfo[v]
inline uint32_t viewW2S() { return addr::worldToScreenMat + 0x40u * uint32_t(curView().view); }
inline uint32_t viewCtxAddr() { return addr::worldCtx + 0x1F0u * uint32_t(curView().view); }
inline uint32_t viewInfoAddr() { return addr::viewInfo + 0x14u * uint32_t(curView().view); }
// Geometry of view `view` as its viewDef stands now (outside RAM or with no size: full screen)
inline ViewCtx readViewCtx(const Ram &ram, int view) {
    ViewCtx v;
    v.view = view;
    const uint32_t def = ram.u32(addr::viewInfo + 0x14u * uint32_t(view) + 4) & 0x1FFFFFFFu;
    if (ram.ok(def, 0x20) && ram.u16(def + 4) && ram.u16(def + 6)) {
        v.cx = float(int32_t(ram.u32(def + 0x18)));
        v.cy = float(int32_t(ram.u32(def + 0x1C)));
        v.w = ram.u16(def + 4);
        v.h = ram.u16(def + 6);
    }
    const uint32_t def0 = ram.u32(addr::viewInfo + 4) & 0x1FFFFFFFu;
    v.grid = ram.u32(wotm::sym::viewNumViews) >= 2u && ram.u32(wotm::sym::gUseUnifiedView) == 0u && ram.ok(def0, 8) && ram.u16(def0 + 4) >= 640u;
    return v;
}

// Upload packet of a texture (`texInfo[id]`): `texmSetupTexture` builds the SDK's `sceGsLoadImage` at +0x20
// (`sceGsSetDefLoadImage`): BITBLTBUF at +0x30 (DBW in bits 48..53, DPSM 56..61), TRXREG at +0x50 (RRW 0..11, RRH 32..43); linear
// pixels from +0x80. (Until 2026-10-15 the runtime stub wrote its own struct there, with width at +0x24, height at +0x26 and
// psm at +0x2B; RAM dumps from that period no longer work.)
struct UploadHdr {
    uint32_t w, h, psm, bw;
    static UploadHdr read(const Ram &ram, uint32_t ptr) {
        return {ram.u16(ptr + 0x50) & 0xFFFu, ram.u16(ptr + 0x54) & 0xFFFu, ram.u8(ptr + 0x37) & 0x3Fu, ram.u8(ptr + 0x36) & 0x3Fu};
    }
};

}  // namespace wotm

// Particles and effects: needs Ram (above); the Scene draws them after the translucent pass
#include "wotm_fx.hpp"

namespace wotm {

// ---------------------------------------------------------------- matrices (row vectors)

// a first, then b
inline M4 mul(const M4 &a, const M4 &b) {
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a.m[i][k] * b.m[k][j];
            r.m[i][j] = s;
        }
    return r;
}

inline M4 translateM(float x, float y, float z) {
    M4 r = ident();
    r.m[3][0] = x; r.m[3][1] = y; r.m[3][2] = z;
    return r;
}

inline M4 scaleM(float x, float y, float z) {
    M4 r = ident();
    r.m[0][0] = x; r.m[1][1] = y; r.m[2][2] = z;
    return r;
}

inline M4 readM4(const Ram &ram, uint32_t a) {
    M4 r{};
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) r.m[i][j] = ram.f32(a + 16 * i + 4 * j);
    return r;
}

inline void xform(const M4 &m, float x, float y, float z, float out[3]) {
    out[0] = x * m.m[0][0] + y * m.m[1][0] + z * m.m[2][0] + m.m[3][0];
    out[1] = x * m.m[0][1] + y * m.m[1][1] + z * m.m[2][1] + m.m[3][1];
    out[2] = x * m.m[0][2] + y * m.m[1][2] + z * m.m[2][2] + m.m[3][2];
}

// ---------------------------------------------------------------- mesh (polyPkt)

// A HierObject::polyPkt is a DMA sub-chain (tag QWC/ID=ret + 8 bytes) with a VIF stream for the VU1 microcode:
//   UNPACK V3-32 @0xB5   positions (up to 190)        UNPACK V3-8 @0x175   normals (signed /127)
//   per strip section (ended by MSCNT): GIF tag at H (V4-32, NLOOP = n vertices), then 3 qwords per vertex
//   (ST, RGBAQ, XYZF): S-8 @H+1 position index, V4-8 @H+2 RGBA (0x80 = 1.0), V2-16 @H+3 UV (4096 = 1.0).
//   STMASK 0xBFBFBFBF + S-8 @H+3+3k marks ADC (don't draw) on vertex k: restarts the strip.
constexpr uint8_t kSlotNoTex = 31;   // Vtx::slot of a section with PRIM.TME 0 in the GIF tag: the GS draws vertex color only

struct Vtx {
    float x, y, z;
    float u, v;
    uint8_t r, g, b, a;
    float w = 1.f;                    // weight of bone x (skinned objects only)
    uint8_t cx = 0, cy = 0, cz = 0;   // bone codes (qword address in VU1: slot = (code & 0x7F) / 4)
    uint8_t slot = 0;                 // gsTexCtx/texId index of the section (GIF tag word 3 = 1 + 3*slot; 0 = slot 0); kSlotNoTex = untextured
    uint8_t sub = 0;                  // sub-packet index in the chain (diagnostics)
    uint8_t isAdc = 0;                // triangle normally discarded by ADC (only with WOTM_SHOWADC)
    uint8_t cull = 0;                 // section with culling (bit 15 of the selector): VU1 discards back faces (Scene::backFacing)
    int8_t nx = 0, ny = 0, nz = 0;    // normal (V3-8 @0x175, same index as the position; magnitude ~127): per-vertex lighting (Scene::vertexRgb)
    uint8_t fge = 0;                  // the section's PRIM.FGE: the GS applies fog (Scene::draw, shader)
};

// OBJECT sprite (objType 2, flags +0xC bits 5-6): section with a SPRITE GIF tag at H and 6 qwords per sprite, no indices: ST0 @H+1,
// RGBA @H+2 (V4-8, stride 6), local center + side in w @H+3 (V4-32, stride 6), ST1 @H+4 (V2-16, stride 3 with ST0). The microprogram
// (0x2E58, `pktAddVu1ObjAsm` via table +0x158) projects the center and emits corners center -/+ (512, 256) * side / W GS pixels,
// with ST0/ST1 (/4096, + uvOff), packet color times q5 (capped at 255, unlit) and the fog F at the center. The power-up halo.
struct ObjSprite {
    float x, y, z, size;          // center in object space; side (the w of the V4-32)
    float u0, v0, u1, v1;         // ST of the (-, -) and (+, +) corners of the GS screen (4096 = 1.0)
    uint8_t r, g, b, a;           // raw GS RGBA (0x80 = 1.0)
    uint8_t slot = 0, fge = 0;    // like Vtx::slot / Vtx::fge
};

struct MeshData {
    std::vector<Vtx> tris;   // triangle list (3 vertices per triangle)
    std::vector<ObjSprite> sprites;   // SPRITE sections (ObjSprite): camera-facing, built at draw time
    uint32_t qwc = 0;
    uint32_t bytes = 0;      // skinned: from the polyPkt to the end of the last decoded sub-packet (Goliath's packet exceeds 128 KB)
    uint32_t sig = 0;       // cheap signature to detect RAM reused by another level
    uint16_t texId = 0;
    bool valid = false;
    bool lowAlpha = false;   // some vertex with low alpha (< 0x3C; 0x40 = opaque): mist/shadows, drawn in the translucent pass
    bool skinned = false;    // the polyPkt is a chain of sub-packets with V4-32 (bone codes + weight)
    bool abe = false;        // PRIM.ABE of the GIF tag: only then does vertex alpha count (blend); otherwise opaque
    uint8_t maxA = 0;        // largest vertex alpha (tris and sprites, raw: 0x80 = 1.0); together with the texture's it tells whether As exceeds 1
    uint32_t slotMask = 0;   // texture slots used by the sections (bit i = gsTexCtx[i])
    bool patched = false;    // copy altered by Scene::patchMesh (decoding again brings it back unpatched)
    uint8_t curSub = 0;   // sub-packet being decoded (diagnostics)
    uint8_t curSlot = 0;  // current GS texture: selector 0 in the tag = context unchanged (inherits the previous section's)
    uint8_t curTme = 1;   // PRIM.TME in effect (GIF tag with PRE; without PRE the GS keeps the previous PRIM)
    uint8_t curFge = 0;   // PRIM.FGE in effect (fog; same rule as TME)
    uint8_t curSprite = 0;   // SPRITE-type PRIM in effect (ObjSprite; same rule as TME)
    uint32_t posTot = 0, posUsed = 0;   // positions loaded / used by some index (diagnostics)
    uint32_t dropAdc = 0, dropDegen = 0;   // triangles discarded by ADC / degenerate (diagnostics)
    uint32_t secTot = 0, secBad = 0, secBadEx = 0;   // strip sections / with nloop != indices (diagnostics)
    uint32_t oobIdx = 0, totIdx = 0;   // vertex indices outside `pos` (become (0,0,0)) / total: decoding diagnostics
};

// Skips the zeroed qwords between sub-packets.
inline uint32_t skipZeroQwords(const Ram &ram, uint32_t pp) {
    uint32_t skip = 0;
    while (skip < 0x400 && ram.ok(pp + skip, 16) && ram.u32(pp + skip) == 0 && ram.u32(pp + skip + 4) == 0) skip += 16;
    return pp + skip;
}

// Cheap signature of the packet at `pp` (already past the zeroed qwords): tag, first and last word (skipping the last for skinned,
// whose end is only found by decoding). Scene::mesh revalidates the cache with it; a formula different from decodePacket's made the
// mesh get decoded again on every call, and the pointer returned earlier was left dangling.
inline uint32_t packetSig(const Ram &ram, uint32_t pp) {
    const uint32_t tag = ram.u32(pp), nwords = ((tag & 0xFFFF) + 1) * 4 - 2;
    return tag ^ ram.u32(pp + 8) ^ (ram.u32(pp + 12) * 31u) ^ ((tag & 0x10000) ? 0u : ram.u32(pp + 4 * (nwords + 1)) * 17u);
}

// Decodes one sub-packet into `md`; returns the address of the next one (skinned chain) or 0.
inline uint32_t decodePacket(const Ram &ram, uint32_t pp, MeshData &md, bool first) {
    pp = skipZeroQwords(ram, pp);
    if (!ram.ok(pp, 16)) return 0;
    const uint32_t tag = ram.u32(pp);
    if (((tag >> 28) & 7) != 6) return 0;
    const bool skinnedPkt = (tag & 0x10000) != 0;   // bit 16: V4-32 vertices with bone codes
    const uint32_t qwc = tag & 0xFFFF;
    uint32_t nwords = (qwc + 1) * 4 - 2;
    const uint32_t base = pp + 8;
    if (skinnedPkt) nwords = ram.size > base ? uint32_t(std::min<size_t>(60000, (ram.size - base) / 4)) : 0;   // end = two NOPs in a row
    if (nwords == 0 || (!skinnedPkt && !ram.ok(base, nwords * 4))) return 0;
    if (skinnedPkt) md.skinned = true;
    if (first) {
        md.qwc = qwc;
        md.sig = packetSig(ram, pp);
        md.valid = true;
    }
    uint32_t endPp = 0;

    struct P3 { float x, y, z, w; uint8_t cx, cy, cz; };
    std::vector<P3> pos;
    std::vector<std::array<int8_t, 3>> nrm;   // normals (V3-8 @0x175), by position index
    std::vector<char> usedPos;   // diagnostics: position referenced by some index
    uint32_t hdrAddr = 0, nloop = 0, hdrSel = 0;
    bool haveHdr = false;
    std::vector<uint8_t> idx;
    std::vector<std::array<uint8_t, 4>> rgba;
    std::vector<std::array<int16_t, 2>> uv;
    std::vector<uint32_t> adc;
    std::vector<std::array<float, 4>> spos;   // centers + side of the SPRITE sections
    uint32_t mask = 0;

    auto flush = [&]() {
        if (haveHdr && md.curSprite) {
            const uint32_t sel = hdrSel & 0x7FFF;
            if (sel >= 1 && sel <= 13) md.curSlot = uint8_t((sel - 1) / 3);
            const uint8_t slot = md.curTme ? md.curSlot : kSlotNoTex;
            const size_t n = std::min<size_t>({nloop, spos.size(), rgba.size(), uv.size() / 2});
            for (size_t k = 0; k < n; ++k) {
                ObjSprite s;
                s.x = spos[k][0]; s.y = spos[k][1]; s.z = spos[k][2]; s.size = spos[k][3];
                s.u0 = uv[2 * k][0] / 4096.f; s.v0 = uv[2 * k][1] / 4096.f; s.u1 = uv[2 * k + 1][0] / 4096.f; s.v1 = uv[2 * k + 1][1] / 4096.f;
                s.r = rgba[k][0]; s.g = rgba[k][1]; s.b = rgba[k][2]; s.a = rgba[k][3];
                s.slot = slot; s.fge = md.curFge;
                md.sprites.push_back(s);
                md.slotMask |= 1u << slot;
            }
        } else if (haveHdr && !idx.empty()) {
            const uint32_t n = std::min<uint32_t>(nloop, uint32_t(idx.size()));
            ++md.secTot;
            if (nloop != idx.size() || rgba.size() < n || uv.size() < n) { ++md.secBad; if (md.secBadEx < 4) { ++md.secBadEx; std::fprintf(stderr, "   section: nloop=%u idx=%zu rgba=%zu uv=%zu" "%c", nloop, idx.size(), rgba.size(), uv.size(), 10); } }
            std::vector<bool> restart(n + 1, false);
            for (uint32_t k : adc) if (k < n) restart[k] = true;
            { static const bool hs = std::getenv("WOTM_HDRSEL") != nullptr; if (hs) std::fprintf(stderr, "[hdrsel] tex=%u sel=%u n=%u\n", md.texId, hdrSel, n); }   // diagnostics
            std::vector<Vtx> strip;
            // ADC (GIF bit on vertex k): the GS does NOT draw the triangle ending at k, but the strip continues (the convention for
            // "new strip" is ADC on the first two vertices). Treating it as a restart lost one triangle per restart (holes).
            auto emit = [&]() {
                for (size_t k = 2; k < strip.size(); ++k) {
                    static const bool showAdc = std::getenv("WOTM_SHOWADC") != nullptr;
                    const bool adcTri = k < restart.size() && restart[k];
                    if (adcTri) { ++md.dropAdc; if (!showAdc) continue; }
                    const Vtx &a = strip[(k & 1) ? k - 1 : k - 2];
                    const Vtx &b = strip[(k & 1) ? k - 2 : k - 1];
                    const Vtx &c = strip[k];
                    // discard degenerate triangles (strips joined by a repeated vertex)
                    auto same = [](const Vtx &p, const Vtx &q) { return p.x == q.x && p.y == q.y && p.z == q.z; };
                    if (same(a, b) || same(b, c) || same(a, c)) { ++md.dropDegen; continue; }
                    { Vtx ta = a, tb = b, tc = c; ta.isAdc = tb.isAdc = tc.isAdc = adcTri ? 1 : 0; md.tris.push_back(ta); md.tris.push_back(tb); md.tris.push_back(tc); }
                }
                strip.clear();
            };
            for (uint32_t k = 0; k < n; ++k) {
                const uint32_t j = idx[k];
                ++md.totIdx; if (j >= pos.size()) ++md.oobIdx;
                if (j < pos.size()) { if (usedPos.size() < pos.size()) usedPos.resize(pos.size(), 0); usedPos[j] = 1; }
                const P3 p = j < pos.size() ? pos[j] : P3{0, 0, 0, 1.f, 0, 0, 0};
                Vtx v{};
                v.x = p.x; v.y = p.y; v.z = p.z; v.w = p.w; v.cx = p.cx; v.cy = p.cy; v.cz = p.cz;
                if (j < nrm.size()) { v.nx = nrm[j][0]; v.ny = nrm[j][1]; v.nz = nrm[j][2]; }
                const uint32_t sel = hdrSel & 0x7FFF;   // bit 15: section culling (buildings use 0x8001, 0x8004...)
                if (sel >= 1 && sel <= 13) md.curSlot = uint8_t((sel - 1) / 3);   // 0 = keep the previous texture
                v.slot = md.curTme ? md.curSlot : kSlotNoTex;
                v.fge = md.curFge;
                v.cull = uint8_t((hdrSel >> 15) & 1);
                v.sub = md.curSub;
                md.slotMask |= 1u << v.slot;
                if (k < rgba.size()) { v.r = rgba[k][0]; v.g = rgba[k][1]; v.b = rgba[k][2]; v.a = rgba[k][3]; }
                else { v.r = v.g = v.b = v.a = 128; }
                if (k < uv.size()) { v.u = uv[k][0] / 4096.f; v.v = uv[k][1] / 4096.f; }
                strip.push_back(v);
            }
            emit();
        }
        haveHdr = false; idx.clear(); rgba.clear(); uv.clear(); adc.clear(); spos.clear();
    };

    uint32_t i = 0;
    while (i < nwords) {
        const uint32_t w = ram.u32(base + 4 * i);
        const uint32_t cmd = (w >> 24) & 0x7F, num = (w >> 16) & 0xFF, imm = w & 0xFFFF;
        const uint32_t a = base + 4 * (i + 1);
        if (cmd >= 0x60) {
            const uint32_t vn = (cmd >> 2) & 3, vl = cmd & 3, n = num ? num : 256, addr = imm & 0x3FF;
            static const uint32_t comps[4] = {1, 2, 3, 4}, bits[4] = {32, 16, 8, 5};
            const uint32_t nw = vl == 3 ? (n * 16 + 31) / 32 : (n * bits[vl] * comps[vn] + 31) / 32;
            if (vn == 2 && vl == 0 && addr == 0xB5) {                        // V3-32 positions
                if (haveHdr) flush();
                pos.resize(n);
                for (uint32_t k = 0; k < n; ++k) pos[k] = {ram.f32(a + 12 * k), ram.f32(a + 12 * k + 4), ram.f32(a + 12 * k + 8), 1.f, 0, 0, 0};
            } else if (vn == 3 && vl == 0 && addr == 0xB5 && n > 1) {         // V4-32 (skinned): bone codes in the low 8 bits of x/y/z, weight in w
                if (haveHdr) flush();
                pos.resize(n);
                for (uint32_t k = 0; k < n; ++k) {
                    const uint32_t o = a + 16 * k;
                    pos[k] = {ram.f32(o), ram.f32(o + 4), ram.f32(o + 8), ram.f32(o + 12),
                              uint8_t(ram.u32(o) & 0xFF), uint8_t(ram.u32(o + 4) & 0xFF), uint8_t(ram.u32(o + 8) & 0xFF)};
                }
            } else if (vn == 2 && vl == 2 && addr == 0x175) {                 // V3-8 normals (arrive along with the positions)
                nrm.resize(n);
                for (uint32_t k = 0; k < n; ++k) nrm[k] = {int8_t(ram.u8(a + 3 * k)), int8_t(ram.u8(a + 3 * k + 1)), int8_t(ram.u8(a + 3 * k + 2))};
            } else if (haveHdr && md.curSprite && vn == 3 && vl == 0 && addr == hdrAddr + 3) {   // V4-32 centers + side (SPRITE)
                spos.resize(n);
                for (uint32_t k = 0; k < n; ++k) spos[k] = {ram.f32(a + 16 * k), ram.f32(a + 16 * k + 4), ram.f32(a + 16 * k + 8), ram.f32(a + 16 * k + 12)};
            } else if (haveHdr && md.curSprite && vn == 1 && vl == 1 && addr == hdrAddr + 1) {   // V2-16 ST0/ST1 (SPRITE)
                uv.resize(n);
                for (uint32_t k = 0; k < n; ++k) uv[k] = {ram.s16(a + 4 * k), ram.s16(a + 4 * k + 2)};
            } else if (vn == 3 && vl == 0 && n == 1) {                        // V4-32: GIF tag
                hdrAddr = addr; nloop = ram.u32(a) & 0x7FFF; haveHdr = true;
                hdrSel = ram.u32(a + 12);   // high word of REGS: the game uses it as a texture selector (1 + 3*slot)
                if ((ram.u32(a + 4) >> 14) & 1) md.curTme = uint8_t((ram.u32(a + 4) >> 19) & 1);   // PRE: PRIM in bits 47-57 (TME = bit 4)
                if ((ram.u32(a + 4) >> 14) & 1) md.curFge = uint8_t((ram.u32(a + 4) >> 20) & 1);   // FGE = bit 5 of PRIM
                if ((ram.u32(a + 4) >> 14) & 1) md.curSprite = uint8_t(((ram.u32(a + 4) >> 15) & 7) == 6);   // type 6 = SPRITE
                if (((ram.u32(a + 4) >> 15) >> 6) & 1) md.abe = true;
            } else if (haveHdr && vn == 0 && vl == 2 && (mask & 0xFF) == 0xBF) {   // S-8 under mask: ADC
                for (uint32_t k = 0; k < n; ++k) {
                    const int e = int(addr) - int(hdrAddr) - 3 + 3 * int(k);
                    if (e >= 0 && e % 3 == 0) adc.push_back(uint32_t(e / 3));
                }
            } else if (haveHdr && vn == 0 && vl == 2 && addr == hdrAddr + 1) {      // S-8 indices
                idx.resize(n);
                for (uint32_t k = 0; k < n; ++k) idx[k] = ram.u8(a + k);
            } else if (haveHdr && vn == 3 && vl == 2 && addr == hdrAddr + 2) {      // V4-8 RGBA
                rgba.resize(n);
                for (uint32_t k = 0; k < n; ++k) rgba[k] = {ram.u8(a + 4 * k), ram.u8(a + 4 * k + 1), ram.u8(a + 4 * k + 2), ram.u8(a + 4 * k + 3)};
            } else if (haveHdr && vn == 1 && vl == 1 && addr == hdrAddr + 3) {      // V2-16 UV
                uv.resize(n);
                for (uint32_t k = 0; k < n; ++k) uv[k] = {ram.s16(a + 4 * k), ram.s16(a + 4 * k + 2)};
            }
            i += 1 + nw;
            continue;
        }
        uint32_t extra = 0;
        if (cmd == 0x20) { mask = ram.u32(a); extra = 1; }
        else if (cmd == 0x30 || cmd == 0x31) extra = 4;
        else if (cmd == 0x4A) extra = (num ? num : 256) * 2;
        else if (cmd == 0x50 || cmd == 0x51) extra = (imm ? imm : 65536) * 4;
        else if (cmd == 0x14 || cmd == 0x15 || cmd == 0x17) flush();   // MSCAL/MSCALF/MSCNT: end of section
        else if (cmd == 0 && skinnedPkt && i > 8 && ram.u32(a) == 0) {  // two NOPs in a row: end of the skinned sub-packet
            endPp = (base + 4 * (i + 2) + 15) & ~15u;
            break;
        }
        i += 1 + extra;
    }
    flush();
    md.posTot += uint32_t(pos.size());
    for (char c : usedPos) md.posUsed += c ? 1u : 0u;
    return endPp;
}

inline MeshData decodeObject(const Ram &ram, uint32_t node) {
    MeshData md;
    uint32_t pp = ram.u32(node + 4) & 0x0FFFFFFF;
    if (!ram.ok(pp, 16)) return md;
    md.texId = ram.u16(node + 0x50);
    const uint32_t start = pp;
    for (int guard = 0; pp && guard < 256; ++guard) {
        md.curSub = uint8_t(guard);
        pp = decodePacket(ram, pp, md, guard == 0);
        if (pp) md.bytes = pp - start;
    }
    for (const Vtx &v : md.tris) if (v.a < 0x3C) { md.lowAlpha = true; break; }
    for (const Vtx &v : md.tris) md.maxA = std::max(md.maxA, v.a);
    for (const ObjSprite &s : md.sprites) md.maxA = std::max(md.maxA, s.a);
    return md;
}

// ---------------------------------------------------------------- scene

// Alpha test of the Scene::draw shader when nothing brings another one (TEST_1 0x8D: alpha > 8). In the object traversal the initial
// state comes from the game's ctxDef (Scene::collect: 0x5040C, no test); this threshold is for the passes outside hier (shadows, debris, HUD).
// kGsAlphaNoTest: TEST_1 with ATE = 0 (no test: the GS draws every pixel, including As = 0; the 0x5040C additive glows depend on this).
inline constexpr int16_t kGsAlphaRef = 8, kGsAlphaNoTest = -1;

// 2D button icons in place of McButtons' 3D objects (Scene::drawButtonSprites; the runtime installs it, runtime/wotm_prompts.inc). id = the
// object's (0x1F51.. or 0x240E..); mid = center in the world; right/up = camera axes; size = side in the world; alpha = fade. Draws inside
// the frame's 3D mode. g_buttonIconsOn: whether the 2D ones apply now (player option); with no function, the game's 3D ones.
using ButtonIconFn = void (*)(uint16_t id, const float mid[3], const float right[3], const float up[3], float size, float alpha);
inline ButtonIconFn g_buttonIconFn = nullptr;
inline bool (*g_buttonIconsOn)() = nullptr;

struct DrawItem {
    M4 m;
    uint32_t node;   // HierObject
    uint32_t animPkt = 0;   // CHAR_INSTANCE::animPktPtr (matrix palette of skinned objects)
    int lodIdx = 0;         // index of the chosen LOD (diagnostics; the palette chain is animPktPtr[packetPtrIdx], Scene::palette)
    std::shared_ptr<std::vector<M4>> pal;   // copy of the palette's 31 matrices (filled by takeSnapshot; empty = read from RAM)
    uint8_t blend = kGsAlphaNormal;   // low byte of ALPHA_1 of the nearest node with a ctx (GROUP, TRANSLATE...; Scene::CtxScope, gsBlend)
    float fade = 1.f;       // LOD cross-fade (hierLod): multiplies alpha; < 1 goes to the translucent pass
    float uvOff[2] = {0, 0};   // UV offset (qword 4 of the VU1 header, added to the normalized UV): Scene::objectUvOffset
    float color[4] = {1, 1, 1, 1};   // vertex color multiplier (qword 5, vf31; without the fade, which lives in `fade`): Scene::objectColor
    float spec = 0.f;  // VU1 specular (qword 20) that applies to this object: Scene::walk (inherited from the last one with specLightMe)
    // Parallel lights in object space (x32; columns 0-2 and the half-vector in [3]) when the object sits below a SCALE that corrects
    // the light (Scene::lightScale): hier doesn't rotate paraL, it uses the level's lightDir table (VU1 q6-q8). Without that, ownLight = false.
    bool ownLight = false;
    float lightObj[4][3] = {};
    uint8_t env = 0;   // environment map (Scene::objectEnv): 0 = packet UV, 1 = regular map, 2 = video/previous frame, 3 = CLOAK
    float envK = 0.f;  // weight of the normal in the environment map UV (envMapWeight or CLOAK's)
    uint16_t hudId = 0;   // only in Scene::hud: id of the HUD element (from the _cs's epNode)
    uint16_t btnId = 0;   // only in overlay/hpOverlay: id of the button icon (Scene::drawButtonSprites)
    uint32_t cs = 0;      // _cs whose traversal emitted the object (0 for the stage, like cs_): shadow receiver (psObjectCull)
    int16_t aref = kGsAlphaRef;   // alpha test from TEST_1 of the GROUP containing the object: discards As * 128 <= aref (Scene::groupState; kGsAlphaNoTest = no test)
    bool zmsk = false;   // ZBUF_1.ZMSK of the GROUP containing the object: doesn't write depth (only applies in the translucent pass; Scene::groupState)
    bool lodX = false;   // inside a LOD cross-fade (both LODs, including the one at fade 1): with bit 0x4000 it goes to the alpha OT
    // Point lights touching the object's sphere (Scene::objectPointLights, VU0 program 0x748): ptN > 0 swaps the light program
    // for VU1's 0x15D8. ptRel = -(light position in object space), ptCol = color, ptFall = falloff (PointLight +0x2C).
    uint8_t ptN = 0;
    float ptRel[3][3] = {};
    float ptCol[3][3] = {};
    float ptFall[3] = {};
    uint32_t palBlk = 0;   // address of the copied palette block (takeSnapshot): interpolation only blends palettes from the same block
};

struct Camera {
    float pos[3] = {0, 0, 0};
    float yaw = 0, pitch = 0;   // radians; yaw 0 looks toward +X, Z up
};

// Game camera for one view: what viewUpdate wrote (worldToScreenMat and worldCtx[view].eo). Without a valid matrix (menus), the
// `_cs` of viewInfo[view] (position at +0x10, matrix at +0x20: the rows are the eye axes in world coordinates = right,
// FORWARD, up). worldToScreenMat has focal 384 over 640 px (HFOV ~79.6 degrees) and, at 4:3, the VFOV is ~63.7 degrees (the eye's
// depth axis is Y).
inline GameCamera readGameCamera(const Ram &ram, int view = 0) {
    GameCamera c;
    const uint32_t cs = ram.u32(addr::viewInfo + 0x14 * view);
    if (!ram.ok(cs, 0x60)) return c;
    for (int i = 0; i < 3; ++i) {
        c.pos[i] = ram.f32(cs + 0x10 + 4 * i);
        c.fwd[i] = ram.f32(cs + 0x20 + 16 * 1 + 4 * i);
        c.up[i] = ram.f32(cs + 0x20 + 16 * 2 + 4 * i);
    }
    // worldToScreenMat does NOT store the focal length in fixed elements: row 0 is 384*right + 2048*forward and row 1 is -180.48*up + 2048*forward
    // (the game bakes in the camera axes). Reading [0][0]/[1][2] raw gave a FOV that changed with camera orientation (zoom in/out while turning).
    const uint32_t wm = addr::worldToScreenMat + 0x40 * view;
    // Only the matrix itself: row 3 = forward (unit); the vertical focal is the part of row 1 perpendicular to it. Mixing with a _cs read
    // at another moment gave 1-2 degree FOV spikes on torn reads (the game rewrites the matrix and the _cs at different times).
    float r1[3], r3[3], d13 = 0.f, n3 = 0.f;
    for (int i = 0; i < 3; ++i) { r1[i] = ram.f32(wm + 0x10 + 4 * i); r3[i] = ram.f32(wm + 0x30 + 4 * i); d13 += r1[i] * r3[i]; n3 += r3[i] * r3[i]; }
    float fv = 0.f;
    const float f0 = n3 > 0.5f ? 1.f : 0.f;
    if (n3 > 0.5f) {
        for (int i = 0; i < 3; ++i) { const float p = r1[i] - d13 / n3 * r3[i]; fv += p * p; }
        fv = std::sqrt(fv);
    }
    if (f0 > 0.5f && fv > 1.f) {
        // view height in field lines: 224, except in the 3- and 4-player quadrants (112), whose viewDef lives above the game's
        // 32 MB (wotm_split4.inc). The retail viewDefs stay at 224 (the menu's has 448 and the loading one 6, and the math is the usual)
        const uint32_t def = ram.u32(addr::viewInfo + 0x14 * view + 4) & 0x1FFFFFFFu;
        const float vh = def >= 0x2000000u && ram.ok(def, 8) && ram.u16(def + 6) ? float(ram.u16(def + 6)) : 224.f;
        c.fovy = 2.f * std::atan(vh / (2.f * fv)) * 180.f / 3.14159265f;
        // Eye and axes from what viewUpdate wrote along with the matrix (worldCtx[view].eo = -cs->trans, at +0x20), not from the _cs: in
        // the fight intro the game puts another camera in that _cs for an instant mid-frame, and the read caught that instant
        // (whole or half: the screen cut to another corner of the map for one frame).
        const uint32_t eo = addr::worldCtx + 0x1F0 * view + 0x20;
        float un = 0.f;
        for (int i = 0; i < 3; ++i) {
            c.pos[i] = -ram.f32(eo + 4 * i);
            c.fwd[i] = r3[i] / std::sqrt(n3);
            c.up[i] = -(r1[i] - d13 / n3 * r3[i]);   // row 1 = -180.48*up + 2048*forward
            un += c.up[i] * c.up[i];
        }
        for (int i = 0; i < 3; ++i) c.up[i] /= std::sqrt(un);
    }
    c.ok = std::isfinite(c.pos[0]) && std::isfinite(c.fwd[0]) && (std::fabs(c.fwd[0]) + std::fabs(c.fwd[1]) + std::fabs(c.fwd[2])) > 0.1f;
    return c;
}

// Particle drawn as a camera-facing sprite (debris, dust...). Read from fx[] (0x5BA280, 98 x 12000 bytes).
struct Sprite {
    float pos[3] = {0, 0, 0};
    float size = 1.f;       // side of the quad in world units
    float rot = 0.f;        // radians, around the view axis
    float rgba[4] = {1, 1, 1, 1};
    uint32_t texId = 0;     // index into texInfo (0 = procedural soft circle)
    uint64_t tex0 = 0;      // the effect's TEX0 (psm, palette CBP)
    float uv[4] = {0, 0, 1, 1};   // normalized u0, v0, u1, v1 ((u0, v0) at the top-left corner of the screen)
    uint32_t gsCol = 0;     // debris: GS RGBA (0x80 = 1.0) from the packet's RGBAQ; 0 = use rgba
    int16_t aref = 8;       // debris: alpha test threshold (Scene::setAlphaRef); kGsAlphaRef = default
    bool opaque = false;    // debris without ABE with alphaContext 0: writes depth (ZBUF from setOpeningContext)
    uint32_t key = 0;       // fx * 4096 + index: matches the same particle across two frames
    // particleMove step (Particles::stepBack): pos_B - pos_A = f (step - (0, 0, grav f)) over f fields; zero for one born during the step
    float step[3] = {0, 0, 0};
    float grav = 0.f;
};

// Glyph of the game's text (fontSpritePrint*): one GS sprite. UV in atlas texels, XY in game-screen pixels (640 x 224, field).
struct TextQuad {
    float u0, v0, u1, v1;
    float x0, y0, x1, y1;
    uint8_t r, g, b, a;   // GS RGBAQ: 0x80 = 1.0 (the color modulates the texture)
    uint64_t tex0;        // TEX0 in effect (T8H 512x512 atlas from the stage's .RTX + CBP palette)
};

// Untextured screen rectangle (fade): a GS SPRITE with ABE and ALPHA_1 0x44 ((Cs - Cd)·As + Cd, As = a/0x80) over the view, built
// as a GIF packet by rtFadeScreen, TheGame::updateFadeOut/updateFadeOutAndIn, uiFadeScreen and Shell::FadeScreen. XY on the game
// screen (640 x 224, field), like the text.
struct ScreenRect {
    float x0, y0, x1, y1;
    uint8_t r, g, b, a;   // GS RGBAQ: a = 0x80 covers everything
    // Went to the GS before the frame's text (gsPkt queue dispatched before fontDmaFontData, as in ShellRtLoop::endField for the
    // end-of-stage dialogs): the text goes on top. In rtMain the queue goes out after the text and the fade covers everything (false).
    bool underText = false;
    // Health bar over the enemy (wotmBarRect): view it was built in (-1 = not a bar). In split screen it goes to that view's half of
    // the window and is clipped to it, without stretching to the edge like the fade and the cinema bars
    int8_t barView = -1;
    // Bar with a resident texture (enemy portrait, wotmBarTex): TEX0 of the .RTX image (0 = flat color) and the texel region
    uint64_t tex0 = 0;
    float u0 = 0.f, v0 = 0.f, u1 = 0.f, v1 = 0.f;
    // Bar: index in Snapshot::anchors of the health icon it replaces (-1 = none); photo mode carries it along with the head
    int16_t anchor = -1;
};

// Health icon over a monster (Hud::drawHealthIcon): the world point the game projected (head) and the icon's center on the game
// screen (640 x 224) as it came out. Photo mode (wotm_photo.inc) reprojects the point with its own camera and moves the bars and the icon.
struct HudAnchor {
    float x = 0.f, y = 0.f;   // center on the game screen
    float w[3] = {0.f, 0.f, 0.f};
    int8_t view = 0;
};

// Light with a lens flare (HierLight, op 5, isOn and hasLensFlare) found during collection, in the world: p = position . node matrix, beam = beamNormal .
// the same matrix (rotation only). hierLightNode does the same with the matrix stack and the frame's eye; photo mode (wotm_photo.inc)
// rebuilds the lens flare list with them from the free camera, because the paused game only has the ones that were on screen.
struct WorldLight {
    float p[3] = {0, 0, 0}, beam[3] = {0, 0, 0};
    uint32_t node = 0, cs = 0;    // HierLight node and _cs where collection found it (diagnostics)
    uint32_t flags = 0, id = 0;   // light flags (low byte: 1 isOn, 2 hasLensFlare, 8 isDirectional, 0x10 bidirectional, 0x80 at infinity)
    float intensity = 0.f, rgb[3] = {0, 0, 0}, bw = 0.f;
};
// Particle emitter (HierParticleEmitter, op 22) walked during collection: preset and base in the world, like the preview's Model::emitters
// (e = position . matrix; r0/r2 = rows 0 and 2 of StaticMat = mathfMatrixFromNormal(n) carried by the matrix).
struct WorldEmitter { uint32_t node = 0, preset = 0; float e[3] = {0, 0, 0}, r0[3] = {0, 0, 0}, r2[3] = {0, 0, 0}; };

// Center (GS pixels, viewGetCenter) and size (viewGetWH) of view `view`, from its viewDef.
inline bool viewGeom(const Ram &ram, int view, int &cx, int &cy, int &w, int &h) {
    const uint32_t def = view >= 0 && view < 5 ? ram.u32(addr::viewInfo + 0x14 * uint32_t(view) + 4) & 0x1FFFFFFFu : 0;
    if (!ram.ok(def, 0x20)) return false;
    cx = int(ram.u32(def + 0x18)); cy = int(ram.u32(def + 0x1C)); w = ram.u16(def + 4); h = ram.u16(def + 6);
    return true;
}
// Flat rectangle of a fade as the emitter writes it into the packet (XYZ2 in 1/16 with 16 bits, RGBAQ 32 bits per channel, color
// capped at 255): corners in GS pixels, converted to the game screen (640 x 224) like the text.
inline ScreenRect fadeRect(int x0, int y0, int x1, int y1, uint32_t r, uint32_t g, uint32_t b, uint32_t a) {
    constexpr float kOx = 2048.f - 320.f, kOy = 2048.f - 112.f;
    auto px = [](int v, float o) { return float(uint32_t(v * 16) & 0xFFFF) / 16.f - o; };
    ScreenRect o{};
    o.x0 = px(x0, kOx); o.y0 = px(y0, kOy); o.x1 = px(x1, kOx); o.y1 = px(y1, kOy);
    o.r = uint8_t(std::min(255u, r)); o.g = uint8_t(std::min(255u, g)); o.b = uint8_t(std::min(255u, b)); o.a = uint8_t(std::min(255u, a));
    return o;
}
// The whole-view fade that rtFadeScreen, uiFadeScreen, TheGame::updateFadeOut/updateFadeOutAndIn and Shell::FadeScreen build:
// corner (cx - w/2, cy - h/2), size w x h.
inline bool viewFadeRect(const Ram &ram, int view, uint32_t r, uint32_t g, uint32_t b, uint32_t a, ScreenRect &out) {
    int cx, cy, w, h;
    if (!viewGeom(ram, view, cx, cy, w, h)) return false;
    const int x0 = cx - (w >> 1), y0 = cy - (h >> 1);
    out = fadeRect(x0, y0, x0 + w, y0 + h, r, g, b, a);
    return true;
}


// ScreenPolys screen quads (HUD outside hier: Hud::drawHealthIcon's health icon). Each source in s_sourceObjects (0x20
// bytes: pkt, end, numPolys, headerQw, curQw, obj, rgba, fx) has a GIF packet in s_polyBuffer: polyInitPacket puts the A+D with the
// object's TEX0 at pkt + 0x30 (ALPHA_1 0x44, TEST_1 0x8D: alpha > 8) and polyBuildPrim appends, from pkt + 16·headerQw, 5 qwords
// per quad: RGBAQ, UV, XYZ2, UV, XYZ2 (textured SPRITE, UV in 12.4 texels). dmaData closes the tags, sends and zeroes numPolys:
// read it with numPolys > 0, that is, at dmaData's entry (hook) or before it (the replay collection, at hier's entry, already has them:
// Hud::addHealthIndicators runs first). XY like the text: game screen 640 x 224, GS center at 2048.
inline void readScreenPolys(const Ram &ram, std::vector<TextQuad> &out) {
    auto u64 = [&](uint32_t a) { return uint64_t(ram.u32(a)) | (uint64_t(ram.u32(a + 4)) << 32); };
    const int n = int(ram.u32(sym::s_numSourceObjects));
    for (int s = 0; s < n && s < 8; ++s) {   // s_sourceObjects: 0x100 bytes = 8 sources
        const uint32_t so = sym::s_sourceObjects + 0x20 * uint32_t(s);
        const uint32_t pkt = ram.u32(so), polys = ram.u32(so + 8), hdr = ram.u32(so + 0xC);
        if (!polys || polys > 256 || !ram.ok(pkt + 16 * (hdr + 5 * polys)) || (u64(pkt + 0x38) & 0xFF) != 0x06) continue;
        const uint64_t tex0 = u64(pkt + 0x30);
        for (uint32_t i = 0; i < polys; ++i) {
            const uint32_t q = pkt + 16 * (hdr + 5 * i);
            constexpr float kOx = 2048.f - 320.f, kOy = 2048.f - 112.f;
            TextQuad t{};
            t.r = uint8_t(std::min(255u, ram.u32(q))); t.g = uint8_t(std::min(255u, ram.u32(q + 4)));
            t.b = uint8_t(std::min(255u, ram.u32(q + 8))); t.a = uint8_t(std::min(255u, ram.u32(q + 12)));
            t.u0 = float(ram.u32(q + 16) & 0x3FFF) / 16.f; t.v0 = float(ram.u32(q + 20) & 0x3FFF) / 16.f;
            t.x0 = float(ram.u32(q + 32) & 0xFFFF) / 16.f - kOx; t.y0 = float(ram.u32(q + 36) & 0xFFFF) / 16.f - kOy;
            t.u1 = float(ram.u32(q + 48) & 0x3FFF) / 16.f; t.v1 = float(ram.u32(q + 52) & 0x3FFF) / 16.f;
            t.x1 = float(ram.u32(q + 64) & 0xFFFF) / 16.f - kOx; t.y1 = float(ram.u32(q + 68) & 0xFFFF) / 16.f - kOy;
            t.tex0 = tex0;
            out.push_back(t);
        }
    }
}

// One ScreenPolys quad from the call that builds it, without reading s_polyBuffer: at the entry of polyBuildPrim(src, x, y, pd, pkt) redoes
// resolveFX (the source's and quad's fx: center, mirror in x from the view's right edge, swap of u and v; position relative
// to the current view's corner) and the sprite it writes (source color; V = 0x80 - v). Invalid source or pd outside RAM: false.
inline bool screenPolyFromCall(const Ram &ram, int src, int x, int y, uint32_t pd, TextQuad &t) {
    if (src < 0 || src >= int(ram.u32(sym::s_numSourceObjects)) || src >= 8 || !ram.ok(pd & 0x1FFFFFFFu, 0x24)) return false;
    pd &= 0x1FFFFFFFu;
    const uint32_t so = sym::s_sourceObjects + 0x20u * uint32_t(src);
    const uint32_t view = ram.u32(sym::viewCurView);
    const uint32_t def = view < 5 ? ram.u32(addr::viewInfo + 0x14 * view + 4) & 0x1FFFFFFFu : 0;
    if (!ram.ok(def, 0x20)) return false;
    const int cx = int(ram.u32(def + 0x18)), cy = int(ram.u32(def + 0x1C)), w = ram.u16(def + 4), h = ram.u16(def + 6);
    auto in = [&](int i) { return int(ram.u32(pd + 4 * uint32_t(i))); };   // x0, y0, x1, y1, u0, v0, u1, v1
    const int sfx = ram.u16(so + 0x1C), pfx = ram.u16(pd + 0x20);
    const int fx = (sfx & (0x2 | 0x8)) ^ ((sfx & 0x10) | pfx);
    if (fx & 0x10) { x -= (in(2) - in(0)) / 2; y -= (in(3) - in(1)) / 2; }
    const int ox = (fx & 0x8) ? cx + w / 2 - (x + (in(2) - in(0))) : x + (cx - w / 2), oy = y + (cy - h / 2);
    const int u0 = (fx & 0x2) ? in(6) : in(4), u1 = (fx & 0x2) ? in(4) : in(6);
    const int v0 = (fx & 0x4) ? in(7) : in(5), v1 = (fx & 0x4) ? in(5) : in(7);
    const uint32_t gu0 = uint32_t(u0 * 16), gv0 = uint32_t((0x80 - v0) * 16), gu1 = uint32_t(u1 * 16), gv1 = uint32_t((0x80 - v1) * 16);
    const uint32_t gx0 = uint32_t((in(0) + ox) * 16), gy0 = uint32_t((in(1) + oy) * 16), gx1 = uint32_t((in(2) + ox) * 16), gy1 = uint32_t((in(3) + oy) * 16);
    constexpr float kOx = 2048.f - 320.f, kOy = 2048.f - 112.f;   // like readScreenPolys
    t = TextQuad{};
    t.r = ram.u8(so + 0x18); t.g = ram.u8(so + 0x19); t.b = ram.u8(so + 0x1A); t.a = ram.u8(so + 0x1B);
    t.u0 = float(gu0 & 0x3FFF) / 16.f; t.v0 = float(gv0 & 0x3FFF) / 16.f;
    t.u1 = float(gu1 & 0x3FFF) / 16.f; t.v1 = float(gv1 & 0x3FFF) / 16.f;
    t.x0 = float(gx0 & 0xFFFF) / 16.f - kOx; t.y0 = float(gy0 & 0xFFFF) / 16.f - kOy;
    t.x1 = float(gx1 & 0xFFFF) / 16.f - kOx; t.y1 = float(gy1 & 0xFFFF) / 16.f - kOy;
    const uint32_t obj = ram.u32(so + 0x14) & 0x1FFFFFFFu;   // _hierobject: TEX0 at +0x70 (polyInitPacket copies it into the A+D)
    t.tex0 = ram.ok(obj, 0x78) ? uint64_t(ram.u32(obj + 0x70)) | (uint64_t(ram.u32(obj + 0x74)) << 32) : 0;
    return true;
}

// Game text from the font calls (common/font), without reading fontPacket: the hooks (wotm_overrides.cpp) follow what the game
// asks to build and redo each glyph with the same math as fontBuildPrim, from the font state (fonts[2], 0x38 each: atlas
// cell at +0/+2, glyph size in the atlas at +4/+6, size on screen at +8/+0xA, cursor at +0x10/+0x12, color at +0x18) and the current
// view (center and size of the viewDef). Positions and UV in 1/16 (GS subpixel), XY with the GS center at 2048 as in the packet.
//  - fontInitPacket(pkt, vram): TEX0 of the boot atlas (vram - 0x840 in TBP0, vram - 0x40 in CBP, T8H 512x512) and default color.
//  - fontSetTexId(no): TEX0 of the stage's .RTX atlas (no + 0x70) and atlas mode 1 (D_006F8D38).
//  - fontSetColorGifTag(font): the font's color applies to the following glyphs, from any font (the color tag belongs to the packet).
//    Called with an empty packet (right after dispatch), it rewrites the header color, which applies at the start of each frame.
//  - fontBuildPrim(font, c): one glyph. Atlas 0: 8 columns starting at '!', lowercase becomes uppercase; atlas 1: 16 columns.
//  - fontDmaFontData: closes the frame (the text sent to the GS) and resets the color to the header's.
struct FontText {
    static constexpr uint32_t kFonts = 0x00735890;      // D_00735890: Font fonts[2]
    static constexpr uint32_t kAtlasMode = 0x006F8D38;  // D_006F8D38: 0 = boot atlas, 1 = .RTX atlas (fontSetTexId)
    static constexpr uint32_t kPktState = 0x006F8D3C;   // D_006F8D3C: 0 = no packet, 1 = last tag is a color tag, 2 = a sprite tag
    static constexpr uint32_t kPktCount = 0x006F8D30, kPktHeader = 0x006F8D44;   // qwords in the packet; header size
    uint64_t tex0 = 0;
    uint8_t col[4] = {0x80, 0x80, 0x40, 0x80}, headerCol[4] = {0x80, 0x80, 0x40, 0x80};
    std::vector<TextQuad> building;   // glyphs since the last fontDmaFontData

    void initPacket(uint32_t vram) {
        const uint64_t tbp = uint16_t(int32_t((vram - 0x840u) << 10) >> 16), cbp = uint16_t(int32_t((vram - 0x40u) << 10) >> 16);
        tex0 = tbp | (cbp << 37) | (0xBBA8ull << 19) | 0x8000ull | (0x8000ull << 46);
        static constexpr uint8_t kDef[4] = {0x80, 0x80, 0x40, 0x80};
        std::memcpy(col, kDef, 4);
        std::memcpy(headerCol, kDef, 4);
        building.clear();
    }
    void setTexId(const Ram &ram, uint32_t node) {
        node &= 0x1FFFFFFFu;
        if (ram.ok(node, 0x78) && !(ram.u32(node) & 0x3F)) tex0 = uint64_t(ram.u32(node + 0x70)) | (uint64_t(ram.u32(node + 0x74)) << 32);
    }
    // At the entry of fontSetColorGifTag (the color is already in the font; the packet state is still the previous one).
    void setColor(const Ram &ram, int font) {
        const uint32_t f = kFonts + 0x38u * uint32_t(font) + 0x18;
        if (font < 0 || font > 1) return;
        for (int i = 0; i < 4; ++i) col[i] = ram.u8(f + i);
        if (ram.u32(kPktState) == 1 && ram.u32(kPktCount) == ram.u32(kPktHeader)) std::memcpy(headerCol, col, 4);
    }
    // At the entry of fontBuildPrim.
    void glyph(const Ram &ram, int font, int ch) {
        if (font < 0 || font > 1 || building.size() >= 4096) return;
        const uint32_t f = kFonts + 0x38u * uint32_t(font);
        const uint32_t view = ram.u32(sym::viewCurView);
        const uint32_t def = view < 5 ? ram.u32(addr::viewInfo + 0x14 * view + 4) & 0x1FFFFFFFu : 0;
        if (!ram.ok(def, 0x20)) return;
        const int cx = int(ram.u32(def + 0x18)), cy = int(ram.u32(def + 0x1C)), w = ram.u16(def + 4), h = ram.u16(def + 6);
        int c = int8_t(ch);
        const bool rtx = ram.u32(kAtlasMode) != 0;
        if (!rtx && unsigned(c - 0x61) < 0x1Au) c = int8_t(c - 0x20);
        int row, column, base;
        if (rtx) { row = c >> 4; column = c & 15; base = 0x10; }
        else { const int a = c - 0x21; row = a >> 3; column = a & 7; base = 8; }
        const uint16_t u0 = uint16_t(ram.u16(f) * column + base), v0 = uint16_t(ram.u16(f + 2) * row + base);
        const uint16_t u1 = uint16_t(ram.u16(f + 4) + u0), v1 = uint16_t(ram.u16(f + 6) + v0);
        const uint16_t x0 = uint16_t(ram.u16(f + 0x10) + cx * 16 - w * 8), y0 = uint16_t(ram.u16(f + 0x12) + cy * 16 - h * 8);
        const uint16_t x1 = uint16_t(ram.u16(f + 8) + x0), y1 = uint16_t(ram.u16(f + 0xA) + y0);
        constexpr float kOx = 2048.f - 320.f, kOy = 2048.f - 112.f;   // like Scene::updateText
        TextQuad t{};
        t.u0 = float(u0 & 0x3FFF) / 16.f; t.v0 = float(v0 & 0x3FFF) / 16.f;
        t.u1 = float(u1 & 0x3FFF) / 16.f; t.v1 = float(v1 & 0x3FFF) / 16.f;
        t.x0 = float(x0) / 16.f - kOx; t.y0 = float(y0) / 16.f - kOy;
        t.x1 = float(x1) / 16.f - kOx; t.y1 = float(y1) / 16.f - kOy;
        t.r = col[0]; t.g = col[1]; t.b = col[2]; t.a = col[3];
        t.tex0 = tex0;
        building.push_back(t);
    }
    // At the entry of fontDmaFontData: true = the frame was closed (glyphs in `out`).
    bool sent(const Ram &ram, std::vector<TextQuad> &out) {
        if (ram.u32(kPktState) == 0) return false;
        out.swap(building);
        building.clear();
        std::memcpy(col, headerCol, 4);
        return true;
    }
};

// Active shadow blocker (Scene::collectShadows / drawShadows): psBlkLst entry and the objects that cast the silhouette.
struct ShadowBlocker {
    int index = 0;               // psBlkLst entry
    float origin[3] = {0, 0, 0}; // cs->trans
    float extent = 0;            // half side of the light frame (node radius * 1.1)
    uint32_t shadowCs = 0;       // entry +0x14 (psBlockerSetShadowCs): only objects of this _cs receive the shadow (psObjectCull)
    std::vector<DrawItem> casters;
    std::vector<uint32_t> casterNodes;
};
struct ParaLights { float dir[4][3] = {}; float col[3][3] = {}; };   // parallel lights (Scene::readParaLights)
struct Fog { float fa = 0.f, maxVal = 255.f, fb = 0.f; float col[3] = {0, 0, 0}; };   // fog (Scene::readFog, Scene::fogF)

// Everything the drawing reads from the game's frame, copied from RAM in one go (items with the copied palette, particles, shadows,
// lights, fog). In fights and menus it is taken on the EE thread at the entry of timerWaitForMinUpdateRate, with the frame ready
// (wotmCaptureFrame); draws until the game's next frame repeat this one, without reading the RAM the game is rewriting.
struct Snapshot {
    std::vector<Sprite> sprites;
    std::vector<DrawItem> items, sky, overlay, hud, hp, hpOverlay;
    std::vector<FxSystem> fx;
    std::shared_ptr<const uint8_t> fxRam;  // shadow copy of the particle RAM (Particles::shadowRam, live); nullptr = read the RAM passed to draw
    size_t fxRamSize = 0;                  // size of the fxRam buffer (the RAM's)
    Particles::FlareFrame flares;       // the frame's lens flare (Particles::collectFlares); without it the drawing side has no list
    std::vector<ShadowBlocker> shadows;
    std::vector<ScreenRect> screenRects;   // the frame's fades (wotmScreenRect, in the emitters' hooks; not from collect)
    std::vector<TextQuad> screenPolys;     // ScreenPolys HUD (readScreenPolys: in takeSnapshot and in the dmaData hook)
    std::vector<HudAnchor> anchors;        // world point of each health icon in the frame (bars and icon follow the camera in photo mode)
    std::vector<WorldLight> worldLights;   // lens-flare lights walked by the collection (photo mode)
    std::vector<WorldEmitter> emitters;    // particle emitters walked by the collection (photo mode: Agamo's fire)
    ParaLights lights;
    Fog fog;
    GameCamera cam;
    ViewCtx vctx;       // game view of this snapshot (split screen: 0 = left, 1 = right; quadrants: v & 1 column, v >> 1 row)
    uint32_t g = 0;     // game frame counter
    // Stamp for interpolation (Scene::showBlend): field count at the start of the frame (D_006F8DC0; a 2-field frame = gap of 2)
    // and the host clock at capture (seconds, steady_clock). fields = 0: no stamp (replay, local collection)
    uint32_t fields = 0;
    double hostT = 0.0;
    uint32_t culled = 0;  // OBJECTs and GROUPs that collect culled by the view (the stage exists even with few items on screen)
    // Camera the particle shadow copy (fxRam) was built with, when it isn't `cam` (chase view: view 0's);
    // show and showBlend carry that camera's view worldToScreenMat and eye over to the drawn one (Scene::aimFxRam)
    GameCamera fxCam;
    bool valid = false;
    // Split screen: views 1 to 3 of the same frame (collected at the exit of hier(v, 0)); views[v - 1] = nullptr: no view v
    std::shared_ptr<Snapshot> views[3];
    // Players 5..8 (WOTM_LOCALPLAYER=5..8): the game has no view or camera for them; the whole scene, collected without the game's view
    // culling, with a native chase camera behind the local player's monster (wotm_live.inc, wotmCaptureFrame)
    std::shared_ptr<Snapshot> chase;
};

class Scene {
public:
    // Native frame width / 4:3 width at the same height (Hor+ widescreen; 1 = original screen). Filled by: wotm_live.inc
    float wide = 1.f;
    // Sphere culling against the game view, like hierTraverseAsm (Scene::outOfView); the viewer with a free camera turns it off.
    bool fovCull = true;
    // Sphere radius in culling multiplied by this: with interpolation the drawn view lags up to one frame behind the culling one (B's),
    // and what B culled at the edge would vanish during a turn (wotm_live.inc sets 1.2 with interpolation)
    float cullMargin = 1.f;
    // Native camera during collection (chase view for players 5..8, wotm_live.inc): the eye (LOD, transparent CONTROL buildings)
    // and the aim point come from it and its monster, not from the game view's camera. That way the building between it and the monster
    // (or with it inside) uses the transparent version, as the game does with its own cameras.
    struct NativeEye { bool on = false; float eye[3] = {}; float target[3] = {}; };
    NativeEye nativeEye;
    mutable int ghosted = 0;   // CONTROLs that switched to the transparent version in the last collection
    // Game view that collect follows and draw draws (split screen; ViewCtx). Collection gets it from the caller; show copies it from the
    // snapshot.
    ViewCtx vctx;
    std::vector<DrawItem> items;
    std::vector<DrawItem> sky;   // sky / clouds (drawn first, no depth, at skyCs->trans)
    std::vector<DrawItem> overlay;   // button icons (McButtons): on top of everything, no depth test
    std::vector<DrawItem> hud;       // HUD elements (ids 0x1C20..0x2133): over the scene, no depth, after the icons (DrawItem::hudId = object id)
    // High-priority list (CsPool::m_HPActiveList; hier(v, 1) -> hierTraceHPCsList), minus the HUD: dialog frames (shell->m_cs610,
    // pauseLevelOverlay and friends) and their button icons. ShellRtLoop::endField draws it after the gsPkt queue (fade) and before the
    // text; in rtMain and displayDialog it comes before both. drawHp, called by the presenting side after the fades under the text.
    std::vector<DrawItem> hp, hpOverlay;   // hpOverlay: its button icons (fadeByNode)
    std::vector<Sprite> sprites;   // particles (debris, dust) read from fx[]; drawn as camera-facing sprites
    std::vector<TextQuad> text;    // the frame's glyphs (updateText), drawn by drawQuads after the 3D scene and the screenPolys
    std::vector<ScreenRect> screenRects;   // fades (Snapshot::screenRects), drawn last by drawScreenRects
    std::vector<TextQuad> screenPolys;     // ScreenPolys HUD (Snapshot::screenPolys), drawn by drawScreenPolys before the text
    std::vector<WorldLight> worldLights;   // Snapshot::worldLights (walk finds them at op 5 nodes)
    std::vector<WorldEmitter> emitters;    // Snapshot::emitters (op 22 nodes)
    uint32_t visited = 0;
    uint32_t culled = 0;   // OBJECTs and GROUPs outside the view in collect (Snapshot::culled)
    uint32_t byOp[64] = {};

    gs::Vram vram;   // debugging only: the dump's VRAM holds stale palettes (menu leftovers)

    // Stage palettes, read from the disc's `.RTX`: entries of `(word0 >> 2) * 16` bytes with a 16-byte header (GS descriptor
    // at +8: relative DBP in the half-word at +0xA, TW/TH in bits 40..47) and the body in logical palette order (entry i = word i):
    // 16x16 CT32 = 256 colors, 8x2 CT32 = 16 colors. The object's TEX0.CBP is `base + DBP`, with base = tempVramTexAddr >> 6.
    // The key is the ABSOLUTE CBP (file base + DBP). Level base = tempVramTexAddr >> 6; monster i's = fileStatus.maxTexAddr[i-1]
    // (texmResInit stacks the .RTX files in the order the Shell loaded them: level, players, AIs).
    std::unordered_map<uint32_t, std::vector<uint32_t>> rtxPal;
    // Resident .RTX images (PSMT8H/4HL/4HH: screens, fonts, menu logos, HUD portraits): key (psm << 16) | DBP. The RAM pixels
    // at `texInfo[id]` are useless (the pointer points at something else); the TEX0's TBP0 and the entry's DBP match. In
    // monster files the DBP is relative: texmResRelocate adds the end of the previous file's resident area (`resBase`).
    std::unordered_map<uint32_t, std::vector<uint8_t>> rtxImg;

    void clearRtx() { rtxPal.clear(); rtxImg.clear(); rtxImgSrc_.clear(); }

    // ---- mod textures as PNG (wotm_texmods.hpp)
    struct ImgSrc { std::string stem; uint32_t texId; };
    std::unordered_map<uint32_t, ImgSrc> rtxImgSrc_;   // rtxImg key -> file and disc texId
    std::string discRoot_;                              // root of the extracted disc, with the slash (loadResources)
    std::map<std::string, std::map<uint32_t, uint32_t>> texPackets_;   // FILE -> packet offset in the .TEX -> disc texId

    // Disc path ("MON/LIZARD0.RTX"): the mods/ file (wotm_mods.hpp) if there is one, otherwise the root's
    std::string discPath(const std::string &rel) const {
        std::string p = mods::Registry::get().hostPath(rel);
        return p.empty() ? discRoot_ + mods::Registry::get().aliasOf(rel) : p;   // mod costume: the base costume's file
    }

    // Mod PNG in place of the object's texture (global texInfo texId + TEX0). Resident ones (T8H/4HL/4HH): by DBP, like
    // decodeResident. The others: the upload packet at texInfo[texId] is inside the .TEX loaded at fileStatus.texAddr[i]; the
    // offset into it gives the disc texId (the packet in RAM already has the relocated id). File i's name is fileStatus.names[i].
    // hiAlpha: the original texture goes above 0x80 (texmods::loadPng)
    bool modTexture(const Ram &ram, uint32_t texId, const gs::Tex0 &t0, bool tcc, bool rawAlpha, std::vector<uint32_t> &px, uint32_t &w, uint32_t &h,
                    bool hiAlpha) {
        texmods::Index &ix = texmods::Index::get();
        if (ix.empty()) return false;
        char what[64];
        if (t0.psm == gs::T8H || t0.psm == gs::T4HL || t0.psm == gs::T4HH) {
            const auto it = rtxImgSrc_.find((t0.psm << 16) | t0.tbp0);
            if (it == rtxImgSrc_.end()) return false;
            const texmods::Png *png = ix.find(it->second.stem, true, it->second.texId);
            std::snprintf(what, sizeof what, "%s.RTX t%u", it->second.stem.c_str(), it->second.texId);
            return png && texmods::loadPng(*png, what, 1u << t0.tw, 1u << t0.th, rawAlpha, tcc, px, w, h, hiAlpha);
        }
        if (!texId || texId >= 1000) return false;
        const uint32_t fs = addr::fileStatus;
        const uint32_t ptr = ram.u32(addr::texInfo + 16 * texId) & 0x0FFFFFFF;
        const int nNames = ram.u8(fs + 0x74);
        // .TEX (texAddr @0x00, texLoaded @0x38) and, for the regular textures that live in the .RTX, resAddr @0x78 / resLoaded @0xB0
        for (const bool rtx : {false, true}) {
            const uint32_t tab = rtx ? fs + 0x78 : fs;
            const int n = std::min<int>(ram.u8(rtx ? fs + 0xB0 : fs + 0x38), std::min(nNames, 14));
            for (int i = 0; i < n; ++i) {
                const uint32_t a = ram.u32(tab + 4 * uint32_t(i)) & 0x0FFFFFFF, b = ram.u32(tab + 4 * uint32_t(i + 1)) & 0x0FFFFFFF;
                if (ptr < a || ptr >= b) continue;
                std::string stem;
                for (int c = 0; c < 8; ++c) {
                    const char ch = char(ram.u8(fs + 0xB1 + 9 * uint32_t(i) + uint32_t(c)));
                    if (!ch) break;
                    stem += char(std::toupper(static_cast<unsigned char>(ch)));
                }
                if (stem.empty()) return false;
                const std::string file = stem + (rtx ? ".RTX" : ".TEX");
                auto pk = texPackets_.find(file);
                if (pk == texPackets_.end()) {
                    std::vector<uint8_t> d;
                    for (const char *dir : {"LVL/", "MON/", "SHELL/", ""}) {
                        d = texmods::readUnzipped(discPath(*dir ? dir + file : stem + "/" + file));
                        if (!d.empty()) break;
                    }
                    pk = texPackets_.emplace(file, texmods::packetIds(d)).first;
                }
                const auto id = pk->second.find(ptr - a);
                if (id == pk->second.end()) return false;
                const texmods::Png *png = ix.find(stem, rtx, id->second);
                if (!png) return false;
                const UploadHdr hd = UploadHdr::read(ram, ptr);
                std::snprintf(what, sizeof what, "%s t%u", file.c_str(), id->second);
                return texmods::loadPng(*png, what, hd.w, hd.h, rawAlpha, tcc, px, w, h, hiAlpha);
            }
        }
        return false;
    }
    // Texel region (u0, v0, width, height) of the original texture within the loaded texture, which may be a PNG of a different size
    static Rectangle texelRect(const Texture2D &t, uint64_t tex0, Rectangle r) {
        const gs::Tex0 t0 = gs::Tex0::decode(tex0);
        const float kx = float(t.width) / float(1u << t0.tw), ky = float(t.height) / float(1u << t0.th);
        return Rectangle{r.x * kx, r.y * ky, r.width * kx, r.height * ky};
    }

    // Loads the level and monsters of the current game from the extracted disc (`root` contains LVL/ and MON/). Idempotent: only redoes
    // it when the names/bases in fileStatus change. Returns true if it found at least the level file.
    bool loadResources(const Ram &ram, const std::string &rootArg, std::string *log = nullptr) {
        const uint32_t fs = addr::fileStatus;
        const int n = ram.u8(fs + 0xB0);
        if (n < 1 || n > 14) return false;
        uint32_t sig = ram.u32(addr::tempVramTexAddr) * 2654435761u;
        std::vector<std::string> names;
        names.resize(size_t(n));
        for (int i = 0; i < n; ++i) {
            for (int c = 0; c < 8; ++c) {
                const char ch = char(ram.u8(fs + 0xB1 + 9 * i + c));
                if (!ch) break;
                names[size_t(i)] += char(std::toupper(static_cast<unsigned char>(ch)));
            }
            sig = (sig ^ ram.u16(fs + 0x14C + 2 * i)) * 16777619u;
            for (char ch : names[size_t(i)]) sig = (sig ^ uint8_t(ch)) * 16777619u;
        }
        if (resSig_ == sig && !rtxPal.empty()) return true;
        if (names[0].empty()) return false;
        // No explicit root: WOTM_DISC (as in wotm_fmv.hpp), otherwise the current directory.
        std::string root = rootArg;
        if (root.empty())
            if (const char *d = std::getenv("WOTM_DISC")) root = d;
        std::string sep = root.empty() || root.back() == '/' || root.back() == '\\' ? "" : "/";
        discRoot_ = root + sep;
        auto path = [&](const std::string &rel) { return discPath(rel); };
        rtxPal.clear(); rtxImg.clear(); rtxImgSrc_.clear();
        const uint32_t base0 = ram.u32(addr::tempVramTexAddr) >> 6;
        bool lvl = loadRtx(path("LVL/" + names[0] + ".RTX").c_str(), base0);
        // The menu (SHELL) lives in SHELL/SHELL.RTX, not in LVL/.
        if (!lvl) lvl = loadRtx(path(names[0] + "/" + names[0] + ".RTX").c_str(), base0);
        if (!lvl) lvl = loadRtx(path("SHELL/" + names[0] + ".RTX").c_str(), base0);   // LOAD, UI, SHELL2, PRESHELL
        if (log) *log = names[0] + (lvl ? "" : " (not found)");
        for (int i = 1; i < n; ++i) {
            if (names[size_t(i)].empty()) continue;
            const bool ok = loadRtx(path("MON/" + names[size_t(i)] + ".RTX").c_str(), ram.u16(fs + 0x14C + 2 * (i - 1)), true,
                                    ram.u16(fs + 0x168 + 2 * (i - 1)));   // the previous file's maxTexAddr and maxResAddr
            if (log) *log += " " + names[size_t(i)] + (ok ? "" : "(?)");
        }
        invalidate();
        resSig_ = sig;
        return lvl;
    }

    // base: added to the palettes' DBP (file base); resBase: added to the resident images' DBP (0 for the stage and the menu; for a monster,
    // the previous file's fileStatus.maxResAddr, as in texmResRelocate).
    bool loadRtx(const char *path, uint32_t base, bool append = false, uint32_t resBase = 0) {
        if (!append) { rtxPal.clear(); rtxImg.clear(); rtxImgSrc_.clear(); }
        std::string stem = path;   // "LIZARD0": name for mod resident images (texmods, <file>_rtx_t<id>.png)
        stem = stem.substr(stem.find_last_of("/\\") + 1);
        stem = stem.substr(0, stem.find('.'));
        for (char &c : stem) c = char(std::toupper(static_cast<unsigned char>(c)));
        std::vector<uint8_t> raw;
        if (FILE *f = std::fopen(path, "rb")) {
            std::fseek(f, 0, SEEK_END);
            raw.resize(size_t(std::ftell(f)));
            std::fseek(f, 0, SEEK_SET);
            if (std::fread(raw.data(), 1, raw.size(), f) != raw.size()) raw.clear();
            std::fclose(f);
        }
        if (raw.size() < 34) return false;
        std::vector<uint8_t> data;
        static const uint8_t kMagic[4] = {'I', 'E', 3, 4};
        if (std::memcmp(raw.data(), kMagic, 4) == 0) {   // zip with the magic swapped, one file in raw deflate
            uint32_t csize = 0;
            uint16_t nlen = 0, xlen = 0;
            std::memcpy(&csize, raw.data() + 18, 4);
            std::memcpy(&nlen, raw.data() + 26, 2);
            std::memcpy(&xlen, raw.data() + 28, 2);
            const size_t off = 30 + nlen + xlen;
            if (off + csize > raw.size()) return false;
            int outSize = 0;
            unsigned char *d = DecompressData(raw.data() + off, int(csize), &outSize);
            if (!d || outSize <= 0) return false;
            data.assign(d, d + outSize);
            MemFree(d);
        } else {
            data = raw;
        }
        size_t off = 0, added = 0;
        // The size-0 entry is the last and also counts (it runs to the end of the file): in monsters it is the HUD portrait (T8H 128x64).
        bool last = false;
        while (!last && off + 16 <= data.size()) {
            uint32_t w[4];
            std::memcpy(w, data.data() + off, 16);
            last = w[0] == 0;
            const size_t sz = last ? data.size() - off : size_t(w[0] >> 2) * 16;
            if (sz < 16 || off + sz > data.size()) break;
            const uint64_t desc = uint64_t(w[2]) | (uint64_t(w[3]) << 32);
            const uint32_t psm = uint32_t(desc >> 32) & 0x3F, tw = uint32_t(desc >> 40) & 15, th = uint32_t(desc >> 44) & 15;
            if (psm == 0 && ((tw == 4 && th == 4) || (tw == 3 && th == 1))) {
                std::vector<uint32_t> p((sz - 16) / 4);
                std::memcpy(p.data(), data.data() + off + 16, p.size() * 4);
                rtxPal[base + ((w[2] >> 16) & 0xFFFF)] = std::move(p);
                ++added;
            } else if ((psm == gs::T8H || psm == gs::T4HL || psm == gs::T4HH) && sz > 0x80) {
                const uint32_t key = (psm << 16) | ((resBase + ((w[2] >> 16) & 0xFFFF)) & 0xFFFF);
                rtxImg[key].assign(data.begin() + long(off) + 0x80, data.begin() + long(off + sz));
                rtxImgSrc_[key] = {stem, w[2] & 0xFFFF};
            }
            off += sz;
        }
        return added > 0;
    }

    // Game texture filter (TEXTURE FILTER option, WOTM_TEXFILTER): 0 bilinear without mipmaps (the GS's), 1 trilinear (mipmaps), 2..16
    // anisotropic with trilinear. Applies to existing textures and to future ones; drawing calls it every frame and it only acts on a change.
    void setTexFilter(int f) {
        if (f == texFilter_) return;
        texFilter_ = f;
        for (auto &kv : tex_) applyTexFilter(kv.second);
        std::fprintf(stderr, "[native] texture filter: %s (%zu textures)\n", f <= 0 ? "bilinear" : f == 1 ? "trilinear" : "anisotropic", tex_.size());
    }

    void invalidate() {
        cache_.clear();
        for (auto &kv : tex_) if (kv.second.id) UnloadTexture(kv.second);
        tex_.clear();
        for (auto &kv : fontTex_) if (kv.second.id) UnloadTexture(kv.second);
        fontTex_.clear();
        for (DustSrc &s : dustSrc_) s.ok = false;
        for (uint64_t &h : dustHash_) h = 0;
        fx.resetFlareDebounce();   // visible fraction from the previous frame and the old level
    }

    // The object's TEX0 (gsTexCtx[slot]); 0 when the object has no texture or the section is kSlotNoTex
    static uint64_t objectTex0(const Ram &ram, uint32_t node, uint32_t slot = 0) {
        const uint32_t n = slot == kSlotNoTex ? 0 : ram.u16(node + 0x5A);
        if (n == 0) return 0;
        if (slot >= n || slot >= 5) slot = 0;
        return uint64_t(ram.u32(node + 0x70 + 0x30 * slot)) | (uint64_t(ram.u32(node + 0x74 + 0x30 * slot)) << 32);
    }
    // texId of the slot (texId[5] at +0x50); an invalid slot falls back to 0; kSlotNoTex = 0
    static uint32_t objectTexId(const Ram &ram, uint32_t node, uint32_t slot = 0) {
        if (slot == kSlotNoTex) return 0;
        if (slot >= ram.u16(node + 0x5A) || slot >= 5) slot = 0;
        return ram.u16(node + 0x50 + 2 * slot);
    }

    // Texture key of an object: id in `texInfo` + palette (TEX0's CBP/CSA/CPSM) + TCC = 0 (bit 30; decodeTex). 0 = no texture.
    // texId 0 with TEX0 = the buffer on display, i.e. the previous frame (rtMain -> texmPrepTexInfoZero; texmGetTextureAddrs sets up
    // PSMCT24 640 wide, window 1024x256): key kScreenKey, texture from screenTexture.
    static constexpr uint64_t kScreenKey = 1ull << 63;
    static uint64_t texKey(const Ram &ram, uint32_t node, uint32_t slot = 0) {
        const uint64_t t0 = objectTex0(ram, node, slot);
        const uint32_t tid = objectTexId(ram, node, slot);
        if (!t0) return 0;
        if (!tid) return kScreenKey;
        const gs::Tex0 t = gs::Tex0::decode(t0);
        const uint64_t noTcc = (t0 >> 34) & 1 ? 0 : 1ull << 30;
        return (uint64_t(tid) << 40) | noTcc | (uint64_t(t.cbp) << 12) | (uint64_t(t.csa) << 4) | t.cpsm | (1ull << 62);
    }
    // For CLOAK (DrawItem::env 3) pktBldEnvMapDma replaces the TEX0 of each textured slot with texId 0's (texmGetTextureAddrs).
    static uint64_t texKey(const Ram &ram, const DrawItem &it, uint32_t slot) {
        const uint64_t k = texKey(ram, it.node, slot);
        return k && it.env == 3 ? kScreenKey : k;
    }

    // Fills the 256-entry palette. The GS reads 256-color CLUTs in CSM1 with bits 3 and 4 of the index swapped (entries 8-15 and
    // 16-23 of each group of 32); the .RTX stores the raw upload, so reading has to apply the swap (without it: grain in gradients).
    // rawAlpha: raw GS alpha (0x80 = 1.0), for what Scene::draw blends like the GS; otherwise 0..255 with 0x80 saturated (fixAlpha).
    static void fillPal(uint32_t pal[256], const std::vector<uint32_t> &src, bool is8bit, bool rawAlpha = false) {
        for (size_t i = 0; i < 256 && i < src.size(); ++i) {
            const size_t k = (is8bit && src.size() >= 256) ? ((i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)) : i;
            pal[i] = rawAlpha ? src[k] : gs::fixAlpha(src[k]);
        }
    }

    // Object texture: the pixels come from the upload packet pointed to by `texInfo[texId]` (linear pixels from +0x80,
    // header in UploadHdr); the palette comes from the .RTX (loaded along with the stage).
    // TEX0.TCC = 0 (RGB only): the GS uses vertex alpha and ignores the texture's; here the texel comes out with alpha 1.0 (the menu's
    // drive-in screen, tex 76, with alpha 0 and 0x80 vertices over the logo).
    bool decodeTex(const Ram &ram, uint32_t node, std::vector<uint32_t> &px, uint32_t &w, uint32_t &h, uint32_t slot = 0, bool rawAlpha = false) const {
        const uint64_t t0v = objectTex0(ram, node, slot);
        if (!t0v) return false;
        if (!decodeTexRgba(ram, node, t0v, px, w, h, slot, rawAlpha)) return false;
        if (!((t0v >> 34) & 1)) {
            const uint32_t one = rawAlpha ? 0x80000000u : 0xFF000000u;
            for (uint32_t &c : px) c = (c & 0x00FFFFFFu) | one;
        }
        return true;
    }
    bool decodeTexRgba(const Ram &ram, uint32_t node, uint64_t t0v, std::vector<uint32_t> &px, uint32_t &w, uint32_t &h, uint32_t slot, bool rawAlpha) const {
        const gs::Tex0 t0 = gs::Tex0::decode(t0v);
        const uint32_t ptr = ram.u32(addr::texInfo + 16 * objectTexId(ram, node, slot)) & 0x0FFFFFFF;
        if (t0.psm == gs::T8H || t0.psm == gs::T4HL || t0.psm == gs::T4HH) {
            uint32_t pal[256] = {};
            if (decodeResident(t0, px, w, h, rawAlpha, pal)) return true;
            // Palette without the image in the .RTX: the RAM at `ptr` (unreliable).
            if (rtxPal.find(t0.cbp) == rtxPal.end() || !ram.ok(ptr + 0x80)) return false;
            return gs::decodeUpload(ram.p + ptr, ram.size - ptr, w, h, t0.psm == gs::T8H ? gs::T8 : gs::T4, t0.csa, pal, px);
        }
        return decodeUploadTex(ram, ptr, t0, px, w, h, false, rawAlpha);
    }

    // Resident textures in PSMT8H/4HL/4HH (menu screens and logos, fonts, portraits, HUD icons): `texInfo[id]` points at something
    // else; the pixels come from the disc's .RTX, entry with DBP == TEX0.TBP0 and the same psm (4HL and 4HH share the DBP: they're the two nibbles).
    // 8 bits, or packed 4 bits (low nibble first); size = TEX0's TW/TH. `pal` comes out filled if the palette exists.
    bool decodeResident(const gs::Tex0 &t0, std::vector<uint32_t> &px, uint32_t &w, uint32_t &h, bool rawAlpha, uint32_t pal[256]) const {
        if (t0.psm != gs::T8H && t0.psm != gs::T4HL && t0.psm != gs::T4HH) return false;
        const auto it = rtxPal.find(t0.cbp);
        if (it == rtxPal.end()) return false;
        fillPal(pal, it->second, t0.psm == gs::T8H, rawAlpha);
        w = 1u << t0.tw; h = 1u << t0.th;
        const auto im = rtxImg.find((t0.psm << 16) | t0.tbp0);
        return im != rtxImg.end() &&
               gs::decodeUpload(im->second.data(), im->second.size(), w, h, t0.psm == gs::T8H ? gs::T8 : gs::T4, t0.csa, pal, px);
    }

    // Pixels of the upload packet at `ptr` (= texInfo[texId]; header in UploadHdr, linear pixels from +0x80) with the .RTX palette by the TEX0's CBP. Serves objects (decodeTex) and particles (particleTexture).
    bool decodeUploadTex(const Ram &ram, uint32_t ptr, const gs::Tex0 &t0, std::vector<uint32_t> &px, uint32_t &w, uint32_t &h,
                         bool grayFallback = false, bool rawAlpha = false) const {
        if (!ram.ok(ptr + 0x80)) return false;
        uint32_t pal[256] = {};
        const UploadHdr hd = UploadHdr::read(ram, ptr);
        const uint32_t psm = hd.psm;
        if (psm == gs::T8 || psm == gs::T4) {
            // The palette comes from the loaded .RTX (loadRtx); without it the texture falls back to vertex color.
            const auto it = rtxPal.find(t0.cbp);
            if (it != rtxPal.end()) fillPal(pal, it->second, psm == gs::T8, rawAlpha);
            else if (grayFallback) {
                // Particles: many textures are luminance/alpha masks; without the palette, a gray ramp with alpha = index (1.0 at the end)
                const uint32_t n = psm == gs::T8 ? 256 : 16;
                for (uint32_t i = 0; i < n; ++i) {
                    const uint32_t v = i * 255 / (n - 1), a = rawAlpha ? i * 128 / (n - 1) : v;
                    pal[i] = v | (v << 8) | (v << 16) | (a << 24);
                }
            } else return false;
        }
        w = hd.w; h = hd.h;
        static const bool t4swz = std::getenv("WOTM_T4SWZ") != nullptr;   // diagnostics: the packet's T4 pixels as GS memory (scrambled nibbles)
        if (t4swz && psm == gs::T4 && w && h && w <= 512 && h <= 512) {
            px.assign(size_t(w) * h, 0);
            const uint32_t bw = std::max<uint32_t>(1, hd.bw);
            for (uint32_t y = 0; y < h; ++y)
                for (uint32_t x = 0; x < w; ++x) {
                    const uint32_t n = gs::nibT4(0, bw, x, y);
                    const uint8_t b = ram.u8(ptr + 0x80 + n / 2);
                    px[size_t(y) * w + x] = pal[(t0.csa * 16 + ((n & 1) ? (b >> 4) : (b & 15))) & 255];
                }
            return true;
        }
        return gs::decodeUpload(ram.p + ptr + 0x80, ram.size - ptr - 0x80, w, h, psm, t0.csa, pal, px, rawAlpha);
    }

    // Particle texture: texId (index into the global texInfo) + TEX0 from the particle table (psm/CBP). 0 = didn't decode.
    // Particles::kDustTexId = the composite dust/smoke texture (dustTexture).
    // WOTM_FXNOGRAY=1: no gray ramp when the palette isn't in the loaded .RTX files (diagnostics).
    unsigned particleTexture(const Ram &ram, uint32_t texId, uint64_t tex0) {
        if (texId == Particles::kDustTexId) return dustTexture(ram);
        if (!texId || texId >= 1000) return 0;
        const gs::Tex0 t0 = gs::Tex0::decode(tex0);
        const uint64_t key = (uint64_t(texId) << 40) | (uint64_t(t0.cbp) << 12) | (uint64_t(t0.csa) << 4) | t0.cpsm | (1ull << 61);
        const auto it = tex_.find(key);
        if (it != tex_.end()) return it->second.id;
        static const bool gray = std::getenv("WOTM_FXNOGRAY") == nullptr;
        const uint32_t ptr = ram.u32(addr::texInfo + 16 * texId) & 0x0FFFFFFF;
        std::vector<uint32_t> px;
        uint32_t w = 0, h = 0;
        Texture2D t{};
        bool ok = decodeUploadTex(ram, ptr, t0, px, w, h, gray, true) && w && h && px.size() >= size_t(w) * h;
        modParticle(ram, texId, tex0, ok, px, w, h);
        if (ok) {
            Image img{px.data(), int(w), int(h), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            t = LoadTextureFromImage(img);
            if (t.id) { SetTextureFilter(t, TEXTURE_FILTER_BILINEAR); SetTextureWrap(t, TEXTURE_WRAP_CLAMP); }
        }
        applyTexFilter(t);
        tex_[key] = t;
        return t.id;
    }

    // Composite dust/smoke texture (VRAM 0xFD800, TEX0 with TBP 0x3F60, CT32 64x64, four 32x32 cells), which the game rebuilds
    // in VRAM on every particleDraw with some system active (texDrawDustPacketBuild, packet at 0x78F6E0). Here the same math on the CPU, redone
    // when the parameters change (the phase advances 1/256 per frame; depends on the camera). docs/passos/VU1CHECK.md, "Corrigido".
    //  - Pass 1 (microprogram 0x2E98): a generator h = frac(c + h * b) draws 70 normals x 4 cells; each becomes a SPRITE with
    //    half-side 5 + 4t texels (t = frac(phase + h.w)) at the cell center + A * n * t, with A from the worldToScreenMat columns (q0-q3)
    //    and the focal (q5); z = min(72 - 1000 t (forward . n), 128), a = min(t, 1 - t) z, emitted only when a >= 1 (hemisphere facing the
    //    camera), up to 199 sprites. Draws source 1 (TextureInfo[32]) with RGBA (1, 106, 1, a) additive RGB only: density lands in G.
    //  - Half of RGB and A = G & 0x7F (copyGreenChannelToAlpha, the CT16 trick).
    //  - Pass 2 (MSCNT, 0x31C0): background and sprites with color = ambient + 0.5 + sum of the stage lights (dustLighting) along the normal, and
    //    A = a * 1.20755, with source 2 (TextureInfo[33]) in (Cs - Cd) * As + Cd RGB only.
    // Plain float arithmetic (the VU truncates); the distribution matches the investigation's dustsim.py. On METRO: color ~(57, 57, 40),
    // alpha up to 127 (dark smoke).
    unsigned dustTexture(const Ram &ram) {
        constexpr uint32_t kPkt = 0x0078F6E0u, kParams = 0x006F8DA4u, kNormals = 0x006F8D9Cu, kColors = 0x006F8DA0u, kCtx = 0x00256B90u;
        const uint32_t pa = ram.u32(kParams), na = ram.u32(kNormals), ca = ram.u32(kColors);
        if (!ram.ok(pa - 16, 48) || !ram.ok(na, 48) || !ram.ok(ca, 80) || !ram.ok(kPkt + 0xB4, 128)) return dustTex_[dustLast_].id;
        float P[8], L[3][4], C[5][3], ctx[8][4];
        for (int i = 0; i < 8; ++i) P[i] = ram.f32(pa + 4 * uint32_t(i));
        for (int r = 0; r < 3; ++r) for (int i = 0; i < 4; ++i) L[r][i] = ram.f32(na + 16 * uint32_t(r) + 4 * uint32_t(i));
        for (int k = 0; k < 5; ++k) for (int i = 0; i < 3; ++i) C[k][i] = ram.f32(ca + 16 * uint32_t(k) + 4 * uint32_t(i));
        for (int q = 0; q < 8; ++q) for (int i = 0; i < 4; ++i) ctx[q][i] = ram.f32(kCtx + 16 * uint32_t(q) + 4 * uint32_t(i));
        const uint32_t nloop = ram.u32(pa - 16), seed = ram.u32(pa - 4);
        uint64_t hash = 1469598103934665603ull;
        auto mix = [&](const void *d, size_t n) { for (size_t i = 0; i < n; ++i) hash = (hash ^ static_cast<const uint8_t *>(d)[i]) * 1099511628211ull; };
        mix(P, sizeof P); mix(L, sizeof L); mix(C, sizeof C); mix(ctx, sizeof ctx); mix(&nloop, 4); mix(&seed, 4);
        for (int i = 0; i < kDustSlots; ++i)
            if (dustTex_[i].id && hash == dustHash_[i]) return dustTex_[dustLast_ = i].id;

        auto source = [&](uint32_t ti, DustSrc &s) {   // decoded once per (texId, TEX0)
            const uint32_t k = ram.u16(fxaddr::textureInfo + 8 * ti + 2) & 127, id = ram.u16(fxaddr::texId + 2 * k);
            const uint64_t t0 = uint64_t(ram.u32(fxaddr::tex0 + 8 * k)) | (uint64_t(ram.u32(fxaddr::tex0 + 8 * k + 4)) << 32);
            if (!id || id >= 1000) return false;
            if (s.ok && s.id == id && s.tex0 == t0) return true;
            const uint32_t ptr = ram.u32(addr::texInfo + 16 * id) & 0x0FFFFFFF;
            s.id = id; s.tex0 = t0;
            s.ok = decodeUploadTex(ram, ptr, gs::Tex0::decode(t0), s.px, s.w, s.h, false, true) && s.w && s.h && s.px.size() >= size_t(s.w) * s.h;
            return s.ok;
        };
        DustSrc &src1 = dustSrc_[0], &src2 = dustSrc_[1];
        if (!source(32, src1) || !source(33, src2)) return dustTex_[dustLast_].id;
        const double startTime = GetTime();   // diagnostics (WOTM_DUSTLOG): cost of the math, excluding decoding the sources

        // ---- pass 1: sprite list
        auto fbits = [](uint32_t b) { float f; std::memcpy(&f, &b, 4); return f; };
        auto fbitsOf = [](float f) { uint32_t b; std::memcpy(&b, &f, 4); return b; };
        uint32_t rnd = 0x3F800000u | (seed & 0x7FFFFFu);   // the VU's RINIT / RNEXT
        auto rnext = [&]() { const uint32_t x = rnd & 0x7FFFFFu; rnd = ((((x << 1) | (((x >> 4) ^ (x >> 22)) & 1u)) & 0x7FFFFFu) | 0x3F800000u); return fbits(rnd); };
        float h[4];
        for (float &v : h) v = rnext();
        const float hc[4] = {fbits(0x3f3659ad), fbits(0x3f1ec458), fbits(0x3eb64807), fbits(0x3f16d60a)};
        const float hb[4] = {fbits(0x3f7e2a20), fbits(0x3f7cd6ea), fbits(0x3f66b74f), fbits(0x3f7e6ea7)};
        const float sx = P[2] / ctx[5][0], sy = P[2] / ctx[5][1];
        float col[3][3];
        for (int i = 0; i < 3; ++i) {
            const float *c = ctx[i];
            col[i][0] = (c[0] + ctx[6][0] * c[3]) * sx; col[i][1] = (c[1] + ctx[6][1] * c[3]) * sy; col[i][2] = P[6] * c[3];
        }
        const float fwd[3] = {ctx[0][3], ctx[1][3], ctx[2][3]};
        static const float kCenter[4][2] = {{2032, 2032}, {2064, 2032}, {2032, 2064}, {2064, 2064}};   // cells on the GS screen (2016 = texel 0)
        int16_t uvtab[16][4];   // the packet's UNPACK V4-16: 4 sub-images x 4 mirrorings of the source, in 1/16 texel
        for (uint32_t k = 0; k < 16; ++k)
            for (uint32_t c = 0; c < 4; ++c) uvtab[k][c] = ram.s16(kPkt + 0xB4 + 8 * k + 2 * c);
        struct DSpr { int x0, y0, x1, y1, a1; const int16_t *uv; float n[3]; int rgb2[3], a2; };
        std::vector<DSpr> spr;
        float r21[3] = {h[0] - 1.5f, h[1] - 1.5f, h[2] - 1.5f};
        int left = int(nloop), vi5 = 29;
        bool full = false;
        while (left > 0 && !full) {
            float t = P[3] + h[3];
            t -= t >= 0.f ? std::floor(t) : std::trunc(t);
            for (int i = 0; i < 4; ++i) { h[i] = hc[i] + h[i] * hb[i]; h[i] -= std::trunc(h[i]); }
            const float r2 = r21[0] * r21[0] + r21[1] * r21[1] + r21[2] * r21[2];
            const float half = (1.f + P[0]) + t * P[1];
            std::rotate(h, h + 1, h + 4);   // MR32
            const float rs = 1.f / std::sqrt(r2);
            float n[3] = {r21[0] * rs, r21[1] * rs, r21[2] * rs};
            for (int i = 0; i < 3; ++i) r21[i] = h[i] - 0.5f;
            for (int k = 0; k < 4; ++k) {
                const uint32_t idx = fbitsOf(h[0]) & 15;
                std::rotate(h, h + 1, h + 4);
                --left;
                if (vi5 >= 0x3FC) { full = true; break; }
                const float d[3] = {n[0] * t, n[1] * t, n[2] * t};
                const float cx = col[0][0] * d[0] + col[1][0] * d[1] + col[2][0] * d[2] + kCenter[k][0];
                const float cy = col[0][1] * d[0] + col[1][1] * d[1] + col[2][1] * d[2] + kCenter[k][1];
                const float cz = std::min(col[0][2] * d[0] + col[1][2] * d[1] + col[2][2] * d[2] + P[7], 128.f);
                const float a = std::min(1.f - t, t) * cz;
                if (a >= 1.f) {
                    DSpr s{int((cx - half) * 16.f) - 2016 * 16, int((cy - half) * 16.f) - 2016 * 16, int((cx + half) * 16.f) - 2016 * 16,
                           int((cy + half) * 16.f) - 2016 * 16, int(a), uvtab[idx], {n[0], n[1], n[2]}, {0, 0, 0}, 0};
                    spr.push_back(s);
                    vi5 += 5;
                }
                const float n0 = n[0];
                n[0] = -n[1]; n[1] = n[2]; n[2] = n0;   // the normal rotates from cell to cell: (x, y, z) -> (-y, z, x)
            }
        }

        // ---- pass 2: light
        auto light = [&](const float nv[3], int out[3]) {
            float dots[4];
            for (int j = 0; j < 4; ++j) dots[j] = std::max(0.f, L[0][j] * nv[0] + L[1][j] * nv[1] + L[2][j] * nv[2]);
            for (int i = 0; i < 3; ++i) {
                float v = C[4][i] + 0.5f;
                for (int j = 0; j < 4; ++j) v += C[j][i] * dots[j];
                out[i] = std::min(255, std::max(0, int(v)));
            }
        };
        int bg[3];
        light(fwd, bg);
        for (DSpr &s : spr) { light(s.n, s.rgb2); s.a2 = int(float(s.a1) * P[5]); }

        // ---- rasterization like the GS (bilinear with CLAMP on the source; top-left rule)
        // The bilinear sample splits into column and row: index already CLAMPed and the weight of each side, computed once per column and
        // per row of the sprite with the same float math (the result is the same as sampling per pixel).
        struct Tap { int i0, i1; float w0, w1; };
        auto taps = [](float u, int n) {
            u -= 0.5f;
            const int i = int(std::floor(u));
            const float f = u - float(i);
            return Tap{std::min(std::max(i, 0), n - 1), std::min(std::max(i + 1, 0), n - 1), 1.f - f, f};
        };
        std::vector<std::array<int, 4>> fb(64 * 64, std::array<int, 4>{0, 0, 0, 0});
        // greenOnly (pass 1): only the source's G and A count; the frame's R and B are replaced by the background before pass 2
        auto draw = [&](const DSpr &s, const DustSrc &src, const int rgb[3], int a, bool additive, bool greenOnly) {
            const float u0 = s.uv[0] / 16.f, v0 = s.uv[1] / 16.f, u1 = s.uv[2] / 16.f, v1 = s.uv[3] / 16.f;
            if (s.x1 <= s.x0 || s.y1 <= s.y0) return;
            const int pxa = std::max(0, (s.x0 + 15) >> 4), pxb = std::min(64, (s.x1 + 15) >> 4);
            const int pya = std::max(0, (s.y0 + 15) >> 4), pyb = std::min(64, (s.y1 + 15) >> 4);
            if (pxa >= pxb || pya >= pyb) return;
            Tap cx[64];
            for (int px = pxa; px < pxb; ++px)
                cx[px] = taps(u0 + (u1 - u0) * float(px * 16 - s.x0) / float(s.x1 - s.x0), int(src.w));
            const int c0 = greenOnly ? 1 : 0, c1 = greenOnly ? 2 : 3;
            for (int py = pya; py < pyb; ++py) {
                const Tap ry = taps(v0 + (v1 - v0) * float(py * 16 - s.y0) / float(s.y1 - s.y0), int(src.h));
                const uint32_t *row[2] = {src.px.data() + size_t(ry.i0) * src.w, src.px.data() + size_t(ry.i1) * src.w};
                const float wy[2] = {ry.w0, ry.w1};
#if defined(_M_X64) || defined(__SSE4_1__)
                // all 4 channels at once (RGBA in lanes): same sum and same integer math as the scalar version below
                const __m128i vrgb = _mm_setr_epi32(rgb[0], rgb[1], rgb[2], 0), v255 = _mm_set1_epi32(255), vzero = _mm_setzero_si128();
                (void)c0; (void)c1;
                auto texel = [](uint32_t p) { return _mm_cvtepi32_ps(_mm_cvtepu8_epi32(_mm_cvtsi32_si128(int(p)))); };
                for (int px = pxa; px < pxb; ++px) {
                    const Tap &tx = cx[px];
                    __m128 acc = _mm_setzero_ps();
                    for (int dj = 0; dj < 2; ++dj) {
                        acc = _mm_add_ps(acc, _mm_mul_ps(texel(row[dj][tx.i0]), _mm_set1_ps(tx.w0 * wy[dj])));
                        acc = _mm_add_ps(acc, _mm_mul_ps(texel(row[dj][tx.i1]), _mm_set1_ps(tx.w1 * wy[dj])));
                    }
                    const __m128i t = _mm_cvttps_epi32(acc);
                    const __m128i as = _mm_set1_epi32((a * _mm_extract_epi32(t, 3)) >> 7);
                    int *dp = fb[size_t(py) * 64 + size_t(px)].data();
                    const __m128i d = _mm_loadu_si128(reinterpret_cast<const __m128i *>(dp));
                    const __m128i cs = _mm_srai_epi32(_mm_mullo_epi32(vrgb, t), 7);
                    __m128i r;
                    if (additive)
                        r = _mm_min_epi32(v255, _mm_add_epi32(_mm_srai_epi32(_mm_mullo_epi32(cs, as), 7), d));
                    else
                        r = _mm_max_epi32(vzero, _mm_min_epi32(v255, _mm_add_epi32(_mm_srai_epi32(_mm_mullo_epi32(_mm_sub_epi32(cs, d), as), 7), d)));
                    r = _mm_blend_epi16(r, d, 0xC0);   // the frame's alpha doesn't change
                    _mm_storeu_si128(reinterpret_cast<__m128i *>(dp), r);
                }
#else
                for (int px = pxa; px < pxb; ++px) {
                    const Tap &tx = cx[px];
                    float acc[4] = {0, 0, 0, 0};
                    for (int dj = 0; dj < 2; ++dj) {   // same summation order as the per-pixel sample
                        const float wa = tx.w0 * wy[dj], wb = tx.w1 * wy[dj];
                        const uint32_t pa = row[dj][tx.i0], pb = row[dj][tx.i1];
                        for (int c = c0; c < c1; ++c) acc[c] += float((pa >> (8 * c)) & 255) * wa;
                        acc[3] += float(pa >> 24) * wa;
                        for (int c = c0; c < c1; ++c) acc[c] += float((pb >> (8 * c)) & 255) * wb;
                        acc[3] += float(pb >> 24) * wb;
                    }
                    const int as = (a * int(acc[3])) >> 7;
                    auto &d = fb[size_t(py) * 64 + size_t(px)];
                    for (int c = c0; c < c1; ++c) {
                        const int cs = (rgb[c] * int(acc[c])) >> 7;
                        d[c] = additive ? std::min(255, ((cs * as) >> 7) + d[c]) : std::max(0, std::min(255, (((cs - d[c]) * as) >> 7) + d[c]));
                    }
                }
#endif
            }
        };
        const int rgb1[3] = {1, int(P[4]), 1};
        for (const DSpr &s : spr) draw(s, src1, rgb1, s.a1, true, true);
        for (auto &p : fb) {
            for (int c = 0; c < 3; ++c) p[c] >>= 1;          // sprite (0, 0, 0, 64) in (Cs - Cd) * As + Cd: ((-Cd * 64) >> 7) + Cd
            p[3] = p[1] & 0x7F;                            // copyGreenChannelToAlpha (CT16's bit 15 comes out 0)
            for (int c = 0; c < 3; ++c) p[c] = bg[c];      // pass 2: background
        }
        for (const DSpr &s : spr) draw(s, src2, s.rgb2, s.a2, false, false);

        std::vector<uint32_t> px(64 * 64);   // raw GS alpha (0x80 = 1.0), like the other particle textures
        for (size_t i = 0; i < px.size(); ++i)
            px[i] = uint32_t(fb[i][0]) | (uint32_t(fb[i][1]) << 8) | (uint32_t(fb[i][2]) << 16) | (uint32_t(fb[i][3]) << 24);
        const double mathEnd = GetTime();
        const int slot = dustNext_;
        dustNext_ = (dustNext_ + 1) % kDustSlots;
        Texture2D &dt = dustTex_[slot];
        if (!dt.id) {
            Image img{px.data(), 64, 64, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            dt = LoadTextureFromImage(img);
            if (dt.id) { SetTextureFilter(dt, TEXTURE_FILTER_BILINEAR); SetTextureWrap(dt, TEXTURE_WRAP_CLAMP); }
        } else {
            UpdateTexture(dt, px.data());
        }
        dustHash_[slot] = hash;
        dustLast_ = slot;
        {   // diagnostics: WOTM_DUSTLOG=1 prints the summary on every regeneration
            static const bool lg = std::getenv("WOTM_DUSTLOG") != nullptr;
            if (lg) {
                int amax = 0; double asum = 0, cw[3] = {0, 0, 0};
                for (const auto &p : fb) { amax = std::max(amax, p[3]); asum += p[3]; for (int c = 0; c < 3; ++c) cw[c] += double(p[c]) * p[3]; }
                std::fprintf(stderr, "[dust] phase %.4f sprites %zu bg %d %d %d alpha max %d mean %.1f weighted color %.0f %.0f %.0f (%.2f ms, upload %.2f ms)\n", P[3], spr.size(), bg[0], bg[1], bg[2],
                             amax, asum / 4096.0, asum ? cw[0] / asum : 0.0, asum ? cw[1] / asum : 0.0, asum ? cw[2] / asum : 0.0, (mathEnd - startTime) * 1000.0, (GetTime() - mathEnd) * 1000.0);
            }
        }
        return dt.id;
    }
    struct DustSrc { std::vector<uint32_t> px; uint32_t w = 0, h = 0, id = 0; uint64_t tex0 = 0; bool ok = false; };
    DustSrc dustSrc_[2];   // sources 1 and 2 (TextureInfo[32] and [33]) decoded
    // one per view (the math depends on the camera): each view draws with its own, and a redraw of the same game frame reuses
    static constexpr int kDustSlots = 4;
    Texture2D dustTex_[kDustSlots]{};
    uint64_t dustHash_[kDustSlots]{};
    int dustNext_ = 0, dustLast_ = 0;

    Particles fx;   // the frame's particle systems (collected along with the scene, drawn after the translucent pass)

    // ------------------------------------------------------------ cast shadows (common/ps)
    // In retail, each active blocker (up to 10 in psBlkLst: monsters and the like, registered by psBlockerAdd) is drawn from a camera
    // placed in the light direction (view 4, at `radius * factor` from the origin) into a small texture, filtered and reprojected onto
    // nearby objects (psBldBlkRObjs). Here: the silhouette is rasterized on the CPU into an orthographic frame in light space (without
    // switching framebuffers inside BeginMode3D, which would break the runtime's render texture) and the triangles of nearby objects are
    // redrawn with that projected texture. docs/passos/NATIVO.md ("Projected shadows").
    std::vector<ShadowBlocker> shadows;

    // Active blockers, like psFindActiveBlockers: shadows on (psSetShadowOnOff), entry in use, cs drawable and visible in view 0
    void collectShadows(const Ram &ram) {
        shadows.clear();
        static const bool off = std::getenv("WOTM_NOSHADOW") != nullptr;
        if (off || !ram.u32(addr::psShadowOn)) return;
        for (int i = 0; i < 10; ++i) {
            const uint32_t e = addr::psBlkLst + 0x30 * uint32_t(i);
            const uint32_t cs = ram.u32(e + 0x10);
            if (ram.u16(e + 0x2E) != 1 || !ram.ok(cs, 0xB0) || !ram.u8(cs + 0xC) || !(ram.u8(cs + 0xE) & 1)) continue;
            ShadowBlocker b;
            b.index = i;
            for (int c = 0; c < 3; ++c) b.origin[c] = ram.f32(cs + 0x10 + 4 * c);
            const uint32_t rp = ram.u32(e + 0x20);   // &radius of the sphere of the cs's root node (GROUP); 0 if the node isn't a GROUP
            const float r = rp ? ram.f32(rp) : 0.f;
            b.extent = (r > 1.f && r < 5000.f ? r : 80.f) * 1.1f;
            b.shadowCs = ram.u32(e + 0x14);
            std::vector<DrawItem> keep;
            keep.swap(items);
            // LOD like hierShadow: the blocker's fixed distance (lodRangeSq at +0x28, 1200^2 in psBlockerAdd; the final boss uses
            // 20000^2), not the view eye's; and without view culling (the game walks with the light camera, which covers the blocker).
            // That way the silhouette is the same in every split-screen view (Scene::shadowTexture reuses it) and a monster half out
            // of view doesn't lose part of its shadow. With no useful value in the entry: the eye, as before.
            const float lodD2 = ram.f32(e + 0x28);
            static const bool eyeLod = std::getenv("WOTM_SHADOWEYELOD") != nullptr;   // diagnostics: LOD and culling by the eye (as before)
            shadowLodD2_ = !eyeLod && std::isfinite(lodD2) && lodD2 > 0.f ? lodD2 : -1.f;
            drawCs(ram, cs, false);
            const uint32_t cs2 = ram.u32(e + 0x18);   // second cs (psBlockerSet2ndCs: what the monster is holding, for example)
            if (ram.ok(cs2, 0xB0) && ram.u8(cs2 + 0xC)) drawCs(ram, cs2, false);
            shadowLodD2_ = -1.f;
            b.casters.swap(items);
            items.swap(keep);
            for (const DrawItem &it : b.casters) b.casterNodes.push_back(it.node);   // all of them (receiver excluded), before the filter
            // dontCastShadow (bit 29 of the OBJECT bits at +0xC): psBldBlkRObjs leaves without emitting the object. On Kineticlops only the eye
            // casts; the translucent body has the bit.
            constexpr uint32_t kObjDontCastShadow = 0x20000000;
            b.casters.erase(std::remove_if(b.casters.begin(), b.casters.end(),
                                           [&](const DrawItem &it) { return (ram.u32(it.node + 0xC) & kObjDontCastShadow) != 0; }),
                            b.casters.end());
            std::sort(b.casterNodes.begin(), b.casterNodes.end());
            if (!b.casters.empty()) shadows.push_back(std::move(b));
        }
    }

    // Draws the shadows over the objects already drawn (after the opaque ones, before the translucent ones).
    void drawShadows(const Ram &ram) {
        if (shadows.empty()) return;
        float L[3] = {ram.f32(addr::psLitDirWs), ram.f32(addr::psLitDirWs + 4), ram.f32(addr::psLitDirWs + 8)};   // from the scene toward the light
        const float ll = std::sqrt(L[0] * L[0] + L[1] * L[1] + L[2] * L[2]);
        if (!(ll > 1e-3f)) return;
        for (float &c : L) c /= ll;
        // orthonormal basis of the light frame
        const float hint[3] = {std::fabs(L[2]) < 0.9f ? 0.f : 1.f, 0.f, std::fabs(L[2]) < 0.9f ? 1.f : 0.f};
        float R[3] = {hint[1] * L[2] - hint[2] * L[1], hint[2] * L[0] - hint[0] * L[2], hint[0] * L[1] - hint[1] * L[0]};
        const float rl = std::sqrt(R[0] * R[0] + R[1] * R[1] + R[2] * R[2]);
        for (float &c : R) c /= rl;
        const float U[3] = {L[1] * R[2] - L[2] * R[1], L[2] * R[0] - L[0] * R[2], L[0] * R[1] - L[1] * R[0]};
        static const float kShadowPull = std::getenv("WOTM_SHADOWPULL") ? float(std::atof(std::getenv("WOTM_SHADOWPULL"))) : 0.002f;   // diagnostics
        const float kShadowPullMax = kShadowPull * 1000.f;
        float inten = ram.f32(addr::psShadowIntens);   // 80 in psVarInit (GS scale: 0x80 = 1.0)
        inten = (inten > 0.f && inten < 1000.f) ? std::min(1.f, inten / 128.f) : 0.625f;

        rlDrawRenderBatchActive();
        rlDisableDepthMask();
        rlDisableBackfaceCulling();
        rlSetBlendMode(BLEND_ALPHA);
        for (size_t k = 0; k < shadows.size(); ++k) {
            const ShadowBlocker &b = shadows[k];
            static const bool shadowLog = std::getenv("WOTM_SHADOWLOG") != nullptr;   // once: getenv is slow on Windows
            if (shadowLog) {
                std::fprintf(stderr, "[shadow] blocker %d origin %.1f %.1f %.1f extent %.1f light %.3f %.3f %.3f receiver cs %x (0 = stage), %zu cast:\n", b.index,
                             b.origin[0], b.origin[1], b.origin[2], b.extent, L[0], L[1], L[2], b.shadowCs, b.casters.size());
                for (const DrawItem &it : b.casters) {
                    const MeshData *md = mesh(ram, it.node);
                    std::fprintf(stderr, "[shadow]   node %x tex %u bits %08x lod %d fade %.2f pos %.1f %.1f %.1f tris %zu skinned %d\n", it.node, unsigned(ram.u16(it.node + 0x50)),
                                 ram.u32(it.node + 0xC), it.lodIdx, it.fade, it.m.m[3][0], it.m.m[3][1], it.m.m[3][2], md ? md->tris.size() / 3 : size_t(0), md ? int(md->skinned) : -1);
                }
            }
            unsigned tex = 0;
            {
                ProfScope ps(DrawProf::ShadowTex);
                tex = shadowTexture(ram, k, b, R, U);
            }
            if (!tex) continue;
            const float E = b.extent;
            auto lightUV = [&](const float p[3], float &u, float &v, float &h) {
                const float d[3] = {p[0] - b.origin[0], p[1] - b.origin[1], p[2] - b.origin[2]};
                u = (d[0] * R[0] + d[1] * R[1] + d[2] * R[2]) / E;
                v = (d[0] * U[0] + d[1] * U[1] + d[2] * U[2]) / E;
                h = (d[0] * L[0] + d[1] * L[1] + d[2] * L[2]) / E;   // > 0: between the blocker and the light
            };
            rlBegin(RL_TRIANGLES);
            rlSetTexture(tex);
            auto receive = [&](const std::vector<DrawItem> &list) {
                for (const DrawItem &it : list) {
                    if (std::binary_search(b.casterNodes.begin(), b.casterNodes.end(), it.node)) continue;
                    // only an OBJECT without 0x200 or 0x60 in its flags receives shadow: hierTraverseAsm only calls psObjectCull (the
                    // receiver test) on those. In luta_hud only the floors receive (flags 0x90); the glass, trees and glows don't
                    if (ram.u16(it.node + 0xC) & 0x260) continue;
                    // and only the _cs the blocker indicates (psObjectCull compares entry +0x14 with the _cs being traversed). Almost always
                    // it's 0, the stage; Kineticlops points to the _cs of a quad on the ground (tex 324) and only that receives: native also
                    // cast onto the floor, and the shadow came out doubled
                    static const bool anyCs = std::getenv("WOTM_SHADOWANYCS") != nullptr;   // diagnostics: any _cs receives (as before)
                    if (it.cs != b.shadowCs && !anyCs) continue;
                    // object sphere (+0x10 center, +0x1C radius) against the shadow cylinder
                    float c[3];
                    xform(it.m, ram.f32(it.node + 0x10), ram.f32(it.node + 0x14), ram.f32(it.node + 0x18), c);
                    const float rad = std::fabs(ram.f32(it.node + 0x1C));
                    float u, v, h;
                    lightUV(c, u, v, h);
                    const float rr = rad / E;
                    if (!(std::isfinite(u) && std::isfinite(rr)) || u * u + v * v > (1.5f + rr) * (1.5f + rr) || h - rr > 0.5f) continue;
                    static const bool slog = std::getenv("WOTM_SHADOWLOG") != nullptr;
                    if (slog)
                        std::fprintf(stderr, "[shadow] blocker %d receives (cs %x) node %x tex %u flags %04x center %.1f %.1f %.1f radius %.1f (u %.2f v %.2f h %.2f)\n", b.index,
                                     it.cs, it.node, unsigned(ram.u16(it.node + 0x50)), unsigned(ram.u16(it.node + 0xC)), c[0], c[1], c[2], rad, u, v, h);
                    // the receiver's zBufferFudge, as in drawList: Kineticlops's shadow quad sits flush with the floor and only gets
                    // in front of it through the fudge (1.0122). Only on receivers with their own cs: the stage floor layers (fudge 1.0012)
                    // darken the shadow beyond the reference with fudge (congar_olhos)
                    const float fz = ram.f32(it.node + 0x28);
                    const bool fudge = it.cs != 0 && eyeOk_ && fz > 0.5f && fz < 2.f && fz != 1.f;
                    if (fudge && slog) std::fprintf(stderr, "[shadow]   fudge %.4f node %x\n", fz, it.node);
                    forEachTriPos(ram, it, [&](const float p[3][3]) {
                        float tu[3], tv[3], th[3];
                        for (int i = 0; i < 3; ++i) lightUV(p[i], tu[i], tv[i], th[i]);
                        if ((tu[0] < -1 && tu[1] < -1 && tu[2] < -1) || (tu[0] > 1 && tu[1] > 1 && tu[2] > 1) ||
                            (tv[0] < -1 && tv[1] < -1 && tv[2] < -1) || (tv[0] > 1 && tv[1] > 1 && tv[2] > 1) ||
                            (th[0] > 0.5f && th[1] > 0.5f && th[2] > 0.5f))
                            return;
                        for (int i = 0; i < 3; ++i) {
                            // vanishes above the blocker (light side); pulls the vertex toward the eye so it doesn't fight the surface
                            const float fade = th[i] < 0.15f ? 1.f : std::max(0.f, 1.f - (th[i] - 0.15f) / 0.35f);
                            float q[3] = {p[i][0], p[i][1], p[i][2]};
                            if (fudge) for (int a = 0; a < 3; ++a) q[a] = eye_[a] + (q[a] - eye_[a]) / fz;
                            if (eyeOk_) {
                                const float dx = eye_[0] - q[0], dy = eye_[1] - q[1], dz = eye_[2] - q[2];
                                const float dl = std::sqrt(dx * dx + dy * dy + dz * dz);
                                // 0.2% of the distance (up to 2); WOTM_SHADOWPULL overrides it (in luta_hud 0 to 0.002 gives the same)
                                if (dl > 1e-3f) { const float s = std::min(kShadowPullMax, dl * kShadowPull) / dl; q[0] += dx * s; q[1] += dy * s; q[2] += dz * s; }
                            }
                            rlColor4ub(255, 255, 255, uint8_t(255.f * inten * fade));
                            rlTexCoord2f(tu[i] * 0.5f + 0.5f, tv[i] * 0.5f + 0.5f);
                            rlVertex3f(q[0], q[1], q[2]);
                        }
                    });
                }
            };
            receive(items);
            rlEnd();
            rlDrawRenderBatchActive();
        }
        rlSetTexture(0);
        rlEnableDepthMask();
        rlEnableBackfaceCulling();
    }

    // Back face seen from `eye`: VU1 (the objects' microprogram) puts ADC on triangles with positive area on the GS screen
    // (y down), only in sections with bit 15 of the selector (Vtx::cull). In world space: normal (b - a) x (c - a) facing away from the eye.
    static bool backFacing(const float p[3][3], const float eye[3]) {
        const float u[3] = {p[1][0] - p[0][0], p[1][1] - p[0][1], p[1][2] - p[0][2]};
        const float v[3] = {p[2][0] - p[0][0], p[2][1] - p[0][1], p[2][2] - p[0][2]};
        const float n[3] = {u[1] * v[2] - u[2] * v[1], u[2] * v[0] - u[0] * v[2], u[0] * v[1] - u[1] * v[0]};
        return n[0] * (p[0][0] - eye[0]) + n[1] * (p[0][1] - eye[1]) + n[2] * (p[0][2] - eye[2]) > 0.f;
    }

    // Triangles of an item in world coordinates (with skinning), all slots: f(p, v, rgb) with the three decoded vertices (UV,
    // color, texture slot and the section's culling), each one's color in GS units (vertexRgb: with VU1 lighting on lit ones) and the
    // world-space normal for the environment map (envNormal; zero without DrawItem::env)
    template <class F> void forEachTri(const Ram &ram, const DrawItem &it, F &&f) {
        const MeshData *md = mesh(ram, it.node);
        if (!md || !md->valid) return;
        const uint32_t pal = md->skinned ? palette(ram, it) : 0;
        const ObjLight ol = objectLight(ram, it);
        const auto &t = md->tris;
        std::vector<M4> pm;   // palette (bone x for the normal on lit objects and in the environment map)
        if (pal && (ol.lit || it.env))
            for (uint32_t s = 0; s < 32; ++s) pm.push_back(readM4(ram, pal + 0x10 + 0x40 * s));
        float p[3][3], nw[3][3] = {};
        uint8_t rgb[3][3];
        for (size_t k = 0; k + 2 < t.size(); k += 3) {
            for (int c = 0; c < 3; ++c) {
                const Vtx &v = t[k + c];
                const M4 *bone = pm.empty() ? nullptr : &pm[(v.cx & 0x7F) / 4];
                if (pal) skinVertex(ram, pal, it.m, v, p[c]);
                else xform(it.m, v.x, v.y, v.z, p[c]);
                vertexRgb(ol, it.color, bone, it.m, v, rgb[c]);
                if (it.env) envNormal(v, bone, it.m, nw[c]);
            }
            f(p, &t[k], rgb, nw);
        }
    }

    // Only the world positions (with skinning) of an item's triangles, without color or normal: what shadows use. The palette is the
    // snapshot's (DrawItem::pal, the same as drawList's); without it, the RAM's.
    template <class F> void forEachTriPos(const Ram &ram, const DrawItem &it, F &&f) {
        const MeshData *md = mesh(ram, it.node);
        if (!md || !md->valid) return;
        std::vector<M4> tmp;
        const M4 *pm = nullptr;
        if (md->skinned) {
            if (it.pal && it.pal->size() >= 32) pm = it.pal->data();
            else if (const uint32_t pal = palette(ram, it)) {
                for (uint32_t s = 0; s < 32; ++s) tmp.push_back(readM4(ram, pal + 0x10 + 0x40 * s));
                pm = tmp.data();
            }
        }
        const auto &t = md->tris;
        float p[3][3];
        for (size_t k = 0; k + 2 < t.size(); k += 3) {
            for (int c = 0; c < 3; ++c) {
                if (pm) skinVertexPM(pm, it.m, t[k + c], p[c]);
                else xform(it.m, t[k + c].x, t[k + c].y, t[k + c].z, p[c]);
            }
            f(p);
        }
    }

    // Silhouette key: everything that goes into it (blocker, light frame and, for each caster, node, matrix and palette), with
    // positions in 1/4 unit and rotations in 1/1024. Each view's hier re-evaluates the pose and the last bits of the matrices change from
    // one view to another (same game frame, same numbers to the eye); the silhouette has ~2 units per texel, so that doesn't
    // change it. Split-screen views 1 to 3 end up sharing the texture; view 0 (before TheGame::Update) falls back to its own when there is movement.
    static uint64_t shadowKey(const ShadowBlocker &b, const float R[3], const float U[3]) {
        uint64_t h = 1469598103934665603ull;
        auto mixI = [&](int64_t v) {
            for (int i = 0; i < 8; ++i) h = (h ^ uint8_t(v >> (8 * i))) * 1099511628211ull;
        };
        auto q = [](float f, float s) { return std::isfinite(f) ? int64_t(std::llround(double(f) * s)) : INT64_MIN; };
        auto mixM = [&](const M4 &m) {
            for (int r = 0; r < 4; ++r)
                for (int c = 0; c < 4; ++c) mixI(q(m.m[r][c], r == 3 ? 4.f : 1024.f));
        };
        mixI(b.index);
        for (int c = 0; c < 3; ++c) mixI(q(b.origin[c], 4.f));
        mixI(q(b.extent, 4.f));
        for (int c = 0; c < 3; ++c) mixI(q(R[c], 1024.f)), mixI(q(U[c], 1024.f));
        for (const DrawItem &it : b.casters) {
            mixI(it.node);
            mixM(it.m);
            if (it.pal) for (const M4 &m : *it.pal) mixM(m);
            else mixI(it.animPkt);   // no copy (viewer): reads RAM, no cache across views
        }
        return h;
    }

    // Stored silhouette (Scene::shadowCache_)
    struct ShadowCacheEntry {
        uint64_t key = 0, used = 0, redraw = ~0ull;
        Texture2D tex{};
        int index = -1;
        float origin[3] = {}, extent = 0.f, R[3] = {};
        std::vector<uint32_t> nodes;
        std::vector<float> pos;   // translation (m[3]) of each casting node
    };

    // Does another view's silhouette from the same redraw work for this one? Same blocker, same light, same casting nodes, and origin,
    // extent and position of each node within what the silhouette (~2 units per texel) can't show. Each view's hier re-evaluates the pose,
    // and limbs move a few tenths of a unit from one view to another in the same frame: the exact key almost never matched.
    bool shadowNear(const ShadowCacheEntry &e, const ShadowBlocker &b, const float R[3]) const {
        if (e.redraw != redrawId || e.index != b.index || e.nodes.size() != b.casters.size()) return false;
        if (std::fabs(e.extent - b.extent) > 0.02f * b.extent) return false;
        for (int c = 0; c < 3; ++c)
            if (std::fabs(e.origin[c] - b.origin[c]) > kShadowTol || std::fabs(e.R[c] - R[c]) > 1e-3f) return false;
        for (size_t i = 0; i < b.casters.size(); ++i) {
            const DrawItem &it = b.casters[i];
            if (e.nodes[i] != it.node) return false;
            for (int c = 0; c < 3; ++c)
                if (std::fabs(e.pos[3 * i + c] - it.m.m[3][c]) > kShadowTol) return false;
        }
        return true;
    }

    // Blocker silhouette in the light frame: rasterized on the CPU (kShadowRes^2), blurred (retail filters with sprites) and sent
    // as a black texture with alpha = coverage. Reused by the entries' key (shadowKey: a redraw without a new frame) or from
    // another view of the same redraw (shadowNear: split screen). Items without a palette copy (viewer) always redo it.
    unsigned shadowTexture(const Ram &ram, size_t slot, const ShadowBlocker &b, const float R[3], const float U[3]) {
        constexpr int N = kShadowRes;
        bool cacheable = true;
        for (const DrawItem &it : b.casters) cacheable = cacheable && (!it.animPkt || it.pal);
        const uint64_t key = cacheable ? shadowKey(b, R, U) : 0;
        ++shadowTick_;
        static const bool slogTex = std::getenv("WOTM_SHADOWLOG") != nullptr;
        static const bool noReuse = std::getenv("WOTM_SHADOWNOCACHE") != nullptr;   // diagnostics: redo every silhouette (as before)
        if (cacheable && !slogTex && !noReuse)
            for (ShadowCacheEntry &e : shadowCache_)
                if (e.tex.id && (e.key == key || shadowNear(e, b, R))) {
                    e.used = shadowTick_;
                    if (drawProfOn()) ++drawProf().shadowHit;
                    return e.tex.id;
                }
        if (drawProfOn()) ++drawProf().shadowMiss;
        static const bool keyLog = std::getenv("WOTM_SHADOWKEYLOG") != nullptr;   // diagnostics: what goes into each silhouette's key
        if (keyLog) {
            std::fprintf(stderr, "[shadowkey] blocker %d key %016llx origin %.3f %.3f %.3f extent %.3f casters", b.index, (unsigned long long)key,
                         b.origin[0], b.origin[1], b.origin[2], b.extent);
            for (const DrawItem &it : b.casters) {
                ShadowBlocker one;
                one.casters.push_back(it);
                const uint64_t hm = shadowKey(one, R, U);   // only this one (matrix and palette)
                one.casters.back().pal.reset();
                std::fprintf(stderr, " %x(lod %d, pal %d, x %.3f, m+pal %04llx, m %04llx)", it.node, it.lodIdx, it.pal ? 1 : 0, it.m.m[3][0],
                             (unsigned long long)(hm & 0xFFFF), (unsigned long long)(shadowKey(one, R, U) & 0xFFFF));
            }
            std::fprintf(stderr, "\n");
        }
        (void)slot;
        std::vector<float> &cov = shadowCov_;
        cov.assign(size_t(N * N), 0.f);
        const float E = b.extent;
        for (const DrawItem &it : b.casters) {
            forEachTriPos(ram, it, [&](const float p[3][3]) {
                float x[3], y[3];
                for (int i = 0; i < 3; ++i) {
                    const float d[3] = {p[i][0] - b.origin[0], p[i][1] - b.origin[1], p[i][2] - b.origin[2]};
                    x[i] = ((d[0] * R[0] + d[1] * R[1] + d[2] * R[2]) / E * 0.5f + 0.5f) * N;
                    y[i] = ((d[0] * U[0] + d[1] * U[1] + d[2] * U[2]) / E * 0.5f + 0.5f) * N;
                }
                const float area = (x[1] - x[0]) * (y[2] - y[0]) - (x[2] - x[0]) * (y[1] - y[0]);
                if (!(std::fabs(area) > 1e-6f)) return;
                const int x0 = std::max(1, int(std::floor(std::min({x[0], x[1], x[2]})))), x1 = std::min(N - 2, int(std::ceil(std::max({x[0], x[1], x[2]}))));
                const int y0 = std::max(1, int(std::floor(std::min({y[0], y[1], y[2]})))), y1 = std::min(N - 2, int(std::ceil(std::max({y[0], y[1], y[2]}))));
                for (int py = y0; py <= y1; ++py)
                    for (int px = x0; px <= x1; ++px) {
                        const float cx = px + 0.5f, cy = py + 0.5f;
                        const float w0 = (x[1] - cx) * (y[2] - cy) - (x[2] - cx) * (y[1] - cy);
                        const float w1 = (x[2] - cx) * (y[0] - cy) - (x[0] - cx) * (y[2] - cy);
                        const float w2 = (x[0] - cx) * (y[1] - cy) - (x[1] - cx) * (y[0] - cy);
                        if ((w0 >= 0 && w1 >= 0 && w2 >= 0) || (w0 <= 0 && w1 <= 0 && w2 <= 0)) cov[size_t(py * N + px)] = 1.f;
                    }
            });
        }
        for (int pass = 0; pass < 2; ++pass) {   // 3x3 box twice; the border stays at 0 (CLAMP doesn't extend the shadow)
            std::vector<float> &o = shadowTmp_;
            o.assign(cov.size(), 0.f);
            for (int y = 1; y < N - 1; ++y)
                for (int x = 1; x < N - 1; ++x) {
                    float s = 0;
                    for (int dy = -1; dy <= 1; ++dy)
                        for (int dx = -1; dx <= 1; ++dx) s += cov[size_t((y + dy) * N + x + dx)];
                    o[size_t(y * N + x)] = s / 9.f;
                }
            cov.swap(o);
        }
        std::vector<uint32_t> &px = shadowPx_;
        px.resize(cov.size());
        for (size_t i = 0; i < cov.size(); ++i) px[i] = uint32_t(std::min(1.f, cov[i]) * 255.f) << 24;
        if (slogTex) {   // diagnostics: silhouette in shadow_<slot>.png (white = shadow) in the current directory
            std::vector<uint32_t> vis(px.size());
            for (size_t i = 0; i < px.size(); ++i) { const uint32_t a = px[i] >> 24; vis[i] = 0xFF000000u | a | (a << 8) | (a << 16); }
            Image img{vis.data(), N, N, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            char name[64];
            std::snprintf(name, sizeof name, "shadow_%zu.png", slot);
            ExportImage(img, name);
        }
        // entry to reuse: the least recently used (at most kShadowCacheN textures; 10 blockers x 4 views fit)
        ShadowCacheEntry *e = nullptr;
        if (shadowCache_.size() < kShadowCacheN) e = &shadowCache_.emplace_back();
        else
            for (ShadowCacheEntry &c : shadowCache_)
                if (!e || c.used < e->used) e = &c;
        e->key = cacheable ? key : 0;
        e->used = shadowTick_;
        e->redraw = cacheable ? redrawId : ~0ull;
        e->index = b.index;
        e->extent = b.extent;
        std::copy(b.origin, b.origin + 3, e->origin);
        std::copy(R, R + 3, e->R);
        e->nodes.clear();
        e->pos.clear();
        for (const DrawItem &it : b.casters) {
            e->nodes.push_back(it.node);
            for (int c = 0; c < 3; ++c) e->pos.push_back(it.m.m[3][c]);
        }
        Texture2D &t = e->tex;
        if (!t.id) {
            Image img{px.data(), N, N, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            t = LoadTextureFromImage(img);
            if (t.id) { SetTextureFilter(t, TEXTURE_FILTER_BILINEAR); SetTextureWrap(t, TEXTURE_WRAP_CLAMP); }
        } else {
            UpdateTexture(t, px.data());
        }
        return t.id;
    }

    // Mod PNG on a particle texture (raw GS alpha, like the original). Particle UVs are normalized by the TEX0, so
    // any resolution works. Without the original (missing palette), the PNG goes in alone, with alpha on the 0..0x80 scale.
    void modParticle(const Ram &ram, uint32_t texId, uint64_t tex0, bool &ok, std::vector<uint32_t> &px, uint32_t &w, uint32_t &h) {
        if (texmods::Index::get().empty()) return;
        uint32_t amax = 0;
        if (ok) for (uint32_t c : px) amax = std::max(amax, c >> 24);
        std::vector<uint32_t> mpx;
        uint32_t mw = 0, mh = 0;
        if (modTexture(ram, texId, gs::Tex0::decode(tex0), (tex0 >> 34) & 1, true, mpx, mw, mh, amax > 0x80)) {
            px.swap(mpx); w = mw; h = mh; ok = true;
        }
    }

    // Effect texture (particles): `texInfo[id]` holds the packet (as for objects) and the effect's TEX0 (fx[i].TEX0ptr) carries psm and the palette CBP.
    bool decodeFxTex(const Ram &ram, uint32_t id, uint64_t tex0, std::vector<uint32_t> &px, uint32_t &w, uint32_t &h) const {
        const gs::Tex0 t0 = gs::Tex0::decode(tex0);
        const uint32_t ptr = ram.u32(addr::texInfo + 16 * id) & 0x0FFFFFFF;
        if (!ram.ok(ptr + 0x80)) return false;
        uint32_t pal[256] = {};
        const UploadHdr hd = UploadHdr::read(ram, ptr);
        const uint32_t psm = hd.psm;
        if (psm == gs::T8 || psm == gs::T4) {
            const auto it = rtxPal.find(t0.cbp);
            if (it == rtxPal.end()) return false;
            fillPal(pal, it->second, psm == gs::T8, true);
        }
        w = hd.w; h = hd.h;
        return w && h && gs::decodeUpload(ram.p + ptr + 0x80, ram.size - ptr - 0x80, w, h, psm, t0.csa, pal, px, true);
    }

    // diagnostics: WOTM_TEXFROM=<from>:<to> draws the vertices of slot <from> with the texture of slot <to>
    static uint32_t texRemap(uint32_t slot) {
        static int from = -1, to = -1; static bool init = false;
        if (!init) { init = true; if (const char *e = std::getenv("WOTM_TEXFROM")) std::sscanf(e, "%d:%d", &from, &to); }
        return (from >= 0 && int(slot) == from) ? uint32_t(to) : slot;
    }

    unsigned (*movieTexture)() = nullptr;   // movie texture (texId 999); 0 = no frame. Filled by whoever plays the movie (wotm_live.inc)
    // previous frame (texId 0), upside-down render texture with the whole 640x224 GS screen; 0 = none. Filled by: wotm_live.inc
    unsigned (*screenTexture)() = nullptr;
    unsigned screenId_ = 0;   // last id returned for kScreenKey (drawing converts its UV)

    unsigned textureId(const Ram &ram, const DrawItem &it, uint32_t slot) {
        if (texKey(ram, it, slot) == kScreenKey) return screenId_ = screenTexture ? screenTexture() : 0;
        return textureId(ram, it.node, slot);
    }
    // texture replacing that of object `node` (texture slot), with raw alpha and bottom-to-top rows like the game's; 0 = the
    // object's. Filled by: wotm_levelsel.inc (photo of the stages without art in SHELL)
    unsigned (*nodeTexture)(uint32_t node, uint32_t slot) = nullptr;

    unsigned textureId(const Ram &ram, uint32_t node, uint32_t slot = 0) {
        if (nodeTexture)
            if (const unsigned id = nodeTexture(node & 0x1FFFFFFFu, slot)) return id;
        // menu movie: moviesSetScreenObject puts texId 999 on the object (wotm_fmv.hpp); the texture comes from the player, frame by frame
        if (movieTexture && objectTexId(ram, node, slot) == 999)
            if (const unsigned id = movieTexture()) return id;
        const uint64_t key = texKey(ram, node, slot);
        if (!key) return 0;
        if (key == kScreenKey) return screenId_ = screenTexture ? screenTexture() : 0;
        auto it = tex_.find(key);
        if (it != tex_.end()) return it->second.id;
        Texture2D t{};
        std::vector<uint32_t> px;
        uint32_t w = 0, h = 0;
        const uint64_t t0v = objectTex0(ram, node, slot);
        // raw GS alpha: the draw shader blends As = At * Av / 16384. The original comes first: the mod PNG reads alpha on its scale
        const bool orig = decodeTex(ram, node, px, w, h, slot, true);
        uint32_t origAmax = 0;
        if (orig)
            for (uint32_t c : px) origAmax = std::max(origAmax, c >> 24);
        std::vector<uint32_t> mpx;
        uint32_t mw = 0, mh = 0;
        const bool fromMod = modTexture(ram, objectTexId(ram, node, slot), gs::Tex0::decode(t0v), (t0v >> 34) & 1, true, mpx, mw, mh,
                                        origAmax > 0x80);
        if (fromMod) { px.swap(mpx); w = mw; h = mh; }
        if (fromMod || orig) {
            if (std::getenv("WOTM_ALPHASTAT")) {
                size_t a0 = 0, am = 0;
                for (uint32_t c : px) { const uint32_t a = c >> 24; if (a == 0) ++a0; else if (a < 255) ++am; }
                if (a0 || am) std::fprintf(stderr, "alpha tex=%u %ux%u transparent=%.0f%% partial=%.0f%%\n", ram.u16(node + 0x50), w, h, 100.0 * a0 / px.size(), 100.0 * am / px.size());
            }
            {
                // Texels with alpha 0 hold an arbitrary (dark) color; the bilinear filter blends it in at the edges and makes a fringe.
                // Spreads the color of visible neighbors into the transparent ones (2 passes of 3x3).
                size_t partial = 0, clear = 0;
                // raw alpha: semi-transparent = below ~1.0 with the vertex at 0x80 (0x7C); 0x80..0xFF is opaque
                for (uint32_t c : px) { const uint32_t a = c >> 24; if (a == 0) ++clear; else if (a < 0x7C) ++partial; }
                partial_[key] = partial * 100 > px.size();   // more than 1% of texels semi-transparent
                uint32_t amax = 0;
                for (uint32_t c : px) amax = std::max(amax, c >> 24);
                texAmax_[key] = uint8_t(amax);
                if (clear && clear < px.size()) {
                    std::vector<uint8_t> solid(px.size());
                    for (size_t i = 0; i < px.size(); ++i) solid[i] = (px[i] >> 24) != 0;
                    for (int pass = 0; pass < 2; ++pass) {
                        std::vector<uint32_t> np = px;
                        std::vector<uint8_t> ns = solid;
                        for (uint32_t y = 0; y < h; ++y)
                            for (uint32_t x = 0; x < w; ++x) {
                                if (solid[y * w + x]) continue;
                                uint32_t r = 0, g = 0, b = 0, n = 0;
                                for (int dy = -1; dy <= 1; ++dy)
                                    for (int dx = -1; dx <= 1; ++dx) {
                                        const size_t q = size_t((y + h + uint32_t(dy)) % h) * w + (x + w + uint32_t(dx)) % w;
                                        if (!solid[q]) continue;
                                        r += px[q] & 255; g += (px[q] >> 8) & 255; b += (px[q] >> 16) & 255; ++n;
                                    }
                                if (n) { np[y * w + x] = (r / n) | ((g / n) << 8) | ((b / n) << 16); ns[y * w + x] = 1; }
                            }
                        px.swap(np);
                        solid.swap(ns);
                    }
                }
            }
            Image img{};
            img.data = px.data(); img.width = int(w); img.height = int(h); img.mipmaps = 1;
            img.format = PIXELFORMAT_UNCOMPRESSED_R8G8B8A8;
            t = LoadTextureFromImage(img);
            if (t.id && fromMod) { GenTextureMipmaps(&t); SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR); }   // high-res PNG: no aliasing at a distance
            else if (t.id) SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
            if (t.id) SetTextureWrap(t, TEXTURE_WRAP_REPEAT);
        }
        applyTexFilter(t);
        tex_[key] = t;
        return t.id;
    }

    // colorQuad (+0x70) of a _cs that the game changes between hier(v,0) and the end of the frame, as the value was when hier drew. Known
    // case: the pick-up the monster can grab. rtMain calls LevelPickups::enableHighlights (pulses green in the colorQuad of the highlighted
    // pick-up's _cs) before hier(v,0), and Monster::update, inside TheGame::Update after it, calls computeHighlight, which
    // resets colorQuad to 1. Capture at the end of the frame only saw the 1 (the car never turned green). Filled by the live capture
    // (enableHighlights hook in the runtime); empty in the viewer and the replay, which read RAM.
    struct CsColorAtDraw { uint32_t cs; float q[4]; };
    std::vector<CsColorAtDraw> csColorAtDraw;

    // Gathers the visible objects from world->ep and the CS lists.
    // maxLod < 0: automatic LOD by distance to the game camera's eye (like hierLod); >= 0 forces that index (0 = coarsest).
    void collect(const Ram &ram, int maxLod = -1) {
        ViewScope vs(vctx);
        lights_ = readParaLights(ram);
        pts_ = readPointLights(ram);
        fog_ = readFog(ram);
        items.clear();
        sky.clear();
        overlay.clear();
        hud.clear();
        hp.clear();
        hpOverlay.clear();
        sprites.clear();
        worldLights.clear();
        emitters.clear();
        atMat_ = 0;
        animVal_ = 0;
        cs_ = 0;
        blend_ = kGsAlphaNormal;
        aref_ = kGsAlphaRef;
        zmsk_ = false;
        {   // traversal starts at the game's default ctx (ctxGetDefaultGSCtx = ctxDef, built by ctxSetUpGSCtx: ALPHA_1 0x44,
            // TEST_1 0x5040C = no alpha test, ZBUF_1 writing). Without it native assumed 0x8D (alpha > 8) and discarded the top of the
            // drive-in's sky dome (vertex alpha 0), which the GS draws.
            int b, r, z;
            groupState(ram, sym::ctxDef, b, r, z);
            if (b >= 0) blend_ = uint8_t(b);
            if (r != kRefAbsent) aref_ = int16_t(r);
            if (z >= 0) zmsk_ = z != 0;
        }
        lodX_ = false;
        spec_ = 0.f;
        lightOwn_ = false;
        visited = 0;
        culled = 0;
        std::memset(destrHist_, 0, sizeof destrHist_);
        std::memset(byOp, 0, sizeof byOp);
        seen_.clear();
        lod_ = maxLod;
        eyeOk_ = false;
        ghosted = 0;
        {
            const GameCamera gc = readGameCamera(ram, vctx.view);
            cam_ = gc;
            eyeOk_ = gc.ok;
            for (int c = 0; c < 3; ++c) eye_[c] = nativeEye.on ? nativeEye.eye[c] : gc.pos[c];
            eyeOk_ = eyeOk_ || nativeEye.on;
        }
        const uint32_t wc = ram.u32(addr::world);
        if (ram.ok(wc, 0x40)) {
            const uint32_t ep = ram.u32(wc);
            if (ram.ok(ep)) walk(ram, ep, ident(), 0);
        }
        // the view's sky: skyCs/skyCs2/skyClouds from worldCtx[view] (+4/+8/+0xC). hierTraceSky uses the _cs's trans like any object
        // (hierCsUpdate), not the eye: trans only changes through viewSetSkyTrans, which hierSetCamera calls after passing through a
        // hier camera node. In menus that's every frame (trans = eye); in fights the sky stays where the last locator camera left it and the
        // game camera moves around it (parallax of the mountain ring, tex 108; docs/investigacoes/2026-10-17/05_tex108.md).
        {
            std::vector<DrawItem> world;
            world.swap(items);
            for (uint32_t off : {4u, 8u, 0xCu}) drawCs(ram, ram.u32(viewCtxAddr() + off), false);
            sky.swap(items);
            items.swap(world);
        }
        csList(ram, addr::csActiveList);
        hpList_ = true;
        csList(ram, addr::csHPActiveList);
        hpList_ = false;
        fx.wide = wide;
        fx.collect(ram);          // particles with retail color/texture/blending (wotm_fx.hpp); debris (13) stays with collectParticles
        collectShadows(ram);
        collectParticles(ram);
        static const bool ds = std::getenv("WOTM_DESTRSTAT") != nullptr;
        if (ds) { std::fprintf(stderr, "[destr] states:"); for (int i = 0; i < 8; ++i) std::fprintf(stderr, " %u", destrHist_[i]); std::fprintf(stderr, "\n"); }
    }

    // Only the particles (fx[] and debris), without the traversal: live drawing collects the scene at the exit of hier(0, 0) and the particles at
    // the end of the frame (the PS2's particleDraw comes after gameUpdate; wotm_live.inc, wotmCaptureScene/wotmCaptureFrame).
    void collectFx(const Ram &ram) {
        ViewScope vs(vctx);
        sprites.clear();
        fx.wide = wide;
        fx.collect(ram);
        collectParticles(ram);
    }

    // Copies the current scene state (after collect) into a snapshot: items, sky, camera and the skinned objects' matrix palettes
    // (RAM is rewritten every game frame; the snapshot has to keep the values).
    Snapshot takeSnapshot(const Ram &ram, uint32_t g) {
        Snapshot sn;
        sn.items = items;
        sn.sky = sky;
        sn.overlay = overlay;
        sn.hud = hud;
        sn.hp = hp;
        sn.hpOverlay = hpOverlay;
        sn.sprites = sprites;
        sn.fx = fx.systems;
        sn.flares = fx.flares;
        sn.worldLights = worldLights;
        sn.emitters = emitters;
        sn.shadows = shadows;
        sn.lights = lights_;
        sn.fog = fog_;
        sn.cam = readGameCamera(ram, vctx.view);
        sn.vctx = vctx;
        sn.culled = culled;
        readScreenPolys(ram, sn.screenPolys);
        sn.g = g;
        sn.valid = true;
        std::unordered_map<uint32_t, std::shared_ptr<std::vector<M4>>> byBlock;
        std::vector<std::vector<DrawItem> *> lists = {&sn.items, &sn.sky};
        for (ShadowBlocker &b : sn.shadows) lists.push_back(&b.casters);   // the silhouette with the same pose as the drawn monster
        for (std::vector<DrawItem> *list : lists)
            for (DrawItem &it : *list) {
                if (!it.animPkt) continue;
                const uint32_t blk = palette(ram, it);
                if (!blk) continue;
                auto &sp = byBlock[blk];
                if (!sp) {
                    sp = std::make_shared<std::vector<M4>>();
                    sp->reserve(32);
                    for (uint32_t k = 0; k < 32; ++k) sp->push_back(readM4(ram, blk + 0x10 + 0x40 * k));
                }
                it.pal = sp;
                it.palBlk = blk;
            }
        return sn;
    }

    // Draws the snapshot of the game's last frame as it is (no interpolation: logic runs at 60/s) and returns its camera.
    GameCamera show(const Snapshot &s) {
        items = s.items;
        sky = s.sky;
        overlay = s.overlay;
        hud = s.hud;
        hp = s.hp;
        hpOverlay = s.hpOverlay;
        sprites = s.sprites;
        fx.systems = s.fx;
        fxRam_ = s.fxRam;
        if (s.fxCam.ok && s.fxRam && s.fxRamSize) {   // particles built with another camera (chase view)
            uint8_t *dst = fxMixBuffer(s.fxRamSize);
            Particles::copyRanges(dst, Ram{s.fxRam.get(), s.fxRamSize}, fx.systems);
            aimFxRam(dst, s.vctx.view, s.fxCam, s.cam);
            fxRam_ = fxMix_;
        }
        fx.flares = s.flares;
        fx.synth.clear();   // photo mode sprites: rebuilt by it every frame
        worldLights = s.worldLights;
        emitters = s.emitters;
        shadows = s.shadows;
        screenRects = s.screenRects;
        screenPolys = s.screenPolys;
        lights_ = s.lights;
        fog_ = s.fog;
        cam_ = s.cam;
        vctx = s.vctx;
        eyeOk_ = s.cam.ok;
        for (int i = 0; i < 3; ++i) eye_[i] = s.cam.pos[i];
        hudMix_.on = false;
        return s.cam;
    }

    // Particle shadow copy for the displayed frame (fxMix_), sized like the snapshot's
    uint8_t *fxMixBuffer(size_t size) {
        if (!fxMix_ || fxMixSize_ != size) {
            fxMix_.reset(new uint8_t[size], std::default_delete<uint8_t[]>());
            fxMixSize_ = size;
        }
        return fxMix_.get();
    }

    // worldToScreenMat of view `view` in the shadow copy with camera `to` in place of `from` (strite, line and fog read it): the
    // axes of `from` swapped for those of `to`, M' = M C_from^T C_to in columns 0-2 (C = rows right, forward, up; d = p - eye);
    // the eye in worldCtx.eo
    static void aimFxRam(uint8_t *dst, int view, const GameCamera &from, const GameCamera &to) {
        const uint32_t v = uint32_t(std::clamp(view, 0, 4));
        const M4 CF = interp::cameraToWorld(from), CT = interp::cameraToWorld(to);
        float T[3][3];
        for (int j = 0; j < 3; ++j)
            for (int k = 0; k < 3; ++k) T[j][k] = CF.m[0][j] * CT.m[0][k] + CF.m[1][j] * CT.m[1][k] + CF.m[2][j] * CT.m[2][k];
        float M[16], eo[3];
        std::memcpy(M, dst + addr::worldToScreenMat + 0x40 * v, sizeof M);
        for (int r = 0; r < 4; ++r) {
            float row[3];
            for (int k = 0; k < 3; ++k) row[k] = M[4 * r] * T[0][k] + M[4 * r + 1] * T[1][k] + M[4 * r + 2] * T[2][k];
            for (int k = 0; k < 3; ++k) M[4 * r + k] = row[k];
        }
        std::memcpy(dst + addr::worldToScreenMat + 0x40 * v, M, sizeof M);
        for (int k = 0; k < 3; ++k) eo[k] = -to.pos[k];
        std::memcpy(dst + addr::worldCtx + 0x1F0 * v + 0x20, eo, sizeof eo);
    }

    // Frame interpolation (step 1: camera, objects, bone palette, sky and shadows; wotm_live.inc, "interpolacao"): B's show
    // with its matrices blended with A's by alpha (0 = A, 1 = B). Matched by (cs, node, occurrence); a big translation jump
    // or a different LOD = B unblended. HUD, icons and the high-priority list, which the game puts in front of the camera every frame, aren't
    // blended: they go to the blended camera by their position relative to B's camera (blending their matrix jittered against the scene).
    // Particles and debris step back by their velocity (step 2; Particles::stepBack). Lens flare, bars over the enemy and the health icon,
    // which the game projected with B's camera, go to the blended one (step 3; blendFlares, blendAnchors); fade alpha blends with
    // A's when the rectangle is the same. Text stays B's. Camera cut = show(B). Returns the drawn camera; cut says whether there was a cut.
    GameCamera showBlend(const Snapshot &A, const Snapshot &B, float alpha, bool *cut = nullptr) {
        const GameCamera cb = show(B);
        if (cut) *cut = false;
        if (alpha >= 1.f) return cb;
        alpha = std::max(alpha, 0.f);
        if (interp::cameraCut(A.cam, B.cam) || !(A.vctx.view == B.vctx.view && A.vctx.w == B.vctx.w && A.vctx.h == B.vctx.h)) {
            if (cut) *cut = true;
            return cb;
        }
        const GameCamera c = interp::lerpCamera(A.cam, B.cam, alpha);
        std::map<std::pair<const void *, const void *>, std::shared_ptr<std::vector<M4>>> palCache;
        // attached to the camera: M' = M_B . (camera B -> world)^-1 . (blended camera -> world)
        const M4 invA = interp::rigidInverse(interp::cameraToWorld(A.cam)), invB = interp::rigidInverse(interp::cameraToWorld(B.cam));
        const M4 rebase = mul(invB, interp::cameraToWorld(c));
        // Object with the same pose relative to the camera in A and B (the game put it in front of the camera: menu panels, AlignCsWithView objects
        // outside the HUD lists): goes with the blended camera, like the HUD, instead of blending the matrix in the world
        attached_ = 0;
        auto sameRel = [&](const M4 &ma, const M4 &mb) {
            const M4 ra = mul(ma, invA), rb = mul(mb, invB);
            for (int i = 0; i < 4; ++i) {
                const float n = std::fabs(rb.m[i][0]) + std::fabs(rb.m[i][1]) + std::fabs(rb.m[i][2]);
                const float tol = i < 3 ? 1e-3f * (n + 1.f) : 0.02f + 1e-4f * n;
                for (int j = 0; j < 3; ++j)
                    if (!(std::fabs(ra.m[i][j] - rb.m[i][j]) <= tol)) return false;
            }
            return true;
        };
        auto mix = [&](const std::vector<DrawItem> &la, std::vector<DrawItem> &out) {
            // A: chain by (cs, node) in list order; B walks the chain in the same order (occurrence)
            std::unordered_map<uint64_t, int> head;
            head.reserve(la.size() * 2);
            std::vector<int> next(la.size(), -1);
            for (int i = int(la.size()) - 1; i >= 0; --i) {
                const uint64_t k = (uint64_t(la[size_t(i)].cs) << 32) | la[size_t(i)].node;
                auto [it, ins] = head.try_emplace(k, i);
                if (!ins) { next[size_t(i)] = it->second; it->second = i; }
            }
            for (DrawItem &r : out) {
                const auto f = head.find((uint64_t(r.cs) << 32) | r.node);
                if (f == head.end() || f->second < 0) continue;
                const DrawItem &a = la[size_t(f->second)];
                f->second = next[size_t(f->second)];
                if (a.lodIdx != r.lodIdx) continue;
                const float dx = r.m.m[3][0] - a.m.m[3][0], dy = r.m.m[3][1] - a.m.m[3][1], dz = r.m.m[3][2] - a.m.m[3][2];
                if (dx * dx + dy * dy + dz * dz > 150.f * 150.f) continue;   // teleport, reused _cs
                if (std::memcmp(&a.m, &r.m, sizeof(M4)) != 0) {
                    if (sameRel(a.m, r.m)) {
                        r.m = mul(r.m, rebase);
                        ++attached_;
                    } else
                        r.m = interp::lerpM4(a.m, r.m, alpha);
                }
                if (a.pal && r.pal && a.pal != r.pal && a.pal->size() == r.pal->size()) {
                    auto &sp = palCache[{a.pal.get(), r.pal.get()}];
                    if (!sp) {
                        sp = std::make_shared<std::vector<M4>>(*r.pal);
                        for (size_t k = 0; k < sp->size(); ++k)
                            if (std::memcmp(&(*a.pal)[k], &(*r.pal)[k], sizeof(M4)) != 0) (*sp)[k] = interp::lerpM4((*a.pal)[k], (*r.pal)[k], alpha);
                    }
                    r.pal = sp;
                }
                r.fade = a.fade + (r.fade - a.fade) * alpha;
            }
        };
        mix(A.items, items);
        mix(A.sky, sky);
        for (ShadowBlocker &sb : shadows)
            for (const ShadowBlocker &sa : A.shadows)
                if (sa.index == sb.index) {
                    for (int i = 0; i < 3; ++i) sb.origin[i] = sa.origin[i] + (sb.origin[i] - sa.origin[i]) * alpha;
                    mix(sa.casters, sb.casters);
                    break;
                }
        for (std::vector<DrawItem> *l : {&hud, &overlay, &hp, &hpOverlay})
            for (DrawItem &it : *l) it.m = mul(it.m, rebase);
        // Particles: stepped back by velocity (1 - alpha) of B's step (Particles::stepBack); those born during the step stay at B
        const uint32_t gap = B.fields - A.fields;
        static const bool fxOff = std::getenv("WOTM_INTERP_NOFX") != nullptr;   // diagnostics: B's particles, as in step 1
        if (!fxOff && A.fields && gap >= 1 && gap <= 4) {
            const float f = float(gap), back = 1.f - alpha;
            static const bool log = std::getenv("WOTM_INTERPLOG") != nullptr;
            static uint32_t loggedG = 0;
            if (log && B.g != loggedG && A.fxRam && A.fxRamSize && B.fxRam && B.fxRamSize) {   // once per pair
                loggedG = B.g;
                logStepBack(A, B, f);
            }
            for (Sprite &sp : sprites) {   // debris
                const float d[3] = {f * sp.step[0], f * sp.step[1], f * (sp.step[2] - sp.grav * f)};
                if (d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 400.f * 400.f) continue;
                for (int k = 0; k < 3; ++k) sp.pos[k] -= back * d[k];
            }
            if (B.fxRam && B.fxRamSize) {
                uint8_t *dst = fxMixBuffer(B.fxRamSize);
                Particles::blendRam(dst, Ram{B.fxRam.get(), B.fxRamSize}, fx.systems, back, f);
                aimFxRam(dst, B.vctx.view, B.fxCam.ok ? B.fxCam : B.cam, c);
                fxRam_ = fxMix_;
            }
        }
        // 2D attached to 3D: from B's screen to the blended camera's (B's focal from the view's worldToScreenMat; the blended FOV scales it)
        static const bool hudOff = std::getenv("WOTM_INTERP_NOHUD") != nullptr;   // diagnostics: B's flares, bars and fades
        if (!hudOff) {
            hudMix_.on = true;
            hudMix_.view = B.vctx.view;
            hudMix_.alpha = alpha;
            hudMix_.CB = interp::cameraToWorld(B.cam);
            hudMix_.CI = interp::cameraToWorld(c);
            interp::Focus fb;
            float M[16];
            const uint32_t wm = addr::worldToScreenMat + 0x40 * uint32_t(std::clamp(B.vctx.view, 0, 4));
            if (B.fxRam && B.fxRamSize >= wm + sizeof M) {
                std::memcpy(M, B.fxRam.get() + wm, sizeof M);
                if (!interp::focusOf(M, fb)) fb = {};
            } else {   // without the shadow copy (WOTM_FXLIVE): by FOV, in the 224-line view
                fb.fv = 112.f / std::tan(B.cam.fovy * 3.14159265f / 360.f);
                fb.fh = fb.fv * (384.f / 180.48f);
            }
            auto scaled = [&](const GameCamera &o) {   // another camera's focal: B's scaled by the FOV
                float k = std::tan(B.cam.fovy * 3.14159265f / 360.f) / std::tan(o.fovy * 3.14159265f / 360.f);
                if (!(std::isfinite(k) && k > 0.f)) k = 1.f;
                return interp::Focus{fb.fh * k, fb.fv * k};
            };
            hudMix_.CA = interp::cameraToWorld(A.cam);
            hudMix_.fb = fb;
            hudMix_.fa = scaled(A.cam);
            hudMix_.fi = scaled(c);
            static const bool log = std::getenv("WOTM_INTERPLOG") != nullptr;
            static uint32_t loggedG = 0;
            if (log && B.g != loggedG) {   // once per pair
                loggedG = B.g;
                logHudBack(A, B);
            }
            blendFlares(fx.flares);
            blendAnchors(B.anchors, &A.anchors, screenRects, &screenPolys);
            // fades: the k-th rectangle that isn't a bar, same place and color in A and B
            size_t ia = 0;
            for (ScreenRect &r : screenRects) {
                if (r.barView >= 0) continue;
                while (ia < A.screenRects.size() && A.screenRects[ia].barView >= 0) ++ia;
                if (ia >= A.screenRects.size()) break;
                const ScreenRect &a = A.screenRects[ia++];
                if (a.x0 == r.x0 && a.y0 == r.y0 && a.x1 == r.x1 && a.y1 == r.y1 && a.r == r.r && a.g == r.g && a.b == r.b && a.tex0 == r.tex0 &&
                    a.underText == r.underText)
                    r.a = static_cast<uint8_t>(std::lround(a.a + (float(r.a) - a.a) * alpha));
            }
        }
        cam_ = c;
        eyeOk_ = c.ok;
        for (int i = 0; i < 3; ++i) eye_[i] = c.pos[i];
        return c;
    }

    // Step 3 diagnostics (WOTM_INTERPLOG): B's flares and health icons carried to A's camera (alpha 0) against the ones the game
    // projected in A (the nearest, with the same id for flares); "without" = the same distance without reprojecting. In GS px. Icons: only the
    // reprojected ones (those stuck to the screen edge go by their screen position in A and B, no error to measure), and how many went by screen.
    void logHudBack(const Snapshot &A, const Snapshot &B) {
        const BlendView keep = hudMix_;
        hudMix_.CI = hudMix_.CA;
        hudMix_.fi = hudMix_.fa;
        hudMix_.alpha = 0.f;
        Particles::FlareFrame fr = B.flares;
        blendFlares(fr);
        auto nearest = [](float x, float y, const auto &list, auto same) {
            float best = 1e30f;
            for (const auto &o : list)
                if (same(o)) best = std::min(best, std::hypot(o.x - x, o.y - y));
            return best;
        };
        int nf = 0;
        double ef = 0, sf = 0;
        for (const Particles::Flare &fl : fr.e) {
            auto same = [&](const Particles::Flare &o) { return o.id == fl.id; };
            const float e = nearest(fl.x, fl.y, A.flares.e, same);
            const Particles::Flare *orig = nullptr;
            for (const Particles::Flare &o : B.flares.e)
                if (o.id == fl.id) { orig = &o; break; }
            if (e > 60.f || !orig) continue;   // no pair in A
            ++nf;
            ef += e;
            sf += nearest(orig->x, orig->y, A.flares.e, same);
        }
        int na = 0, ns = 0;
        double ea = 0, sa = 0;
        for (const HudAnchor &b : B.anchors) {
            float dx, dy;
            int mode = 0;
            if (b.view != hudMix_.view || !anchorShift(b, &A.anchors, dx, dy, &mode)) continue;
            if (mode == 2) { ++ns; continue; }
            auto same = [&](const HudAnchor &o) { return o.view == b.view; };
            const float e = nearest(b.x + dx, b.y + dy, A.anchors, same);
            if (e > 60.f) continue;
            ++na;
            ea += e;
            sa += nearest(b.x, b.y, A.anchors, same);
        }
        std::fprintf(stderr, "[interp] hud g=%u: %d flares, mean error %.2f px (without reprojecting %.2f); %d health icons, %.2f (without %.2f), %d by screen; "
                     "%d objects attached to the camera\n",
                     B.g, nf, nf ? ef / nf : 0., nf ? sf / nf : 0., na, na ? ea / na : 0., na ? sa / na : 0., ns, attached_);
        hudMix_ = keep;
    }

    // Interpolation (step 3): lens flare from B's screen to that of the last showBlend's blended camera (the one at infinity by direction only;
    // the GS's depth W and Z are redone). Whatever ended up behind the camera or off screen is dropped. With no showBlend in effect, nothing.
    void blendFlares(Particles::FlareFrame &fr) const {
        if (!hudMix_.on) return;
        const interp::Focus &fb = hudMix_.fb, &fi = hudMix_.fi;
        const float x0 = -0.5f * fr.vw - 3.f, x1 = 0.5f * fr.vw + 3.f, y0 = -0.5f * fr.vh - 3.f, y1 = 0.5f * fr.vh + 3.f;
        std::vector<Particles::Flare> out;
        out.reserve(fr.e.size());
        for (Particles::Flare fl : fr.e) {
            const bool inf = (fl.flags & 0x80) != 0;
            const float dx = (fl.x - fr.cx) / fb.fh, dy = (fl.y - fr.cy) / fb.fv;
            float w[3], q[3];
            if (inf) {
                const float qb[3] = {dx, 1.f, -dy};
                interp::fromCam(hudMix_.CB, qb, false, w);
                interp::toCam(hudMix_.CI, w, false, q);
                if (q[1] <= 1e-4f) continue;
            } else {
                if (!(fl.w > 0.f)) { out.push_back(fl); continue; }
                const float qb[3] = {dx * fl.w, fl.w, -dy * fl.w};
                interp::fromCam(hudMix_.CB, qb, true, w);
                interp::toCam(hudMix_.CI, w, true, q);
                if (q[1] < 1.f) continue;
                fl.w = q[1];
                fl.z = fr.zScale + fr.zOff / fl.w;
            }
            const float sx = fi.fh * q[0] / q[1], sy = -fi.fv * q[2] / q[1];
            if (sx < x0 || sx > x1 || sy < y0 || sy > y1) continue;
            fl.x = fr.cx + sx;
            fl.y = fr.cy + sy;
            out.push_back(fl);
        }
        fr.e.swap(out);
    }

    // On-screen offset of one of B's health icons at the last showBlend's blended instant (false = stays where B put it). Matches the
    // nearest A icon in the world (prev; same view, up to 100 units: the monster moves between A and B). If the icon sits at the same
    // distance from its point's projection in A and in B (the game put it over the monster), the blended point goes through the blended camera
    // (mode 1); otherwise (the game pinned it to the screen edge) it blends the screen position from A and B (mode 2).
    bool anchorShift(const HudAnchor &b, const std::vector<HudAnchor> *prev, float &dx, float &dy, int *mode = nullptr) const {
        const HudAnchor *a = nullptr;
        float bd = 100.f * 100.f;
        if (prev)
            for (const HudAnchor &o : *prev) {
                const float d = (o.w[0] - b.w[0]) * (o.w[0] - b.w[0]) + (o.w[1] - b.w[1]) * (o.w[1] - b.w[1]) + (o.w[2] - b.w[2]) * (o.w[2] - b.w[2]);
                if (o.view == b.view && d < bd) { bd = d; a = &o; }
            }
        if (!a) return false;
        const float t = hudMix_.alpha;
        float xb, yb, xa, ya, xi, yi, w[3];
        for (int k = 0; k < 3; ++k) w[k] = a->w[k] + (b.w[k] - a->w[k]) * t;
        if (interp::project(hudMix_.CB, hudMix_.fb, b.w, xb, yb) && interp::project(hudMix_.CA, hudMix_.fa, a->w, xa, ya) &&
            std::fabs((b.x - xb) - (a->x - xa)) < 3.f && std::fabs((b.y - yb) - (a->y - ya)) < 3.f &&
            interp::project(hudMix_.CI, hudMix_.fi, w, xi, yi)) {
            dx = xi - xb;
            dy = yi - yb;
            if (mode) *mode = 1;
        } else {
            dx = (a->x - b.x) * (1.f - t);
            dy = (a->y - b.y) * (1.f - t);
            if (mode) *mode = 2;
        }
        return std::fabs(dx) < 200.f && std::fabs(dy) < 200.f;
    }

    // Interpolation (step 3): bars over the enemy (ScreenRect::anchor) and health icon (polys centered on the noted point) of the
    // last showBlend's view, carried to the blended instant by anchorShift of the world point the game projected (HudAnchor; prev = A's
    // ones).
    void blendAnchors(const std::vector<HudAnchor> &anchors, const std::vector<HudAnchor> *prev, std::vector<ScreenRect> &rects,
                      std::vector<TextQuad> *polys) const {
        if (!hudMix_.on || anchors.empty()) return;
        std::vector<std::array<float, 2>> shift(anchors.size(), {NAN, NAN});
        bool any = false;
        for (size_t i = 0; i < anchors.size(); ++i) {
            float dx, dy;
            if (anchors[i].view == hudMix_.view && anchorShift(anchors[i], prev, dx, dy)) {
                shift[i] = {dx, dy};
                any = true;
            }
        }
        if (!any) return;
        for (ScreenRect &r : rects) {
            if (r.barView != hudMix_.view || r.anchor < 0 || size_t(r.anchor) >= shift.size() || std::isnan(shift[size_t(r.anchor)][0])) continue;
            const auto &sh = shift[size_t(r.anchor)];
            r.x0 += sh[0]; r.x1 += sh[0]; r.y0 += sh[1]; r.y1 += sh[1];
        }
        if (!polys) return;
        for (TextQuad &t : *polys) {
            const float cx = 0.5f * (t.x0 + t.x1), cy = 0.5f * (t.y0 + t.y1);
            for (size_t i = 0; i < anchors.size(); ++i)
                if (!std::isnan(shift[i][0]) && std::fabs(anchors[i].x - cx) < 1.5f && std::fabs(anchors[i].y - cy) < 1.5f) {
                    t.x0 += shift[i][0]; t.x1 += shift[i][0]; t.y0 += shift[i][1]; t.y1 += shift[i][1];
                    break;
                }
        }
    }

    // Step-back diagnostics (WOTM_INTERPLOG): each particle that moved, stepped back one whole step (alpha 0), against the nearest one of the
    // same system in A; "without" = the same distance without stepping back. Debris by the step stored in the Sprite, against those of the same fx in A.
    static void logStepBack(const Snapshot &A, const Snapshot &B, float f) {
        const Ram ra{A.fxRam.get(), A.fxRamSize}, rb{B.fxRam.get(), B.fxRamSize};
        double eR = 0, eS = 0;
        long cnt = 0;
        auto nearest = [](const float p[3], const std::vector<std::array<float, 3>> &pts) {
            float best = 1e30f;
            for (const auto &q : pts) best = std::min(best, (p[0] - q[0]) * (p[0] - q[0]) + (p[1] - q[1]) * (p[1] - q[1]) + (p[2] - q[2]) * (p[2] - q[2]));
            return std::sqrt(best);
        };
        for (const FxSystem &s : B.fx) {
            if (std::none_of(A.fx.begin(), A.fx.end(), [&](const FxSystem &x) { return x.index == s.index; })) continue;
            Particles::StepBack o;
            if (!Particles::stepBack(rb, s.base, s.n, o) || !o.moved) continue;
            const int nA = int32_t(ra.u32(s.base + fxoff::numParticles));
            const uint32_t pa = ra.u32(s.base + fxoff::pos), pb = rb.u32(s.base + fxoff::pos), vb = rb.u32(s.base + fxoff::vel);
            if (nA <= 0 || nA > 1024 || !ra.ok(pa, 16 * uint32_t(nA)) || !rb.ok(pb, 16 * uint32_t(o.moved)) || !rb.ok(vb, 16 * uint32_t(o.moved))) continue;
            std::vector<std::array<float, 3>> pts(static_cast<size_t>(nA));
            for (int j = 0; j < nA; ++j)
                for (int k = 0; k < 3; ++k) pts[size_t(j)][size_t(k)] = ra.f32(pa + 16 * uint32_t(j) + 4 * uint32_t(k));
            for (int i = 0; i < o.moved; ++i) {
                float p[3], v[3], d[3], r[3];
                for (int k = 0; k < 3; ++k) { p[k] = rb.f32(pb + 16 * uint32_t(i) + 4 * uint32_t(k)); v[k] = rb.f32(vb + 16 * uint32_t(i) + 4 * uint32_t(k)); }
                Particles::stepOffset(o, v, f, d);
                for (int k = 0; k < 3; ++k) r[k] = p[k] - d[k];
                eR += nearest(r, pts);
                eS += nearest(p, pts);
                ++cnt;
            }
        }
        double sR = 0, sS = 0;
        long sc = 0;
        for (const Sprite &sp : B.sprites) {
            if (sp.step[0] == 0.f && sp.step[1] == 0.f && sp.step[2] == 0.f) continue;
            std::vector<std::array<float, 3>> pts;
            for (const Sprite &a : A.sprites)
                if ((a.key >> 12) == (sp.key >> 12)) pts.push_back({a.pos[0], a.pos[1], a.pos[2]});
            if (pts.empty()) continue;
            const float r[3] = {sp.pos[0] - f * sp.step[0], sp.pos[1] - f * sp.step[1], sp.pos[2] - f * (sp.step[2] - sp.grav * f)};
            sR += nearest(r, pts);
            sS += nearest(sp.pos, pts);
            ++sc;
        }
        // worldToScreenMat rebuilt with A's camera (the alpha-0 one, as in showBlend) against the one recorded in A: largest relative difference
        float matErr = 0.f;
        {
            const uint32_t v = uint32_t(std::clamp(B.vctx.view, 0, 4)), wa = addr::worldToScreenMat + 0x40 * v;
            const M4 CB = interp::cameraToWorld(B.cam), CA = interp::cameraToWorld(A.cam);
            float T[3][3], scale = 0.f;
            for (int j = 0; j < 3; ++j)
                for (int k = 0; k < 3; ++k) T[j][k] = CB.m[0][j] * CA.m[0][k] + CB.m[1][j] * CA.m[1][k] + CB.m[2][j] * CA.m[2][k];
            for (int r = 0; r < 4; ++r)
                for (int k = 0; k < 3; ++k) {
                    const float m = rb.f32(wa + 16 * uint32_t(r)) * T[0][k] + rb.f32(wa + 16 * uint32_t(r) + 4) * T[1][k] + rb.f32(wa + 16 * uint32_t(r) + 8) * T[2][k];
                    const float a = ra.f32(wa + 16 * uint32_t(r) + 4 * uint32_t(k));
                    matErr = std::max(matErr, std::fabs(m - a));
                    scale = std::max(scale, std::fabs(a));
                }
            matErr = scale > 0.f ? matErr / scale : 0.f;
        }
        if (cnt || sc)
            std::fprintf(stderr, "[interp] stepback g=%u f=%.0f: %ld particles, mean error %.2f (without stepback %.2f); %ld debris, %.2f (without %.2f); matrix %.5f\n", B.g, f, cnt,
                         cnt ? eR / double(cnt) : 0.0, cnt ? eS / double(cnt) : 0.0, sc, sc ? sR / double(sc) : 0.0, sc ? sS / double(sc) : 0.0, matErr);
    }

    // ---- Particles (fx[], 0x5BA280: 98 x 0x2EE0 bytes; VU1 builds the quads, here they become camera-facing sprites) ----
    // Per effect: vis.attribs & 0xFF = type (3 sprite, 4 texsprite, 13 debris, 14 pedestrian, 15 dust...), numParticles +0x2ED0, pos[] (+0x2EB0,
    // 16 bytes: world xyz and w = rotation for debris), age[] (+0x2EB8, int16), info.lifetime (+0x2E4A), min/max size (+0x2E38/+0x2E3C),
    // texLoad[0] (+0x2C40) = index into texInfo and TEX0ptr (+0x2EBC) = TEX0 (psm, palette CBP). Debris (13) only, with the FragTex atlas in
    // 8x8 cells; debris without an atlas becomes a soft circle. The other types (including the composite dust, dustTexture) stay with `fx`.
    void collectParticles(const Ram &ram) {
        static const bool off = std::getenv("WOTM_NOFX") != nullptr;   // diagnostics: no particles
        if (off) return;
        constexpr uint32_t kFx = wotm::sym::fx, kStride = 12000u;
        FxView fog{};   // VU1 discards the whole particle in fog (F < 1; Particles::setFog)
        float row3[4];
        for (int k = 0; k < 4; ++k) row3[k] = ram.f32(viewW2S() + 48 + 4 * k);
        Particles::setFog(ram, fog, row3, eye_);
        for (uint32_t i = 1; i < fxaddr::count && sprites.size() < 6000; ++i) {   // fx[100]: VU1 draws debris in fx[98] and fx[99]
            const uint32_t b = kFx + i * kStride;
            const uint32_t np = ram.u32(b + 0x2ED0);
            const uint32_t type = ram.u32(b + 0x2E90) & 0xFF;
            if (!np || np > 1024 || type != 13 || !Particles::inViewFov(ram, b, wide)) continue;   // debris only; the other types stay with fx (wotm_fx.hpp: retail color, texture and blending)
            const uint32_t pos = ram.u32(b + 0x2EB0), age = ram.u32(b + 0x2EB8);
            if (!ram.ok(pos, 16 * np) || !ram.ok(age, 2 * np)) continue;
            Particles::StepBack sb;   // interpolation (showBlend): each debris piece's step
            const uint32_t velp = ram.u32(b + fxoff::vel);
            const bool haveStep = Particles::stepBack(ram, b, int(np), sb) && ram.ok(velp, 16 * uint32_t(sb.moved));
            const int life = int(int16_t(ram.u16(b + 0x2E30 + 0x1A)));
            const float minS = ram.f32(b + 0x2E30 + 8), maxS = ram.f32(b + 0x2E30 + 0xC);
            const uint32_t texLoad = ram.u16(b + 0x2C40);
            const uint32_t t0p = ram.u32(b + 0x2EBC);
            uint64_t tex0 = 0;
            if (ram.ok(t0p, 8)) tex0 = uint64_t(ram.u32(t0p)) | (uint64_t(ram.u32(t0p + 4)) << 32);
            // fragPacketInit: TextureInfo[+0x2E98] gives the atlas (width and height in 1/16 texel; 8x8 cells = 0x80), one
            // RGBAQ color per system and the context (alphaContext 0: TEST_1 0x5040B, alpha >= 0x40, and ZBUF writing)
            const uint32_t ti = wotm::sym::TextureInfo + 8 * (ram.u32(b + 0x2E98) & 127);
            const uint32_t tw16 = ram.u16(ti + 4), th16 = ram.u16(ti + 6);
            const uint32_t cols = tw16 >> 7, rows = th16 >> 7;
            const bool atlas = type == 13 && texLoad != 0xFFFF && tex0 && cols && rows;
            uint32_t gsCol = 0x80808080u;
            if (ram.u32(b + 0x2E30) & 0x200) {   // color as floats (0..255; alpha 0..1)
                const uint32_t cp = ram.u32(b + 0x2E58);
                if (ram.ok(cp, 16)) {
                    auto ub = [](float v) { return uint32_t(std::clamp(v, 0.f, 255.f)); };
                    gsCol = ub(ram.f32(cp)) | ub(ram.f32(cp + 4)) << 8 | ub(ram.f32(cp + 8)) << 16 | ub(ram.f32(cp + 12) * 128.f) << 24;
                }
            } else {
                gsCol = (ram.u32(fxaddr::colors + 4 * ram.u32(b + 0x2EA0)) & 0x00FFFFFFu) | 0x80000000u;
            }
            const bool opaque = ram.u32(b + 0x2EA8) == 0;
            const float counter = float(int32_t(ram.u32(b + 0x2DC0)));   // fragPacketBuild: +4 per frame, goes to q330.y
            for (uint32_t k = 0; k < np; ++k) {
                Sprite s;
                for (int c = 0; c < 3; ++c) s.pos[c] = ram.f32(pos + 16 * k + 4 * c);
                if (!std::isfinite(s.pos[0]) || !std::isfinite(s.pos[1]) || !std::isfinite(s.pos[2])) continue;
                if (Particles::foggedAt(fog, s.pos)) continue;
                const int a = int(int16_t(ram.u16(age + 2 * k)));
                if (a < 0 || (life > 0 && a >= life)) continue;
                const float t = life > 0 ? float(a) / float(life) : 0.f;
                s.key = i * 4096u + k;
                if (haveStep && int(k) < sb.moved) {
                    for (int c = 0; c < 3; ++c) s.step[c] = ram.f32(velp + 16 * k + 4 * uint32_t(c)) + sb.wind[c];
                    s.grav = sb.grav;
                    if (sb.keepZ) { s.step[2] = 0.f; s.grav = 0.f; }
                }
                if (atlas) {   // debris: screen-aligned GS SPRITE (fragPacketInit/fragPacketBuild, microprogram 0x1180)
                    // pos.w (particleEmit, is13): float in [-2, 2) that particleMove varies with vel.w, with random low bits.
                    //  - side = max(minSize, |w| maxSize / 2) (q330 = (minSize, counter, 128, maxSize / 2)), no rotation;
                    //  - UV entry = bits(w) & (rows * 4 - 1) (w from the tag in q328): 4 per atlas row (normal, mirror V,
                    //    mirror U, both), u0 = 0x08, u1 = 0x88 in 1/16 texel, v = 0x80 * row + same;
                    //  - column: FTOI4(counter * w) & ((columns - 1) * 0x80) added to both U (q329.w): the debris steps through
                    //    the row's cells as the counter advances (4 per frame);
                    //  - PRIM 0x136 (SPRITE, TME, FGE, FST, no ABE): opaque, with the system color; vanishes when particleMove removes it.
                    const float w = ram.f32(pos + 16 * k + 12);
                    const uint32_t e = ram.u32(pos + 16 * k + 12) & (rows * 4 - 1);
                    const int32_t colOff = int32_t(counter * w * 16.f) & int32_t((cols - 1) * 0x80);
                    const uint32_t vRow = 0x80 * (e >> 2);
                    const uint32_t lo = 0x08, hi = 0x88;
                    const bool fu = (e & 3) == 2 || (e & 3) == 3, fv = (e & 3) == 1 || (e & 3) == 3;
                    const float uA = float((fu ? hi : lo) + colOff), uB = float((fu ? lo : hi) + colOff);
                    const float vA = float((fv ? hi : lo) + vRow), vB = float((fv ? lo : hi) + vRow);
                    s.uv[0] = uA / float(tw16); s.uv[1] = vA / float(th16); s.uv[2] = uB / float(tw16); s.uv[3] = vB / float(th16);
                    s.texId = texLoad; s.tex0 = tex0;
                    s.size = std::max(minS, std::fabs(w) * maxS * 0.5f);
                    s.rot = 0.f;
                    s.gsCol = gsCol;
                    s.opaque = opaque;
                    s.aref = opaque ? int16_t(0x40 - 1) : kGsAlphaRef;   // TEST_1 0x5040B: GEQUAL 0x40
                } else {       // dust/smoke: soft circle that grows and fades
                    s.size = minS + (maxS > minS ? (maxS - minS) * t : 0.f);
                    if (s.size < 1.f) s.size = 8.f;
                    s.rgba[0] = 0.78f; s.rgba[1] = 0.72f; s.rgba[2] = 0.65f;
                    s.rgba[3] = 0.55f * (1.f - t);
                }
                sprites.push_back(s);
            }
        }
    }

    unsigned fxTexture(const Ram &ram, uint32_t texId, uint64_t tex0, int &w, int &h) {
        static Texture2D soft{};
        if (!texId || !tex0) {
            if (!soft.id) {   // soft circle (radial alpha)
                std::vector<uint32_t> px(64 * 64);
                for (int y = 0; y < 64; ++y)
                    for (int x = 0; x < 64; ++x) {
                        const float d = std::sqrt(float((x - 32) * (x - 32) + (y - 32) * (y - 32))) / 32.f;
                        const float al = d >= 1.f ? 0.f : (1.f - d) * (1.f - d);
                        px[size_t(y) * 64 + x] = 0x00FFFFFFu | (uint32_t(al * 255.f) << 24);
                    }
                Image img{px.data(), 64, 64, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                soft = LoadTextureFromImage(img);
                SetTextureFilter(soft, TEXTURE_FILTER_BILINEAR);
                SetTextureWrap(soft, TEXTURE_WRAP_CLAMP);
            }
            w = h = 64;
            return soft.id;
        }
        const uint64_t key = (0xF0ull << 56) | (uint64_t(texId) << 32) | uint32_t(gs::Tex0::decode(tex0).cbp);
        auto it = tex_.find(key);
        if (it != tex_.end()) { w = it->second.width; h = it->second.height; return it->second.id; }
        Texture2D t{};
        std::vector<uint32_t> px;
        uint32_t tw = 0, th = 0;
        bool ok = decodeFxTex(ram, texId, tex0, px, tw, th);
        modParticle(ram, texId, tex0, ok, px, tw, th);
        if (ok) {
            Image img{px.data(), int(tw), int(th), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            t = LoadTextureFromImage(img);
            // BILINEAR like the GS (Plantboss's drops came out as texel blocks and looked bigger: photo 2026-10-19_003247/001)
            if (t.id) { SetTextureFilter(t, TEXTURE_FILTER_BILINEAR); SetTextureWrap(t, TEXTURE_WRAP_CLAMP); }
        }
        applyTexFilter(t);
        tex_[key] = t;
        w = t.width; h = t.height;
        return t.id;
    }

    // Sprites sorted from farthest to nearest, without writing depth; one batch per texture.
    // Corners (left-bottom, right-bottom, right-top, left-top) of the debris piece facing the camera (R/U: screen axes), rotated by s.rot.
    // Used by drawSprites and the VU1 check.
    static void spriteCorners(const Sprite &s, const float R[3], const float U[3], float c[4][3]) {
        const float cs = std::cos(s.rot), sn = std::sin(s.rot), hs = s.size * 0.5f;
        const float cx[4] = {-1, 1, 1, -1}, cy[4] = {-1, -1, 1, 1};
        for (int a = 0; a < 3; ++a) {
            const float ax = (R[a] * cs + U[a] * sn) * hs, ay = (-R[a] * sn + U[a] * cs) * hs;   // rotated "right" and "up" axes
            for (int k = 0; k < 4; ++k) c[k][a] = s.pos[a] + ax * cx[k] + ay * cy[k];
        }
    }

    // Camera for ObjSprites: R/U = right/up axes of the raylib camera (the same as drawing), M = worldToScreenMat, focals fx/fy.
    struct SpriteView {
        bool ok = false;
        float R[3], U[3], eye[3], M[4][4], fx = 0.f, fy = 0.f;
        static SpriteView make(const Ram &ram, const float eye[3]) {
            SpriteView v;
            const Matrix mv = rlGetMatrixModelview();
            const float R[3] = {mv.m0, mv.m4, mv.m8}, U[3] = {mv.m1, mv.m5, mv.m9};
            for (int a = 0; a < 3; ++a) { v.R[a] = R[a]; v.U[a] = U[a]; v.eye[a] = eye[a]; }
            for (int r = 0; r < 4; ++r)
                for (int k = 0; k < 4; ++k) v.M[r][k] = ram.f32(viewW2S() + 16 * r + 4 * k);
            v.ok = Particles::screenFoci(v.M, v.fx, v.fy);
            return v;
        }
    };

    // ObjSprite corners in the world like microprogram 0x2E58: center c = m * (x, y, z), W = row 3 of M * (c - eye), corners
    // c -/+ (512, 256) * side / W GS pixels. In the center's view plane (same W), the offset is 512 * side / fx along R and
    // 256 * side / fy along -U (GS y goes down). Order: (-, -), (+, -), (+, +), (-, +) of the GS. False with W < 1 (CLIP discards).
    static bool objSpriteCorners(const SpriteView &v, const M4 &m, const ObjSprite &s, float c[4][3]) {
        if (!v.ok) return false;
        float p[3];
        xform(m, s.x, s.y, s.z, p);
        float w = v.M[3][3];
        for (int a = 0; a < 3; ++a) w += v.M[3][a] * (p[a] - v.eye[a]);
        if (!(w >= 1.f)) return false;
        const float hx = 512.f * s.size / v.fx, hy = 256.f * s.size / v.fy;
        const float sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
        for (int k = 0; k < 4; ++k)
            for (int a = 0; a < 3; ++a) c[k][a] = p[a] + v.R[a] * hx * sx[k] - v.U[a] * hy * sy[k];
        return true;
    }

    void drawSprites(const Ram &ram) {
        if (sprites.empty() || !cam_.ok) return;
        float R[3] = {cam_.fwd[1] * cam_.up[2] - cam_.fwd[2] * cam_.up[1], cam_.fwd[2] * cam_.up[0] - cam_.fwd[0] * cam_.up[2], cam_.fwd[0] * cam_.up[1] - cam_.fwd[1] * cam_.up[0]};
        const float rn = std::sqrt(R[0] * R[0] + R[1] * R[1] + R[2] * R[2]);
        if (rn < 1e-6f) return;
        for (float &v : R) v /= rn;
        const float *U = cam_.up;
        std::vector<std::pair<float, uint32_t>> order;
        order.reserve(sprites.size());
        for (uint32_t i = 0; i < sprites.size(); ++i) {
            const Sprite &s = sprites[i];
            const float dx = s.pos[0] - cam_.pos[0], dy = s.pos[1] - cam_.pos[1], dz = s.pos[2] - cam_.pos[2];
            if (dx * cam_.fwd[0] + dy * cam_.fwd[1] + dz * cam_.fwd[2] < 1.f) continue;   // behind the camera
            order.push_back({-(dx * dx + dy * dy + dz * dz), i});   // farthest first
        }
        std::sort(order.begin(), order.end());
        rlDisableBackfaceCulling();
        rlDisableDepthMask();
        shaderNormal(0.f, true, true);   // fog (FGE) and GS alpha in the draw shader, like the debris XGKICK
        unsigned cur = ~0u;
        bool open = false, curOpaque = false;
        int16_t curRef = kGsAlphaRef;
        gsBlend(kGsAlphaNormal);
        for (const auto &o : order) {
            const Sprite &s = sprites[o.second];
            int tw = 0, th = 0;
            const unsigned id = fxTexture(ram, s.texId, s.tex0, tw, th);
            if (!id) continue;
            if (id != cur || !open || s.opaque != curOpaque || s.aref != curRef) {
                if (open) { rlEnd(); rlDrawRenderBatchActive(); }
                if (s.opaque != curOpaque) {   // opaque debris: no ABE (PRIM 0x136) and writing depth
                    gsBlend(s.opaque ? kGsNoAbe : kGsAlphaNormal);
                    if (s.opaque) rlEnableDepthMask(); else rlDisableDepthMask();
                    curOpaque = s.opaque;
                }
                if (s.aref != curRef) { setAlphaRef(s.aref); curRef = s.aref; }
                rlBegin(RL_TRIANGLES);
                rlSetTexture(id);
                cur = id; open = true;
            }
            const float u0 = s.uv[0], v0 = s.uv[1], u1 = s.uv[2], v1 = s.uv[3];
            // color and alpha in the shader's GS mode: 0x80 = 1.0 (the texture brings raw alpha)
            auto ch = [](float v) { return uint8_t(128.f * std::max(0.f, std::min(1.f, v))); };
            unsigned char cr = ch(s.rgba[0]), cg = ch(s.rgba[1]), cb = ch(s.rgba[2]), ca = ch(s.rgba[3]);
            if (s.gsCol) { cr = uint8_t(s.gsCol); cg = uint8_t(s.gsCol >> 8); cb = uint8_t(s.gsCol >> 16); ca = uint8_t(s.gsCol >> 24); }
            float c[4][3];
            spriteCorners(s, R, U, c);
            const float cu[4] = {u0, u1, u1, u0}, cv[4] = {v1, v1, v0, v0};
            const int tri[6] = {0, 1, 2, 0, 2, 3};
            for (int q = 0; q < 6; ++q) {
                const int k = tri[q];
                rlColor4ub(cr, cg, cb, ca);
                rlTexCoord2f(cu[k], cv[k]);
                rlVertex3f(c[k][0], c[k][1], c[k][2]);
            }
        }
        if (open) { rlEnd(); rlDrawRenderBatchActive(); }
        if (curRef != kGsAlphaRef) setAlphaRef(kGsAlphaRef);
        gsBlend(kGsAlphaNormal);
        rlNormal3f(0.f, 0.f, 0.f);
        rlSetTexture(0);
        rlEnableDepthMask();
        rlEnableBackfaceCulling();
    }

    // Diagnostics: UV range per textured item (grouped by range size).
    void dumpUv(const Ram &ram) {
        int shown = 0;
        for (const DrawItem &it : items) {
            const MeshData *md = mesh(ram, it.node);
            if (!md || md->tris.empty() || !texKey(ram, it.node)) continue;
            float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
            for (const Vtx &v : md->tris) { u0 = std::min(u0, v.u); u1 = std::max(u1, v.u); v0 = std::min(v0, v.v); v1 = std::max(v1, v.v); }
            if ((u1 - u0 > 1.01f || v1 - v0 > 1.01f || u0 < -0.01f || v0 < -0.01f) && shown++ < 12)
                std::fprintf(stderr, "uv node=%06x tex=%u u[%.2f %.2f] v[%.2f %.2f] tris=%zu\n", it.node, ram.u16(it.node + 0x50), u0, u1, v0, v1, md->tris.size() / 3);
        }
        std::fprintf(stderr, "uv outside [0,1]: %d items (showing up to 12)\n", shown);
    }

    // Diagnostics (WOTM_UVISO): ratio |grad u| / |grad v| per texture size. Gave 1.0 even at 128x64: it only shows that 128x64
    // textures are squashed square art. It does NOT prove UVs are in texels: rendering with texel=uv/16 produces noise; normalized UV (4096 = 1.0) is right.
    void dumpUvIso(const Ram &ram) {
        std::map<std::pair<uint32_t, uint32_t>, std::vector<float>> byDim;
        for (const DrawItem &it : items) {
            const MeshData *md = mesh(ram, it.node);
            if (!md || md->skinned || md->tris.empty() || !texKey(ram, it.node)) continue;
            const uint32_t ptr = ram.u32(addr::texInfo + 16 * ram.u16(it.node + 0x50)) & 0x0FFFFFFF;
            const UploadHdr hd = UploadHdr::read(ram, ptr);
            const uint32_t w = hd.w, h = hd.h;
            if (!w || !h) continue;
            for (size_t i = 0; i + 2 < md->tris.size(); i += 3) {
                const Vtx &a = md->tris[i], &b = md->tris[i + 1], &c = md->tris[i + 2];
                const double e1[3] = {b.x - a.x, b.y - a.y, b.z - a.z}, e2[3] = {c.x - a.x, c.y - a.y, c.z - a.z};
                const double g11 = e1[0]*e1[0] + e1[1]*e1[1] + e1[2]*e1[2], g22 = e2[0]*e2[0] + e2[1]*e2[1] + e2[2]*e2[2];
                const double g12 = e1[0]*e2[0] + e1[1]*e2[1] + e1[2]*e2[2];
                const double det = g11 * g22 - g12 * g12;
                if (det < 1e-3 * g11 * g22 || g11 < 1e-4) continue;   // degenerate/too thin
                // |grad f|^2 = [d1 d2] G^-1 [d1 d2]^T, with d = differences of f along e1/e2
                auto gmag = [&](double d1, double d2) { return std::sqrt((g22 * d1 * d1 - 2 * g12 * d1 * d2 + g11 * d2 * d2) / det); };
                const double gu = gmag(b.u - a.u, c.u - a.u), gv = gmag(b.v - a.v, c.v - a.v);
                if (gu < 1e-7 || gv < 1e-7) continue;
                byDim[{w, h}].push_back(float(gu / gv));
            }
            {   // TEX0's TW/TH (2^n) vs descriptor, once per texture
                static std::set<uint32_t> seenTex;
                const uint32_t tid = ram.u16(it.node + 0x50);
                if (seenTex.insert(tid).second) {
                    const gs::Tex0 t0 = gs::Tex0::decode(objectTex0(ram, it.node));
                    float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
                    for (const Vtx &v : md->tris) { u0 = std::min(u0, v.u); u1 = std::max(u1, v.u); v0 = std::min(v0, v.v); v1 = std::max(v1, v.v); }
                    std::fprintf(stderr, "uviso-tex id=%u desc=%ux%u tex0 TW=%u TH=%u (=%ux%u) tfx=%u uv[%.2f %.2f][%.2f %.2f]\n", tid, w, h, t0.tw, t0.th, 1u << t0.tw, 1u << t0.th, t0.tfx, u0, u1, v0, v1);
                }
            }
        }
        for (auto &kv : byDim) {
            auto &v = kv.second;
            if (v.size() < 20) continue;
            std::sort(v.begin(), v.end());
            std::fprintf(stderr, "uviso tex %ux%u: tris=%zu ratio |gu|/|gv| p25=%.3f med=%.3f p75=%.3f (h/w=%.3f)\n", kv.first.first, kv.first.second, v.size(),
                         v[v.size() / 4], v[v.size() / 2], v[v.size() * 3 / 4], double(kv.first.second) / kv.first.first);
        }
    }

    // Diagnostics (WOTM_SKINFIT="nodeA,nodeB,pal" in hex): compares skinning variants of node B against node A (another LOD of the same monster,
    // taken as reference) by the mean Chamfer distance between the skinned vertex clouds.
    void fitSkin(const Ram &ram, uint32_t nodeA, uint32_t nodeB, uint32_t pal) {
        const MeshData *ma = mesh(ram, nodeA), *mb = mesh(ram, nodeB);
        if (!ma || !mb || ma->tris.empty() || mb->tris.empty()) { std::fprintf(stderr, "skinfit: empty mesh\n"); return; }
        auto cloud = [&](const MeshData *md, int variant) {
            std::vector<std::array<float, 3>> out;
            for (const Vtx &v : md->tris) {
                const int mask = (variant == 1) ? 0xFF : 0x7F;
                const float w = std::min(1.f, std::max(0.f, v.w));
                auto slotOf = [&](uint8_t c) { return int((c & mask) / 4); };
                float a[3], b[3];
                const int sa = slotOf(v.cx), sb = slotOf(v.cy);
                if (sa > 30 || sb > 30) { out.push_back({1e6f, 1e6f, 1e6f}); continue; }
                xform(readM4(ram, pal + 0x10 + 0x40 * sa), v.x, v.y, v.z, a);
                xform(readM4(ram, pal + 0x10 + 0x40 * sb), v.x, v.y, v.z, b);
                float ww = w;
                if (variant == 2) ww = 1.f - w;          // weights swapped
                if (variant == 3) ww = 1.f;              // only bone x
                if (variant == 4) { ww = 0.f; }          // only bone y
                out.push_back({a[0] * ww + b[0] * (1 - ww), a[1] * ww + b[1] * (1 - ww), a[2] * ww + b[2] * (1 - ww)});
            }
            return out;
        };
        auto chamfer = [](const std::vector<std::array<float, 3>> &x, const std::vector<std::array<float, 3>> &y) {
            std::vector<double> d;
            const size_t step = std::max<size_t>(1, x.size() / 1500);
            for (size_t i = 0; i < x.size(); i += step) {
                float best = 1e30f;
                for (const auto &q : y) {
                    const float dx = x[i][0] - q[0], dy = x[i][1] - q[1], dz = x[i][2] - q[2];
                    best = std::min(best, dx * dx + dy * dy + dz * dz);
                }
                d.push_back(std::sqrt(double(best)));
            }
            std::sort(d.begin(), d.end());
            auto pc = [&](double q) { return d.empty() ? 0.0 : d[std::min(d.size() - 1, size_t(q * double(d.size())))]; };
            struct R { double med, p90, p99, mx; };
            return R{pc(0.5), pc(0.9), pc(0.99), d.empty() ? 0.0 : d.back()};
        };
        const auto ref = cloud(ma, 0);
        const char *names[5] = {"current (mask 0x7F, w on bone x)", "mask 0xFF", "weights swapped", "bone x only", "bone y only"};
        for (int v = 0; v < 5; ++v) {
            const auto c = cloud(mb, v);
            { const auto r1 = chamfer(c, ref), r2 = chamfer(ref, c); std::fprintf(stderr, "skinfit v%d (%s): B->A med %.1f p90 %.1f p99 %.1f max %.1f | A->B med %.1f p90 %.1f p99 %.1f max %.1f\n", v, names[v], r1.med, r1.p90, r1.p99, r1.mx, r2.med, r2.p90, r2.p99, r2.mx); }
        }
    }

    // Diagnostics (WOTM_BONECODES="node"): histogram of the bone codes (bit 0x80 and slot) of a skinned node.
    void boneCodes(const Ram &ram, uint32_t node) {
        const MeshData *md = mesh(ram, node);
        if (!md) return;
        size_t hi[3] = {0, 0, 0}, tot = 0, wlt1 = 0;
        std::map<int, size_t> slots[3];
        for (const Vtx &v : md->tris) {
            const uint8_t c[3] = {v.cx, v.cy, v.cz};
            for (int i = 0; i < 3; ++i) { if (c[i] & 0x80) ++hi[i]; ++slots[i][(c[i] & 0x7F) / 4]; }
            ++tot;
            if (v.w < 0.999f) ++wlt1;
        }
        std::fprintf(stderr, "bonecodes node=%06x vertices=%zu | with bit 0x80: x=%zu y=%zu z=%zu | w<1: %zu | slots used in x: %zu, y: %zu\n", node, tot, hi[0], hi[1], hi[2], wlt1, slots[0].size(), slots[1].size());
        std::string row = "   slots x: ";
        for (auto &kv : slots[0]) row += std::to_string(kv.first) + "(" + std::to_string(kv.second) + ") ";
        std::fprintf(stderr, "%s\n", row.c_str());
    }

    // Diagnostics (WOTM_COVER="ref1,ref2,..:test1,test2"): coverage in the reference pose (raw positions, no skinning).
    // Samples points on the surface of the reference nodes and measures the fraction within `tol` of some triangle of the test nodes.
    void coverage(const Ram &ram, const std::vector<uint32_t> &refs, const std::vector<uint32_t> &tests, float tol) {
        auto gather = [&](const std::vector<uint32_t> &ns) {
            std::vector<std::array<float, 9>> t;
            for (uint32_t n : ns) {
                const MeshData *md = mesh(ram, n);
                if (!md) continue;
                static const uint32_t coverPal = std::getenv("WOTM_COVERPAL") ? uint32_t(std::strtoul(std::getenv("WOTM_COVERPAL"), nullptr, 16)) : 0;   // with a palette: skinned
                for (size_t k = 0; k + 2 < md->tris.size(); k += 3) {
                    if (coverPal) {
                        float q[3][3];
                        const M4 id = ident();
                        for (int c = 0; c < 3; ++c) skinVertex(ram, coverPal, id, md->tris[k + c], q[c]);
                        t.push_back({q[0][0], q[0][1], q[0][2], q[1][0], q[1][1], q[1][2], q[2][0], q[2][1], q[2][2]});
                    } else {
                        const Vtx &a = md->tris[k], &b = md->tris[k + 1], &c = md->tris[k + 2];
                        t.push_back({a.x, a.y, a.z, b.x, b.y, b.z, c.x, c.y, c.z});
                    }
                }
            }
            return t;
        };
        const auto R = gather(refs), T = gather(tests);
        // point-triangle distance (Ericson)
        auto dist2 = [](const float *p, const std::array<float, 9> &tr) {
            auto sub = [](const float *a, const float *b, float *o) { for (int i = 0; i < 3; ++i) o[i] = a[i] - b[i]; };
            auto dot = [](const float *a, const float *b) { return a[0] * b[0] + a[1] * b[1] + a[2] * b[2]; };
            const float *a = &tr[0], *b = &tr[3], *c = &tr[6];
            float ab[3], ac[3], ap[3]; sub(b, a, ab); sub(c, a, ac); sub(p, a, ap);
            const float d1 = dot(ab, ap), d2 = dot(ac, ap);
            auto pt = [&](float u, float v, float w) { float q[3]; for (int i = 0; i < 3; ++i) q[i] = u * a[i] + v * b[i] + w * c[i]; float d[3]; sub(p, q, d); return dot(d, d); };
            if (d1 <= 0 && d2 <= 0) return pt(1, 0, 0);
            float bp[3]; sub(p, b, bp); const float d3 = dot(ab, bp), d4 = dot(ac, bp);
            if (d3 >= 0 && d4 <= d3) return pt(0, 1, 0);
            const float vc = d1 * d4 - d3 * d2;
            if (vc <= 0 && d1 >= 0 && d3 <= 0) { const float v = d1 / (d1 - d3); return pt(1 - v, v, 0); }
            float cp[3]; sub(p, c, cp); const float d5 = dot(ab, cp), d6 = dot(ac, cp);
            if (d6 >= 0 && d5 <= d6) return pt(0, 0, 1);
            const float vb = d5 * d2 - d1 * d6;
            if (vb <= 0 && d2 >= 0 && d6 <= 0) { const float w = d2 / (d2 - d6); return pt(1 - w, 0, w); }
            const float va = d3 * d6 - d5 * d4;
            if (va <= 0 && (d4 - d3) >= 0 && (d5 - d6) >= 0) { const float w = (d4 - d3) / ((d4 - d3) + (d5 - d6)); return pt(0, 1 - w, w); }
            const float den = 1.f / (va + vb + vc), v = vb * den, w = vc * den;
            return pt(1 - v - w, v, w);
        };
        size_t n = 0, ok = 0;
        uint32_t rng = 12345;
        auto rnd = [&]() { rng = rng * 1664525u + 1013904223u; return float(rng >> 8) / 16777216.f; };
        for (size_t s = 0; s < 4000 && !R.empty(); ++s) {
            const auto &tr = R[size_t(rnd() * float(R.size())) % R.size()];
            float u = rnd(), v = rnd(); if (u + v > 1) { u = 1 - u; v = 1 - v; }
            float p[3]; for (int i = 0; i < 3; ++i) p[i] = tr[i] * (1 - u - v) + tr[3 + i] * u + tr[6 + i] * v;
            float best = 1e30f;
            for (const auto &tt : T) best = std::min(best, dist2(p, tt));
            ++n; if (best < tol * tol) ++ok;
        }
        std::fprintf(stderr, "cover: %zu test triangles cover %.1f%% of the reference surface (tol %.1f; %zu samples)%c", T.size(), n ? 100.0 * double(ok) / double(n) : 0.0, tol, n, 10);
    }

    // Diagnostics (WOTM_SKINSAN=1): skinning sanity. Per triangle, ratio between the skinned perimeter and the reference pose's;
    // vertices in triangles with an absurd ratio are "exploded": groups them by bone slot and weight to find the pattern.
    void dumpSkinSanity(const Ram &ram) {
        for (const DrawItem &it : items) {
            const MeshData *md = mesh(ram, it.node);
            if (!md || !md->skinned || md->tris.size() < 60) continue;
            const uint32_t pal = palette(ram, it);
            if (!pal) { std::fprintf(stderr, "skinsan node=%06x no palette\n", it.node); continue; }
            const M4 id = ident();
            size_t bad = 0, total = 0;
            std::map<std::string, size_t> why;
            for (size_t k = 0; k + 2 < md->tris.size(); k += 3) {
                float p[3][3];
                double o = 0, sk = 0;
                for (int c = 0; c < 3; ++c) skinVertex(ram, pal, id, md->tris[k + c], p[c]);
                for (int c = 0; c < 3; ++c) {
                    const Vtx &a = md->tris[k + c], &b = md->tris[k + (c + 1) % 3];
                    o += std::sqrt(double((a.x - b.x) * (a.x - b.x) + (a.y - b.y) * (a.y - b.y) + (a.z - b.z) * (a.z - b.z)));
                    const float *pa = p[c], *pb = p[(c + 1) % 3];
                    sk += std::sqrt(double((pa[0] - pb[0]) * (pa[0] - pb[0]) + (pa[1] - pb[1]) * (pa[1] - pb[1]) + (pa[2] - pb[2]) * (pa[2] - pb[2])));
                }
                ++total;
                if (o < 1e-4) continue;
                const double r = sk / o;
                if (r > 2.5 || r < 0.4) {
                    ++bad;
                    for (int c = 0; c < 3; ++c) {
                        const Vtx &v = md->tris[k + c];
                        char key[96];
                        std::snprintf(key, sizeof key, "cx=%02x(slot %d) cy=%02x(slot %d) cz=%02x w=%.2f", v.cx, (v.cx & 0x7F) / 4, v.cy, (v.cy & 0x7F) / 4, v.cz, v.w);
                        ++why[key];
                    }
                }
            }
            {   // fraction of triangles with an edge longer than 3x the median (spurious bridges between strips)
                std::vector<float> longest;
                for (size_t k = 0; k + 2 < md->tris.size(); k += 3) {
                    float q[3][3];
                    for (int c = 0; c < 3; ++c) skinVertex(ram, pal, id, md->tris[k + c], q[c]);
                    float mx = 0;
                    for (int c = 0; c < 3; ++c) {
                        const float *a2 = q[c], *b2 = q[(c + 1) % 3];
                        mx = std::max(mx, std::sqrt((a2[0] - b2[0]) * (a2[0] - b2[0]) + (a2[1] - b2[1]) * (a2[1] - b2[1]) + (a2[2] - b2[2]) * (a2[2] - b2[2])));
                    }
                    longest.push_back(mx);
                }
                std::vector<float> sorted = longest;
                std::sort(sorted.begin(), sorted.end());
                const float med = sorted.empty() ? 0.f : sorted[sorted.size() / 2];
                size_t nl = 0;
                for (float l : longest) if (l > 3.f * med) ++nl;
                std::fprintf(stderr, "skinsan-longedge node=%06x median %.2f, triangles > 3x: %zu of %zu (%.1f%%)\n", it.node, med, nl, longest.size(), longest.empty() ? 0.0 : 100.0 * nl / longest.size());
            }
            std::fprintf(stderr, "skinsan node=%06x tris=%zu bad=%zu (%.1f%%)\n", it.node, total, bad, total ? 100.0 * bad / total : 0.0);
            std::vector<std::pair<size_t, std::string>> v;
            for (auto &kv : why) v.push_back({kv.second, kv.first});
            std::sort(v.rbegin(), v.rend());
            for (size_t i = 0; i < v.size() && i < 8; ++i) std::fprintf(stderr, "   %5zu x %s\n", v[i].first, v[i].second.c_str());
        }
    }

    // Diagnostics: lists the skinned items (node, triangles, world position, palette found?).
    void dumpSkinned(const Ram &ram) {
        if (std::getenv("WOTM_LISTRIGID")) {   // diagnostics: rigid parts of the characters (non-skinned items with animPkt): node, texture, radius, world position, blend
            for (const DrawItem &it : items) {
                const MeshData *md = mesh(ram, it.node);
                if (!it.animPkt || (md && md->skinned)) continue;
                std::fprintf(stderr, "[rigid] pkt %x node %x tex %u tris %zu radius %.1f pos %.1f %.1f %.1f blend %d f30 %08x %08x\n", it.animPkt, it.node, unsigned(ram.u16(it.node + 0x50)),
                             md ? md->tris.size() / 3 : size_t(0), ram.f32(it.node + 8), it.m.m[3][0], it.m.m[3][1], it.m.m[3][2], int(it.blend), ram.u32(it.node + 0x30), ram.u32(it.node + 0x34));
            }
        }
        for (const DrawItem &it : items) {
            const MeshData *md = mesh(ram, it.node);
            if (!md || !md->skinned) continue;
            std::fprintf(stderr, "   m: [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f] [%.3f %.3f %.3f %.3f]%c", it.m.m[0][0], it.m.m[0][1], it.m.m[0][2], it.m.m[0][3], it.m.m[1][0], it.m.m[1][1], it.m.m[1][2], it.m.m[1][3], it.m.m[2][0], it.m.m[2][1], it.m.m[2][2], it.m.m[2][3], it.m.m[3][0], it.m.m[3][1], it.m.m[3][2], it.m.m[3][3], 10);
            std::fprintf(stderr, "skinned node=%06x tris=%zu pos=(%.0f %.0f %.0f) animPkt=%06x lod=%d pal=%06x\n", it.node, md->tris.size() / 3,
                         it.m.m[3][0], it.m.m[3][1], it.m.m[3][2], it.animPkt, it.lodIdx, palette(ram, it));
            {
                float u0 = 1e9f, u1 = -1e9f, v0 = 1e9f, v1 = -1e9f;
                for (const Vtx &v : md->tris) { u0 = std::min(u0, v.u); u1 = std::max(u1, v.u); v0 = std::min(v0, v.v); v1 = std::max(v1, v.v); }
                std::fprintf(stderr, "   uv u[%.2f %.2f] v[%.2f %.2f] abe=%d\n", u0, u1, v0, v1, int(md->abe));
                std::fprintf(stderr, "   lowAlpha=%d abe=%d (lowAlpha sends the object to the translucent pass, without depth)\n", int(md->lowAlpha), int(md->abe));
                {   // histogram of vertex alpha (effective a*4) and mean color: tells an opaque layer from a glow/additive layer
                    unsigned h[8] = {0}; double sr = 0, sg = 0, sb = 0;
                    for (const Vtx &v : md->tris) { ++h[std::min(7, v.a * 4 / 32)]; sr += v.r; sg += v.g; sb += v.b; }
                    const double n = md->tris.empty() ? 1.0 : double(md->tris.size());
                    std::fprintf(stderr, "   vtx alpha (a*4 in eighths of 256): %u %u %u %u %u %u %u %u | mean color rgb %.0f %.0f %.0f\n", h[0], h[1], h[2], h[3], h[4], h[5], h[6], h[7], sr / n, sg / n, sb / n);
                }
                std::fprintf(stderr, "   indices: %u total, %u outside pos; discarded: %u by ADC, %u degenerate; emitted %zu; positions %u, used %u\n", md->totIdx, md->oobIdx, md->dropAdc, md->dropDegen, md->tris.size() / 3, md->posTot, md->posUsed);
            }
            const uint64_t t0v = objectTex0(ram, it.node);
            const gs::Tex0 t0 = gs::Tex0::decode(t0v);
            const uint32_t ptr = ram.u32(addr::texInfo + 16 * ram.u16(it.node + 0x50)) & 0x0FFFFFFF;
            std::fprintf(stderr, "   texId=%u tex0=%016llx cbp=%u csa=%u base=%u psm=%u ptr=%06x hasPal=%d\n", ram.u16(it.node + 0x50),
                         (unsigned long long)t0v, t0.cbp, t0.csa, ram.u32(addr::tempVramTexAddr) >> 6, UploadHdr::read(ram, ptr).psm, ptr,
                         int(rtxPal.count(t0.cbp)));
        }
    }

    const MeshData *mesh(const Ram &ram, uint32_t node) {
        const uint32_t pp = ram.u32(node + 4) & 0x0FFFFFFF;
        auto it = cache_.find(pp);
        if (it != cache_.end()) {
            // revalidate the signature (the level's RAM may have been reused)
            if (it->second->sig == packetSig(ram, skipZeroQwords(ram, pp))) { lastMesh_ = it->second; return it->second.get(); }
        }
        auto md = std::make_shared<MeshData>(decodeObject(ram, node));
        cache_[pp] = md;
        lastMesh_ = md;
        return md.get();
    }
    // Swaps the object's mesh in the cache for a copy altered by f (once per decode). A new copy, not the same one altered: the
    // resident arena keeps vertices by mesh pointer. Used by wotm_levelsel.inc (UV of the photo for stages without art in SHELL).
    template <class F> bool patchMesh(const Ram &ram, uint32_t node, F &&f) {
        if (!mesh(ram, node)) return false;
        auto it = cache_.find(ram.u32(node + 4) & 0x0FFFFFFF);
        if (it == cache_.end()) return false;
        if (it->second->patched) return true;
        auto md = std::make_shared<MeshData>(*it->second);
        f(*md);
        md->patched = true;
        it->second = md;
        lastMesh_ = md;
        return true;
    }
    std::shared_ptr<const MeshData> lastMesh_;   // the mesh from the last mesh() call (drawList: the deferred chunk holds it)

    // Alpha test threshold in the draw shader (DrawItem::aref, GS alpha units: 0x80 = 1.0). Flushes the batch first: the
    // uniform applies to whatever is drawn afterward.
    void setAlphaRef(int16_t r) {
        if (alphaRefLoc_ < 0) return;
        rlDrawRenderBatchActive();
        const float f = float(r);
        SetShaderValue(alphaShader_, alphaRefLoc_, &f, SHADER_UNIFORM_FLOAT);
    }

    // Draws with raylib (must be inside BeginMode3D).
    // Turns on the GS shader (fog, alpha) for an object pass; returns whether it did (the caller does EndShaderMode and clears
    // g_premulShader at the end).
    bool beginShader(const Ram &ram, bool wire) {
        // Alpha test: transparent pixels (shadows, propellers, grates) must not write depth or color.
        // The vertex normal carries (fogBurnThru, 1, FGE + 2 * GS) (shaderNormal; rlNormal3f normalizes, so the shader reads the ratios
        // over y); the drawer fills it in and clears it at the end (normal 0 = no fog, plain alpha).
        //  - FGE: fog (see Fog).
        //  - GS: texture and vertex carry raw GS color and alpha (0x80 = 1.0) and the shader modulates like the GS (TFX MODULATE):
        //    color = texel * Cv / 128 (Cv > 0x80 brightens) and As = min(At * Av / 128, 255) / 128 (up to 1.99: the texture function saturates
        //    at 255). In regular blending, As above 1 comes out in three passes (modes 3 to 5, gsBlendCommon);
        //    without GS mode, the plain product (shadows, debugging).
        //  - PM: additive/subtractive blending (gsBlend 1 and 2); the output is color times the whole As, and blending uses factor 1.
        //  - mode (+8, +16): the two passes of GsBlendFactors::doubled (alpha As / 2; white output).
        if (!alphaShaderTried_) {
            alphaShaderTried_ = true;
            static const char *vs =
                "#version 330\n"
                "in vec3 vertexPosition; in vec2 vertexTexCoord; in vec3 vertexNormal; in vec4 vertexColor;\n"
                "uniform mat4 mvp; uniform vec3 fogEye; uniform vec4 fogRow3; uniform vec3 fogParams;\n"
                "out vec2 fragTexCoord; out vec4 fragColor; out float fragFog; out float fragGsA; out float fragPm; out float fragMode; out float fragFge;\n"
                "void main() {\n"
                "  fragTexCoord = vertexTexCoord; fragColor = vertexColor;\n"
                // normal = (burn, 1, FGE + 2 * GS + 4 * PM) normalized by rlNormal3f: the ratios over y survive; y = 0 = nothing
                "  float burn = 0.0, fl = 0.0;\n"
                "  if (vertexNormal.y > 1e-6) { burn = vertexNormal.x / vertexNormal.y; fl = floor(vertexNormal.z / vertexNormal.y + 0.5); }\n"
                "  fragGsA = mod(floor(fl / 2.0), 2.0) > 0.5 ? 1.0 : 0.0;\n"
                "  fragPm = mod(floor(fl / 4.0), 2.0) > 0.5 ? 1.0 : 0.0;\n"
                "  fragMode = floor(fl / 8.0);\n"
                "  fragFge = mod(fl, 2.0) > 0.5 ? 1.0 : 0.0;\n"
                "  float w = dot(fogRow3.xyz, vertexPosition - fogEye) + fogRow3.w;\n"
                "  float f = 255.0;\n"
                "  if (mod(fl, 2.0) > 0.5 && w > 0.001) f = clamp(burn + fogParams.x + fogParams.z / w, 0.0, fogParams.y);\n"
                "  fragFog = f / 255.0;\n"
                "  gl_Position = mvp * vec4(vertexPosition, 1.0);\n"
                "}\n";
            static const char *fs =
                "#version 330\n"
                "in vec2 fragTexCoord; in vec4 fragColor; in float fragFog; in float fragGsA; in float fragPm; in float fragMode; in float fragFge;\n"
                "uniform sampler2D texture0; uniform vec4 colDiffuse; uniform vec3 fogColor; uniform float fogSplit; uniform float alphaRef; uniform float gsTrunc; out vec4 finalColor;\n"
                "void main() { vec4 t = texture(texture0, fragTexCoord) * fragColor * colDiffuse;\n"
                // GS: color = texel * vertex color / 128 (the vertex comes raw, 0x80 = 1.0; above that it brightens), capped at 255;
                // As = At * Av / 128, saturated at 255 by the texture function (like the color), / 128: up to 255/128
                "  float as = t.a;\n"
                // default (WOTM_GSTRUNC=0 turns it off): the GS's integer steps (integer texel, truncated DDA color, (Ct * Cv) >> 7 and (At * Av) >> 7 capped at
                // 255); in float native comes out ~2 levels brighter (lead in VU1CHECK.md)
                "  if (fragGsA > 0.5 && gsTrunc > 0.5) {\n"
                "    vec4 tx = floor(texture(texture0, fragTexCoord) * 255.0 + 0.5);\n"
                "    vec4 cv = floor(fragColor * colDiffuse * 255.0 + 0.001);\n"
                "    t.rgb = min(floor(tx.rgb * cv.rgb / 128.0), 255.0) / 255.0;\n"
                "    as = min(floor(tx.a * cv.a / 128.0), 255.0) / 128.0; t.a = min(as, 1.0);\n"
                "  } else if (fragGsA > 0.5) { t.rgb = min(t.rgb * 1.9922, vec3(1.0)); as = min(t.a * 3.9690, 1.9922); t.a = min(as, 1.0); }\n"
                // GS alpha test (the GROUP's TEST_1, DrawItem::aref; default kGsAlphaRef = alpha > 8)
                "  if (as * 128.0 <= alphaRef) discard;\n"
                "  if (fragMode > 1.5 && fragMode < 2.5) { finalColor = vec4(1.0); return; }\n"   // 2nd pass of doubled (gsBlendDouble)
                // regular blending with As > 1 (gsBlendCommon): 3rd pass, destination * As (otherwise no effect); 2nd pass only where As > 1
                "  if (fragMode > 4.5) { finalColor = as > 1.0 ? vec4(vec3(as - 1.0), 1.0) : vec4(0.0); return; }\n"
                "  if (fragMode > 3.5 && as <= 1.0) discard;\n"

                "  if (fogSplit < -1.5) { finalColor = vec4(vec3(1.0 - fragFog), 1.0); return; }\n"   // WOTM_FOGSPLIT=2: fog mask
                "  float f = gl_FragCoord.x < fogSplit ? 1.0 : fragFog;\n"
                "  vec3 c = mix(fogColor, t.rgb, f);\n"
                // GS: truncated integer F; (F * C) >> 8 + ((255 - F) * Cf) >> 8 (with F = 255 it gives C - 1); only with FGE
                "  if (fragGsA > 0.5 && gsTrunc > 0.5 && fragFge > 0.5 && gl_FragCoord.x >= fogSplit) {\n"
                "    float F = floor(f * 255.0 + 0.001);\n"
                "    c = (floor(F * floor(t.rgb * 255.0 + 0.5) / 256.0) + floor((255.0 - F) * floor(fogColor * 255.0 + 0.5) / 256.0)) / 255.0;\n"
                "  }\n"
                // additive/subtractive: color already times the whole As (blending uses factor 1; gsBlend)
                // doubled (mode 1): alpha As / 2, without the cap at 1 (the 2nd pass doubles the destination)
                // As > 1 in regular blending: 1st pass has no effect (alpha 0); 2nd pass Cs - Cd (1 - 1/As)
                "  finalColor = fragPm > 0.5 ? vec4(c * as, 1.0) : fragMode > 3.5 ? vec4(c, 1.0 - 1.0 / as)\n"
                "             : fragMode > 2.5 ? vec4(c, as > 1.0 ? 0.0 : as) : vec4(c, fragMode > 0.5 ? as * 0.5 : t.a); }\n";
            alphaShader_ = LoadShaderFromMemory(vs, fs);
            fogLoc_[0] = GetShaderLocation(alphaShader_, "fogEye");
            fogLoc_[1] = GetShaderLocation(alphaShader_, "fogRow3");
            fogLoc_[2] = GetShaderLocation(alphaShader_, "fogParams");
            fogLoc_[3] = GetShaderLocation(alphaShader_, "fogColor");
            fogLoc_[4] = GetShaderLocation(alphaShader_, "fogSplit");
            alphaRefLoc_ = GetShaderLocation(alphaShader_, "alphaRef");
            setAlphaRef(kGsAlphaRef);
            // the GS's integer math in the shader (modulation, Gouraud, fog), on by default: the VEGAS floor in software PCSX2 comes out
            // 2 levels below float; WOTM_GSTRUNC=0 goes back to float
            const int gsTruncLoc = GetShaderLocation(alphaShader_, "gsTrunc");
            const float gsTrunc = (std::getenv("WOTM_GSTRUNC") && std::atoi(std::getenv("WOTM_GSTRUNC")) == 0) ? 0.0f : 1.0f;
            if (gsTruncLoc >= 0) SetShaderValue(alphaShader_, gsTruncLoc, &gsTrunc, SHADER_UNIFORM_FLOAT);
        }
        const bool useShader = !wire && alphaShader_.id > 0;
        if (useShader) {
            float row3[4];
            for (int k = 0; k < 4; ++k) row3[k] = ram.f32(viewW2S() + 48 + 4 * k);
            const float params[3] = {fog_.fa, fog_.maxVal, fog_.fb};
            if (fogLoc_[0] >= 0) SetShaderValue(alphaShader_, fogLoc_[0], eye_, SHADER_UNIFORM_VEC3);
            if (fogLoc_[1] >= 0) SetShaderValue(alphaShader_, fogLoc_[1], row3, SHADER_UNIFORM_VEC4);
            if (fogLoc_[2] >= 0) SetShaderValue(alphaShader_, fogLoc_[2], params, SHADER_UNIFORM_VEC3);
            if (fogLoc_[3] >= 0) SetShaderValue(alphaShader_, fogLoc_[3], fog_.col, SHADER_UNIFORM_VEC3);
            // diagnostics: WOTM_FOGSPLIT=1 removes fog from the left half of the screen (comparison in the same frame); =2 draws the fog
            // mask (white = FOGCOL only) and prints the parameters every ~2 s
            static const int split = std::getenv("WOTM_FOGSPLIT") ? std::atoi(std::getenv("WOTM_FOGSPLIT")) : 0;
            // =3: fog only on even frames (two captures in a row = with and without, nearly the same scene)
            static uint32_t drawCount = 0;
            const bool oddFrame = (drawCount++ & 1) != 0;
            const float splitX = split == 1 ? 0.5f * float(rlGetFramebufferWidth()) : split == 2 ? -2.f
                               : (split == 3 && oddFrame) ? 1e9f : -1.f;
            static int fogLog = 0;
            if (split && fogLog++ % 120 == 0)
                std::fprintf(stderr, "[fog] FA %.3f FB %.1f max %.1f color %.0f %.0f %.0f row3 %.3f %.3f %.3f %.3f\n", fog_.fa, fog_.fb, fog_.maxVal,
                             fog_.col[0] * 255.f, fog_.col[1] * 255.f, fog_.col[2] * 255.f, row3[0], row3[1], row3[2], row3[3]);
            if (fogLoc_[4] >= 0) SetShaderValue(alphaShader_, fogLoc_[4], &splitX, SHADER_UNIFORM_FLOAT);
            BeginShaderMode(alphaShader_);
            g_premulShader = true;
        }
        vs_.setShader(useShader ? alphaShader_.id : 0u, useShader ? alphaShader_.locs : nullptr);   // rlgl's current one (0 = the default)
        return useShader;
    }

    void draw(const Ram &ram, bool wire = false) {
        ViewScope vs(vctx);
        if (drawProfOn()) ++drawProf().views;
        const bool useShader = beginShader(ram, wire);
        rlDisableDepthTest();   // the sky is drawn first, without depth
        {
            ProfScope ps(DrawProf::Sky);
            drawList(ram, sky, wire);
            rlDrawRenderBatchActive();
        }
        rlEnableDepthTest();
        static const bool onePass = std::getenv("WOTM_ONEPASS") != nullptr;   // diagnostics: everything in a single pass (translucent ones not separated)
        // Like hier: outside the alpha OT, opaque and translucent together in traversal order (writing Z per the GROUP's ZMSK); the
        // alpha OT (pass 1) afterward. With two passes (opaque first), the stage preview (tex 102, As ~ 1) came out before the big-screen
        // logo (tex 140/141, no Z write) and the logo ended up on top; on the GS the preview comes after and covers it.
        {
            ProfScope ps(DrawProf::Trav);
            drawList(ram, items, wire, (wire || onePass) ? -1 : 2);
            rlDrawRenderBatchActive();
        }
        if (!wire && !onePass) {
            ProfScope ps(DrawProf::AlphaOT);
            rlDisableDepthMask();   // translucent: drawList writes depth item by item per ZBUF_1.ZMSK (DrawItem::zmsk)
            drawList(ram, items, wire, 1);
            rlDrawRenderBatchActive();
            rlEnableDepthMask();
        }
        // cast shadows after the alpha OT: in the XGKICK the receivers redrawn with the shadow texture come after it
        if (!wire) { ProfScope ps(DrawProf::Shadows); drawShadows(ram); }
        // particles last (they test depth, don't write it): debris (drawSprites), then the other types in particleDraw order
        if (!wire) { ProfScope ps(DrawProf::Sprites); drawSprites(ram); }
        {
            ProfScope ps(DrawProf::Fx);
            // Particle alpha test: the translucent packets don't touch TEST and inherit 0x7000D (As > 0) from restoreScreenDrawEnv;
            // debris brings its own TEST and closes with setClosingContext (0x5004B: As >= 4). Measured by the check (XGKICK's TEST).
            if (!wire) setAlphaRef(sprites.empty() ? 0 : 3);
            // particles with the RAM from the capture instant (fxRam_), not the live one the game is already rewriting; textures still come from live RAM
            if (!wire) fx.draw(fxRam_ ? Ram{fxRam_.get(), ram.size} : ram, [&](uint32_t id, uint64_t t0) { return particleTexture(ram, id, t0); }, eye_);
            if (!wire) setAlphaRef(kGsAlphaRef);
            if (!wire && !deferFlares && !fx.flares.e.empty()) {   // lens flare: screen, after the particles
                if (fullDepth) {   // depth without waiting on the GPU, as in split screen (drawFlaresFor)
                    rlDrawRenderBatchActive();
                    fullDepth->issue(rlGetFramebufferWidth(), rlGetFramebufferHeight(), fullDepthFbo, redrawId);
                    fx.depthAsync = fullDepth;
                }
                fx.drawFlares([&](uint32_t id, uint64_t t0) { return particleTexture(ram, id, t0); });
                fx.depthAsync = nullptr;
            }
            rlDrawRenderBatchActive();
        }
        ProfScope psHud(DrawProf::Hud);
        if (!wire && barPass.rects) {   // bars over the enemies under the player's HUD (barPass), outside the GS shader
            rlDrawRenderBatchActive();
            if (useShader) { EndShaderMode(); g_premulShader = false; vs_.setShader(0u, nullptr); }
            drawBarPass();
            if (useShader) { BeginShaderMode(alphaShader_); g_premulShader = true; vs_.setShader(alphaShader_.id, alphaShader_.locs); }
            gsBlend(kGsAlphaNormal);   // drawList starts assuming the GS's regular blending (EndBlendMode left raylib's)
        }
        rlDisableDepthTest();   // button icons and HUD on top of everything (z near 0: the depth test hides them under the floor)
        const bool sprites2d = !wire && g_buttonIconFn && (!g_buttonIconsOn || g_buttonIconsOn());
        if (!sprites2d) drawList(ram, overlay, wire, -1, true);
        drawList(ram, hud, wire, -1);   // HUD last, in traversal order
        rlDrawRenderBatchActive();
        rlEnableDepthTest();
        if (useShader) EndShaderMode();
        g_premulShader = false;
        if (sprites2d) drawButtonSprites(ram, overlay);
    }

    // Deferred lens flare (deferFlares): the occlusion test reads the depth already drawn (glReadPixels), and each read waits for the GPU
    // to finish what it received. In split screen, with the flare inside each view's draw, the CPU stalled on every view (redraw of 3
    // views at ~60 ms). With deferral, all views are drawn first and each one's flares come afterward, on its texture, with
    // the view's list and context (`fl`, `vc`): a single wait per redraw. Inside the view's BeginMode3D.
    bool deferFlares = false;
    // Live full screen: Scene::draw's flare uses the depth from a recent redraw (AsyncDepth; the caller does the fetch at the
    // start of the redraw), copied from the `fullDepthFbo` framebuffer the frame draws into. Null (replay, corpus): the direct read.
    AsyncDepth *fullDepth = nullptr;
    unsigned fullDepthFbo = 0;
    uint64_t redrawId = 0;   // redraw in progress (the caller increments it before the views): shadow silhouettes shared among them
    // `depth`: the view's depth without waiting on the GPU (AsyncDepth; the caller did the fetch at the start of the redraw); this is where the
    // request for this time's copy goes, from the `depthFbo` framebuffer the view draws into
    void drawFlaresFor(const Ram &ram, const Particles::FlareFrame &fl, const ViewCtx &vc, AsyncDepth *depth = nullptr, unsigned depthFbo = 0) {
        if (fl.e.empty()) return;
        if (depth) {
            rlDrawRenderBatchActive();
            depth->issue(rlGetFramebufferWidth(), rlGetFramebufferHeight(), depthFbo, redrawId);
        }
        fx.depthAsync = depth;
        struct ResetDepth { Particles &p; ~ResetDepth() { p.depthAsync = nullptr; } } resetDepth{fx};
        const Particles::FlareFrame keepFl = fx.flares;
        const ViewCtx keepVc = vctx;
        fx.flares = fl;
        vctx = vc;
        {
            ViewScope vs(vctx);
            const bool useShader = beginShader(ram, false);
            rlEnableDepthTest();
            setAlphaRef(kGsAlphaRef);
            fx.drawFlares([&](uint32_t id, uint64_t t0) { return particleTexture(ram, id, t0); });
            rlDrawRenderBatchActive();
            if (useShader) EndShaderMode();
            g_premulShader = false;
        }
        fx.flares = keepFl;
        vctx = keepVc;
    }

    // 2D button icons (g_buttonIconFn) in place of McButtons' 3D objects: one per _cs, at the center of its triangles, sized by
    // the longest side on screen (camera axes), with the fade from screenSetButtonAlpha (+0x4C of the object, like fadeByNode). Inside the frame's
    // 3D mode, without depth.
    void drawButtonSprites(const Ram &ram, const std::vector<DrawItem> &list) {
        if (list.empty()) return;
        const Matrix mv = rlGetMatrixModelview();
        const float right[3] = {mv.m0, mv.m4, mv.m8}, up[3] = {mv.m1, mv.m5, mv.m9};
        struct Acc { uint32_t cs; uint16_t id; float lo[2], hi[2], c[3]; int n; float alpha; };
        std::vector<Acc> acc;
        for (const DrawItem &it : list) {
            const MeshData *md = mesh(ram, it.node);
            if (!md || md->tris.empty()) continue;
            Acc *a = nullptr;
            for (Acc &x : acc) if (x.cs == it.cs) a = &x;
            if (!a) { acc.push_back({it.cs, it.btnId, {1e30f, 1e30f}, {-1e30f, -1e30f}, {0, 0, 0}, 0, 0.f}); a = &acc.back(); }
            a->alpha = std::max(a->alpha, std::min(1.f, std::max(0.f, ram.f32(it.node + 0x4C))) * it.fade);
            for (const Vtx &v : md->tris) {
                float w[3];
                xform(it.m, v.x, v.y, v.z, w);
                const float pr = w[0] * right[0] + w[1] * right[1] + w[2] * right[2], pu = w[0] * up[0] + w[1] * up[1] + w[2] * up[2];
                a->lo[0] = std::min(a->lo[0], pr); a->hi[0] = std::max(a->hi[0], pr);
                a->lo[1] = std::min(a->lo[1], pu); a->hi[1] = std::max(a->hi[1], pu);
                for (int k = 0; k < 3; ++k) a->c[k] += w[k];
                ++a->n;
            }
        }
        rlDisableDepthTest();
        for (const Acc &a : acc) {
            if (!a.n || a.alpha <= 0.f) continue;
            const float c[3] = {a.c[0] / float(a.n), a.c[1] / float(a.n), a.c[2] / float(a.n)};
            // center: the middle of the box along the camera axes, at the mean depth
            const float cr = c[0] * right[0] + c[1] * right[1] + c[2] * right[2], cu = c[0] * up[0] + c[1] * up[1] + c[2] * up[2];
            const float dr = (a.lo[0] + a.hi[0]) * 0.5f - cr, du = (a.lo[1] + a.hi[1]) * 0.5f - cu;
            const float mid[3] = {c[0] + right[0] * dr + up[0] * du, c[1] + right[1] * dr + up[1] * du, c[2] + right[2] * dr + up[2] * du};
            const float size = std::max(a.hi[0] - a.lo[0], a.hi[1] - a.lo[1]);
            g_buttonIconFn(a.id, mid, right, up, size, a.alpha);
        }
        rlDrawRenderBatchActive();
        rlEnableDepthTest();
    }

    // High-priority list (`hp`), with the frame's 3D camera on: in front of the camera, on top of what's already on screen, without
    // depth (like the button icons).
    void drawHp(const Ram &ram, bool wire = false) {
        if (hp.empty() && hpOverlay.empty()) return;
        ViewScope vs(vctx);
        const bool useShader = beginShader(ram, wire);
        rlDisableDepthTest();
        drawList(ram, hp, wire, -1);
        const bool sprites2d = !wire && g_buttonIconFn && (!g_buttonIconsOn || g_buttonIconsOn());
        if (!sprites2d) drawList(ram, hpOverlay, wire, -1, true);
        rlDrawRenderBatchActive();
        rlEnableDepthTest();
        if (useShader) EndShaderMode();
        g_premulShader = false;
        if (sprites2d) drawButtonSprites(ram, hpOverlay);
    }

    // Translucent: texture with partial alpha or low-alpha vertices. These go in the second pass (back to front, without writing depth).
    bool translucent(const Ram &ram, const DrawItem &it) {
        const MeshData *md = mesh(ram, it.node);
        if (it.fade < 1.f) return true;   // LOD cross-fade
        if (md && md->lowAlpha) return true;
        if (md && !md->sprites.empty()) return true;   // ObjSprite: glow (alphaSort on the power-up halo)
        if (md && md->abe && it.blend != kGsAlphaNormal) return true;   // additive/darkening blending needs the translucent pass
        for (uint32_t sl = 0; sl < 5; ++sl) {
            if (md && md->slotMask && !(md->slotMask & (1u << sl))) continue;
            const uint64_t k = texKey(ram, it, sl);
            if (!k) continue;
            textureId(ram, it, sl);
            const auto f = partial_.find(k);
            if (f != partial_.end() && f->second) return true;
        }
        return false;
    }

    // Alpha OT bin (pktLinkAlphaObjAsm): z = vf30.x of VU0 program 0x0, the distance from the center of the node's sphere (+0x10) to
    // the view's plane 0 (the near one, fovNorms; in the world, the same as Particles::inViewVolume), in node space (divided by the scale).
    // z <= 8 -> bin 1; otherwise int(min(z, 1016) / 8) + 1, up to 128. pktInitReverseAlphaOT chains from bin 128 down to 0.
    // OBJECT with bit 0x8000 in its flags (+0xC): hierTraverseAsm links it into the alpha OT (pktLinkAlphaObjAsm), which goes out after
    // all of hier's normal chains; the rest go out in traversal order.
    // Bit 0x4000: only inside a LOD cross-fade (hierTraverseAsm tests flags & 0xC000; without 0x8000, it requires the inherited LOD
    // weight > 0). In luta_hud the tex 234 columns come out in the middle of the OT (program 1001 of 936-1214).
    static bool inAlphaOT(const Ram &ram, const DrawItem &it) {
        const uint16_t f = ram.u16(it.node + 0xC);
        return (f & 0x8000) || ((f & 0x4000) && it.lodX);
    }

    static int alphaBin(const Ram &ram, const DrawItem &it) {
        const uint32_t w = viewCtxAddr();   // worldCtx[view]: plane 0 at +0xF0, threshold at +0x120
        const M4 &m = it.m;
        float c[3], s2 = 0.f, z = -ram.f32(w + 0xF0 + 48);
        xform(m, ram.f32(it.node + 0x10), ram.f32(it.node + 0x14), ram.f32(it.node + 0x18), c);
        for (int k = 0; k < 3; ++k) z += ram.f32(w + 0xF0 + 4 * k) * c[k];
        for (int i = 0; i < 3; ++i) s2 = std::max(s2, m.m[i][0] * m.m[i][0] + m.m[i][1] * m.m[i][1] + m.m[i][2] * m.m[i][2]);
        if (s2 > 1e-12f) z /= std::sqrt(s2);
        if (!(z > 8.f)) return 1;
        return int(std::min(z, 1016.f) * 0.125f) + 1;
    }

    // pass: -1 everything; 0 only opaque/cutout; 1 only the alpha OT (inAlphaOT; alphaBin, LIFO tie-break); 2 everything outside the OT, in
    // traversal order (batches only between consecutive items with the same texture and the same state)
    // fadeByNode: multiplies alpha by the float at +0x4C of the object (button fade: screenSetButtonAlpha writes `targetAlpha` there)
    void drawList(const Ram &ram, const std::vector<DrawItem> &list, bool wire, int pass = -1, bool fadeByNode = false) {
        rlDisableBackfaceCulling();
        {   // diagnostics: WOTM_CMP=1 compares, per triangle, the edges of two skinned items with the same mesh (e.g. player vs enemy)
            static const bool cmp = std::getenv("WOTM_CMP") != nullptr;
            static bool done = false;
            if (cmp && !done) {
                const DrawItem *A = nullptr, *B = nullptr;
                for (const DrawItem &it : list) {
                    const MeshData *md = mesh(ram, it.node);
                    if (!md || !md->skinned || md->tris.size() != 4428 || !it.animPkt) continue;
                    if (!A) A = &it; else if (!B && it.animPkt != A->animPkt) B = &it;
                }
                if (A && B) {
                    done = true;
                    const MeshData *md = mesh(ram, A->node);
                    const uint32_t pa = palette(ram, *A), pb = palette(ram, *B);
                    int slotBad[32] = {0}, slotAll[32] = {0}, nbad = 0, ntri = 0;
                    for (size_t k = 0; k + 2 < md->tris.size(); k += 3) {
                        float qa[3][3], qb[3][3];
                        for (int c = 0; c < 3; ++c) { skinVertex(ram, pa, M4{}, md->tris[k + c], qa[c]); skinVertex(ram, pb, M4{}, md->tris[k + c], qb[c]); }
                        auto mx = [](float q[3][3]) { float m = 0; for (int c = 0; c < 3; ++c) { const float *a = q[c], *b = q[(c + 1) % 3]; m = std::max(m, std::sqrt((a[0]-b[0])*(a[0]-b[0]) + (a[1]-b[1])*(a[1]-b[1]) + (a[2]-b[2])*(a[2]-b[2]))); } return m; };
                        const float ea = mx(qa), eb = mx(qb);
                        ++ntri;
                        auto area = [](float q[3][3]) { const float u[3] = {q[1][0]-q[0][0], q[1][1]-q[0][1], q[1][2]-q[0][2]}, w[3] = {q[2][0]-q[0][0], q[2][1]-q[0][1], q[2][2]-q[0][2]};
                            const float cx = u[1]*w[2]-u[2]*w[1], cy = u[2]*w[0]-u[0]*w[2], cz = u[0]*w[1]-u[1]*w[0]; return 0.5f * std::sqrt(cx*cx + cy*cy + cz*cz); };
                        const float aa = area(qa), ab = area(qb);
                        const bool bad = ab < 0.15f * aa && aa > 4.f;   // triangle collapsed in B (area < 15%% of A's)
                        if (bad) ++nbad;
                        for (int c = 0; c < 3; ++c) {
                            const Vtx &v = md->tris[k + c];
                            const int sl[3] = {(v.cx & 0x7F) / 4, (v.cy & 0x7F) / 4, (v.cz & 0x7F) / 4};
                            for (int z = 0; z < 3; ++z) if (sl[z] < 32) { ++slotAll[sl[z]]; if (bad) ++slotBad[sl[z]]; }
                        }
                    }
                    std::fprintf(stderr, "[cmp] tris %d, collapsed in B (area < 15%% of A's): %d\n", ntri, nbad);
                    for (int z = 0; z < 31; ++z) std::fprintf(stderr, "[cmp] slot %2d: uses %4d, in torn %4d (%.0f%%)\n", z, slotAll[z], slotBad[z], slotAll[z] ? 100.0 * slotBad[z] / slotAll[z] : 0.0);
                }
            }
        }
        struct Ent { uint64_t key; uint32_t item; uint32_t slot; };
        constexpr uint32_t kSlotDouble = 0x80000000u;   // Ent::slot: 2nd pass of GsBlendFactors::doubled (destination * 2)
        constexpr uint32_t kSlotOver1 = 0x40000000u;    // Ent::slot: 2nd and 3rd passes of regular blending with As > 1 (gsBlendCommon)
        constexpr uint32_t kSlotOver2 = 0x20000000u;
        constexpr uint32_t kSlotPass = kSlotDouble | kSlotOver1 | kSlotOver2;
        // Regular blending where As can exceed 1: the largest vertex alpha (with the fade) times the texture's largest is above 128 * 128.
        // Untextured, the white 0x80; previous frame and movie go without GS mode (As up to 1).
        // The sky is left out: it's the frame's first draw, and the reference doesn't show the gain on the dome (the destination under it is the
        // previous frame, the same sky, and Cs As - Cd (As - 1) converges to Cs, the same as As capped at 1). In native the destination is the
        // black clear, and the passes made the dome As times brighter (x1.027 in the corpus).
        static const bool idcOpaqueOver = std::getenv("WOTM_IDCOLOR") != nullptr;
        static const bool noOver = std::getenv("WOTM_NOOVER") != nullptr;   // diagnostics: As capped at 1 in regular blending (as before)
        const bool skyList = &list == &sky;
        auto over = [&](const DrawItem &it, const MeshData *m, uint32_t sl) {
            if (wire || noOver || skyList || idcOpaqueOver || !m || !m->abe || !gsBlendCommon(it.blend)) return false;
            const float av = float(m->maxA) * it.fade * std::max(0.f, it.color[3]);
            if (av * 255.f <= 16384.f) return false;
            const uint32_t ts = texRemap(sl);
            const uint64_t key = texKey(ram, it, ts);
            if (key == kScreenKey || (movieTexture && objectTexId(ram, it.node, ts) == 999)) return false;
            uint32_t at = 0x80;
            if (key && textureId(ram, it.node, ts)) { const auto f = texAmax_.find(key); if (f != texAmax_.end()) at = f->second; }
            return av * float(at) > 16384.f;
        };
        std::vector<Ent> order;   // (texture key, item index, texture slot)
        order.reserve(list.size());
        const bool far2near = pass == 1 && !wire;   // alpha OT: bins and LIFO
        const bool trav = pass == 2 && !wire;       // outside the OT: traversal order, like hier's normal chains
        for (uint32_t i = 0; i < list.size(); ++i) {
            if (pass >= 0 && !wire) {
                const bool sel = pass == 0 ? !translucent(ram, list[i]) : pass == 1 ? inAlphaOT(ram, list[i]) : !inAlphaOT(ram, list[i]);
                if (!sel) continue;
            }
            {   // diagnostics: WOTM_ONLYTEX=<id[,id..]> draws only those texIds (bisecting defects)
                static const char *only = std::getenv("WOTM_ONLYTEX");
                if (only) {   // with a leading '-': draws everything except those
                    const bool skip = *only == '-';
                    const std::string s = std::string(",") + (skip ? only + 1 : only) + ",";
                    if ((s.find("," + std::to_string(ram.u16(list[i].node + 0x50)) + ",") == std::string::npos) != skip) continue;
                }
            }
            {   // diagnostics: WOTM_ONLYNODE=<hex[,hex..]> draws only those nodes (isolates one character from the rest of the scene)
                static const char *onlyN = std::getenv("WOTM_ONLYNODE");
                if (onlyN) {
                    char b[16]; std::snprintf(b, sizeof b, ",%x,", list[i].node);
                    if ((std::string(",") + onlyN + ",").find(b) == std::string::npos) continue;
                }
                static const char *skipN = std::getenv("WOTM_SKIPNODE");   // =<hex[,hex..]>: draws everything except those nodes
                if (skipN) {
                    char b[16]; std::snprintf(b, sizeof b, ",%x,", list[i].node);
                    if ((std::string(",") + skipN + ",").find(b) != std::string::npos) continue;
                }
            }
            const MeshData *mdi = mesh(ram, list[i].node);
            {   // diagnostics: WOTM_LISTITEMS=1 lists the items of each pass (node, texId, zBufferFudge, translucent, position, triangles)
                static const bool li = std::getenv("WOTM_LISTITEMS") != nullptr;
                if (li && !wire)
                    std::fprintf(stderr, "[item] pass %d node %x tex %u fz %.4f tris %zu pos %.1f %.1f %.1f blend %02x abe %d zmsk %d fade %.2f\n", pass, list[i].node,
                                 unsigned(ram.u16(list[i].node + 0x50)), ram.f32(list[i].node + 0x28), mdi ? mdi->tris.size() / 3 : size_t(0), list[i].m.m[3][0],
                                 list[i].m.m[3][1], list[i].m.m[3][2], list[i].blend, mdi ? int(mdi->abe) : -1, int(list[i].zmsk), list[i].fade);
            }
            const uint32_t smask = (mdi && mdi->slotMask) ? mdi->slotMask : 1u;
            for (uint32_t sl = 0; sl < 32; ++sl) {   // slots 0-4 and kSlotNoTex
            if (!(smask & (1u << sl))) continue;
            { static const int onlySl = std::getenv("WOTM_ONLYSLOT") ? std::atoi(std::getenv("WOTM_ONLYSLOT")) : -1; if (onlySl >= 0 && int(sl) != onlySl) continue; }   // diagnostics: only this texture slot
            uint64_t key = wire ? 0 : texKey(ram, list[i], texRemap(sl));
            if (far2near) key = uint64_t(2 + 128 - alphaBin(ram, list[i]));   // farthest bin first (never 0 or 1)
            order.push_back({key, i, sl});
            if (!wire && mdi && mdi->abe && gsBlendFactors(list[i].blend).doubled) order.push_back({key, i, sl | kSlotDouble});   // 2nd pass, right after
            if (over(list[i], mdi, sl)) { order.push_back({key, i, sl | kSlotOver1}); order.push_back({key, i, sl | kSlotOver2}); }
            }
        }
        // Translucent pass in retail order: objects outside the alpha OT go out in the normal chains, in traversal order; then
        // the OT (inAlphaOT), from the farthest bins to the nearest and, within a bin, the last in traversal first
        // (pktLinkAlphaObjAsm inserts at the head of the list). An item's slots and the 2nd pass stay together.
        if (!trav)   // in traversal the order is already the list's (ascending items, an item's slots together)
            std::stable_sort(order.begin(), order.end(), [far2near](const Ent &a, const Ent &b) {
                if (a.key != b.key) return a.key < b.key;
                return far2near && a.item > b.item;   // OT: the last in traversal first
            });
        {   // diagnostics: WOTM_ALPHAORDER=1 lists the translucent pass in output order (node, texId, alpha OT bin, ALPHA_1)
            static const bool ao = std::getenv("WOTM_ALPHAORDER") != nullptr;
            if (ao && far2near)
                for (const Ent &e : order)
                    std::fprintf(stderr, "[alpha] node %x tex %u bin %d item %u slot %x blend %02x\n", list[e.item].node, unsigned(ram.u16(list[e.item].node + 0x50)),
                                 130 - int(e.key), e.item, e.slot, list[e.item].blend);
        }

        SpriteView sprView;   // axes and focals for the ObjSprites (built at the first item that has one)
        EnvView envV;         // camera for the environment map UV (same, at the first item with DrawItem::env)
        const unsigned white = gsWhiteTexture();   // untextured sections (PRIM.TME 0): GS alpha 0x80 = 1.0
        uint64_t cur = ~0ull;
        bool curClamp = false;     // open batch with DrawItem::env 1 (texture in CLAMP)
        unsigned clampedId = 0;    // texture set to CLAMP in the open batch; goes back to REPEAT after the flush
        bool open = false;
        uint8_t curBlend = kGsAlphaNormal;   // DrawItem::blend active in GL (the previous batch was already flushed when this changes)
        bool curPremul = false;              // gsBlendPremul(curBlend): premultiplied color in the shader (shaderNormal with pm)
        constexpr uint8_t kBlendDouble = 0xFF;   // curBlend in doubled's 2nd pass (A = 3 doesn't exist: it's never an ALPHA)
        uint32_t curPass = 0;  // pass of the open batch (0 or kSlotDouble/kSlotOver1/kSlotOver2)
        int curMode = 0;       // shaderNormal mode in the open batch (1 and 2: doubled passes; 3 to 5: As > 1 in regular blending)
        uint8_t curItBlend = 0;
        int16_t curItRef = 0;   // item state of the open batch (traversal pass: a batch only with the same state)
        bool curItZ = false, curItAbe = false;
        // Vertices through the VtxStream (the same rlgl would send, without the four calls per vertex); wireframe and WOTM_NOVTXSTREAM=1
        // use rlgl's immediate mode. In the stream, a new batch that only changes texture becomes another command in the same upload; the queue is
        // flushed only before a GL state change (blending, alpha test, Z mask, CLAMP), which takes effect at the flush.
        static const bool noStream = std::getenv("WOTM_NOVTXSTREAM") != nullptr;
        const bool stream = !wire && !noStream && vs_.ok();
        // in the stream, the vertex math of each chunk (transform, bones, lighting) goes to the helper threads (JobPool): the
        // state and commands go out here, in order, and the vertices drop into their place at the flush
        const bool deferVerts = stream && JobPool::get().active();
        if (resReset_ && stream && vs_.empty()) {   // arena full: start over (orphans the buffer; draws already sent keep using the old one)
            vs_.arenaReset();
            resChunks_.clear();
            resReset_ = false;
        }
        auto flushStream = [&] {
            if (!stream || vs_.empty()) return;
            ProfScope ps(DrawProf::Flush);
            vs_.flush();
            if (drawProfOn()) { ++drawProf().batches; drawProf().draws += vs_.draws(); }
            vs_.resetDraws();
        };
        // GL state change: in the stream it enters the VtxStream queue (in vertex order, without flushing); otherwise right now
        auto glState = [&](auto &&fn) { if (stream) vs_.state(fn); else fn(); };
        auto setBlend = [&](uint8_t b) {
            if (b == curBlend) return;
            curBlend = b;
            curPremul = gsBlendPremul(b);
            glState([b] { gsBlend(b); });
        };
        int16_t curRef = kGsAlphaRef;   // DrawItem::aref in the shader uniform (same)
        auto setRef = [&](int16_t r) {
            if (r == curRef) return;
            curRef = r;
            glState([this, r] { setAlphaRef(r); });
        };
        int curZm = -1;   // Z mask set here (-1: the caller's)
        auto setZm = [&](bool on) {
            if (int(on) == curZm) return;
            curZm = int(on);
            glState([on] { if (on) rlEnableDepthMask(); else rlDisableDepthMask(); });
        };
        auto vColor = [&](uint8_t r, uint8_t g, uint8_t b, uint8_t a) { if (stream) vs_.color(r, g, b, a); else rlColor4ub(r, g, b, a); };
        auto vTex = [&](float u, float v) { if (stream) vs_.texcoord(u, v); else rlTexCoord2f(u, v); };
        auto vNormal = [&](float burn, bool fge, bool gs, bool pm, int mode) {
            if (!stream) { shaderNormal(burn, fge, gs, pm, mode); return; }
            float n[3];
            shaderNormalVals(burn, fge, gs, pm, mode, n);
            vs_.normal(n[0], n[1], n[2]);
        };
        auto vVertex = [&](float x, float y, float z) { if (stream) vs_.vertex(x, y, z); else rlVertex3f(x, y, z); };
        for (const auto &o : order) {
            const DrawItem &it = list[o.item];
            const MeshData *md = mesh(ram, it.node);
            std::shared_ptr<const MeshData> mdKeep = lastMesh_;   // the deferred chunk holds the mesh (the cache may swap it before the flush)
            {   // diagnostics: WOTM_LISTSKIN=1 lists the skinned items drawn (node, LOD, animPkt, object position)
                static const bool ls = std::getenv("WOTM_LISTSKIN") != nullptr;
                if (ls && md && md->skinned && !wire)
                    std::fprintf(stderr, "[skin] node %x lod %d animPkt %x pos %.1f %.1f %.1f tris %zu slot %u\n", it.node, it.lodIdx, it.animPkt, it.m.m[3][0], it.m.m[3][1], it.m.m[3][2], md->tris.size() / 3, o.slot);
                if (ls && md && md->skinned && !wire) {
                    uint64_t h = 1469598103934665603ull;   // FNV-1a of the decoded geometry (no color): compares meshes of different monsters
                    for (const Vtx &v : md->tris) {
                        const float f[6] = {v.x, v.y, v.z, v.u, v.v, v.w};
                        const uint8_t *q = reinterpret_cast<const uint8_t *>(f);
                        for (int b = 0; b < 24; ++b) h = (h ^ q[b]) * 1099511628211ull;
                        const uint8_t c[4] = {v.cx, v.cy, v.cz, v.slot};
                        for (int b = 0; b < 4; ++b) h = (h ^ c[b]) * 1099511628211ull;
                    }
                    int perSlot[8] = {0};
                    for (const Vtx &v : md->tris) ++perSlot[v.slot & 7];
                    std::fprintf(stderr, "   slotMask %x verts/slot %d %d %d %d %d texIds %u %u %u\n", md->slotMask, perSlot[0], perSlot[1], perSlot[2], perSlot[3], perSlot[4],
                                 unsigned(ram.u16(it.node + 0x50)), unsigned(ram.u16(it.node + 0x52)), unsigned(ram.u16(it.node + 0x54)));
                    std::fprintf(stderr, "   geomhash %016llx nverts %zu\n", (unsigned long long)h, md->tris.size());
                    // edges after skinning (world units): a torn mesh has a long tail; compares weight models and palettes
                    const uint32_t palD = palette(ram, it);
                    std::vector<float> el;
                    for (size_t k = 0; palD && k + 2 < md->tris.size(); k += 3) {
                        float q[3][3];
                        for (int c = 0; c < 3; ++c) skinVertex(ram, palD, it.m, md->tris[k + c], q[c]);
                        float mx = 0;
                        for (int c = 0; c < 3; ++c) {
                            const float *a = q[c], *b = q[(c + 1) % 3];
                            mx = std::max(mx, std::sqrt((a[0] - b[0]) * (a[0] - b[0]) + (a[1] - b[1]) * (a[1] - b[1]) + (a[2] - b[2]) * (a[2] - b[2])));
                        }
                        el.push_back(mx);
                    }
                    if (!el.empty()) {
                        std::sort(el.begin(), el.end());
                        std::fprintf(stderr, "   maxedge med %.2f p90 %.2f p99 %.2f max %.2f\n", el[el.size() / 2], el[el.size() * 9 / 10], el[el.size() * 99 / 100], el.back());
                    }
                }
                if (ls && md && md->skinned && !wire) { for (int r = 0; r < 3; ++r) std::fprintf(stderr, "   m[%d] %.3f %.3f %.3f %.3f\n", r, it.m.m[r][0], it.m.m[r][1], it.m.m[r][2], it.m.m[r][3]); }
            }
            if (!md || !md->valid || (md->tris.empty() && md->sprites.empty())) continue;
            const uint32_t passK = o.slot & kSlotPass;
            const bool dbl = passK == kSlotDouble;
            const uint32_t slot = o.slot & ~kSlotPass;
            const bool multi = md->slotMask & (md->slotMask - 1);   // more than one slot used
            const bool envClamp = it.env == 1 && !wire;   // regular environment map: CLAMP_1 = 5 (object textures repeat)
            // in traversal the batch continues only with the same texture and the same GS state (blending, alpha test, ZMSK)
            const bool travState = trav && open && (it.blend != curItBlend || it.aref != curItRef || it.zmsk != curItZ || md->abe != curItAbe);
            if (o.key != cur || !open || far2near || travState || envClamp != curClamp || passK != curPass) {
                curItBlend = it.blend; curItRef = it.aref; curItZ = it.zmsk; curItAbe = md->abe;
                if (open && !stream) {   // one batch per texture (avoids rlgl's automatic alignment)
                    ProfScope ps(DrawProf::Flush);
                    rlEnd();
                    rlDrawRenderBatchActive();
                    if (drawProfOn()) ++drawProf().batches;
                }
                if (clampedId) {
                    const unsigned t = clampedId;
                    glState([t] { rlTextureParameters(t, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_REPEAT); rlTextureParameters(t, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_REPEAT); });
                    clampedId = 0;
                }
                if (!wire && dbl) { glState([] { gsBlendDouble(); }); curBlend = kBlendDouble; curPremul = false; }
                else if (!wire && passK == kSlotOver1) { glState([] { gsBlendOver1(); }); curBlend = kBlendDouble; curPremul = false; }
                else if (!wire && passK == kSlotOver2) { glState([] { gsBlendOver2(); }); curBlend = kBlendDouble; curPremul = false; }
                // without ABE the GS writes the source color without blending (alpha, with the LOD fade, only counts for the test): luta_hud's
                // building 133 in cross-fade came out half transparent and showed the building behind it
                else if (!wire) {
                    static const bool idcOpaque = std::getenv("WOTM_IDCOLOR") && std::atoi(std::getenv("WOTM_IDCOLOR")) == 2;
                    setBlend(md->abe && !idcOpaque ? it.blend : kGsNoAbe);
                }
                curPass = passK;
                if (!wire) setRef(it.aref);
                // translucent pass: writes depth like the GS, unless the GROUP turns on ZMSK (the caller restores the mask at the end)
                if (far2near || trav) setZm(!it.zmsk);
                cur = o.key;
                curClamp = envClamp;
                const unsigned id = wire ? 0 : textureId(ram, it, texRemap(slot));
                if (envClamp && id && id != screenId_) {
                    glState([id] { rlTextureParameters(id, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_CLAMP); rlTextureParameters(id, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_CLAMP); });
                    clampedId = id;
                }
                static const bool idc = std::getenv("WOTM_IDCOLOR") != nullptr;   // diagnostics: color = the object's texId (read the pixel to find the id)
                if (stream) vs_.begin(RL_TRIANGLES, (id && !idc) ? id : white);
                else {
                    rlBegin(wire ? RL_LINES : RL_TRIANGLES);   // rlBegin with another mode resets the batch texture: set it afterward
                    rlSetTexture((id && !idc) ? id : white);
                }
                open = true;
            }
            // mode per item (goes in the vertex): an item with As > 1 can land in a batch opened by another one without it
            curMode = dbl ? 2 : passK == kSlotOver1 ? 4 : passK == kSlotOver2 ? 5
                    : (!wire && md->abe && gsBlendFactors(it.blend).doubled) ? 1 : over(it, md, slot) ? 3 : 0;
            static const bool idcMode = std::getenv("WOTM_IDCOLOR") != nullptr;   // once: getenv per vertex is slow on Windows
            static const bool idcNode = idcMode && std::atoi(std::getenv("WOTM_IDCOLOR")) == 2;   // =2: node >> 4 in RGB, no blending or fog
            const unsigned tid = (wire || !cur) ? 0 : textureId(ram, it, texRemap(slot));
            // alpha like the GS (texture with raw alpha, see textureId), except for the previous frame (render texture) and the movie
            const bool gsA = !(tid && (tid == screenId_ || (movieTexture && objectTexId(ram, it.node, texRemap(slot)) == 999)));
            const auto &t = md->tris;
            if (drawProfOn()) drawProf().verts += long(t.size());
            {   // diagnostics: WOTM_TEXINFO=<id> prints (up to WOTM_TEXINFO_N times, default 6) the objects with that texId: blending, color, fudge, texture and vertices
                static const int ti = std::getenv("WOTM_TEXINFO") ? std::atoi(std::getenv("WOTM_TEXINFO")) : -1;
                static int printed = 0;
                static const int tiMax = std::getenv("WOTM_TEXINFO_N") ? std::atoi(std::getenv("WOTM_TEXINFO_N")) : 6;
                if (printed < tiMax && ((ti >= 0 && int(objectTexId(ram, it.node, slot)) == ti) || (ti == -2 && !md->sprites.empty()) || (ti == -3 && it.blend != kGsAlphaNormal && !t.empty()) || (ti == -4 && it.aref == kGsAlphaNoTest) || (ti == -5 && it.color[1] > 1.05f && it.color[0] < 0.95f))) {   // -2: all OBJECT sprites; -3: meshes outside regular blending; -4: no alpha test; -5: greenish color (highlighted pick-up)
                    ++printed;
                    std::fprintf(stderr, "[texinfo] tex %u node %x blend %d abe %d color %.3f %.3f %.3f %.3f fade %.3f gsA %d key %llx tris %zu sprites %zu tex0 %llx aref %d\n", objectTexId(ram, it.node, slot), it.node, it.blend,
                                 int(md->abe), it.color[0], it.color[1], it.color[2], it.color[3], it.fade, int(gsA), (unsigned long long)o.key, t.size(),
                                 md->sprites.size(), (unsigned long long)objectTex0(ram, it.node, slot), int(it.aref));
                    std::fprintf(stderr, "[texinfo]   fudge %.4f burn %.3f flags %04x pos %.1f %.1f %.1f\n", ram.f32(it.node + 0x28), ram.f32(it.node + 0x2C),
                                 ram.u16(it.node + 0xC), it.m.m[3][0], it.m.m[3][1], it.m.m[3][2]);
                    std::vector<uint32_t> tp;
                    uint32_t tw = 0, th = 0;
                    if (decodeTex(ram, it.node, tp, tw, th, slot, true)) {
                        double s[4] = {0, 0, 0, 0};
                        uint32_t amin = 255, amax = 0;
                        for (uint32_t c : tp) { for (int k = 0; k < 4; ++k) s[k] += (c >> (8 * k)) & 255; amin = std::min(amin, c >> 24); amax = std::max(amax, c >> 24); }
                        std::fprintf(stderr, "[texinfo]   texture %ux%u mean %.0f %.0f %.0f %.0f alpha %u..%u texKey %llx\n", tw, th, s[0] / tp.size(), s[1] / tp.size(),
                                     s[2] / tp.size(), s[3] / tp.size(), amin, amax, (unsigned long long)texKey(ram, it.node, slot));
                        if (const char *dir = std::getenv("WOTM_TEXINFO_PNG")) {   // writes <dir>/texinfo_<texKey>.png (raw GS alpha, 0x80 = 1.0)
                            char name[1024];
                            std::snprintf(name, sizeof name, "%s/texinfo_%llx.png", dir, (unsigned long long)texKey(ram, it.node, slot));
                            Image im{tp.data(), int(tw), int(th), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                            ExportImage(im, name);
                        }
                        for (uint32_t y = 0; y < th; y += th / 8) {
                            std::fprintf(stderr, "[texinfo]   ");
                            for (uint32_t x = 0; x < tw; x += tw / 8) std::fprintf(stderr, "%08x ", tp[y * tw + x]);
                            std::fprintf(stderr, "\n");
                        }
                    }
                    for (size_t q = 0; q < t.size() && q < 6; ++q)
                        std::fprintf(stderr, "[texinfo]   v %.1f %.1f %.1f uv %.3f %.3f rgba %u %u %u %u fge %d\n", t[q].x, t[q].y, t[q].z, t[q].u, t[q].v,
                                     t[q].r, t[q].g, t[q].b, t[q].a, int(t[q].fge));
                    for (size_t q = 0; q < md->sprites.size() && q < 6; ++q) {   // OBJECT sprite (objType 2)
                        const ObjSprite &s = md->sprites[q];
                        std::fprintf(stderr, "[texinfo]   sprite uv %.3f %.3f - %.3f %.3f rgba %u %u %u %u fge %d\n", s.u0, s.v0, s.u1, s.v1,
                                     unsigned(s.r), unsigned(s.g), unsigned(s.b), unsigned(s.a), int(s.fge));
                    }
                }
            }
            static const bool forceOpaqueEnv = std::getenv("WOTM_OPAQUE") != nullptr;   // diagnostics: vertex alpha ignored on skinned
            const bool forceOpaque = forceOpaqueEnv && md->skinned;
            const float fade = (fadeByNode ? std::min(1.f, std::max(0.f, ram.f32(it.node + 0x4C))) : 1.f) * it.fade * std::max(0.f, it.color[3]);
            const ObjLight ol = objectLight(ram, it);
            const float fogBurn = ram.f32(it.node + 0x2C);   // fogBurnThru (added to the fog F)
            static const bool noVcol = std::getenv("WOTM_NOVCOL") != nullptr;
            static const bool noSkin = std::getenv("WOTM_NOSKIN") != nullptr;   // diagnostics: reference pose, no skinning
            static const uint32_t palOverride = std::getenv("WOTM_PALBLOCK") ? uint32_t(std::strtoul(std::getenv("WOTM_PALBLOCK"), nullptr, 16)) : 0;   // diagnostics
            const uint32_t pal = (md->skinned && !noSkin) ? (palOverride ? palOverride : palette(ram, it)) : 0;
            std::vector<M4> pmTmp;   // palette read from RAM (item without a copy: viewer, or WOTM_PALBLOCK)
            const M4 *pm = nullptr;
            if (md->skinned && !noSkin) {
                if (!palOverride && it.pal && it.pal->size() >= 31) pm = it.pal->data();
                else if (pal) { for (uint32_t kk = 0; kk < 32; ++kk) pmTmp.push_back(readM4(ram, pal + 0x10 + 0x40 * kk)); pm = pmTmp.data(); }
            }
            M4 skinM = it.m;
            {   // diagnostics: WOTM_SKINM1=<animPkt hex> uses that item's object matrix for all skinned items (isolates matrix vs palette)
                static const uint32_t refPkt = std::getenv("WOTM_SKINM1") ? uint32_t(std::strtoul(std::getenv("WOTM_SKINM1"), nullptr, 16)) : 0;
                if (refPkt && md->skinned)
                    for (const DrawItem &r : list) if (r.animPkt == refPkt) { skinM = r.m; break; }
            }   // diagnostics: WOTM_SKINPOS=x,y,z puts all skinned items at that position (tests position dependence)
            { static float sp[3]; static const int have = std::getenv("WOTM_SKINPOS") ? std::sscanf(std::getenv("WOTM_SKINPOS"), "%f,%f,%f", sp, sp + 1, sp + 2) : 0; if (have == 3 && md->skinned) for (int c = 0; c < 3; ++c) skinM.m[3][c] = sp[c]; }
            // zBufferFudge (object +0x28): depth multiplier (1.0 = nothing). Scaling the vertex along the ray from the
            // eye changes only the depth, not the position on screen: fixes z-fighting between coplanar layers.
            const float fz = ram.f32(it.node + 0x28);
            const bool fudge = !wire && eyeOk_ && fz > 0.5f && fz < 2.f && fz != 1.f;
            const float fzScale = 1.f / fz;   // zBufferFudge > 1 = nearer (along the eye ray)
            float rowLen[3];   // rows of the light matrix (vertexRgb), once per item
            rowLengths(pm ? skinM : it.m, rowLen);
            {   // diagnostics: WOTM_SKINBOX=1 prints the (world) box of each node's skinned vertices, per bone x
                static const bool sb = std::getenv("WOTM_SKINBOX") != nullptr;
                if (sb && pm && md->skinned && !wire) {
                    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f}, rlo[3] = {1e30f, 1e30f, 1e30f}, rhi[3] = {-1e30f, -1e30f, -1e30f};
                    for (const Vtx &v : t) {
                        float p[3];
                        skinVertexPM(pm, skinM, v, p);
                        const float r[3] = {v.x, v.y, v.z};
                        for (int c = 0; c < 3; ++c) { lo[c] = std::min(lo[c], p[c]); hi[c] = std::max(hi[c], p[c]); rlo[c] = std::min(rlo[c], r[c]); rhi[c] = std::max(rhi[c], r[c]); }
                    }
                    for (int s = 0; s < 31; ++s) {   // largest |element| of each palette matrix (detects garbage/NaN)
                        float mx = 0.f; bool bad = false;
                        for (int r = 0; r < 4; ++r) for (int c = 0; c < 4; ++c) { const float e = pm[s].m[r][c]; if (!(e == e) || std::fabs(e) > 1e6f) bad = true; mx = std::max(mx, std::fabs(e)); }
                        if (bad || mx > 500.f) std::fprintf(stderr, "[skinbox]   bone %d: max|e| %.3g%s\n", s, double(mx), bad ? " (GARBAGE)" : "");
                    }
                    std::fprintf(stderr, "[skinbox] node %x world x[%.1f %.1f] y[%.1f %.1f] z[%.1f %.1f] | raw x[%.1f %.1f] y[%.1f %.1f] z[%.1f %.1f]\n", it.node, lo[0], hi[0], lo[1], hi[1], lo[2], hi[2], rlo[0], rhi[0], rlo[1], rhi[1], rlo[2], rhi[2]);
                }
            }
            // environment map camera and sprite axes: once per list, here (read from RAM), before the vertices
            if (it.env && tid && !envV.ok && eyeOk_) {
                float W[4][4];
                for (int r = 0; r < 4; ++r)
                    for (int c = 0; c < 4; ++c) W[r][c] = ram.f32(viewW2S() + 16 * r + 4 * c);
                envV = envView(ram, W, eye_);
            }
            if (!md->sprites.empty() && !sprView.ok) sprView = SpriteView::make(ram, eye_);
            EmitIn in{&it, md, slot, multi, pm, skinM, fudge, fzScale, {rowLen[0], rowLen[1], rowLen[2]}, ol, fade, fogBurn, tid, gsA,
                      forceOpaque, curPremul, curMode, wire};
            // Static geometry (no bones, environment map, zBufferFudge or sprites; the same key as a previous
            // redraw): the vertices stay in the GPU arena and the following views and frames only draw the range (residentDraw)
            if (stream && !pm && !md->skinned && !fudge && !it.env && md->sprites.empty() && gsA && tid != screenId_ &&
                mdKeep.get() == md && residentDraw(in, mdKeep))
                continue;
            if (deferVerts && mdKeep.get() == md) {   // this chunk's vertices go out on the helper threads (below), in its place in the VtxStream
                if (nJobs_ == jobs_.size()) jobs_.emplace_back();
                EmitJob &j = jobs_[nJobs_++];
                j.keep = mdKeep;
                if (!pmTmp.empty()) { j.pmOwn.swap(pmTmp); in.pm = j.pmOwn.data(); }
                j.in = in;
                j.buf = vs_.defer();
            } else {
                struct Direct {
                    decltype(vColor) &c; decltype(vTex) &t; decltype(vNormal) &n; decltype(vVertex) &v;
                    void color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { c(r, g, b, a); }
                    void tex(float u, float w) { t(u, w); }
                    void shaderN(float burn, bool fge, bool gs, bool pm, int mode) { n(burn, fge, gs, pm, mode); }
                    void vertex(float x, float y, float z) { v(x, y, z); if (drawProfOn()) ++drawProf().emitted; }
                    void tri(bool) {}
                } sink{vColor, vTex, vNormal, vVertex};
                emitVerts(ram, in, envV, sprView, sink);
            }
        }
        if (nJobs_) {   // the deferred chunks, in parallel; each in its own buffer
            ProfScope ps(DrawProf::Jobs);
            JobPool::get().parallelFor(int(nJobs_), [&](int i) {
                EmitJob &j = jobs_[size_t(i)];
                BufSink sink{vs_.deferBuf(j.buf)};
                emitVerts(ram, j.in, envV, sprView, sink);
            });
            if (drawProfOn()) for (size_t i = 0; i < nJobs_; ++i) drawProf().emitted += long(vs_.deferBuf(jobs_[i].buf).size());
            for (size_t i = 0; i < nJobs_; ++i) { jobs_[i].keep.reset(); jobs_[i].pmOwn.clear(); }
            nJobs_ = 0;
        }
        if (open) {   // flushes the last batch BEFORE turning culling back on (GL state applies at the flush)
            if (stream) flushStream();
            else { rlEnd(); rlDrawRenderBatchActive(); }
        }
        if (clampedId) { rlTextureParameters(clampedId, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_REPEAT); rlTextureParameters(clampedId, RL_TEXTURE_WRAP_T, RL_TEXTURE_WRAP_REPEAT); }
        rlNormal3f(0.f, 0.f, 0.f);   // whatever comes next (shadows, particles) goes out without fog
        setBlend(kGsAlphaNormal);
        setRef(kGsAlphaRef);
        rlSetTexture(0);
        if (trav) rlEnableDepthMask();   // traversal changes the mask item by item; whoever comes next expects to write
        rlEnableBackfaceCulling();
    }

    // Game text (pause menu, HUD): the game builds the frame in `fontPacket` (a GIF packet for the GS: A+D header with the atlas TEX0,
    // then sprite runs [UV, XYZ3, UV, XYZ2] x n separated by [PRIM, RGBAQ] that change color) and dispatches it in fontDmaFontData, which
    // resets the counter (D_006F8D30 goes back to D_006F8D44 = header size) and leaves the packet in RAM. Counter != header = the game is
    // building the frame: keeps the previous text. In a frame without text the terminator (EOP) lands right after the header and the text disappears.
    void updateText(const Ram &ram) {
        if (ram.u32(0x006F8D30) != ram.u32(0x006F8D44)) return;
        std::vector<TextQuad> q;
        uint32_t p = addr::fontPacket;
        uint64_t tex0 = 0;
        uint8_t col[4] = {0x80, 0x80, 0x80, 0x80};
        auto u64 = [&](uint32_t a) { return uint64_t(ram.u32(a)) | (uint64_t(ram.u32(a + 4)) << 32); };
        for (int guard = 0; guard < 4096 && ram.ok(p, 16); ++guard) {
            const uint64_t tag = u64(p), regs = u64(p + 8);
            const uint32_t nloop = uint32_t(tag & 0x7FFF), flg = uint32_t(tag >> 58) & 3, nreg = (uint32_t(tag >> 60) & 15) ? (uint32_t(tag >> 60) & 15) : 16;
            p += 16;
            if (flg == 0 && regs == 0xE) {   // A+D: register address in hi, data in lo
                for (uint32_t i = 0; i < nloop && ram.ok(p, 16); ++i, p += 16)
                    if ((u64(p + 8) & 0xFF) == 0x06) tex0 = u64(p);   // TEX0_1
            } else if (flg == 1 && nreg == 2 && (regs & 0xFF) == 0x10) {   // [PRIM, RGBAQ]: color of the next run
                for (uint32_t i = 0; i < nloop && ram.ok(p, 16); ++i, p += 16) {
                    const uint32_t c = ram.u32(p + 8);
                    col[0] = uint8_t(c); col[1] = uint8_t(c >> 8); col[2] = uint8_t(c >> 16); col[3] = uint8_t(c >> 24);
                }
            } else if (flg == 1 && nreg == 4 && (regs & 0xFFFF) == 0x53D3) {   // sprites: UV, XYZ3, UV, XYZ2 (12.4, GS center at 2048)
                for (uint32_t i = 0; i < nloop && ram.ok(p, 32); ++i, p += 32) {
                    constexpr float kOx = 2048.f - 320.f, kOy = 2048.f - 112.f;   // full viewport: 640 x 224 (field), center 2048
                    TextQuad t{};
                    t.u0 = float(ram.u16(p) & 0x3FFF) / 16.f; t.v0 = float(ram.u16(p + 2) & 0x3FFF) / 16.f;
                    t.x0 = float(ram.u16(p + 8)) / 16.f - kOx; t.y0 = float(ram.u16(p + 10)) / 16.f - kOy;
                    t.u1 = float(ram.u16(p + 16) & 0x3FFF) / 16.f; t.v1 = float(ram.u16(p + 18) & 0x3FFF) / 16.f;
                    t.x1 = float(ram.u16(p + 24)) / 16.f - kOx; t.y1 = float(ram.u16(p + 26)) / 16.f - kOy;
                    t.r = col[0]; t.g = col[1]; t.b = col[2]; t.a = col[3];
                    t.tex0 = tex0;
                    q.push_back(t);
                }
            } else {
                break;   // terminator (EOP, A+D of FBA) or garbage
            }
            if (tag & 0x8000) break;   // EOP
        }
        text.swap(q);
    }

    // Draws screen quads (text, ScreenPolys HUD) in game-screen coordinates (640 x 224) scaled to the central 4:3 strip
    // of `rw` x `rh` (outside 3D mode, over the scene). rtMain order: screenPolys (ScreenPolys::dmaData) and then text
    // (fontDmaFontData).
    void drawQuads(const std::vector<TextQuad> &quads, float rw, float rh) {
        if (quads.empty()) return;
        if (!textShaderTried_) {
            // The GS does texture * color / 0x80 (0x80 = 1.0) and saturates at 1.0: colors above 0x80 brighten the glyph. raylib only multiplies by
            // <= 1; the shader doubles RGB (the vertex color comes in as r/255, i.e. r/0x80 = 2 * r/255).
            textShaderTried_ = true;
            static const char *fs =
                "#version 330\n"
                "in vec2 fragTexCoord; in vec4 fragColor;\n"
                "uniform sampler2D texture0; uniform vec4 colDiffuse; out vec4 finalColor;\n"
                "void main() { vec4 t = texture(texture0, fragTexCoord) * fragColor * colDiffuse; finalColor = vec4(min(t.rgb * 2.0, 1.0), t.a); }\n";
            textShader_ = LoadShaderFromMemory(nullptr, fs);
        }
        const GsScreenRect gr = gsScreenRect(rw, rh);
        const float sx = gr.w / 640.f, sy = gr.h / 224.f;
        if (textShader_.id > 0) BeginShaderMode(textShader_);
        for (const TextQuad &t : quads) {
            const Texture2D tex = residentTexture(t.tex0);
            if (!tex.id) continue;
            const bool gain = textShader_.id > 0;   // without the shader the color stays <= 1.0 (doubles here)
            const int k = gain ? 1 : 2;
            const Color tint{uint8_t(std::min(255, t.r * k)), uint8_t(std::min(255, t.g * k)), uint8_t(std::min(255, t.b * k)), uint8_t(std::min(255, t.a * 2))};
            // Inverted UV (u1 < u0, v1 < v0: ScreenPolys mirrors and polyBuildPrim writes V = 0x80 - v): the GS walks from u0 to u1.
            // DrawTexturePro with negative width mirrors the range [x, x + |w|], so the origin is the smaller of the two.
            DrawTexturePro(tex, texelRect(tex, t.tex0, Rectangle{std::min(t.u0, t.u1), std::min(t.v0, t.v1), t.u1 - t.u0, t.v1 - t.v0}),
                           Rectangle{gr.x0 + t.x0 * sx, t.y0 * sy, (t.x1 - t.x0) * sx, (t.y1 - t.y0) * sy}, Vector2{0.f, 0.f}, 0.f, tint);
        }
        if (textShader_.id > 0) EndShaderMode();
    }

    // Fades in GS order: underText = true before the text (ShellRtLoop::endField dispatches the gsPkt queue before
    // fontDmaFontData: the token dialog comes out over the dark screen), false on top of everything (rtMain sends the queue after the text;
    // uiFadeScreen comes after fontDmaFontData; Shell::FadeScreen). A rectangle as wide as the game screen covers the whole frame (in
    // widescreen the fade darkens the whole window); a smaller one stays in the central 4:3 strip, like the text. In split screen (splitEdges) each
    // edge counts on its own: a view's fade and cinema bars (0..320 or 320..640) reach the window edge on its side.
    bool splitEdges = false;
    // WOTM_LOCALPLAYER: localView >= 0 = only the local player's view on screen (the other views' bars are left out)
    int localView = -1;
    int stripView = -1;   // 3-player split screen: the view that takes the whole bottom row (2), -1 = none
    // Bars over the enemies under the player's HUD: with `rects`, draw paints them in 2D right before the `hud` list, onto the texture it
    // draws to (tw x th, with the corner (ox, oy) of the rw x rh window), only those of view `view` (-1 = all, full screen); with barsUnderHud
    // drawScreenRects skips them. Without `rects` (replay, corpus) they come out in drawScreenRects, on top of everything.
    struct BarPass {
        const std::vector<ScreenRect> *rects = nullptr;
        float rw = 0.f, rh = 0.f;
        int view = -1;
        float ox = 0.f, oy = 0.f, tw = 0.f, th = 0.f;
    };
    BarPass barPass;
    bool barsUnderHud = false;

    void drawBarPass() {
        const BarPass &p = barPass;
        auto mine = [&](const ScreenRect &r) {
            return r.barView >= 0 && (p.view < 0 || r.barView == p.view) && (localView < 0 || r.barView == localView);
        };
        if (!p.rects || std::none_of(p.rects->begin(), p.rects->end(), mine)) return;
        rlDrawRenderBatchActive();
        rlDisableDepthTest();
        // matrices swapped and restored directly: rlgl has a single stack for both (and pushing the modelview turns on the CPU
        // transform), and the caller's BeginMode3D already pushed the projection. Orthographic for the texture (tw x th), with the window offset by (ox, oy)
        const Matrix keepProj = rlGetMatrixProjection(), keepView = rlGetMatrixModelview();
        Matrix ortho{};
        ortho.m0 = 2.f / p.tw;
        ortho.m5 = -2.f / p.th;
        ortho.m10 = -2.f;
        ortho.m12 = -1.f - 2.f * p.ox / p.tw;
        ortho.m13 = 1.f + 2.f * p.oy / p.th;
        ortho.m14 = -1.f;
        ortho.m15 = 1.f;
        Matrix ident{};
        ident.m0 = ident.m5 = ident.m10 = ident.m15 = 1.f;
        rlSetMatrixProjection(ortho);
        rlSetMatrixModelview(ident);
        BeginBlendMode(BLEND_ALPHA);
        const GsScreenRect gr = gsScreenRect(p.rw, p.rh);
        // The game pins the icon's center 8-16 px from the view's edge, and the bar is wider than the icon: instead of being clipped at the edge,
        // the whole bar (all rectangles of the same icon, ScreenRect::anchor) is pushed into its area
        struct Span { float x0 = 1e30f, x1 = -1e30f, lo = 0.f, hi = 0.f; };
        std::vector<Span> spans;
        for (const ScreenRect &r : *p.rects) {
            if (!mine(r) || r.anchor < 0) continue;
            if (size_t(r.anchor) >= spans.size()) spans.resize(size_t(r.anchor) + 1);
            Span &sp = spans[size_t(r.anchor)];
            float x0, x1;
            barSpan(gr, p.rw, r, x0, x1, sp.lo, sp.hi);
            sp.x0 = std::min(sp.x0, x0);
            sp.x1 = std::max(sp.x1, x1);
        }
        for (const ScreenRect &r : *p.rects) {
            if (!mine(r)) continue;
            float nudge = 0.f;
            if (r.anchor >= 0 && size_t(r.anchor) < spans.size()) {
                const Span &sp = spans[size_t(r.anchor)];
                if (sp.x0 < sp.lo) nudge = sp.lo - sp.x0;
                else if (sp.x1 > sp.hi) nudge = std::max(sp.hi - sp.x1, sp.lo - sp.x0);
            }
            drawBar(gr, p.rw, p.rh, r, nudge);
        }
        EndBlendMode();
        rlDrawRenderBatchActive();
        rlSetMatrixProjection(keepProj);
        rlSetMatrixModelview(keepView);
    }

    // Bar over the enemy: x in the window (rw wide) before clipping and the area it appears in (lo, hi: the view's half in split
    // screen, the window otherwise)
    void barSpan(const GsScreenRect &gr, float rw, const ScreenRect &r, float &x0, float &x1, float &lo, float &hi) const {
        // window px per GS px: the view's strip (gsScreenRect, at its aspect) over its width, with the view's height in the
        // math (quadrants: 112 lines in the whole window's strip). In split screen the strip is the half's (2:3, 320 GS px)
        const ViewCtx &cv = curView();
        const float k = cv.w > 0.f && cv.h > 0.f ? gr.w / cv.w * (cv.h / 224.f) : gr.w / 640.f;
        x0 = gr.x0 + r.x0 * k;
        x1 = gr.x0 + r.x1 * k;
        lo = 0.f;
        hi = rw;
        if (splitEdges && r.barView != stripView) {
            // each view goes out in its half with its own camera (drawView): the view's center in the game (160 or 480) goes to the center of the
            // half, not to where the whole window's 4:3 strip would put it (it ended up 1/4 of the widescreen margin to the side). In
            // quadrants the column applies (v & 1); the height is already the quadrant's in the game. The bottom strip for 3 players (stripView) has
            // the screen's width, as outside split screen
            const float half = 0.5f * rw, v = float(r.barView & 1);
            const float shift = v * half + 0.5f * half - (160.f + 320.f * v) * k - gr.x0;
            x0 += shift;
            x1 += shift;
            lo = v * half;
            hi = (v + 1.f) * half;
        }
    }

    // One bar over the enemy, in pixels of the rw x rh window; nudge = offset of the whole bar so it fits in the area (drawBarPass)
    void drawBar(const GsScreenRect &gr, float rw, float rh, const ScreenRect &r, float nudge = 0.f) {
        const uint8_t a = uint8_t(std::min(255, int(r.a) * 255 / 0x80));
        if (!a) return;
        const float sy = rh / 224.f;
        float x0, x1, lo, hi;
        barSpan(gr, rw, r, x0, x1, lo, hi);
        x0 += nudge;
        x1 += nudge;
        const float fx0 = x0, fw = x1 - x0;   // before clipping (the portrait takes the texture region from here)
        x0 = std::max(x0, lo);
        x1 = std::min(x1, hi);
        if (x1 <= x0) return;
        if (r.tex0) {   // portrait: the texture region left after clipping
            const Texture2D t = residentTexture(r.tex0);
            if (!t.id || fw <= 0.f) return;
            const float du = (r.u1 - r.u0) / fw;
            const float u0 = r.u0 + (x0 - fx0) * du, u1 = u0 + (x1 - x0) * du;
            DrawTexturePro(t, texelRect(t, r.tex0, Rectangle{u0, std::min(r.v0, r.v1), u1 - u0, r.v1 - r.v0}), Rectangle{x0, r.y0 * sy, x1 - x0, (r.y1 - r.y0) * sy},
                           Vector2{0.f, 0.f}, 0.f, Color{r.r, r.g, r.b, a});
            return;
        }
        DrawRectangleRec(Rectangle{x0, r.y0 * sy, x1 - x0, (r.y1 - r.y0) * sy}, Color{r.r, r.g, r.b, a});
    }

    void drawScreenRects(float rw, float rh, bool underText = false) {
        const GsScreenRect gr = gsScreenRect(rw, rh);
        const float sy = rh / 224.f;
        for (const ScreenRect &r : screenRects) {
            if (r.underText != underText) continue;
            const uint8_t a = uint8_t(std::min(255, int(r.a) * 255 / 0x80));
            if (!a) continue;
            if (r.barView >= 0 && localView >= 0 && r.barView != localView) continue;
            if (r.barView >= 0) {   // bar over the enemy (with barsUnderHud it already went out in draw, under the HUD)
                if (!barsUnderHud) drawBar(gr, rw, rh, r);
                continue;
            }
            const bool full = r.x0 <= 0.f && r.x1 >= 640.f;
            const bool left = full || (splitEdges && r.x0 <= 0.f), right = full || (splitEdges && r.x1 >= 640.f);
            const float x0 = left ? 0.f : gr.x0 + r.x0 * gr.w / 640.f, x1 = right ? rw : gr.x0 + r.x1 * gr.w / 640.f;
            DrawRectangleRec(Rectangle{x0, r.y0 * sy, x1 - x0, (r.y1 - r.y0) * sy}, Color{r.r, r.g, r.b, a});
        }
    }

private:
    std::unordered_map<uint64_t, Texture2D> fontTex_;   // drawQuads resident textures by TEX0 (id 0 = not decoded)
    Shader textShader_{};
    bool textShaderTried_ = false;

    // Resident texture of a screen quad (decodeResident): font atlas (T8H 512x512 from the stage's .RTX), HUD icons (T4HL).
    Texture2D residentTexture(uint64_t tex0) {
        const auto it = fontTex_.find(tex0);
        if (it != fontTex_.end()) return it->second;
        Texture2D t{};
        std::vector<uint32_t> px;
        uint32_t pal[256] = {}, w = 0, h = 0;
        const gs::Tex0 t0 = gs::Tex0::decode(tex0);
        uint32_t origAmax = 0;   // the original's raw alpha: the mod PNG reads alpha on its scale (texmods::loadPng)
        if (!texmods::Index::get().empty() && decodeResident(t0, px, w, h, true, pal))
            for (uint32_t c : px) origAmax = std::max(origAmax, c >> 24);
        if (modTexture(Ram{}, 0, t0, (tex0 >> 34) & 1, false, px, w, h, origAmax > 0x80) || decodeResident(t0, px, w, h, false, pal)) {
            Image img{px.data(), int(w), int(h), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            t = LoadTextureFromImage(img);
            if (t.id) { GenTextureMipmaps(&t); SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR); SetTextureWrap(t, TEXTURE_WRAP_CLAMP); }
        }
        fontTex_[tex0] = t;
        return t;
    }

    std::unordered_map<uint32_t, std::shared_ptr<MeshData>> cache_;
    std::unordered_map<uint64_t, Texture2D> tex_;
    int texFilter_ = 0;
    bool anisoSet_ = false;   // some anisotropic filter was already requested (without it, nothing to undo; without the extension raylib warns on every call)
    // Current filter on a tex_ texture (made or remade): mipmaps the first time the filter asks for them
    void applyTexFilter(Texture2D &t) {
        if (!t.id) return;
        if (texFilter_ <= 0) {
            rlTextureParameters(t.id, RL_TEXTURE_MIN_FILTER, RL_TEXTURE_FILTER_LINEAR);
            rlTextureParameters(t.id, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_LINEAR);
            if (anisoSet_) rlTextureParameters(t.id, RL_TEXTURE_FILTER_ANISOTROPIC, 1);
            return;
        }
        if (t.mipmaps <= 1) GenTextureMipmaps(&t);
        rlTextureParameters(t.id, RL_TEXTURE_MIN_FILTER, t.mipmaps > 1 ? RL_TEXTURE_FILTER_MIP_LINEAR : RL_TEXTURE_FILTER_LINEAR);
        rlTextureParameters(t.id, RL_TEXTURE_MAG_FILTER, RL_TEXTURE_FILTER_LINEAR);
        if (texFilter_ >= 2 || anisoSet_) {
            rlTextureParameters(t.id, RL_TEXTURE_FILTER_ANISOTROPIC, texFilter_ >= 2 ? texFilter_ : 1);
            anisoSet_ = true;
        }
    }
    std::vector<uint32_t> seen_;   // nodes already visited this frame (cycles / DAG)
    int lod_ = -1;
    float shadowLodD2_ = -1.f;   // >= 0: traversal for a shadow (collectShadows): LOD by this squared distance and no view culling
    uint32_t destrHist_[8] = {};   // diagnostics (WOTM_DESTRSTAT): destructibles by current state
    Shader alphaShader_{};
    VtxStream vs_;   // drawList vertices without rlgl's immediate mode (wotm_fx.hpp)
    int alphaRefLoc_ = -1;   // alpha test uniform (DrawItem::aref) in alphaShader_
    int fogLoc_[5] = {-1, -1, -1, -1, -1};   // fog uniforms in alphaShader_: fogEye, fogRow3, fogParams, fogColor, fogSplit
    bool alphaShaderTried_ = false;
    std::unordered_map<uint64_t, bool> partial_;   // texture (texKey) with semi-transparent alpha
    std::unordered_map<uint64_t, uint8_t> texAmax_;   // largest raw alpha of the texture (texKey): 0x80 = 1.0, up to 0xFF
    static constexpr size_t kShadowCacheN = 48;
    static constexpr float kShadowTol = 1.5f;   // world units (the silhouette has ~2 per texel)
    std::vector<ShadowCacheEntry> shadowCache_;   // shadow silhouettes by the entries' key (shadowKey), least recently used goes out
    uint64_t shadowTick_ = 0;
    std::vector<float> shadowCov_, shadowTmp_;    // silhouette coverage and blur (reused)
    std::vector<uint32_t> shadowPx_;
    static constexpr int kShadowRes = 128;
    uint32_t resSig_ = 0;   // signature of the fileStatus resources already loaded (loadResources)
    GameCamera cam_;   // camera of the last collect/blend (axes for the sprites)
    bool eyeOk_ = false;
    float eye_[3] = {0, 0, 0};
    std::shared_ptr<const uint8_t> fxRam_;   // Snapshot::fxRam of the displayed frame (or fxMix_ during interpolation)
    std::shared_ptr<uint8_t> fxMix_;         // particle shadow copy at the blended instant (showBlend, Particles::blendRam)
    // cameras A, B and the blended one from the last showBlend (no cut), with each one's focal: blendFlares and blendAnchors; show turns it off
    int attached_ = 0;   // objects the last showBlend carried with the camera (same relative pose in A and B)
    struct BlendView { bool on = false; int view = 0; float alpha = 1.f; M4 CA, CB, CI; interp::Focus fa, fb, fi; } hudMix_;
    size_t fxMixSize_ = 0;
    uint32_t atMat_ = 0;   // animOutput.atMat of the CHAR_INSTANCE containing the current node
    uint32_t animPkt_ = 0; // animPktPtr of the CHAR_INSTANCE containing the current node
    uint32_t animVal_ = 0; // animOutput.val (animation channel floats) of the CHAR_INSTANCE containing the current node
    uint32_t cs_ = 0;      // _cs being drawn (drawCs; 0 in the world and the sky): colorQuad in objectColor
    int lodIdx_ = 0;       // LOD most recently chosen on the way down
    int16_t aref_ = kGsAlphaRef;       // DrawItem::aref inherited from the nearest node with a TEST_1 ctx (CtxScope)
    bool zmsk_ = false;                // DrawItem::zmsk inherited from the nearest node with a ZBUF_1 ctx (CtxScope)
    uint8_t blend_ = kGsAlphaNormal;   // DrawItem::blend inherited from the nearest node with an ALPHA_1 ctx (CtxScope)
    float fade_ = 1.f;     // DrawItem::fade (hierLod's LOD cross-fade) on the way down
    bool lodX_ = false;    // DrawItem::lodX on the way down
    float spec_ = 0.f;     // DrawItem::spec: the qword 20 that is in VU1 memory at this point of hier's order

    // Level light table (lightDir, scratchpad 0x70001300 + 64 * level; VU1 q6-q8): rows = object axes, columns = lights
    // 0-2 and the half-vector, x32. Outside a SCALE it is paraL carried by the matrix (VU0 microprogram 0x1A8 does child = M * parent), and
    // vertexRgb reproduces it with the matrix rows normalized. hierScaleNode (flag +0xC = 0) and hierAnimScaleNode (non-uniform
    // scale: |sx - sy| or |sx - sz| >= 0.001) write something else: diag(1/sx, 1/sy, 1/sz) * parent (mathfMulMatrix3x3, which only writes
    // xyz and leaves the w of the unit matrix: half-vector 0, no specular), each column normalized (mathfNormalizeColumns; norm < 1e-7
    // becomes (0, 0, 1)) and times 32. With scale (1; 4; 5), Togera's two 0x311 pieces came out 15 to 20 brighter in native. Below
    // one of those nodes, lightOwn_ carries the table (lightObj_[light][axis]) and each local matrix rotates it like 0x1A8.
    bool lightOwn_ = false;
    float lightObj_[4][3] = {};
    struct LightScope {   // restores the light table on leaving the node
        Scene &s;
        bool own;
        float l[4][3];
        explicit LightScope(Scene &sc) : s(sc), own(sc.lightOwn_) { std::memcpy(l, sc.lightObj_, sizeof l); }
        ~LightScope() { s.lightOwn_ = own; std::memcpy(s.lightObj_, l, sizeof l); }
    };
    void lightRotate(const M4 &local) {   // child = local * parent (rows of local = child axes in parent space)
        if (!lightOwn_) return;
        float o[4][3];
        for (int i = 0; i < 4; ++i)
            for (int r = 0; r < 3; ++r)
                o[i][r] = local.m[r][0] * lightObj_[i][0] + local.m[r][1] * lightObj_[i][1] + local.m[r][2] * lightObj_[i][2];
        std::memcpy(lightObj_, o, sizeof o);
    }
    void lightScale(const M4 &m, float sx, float sy, float sz) {   // m = parent matrix (world)
        float base[4][3];
        if (lightOwn_) std::memcpy(base, lightObj_, sizeof base);
        else   // the parent's table: paraL in parent space (rows of m normalized, like vertexRgb)
            for (int r = 0; r < 3; ++r) {
                const float len = std::sqrt(m.m[r][0] * m.m[r][0] + m.m[r][1] * m.m[r][1] + m.m[r][2] * m.m[r][2]);
                for (int i = 0; i < 4; ++i)
                    base[i][r] = len > 1e-6f ? (m.m[r][0] * lights_.dir[i][0] + m.m[r][1] * lights_.dir[i][1] + m.m[r][2] * lights_.dir[i][2]) / len : 0.f;
            }
        const float s[3] = {sx, sy, sz};
        for (int i = 0; i < 3; ++i) {
            float v[3], n2 = 0.f;
            for (int r = 0; r < 3; ++r) { v[r] = base[i][r] / s[r]; n2 += v[r] * v[r]; }
            const float n = std::sqrt(n2);
            if (!(n >= 1e-7f)) { v[0] = v[1] = 0.f; v[2] = 1.f; }
            else for (float &x : v) x /= n;
            for (int r = 0; r < 3; ++r) lightObj_[i][r] = 32.f * v[r];
        }
        lightObj_[3][0] = lightObj_[3][1] = lightObj_[3][2] = 0.f;
        lightOwn_ = true;
    }

    // A GROUP's ctx (+4) is a GS state packet: an A+D tag (NLOOP at +0x10, one A+D register) and the (data, address) pairs from
    // +0x20; measured: ALPHA_1 (0x42), TEST_1 (0x47) and ZBUF_1. Translates into the children's state (-1 = ctx without the register,
    // inherits the parent's):
    //  - alpha: low byte of ALPHA_1 (A,B,C,D = bits 0-1, 2-3, 4-5, 6-7; see gsBlend) for DrawItem::blend. Measured: 0x44 regular, 0x48
    //    additive (Kineticlops), 0x42 darkens and shifts the hue, 0x46 and 0x81 (Kineticlops in another color);
    //  - aref: alpha test threshold from TEST_1 for DrawItem::aref. Without ATE (bit 0): no test (kGsAlphaNoTest; the power-up
    //    halo and the lamp-post glow, 0x5040C with vertex alpha 6 to 38, lost their gradient at threshold 8 and became hard-edged
    //    discs) and ALWAYS (ATST 1) too. GREATER (6) passes with A > AREF (bits 4-11), GEQUAL (5) with A >= AREF; the other ATSTs don't
    //    show up and stay at the shader's default (kGsAlphaRef).
    //    Kineticlops's 0x81 comes with 0x5040D (A > 0x40): (Cd - Cs) As doesn't add the background, and without the test the whole body would come out black.
    //  - zmsk: ZBUF_1.ZMSK (bit 32) for DrawItem::zmsk. The drive-in big-screen logo (tex 140/141, GROUP with ZTST GEQUAL and ZMSK) sits
    //    behind the screen (e8da50, no ctx: writes Z) and the GS hides it; without the screen's Z, native showed it on stage select.
    static constexpr int kRefAbsent = -1000;   // groupState: ctx without TEST_1 (inherits the parent's aref)
    static void groupState(const Ram &ram, uint32_t ctx, int &alpha, int &aref, int &zmsk) {
        alpha = zmsk = -1;
        aref = kRefAbsent;
        if (!ctx || !ram.ok(ctx, 0x20) || ram.u32(ctx + 0x14) != 0x10000000 || ram.u32(ctx + 0x18) != 0xE) return;
        const uint32_t n = std::min<uint32_t>(ram.u32(ctx + 0x10) & 0x7FFF, 8);
        if (!ram.ok(ctx, 0x20 + 16 * n)) return;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t d = ram.u32(ctx + 0x20 + 16 * i), reg = ram.u32(ctx + 0x28 + 16 * i) & 0xFF;
            if (reg == 0x42) alpha = int(d & 0xFF);
            else if (reg == 0x47) {
                const uint32_t atst = (d >> 1) & 7, ref = (d >> 4) & 0xFF;
                static const bool oldAte = std::getenv("WOTM_ATEOLD") != nullptr;   // diagnostics: ATE 0 with the default threshold (before 2026-10-17)
                aref = (!(d & 1) || atst == 1) ? (oldAte ? kGsAlphaRef : kGsAlphaNoTest) : atst == 6 ? int(ref) : atst == 5 ? std::max(int(ref) - 1, 0) : kGsAlphaRef;
            } else if (reg == 0x4E) zmsk = int(ram.u32(ctx + 0x24 + 16 * i) & 1);
        }
    }

    // GS state inherited on the way down. hierTraverseAsm swaps the current ctx ($s4, which starts at ctxGetDefaultGSCtx) at every node that
    // carries a non-null ctx, not only GROUP: TRANSLATE, ROTATE and SCALE at +4; ANIM_XFORM and ANIM_SCALE at +8 (count at +4). The
    // children inherit and the parent's comes back on exit ($s4 goes on the traversal stack). The explosion sphere (tex 146) sits below one of these
    // with ALPHA_1 0x48 (additive), with no GROUP with a ctx; native drew it with regular blending. Same with the power-up halo (tex 70):
    // it came out as a dark disc instead of brightening.
    struct CtxScope {
        Scene &s;
        uint8_t blend;
        int16_t aref;
        bool zmsk;
        CtxScope(Scene &sc, const Ram &ram, uint32_t ctx) : s(sc), blend(sc.blend_), aref(sc.aref_), zmsk(sc.zmsk_) {
            int b, r, z;
            groupState(ram, ctx & 0x0FFFFFFF, b, r, z);
            if (b >= 0) s.blend_ = uint8_t(b);
            if (r != kRefAbsent) s.aref_ = int16_t(r);
            if (z >= 0) s.zmsk_ = z != 0;
        }
        ~CtxScope() { s.blend_ = blend; s.aref_ = aref; s.zmsk_ = zmsk; }
    };

    // The OBJECT's UV offset, as hierTraverseAsm builds qword 4 of the VU1 header (vf19.xy, which the draw programs add
    // to the normalized UV): uOffset/vOffset (+0x20/+0x24) and, with uvScroll (bit 0x800 of the flags) inside a CHAR_INSTANCE, the
    // animation channels uOffIdx/vOffIdx (+0x30/+0x32; negative index = 0).
    void objectUvOffset(const Ram &ram, uint32_t node, float out[2]) const {
        out[0] = ram.f32(node + 0x20);
        out[1] = ram.f32(node + 0x24);
        if (!(ram.u16(node + 0xC) & 0x800) || !animVal_) return;
        for (int k = 0; k < 2; ++k) {
            const int16_t idx = ram.s16(node + 0x30 + 2 * k);
            out[k] = idx >= 0 ? ram.f32(animVal_ + 4 * uint32_t(idx)) : 0.f;
        }
    }

    // The OBJECT's vertex color multiplier, as hierTraverseAsm builds qword 5 of the VU1 header (vf31, which every draw program
    // applies to the vertex color): redAnim..alphaAnim (+0x40) or, inside a CHAR_INSTANCE, the channels rAnimIdx..aAnimIdx
    // (+0x34; negative index = the node's), times the colorQuad (+0x70) of the CS being drawn. Retail also multiplies alpha by the node's
    // fade (here in DrawItem::fade). Asm details (0x2085C8-0x2086D0):
    //  - the blue channel never comes from the animation: in place of the lwc1 of the animated value there is an swc1 of blueAnim (+0x48 stays);
    //  - colorQuad only applies with CsColor::isEnabled() (D_007B3040, copied to scratchpad +0x8C by pktCacheForAsm);
    //  - the asm also multiplies RGB by the animated ambient color (viewAnimAmbientRed/Green/Blue = ambient / 32 from viewSetAmbient)
    //    when the scratchpad flag +0x84 (viewAmbientChanged, written by pktCacheForAsm) is set, but the q5 that reaches
    //    VU1 never carries it: on UFO (ambient 15, 0.46875, flag set in RAM) the 1,309 headers of photo 2026-10-18_232542/002
    //    have q5 = (1, 1, 1, alpha), and PCSX2 shows the stage bright. Applied here, UFO came out 2.13 times darker. Left out.
    void objectColor(const Ram &ram, uint32_t node, float out[4]) const {
        const bool quad = cs_ && ram.u32(0x007B3040) != 0;
        const float *atDraw = nullptr;   // colorQuad at hier time (csColorAtDraw), instead of what's left in RAM
        if (quad)
            for (const CsColorAtDraw &c : csColorAtDraw)
                if (c.cs == cs_) { atDraw = c.q; break; }
        for (int k = 0; k < 4; ++k) {
            out[k] = ram.f32(node + 0x40 + 4 * k);
            const int16_t idx = ram.s16(node + 0x34 + 2 * k);
            if (animVal_ && idx >= 0 && k != 2) out[k] = ram.f32(animVal_ + 4 * uint32_t(idx));
            if (quad) out[k] *= atDraw ? atDraw[k] : ram.f32(cs_ + 0x70 + 4 * k);
            if (!std::isfinite(out[k])) out[k] = 1.f;
        }
    }

    // Environment map: pktAddVu1ObjAsm calls pktBldEnvMapDma when the flags (+0xC) have envMapped (0x2), envMapIsVideo (0x4) or
    // envMapCloakFx (0x8, which it sets when the _cs being drawn has cloakMe, +0xAA). Branch 2 (video, previous frame or CLOAK) with
    // envMapIsVideo, texId[0] (+0x50) = 0 or CLOAK; otherwise branch 1. k = envMapWeight (+0x5C; 0 = 16); for CLOAK, cloakWeight (+0xAC) + j/64,
    // j = 3 - (count & 7) with count & 8, otherwise (count & 7) - 3 (count = frame counter 0x6F88E0). CLOAK (env 3) also sets
    // ALPHA_1 0x44 (kGsAlphaNormal), replaces the color multiplier q5 with (2.59375; 2.59375; 2.59375; 1) and each slot's texture with the
    // previous frame's (texKey with DrawItem).
    void objectEnv(const Ram &ram, uint32_t node, DrawItem &it) const {
        const uint16_t f = ram.u16(node + 0xC);
        const bool cloak = cs_ && ram.s16(cs_ + 0xAA) != 0;
        if (!(f & 0x6) && !cloak) return;
        it.env = cloak ? 3 : ((f & 0x4) || ram.u16(node + 0x50) == 0) ? 2 : 1;
        it.envK = ram.f32(node + 0x5C);
        if (it.envK == 0.f) it.envK = 16.f;
        if (cloak) {
            const uint32_t n = ram.u32(wotm::sym::count_006F88E0);
            const int j = (n & 8) ? 3 - int(n & 7) : int(n & 7) - 3;
            it.envK = ram.f32(cs_ + 0xAC) + float(j) / 64.f;
            it.blend = kGsAlphaNormal;
            for (int c = 0; c < 3; ++c) it.color[c] = 2.59375f;
            it.color[3] = 1.f;
        }
    }

    // Matrix palette of a skinned object: animPktPtr[i] is a DMA chain whose 1st tag (ref) points to the palette block: 1 qword
    // of VIF (UNPACK V4-32 to VU1 qword 0x358) and the matrices, 4 qwords each, from +0x10. Usually 0x7d qwords (31
    // matrices); the Dragon's body uses its own 0x41 palette (16 matrices), sent before each sub-packet. Since the destination in
    // VU1 is the same, the bone code indexes both the same way. Index i is the OBJECT's at +0x68, as in pktAddVu1ObjAsm
    // (negative: the game sends the +4 packet, without a palette); the LOD index only matches when the character has one skinned object per
    // LOD (the Dragon has body and wings: with the LOD's, the body picked up another object's palette). The final boss (CAPITOL) uses 0x81
    // qwords (32 matrices, the whole palette the bone code can reach); with the old 0x7D limit, three of its four parts
    // had no palette and came out in the raw pose (legs in the air, dome out of place). Returns 0 if not found.
    static uint32_t palette(const Ram &ram, const DrawItem &it) {
        if (!it.animPkt) return 0;
        int idx = ram.ok(it.node + 0x68, 4) ? int(int32_t(ram.u32(it.node + 0x68))) : -1;
        if (idx < 0) idx = it.lodIdx;   // no index in the OBJECT: the old behavior
        if (idx < 0 || idx > 63) return 0;
        const uint32_t chain = ram.u32(it.animPkt + 4 * uint32_t(idx)) & 0x0FFFFFFF;
        if (!ram.ok(chain, 8) || ((ram.u32(chain) >> 28) & 7) != 3) return 0;
        const uint32_t qwc = ram.u32(chain) & 0xFFFF;
        if (qwc < 5 || qwc > 0x81 || (qwc - 1) % 4 != 0) return 0;
        const uint32_t block = ram.u32(chain + 4) & 0x0FFFFFFF;
        return ram.ok(block, 0x10 + 0x40 * 32) ? block : 0;
    }

    // Per-vertex lighting, like the VU1 program at 0x1010 (OBJECT with dirLight, bit 0x100 of the flags at +0xC): with the raw normal n (magnitude
    // ~127) rotated by bone x (skinned) and by the object, d_i = max(0, n.L_i / 4096) for the three parallel lights and the specular
    // half-vector (columns of paraL, x32; hier carries them into each object's space in lightDir) and
    //   color = min(255, vertex color * (sum of color_i * d_i) + specular * d_3^16)
    // with color_i in vu1LightColor (D_0025B238, color/128; header qwords 17-19) and specular = specIntensity (+0x6C) with specLightMe
    // (bit 0x2000), otherwise 0 (qword 20). Header qword 5, which also multiplies the vertex color, is 1.
    static ParaLights readParaLights(const Ram &ram) {
        ParaLights l;
        for (int i = 0; i < 4; ++i)
            for (int a = 0; a < 3; ++a) l.dir[i][a] = ram.f32(addr::paraL + 16 * a + 4 * i);
        for (int i = 0; i < 3; ++i)
            for (int c = 0; c < 3; ++c) l.col[i][c] = ram.f32(addr::vu1LightColor + 16 * i + 4 * c);
        return l;
    }
    ParaLights lights_;   // parallel lights from the last collect (vertexRgb)

    // Point lights (PointLights in pointLights 0x6DFCE0): hier calls GetClosestPts, which puts the up to 8 active lights nearest
    // the camera into VU0 memory (closest[] at +0x800, in order; position xyz + range in w from q30, color rgb + falloff in w from
    // q38) and hierCacheForAsm writes the camera position (viewGetTrans) to q28 and the count (+0x820) to the scratchpad.
    struct PointLightList { int n = 0; float pos[8][4] = {}; float col[8][4] = {}; float eye[3] = {}; };
    static PointLightList readPointLights(const Ram &ram) {
        PointLightList l;
        constexpr uint32_t pl = wotm::sym::pointLights;
        if (!ram.ok(pl, 0x8A0) || ram.u32(pl + 0x820) == 0) return l;
        const uint32_t cs = ram.u32(viewInfoAddr());
        if (!ram.ok(cs, 0x20)) return l;
        for (int a = 0; a < 3; ++a) l.eye[a] = ram.f32(cs + 0x10 + 4 * a);
        static const bool log = std::getenv("WOTM_PTLOG") != nullptr;
        if (log) {
            std::fprintf(stderr, "[ptl] count %u closest", ram.u32(pl + 0x820));
            for (int k = 0; k < 8; ++k) std::fprintf(stderr, " %d", int32_t(ram.u32(pl + 0x800 + 4 * k)));
            std::fprintf(stderr, " eye (%g %g %g)\n", l.eye[0], l.eye[1], l.eye[2]);
            for (int i = 0; i < 32; ++i) {
                const uint32_t p = pl + 0x40 * uint32_t(i);
                if (!ram.u32(p)) continue;
                std::fprintf(stderr, "[ptl] light %d f38 %u f30 %u pos (%g %g %g) range %g color (%g %g %g) falloff %g int %g\n", i, ram.u32(p + 0x38),
                             ram.u32(p + 0x30), ram.f32(p + 0x10), ram.f32(p + 0x14), ram.f32(p + 0x18), ram.f32(p + 0x1C), ram.f32(p + 0x20),
                             ram.f32(p + 0x24), ram.f32(p + 0x28), ram.f32(p + 0x2C), ram.f32(p + 0x34));
            }
        }
        // closest[0] < 0: GetClosestPts writes count 0 and leaves without clearing closest[1..7] (leftovers from earlier frames)
        if (int32_t(ram.u32(pl + 0x800)) < 0) return l;
        for (int k = 0; k < 8; ++k) {
            const int32_t i = int32_t(ram.u32(pl + 0x800 + 4 * k));
            if (i < 0 || i >= 32) continue;
            const uint32_t p = pl + 0x40 * uint32_t(i);
            for (int a = 0; a < 4; ++a) {
                l.pos[l.n][a] = ram.f32(p + 0x10 + 4 * a);
                l.col[l.n][a] = ram.f32(p + 0x20 + 4 * a);
            }
            ++l.n;
        }
        return l;
    }
    PointLightList pts_;   // from the last collect (objectPointLights)

    // Point lights of an OBJECT, like hierTraverseAsm and VU0 program 0x748: only a regular OBJECT (objType 0) with pointLight (0x80),
    // with lights in the frame and, inside a _cs, with ptLightMe (+0xA9). With R = rows 0-2 of the matrix and T = row 3 minus the camera (hier
    // stacks matrices relative to the camera), for each light in the list, in order, up to 3:
    //   v = R^T (L - camera);  touches if |T + center - v|^2 < range^2 + radius^2;  ptRel = T - v
    // The math mixes spaces (T isn't rotated), as in the game; with R = identity it's the light position in object space.
    void objectPointLights(const Ram &ram, uint32_t node, DrawItem &it) const {
        it.ptN = 0;
        if (!pts_.n) return;
        const uint16_t f = ram.u16(node + 0xC);
        if (!(f & 0x80) || (f & 0x60)) return;
        if (cs_ && ram.s8(cs_ + 0xA9) == 0) return;
        const M4 &m = it.m;
        float T[3], c[3];
        for (int a = 0; a < 3; ++a) {
            T[a] = m.m[3][a] - pts_.eye[a];
            c[a] = T[a] + ram.f32(node + 0x10 + 4 * a);
        }
        const float r = ram.f32(node + 0x1C);
        for (int k = 0; k < pts_.n && it.ptN < 3; ++k) {
            float d[3], v[3], s = 0.f;
            for (int a = 0; a < 3; ++a) d[a] = pts_.pos[k][a] - pts_.eye[a];
            for (int a = 0; a < 3; ++a) {
                v[a] = m.m[a][0] * d[0] + m.m[a][1] * d[1] + m.m[a][2] * d[2];
                s += (c[a] - v[a]) * (c[a] - v[a]);
            }
            static const bool log = std::getenv("WOTM_PTLOG") != nullptr;   // diagnostics: each sphere test
            if (log)
                std::fprintf(stderr, "[ptl] node %x light %d v (%g %g %g) T (%g %g %g) d2 %g lim %g\n", node, k, v[0], v[1], v[2], T[0], T[1], T[2], s,
                             pts_.pos[k][3] * pts_.pos[k][3] + r * r);
            if (!(s - (pts_.pos[k][3] * pts_.pos[k][3] + r * r) < 0.f)) continue;
            const int j = it.ptN++;
            for (int a = 0; a < 3; ++a) {
                it.ptRel[j][a] = T[a] - v[a];
                it.ptCol[j][a] = pts_.col[k][a];
            }
            it.ptFall[j] = pts_.col[k][3];
        }
    }

public:
    // Fog, like the VU1 draw programs and the GS: per vertex of the sections with PRIM.FGE,
    //   F = clamp(the OBJECT's fogBurnThru (+0x2C) + fogFA + fogFB / W, 0, fogMaxVal)   (W = row 3 of worldToScreenMat . (p - eye))
    // and the GS blends (F * color + (255 - F) * FOGCOL) / 255 (F = 255: no fog). Parameters from vu1FogParams; FOGCOL from fogColorCtx[4].
    static Fog readFog(const Ram &ram) {
        Fog f;
        f.fa = ram.f32(addr::vu1FogParams);
        f.maxVal = ram.f32(addr::vu1FogParams + 4);
        f.fb = ram.f32(addr::vu1FogParams + 12);
        const uint32_t c = ram.u32(addr::fogColorCtx + 0x20);
        for (int k = 0; k < 3; ++k) f.col[k] = float((c >> (8 * k)) & 0xFF) / 255.f;
        return f;
    }
    Fog fog_;   // from the last collect
    // F of a point (world) of a section with FGE; same math as the draw shader. row3/eye: row 3 of worldToScreenMat and the eye.
    float fogF(const float p[3], float burn, const float row3[4], const float eye[3]) const {
        const float w = row3[0] * (p[0] - eye[0]) + row3[1] * (p[1] - eye[1]) + row3[2] * (p[2] - eye[2]) + row3[3];
        if (!(w > 1e-3f)) return fog_.maxVal;
        return std::min(fog_.maxVal, std::max(0.f, burn + fog_.fa + fog_.fb / w));
    }

    // Environment map UV (DrawItem::env), as pktBldEnvMapDma builds q997-q999 and the VU1 program (0x0A78; 0x0C08 with clipping)
    // computes ST = R . n + (S + offset) * scale, Q = 1. n = raw normal / 4096 (ITOF12; for skinned, rotated by bone x and truncated
    // as in vertexRgb) carried to the world by the rows of the object matrix (with the _cs's scale); e = (right . n, down . n) with the
    // eye axes (worldCtx's weMat, equal to those taken from worldToScreenMat); S = the vertex's (X, Y) on the GS screen. With the ViewDef of
    // view 0 (viewInfo[0].def: w, h at +4/+6 and offX/offY at +0x10/+0x14):
    //   branch 1: s = 0.5 + k e.x + f (X - offX) / w,  t = 0.5 - k e.y - f (Y - offY) / h,  f = 1 - 0.05 |k|
    //   branch 2: s = k e.x + (X - offX + w/2) / 1024,  t = k e.y + (Y - offY + h/2) / 256   (texels of the 1024 x 256 screen; without the
    //           half texel of the odd field, viewIsOddScan, or the +320 of views > 0)
    struct EnvView { float W[4][4]; float eye[3]; float right[3], down[3]; float offX = 2048.f, offY = 2048.f, w = 640.f, h = 224.f; bool ok = false; };
    static EnvView envView(const Ram &ram, const float W[4][4], const float eye[3]) {
        EnvView ev;
        std::memcpy(ev.W, W, sizeof ev.W);
        std::memcpy(ev.eye, eye, sizeof ev.eye);
        float rl = 0.f, dl = 0.f;
        const float ocx = curView().cx, ocy = curView().cy;   // view center on the GS (row 0 = focal * right + cx * forward)
        for (int a = 0; a < 3; ++a) {
            ev.right[a] = W[0][a] - ocx * W[3][a];
            ev.down[a] = W[1][a] - ocy * W[3][a];
            rl += ev.right[a] * ev.right[a];
            dl += ev.down[a] * ev.down[a];
        }
        if (!(rl > 0.f && dl > 0.f)) return ev;
        rl = std::sqrt(rl); dl = std::sqrt(dl);
        for (int a = 0; a < 3; ++a) { ev.right[a] /= rl; ev.down[a] /= dl; }
        const uint32_t def = ram.u32(viewInfoAddr() + 4) & 0x0FFFFFFF;   // viewInfo[view].def
        if (ram.ok(def, 0x18) && ram.u16(def + 4) && ram.u16(def + 6)) {
            ev.w = ram.u16(def + 4); ev.h = ram.u16(def + 6);
            ev.offX = ram.f32(def + 0x10); ev.offY = ram.f32(def + 0x14);
        }
        ev.ok = true;
        return ev;
    }
    // Vertex normal in the world for envUv. bone: bone x (skinned) or null; m: object matrix.
    static void envNormal(const Vtx &v, const M4 *bone, const M4 &m, float out[3]) {
        float n[3] = {float(v.nx), float(v.ny), float(v.nz)};
        if (bone) {
            const float b[3] = {n[0], n[1], n[2]};
            for (int c = 0; c < 3; ++c) n[c] = std::trunc(b[0] * bone->m[0][c] + b[1] * bone->m[1][c] + b[2] * bone->m[2][c]);
        }
        for (int c = 0; c < 3; ++c) out[c] = (n[0] * m.m[0][c] + n[1] * m.m[1][c] + n[2] * m.m[2][c]) / 4096.f;
    }
    // False behind the camera (the packet UV stays).
    static bool envUv(uint8_t env, float k, const EnvView &ev, const float p[3], const float nw[3], float &u, float &v) {
        if (!env || !ev.ok) return false;
        const float d[3] = {p[0] - ev.eye[0], p[1] - ev.eye[1], p[2] - ev.eye[2]};
        const float q = ev.W[3][0] * d[0] + ev.W[3][1] * d[1] + ev.W[3][2] * d[2] + ev.W[3][3];
        if (!(q > 1e-3f)) return false;
        const float X = (ev.W[0][0] * d[0] + ev.W[0][1] * d[1] + ev.W[0][2] * d[2] + ev.W[0][3]) / q;
        const float Y = (ev.W[1][0] * d[0] + ev.W[1][1] * d[1] + ev.W[1][2] * d[2] + ev.W[1][3]) / q;
        const float ex = k * (ev.right[0] * nw[0] + ev.right[1] * nw[1] + ev.right[2] * nw[2]);
        const float ey = k * (ev.down[0] * nw[0] + ev.down[1] * nw[1] + ev.down[2] * nw[2]);
        if (env == 1) {
            const float f = 1.f - 0.05f * std::fabs(k);
            u = 0.5f + ex + f * (X - ev.offX) / ev.w;
            v = 0.5f - ey - f * (Y - ev.offY) / ev.h;
        } else {
            u = ex + (X - ev.offX + ev.w * 0.5f) / 1024.f;
            v = ey + (Y - ev.offY + ev.h * 0.5f) / 256.f;
        }
        return true;
    }
private:

    // obj: lights in object space (DrawItem::lightObj) when it has them; null = paraL rotated by the object matrix
    struct ObjLight { bool lit = false; float spec = 0.f; const DrawItem *pt = nullptr; const float (*obj)[3] = nullptr; };

    // Input for the vertex math of a drawList chunk (item + slot + pass), all already resolved on the window thread
    struct EmitIn {
        const DrawItem *it; const MeshData *md; uint32_t slot; bool multi;
        const M4 *pm; M4 skinM; bool fudge; float fzScale; float rowLen[3];
        ObjLight ol; float fade, fogBurn; unsigned tid; bool gsA, forceOpaque, premul; int mode; bool wire;
        bool gpuCull = false;   // all faces, with Sink::tri marking the culled ones (the GPU culls: resident arena)
    };
    struct EmitJob { EmitIn in; std::shared_ptr<const MeshData> keep; std::vector<M4> pmOwn; int buf = -1; };
    std::deque<EmitJob> jobs_;   // deferred chunks of the drawList in progress (reused)
    size_t nJobs_ = 0;
    // Destination for a deferred chunk's vertices: the same the VtxStream would do (current color, UV and normal; the normal normalized like
    // rlNormal3f), into its own buffer
    struct BufSink {
        std::vector<VtxStream::V> &out;
        VtxStream::V cur{};
        float last[3] = {0.f, 0.f, 0.f};
        void color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { cur.c[0] = r; cur.c[1] = g; cur.c[2] = b; cur.c[3] = a; }
        void tex(float u, float v) { cur.u = u; cur.v = v; }
        void shaderN(float burn, bool fge, bool gs, bool pm, int mode) {
            float n[3];
            shaderNormalVals(burn, fge, gs, pm, mode, n);
            if (n[0] == last[0] && n[1] == last[1] && n[2] == last[2]) return;
            last[0] = n[0]; last[1] = n[1]; last[2] = n[2];
            const float length = sqrtf(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            if (length != 0.0f) { const float il = 1.0f / length; n[0] *= il; n[1] *= il; n[2] *= il; }
            cur.nx = n[0]; cur.ny = n[1]; cur.nz = n[2];
        }
        void vertex(float x, float y, float z) { cur.x = x; cur.y = y; cur.z = z; out.push_back(cur); }
        void tri(bool) {}
    };

    // ---- resident arena (VtxStream::arenaAlloc): drawList chunks that don't change between views and frames
    // Everything that goes into the vertex math of a static chunk (emitVerts with no bones, environment map, fudge or
    // sprites). Bytes zeroed before filling (compared with memcmp).
    struct ResKey {
        const MeshData *md; uint32_t sig, node, slot; int32_t mode;
        float m[16], color[4], fade, fogBurn, uvOff[2], spec;
        float lobj[4][3], ldir[4][3], lcol[3][3];
        float ptRel[3][3], ptCol[3][3], ptFall[3];   // point lights (pointRgb)
        uint8_t lit, ownLight, forceOpaque, premul, premulShader, texOn, multi, ptN;
    };
    struct ResRun { uint32_t start, n; bool cull; };
    struct ResChunk { ResKey key; long off = 0; std::vector<ResRun> runs; std::shared_ptr<const MeshData> keep; };
    // Arena sink: like BufSink, with the culled and unculled face ranges in order
    struct ResSink : BufSink {
        std::vector<ResRun> &runs;
        ResSink(std::vector<VtxStream::V> &o, std::vector<ResRun> &r) : BufSink{o}, runs(r) {}
        void tri(bool cull) {
            if (runs.empty() || runs.back().cull != cull) runs.push_back({uint32_t(out.size()), 0, cull});
            runs.back().n += 3;
        }
    };
    std::unordered_map<uint64_t, ResChunk> resChunks_;
    std::unordered_map<uint64_t, uint64_t> resSeen_;   // key -> redraw it appeared in (a chunk only after repeating in a following redraw)
    std::vector<VtxStream::V> resTmp_;
    bool resReset_ = false;   // arena full: resets at the start of the next drawList (with the queue empty)
    // Draws the chunk from the arena (true) or leaves it to the regular path (false: new key, arena full, WOTM_RESIDENT=0)
    bool residentDraw(const EmitIn &in, const std::shared_ptr<const MeshData> &keep) {
        // WOTM_RESIDENT=0 turns it off; =2 puts it in the arena on the first time (replay and corpus, which don't repeat redraws)
        static const int mode = [] { const char *e = std::getenv("WOTM_RESIDENT"); return e ? std::atoi(e) : 1; }();
        static const bool diag = std::getenv("WOTM_IDCOLOR") || std::getenv("WOTM_NOVCOL") || std::getenv("WOTM_SUBPKT") || std::getenv("WOTM_UVNEG");
        if (mode == 0 || diag) return false;
        ResKey k;
        std::memset(&k, 0, sizeof k);
        const DrawItem &it = *in.it;
        k.md = in.md; k.sig = in.md->sig; k.node = it.node; k.slot = in.slot; k.mode = in.mode;
        std::memcpy(k.m, &it.m, sizeof k.m);
        std::memcpy(k.color, it.color, sizeof k.color);
        k.fade = in.fade; k.fogBurn = in.fogBurn; k.uvOff[0] = it.uvOff[0]; k.uvOff[1] = it.uvOff[1];
        k.lit = in.ol.lit; k.forceOpaque = in.forceOpaque; k.premul = in.premul; k.premulShader = g_premulShader;
        k.texOn = in.tid != 0; k.multi = in.multi;
        if (in.ol.pt) {
            const DrawItem &p = *in.ol.pt;
            k.ptN = p.ptN;
            std::memcpy(k.ptRel, p.ptRel, sizeof k.ptRel);
            std::memcpy(k.ptCol, p.ptCol, sizeof k.ptCol);
            std::memcpy(k.ptFall, p.ptFall, sizeof k.ptFall);
        } else if (in.ol.lit) {
            k.spec = in.ol.spec;
            if (in.ol.obj) { k.ownLight = 1; std::memcpy(k.lobj, in.ol.obj, sizeof k.lobj); }
            else { std::memcpy(k.ldir, lights_.dir, sizeof k.ldir); std::memcpy(k.lcol, lights_.col, sizeof k.lcol); }
        }
        static_assert(sizeof(ResKey) % 8 == 0, "ResKey in 8-byte words");
        uint64_t h = 0x243F6A8885A308D3ull;   // 8 bytes at a time
        for (size_t i = 0; i < sizeof k; i += 8) {
            uint64_t w;
            std::memcpy(&w, reinterpret_cast<const uint8_t *>(&k) + i, 8);
            h = (h ^ w) * 0x9E3779B97F4A7C15ull;
            h ^= h >> 29;
        }
        auto f = resChunks_.find(h);
        if (f == resChunks_.end() || std::memcmp(&f->second.key, &k, sizeof k) != 0) {
            auto s = resSeen_.find(h);
            if (mode != 2 && (s == resSeen_.end() || s->second == redrawId)) {   // first time (or only in this redraw): regular path
                if (s == resSeen_.end()) {
                    if (resSeen_.size() > 200000) resSeen_.clear();
                    resSeen_.emplace(h, redrawId);
                }
                return false;
            }
            resTmp_.clear();
            ResChunk c;
            c.key = k;
            c.keep = keep;
            EmitIn ein = in;
            ein.gpuCull = true;
            ResSink sink(resTmp_, c.runs);
            emitVerts(Ram{}, ein, EnvView{}, SpriteView{}, sink);
            c.off = vs_.arenaAlloc(resTmp_.size());
            if (c.off < 0) { resReset_ = true; return false; }
            vs_.arenaUpload(c.off, resTmp_.data(), resTmp_.size());
            if (drawProfOn()) drawProf().resUp += long(resTmp_.size());
            f = resChunks_.insert_or_assign(h, std::move(c)).first;
        }
        for (const ResRun &r : f->second.runs) {
            vs_.resident(f->second.off + long(r.start), int(r.n), r.cull);
            if (drawProfOn()) drawProf().resident += long(r.n);
        }
        return true;
    }
    // Vertices of a drawList chunk (the slot's triangles and the object's sprites). Read-only: runs on the helper threads (JobPool)
    template <class Sink> void emitVerts(const Ram &ram, const EmitIn &in, const EnvView &envV, const SpriteView &sprView, Sink &sink) const {
        const DrawItem &it = *in.it;
        const MeshData *md = in.md;
        const auto &t = md->tris;
        const uint32_t slot = in.slot;
        const M4 *pm = in.pm;
        const M4 &skinM = in.skinM;
        const bool wire = in.wire, gsA = in.gsA;
        const unsigned tid = in.tid;
        static const bool idcMode = std::getenv("WOTM_IDCOLOR") != nullptr;   // once: getenv per vertex is slow on Windows
        static const bool idcNode = idcMode && std::atoi(std::getenv("WOTM_IDCOLOR")) == 2;   // =2: node >> 4 in RGB, no blending or fog
        static const bool noVcol = std::getenv("WOTM_NOVCOL") != nullptr;
        static const int onlySub = std::getenv("WOTM_SUBPKT") ? std::atoi(std::getenv("WOTM_SUBPKT")) : -1;
        static const char *un = std::getenv("WOTM_UVNEG");   // diagnostics: WOTM_UVNEG=u|v|uv flips the UV sign
        for (size_t k = 0; k + 2 < t.size(); k += 3) {
            if (in.multi && t[k].slot != slot) continue;   // only this slot's sections
            if (onlySub >= 0 && md->skinned && t[k].sub != onlySub) continue;
            float p[3][3];
            for (int c = 0; c < 3; ++c) {
                if (pm) skinVertexPM(pm, skinM, t[k + c], p[c]);
                else xform(it.m, t[k + c].x, t[k + c].y, t[k + c].z, p[c]);
                if (in.fudge) for (int a = 0; a < 3; ++a) p[c][a] = eye_[a] + (p[c][a] - eye_[a]) * in.fzScale;
            }
            if (t[k].cull && !wire && !in.gpuCull && backFacing(p, eye_)) continue;
            sink.tri(t[k].cull != 0);
            float eu[3], ev[3];   // environment map UV per vertex
            bool envOk[3] = {false, false, false};
            if (it.env && tid) {
                for (int c = 0; c < 3; ++c) {
                    float nw[3];
                    envNormal(t[k + c], pm ? &pm[(t[k + c].cx & 0x7F) / 4] : nullptr, pm ? skinM : it.m, nw);
                    envOk[c] = envUv(it.env, it.envK, envV, p[c], nw, eu[c], ev[c]);
                }
            }
            for (int c = 0; c < 3; ++c) {
                const int cs[2] = {c, (c + 1) % 3};
                for (int e = 0; e < (wire ? 2 : 1); ++e) {
                    const Vtx &v = t[k + cs[e]];
                    bool gsV = gsA;   // shader GS mode on this vertex (debug colors go without it)
                    if (idcNode) { gsV = false; const uint32_t n = it.node >> 4; sink.color(uint8_t(n), uint8_t(n >> 8), uint8_t(n >> 16), 255); }
                    else if (idcMode) { gsV = false; const uint32_t tx = objectTexId(ram, it.node, slot); sink.color(uint8_t(tx & 255), uint8_t(tx >> 8), 128, 255); }
                    else if (v.isAdc) { gsV = false; sink.color(255, 0, 0, 255); }   // WOTM_SHOWADC
                    else if (noVcol && md->skinned) { gsV = false; sink.color(255, 255, 255, 255); }   // diagnostics
                    else {
                        uint8_t rgb[3];
                        vertexRgb(in.ol, it.color, pm ? &pm[(v.cx & 0x7F) / 4] : nullptr, pm ? skinM : it.m, v, rgb, in.rowLen);
                        // raw color and alpha in GS mode (the shader does texel * Cv / 128 and At * Av / 16384); for the previous frame and the movie,
                        // the old way (color doubled and capped at 1.0, alpha 4a/255)
                        const float va = v.a * in.fade * (gsA ? 1.f : 4.f);
                        const int kk = gsA ? 1 : 2;
                        sink.color(uint8_t(std::min(255, rgb[0] * kk)), uint8_t(std::min(255, rgb[1] * kk)), uint8_t(std::min(255, rgb[2] * kk)),
                                   in.forceOpaque ? uint8_t(255) : uint8_t(std::min(255.f, va)));
                    }
                    const float tu = envOk[cs[e]] ? eu[cs[e]] : v.u + it.uvOff[0], tv = envOk[cs[e]] ? ev[cs[e]] : v.v + it.uvOff[1];
                    // GS texels on the 640x224 screen, which in widescreen is the central 4:3 strip of the previous frame (gsScreenRect)
                    if (tid && tid == screenId_) sink.tex(0.5f - 0.5f / wide + tu * (1024.f / 640.f) / wide, 1.f - tv * (256.f / 224.f));
                    else if (tid && envOk[cs[e]]) sink.tex(tu, tv);
                    else if (tid) { const float su = (un && std::strchr(un, 'u')) ? -1.f : 1.f, sv = (un && std::strchr(un, 'v')) ? -1.f : 1.f; sink.tex(su * (v.u + it.uvOff[0]), sv * (v.v + it.uvOff[1])); }
                    sink.shaderN(in.fogBurn, v.fge && !idcNode, gsV, in.premul, in.mode);   // fog and GS modulation in the draw shader
                    sink.vertex(p[cs[e]][0], p[cs[e]][1], p[cs[e]][2]);
                }
            }
        }
        for (const ObjSprite &s : md->sprites) {
            if (in.multi && s.slot != slot) continue;
            float c[4][3];
            if (!objSpriteCorners(sprView, it.m, s, c)) continue;
            if (in.fudge) for (auto &q : c) for (int a = 0; a < 3; ++a) q[a] = eye_[a] + (q[a] - eye_[a]) * in.fzScale;
            const uint8_t cr = uint8_t(std::min(255.f, s.r * it.color[0])), cg = uint8_t(std::min(255.f, s.g * it.color[1])),
                          cb = uint8_t(std::min(255.f, s.b * it.color[2])), ca = uint8_t(std::min(255.f, s.a * in.fade));
            // ST like the GS (repeats): shifts the pair to >= 0, so as not to depend on raylib's texture wrap mode
            const float u0 = s.u0 + it.uvOff[0], u1 = s.u1 + it.uvOff[0], v0 = s.v0 + it.uvOff[1], v1 = s.v1 + it.uvOff[1];
            const float du = std::floor(std::min(u0, u1)), dv = std::floor(std::min(v0, v1));
            const float cu[4] = {u0 - du, u1 - du, u1 - du, u0 - du}, cv[4] = {v0 - dv, v0 - dv, v1 - dv, v1 - dv};
            static const int triIdx[6] = {0, 1, 2, 0, 2, 3}, edgeIdx[8] = {0, 1, 1, 2, 2, 3, 3, 0};
            const int *ix = wire ? edgeIdx : triIdx;
            for (int q = 0; q < (wire ? 8 : 6); ++q) {
                const int k = ix[q];
                sink.color(cr, cg, cb, ca);
                if (tid) sink.tex(cu[k], cv[k]);
                sink.shaderN(in.fogBurn, s.fge, gsA, in.premul, in.mode);
                sink.vertex(c[k][0], c[k][1], c[k][2]);
            }
        }
    }
    // The environment map program (DrawItem::env) doesn't apply the parallel light, even with dirLight. pktAddVu1ObjAsm picks the
    // point light program (0x15D8, table +0x16C) before the parallel light one: with a point light, the parallel one is left out.
    static ObjLight objectLight(const Ram &ram, const DrawItem &it) {
        const uint16_t f = ram.u16(it.node + 0xC);
        if (it.env) return {};
        if (it.ptN) return {false, 0.f, &it};
        return {(f & 0x100) != 0, it.spec, nullptr, it.ownLight ? it.lightObj : nullptr};
    }

    // Color with point lights, like VU1 program 0x15D8: p = vertex position in object space, n = raw normal / 128, and for
    // each light k, d_k = p + ptRel_k (vertex minus light) and
    //   f_k = min(0.5; max(0, -d_k . n) * falloff_k / |d_k|^2)
    //   color = min(255, vertex color * vf31 + sum of color_k * f_k)
    // No parallel light or specular.
    static void pointRgb(const DrawItem &it, const float vc[3], const Vtx &v, uint8_t out[3]) {
        const float n[3] = {float(v.nx) / 128.f, float(v.ny) / 128.f, float(v.nz) / 128.f};
        const float p[3] = {v.x, v.y, v.z};
        float acc[3] = {vc[0], vc[1], vc[2]};
        for (int k = 0; k < it.ptN; ++k) {
            float d[3], d2 = 0.f, dot = 0.f;
            for (int a = 0; a < 3; ++a) {
                d[a] = p[a] + it.ptRel[k][a];
                d2 += d[a] * d[a];
                dot -= d[a] * n[a];
            }
            if (!(d2 > 0.f)) continue;
            const float fk = std::min(0.5f, std::max(0.f, dot) * it.ptFall[k] / d2);
            for (int c = 0; c < 3; ++c) acc[c] += it.ptCol[k][c] * fk;
        }
        for (int c = 0; c < 3; ++c) out[c] = uint8_t(std::min(255.f, std::max(0.f, acc[c])));
    }

    // Vertex color in GS units (0x80 = 1.0): the packet's times the object multiplier (DrawItem::color, vf31) or, for
    // lit ones, the VU1 lighting. bone: the vertex's bone x (skinned) or null.
    // Length of each row 0-2 of m (vertexRgb normalizes the rows with it): per item, outside the vertex loop
    static void rowLengths(const M4 &m, float len[3]) {
        for (int r = 0; r < 3; ++r) len[r] = std::sqrt(m.m[r][0] * m.m[r][0] + m.m[r][1] * m.m[r][1] + m.m[r][2] * m.m[r][2]);
    }
    // rowLen: rowLengths(m) already computed (same result; without it, computes here)
    void vertexRgb(const ObjLight &ol, const float mul[4], const M4 *bone, const M4 &m, const Vtx &v, uint8_t out[3], const float *rowLen = nullptr) const {
        const float vc[3] = {v.r * mul[0], v.g * mul[1], v.b * mul[2]};
        if (ol.pt) {
            pointRgb(*ol.pt, vc, v, out);
            return;
        }
        if (!ol.lit) {
            for (int c = 0; c < 3; ++c) out[c] = uint8_t(std::min(255.f, std::max(0.f, vc[c])));
            return;
        }
        float n[3] = {float(v.nx), float(v.ny), float(v.nz)};
        if (bone) {   // VU1's skinning loop (0x3D0) writes the rotated normal with FTOI12 (truncates to 1/4096) and lighting reads it with ITOF12
            const float b[3] = {n[0], n[1], n[2]};
            for (int c = 0; c < 3; ++c) n[c] = std::trunc(b[0] * bone->m[0][c] + b[1] * bone->m[1][c] + b[2] * bone->m[2][c]);
        }
        float d[4];
        if (ol.obj) {   // lights already in object space (below a SCALE: Scene::lightScale), against the object's normal
            for (int i = 0; i < 4; ++i) d[i] = std::max(0.f, (n[0] * ol.obj[i][0] + n[1] * ol.obj[i][1] + n[2] * ol.obj[i][2]) / 4096.f);
        } else {
            float w[3] = {0, 0, 0};   // in the world, with the object rows normalized (no SCALE on the path, the matrix is a rotation)
            for (int r = 0; r < 3; ++r) {
                const float len = rowLen ? rowLen[r] : std::sqrt(m.m[r][0] * m.m[r][0] + m.m[r][1] * m.m[r][1] + m.m[r][2] * m.m[r][2]);
                if (len > 1e-6f)
                    for (int c = 0; c < 3; ++c) w[c] += n[r] * m.m[r][c] / len;
            }
            for (int i = 0; i < 4; ++i) d[i] = std::max(0.f, (w[0] * lights_.dir[i][0] + w[1] * lights_.dir[i][1] + w[2] * lights_.dir[i][2]) / 4096.f);
        }
        float s = d[3];
        for (int k = 0; k < 4; ++k) s *= s;   // ^16
        for (int c = 0; c < 3; ++c) {
            const float x = vc[c] * (lights_.col[0][c] * d[0] + lights_.col[1][c] * d[1] + lights_.col[2][c] * d[2]) + ol.spec * s;
            out[c] = uint8_t(std::min(255.f, std::max(0.f, x)));
        }
    }

    // Skinning, like the VU1 microcode (disassembled with tools/vu1dis.py; loop at 0x3C8): three bones per vertex.
    // The bone codes (low 8 bits of x, y, z; & 0x7F = qword address of the matrix in the palette) pick 3 matrices and the weights
    // come from the float w: w1 = w, w2 = frac(w * 2048) (low mantissa), w3 = 1 - w1 - w2. pos = w1*Mx*p + w2*My*p + w3*Mz*p.
    // Palette slot = (code & 0x7F) / 4. The normal only rotates by bone x (vertexRgb).
    static void skinVertexPM(const M4 *pm, const M4 &m, const Vtx &v, float out[3]) {
        float w1 = v.w, w2 = 0.f, w3 = 0.f;
        { const float s = v.w * 2048.f; w2 = s - std::trunc(s); w3 = 1.f - w1 - w2; }
        const uint8_t codes[3] = {v.cx, v.cy, v.cz};
        const float ws[3] = {w1, w2, w3};
        float q[3] = {0, 0, 0};
        for (int k = 0; k < 3; ++k) {
            if (ws[k] == 0.f) continue;
            float a[3];
            xform(pm[(codes[k] & 0x7F) / 4], v.x, v.y, v.z, a);
            for (int c = 0; c < 3; ++c) q[c] += a[c] * ws[k];
        }
        xform(m, q[0], q[1], q[2], out);
    }

    static void skinVertex(const Ram &ram, uint32_t pal, const M4 &m, const Vtx &v, float out[3]) {
        float w1 = v.w, w2 = 0.f, w3 = 0.f;
        {   // 3 bones: w1 = w, w2 = frac(w * 2048), w3 = the remainder (VU1 microcode)
            const float s = v.w * 2048.f;
            w2 = s - std::trunc(s);
            w3 = 1.f - w1 - w2;
        }
        const uint8_t codes[3] = {v.cx, v.cy, v.cz};
        const float ws[3] = {w1, w2, w3};
        float q[3] = {0, 0, 0};
        for (int k = 0; k < 3; ++k) {
            if (ws[k] == 0.f) continue;
            float a[3];
            xform(readM4(ram, pal + 0x10 + 0x40 * ((codes[k] & 0x7F) / 4)), v.x, v.y, v.z, a);
            for (int c = 0; c < 3; ++c) q[c] += a[c] * ws[k];
        }
        xform(m, q[0], q[1], q[2], out);
    }

    // ---- CONTROL (hierCtrlNode, 0x2048F0): buildings with a "transparent" version when they block the view of the monster ----
    // Node: +4 child1, +8 child2, +0xC numVolumes, +0xE volType, +0x10 sphere (center, radius at +0x1C), +0x20 BSP volumes (pointers).
    // volType 0: if the eye is NOT in air (leaf != 0x200) in some volume -> child2 (camera inside the building). volType != 0: for each
    // aim point (g_cameraLosPoints: unit direction from the camera to the monster + distance in w) the eye->point segment must not
    // cross solid (leaf != 0x200) in any volume; if it does -> child2 (which may be null = the building disappears).
    static constexpr uint32_t kLosPoints = wotm::sym::g_cameraLosPoints, kLosPerView = wotm::sym::g_cameraLosPointsPerView;

    static void toLocal(const M4 &m, const float w[3], float out[3]) {
        for (int c = 0; c < 3; ++c) {
            const float *r = m.m[c];
            const float n2 = r[0] * r[0] + r[1] * r[1] + r[2] * r[2];
            out[c] = n2 > 1e-9f ? ((w[0] - m.m[3][0]) * r[0] + (w[1] - m.m[3][1]) * r[1] + (w[2] - m.m[3][2]) * r[2]) / n2 : 0.f;
        }
    }

    // Leaf where the point lands (0x200 = air), same as bspPointTest.
    static int bspPoint(const Ram &ram, uint32_t b, const float p[3]) {
        int ret = 0x200;
        for (int guard = 0; b && ram.ok(b, 32) && guard < 64; ++guard) {
            const float d = ram.f32(b + 0x10) * p[0] + ram.f32(b + 0x14) * p[1] + ram.f32(b + 0x18) * p[2] - ram.f32(b + 0x1C);
            const bool neg = d < 0.f;
            ret = neg ? ram.u16(b + 0xE) : ram.u16(b + 0xC);
            b = neg ? ram.u32(b + 4) : ram.u32(b);
        }
        return ret;
    }

    // True if the segment a->c crosses any leaf that isn't air.
    static bool bspSegHit(const Ram &ram, uint32_t b, const float a[3], const float c[3], int depth = 0) {
        if (depth > 64) return false;
        if (!b || !ram.ok(b, 32)) return false;
        const float n[3] = {ram.f32(b + 0x10), ram.f32(b + 0x14), ram.f32(b + 0x18)}, dd = ram.f32(b + 0x1C);
        const float d0 = n[0] * a[0] + n[1] * a[1] + n[2] * a[2] - dd, d1 = n[0] * c[0] + n[1] * c[1] + n[2] * c[2] - dd;
        const uint32_t front = ram.u32(b), back = ram.u32(b + 4);
        auto side = [&](bool neg, const float p[3], const float q[3]) -> bool {
            const uint32_t kid = neg ? back : front;
            if (kid) return bspSegHit(ram, kid, p, q, depth + 1);
            return (neg ? ram.u16(b + 0xE) : ram.u16(b + 0xC)) != 0x200;   // leaf
        };
        if (d0 >= 0.f && d1 >= 0.f) return side(false, a, c);
        if (d0 < 0.f && d1 < 0.f) return side(true, a, c);
        const float t = d0 / (d0 - d1);
        const float mid[3] = {a[0] + (c[0] - a[0]) * t, a[1] + (c[1] - a[1]) * t, a[2] + (c[2] - a[2]) * t};
        return d0 >= 0.f ? (side(false, a, mid) || side(true, mid, c)) : (side(true, a, mid) || side(false, mid, c));
    }

    uint32_t ctrlChild(const Ram &ram, uint32_t node, const M4 &m) const {
        const uint32_t c1 = ram.u32(node + 4), c2 = ram.u32(node + 8);
        if (!eyeOk_) return c1;
        const uint32_t nvol = std::min<uint32_t>(ram.u8(node + 0xC), 16), vtype = ram.u8(node + 0xE);
        if (!nvol || !ram.ok(node, 0x20 + 4 * nvol)) return c1;
        float eyeL[3];
        toLocal(m, eye_, eyeL);
        bool visible = true;
        if (vtype == 0) {
            for (uint32_t i = 0; i < nvol && visible; ++i)
                if (bspPoint(ram, ram.u32(node + 0x20 + 4 * i), eyeL) != 0x200) visible = false;
        } else if (nativeEye.on) {   // native camera: the segment from the eye to its monster
            float pl[3];
            toLocal(m, nativeEye.target, pl);
            for (uint32_t i = 0; i < nvol && visible; ++i)
                if (bspSegHit(ram, ram.u32(node + 0x20 + 4 * i), eyeL, pl)) visible = false;
        } else {
            const uint32_t v = uint32_t(curView().view);
            const int nlos = std::min<int>(int(ram.u32(kLosPerView + 4 * v)), 2);
            for (int k = 0; k < nlos && visible; ++k) {
                const uint32_t pa = kLosPoints + 0x20 * v + 16 * k;
                const float len = ram.f32(pa + 12);
                if (!(len > 0.f) || len > 1e6f) continue;
                const float pw[3] = {eye_[0] + ram.f32(pa) * len, eye_[1] + ram.f32(pa + 4) * len, eye_[2] + ram.f32(pa + 8) * len};
                float pl[3];
                toLocal(m, pw, pl);
                for (uint32_t i = 0; i < nvol && visible; ++i)
                    if (bspSegHit(ram, ram.u32(node + 0x20 + 4 * i), eyeL, pl)) visible = false;
            }
        }
        if (!visible) ++ghosted;
        return visible ? c1 : c2;
    }

    // hierTraverseAsm's sphere culling: VU0 program 0x0 tests the node's sphere (+0x10 center, +0x1C radius) against the view
    // planes (Particles::inViewVolume); outside any plane, the OBJECT doesn't go to VU1 (unless nofov, bit 0 of +0xC) and the GROUP
    // skips its children (with g_doFOVGroups and without noFOV). The game tests in node space with column-normalized planes; in the world the
    // radius goes by the matrix's largest scale.
    bool outOfView(const Ram &ram, uint32_t node, const M4 &m) const {
        if (!fovCull) return false;
        float c[3], s2 = 0.f;
        xform(m, ram.f32(node + 0x10), ram.f32(node + 0x14), ram.f32(node + 0x18), c);
        for (int i = 0; i < 3; ++i) s2 = std::max(s2, m.m[i][0] * m.m[i][0] + m.m[i][1] * m.m[i][1] + m.m[i][2] * m.m[i][2]);
        static constexpr float kNoBox[3] = {0.f, 0.f, 0.f};
        return !Particles::inViewVolume(ram, c, kNoBox, ram.f32(node + 0x1C) * std::sqrt(s2) * cullMargin, wide);
    }

    bool already(uint32_t n) {
        // small list ordered by insertion; the game has up to a few thousand nodes
        for (uint32_t s : seen_) if (s == n) return true;
        seen_.push_back(n);
        return false;
    }

    void kids(const Ram &ram, uint32_t arr, uint32_t n, const M4 &m, int depth) {
        if (n > 4096) n = 4096;
        for (uint32_t i = 0; i < n; ++i) {
            const uint32_t c = ram.u32(arr + 4 * i);
            if (ram.ok(c)) walk(ram, c, m, depth + 1);
        }
    }

    void walk(const Ram &ram, uint32_t node, const M4 &m, int depth) {
        if (depth > 64 || visited > 200000 || !ram.ok(node)) return;
        // The same node can appear with different matrices (instances): only cut deep cycles
        if (depth > 24 && already(node)) return;
        ++visited;
        const uint32_t op = ram.u32(node) & 0x3F;
        ++byOp[op];
        switch (op) {
        case 0:                                                                                                 // OBJECT
            if (!(ram.u16(node + 0xC) & 1) && shadowLodD2_ < 0.f && outOfView(ram, node, m)) { ++culled; break; }
            items.push_back({m, node, animPkt_, lodIdx_, nullptr, blend_, fade_});
            items.back().cs = cs_;
            // pktAddVu1ObjAsm only writes qword 20 (specular, specIntensity +0x6C) with specLightMe (0x2000); without it the light
            // program reads whatever the last object left in VU1 memory. Togera's parts without the bit (skinned body, flags 0x310) inherit
            // the specular of the monster's own previous part (check: ~10% of its vertices came out darker, only in the gray).
            if (ram.u16(node + 0xC) & 0x2000) spec_ = ram.f32(node + 0x6C);
            items.back().spec = spec_;
            if (lightOwn_) {
                items.back().ownLight = true;
                std::memcpy(items.back().lightObj, lightObj_, sizeof lightObj_);
            }
            items.back().aref = aref_;
            items.back().zmsk = zmsk_;
            items.back().lodX = lodX_;
            objectUvOffset(ram, node, items.back().uvOff);
            objectColor(ram, node, items.back().color);
            objectEnv(ram, node, items.back());
            objectPointLights(ram, node, items.back());
            break;
        case 1: {                                                                           // GROUP (ctx at +4 = GS state: the children's ALPHA_1 and TEST_1)
            const uint16_t gflags = ram.u16(node + 0xA);                                    // visual (bit 0), noFOV (bit 2)
            if (!(gflags & 1)) break;
            if (!(gflags & 4) && ram.u32(addr::g_doFOVGroups) && shadowLodD2_ < 0.f && outOfView(ram, node, m)) { ++culled; break; }
            const CtxScope cs(*this, ram, ram.u32(node + 4));
            kids(ram, node + 0x20, ram.u16(node + 8), m, depth);
            break;
        }
        case 2: {                                                                           // LOD
            const uint32_t n = ram.u32(node + 4);
            if (n > 0 && n < 16) {
                int pick = -1;
                if (lod_ >= 0) pick = int(std::min<uint32_t>(uint32_t(lod_), n - 1));
                else {
                    // hierLod: the LARGEST index whose switchOutDis (squared distance) > distance to the LOD center; none = don't draw.
                    // In the shadow traversal (collectShadows) the distance is the blocker's fixed one (the game's shadowLodRangeSq)
                    float d2 = 0.f;
                    if (shadowLodD2_ >= 0.f) d2 = shadowLodD2_;
                    else if (eyeOk_) {
                        float c[3];
                        xform(m, ram.f32(node + 0x10), ram.f32(node + 0x14), ram.f32(node + 0x18), c);
                        d2 = (c[0] - eye_[0]) * (c[0] - eye_[0]) + (c[1] - eye_[1]) * (c[1] - eye_[1]) + (c[2] - eye_[2]) * (c[2] - eye_[2]);
                    }
                    for (int k = int(n) - 1; k >= 0; --k)
                        if (d2 < ram.f32(node + 0x20 + 16 * k + 4)) { pick = k; break; }
                }
                if (pick < 0) break;
                const uint32_t i = uint32_t(pick);
                const uint32_t c = ram.u32(node + 0x20 + 16 * i + 8);
                const int savedLod = lodIdx_;
                const float savedFade = fade_;
                // Cross-fade (hierLod): past the `fade` distance (+0x20 + 16i, squared like switchOutDis), the game draws LOD i and
                // i - 1 together, with t = 1 - (d - fade) / (switchOutDis - fade): t > 0.5 -> full i and i - 1 with alpha (1 - t) * 2;
                // otherwise full i - 1 and i with alpha 2t; without i - 1, i with alpha t. Without this native switched LOD all at once (VU1
                // check, lead 7: VU1 drew an "extra" instance, the neighboring LOD in the same place).
                float fi = 1.f, fprev = 0.f;
                const float fd = lod_ < 0 && eyeOk_ && shadowLodD2_ < 0.f ? ram.f32(node + 0x20 + 16 * i) : 0.f;
                const bool savedX = lodX_;
                if (fd > 0.f) {
                    float d2 = 0.f, cc[3];
                    xform(m, ram.f32(node + 0x10), ram.f32(node + 0x14), ram.f32(node + 0x18), cc);
                    for (int a = 0; a < 3; ++a) d2 += (cc[a] - eye_[a]) * (cc[a] - eye_[a]);
                    if (fd < d2) {
                        const float t = 1.f - (d2 - fd) / (ram.f32(node + 0x20 + 16 * i + 4) - fd);
                        if (i >= 1) { if (t > 0.5f) fprev = (1.f - t) * 2.f; else { fprev = 1.f; fi = t * 2.f; } }
                        else fi = t;
                        lodX_ = true;   // hierTraverseAsm keeps the weight (!= 0) and sends the 0x4000 OBJECTs to the alpha OT
                    }
                }
                // hierTraverseAsm order: in a cross-fade it goes straight to LOD i - 1's child and pushes LOD i's (with the weight),
                // which goes out afterward. With translucent objects writing Z during traversal, a nearly transparent LOD i in front made the solid
                // i - 1 fail the test (luta_hud: building d51400 under d51c40 at fade 0.21, the horizon showed through)
                if (fprev > 0.f) {
                    const uint32_t c2 = ram.u32(node + 0x20 + 16 * (i - 1) + 8);
                    lodIdx_ = int(i - 1);
                    fade_ = savedFade * fprev;
                    if (ram.ok(c2)) walk(ram, c2, m, depth + 1);
                }
                lodIdx_ = int(i);
                fade_ = savedFade * fi;
                if (ram.ok(c) && fade_ > 0.f) walk(ram, c, m, depth + 1);
                fade_ = savedFade;
                lodX_ = savedX;
                lodIdx_ = savedLod;
            }
            break;
        }
        case 3: {                                                                           // TRANSLATE
            const CtxScope cs(*this, ram, ram.u32(node + 4));
            kids(ram, node + 0x1C, ram.u32(node + 8),
                 mul(translateM(ram.f32(node + 0x10), ram.f32(node + 0x14), ram.f32(node + 0x18)), m), depth);
            break;
        }
        case 4: {                                                                           // ROTATE
            const CtxScope cs(*this, ram, ram.u32(node + 4));
            const LightScope ls(*this);
            const M4 local = readM4(ram, node + 0x10);
            lightRotate(local);
            kids(ram, node + 0x50, ram.u32(node + 8), mul(local, m), depth);
            break;
        }
        case 30: {                                                                          // SCALE
            const CtxScope cs(*this, ram, ram.u32(node + 4));
            const LightScope ls(*this);
            const float sx = ram.f32(node + 0x20), sy = ram.f32(node + 0x24), sz = ram.f32(node + 0x28);
            if (ram.u32(node + 0xC) == 0) lightScale(m, sx, sy, sz);   // hierScaleNode: with +0xC != 0 it copies the parent's table
            kids(ram, node + 0x2C, ram.u32(node + 8),
                 mul(mul(scaleM(sx, sy, sz), translateM(ram.f32(node + 0x10), ram.f32(node + 0x14), ram.f32(node + 0x18))), m), depth);
            break;
        }
        case 6: {                                                                           // SWITCH
            const uint32_t which = ram.u8(node + 8), n = ram.u8(node + 0xB);
            if (which < n) { const uint32_t c = ram.u32(node + 0xC + 4 * which); if (ram.ok(c)) walk(ram, c, m, depth + 1); }
            break;
        }
        case 8: {   // CONTROL: child1 (solid building) or child2 ("transparent"/missing version) depending on whether the volume blocks the view of the monster
            const uint32_t c = ctrlChild(ram, node, m);
            if (c && ram.ok(c)) walk(ram, c, m, depth + 1);
            break;
        }
        case 23: { const uint32_t c = ram.u32(node + 4); if (ram.ok(c)) walk(ram, c, m, depth + 1); break; } // ACTION_DATA
        case 35: kids(ram, node + 0xC, ram.u32(node + 4), m, depth); break;                 // ANIM_XFORM container (rigid monster parts: hands, head...): count at +4, children at +0xC (dbsWalk case 35)
        case 25: {                                                                          // CHAR_INSTANCE
            // The body parts are ANIM_XFORM nodes (rigid hierarchy, no skinning). The game itself computes each node's local matrix
            // every frame (hierAnimTransNode) into animOutput.atMat[matrixIdx]; reading it is enough.
            const uint32_t saved = atMat_, savedPkt = animPkt_, savedVal = animVal_;
            atMat_ = ram.u32(node + 0xC);
            animPkt_ = ram.u32(node + 0x18);
            animVal_ = ram.u32(node + 4);
            kids(ram, node + 0x2C, ram.u32(node + 0x28), m, depth);
            atMat_ = saved;
            animPkt_ = savedPkt;
            animVal_ = savedVal;
            break;
        }
        case 17: {                                                                          // ANIM_XFORM
            if (ram.f32(node + 0xC) == 0.f) break;                                          // visible
            M4 local = ident();
            const uint32_t idx = ram.u32(node + 0xC4);
            if (atMat_ && idx < 4096 && ram.ok(atMat_ + idx * 64, 64)) local = readM4(ram, atMat_ + idx * 64);
            local.m[0][3] = local.m[1][3] = local.m[2][3] = 0.f; local.m[3][3] = 1.f;
            const CtxScope cs(*this, ram, ram.u32(node + 8));
            const LightScope ls(*this);
            lightRotate(local);
            kids(ram, node + 0xCC, ram.u32(node + 4), mul(local, m), depth);
            break;
        }
        case 38: {                                                                          // ANIM_SCALE: scale then translation
            {   // hierAnimScaleNode doesn't write +0xC: visibility is channel val[vidx] of the CHAR_INSTANCE's animation output (-1 = always visible)
                const int vidx = int16_t(ram.u16(node + 0x10));
                const float vis = (vidx >= 0 && animVal_ && vidx < 4096) ? ram.f32(animVal_ + 4 * uint32_t(vidx)) : (vidx >= 0 ? ram.f32(node + 0xC) : 1.f);
                if (vis == 0.f) break;
            }
            // hierAnimScaleNode: the eye goes into the child's space via (eo + translate + postScale) / scale + preScale, i.e. the child's
            // point p goes to the parent at scale * (p + preScale) + translate + postScale (preScale +0x40 and postScale +0x50; ignoring them
            // displaced items attached to the monsters, VU1 check lead 7).
            // Zero scale flattens the child: the game draws nothing (light beams from Plantboss's tree, tex 297, with the scale channel
            // at 0 outside the attack; native replaced 0 with 1 and drew them whole, photo 2026-10-19_004009/002).
            float sx = ram.f32(node + 0x20), sy = ram.f32(node + 0x24), sz = ram.f32(node + 0x28);
            if (sx == 0.f && sy == 0.f && sz == 0.f) break;
            const M4 pre = translateM(ram.f32(node + 0x40), ram.f32(node + 0x44), ram.f32(node + 0x48));
            const M4 post = translateM(ram.f32(node + 0x30) + ram.f32(node + 0x50), ram.f32(node + 0x34) + ram.f32(node + 0x54),
                                       ram.f32(node + 0x38) + ram.f32(node + 0x58));
            const CtxScope cs(*this, ram, ram.u32(node + 8));
            const LightScope ls(*this);
            if (std::fabs(sx - sy) >= 0.001f || std::fabs(sx - sz) >= 0.001f) lightScale(m, sx, sy, sz);   // uniform: copies the parent's
            kids(ram, node + 0x60, ram.u32(node + 4), mul(mul(mul(pre, scaleM(sx, sy, sz)), post), m), depth);
            break;
        }
        case 39: {                                                                          // INTERACTIVE_STATE (+4 whichChild, +0xC child)
            // A destructible's child is a DESTRUCTIBLE: the game picks the state (0 = intact, then damaged/ruined...) in
            // `whichChild` of this parent node (docs/FORMATOS.md, "Destructibles"); traversal descends only into child[whichChild].child.
            const uint32_t c = ram.u32(node + 0xC);
            if (!ram.ok(c)) break;
            if ((ram.u32(c) & 63) == 31) {
                const uint32_t n = ram.u16(c + 0xA);
                uint32_t w = ram.u32(node + 4);
                ++destrHist_[std::min<uint32_t>(w, 7)];
                if (w >= n) w = 0;
                if (!n) break;
                const uint32_t ch = ram.u32(c + 0xC + 12 * w);
                if (ram.ok(ch)) walk(ram, ch, m, depth + 1);
            } else walk(ram, c, m, depth + 1);
            break;
        }
        case 5: {                                                                           // LIGHT (HierLight): only those with a lens flare
            const uint32_t fl = ram.u32(node + 4);
            if ((fl & 3) != 3 || worldLights.size() >= 256) break;
            // collectShadows walks the monsters again (shadow silhouette): the same light in the same _cs only counts once
            if (std::any_of(worldLights.begin(), worldLights.end(), [&](const WorldLight &o) { return o.node == node && o.cs == cs_; })) break;
            WorldLight l;
            xform(m, ram.f32(node + 0x10), ram.f32(node + 0x14), ram.f32(node + 0x18), l.p);
            const float bx = ram.f32(node + 0x20), by = ram.f32(node + 0x24), bz = ram.f32(node + 0x28);
            for (int k = 0; k < 3; ++k) l.beam[k] = bx * m.m[0][k] + by * m.m[1][k] + bz * m.m[2][k];
            l.flags = fl & 0xFF;
            l.node = node;
            l.cs = cs_;
            l.id = (ram.u32(node) >> 7) & 0x7FF;   // head.id2 (bits 7..17)
            l.intensity = ram.f32(node + 8);
            for (int k = 0; k < 3; ++k) l.rgb[k] = ram.f32(node + 0x30 + 4 * uint32_t(k));
            l.bw = ram.f32(node + 0x3C);
            worldLights.push_back(l);
            break;
        }
        case 22: {                                                                          // PARTICLE_EMITTER (HierParticleEmitter)
            if (emitters.size() >= 256) break;
            WorldEmitter em;
            em.node = node;
            em.preset = ram.u32(node + 4);
            xform(m, ram.f32(node + 0x10), ram.f32(node + 0x14), ram.f32(node + 0x18), em.e);
            if (std::any_of(emitters.begin(), emitters.end(), [&](const WorldEmitter &o) {   // again via collectShadows
                    return o.node == node && std::fabs(o.e[0] - em.e[0]) + std::fabs(o.e[1] - em.e[1]) + std::fabs(o.e[2] - em.e[2]) < 0.01f;
                }))
                break;
            const float h = ram.f32(node + 0x24), pt = ram.f32(node + 0x28);
            const float nl[3] = {std::cos(pt) * std::sin(h), std::cos(pt) * std::cos(h), std::sin(pt)};
            float a0[3];   // mathfMatrixFromNormal: with n vertical, row 0 = +X; in general, X horizontal perpendicular to n
            if (std::fabs(nl[2]) > 0.999f) { a0[0] = 1.f; a0[1] = 0.f; a0[2] = 0.f; }
            else { const float l = std::sqrt(nl[0] * nl[0] + nl[1] * nl[1]); a0[0] = nl[1] / l; a0[1] = -nl[0] / l; a0[2] = 0.f; }
            const float a2[3] = {a0[1] * nl[2] - a0[2] * nl[1], a0[2] * nl[0] - a0[0] * nl[2], a0[0] * nl[1] - a0[1] * nl[0]};
            for (int k = 0; k < 3; ++k) {
                em.r0[k] = a0[0] * m.m[0][k] + a0[1] * m.m[1][k] + a0[2] * m.m[2][k];
                em.r2[k] = a2[0] * m.m[0][k] + a2[1] * m.m[1][k] + a2[2] * m.m[2][k];
            }
            emitters.push_back(em);
            break;
        }
        case 31: {                                                                          // DESTRUCTIBLE: state 0 (intact)
            if (ram.u16(node + 0xA)) { const uint32_t c = ram.u32(node + 0xC); if (ram.ok(c)) walk(ram, c, m, depth + 1); }
            break;
        }
        default: break;   // collision, light, particles, animation...: upcoming milestones
        }
    }

    bool hpList_ = false;   // collect walking CsPool::m_HPActiveList (drawCs sends to `hp`)
    void csList(const Ram &ram, uint32_t head) {
        uint32_t n = ram.u32(head + 4);
        for (int guard = 0; ram.ok(n) && n != head && guard < 4096; ++guard, n = ram.u32(n + 4)) {
            drawCs(ram, ram.u32(n), true);
        }
    }

    // Button icons (X, O, square, triangle) from `McButtons`: ids that `McButtons::initButton` accepts (0x1F51..0x1F54 and 0x240E..0x2411).
    // `drawButtonsForString` sets `drawMe` and positions each one with `AlignCsWithView` (a few steps in front of the camera, oriented like it),
    // so they aren't screen-space HUD. They sit at z near 0 (they can fall below the scenery floor): the game draws them on top.
    static bool isButtonId(uint32_t id) { return (id >= 0x1F51 && id <= 0x1F54) || (id >= 0x240E && id <= 0x2411); }

    // A `_cs` (instance with its own matrix and position): epNode at +0, drawMe +0xC, trans +0x10, mat +0x20, scaleMe +0xF, scale +0x80
    void drawCs(const Ram &ram, uint32_t cs, bool needDrawMe, const float *at = nullptr) {
        if (!ram.ok(cs, 0xB0) || (needDrawMe && !ram.u8(cs + 0xC))) return;
        const uint32_t ep = ram.u32(cs);
        if (!ram.ok(ep)) return;
        // object id (high 14 bits); 0x1C20..0x2133 = HUD elements. Hud::update puts the HUD's _cs in front of the camera every frame
        // (trans = eye + R * gHudPos, mat = view rotation), so it projects like the rest; it goes to `hud`, drawn on top of everything.
        const uint32_t id = ram.u32(ep) >> 18;
        const bool isHud = needDrawMe && id >= 0x1C20 && id <= 0x2133 && !isButtonId(id);
        M4 m = readM4(ram, cs + 0x20);
        m.m[3][0] = ram.f32(cs + 0x10); m.m[3][1] = ram.f32(cs + 0x14); m.m[3][2] = ram.f32(cs + 0x18); m.m[3][3] = 1.f;
        m.m[0][3] = m.m[1][3] = m.m[2][3] = 0.f;
        if (at) { m.m[3][0] = at[0]; m.m[3][1] = at[1]; m.m[3][2] = at[2]; }
        else if (eyeOk_) {
            // hier carries the eye into object space with the mat itself (hierCsUpdate: mat * (eo + trans)) and comes back with it, not with the
            // inverse: VU1 draws at eye + (trans - eye) * R^T * R (R = 3x3 of mat). With an orthonormal R nothing changes; for debris
            // that shrinks (rows with norm < 1) the object comes out pulled toward the camera (up to ~37 units; VU1 check, lead 3).
            float a[3];
            for (int i = 0; i < 3; ++i)
                a[i] = (m.m[3][0] - eye_[0]) * m.m[i][0] + (m.m[3][1] - eye_[1]) * m.m[i][1] + (m.m[3][2] - eye_[2]) * m.m[i][2];
            for (int k = 0; k < 3; ++k) m.m[3][k] = eye_[k] + a[0] * m.m[0][k] + a[1] * m.m[1][k] + a[2] * m.m[2][k];
        }
        if (ram.u8(cs + 0xF)) {   // scaleMe
            const float sx = ram.f32(cs + 0x80), sy = ram.f32(cs + 0x84), sz = ram.f32(cs + 0x88);
            m = mul(scaleM(sx, sy, sz), m);
        }
        const size_t before = items.size();
        const uint32_t savedCs = cs_;
        cs_ = cs;
        walk(ram, ep, m, 0);
        cs_ = savedCs;
        if (hpList_ && needDrawMe && !isHud) {   // high-priority list: its own pass (see `hp`)
            std::vector<DrawItem> &dst = isButtonId(id) ? hpOverlay : hp;
            if (isButtonId(id))
                for (size_t i = before; i < items.size(); ++i) items[i].btnId = uint16_t(id);
            dst.insert(dst.end(), std::make_move_iterator(items.begin() + before), std::make_move_iterator(items.end()));
            items.resize(before);
        } else if (needDrawMe && isButtonId(id)) {   // button icons: over the scene, no depth (see `overlay`)
            for (size_t i = before; i < items.size(); ++i) items[i].btnId = uint16_t(id);
            overlay.insert(overlay.end(), std::make_move_iterator(items.begin() + before), std::make_move_iterator(items.end()));
            items.resize(before);
        } else if (isHud) {   // HUD: over the scene (see `hud`), with the id for the check
            for (size_t i = before; i < items.size(); ++i) items[i].hudId = uint16_t(id);
            hud.insert(hud.end(), std::make_move_iterator(items.begin() + before), std::make_move_iterator(items.end()));
            items.resize(before);
        }
    }
};

}  // namespace wotm
