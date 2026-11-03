// wotm_vu1check.inc: compares, per object, what the VU1 microcode sends to the GS against what the native layer (native/wotm_native.hpp)
// draws, in numbers rather than images. Included by wotm_scene.inc (global scope of ps2_runtime.cpp).
//
// WOTM_VU1CHECK=<seconds>, in the usual native mode (PS2X_NO_VU1/PS2X_NO_VIF1). The VIF1 processes every chain that arrives (g_wotmVif1Keep:
// keeps the VU1 microcode and data up to date; without it only ~78% matched) without running the VU1 and without feeding the GS (the native
// layer does the drawing). Every <seconds>, on a fight frame (g_frame >= 30), from the entry of hier(0, 0) to the entry of the next hier(0, 0):
//  - g_wotmVu1Tap on: hierFlushObjQ fires the real DMA (main.cpp), the VU1 runs and every XGKICK arrives here, without going to the
//    GS, tagged with the id of the MSCAL that produced it;
//    the id stores the RAM sources of the chain segments since the previous MSCAL (frame packet, polyPkt, palette...);
//  - at the start of each VIF1 chain the native layer collects the scene from the RAM at that moment: the chain references matrices and
//    palettes in place (REF), and only the 1st packet goes out inside hier; the last one (with the monsters) goes out through the DMA
//    interrupt (hierDmaHandler -> hierFlushObjQ), which the runtime delivers in the middle of the frame logic. The 1st collection takes the
//    whole scene; the others take only the objects the chain references (the hierarchy walk is redone, because the matrices change);
//  - on entry to the next hier(0, 0) it compares. Each VU1 triangle goes to the native object whose polyPkt contains one of the
//    sources of its MSCAL and matches the nearest native triangle of that object in the same chain's collection (vertices within 1 px).
//    The camera is copied into the packet and a pass 0 chain can go out already inside pass 1: the native side is projected with the camera
//    from the entry of each pass (worldToScreenMat of view 0 and the position from _cs) and whichever matches more wins.
// Only triangles entirely inside the screen count; degenerate ones (strip seams) are left out on both sides. The microcode culls
// back faces; native ones left over in those objects are reported as "back faces", not as a difference.
// Texture: each matched pair compares the TEX0 in effect on the GS when the VU1 drew (the PRIM's context) against the one of the slot of the
// section the native side uses (texKey/objectTex0), in the fields that select the texels.
// Output: [vu1check] lines on stderr (totals and the objects with the most differences). WOTM_VU1CHECK_DUMP=<prefix> writes the triangles of
// each check to <prefix>_<g_frame>.csv.
// Inventory ("[vu1check] inv" lines, function inventory): what each side draws with no counterpart on the other, per owner. On the reference
// side it also includes PATH2 primitives (VIF1 DIRECT) and PATH3 primitives (GIF channel), which is how HUD, menus and fades go out; on
// the native side, the objects (including sky and icons) whose polyPkt no VU1 triangle touched. It is the list of what is missing or extra in the port.
// Works on any screen (any caller of hier(0, 0)); the mem:<address>[=<value>] triggers and the F8 key catch the menus.
// WOTM_VU1CHECK_SNAP=<prefix> writes a snapshot of each sampled frame (<prefix>_<g_frame>.vu1snap) that reproduces the check
// without the game: `ps2EntryRunner --vu1cmp <file>... [--dump <prefix>]` (namespace snap and wotmVu1CmpMain, at the end).
// Only in the test executable (ps2EntryRunner): the game one (wotm, PS2X_HW_EMU=0) has no VU1 or GS to compare against and gets the empty branch.
#if defined(WOTM_HAVE_NATIVE) && PS2X_HW_EMU
#include <array>
#include "wotm_syms.h"   // wotm::sym / wotm::fn: ELF addresses by name (tools/native_syms.py --gen)
#include <map>
#include <set>
#include <tuple>

namespace wotm_vu1check
{
// A triangle's texture: the TEX0 fields that select the texels (TBP0, TBW, PSM, TW, TH, CBP, CPSM, CSM, CSA; not TCC/TFX/CLD).
// 0 = no texture (PRIM.TME 0 on the VU1; object without texKey on the native side); kTexUnknown = the VU1 drew with no TEX0 seen in the frame.
constexpr uint64_t kTexMask = 0x3FFFFFFFFull | (0x1FFFFFFull << 37);
constexpr uint64_t kTexUnknown = ~0ull;

// Per-vertex and per-triangle attributes, in GS units (the native ones converted to what it draws):
//  - u, v: normalized texture coordinate (1.0 = width 2^TW): on the VU1, S/Q and T/Q (or UV/16/2^TW with PRIM.FST); on the native side, Vtx::u/v;
//  - c: raw RGBA (0x80 = 1.0): on the VU1, the vertex RGBAQ; on the native side, the RGB of Scene::vertexRgb (with the VU1 lighting on lit ones) and Vtx::a * DrawItem::fade (the drawing still converts:
//    2r and 4a*fade over 255; the alpha scale depends on the texture's alpha and is out of scope here);
//  - abe: PRIM.ABE (VU1) / MeshData::abe (native); alpha: low byte of ALPHA_1/2 (A, B, C, D) of the PRIM's context (kAlphaUnknown = not
//    seen); on the native side, the ALPHA equivalent to DrawItem::blend (with abe) or to RL_BLEND_ALPHA (0x44);
//  - fge: PRIM.FGE (fog; the native side does not draw fog).
constexpr uint16_t kAlphaUnknown = 0x100;
struct Tri {
    uint32_t prog; float x[3], y[3]; uint64_t tex = 0; uint8_t slot = 0; uint16_t fbp = 0xFFFF;   // slot: native only; fbp: the VU1's FRAME (0xFFFF = not seen)
    float u[3] = {}, v[3] = {};
    uint8_t c[3][4] = {};
    uint64_t tex0 = 0;   // full TEX0 (VU1 only: TCC/TFX)
    uint16_t alpha = kAlphaUnknown;
    uint32_t test = 0xFFFFFFFFu;   // TEST of the context (VU1 only; 0xFFFFFFFF = not seen)
    bool abe = false, fge = false;
    uint8_t f[3] = {255, 255, 255};   // per-vertex fog F (VU1: F field of XYZF2; native: Scene::fogF with the projection camera)
    int32_t refTex = -1;   // VU1 only: texture read from the GS VRAM at draw time (index into g_refTex; -1 = not read)
};

// Native triangle in world coordinates; env/envK/nw: environment map (the UV comes out in project, Scene::envUv with the projection camera)
struct WTri { float p[3][3]; uint64_t tex; uint8_t slot; bool cull; float u[3], v[3]; uint8_t c[3][4]; bool abe; uint16_t alpha; bool fge; float burn;
              uint8_t env = 0; float envK = 0.f; float nw[3][3] = {}; };

struct NatObj {
    uint32_t node = 0, span = 0;
    uint16_t tex = 0;
    bool skinned = false, skip = false;   // skip: icons (the native side deliberately draws them elsewhere)
    bool sky = false;                     // sky (Scene::sky): compared like the others; in the owner map it shows up as "sky/icon"
    uint16_t hudId = 0;                    // HUD element (Scene::hud, drawn on top of the scene): object id; 0 = not HUD
    int items = 0;
    uint32_t badVerts = 0;                 // non-finite vertices (invariant)
    std::vector<WTri> tris;
    std::vector<std::array<float, 4>> sprites;   // ObjSprite: center in the world and side length (compareObjSprites)
};
using NatMap = std::map<uint32_t, NatObj>;   // keyed by polyPkt
struct Cam { float W[4][4]; float eye[3]; };

// Particles: each system's packet lives inside its own fx[i] (pkt at +0), so the source of the chain segment tells the system.
// The VU1 sends each particle as two triangles (a quad) or as a GS SPRITE (rectangle); the native side builds the same quads
// (Particles::forEachQuad, and Scene::spriteCorners for debris) and they are projected onto the screen (collectFx).
constexpr uint32_t kFx0 = wotm::fxaddr::fx, kFxEnd = wotm::fxaddr::fx + wotm::fxaddr::count * wotm::fxaddr::stride;
// c: RGBA (GS units) of each corner, the RGBAQ of each vertex (VU1; SPRITE: the 2nd vertex's on all four) or what the native side draws
// (Particles::forEachQuadColored; debris: Sprite::gsCol); hasC: c is filled
struct Quad { float x[4], y[4]; uint8_t n; bool fge = false; int idx = -1; uint8_t c[4][4] = {}; bool hasC = false; };   // n = 4 (quad) or 3 (unpaired triangle, VU1 only); fge: PRIM.FGE (VU1); idx: particle (native, dump only)

// Reference primitive for the inventory ("[vu1check] inv" lines): what the reference draws outside path 1 (PATH2: VIF1 DIRECT;
// PATH3: GIF channel), plus the VU1 SPRITEs, with the RAM source when known (PATH2: DMA chain segment; PATH3: only when the
// DMA read RAM in place). Whether or not entirely on screen (the HUD sits at the edges).
struct RefPrim {
    uint8_t path, kind;   // path 1/2/3; kind = PRIM.PRIM (3/4/5 triangle, 6 sprite)
    uint32_t src;         // RAM source of the packet (0 = unknown)
    float x0, y0, x1, y1; // screen box (GS coordinates)
    uint64_t tex;         // masked TEX0 of the context (0 = no texture, kTexUnknown = not seen)
    uint16_t fbp, alpha;
    bool abe;
    uint8_t rgba[4];
};
// Line segment (GS PRIM 1/2) on screen, with each end's color (GS units): on the VU1, the XGKICK; on the native side, FX_LINE
// (Particles::drawSystem: pos with the particle's color, pos - vel with alpha 0)
struct Seg { float x[2], y[2]; uint8_t c[2][4]; };
constexpr int kFxCams = 3;   // camera at the chain's moment, at pass 0 entry and at pass 1 entry
struct FxNat {
    uint32_t type = 0, preset = 0;
    int n = 0;
    bool built = false;               // info.attribs & 0x400 (packet built this frame)
    std::vector<Quad> quads[kFxCams];   // per camera
    std::vector<Seg> lines[kFxCams];   // FX_LINE: one segment per particle (pos -> pos - vel), per camera
    Cam cams[kFxCams] = {};
    std::vector<std::array<float, 10>> raw;   // dump only: pos (4), vel (3), age, minSize, maxSize of each particle
    std::vector<uint32_t> rawColor;           // dump only: the native color (Particles::colorAt) of each particle
    float prevMat[16] = {}, curMat[16] = {};   // dump only: raw prevViewMat (+0x2EC0, in the packet) and worldToScreenMat
};
using FxMap = std::map<uint32_t, FxNat>;   // keyed by index into fx[]

int g_period = 0;   // seconds between checks
std::chrono::steady_clock::time_point g_next;
bool g_armed = false;
const uint8_t *g_rdram = nullptr;
uint32_t g_frame = 0, g_pass = 0;
Cam g_cam[2];                                   // camera at entry of hier(0, 0) and hier(0, 1)
std::vector<NatMap> g_nats;                     // native collection at the start of each VIF1 chain
NatMap g_natPass1;                               // native collection at entry of hier(0, 1) (matrices from the end of the pass 0 traversal)
std::vector<FxMap> g_fxNats;                    // native particles per chain (only the 1st and those that reference fx[])
std::vector<char> g_fxChain;                    // chain with collected particles
std::vector<std::vector<uint32_t>> g_progs;     // id - 1 -> sources of the chain segments up to the MSCAL
std::vector<uint32_t> g_progChain;              // id - 1 -> chain (index into g_nats)
std::vector<Tri> g_vu;                          // VU1 triangles (Tri::prog = program id)
struct PxTri { uint32_t prog; float x[3], y[3]; uint16_t fbp; };
std::vector<PxTri> g_vuAll;                     // every XGKICK triangle, whether or not entirely on screen (per-pixel attribution in the replay)
uint32_t g_vuKicked = 0, g_vuOff = 0;             // XGKICK triangles in the frame / off screen (partly or entirely)
uint32_t g_vuPrimVerts[8] = {};                 // vertices in primitives the check does not compare (points only), by PRIM.PRIM
std::vector<std::pair<uint32_t, Quad>> g_vuRects;   // GS SPRITEs entirely on screen (program, rectangle)
std::vector<RefPrim> g_vuRectInfo;   // parallel to g_vuRects: texture, FRAME, ABE and color of each SPRITE (inventory)
uint32_t g_vuRectsKicked = 0, g_vuRectsRtt = 0;   // XGKICK SPRITEs / of those, the ones into a texture (FRAME off screen)
// Lens flare SPRITEs (program whose source is the fx[] of lensFlarePacket[0]), whether or not entirely on screen: corners in XGKICK order, UV (FST) and color
struct FlareRect { float x[2], y[2], u[2], v[2]; uint8_t c[4]; uint64_t tex0; };
std::vector<FlareRect> g_vuFlare;
wotm::Scene *g_scene = nullptr;
FILE *g_dump = nullptr;

std::vector<std::pair<uint32_t, Seg>> g_vuLines;   // XGKICK lines entirely on screen and in a screen buffer (program, segment)
uint32_t g_vuLinesKicked = 0;                      // XGKICK segments in the frame (whether or not entirely on screen)

// Texels: the texture of each XGKICK TEX0, read from the GS VRAM at draw time (decodeVram), only when the GS receives the uploads (replay
// with images, g_vram). upTex/upClut: the TBP0 / the CBP received an upload (BITBLTBUF + TRXDIR) earlier in this frame (informational only: in a
// fight the textures are already in the snapshot's VRAM).
struct RefTex { uint64_t tex0 = 0; uint32_t w = 0, h = 0, seq = 0; bool ok = false, upTex = false, upClut = false; std::vector<uint32_t> px; };
std::vector<RefTex> g_refTex;
std::map<uint64_t, int32_t> g_refTexIdx;   // TEX0 without TCC/TFX/CLD -> latest read (redone if the TBP0 or CBP gets an upload after it)
std::map<uint32_t, uint32_t> g_upSeq;      // DBP -> sequence number of the frame's latest upload (1, 2, ...)
uint32_t g_upCount = 0, g_upPath[4] = {};  // uploads in the frame, per path (1 = XGKICK, 2 = DIRECT, 3 = GIF channel)
uint32_t g_bitbltDbp = 0;
const uint8_t *g_vram = nullptr;           // GS VRAM (only in the replay with images); null = no texel comparison

std::vector<RefPrim> g_ref23;   // PATH2/PATH3 primitives of the frame (inventory)
uint8_t g_curPath = 2;   // path of the packet in gifTap(0, ...): 2 (PATH2, source in g_wotmPath2Src) or 3 (PATH3, source in g_curSrc)
uint32_t g_curSrc = 0;

constexpr float kX0 = 1728.f, kX1 = 2368.f, kY0 = 1936.f, kY1 = 2160.f;   // GS screen (640 x 224, centered at 2048)
inline bool inScreen(const Tri &t) {
    for (int c = 0; c < 3; ++c)
        if (!(t.x[c] >= kX0 && t.x[c] < kX1 && t.y[c] >= kY0 && t.y[c] < kY1)) return false;
    return true;
}
inline float area2(const Tri &t) { return (t.x[1] - t.x[0]) * (t.y[2] - t.y[0]) - (t.x[2] - t.x[0]) * (t.y[1] - t.y[0]); }
inline bool degenerate(const Tri &t) { return std::fabs(area2(t)) < 0.02f; }

Cam readCam(const uint8_t *rdram)
{
    const wotm::Ram ram{rdram, PS2_RAM_SIZE};
    Cam c{};
    for (int r = 0; r < 4; ++r)
        for (int k = 0; k < 4; ++k) c.W[r][k] = ram.f32(wotm::addr::worldToScreenMat + 16 * r + 4 * k);
    const wotm::GameCamera gc = wotm::readGameCamera(ram, 0);
    for (int i = 0; i < 3; ++i) c.eye[i] = gc.pos[i];
    return c;
}

// Projection like the VU1: d = p - eye, X = l0.d, Y = l1.d, W = l3.d (worldToScreenMat: rows 0 and 1 = focal * axis + 2048 * forward,
// row 3 = forward). False behind the camera.
inline bool projectPt(const Cam &cam, const float p[3], float &x, float &y)
{
    const float d[3] = {p[0] - cam.eye[0], p[1] - cam.eye[1], p[2] - cam.eye[2]};
    const float q = cam.W[3][0] * d[0] + cam.W[3][1] * d[1] + cam.W[3][2] * d[2] + cam.W[3][3];
    x = (cam.W[0][0] * d[0] + cam.W[0][1] * d[1] + cam.W[0][2] * d[2] + cam.W[0][3]) / q;
    y = (cam.W[1][0] * d[0] + cam.W[1][1] * d[1] + cam.W[1][2] * d[2] + cam.W[1][3]) / q;
    return q > 1e-3f;
}

// Only triangles entirely on screen, in front of the camera and not degenerate.
std::vector<Tri> project(const std::vector<WTri> &in, const Cam &cam, uint32_t key, std::vector<Tri> *culledOut = nullptr)
{
    std::vector<Tri> out;
    wotm::Scene::EnvView ev;   // only with some environment-map triangle
    for (const WTri &w : in) {
        const bool culled = w.cull && wotm::Scene::backFacing(w.p, cam.eye);   // the native side does not draw it (section with culling)
        if (culled && !culledOut) continue;
        Tri t{key, {}, {}, w.tex, w.slot};
        std::memcpy(t.u, w.u, sizeof t.u);
        std::memcpy(t.v, w.v, sizeof t.v);
        std::memcpy(t.c, w.c, sizeof t.c);
        t.abe = w.abe;
        t.alpha = w.alpha;
        t.fge = w.fge;
        if (w.env) {
            if (!ev.ok) ev = wotm::Scene::envView(wotm::Ram{g_rdram, PS2_RAM_SIZE}, cam.W, cam.eye);
            for (int c = 0; c < 3; ++c) wotm::Scene::envUv(w.env, w.envK, ev, w.p[c], w.nw[c], t.u[c], t.v[c]);
        }
        if (w.fge)
            for (int c = 0; c < 3; ++c) t.f[c] = uint8_t(g_scene->fogF(w.p[c], w.burn, cam.W[3], cam.eye));
        bool ok = true;
        for (int c = 0; c < 3 && ok; ++c) ok = projectPt(cam, w.p[c], t.x[c], t.y[c]);
        if (ok && inScreen(t) && !degenerate(t)) (culled ? *culledOut : out).push_back(t);
    }
    return out;
}

// Screen axes taken from the matrix: row 3 = forward; rows 0 and 1 minus 2048 * forward = focal * right / down.
wotm::FxView fxView(const Cam &cam)
{
    wotm::FxView v{};
    float f[3], fl = 0.f, rl = 0.f, ul = 0.f;
    for (int a = 0; a < 3; ++a) {
        f[a] = cam.W[3][a];
        v.right[a] = cam.W[0][a] - 2048.f * f[a];
        v.up[a] = cam.W[1][a] - 2048.f * f[a];
        fl += f[a] * f[a]; rl += v.right[a] * v.right[a]; ul += v.up[a] * v.up[a];
    }
    fl = std::sqrt(fl); rl = std::sqrt(rl); ul = std::sqrt(ul);
    v.depth[3] = cam.W[3][3] / fl;
    for (int a = 0; a < 3; ++a) {
        v.right[a] /= rl; v.up[a] /= ul;
        v.back[a] = -f[a] / fl;
        v.depth[a] = f[a] / fl;
        v.depth[3] -= v.depth[a] * cam.eye[a];
    }
    v.ox[3] = v.oy[3] = 0.f;   // view x and y (ribbons: perpendicular on screen)
    for (int a = 0; a < 3; ++a) {
        v.ox[a] = v.right[a]; v.oy[a] = v.up[a];
        v.ox[3] -= v.right[a] * cam.eye[a]; v.oy[3] -= v.up[a] * cam.eye[a];
    }
    return v;
}

// Native particles from the current RAM (after Scene::collect), quads entirely on screen, per system. As with the objects, the camera
// the packet carries is not known: the quads go out with the current camera and with the ones from the entries of passes 0 and 1 (if
// they already happened), and the comparison uses whichever matches more.
void collectFx(const uint8_t *rdram, FxMap &out)
{
    const wotm::Ram live{rdram, PS2_RAM_SIZE};
    // WOTM_VU1CHECK_FXSHADOW: particles come from the shadow copy (Particles::shadowRam), like in the live window, not from the whole RAM
    static const bool useShadow = std::getenv("WOTM_VU1CHECK_FXSHADOW") != nullptr;
    const std::shared_ptr<const uint8_t> shadow = useShadow ? wotm::Particles::shadowRam(live, g_scene->fx.systems) : nullptr;
    const wotm::Ram ram = shadow ? wotm::Ram{shadow.get(), PS2_RAM_SIZE} : live;
    const Cam cams[kFxCams] = {readCam(rdram), g_cam[0], g_cam[1]};
    const int nCams = g_pass == 1 ? 3 : 2;
    for (int ci = 0; ci < nCams; ++ci) {
        const Cam &cam = cams[ci];
        wotm::FxView view = fxView(cam);
        wotm::Particles::setFog(ram, view, cam.W[3], cam.eye);   // the native side discards particles with F < 1, like the VU1
        auto add = [&](FxNat &s, const float c[4][3], int idx = -1) {
            Quad q{{}, {}, 4, false, idx};
            for (int k = 0; k < 4; ++k)
                if (!projectPt(cam, c[k], q.x[k], q.y[k]) || !(q.x[k] >= kX0 && q.x[k] < kX1 && q.y[k] >= kY0 && q.y[k] < kY1)) return false;
            s.quads[ci].push_back(q);
            return true;
        };
        for (const wotm::FxSystem &sys : g_scene->fx.systems) {
            FxNat &s = out[sys.index];
            s.type = sys.type;
            s.n = sys.n;
            s.preset = ram.u32(sys.base + wotm::fxoff::fxId);
            s.built = true;   // Particles::collect only accepts systems with a built packet
            s.cams[ci] = cam;
            static const bool wantRaw = std::getenv("WOTM_VU1CHECK_DUMP") != nullptr;
            if (!ci && wantRaw) {
                const uint32_t pp = ram.u32(sys.base + wotm::fxoff::pos), vp = ram.u32(sys.base + wotm::fxoff::vel), ap = ram.u32(sys.base + wotm::fxoff::age);
                const float mn = ram.f32(sys.base + wotm::fxoff::minSize), mx = ram.f32(sys.base + wotm::fxoff::maxSize);
                // vel and age may be missing (the chain, type 12, zeroes both): they stay 0
                const bool hv = ram.ok(vp, 16 * uint32_t(sys.n)), ha = ram.ok(ap, 2 * uint32_t(sys.n));
                auto vf = [&](int k, int c) { return hv ? ram.f32(vp + 16 * k + 4 * c) : 0.f; };
                auto ag = [&](int k) { return ha ? int16_t(ram.u16(ap + 2 * k)) : int16_t(0); };
                if (ram.ok(pp, 16 * uint32_t(sys.n)))
                    for (int k = 0; k < sys.n; ++k) {
                        s.raw.push_back({ram.f32(pp + 16 * k), ram.f32(pp + 16 * k + 4), ram.f32(pp + 16 * k + 8), ram.f32(pp + 16 * k + 12),
                                         vf(k, 0), vf(k, 1), vf(k, 2), float(ag(k)), mn, mx});
                        uint32_t col = wotm::Particles::colorAt(ram, sys.base, ag(k));
                        if (sys.type == wotm::FX_STRITE && hv) {   // strite alpha like the drawing does (Particles::drawSystem)
                            float p[4], v[3], tri[4][3], ak = 1.f;
                            for (int a = 0; a < 4; ++a) p[a] = ram.f32(pp + 16 * k + 4 * a);
                            for (int a = 0; a < 3; ++a) v[a] = ram.f32(vp + 16 * k + 4 * a);
                            if (wotm::Particles::striteTriangle(ram, sys.base, p, v, tri, &ak)) col = (col & 0x00FFFFFFu) | (uint32_t(float(col >> 24) * ak) << 24);
                        }
                        s.rawColor.push_back(col);
                    }
                const uint32_t pm = ram.u32(sys.base + 0x2EC0);
                for (int k = 0; k < 16; ++k) {
                    if (ram.ok(pm, 64)) s.prevMat[k] = ram.f32(pm + 4 * k);
                    s.curMat[k] = ram.f32(wotm::addr::worldToScreenMat + 4 * k);
                }
            }
            wotm::Particles::forEachQuadColored(ram, sys, view, [&](int i, const float c[4][3], const float[4][2], uint32_t c01, uint32_t c23) {
                if (add(s, c, i)) {
                    Quad &q = s.quads[ci].back();
                    for (int k = 0; k < 4; ++k) std::memcpy(q.c[k], k < 2 ? &c01 : &c23, 4);
                    q.hasC = true;
                }
            });
            if (sys.type == wotm::FX_LINE) {   // like drawSystem (Particles::lineVu): head with the color, tail through prevViewMat with alpha 0
                for (int k = 0; k < sys.n; ++k) {
                    float p[2][3];
                    uint32_t col;
                    if (!wotm::Particles::lineVu(ram, sys.base, view, k, p[0], p[1], col)) continue;
                    Seg g{};
                    bool in = true;
                    for (int e = 0; e < 2 && in; ++e)
                        in = projectPt(cam, p[e], g.x[e], g.y[e]) && g.x[e] >= kX0 && g.x[e] < kX1 && g.y[e] >= kY0 && g.y[e] < kY1;
                    if (!in) continue;
                    const uint32_t tail = col & 0x00FFFFFFu;
                    std::memcpy(g.c[0], &col, 4);
                    std::memcpy(g.c[1], &tail, 4);
                    s.lines[ci].push_back(g);
                }
            }
        }
        for (const wotm::Sprite &sp : g_scene->sprites) {   // debris (frag): Scene::collectParticles, key = fx * 4096 + particle
            FxNat &s = out[sp.key >> 12];
            if (!ci) {
                const uint32_t b = kFx0 + (sp.key >> 12) * wotm::fxaddr::stride;
                s.type = wotm::FX_FRAG;
                s.preset = ram.u32(b + wotm::fxoff::fxId);
                s.built = (ram.u32(b + wotm::fxoff::infoAttribs) & 0x400) != 0;
                ++s.n;
            }
            float c[4][3];
            wotm::Scene::spriteCorners(sp, view.right, view.up, c);
            if (add(s, c)) {   // debris color like Scene::drawSprites: the packet's RGBAQ (gsCol); without it, rgba (1.0 = 0x80)
                uint32_t col = sp.gsCol;
                if (!col)
                    for (int k = 0; k < 4; ++k) col |= uint32_t(uint8_t(128.f * std::max(0.f, std::min(1.f, sp.rgba[k])))) << (8 * k);
                Quad &q = s.quads[ci].back();
                for (int k = 0; k < 4; ++k) std::memcpy(q.c[k], &col, 4);
                q.hasC = true;
            }
        }
    }
}

// The program's particle system (index into fx[]): the last source inside fx[]; -1 = none.
int fxOf(uint32_t prog)
{
    int sys = -1;
    for (uint32_t a : g_progs[prog - 1])
        if (a >= kFx0 && a < kFxEnd) sys = int((a - kFx0) / wotm::fxaddr::stride);
    return sys;
}

// The fx[] of the lens flare packet (lensFlarePacket[0] and [1]): its sprites go to the "lens flare" line, not to "particles".
bool flareSys(int sys)
{
    if (sys < 0) return false;
    for (int v = 0; v < 2; ++v) {
        uint32_t lf = 0;
        std::memcpy(&lf, g_rdram + wotm::sym::lensFlarePacket + 4 * v, 4);
        if (lf >= kFx0 && lf < kFxEnd && sys == int((lf - kFx0) / wotm::fxaddr::stride)) return true;
    }
    return false;
}

double g_walkMs = 0.0, g_projMs = 0.0;   // time spent on native collections in the sampled frame (hierarchy walk / triangles)

// Native triangles (current RAM, world coordinates, with skinning), per polyPkt. With `only` (sorted sources of the segments of
// one chain), only the objects whose polyPkt contains one of them.
void collectNative(const uint8_t *rdram, NatMap &nat, const std::vector<uint32_t> *only = nullptr)
{
    if (!g_scene) g_scene = new wotm::Scene();
    const wotm::Ram ram{rdram, PS2_RAM_SIZE};
    auto t0 = std::chrono::steady_clock::now();
    g_scene->collect(ram);
    auto t1 = std::chrono::steady_clock::now();
    g_walkMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
    auto add = [&](const std::vector<wotm::DrawItem> &list, bool skip, bool sky = false) {
        for (const wotm::DrawItem &it : list) {
            const uint32_t pp = ram.u32(it.node + 4) & 0x0FFFFFFF;
            const wotm::MeshData *md = g_scene->mesh(ram, it.node);
            if (!pp || !md || !md->valid) continue;
            const uint32_t span = md->skinned ? std::max(md->bytes, 0x100u) : (md->qwc + 1) * 16u;
            if (only) {
                const auto s = std::lower_bound(only->begin(), only->end(), pp);
                if (s == only->end() || *s - pp >= span) continue;
            }
            NatObj &o = nat[pp];
            if (!o.items) {
                o.node = it.node;
                o.tex = ram.u16(it.node + 0x50);
                o.skinned = md->skinned;
                o.span = span;
                o.hudId = it.hudId;
            }
            o.skip = o.skip || skip;
            o.sky = o.sky || sky;
            ++o.items;
            // skip objects (icons) also get triangles: the per-object comparison ignores them, the inventory counts them
            uint64_t tex[32] = {};   // per slot: the texture the native side draws with (Scene::textureId -> texKey/objectTex0)
            for (uint32_t s = 0; s < 32; ++s)
                if (md->slotMask & (1u << s)) {
                    const uint64_t k = wotm::Scene::texKey(ram, it, s);
                    // texId 0 = buffer being displayed: the TEX0 that texmGetTextureAddrs builds with displayBuffer (0x6F87F8)
                    tex[s] = k == wotm::Scene::kScreenKey ? ((uint64_t(ram.u32(wotm::sym::displayBuffer) | 0x28128000u) | (1ull << 33)) & kTexMask)
                           : k ? wotm::Scene::objectTex0(ram, it.node, s) & kTexMask : 0;
                }
            const uint16_t alpha = md->abe ? it.blend : wotm::kGsAlphaNormal;
            g_scene->forEachTri(ram, it, [&](const float p[3][3], const wotm::Vtx *v, const uint8_t (*rgb)[3], const float (*nw)[3]) {
                WTri w;
                w.slot = v[0].slot;
                w.cull = v[0].cull != 0;
                w.tex = tex[w.slot & 31];
                w.abe = md->abe;
                w.alpha = alpha;
                w.fge = v[0].fge != 0;
                w.burn = ram.f32(it.node + 0x2C);   // fogBurnThru
                w.env = it.env;
                w.envK = it.envK;
                std::memcpy(w.nw, nw, sizeof w.nw);
                bool ok = true;
                for (int c = 0; c < 3; ++c) {
                    for (int a = 0; a < 3; ++a) {
                        w.p[c][a] = p[c][a];
                        ok = ok && std::isfinite(p[c][a]);
                    }
                    w.u[c] = v[c].u + it.uvOff[0];
                    w.v[c] = v[c].v + it.uvOff[1];
                    std::memcpy(w.c[c], rgb[c], 3);   // the color the native side draws with (Scene::vertexRgb)
                    w.c[c][3] = uint8_t(std::min(255.f, v[c].a * it.fade * std::max(0.f, it.color[3])));
                }
                if (ok) o.tris.push_back(w);
                else ++o.badVerts;
            });
            for (const wotm::ObjSprite &s : md->sprites) {
                float p[3];
                wotm::xform(it.m, s.x, s.y, s.z, p);
                o.sprites.push_back({p[0], p[1], p[2], s.size});
            }
        }
    };
    add(g_scene->items, false);
    add(g_scene->sky, false, true);   // sky at skyCs->trans like hierTraceSky: included in the comparison
    add(g_scene->overlay, true);
    add(g_scene->hp, false);
    add(g_scene->hpOverlay, true);
    add(g_scene->hud, false);   // HUD: the native side draws it in front of the camera like the other objects; the inventory still lists it by id
    g_projMs += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count();
}

uint32_t g_chains = 0;   // VIF1 chains in the sampled frame

namespace snap { void chain(const uint8_t *ram, const std::pair<uint32_t, uint32_t> *segs, size_t n, const uint8_t *data, uint32_t bytes); }

// Start of each VIF1 chain: collect with the RAM it is going to read (the 1st takes everything; the others, only the objects they reference).
void chainTap(const std::pair<uint32_t, uint32_t> *segs, size_t n, const uint8_t *data, uint32_t bytes)
{
    if (!g_armed || !g_rdram) return;
    snap::chain(g_rdram, segs, n, data, bytes);   // snapshot (WOTM_VU1CHECK_SNAP): the RAM at this moment and the chain's bytes
    ++g_chains;
    std::vector<uint32_t> only;
    for (size_t i = 0; i < n; ++i) only.push_back(segs[i].second);
    std::sort(only.begin(), only.end());
    g_nats.emplace_back();
    collectNative(g_rdram, g_nats.back(), g_nats.size() == 1 ? nullptr : &only);
    if (g_nats.size() > 1) {
        // The alpha OT is built during the hier(v,0) walk and goes out in a hier(v,1) chain, after TheGame::Update, which may have
        // turned the _cs off in between (a reticle that disappears: drawMe 0). An object the chain references that this collection did
        // not find comes from the 1st collection, the one from the moment the packet was built.
        NatMap &cur = g_nats.back();
        const NatMap &first = g_nats.front();
        for (uint32_t a : only) {
            auto it = first.upper_bound(a);
            if (it == first.begin()) continue;
            --it;
            if (a - it->first < it->second.span && !cur.count(it->first)) cur.emplace(it->first, it->second);
        }
    }
    const bool fx = g_nats.size() == 1 || std::any_of(only.begin(), only.end(), [](uint32_t a) { return a >= kFx0 && a < kFxEnd; });
    g_fxNats.emplace_back();
    g_fxChain.push_back(fx);
    if (fx) collectFx(g_rdram, g_fxNats.back());
}

// VU1 header at the MSCAL (dump only): the program address and qwords 5-8 (vf31 and the light directions) and 17-20 (light
// colors) of data memory, which the lighting program (0x1010) reads with LQ n(vi0).
struct ProgHdr { uint32_t addr; float q[8][4]; float pt[7][4]; };
std::vector<ProgHdr> g_progHdr;

// Input of the lens flare microprogram (0x2B20) read from VU1 data memory at the MSCAL, the way it reads it: q44 = (center, 18, n),
// tables in q8-q43, list in q45 + 3i, maximum UV in the w of q0x110 + selector. One per sampled frame (the last one).
std::vector<wotm::Particles::FlareFrame> g_flareVuIn;
void flareFromVu(const uint8_t *vu)
{
    auto f32 = [&](uint32_t q, int c) { float v; std::memcpy(&v, vu + 16 * q + 4 * c, 4); return v; };
    auto u32 = [&](uint32_t q, int c) { uint32_t v; std::memcpy(&v, vu + 16 * q + 4 * c, 4); return v; };
    wotm::Particles::FlareFrame f;
    f.cx = f32(44, 0);
    f.cy = f32(44, 1);
    f.nspr = std::min<int>(18, int(u32(44, 2) & 0xFFFF));
    const int n = std::min<int>(74, int(u32(44, 3) & 0xFFFF));
    for (int k = 0; k < f.nspr; ++k) {
        auto &s = f.spr[k];
        s.tex0 = uint64_t(u32(8 + k, 0)) | (uint64_t(u32(8 + k, 1)) << 32);
        s.t = f32(8 + k, 2);
        s.half = f32(8 + k, 3);
        s.uvMax = float(u32(0x110 + (u32(8 + k, 3) & 3), 3) & 0xFFFF) / 16.f;
        for (int j = 0; j < 4; ++j) s.rgba[j] = f32(26 + k, j);
    }
    for (int i = 0; i < n; ++i) {
        const uint32_t q = 45 + 3 * uint32_t(i);
        wotm::Particles::Flare fl;
        fl.x = f32(q, 0); fl.y = f32(q, 1); fl.z = f32(q, 2); fl.id = u32(q, 3);
        fl.flags = u32(q + 1, 0); fl.zfudge = f32(q + 1, 0); fl.w = f32(q + 1, 1); fl.dot = f32(q + 1, 2); fl.bw = f32(q + 1, 3);
        for (int j = 0; j < 3; ++j) fl.rgb[j] = f32(q + 2, j);
        fl.intensity = f32(q + 2, 3);
        f.e.push_back(fl);
    }
    g_flareVuIn.push_back(std::move(f));
}

uint32_t mscalTap(const uint32_t *srcs, uint32_t n, uint32_t prog, const uint8_t *vu1Data)
{
    if (prog == 0x2B20u && vu1Data) flareFromVu(vu1Data);
    g_progs.emplace_back(srcs, srcs + n);
    g_progChain.push_back(g_nats.empty() ? 0u : uint32_t(g_nats.size() - 1));
    static const bool dump = std::getenv("WOTM_VU1CHECK_DUMP") != nullptr;
    if (dump) {
        ProgHdr h{prog, {}};
        static const int qw[8] = {5, 6, 7, 8, 17, 18, 19, 20};
        if (vu1Data && prog == 0x15D8u) for (int k = 0; k < 7; ++k) std::memcpy(h.pt[k], vu1Data + 16 * (169 + k), 16);   // point lights (q169-q175)
        if (vu1Data)
            for (int k = 0; k < 8; ++k) std::memcpy(h.q[k], vu1Data + 16 * qw[k], 16);
        g_progHdr.push_back(h);
    }
    return uint32_t(g_progs.size());
}

// XGKICK packet: GIF tags; PACKED with XYZ2/XYZF2 (draw, unless ADC) and XYZ3/XYZF3 (only push); PRIM in the tag or via A+D.
// Odd strip triangle with the first two swapped, like the native side (same winding on both sides).
// TEX0_1/TEX0_2 (PACKED 0x6/0x7 or A+D) hold until the next change, across packets: each triangle takes the one of its PRIM's context.
uint64_t g_tex0[2] = {kTexUnknown, kTexUnknown};
uint16_t g_fbp[2] = {0xFFFF, 0xFFFF};   // FRAME_1/FRAME_2.FBP in effect (A+D 0x4C/0x4D)
uint16_t g_alpha[2] = {kAlphaUnknown, kAlphaUnknown};   // ALPHA_1/ALPHA_2 in effect (A+D 0x42/0x43), low byte
uint32_t g_test[2] = {0xFFFFFFFFu, 0xFFFFFFFFu};   // TEST_1/TEST_2 in effect (A+D 0x47/0x48); 0xFFFFFFFF = not seen
// RGBAQ, ST and UV in effect on the GS (they hold across packets, like TEX0)
uint8_t g_rgba[4] = {0x80, 0x80, 0x80, 0x80};
float g_s = 0.f, g_t = 0.f, g_q = 1.f;
uint32_t g_uv = 0;   // U in bits 0-13, V in 16-29 (10.4)
uint8_t g_fogF = 255;   // fog F in effect (XYZF2/XYZF3 or FOG via A+D 0x0A)
uint32_t g_gifZ = 0;    // Z of the last XYZ2/XYZF2 (24 bits in XYZF2), for WOTM_VU1CHECK_Z
// A TEX0's texture read from VRAM the way the GS samples it, in RGBA with the GS's raw alpha (0x80 = 1.0), to compare with the native side
// (Scene::decodeTex with rawAlpha). Formats: CT32/24/16, T8, T4 and the resident T8H/T4HL/T4HH (index in the high byte of a CT32).
// CSM1 palette: 256 colors = 16x16 with entries 8-15 and 16-23 of each group of 32 swapped, 16 colors = 8x2, in the palette's PSM (CT32
// or CT16). With 16 colors the GS loads the 16 entries from CBP at position CSA and reads from there: CSA does not change the color. CT24 and
// CT16 follow what the native side assumes about TEXA (TA0 = 0x80 on CT24; CT16: bit 15 = 0x80, else 0); CT16 expands 5 bits like the GS (v << 3).
bool decodeVram(const uint8_t *vram, uint64_t t0v, std::vector<uint32_t> &out, uint32_t &w, uint32_t &h)
{
    using namespace wotm::gs;
    const Vram v{vram, PS2_GS_VRAM_SIZE};
    const Tex0 t = Tex0::decode(t0v);
    w = 1u << t.tw; h = 1u << t.th;
    if (w > 1024 || h > 1024) return false;
    auto c16 = [](uint32_t x) { return ((x & 31) << 3) | (((x >> 5) & 31) << 11) | (((x >> 10) & 31) << 19) | ((x & 0x8000) ? 0x80000000u : 0u); };
    uint32_t pal[256] = {};
    const bool p8 = t.psm == T8 || t.psm == T8H, p4 = t.psm == T4 || t.psm == T4HL || t.psm == T4HH;
    if (p8 || p4) {
        if (t.cpsm != CT32 && t.cpsm != CT24 && t.cpsm != CT16 && t.cpsm != CT16S) return false;
        for (uint32_t i = 0; i < (p8 ? 256u : 16u); ++i) {
            const uint32_t x = p8 ? (i & 7) | ((i & 0x10) >> 1) : (i & 7), y = p8 ? ((i & 0xE0) >> 4) | ((i & 8) >> 3) : (i >> 3);
            pal[i] = (t.cpsm == CT32 || t.cpsm == CT24) ? rd32(v, addrCT32(t.cbp, 1, x, y)) : c16(rd16(v, addrCT16(t.cbp, 1, x, y)));
            if (t.cpsm == CT24) pal[i] = (pal[i] & 0x00FFFFFFu) | 0x80000000u;
        }
    }
    out.assign(size_t(w) * h, 0);
    for (uint32_t y = 0; y < h; ++y)
        for (uint32_t x = 0; x < w; ++x) {
            uint32_t c = 0;
            switch (t.psm) {
            case CT32: c = rd32(v, addrCT32(t.tbp0, t.tbw, x, y)); break;
            case CT24: c = (rd32(v, addrCT32(t.tbp0, t.tbw, x, y)) & 0x00FFFFFFu) | 0x80000000u; break;
            case CT16: case CT16S: c = c16(rd16(v, addrCT16(t.tbp0, t.tbw, x, y))); break;
            case T8: c = pal[rd8(v, addrT8(t.tbp0, t.tbw, x, y))]; break;
            case T4: { const uint32_t n = nibT4(t.tbp0, t.tbw, x, y), b = rd8(v, n >> 1); c = pal[(n & 1) ? (b >> 4) : (b & 15)]; break; }
            case T8H: c = pal[rd32(v, addrCT32(t.tbp0, t.tbw, x, y)) >> 24]; break;
            case T4HL: c = pal[(rd32(v, addrCT32(t.tbp0, t.tbw, x, y)) >> 24) & 15]; break;
            case T4HH: c = pal[rd32(v, addrCT32(t.tbp0, t.tbw, x, y)) >> 28]; break;
            default: return false;
            }
            out[size_t(y) * w + x] = c;
        }
    return true;
}

// VRAM read of the TEX0 in use (one per TEX0 per frame, redone if the TBP0 or CBP got an upload after it); -1 = no VRAM.
int32_t refTexFor(uint64_t t0)
{
    if (!g_vram || t0 == kTexUnknown) return -1;
    constexpr uint64_t kMask = 0x3FFFFFFFFull | (0xFFFFFFull << 37);   // TBP0..TH and CBP..CSA (without TCC, TFX and CLD)
    const uint64_t key = t0 & kMask;
    const wotm::gs::Tex0 t = wotm::gs::Tex0::decode(t0);
    const bool indexed = t.psm == wotm::gs::T8 || t.psm == wotm::gs::T4 || t.psm == wotm::gs::T8H || t.psm == wotm::gs::T4HL || t.psm == wotm::gs::T4HH;
    auto up = [](uint32_t bp) { const auto it = g_upSeq.find(bp); return it == g_upSeq.end() ? 0u : it->second; };
    const uint32_t seq = std::max(up(t.tbp0), indexed ? up(t.cbp) : 0u);
    const auto it = g_refTexIdx.find(key);
    if (it != g_refTexIdx.end() && g_refTex[size_t(it->second)].seq >= seq) return it->second;
    RefTex r;
    r.tex0 = key;
    r.seq = seq;
    r.upTex = up(t.tbp0) != 0;
    r.upClut = !indexed || up(t.cbp) != 0;
    r.ok = decodeVram(g_vram, t0, r.px, r.w, r.h);
    g_refTex.push_back(std::move(r));
    return g_refTexIdx[key] = int32_t(g_refTex.size() - 1);
}

void gifTap(uint32_t prog, const uint8_t *p, uint32_t bytes)
{
    uint32_t prim = 4, nv = 0;
    float vx[3] = {}, vy[3] = {};
    uint32_t vz[3] = {};
    struct VAttr { float s, t, q; uint32_t uv; uint8_t c[4]; uint8_t f; };
    VAttr va[3] = {};
    float fx0 = 0.f, fy0 = 0.f;   // fan (PRIM 5): the 1st vertex, shared by all triangles
    uint32_t fz0 = 0;
    VAttr fa0 = {};
    auto vertex = [&](float x, float y, bool kick, uint8_t f) {
        vx[0] = vx[1]; vy[0] = vy[1];
        vx[1] = vx[2]; vy[1] = vy[2];
        vx[2] = x; vy[2] = y;
        vz[0] = vz[1]; vz[1] = vz[2]; vz[2] = g_gifZ;
        va[0] = va[1];
        va[1] = va[2];
        va[2] = {g_s, g_t, g_q, g_uv, {g_rgba[0], g_rgba[1], g_rgba[2], g_rgba[3]}, f};
        ++nv;
        const uint32_t type = prim & 7;
        if (nv == 1) { fx0 = x; fy0 = y; fz0 = g_gifZ; fa0 = va[2]; }
        if (type == 5 && nv >= 3) { vx[0] = fx0; vy[0] = fy0; vz[0] = fz0; va[0] = fa0; }   // fan: triangle (1st, second to last, last)
        if (!prog) {   // PATH2/PATH3: the registers are already in g_tex0/g_fbp/...; the primitives go to the inventory (g_ref23)
            if (!kick) return;
            const bool tri = nv >= 3 && (type == 4 || type == 5 || (type == 3 && nv % 3 == 0));
            const bool spr = type == 6 && nv % 2 == 0;
            if (!tri && !spr) return;
            const uint32_t ctx = (prim >> 9) & 1;
            const uint64_t t0 = g_tex0[ctx];
            RefPrim r{g_curPath, uint8_t(type), g_curPath == 2 ? g_wotmPath2Src : g_curSrc, vx[2], vy[2], vx[2], vy[2],
                      !((prim >> 4) & 1) ? 0 : t0 == kTexUnknown ? kTexUnknown : t0 & kTexMask, g_fbp[ctx], g_alpha[ctx], ((prim >> 6) & 1) != 0, {}};
            for (int k = tri ? 0 : 1; k < 2; ++k) {
                r.x0 = std::min(r.x0, vx[k]); r.x1 = std::max(r.x1, vx[k]);
                r.y0 = std::min(r.y0, vy[k]); r.y1 = std::max(r.y1, vy[k]);
            }
            std::memcpy(r.rgba, va[2].c, 4);
            g_ref23.push_back(r);
            return;
        }
        if (kick && (type == 1 || type == 2) && (type == 2 ? nv >= 2 : nv % 2 == 0)) {   // line / line strip: segment of the last 2
            ++g_vuLinesKicked;
            const uint16_t fbp = g_fbp[(prim >> 9) & 1];
            bool in = fbp == 0xFFFF || fbp == 0x46 || fbp == 0x8C;
            for (int k = 1; k < 3; ++k) in = in && vx[k] >= kX0 && vx[k] < kX1 && vy[k] >= kY0 && vy[k] < kY1;
            if (!in) return;
            Seg g{{vx[1], vx[2]}, {vy[1], vy[2]}, {}};
            std::memcpy(g.c[0], va[1].c, 4);
            std::memcpy(g.c[1], va[2].c, 4);
            g_vuLines.push_back({prog, g});
            return;
        }
        if (kick && nv >= 3 && (type == 4 || type == 5 || (type == 3 && nv % 3 == 0))) {
            const int a = (type == 4 && ((nv - 1) & 1)) ? 1 : 0;
            const uint32_t ctx = (prim >> 9) & 1;
            const uint64_t t0 = g_tex0[ctx];
            const uint64_t tex = !((prim >> 4) & 1) ? 0 : t0 == kTexUnknown ? kTexUnknown : t0 & kTexMask;
            Tri t{prog, {vx[a], vx[1 - a], vx[2]}, {vy[a], vy[1 - a], vy[2]}, tex};
            if (tex && tex != kTexUnknown) t.refTex = refTexFor(t0);
            t.fbp = g_fbp[ctx];
            t.tex0 = t0;
            t.alpha = g_alpha[ctx];
            t.test = g_test[ctx];
            t.abe = (prim >> 6) & 1;
            t.fge = (prim >> 5) & 1;
            const bool fst = (prim >> 8) & 1;
            const float tw = t0 == kTexUnknown ? 1.f : float(1u << ((t0 >> 26) & 0xF)), th = t0 == kTexUnknown ? 1.f : float(1u << ((t0 >> 30) & 0xF));
            const int order[3] = {a, 1 - a, 2};
            for (int k = 0; k < 3; ++k) {
                const VAttr &s = va[order[k]];
                if (fst) { t.u[k] = (s.uv & 0x3FFF) / 16.f / tw; t.v[k] = ((s.uv >> 16) & 0x3FFF) / 16.f / th; }
                else if (s.q != 0.f) { t.u[k] = s.s / s.q; t.v[k] = s.t / s.q; }
                std::memcpy(t.c[k], s.c, 4);
                t.f[k] = s.f;
            }
            {   // diagnostic: WOTM_VU1CHECK_Z=<prog[,prog..]> prints the GS Z of those programs' triangles (MSCAL index, column 4 of the dump)
                static const char *zp = std::getenv("WOTM_VU1CHECK_Z");
                if (zp && (*zp == '*' || (std::string(",") + zp + ",").find("," + std::to_string(prog) + ",") != std::string::npos))
                    std::fprintf(stderr, "[z] prog %u (%.1f %.1f %u) (%.1f %.1f %u) (%.1f %.1f %u)\n", prog, vx[0], vy[0], vz[0], vx[1], vy[1], vz[1], vx[2], vy[2], vz[2]);
            }
            ++g_vuKicked;
            g_vuAll.push_back({prog, {t.x[0], t.x[1], t.x[2]}, {t.y[0], t.y[1], t.y[2]}, t.fbp});   // per-pixel attribution: whether or not entirely on screen
            if (!inScreen(t)) ++g_vuOff;
            else if (!degenerate(t)) g_vu.push_back(t);
        } else if (kick && type == 6) {   // SPRITE: rectangle between the last two vertices (particles)
            if (nv % 2) return;
            Quad q{{vx[1], vx[2], vx[2], vx[1]}, {vy[1], vy[1], vy[2], vy[2]}, 4, ((prim >> 5) & 1) != 0};
            for (int k = 0; k < 4; ++k) std::memcpy(q.c[k], va[2].c, 4);   // the SPRITE paints with the 2nd vertex's RGBAQ
            q.hasC = true;
            uint32_t lf = 0;   // lensFlarePacket[0]
            std::memcpy(&lf, g_rdram + wotm::sym::lensFlarePacket, 4);
            if (lf >= kFx0 && lf < kFxEnd && fxOf(prog) == int((lf - kFx0) / wotm::fxaddr::stride)) {
                const uint64_t t0 = g_tex0[(prim >> 9) & 1];
                const float tw = t0 == kTexUnknown ? 1.f : float(1u << ((t0 >> 26) & 0xF)), th = t0 == kTexUnknown ? 1.f : float(1u << ((t0 >> 30) & 0xF));
                FlareRect r{{vx[1], vx[2]}, {vy[1], vy[2]}, {(va[1].uv & 0x3FFF) / 16.f / tw, (va[2].uv & 0x3FFF) / 16.f / tw},
                            {((va[1].uv >> 16) & 0x3FFF) / 16.f / th, ((va[2].uv >> 16) & 0x3FFF) / 16.f / th}, {0, 0, 0, 0}, t0};
                std::memcpy(r.c, va[2].c, 4);
                g_vuFlare.push_back(r);
            }
            bool in = true;
            for (int k = 0; k < 4; ++k) in = in && q.x[k] >= kX0 && q.x[k] < kX1 && q.y[k] >= kY0 && q.y[k] < kY1;
            ++g_vuRectsKicked;
            // render-to-texture (FRAME outside the screen buffers): the dust/smoke compositing (FBP 0x1FB, Scene::dustTexture)
            const uint16_t fbp = g_fbp[(prim >> 9) & 1];
            if (fbp != 0xFFFF && fbp != 0x46 && fbp != 0x8C) { ++g_vuRectsRtt; return; }
            if (in) {
                g_vuRects.push_back({prog, q});
                const uint64_t t0 = g_tex0[(prim >> 9) & 1];
                RefPrim r{1, 6, 0, std::min(vx[1], vx[2]), std::min(vy[1], vy[2]), std::max(vx[1], vx[2]), std::max(vy[1], vy[2]),
                          !((prim >> 4) & 1) ? 0 : t0 == kTexUnknown ? kTexUnknown : t0 & kTexMask, fbp, g_alpha[(prim >> 9) & 1], ((prim >> 6) & 1) != 0, {}};
                std::memcpy(r.rgba, va[2].c, 4);
                g_vuRectInfo.push_back(r);
            }
        } else if (kick && type != 3 && type != 4 && type != 5) ++g_vuPrimVerts[type];
    };
    for (uint32_t pos = 0; pos + 16 <= bytes;) {
        uint64_t lo, hi;
        std::memcpy(&lo, p + pos, 8);
        std::memcpy(&hi, p + pos + 8, 8);
        pos += 16;
        const uint32_t nloop = uint32_t(lo & 0x7FFF), flg = uint32_t((lo >> 58) & 3);
        const uint32_t nreg = uint32_t((lo >> 60) & 0xF) ? uint32_t((lo >> 60) & 0xF) : 16u;
        if ((lo >> 46) & 1) { prim = uint32_t((lo >> 47) & 0x7FF); nv = 0; }
        if (flg == 0) {
            for (uint32_t l = 0; l < nloop; ++l)
                for (uint32_t r = 0; r < nreg && pos + 16 <= bytes; ++r, pos += 16) {
                    const uint32_t reg = uint32_t((hi >> (4 * r)) & 0xF);
                    uint32_t w[4];
                    std::memcpy(w, p + pos, 16);
                    if (reg == 0x4 || reg == 0x5 || reg == 0xC || reg == 0xD) {
                        const bool adc = (w[3] >> 15) & 1;
                        if (reg == 0x4 || reg == 0xC) g_fogF = uint8_t((w[3] >> 4) & 0xFF);   // XYZF2/XYZF3: F in bits 100-107
                        g_gifZ = (reg == 0x4 || reg == 0xC) ? ((w[2] >> 4) & 0xFFFFFF) : w[2];   // PACKED XYZF2: Z in bits 68-91
                        vertex((w[0] & 0xFFFF) / 16.f, (w[1] & 0xFFFF) / 16.f, (reg == 0x4 || reg == 0x5) && !adc, g_fogF);
                    } else if (reg == 0x1) { for (int k = 0; k < 4; ++k) g_rgba[k] = uint8_t(w[k]); }
                    else if (reg == 0x2) { std::memcpy(&g_s, &w[0], 4); std::memcpy(&g_t, &w[1], 4); std::memcpy(&g_q, &w[2], 4); }
                    else if (reg == 0x3) g_uv = (w[0] & 0x3FFF) | ((w[1] & 0x3FFF) << 16);
                    else if (reg == 0xE && (w[2] & 0xFF) == 0x01) { for (int k = 0; k < 4; ++k) g_rgba[k] = uint8_t(w[0] >> (8 * k)); std::memcpy(&g_q, &w[1], 4); }
                    else if (reg == 0xE && (w[2] & 0xFF) == 0x02) { std::memcpy(&g_s, &w[0], 4); std::memcpy(&g_t, &w[1], 4); }
                    else if (reg == 0xE && (w[2] & 0xFF) == 0x03) g_uv = w[0] & 0x3FFF3FFF;
                    else if (reg == 0xE && ((w[2] & 0xFF) == 0x42 || (w[2] & 0xFF) == 0x43)) g_alpha[(w[2] & 0xFF) - 0x42] = uint16_t(w[0] & 0xFF);
                    else if (reg == 0xE && ((w[2] & 0xFF) == 0x47 || (w[2] & 0xFF) == 0x48)) g_test[(w[2] & 0xFF) - 0x47] = w[0];
                    else if (reg == 0x0 || (reg == 0xE && (w[2] & 0xFF) == 0x00)) { prim = w[0] & 0x7FF; nv = 0; }
                    else if (reg == 0x6 || reg == 0x7) g_tex0[reg - 0x6] = uint64_t(w[0]) | (uint64_t(w[1]) << 32);
                    else if (reg == 0xE && ((w[2] & 0xFF) == 0x06 || (w[2] & 0xFF) == 0x07))
                        g_tex0[(w[2] & 0xFF) - 0x06] = uint64_t(w[0]) | (uint64_t(w[1]) << 32);
                    else if (reg == 0xE && ((w[2] & 0xFF) == 0x4C || (w[2] & 0xFF) == 0x4D)) g_fbp[(w[2] & 0xFF) - 0x4C] = uint16_t(w[0] & 0x1FF);
                    else if (reg == 0xE && (w[2] & 0xFF) == 0x50) g_bitbltDbp = w[1] & 0x3FFF;   // BITBLTBUF.DBP (bits 32..45)
                    else if (reg == 0xE && (w[2] & 0xFF) == 0x53 && (w[0] & 3) == 0) {   // TRXDIR 0: upload from the EE to VRAM at DBP
                        g_upSeq[g_bitbltDbp] = ++g_upCount;
                        ++g_upPath[prog ? 1 : g_curPath];
                    }
                }
        } else if (flg == 1) pos += ((nloop * nreg + 1) / 2) * 16;
        else pos += nloop * 16;
        if ((lo >> 15) & 1) break;   // EOP
    }
}

// GIF channel (PATH3) packet for the inventory: the same parser as PATH2 (registers and primitives). The RAM source is only known
// when the GIF channel DMA read RAM in place (the pointer points inside it); in the replay and for copied packets it stays 0.
void path3Tap(const uint8_t *data, uint32_t bytes)
{
    if (!g_armed) return;
    g_curPath = 3;
    g_curSrc = (g_rdram && data >= g_rdram && data < g_rdram + PS2_RAM_SIZE) ? uint32_t(data - g_rdram) : 0u;
    gifTap(0, data, bytes);
    g_curPath = 2;
    g_curSrc = 0;
}

// Native object (polyPkt) that contains one of the program's sources; 0 = none.
uint32_t objectOf(const NatMap &nat, uint32_t prog)
{
    for (uint32_t a : g_progs[prog - 1]) {
        auto it = nat.upper_bound(a);
        if (it == nat.begin()) continue;
        --it;
        if (a - it->first < it->second.span) return it->first;
    }
    return 0;
}

// For each VU1 triangle, the still-free native triangle with the smallest sum of vertex distances (in any order), with every vertex
// within 1 px (4 px grid by centroid). vuPair = index of the matched native triangle (-1 = none); natHit marks the matched native ones.
// Same texture? TBP 0 on the native side: an object from the alpha OT; the texmGetTextureAddrs at the end of hier fills in the TBP only in the
// copy of the alpha packet, not in the polyPkt the reference TEX0 comes from (the native side draws with the texInfo texture, without that TBP).
inline bool sameTex(uint64_t vuTex, uint64_t natTex)
{
    if (vuTex == kTexUnknown) return false;
    const bool noTbp = natTex && !(natTex & 0x3FFFu);
    return noTbp ? (vuTex & ~0x3FFFull) == natTex : vuTex == natTex;
}

// Three rounds: (2) only pairs with the same texture; (1) VU1 triangles with some texture the native object uses; (0) the rest. Without
// this, geometry repeated in the same place (object layers in different slots, the projected shadow pass, whose texture no native
// object has) swapped the pairs and reported a different texture.
uint32_t match(const std::vector<Tri> &vu, const std::vector<Tri> &nat, std::vector<int> &vuPair, std::vector<char> &natHit)
{
    constexpr float kTol = 1.f;
    auto cell = [](float x, float y) { return (uint64_t(uint32_t(int32_t(std::floor(x / 4.f)))) << 32) | uint32_t(int32_t(std::floor(y / 4.f))); };
    std::unordered_multimap<uint64_t, uint32_t> grid;
    for (uint32_t i = 0; i < nat.size(); ++i)
        grid.emplace(cell((nat[i].x[0] + nat[i].x[1] + nat[i].x[2]) / 3.f, (nat[i].y[0] + nat[i].y[1] + nat[i].y[2]) / 3.f), i);
    vuPair.assign(vu.size(), -1);
    natHit.assign(nat.size(), 0);
    std::vector<uint64_t> natTexs;
    for (const Tri &n : nat) natTexs.push_back(n.tex);
    std::sort(natTexs.begin(), natTexs.end());
    natTexs.erase(std::unique(natTexs.begin(), natTexs.end()), natTexs.end());
    std::vector<char> known(vu.size(), 0);
    for (size_t i = 0; i < vu.size(); ++i)
        for (uint64_t nt : natTexs)
            if (sameTex(vu[i].tex, nt)) { known[i] = 1; break; }
    uint32_t matched = 0;
    for (int texRound = 2; texRound >= 0; --texRound)
    for (size_t i = 0; i < vu.size(); ++i) {
        if (vuPair[i] >= 0 || (texRound == 1 && !known[i])) continue;
        const Tri &t = vu[i];
        const float cx = (t.x[0] + t.x[1] + t.x[2]) / 3.f, cy = (t.y[0] + t.y[1] + t.y[2]) / 3.f;
        int best = -1;
        float bestCost = 1e30f;
        for (int dx = -1; dx <= 1; ++dx)
            for (int dy = -1; dy <= 1; ++dy) {
                auto range = grid.equal_range(cell(cx + 4.f * dx, cy + 4.f * dy));
                for (auto g = range.first; g != range.second; ++g) {
                    if (natHit[g->second]) continue;
                    const Tri &n = nat[g->second];
                    if (texRound == 2 && !sameTex(t.tex, n.tex)) continue;
                    float cost = 0.f;
                    bool all = true;
                    for (int a = 0; a < 3 && all; ++a) {
                        float d = 1e30f;
                        int bb = 0;
                        for (int b = 0; b < 3; ++b) {
                            const float e = std::max(std::fabs(t.x[a] - n.x[b]), std::fabs(t.y[a] - n.y[b]));
                            if (e < d) { d = e; bb = b; }
                        }
                        all = d <= kTol;
                        // tie-break by color (overlapping faces with different vertex colors): at most ~0.02 px
                        float dc = 0.f;
                        for (int k = 0; k < 3; ++k) dc += std::fabs(float(t.c[a][k]) - float(n.c[bb][k]));
                        cost += d + 1e-5f * dc;
                    }
                    if (all && cost < bestCost) { bestCost = cost; best = int(g->second); }
                }
            }
        if (best >= 0) { natHit[size_t(best)] = 1; vuPair[i] = best; ++matched; }
    }
    return matched;
}

// A system's VU1 particles (in XGKICK order) -> quads: two consecutive triangles from the same program sharing two vertices
// become a quad; whatever is left stays a triangle (n = 3; its pair went off screen or is degenerate).
// skipFirst: the first triangle stays alone and pairing starts at the second. In a strip (stripe, sweepTrail) every triangle shares two
// vertices with the next one, and one extra triangle at the start (half a bridge clipped at the edge) shifts all the pairs; the caller
// tries both parities and keeps whichever matches more.
std::vector<Quad> vuQuads(const std::vector<Tri> &tris, bool skipFirst = false)
{
    std::vector<Quad> out;
    auto same = [](float ax, float ay, float bx, float by) { return std::fabs(ax - bx) < 0.01f && std::fabs(ay - by) < 0.01f; };
    for (size_t i = 0; i < tris.size(); ++i) {
        const Tri &a = tris[i];
        Quad q{{a.x[0], a.x[1], a.x[2]}, {a.y[0], a.y[1], a.y[2]}, 3, a.fge};
        std::memcpy(q.c, a.c, sizeof a.c);
        q.hasC = true;
        if (i + 1 < tris.size() && tris[i + 1].prog == a.prog && !(skipFirst && i == 0)) {
            const Tri &b = tris[i + 1];
            int shared = 0, extra = -1;
            for (int k = 0; k < 3; ++k) {
                bool in = false;
                for (int j = 0; j < 3; ++j) in = in || same(b.x[k], b.y[k], a.x[j], a.y[j]);
                if (in) ++shared;
                else extra = k;
            }
            if (shared == 2) { q.x[3] = b.x[extra]; q.y[3] = b.y[extra]; std::memcpy(q.c[3], b.c[extra], 4); q.n = 4; ++i; }
        }
        out.push_back(q);
    }
    return out;
}

// Matches each VU1 quad with the nearest free native quad: every vertex of one within 1 px of some vertex of the other (both ways for
// quads; loose triangles only VU1 -> native). Returns how many matched; pairs (optional): the native match of each one (-1 = none).
uint32_t matchQuads(const std::vector<Quad> &vu, const std::vector<Quad> &nat, std::vector<int> *pairs = nullptr)
{
    constexpr float kTol = 1.f;
    auto cell = [](float x, float y) { return (uint64_t(uint32_t(int32_t(std::floor(x / 4.f)))) << 32) | uint32_t(int32_t(std::floor(y / 4.f))); };
    // every vertex of a close to some vertex of b; with color on both, tie-break by the color of the nearest vertex (at most ~0.01 px: two
    // identical quads in the same place, like the age 0 and age 1 particles of preset 29 in the debris, used to match crosswise)
    auto cover = [](const Quad &a, const Quad &b, float &cost) {
        for (int i = 0; i < a.n; ++i) {
            float d = 1e30f;
            int bj = 0;
            for (int j = 0; j < b.n; ++j)
                if (const float e = std::max(std::fabs(a.x[i] - b.x[j]), std::fabs(a.y[i] - b.y[j])); e < d) { d = e; bj = j; }
            if (d > kTol) return false;
            cost += d;
            if (a.hasC && b.hasC)
                for (int k = 0; k < 4; ++k) cost += 1e-5f * std::fabs(float(a.c[i][k]) - float(b.c[bj][k]));
        }
        return true;
    };
    std::unordered_multimap<uint64_t, uint32_t> grid;
    for (uint32_t i = 0; i < nat.size(); ++i) grid.emplace(cell(nat[i].x[0], nat[i].y[0]), i);
    std::vector<char> hit(nat.size(), 0);
    uint32_t matched = 0;
    if (pairs) pairs->assign(vu.size(), -1);
    for (size_t qi = 0; qi < vu.size(); ++qi) {
        const Quad &q = vu[qi];
        int best = -1;
        float bestCost = 1e30f;
        std::vector<uint32_t> cand;
        for (int k = 0; k < q.n; ++k)   // native corner 0 is close to some VU1 corner
            for (int dx = -1; dx <= 1; ++dx)
                for (int dy = -1; dy <= 1; ++dy) {
                    auto range = grid.equal_range(cell(q.x[k] + 4.f * dx, q.y[k] + 4.f * dy));
                    for (auto g = range.first; g != range.second; ++g) cand.push_back(g->second);
                }
        for (uint32_t c : cand) {
            if (hit[c]) continue;
            float cost = 0.f;
            if (!cover(q, nat[c], cost) || (q.n == 4 && !cover(nat[c], q, cost))) continue;
            if (cost < bestCost) { bestCost = cost; best = int(c); }
        }
        if (best >= 0) { hit[size_t(best)] = 1; ++matched; if (pairs) (*pairs)[qi] = best; }
    }
    return matched;
}

// What the VU1 drew for one system in one chain: triangles (paired into quads) and GS SPRITEs (already rectangles).
struct FxVu { std::vector<Tri> tris; std::vector<Quad> rects; };
using FxVuMap = std::map<uint32_t, std::map<uint32_t, FxVu>>;   // system -> chain

// [vu1check] lens flare line: SPRITEs from microprogram 0x2B20 against Particles::flareSprites over the list the VU1 received (a copy from the
// end of the sampled particleDraw; intensity already comes from the emulated GS's Z test, so there is no native Z test here). Matches by
// corners (1/4 px, in XGKICK order); among the matched ones it counts color (> 1), UV (> 0.01 texel) and TEX0 (TBP/TW/TH/PSM) differences.
void compareFlare()
{
    std::vector<wotm::Particles::FlareQuad> nat;
    std::vector<std::vector<wotm::Particles::FlareIn>> ins(g_flareVuIn.size());
    size_t entradas = 0;
    std::vector<int> natFrame;   // index into g_flareVuIn of each native sprite (the table's TEX0)
    for (size_t fi = 0; fi < g_flareVuIn.size(); ++fi) {
        const auto &f = g_flareVuIn[fi];
        for (const auto &fl : f.e) ins[fi].push_back({fl.x, fl.y, fl.intensity, &fl});
        entradas += f.e.size();
        wotm::Particles::flareSprites(f, ins[fi], [&](const wotm::Particles::FlareQuad &q) { nat.push_back(q); natFrame.push_back(int(fi)); });
    }
    if (g_vuFlare.empty() && nat.empty()) return;
    std::vector<char> used(nat.size(), 0);
    uint32_t m = 0, cor = 0, uv = 0, tex = 0;
    for (const FlareRect &r : g_vuFlare) {
        int best = -1;
        for (size_t j = 0; j < nat.size(); ++j) {
            const auto &q = nat[j];
            if (used[j] || std::fabs(q.xa - r.x[0]) > 0.25f || std::fabs(q.ya - r.y[0]) > 0.25f || std::fabs(q.xb - r.x[1]) > 0.25f ||
                std::fabs(q.yb - r.y[1]) > 0.25f) continue;
            if (best < 0 || (q.rgb[0] == r.c[0] && q.rgb[1] == r.c[1] && q.rgb[2] == r.c[2])) best = int(j);
        }
        if (best < 0) {
            if (g_dump) std::fprintf(g_dump, "flare,vu,%.3f,%.3f,%.3f,%.3f,%u,%u,%u,%u\n", r.x[0], r.y[0], r.x[1], r.y[1], r.c[0], r.c[1], r.c[2], r.c[3]);
            continue;
        }
        used[size_t(best)] = 1;
        ++m;
        const auto &q = nat[size_t(best)];
        bool dc = false;
        for (int j = 0; j < 3; ++j) dc = dc || std::abs(int(q.rgb[j]) - int(r.c[j])) > 1;
        dc = dc || r.c[3] != 0x80;
        cor += dc;
        uv += std::fabs(q.u0 - r.u[0]) * 64.f > 0.01f || std::fabs(q.v0 - r.v[0]) * 64.f > 0.01f || std::fabs(q.u1 - r.u[1]) * 64.f > 0.01f ||
              std::fabs(q.v1 - r.v[1]) * 64.f > 0.01f;
        const uint64_t mask = 0x3FFFull | (0x3Full << 20) | (0xFFull << 26);   // TBP0, PSM, TW, TH
        tex += (g_flareVuIn[size_t(natFrame[size_t(best)])].spr[q.k].tex0 & mask) != (r.tex0 & mask);
        if (g_dump && dc)
            std::fprintf(g_dump, "flarecolor,%d,%u,%u,%u,%u,%u,%u,%u\n", q.k, r.c[0], r.c[1], r.c[2], r.c[3], q.rgb[0], q.rgb[1], q.rgb[2]);
        if (g_dump)
            std::fprintf(g_dump, "flareuv,%d,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%.5f,%llx\n", q.k, r.u[0], r.v[0], r.u[1], r.v[1], q.u0, q.v0, q.u1, q.v1,
                         (unsigned long long)r.tex0);
    }
    if (g_dump)
        for (size_t j = 0; j < nat.size(); ++j)
            if (!used[j])
                std::fprintf(g_dump, "flare,nat,%d,%.3f,%.3f,%.3f,%.3f,%u,%u,%u\n", nat[j].k, nat[j].xa, nat[j].ya, nat[j].xb, nat[j].yb, nat[j].rgb[0],
                             nat[j].rgb[1], nat[j].rgb[2]);
    std::fprintf(stderr, "[vu1check] g=%u lens flare: programs %zu inputs %zu | vu %zu native %zu matched %u | color differs %u uv differs %u tex0 differs %u\n",
                 g_frame, g_flareVuIn.size(), entradas, g_vuFlare.size(), nat.size(), m, cor, uv, tex);
}

// [vu1check] particles line and the systems with the most differences. rectsOrphan: GS SPRITEs outside fx[].
void compareFx(const FxVuMap &fxVu, uint32_t rectsOrphan)
{
    int lastFx = -1;   // reference chain for what the VU1 did not draw: the last one with collected particles
    for (size_t c = 0; c < g_fxChain.size(); ++c)
        if (g_fxChain[c]) lastFx = int(c);
    if (lastFx < 0) return;
    std::vector<uint32_t> keys;
    for (const auto &kv : fxVu) keys.push_back(kv.first);
    for (const auto &kv : g_fxNats[size_t(lastFx)]) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    // color: on matched quads, each VU1 corner against the nearest native corner; cor = some RGB off by more than 4, alfa = alpha off by more than 4
    struct Row { uint32_t sys; const FxNat *s; uint32_t vu, nat, m, soltos, fge, comp = 0, cor = 0, alfa = 0; uint8_t vuC[4] = {}, natC[4] = {}; };
    std::vector<Row> rows;
    uint32_t sysVu = 0, sysNat = 0, sysBoth = 0, totVu = 0, totNat = 0, totM = 0, totSoltos = 0;
    static const FxNat kNone;
    for (uint32_t sys : keys) {
        uint32_t chain = uint32_t(lastFx);
        const auto vit = fxVu.find(sys);
        if (vit != fxVu.end()) {
            size_t most = 0;
            for (const auto &kv : vit->second)
                if (kv.second.tris.size() + kv.second.rects.size() > most) { most = kv.second.tris.size() + kv.second.rects.size(); chain = kv.first; }
        }
        const auto nit = g_fxNats[chain].find(sys);
        const FxNat &s = nit != g_fxNats[chain].end() ? nit->second : kNone;
        const bool triFx = s.type == wotm::FX_STRITE || s.type == wotm::FX_WAKE;   // one triangle per particle (native: degenerate quad)
        std::vector<Quad> whole;
        uint32_t soltos = 0, m = 0;
        int cam = 0;   // whichever camera matches more
        for (int par = 0; par < (triFx ? 1 : 2); ++par) {   // parity of the triangle pairs (vuQuads skipFirst): whichever matches more
            std::vector<Quad> vq, w;
            if (vit != fxVu.end())
                for (const auto &kv : vit->second) {
                    const std::vector<Quad> q = vuQuads(kv.second.tris, par == 1);
                    vq.insert(vq.end(), q.begin(), q.end());
                    vq.insert(vq.end(), kv.second.rects.begin(), kv.second.rects.end());
                }
            uint32_t so = 0;
            for (const Quad &q : vq) ((q.n == 4 || triFx) ? (w.push_back(q), 0u) : ++so);
            int cc = 0;
            uint32_t mm = matchQuads(w, s.quads[0]);
            for (int ci = 1; ci < kFxCams && mm < w.size(); ++ci)
                if (!s.quads[ci].empty())
                    if (const uint32_t mc = matchQuads(w, s.quads[ci]); mc > mm) { mm = mc; cc = ci; }
            if (par == 0 || mm > m) { whole.swap(w); soltos = so; m = mm; cam = cc; }
        }
        const std::vector<Quad> &nq = s.quads[cam];
        const uint32_t nVu = uint32_t(whole.size()), nNat = uint32_t(nq.size());
        if (!nVu && !nNat) continue;
        sysVu += nVu > 0; sysNat += nNat > 0; sysBoth += nVu && nNat;
        totVu += nVu; totNat += nNat; totM += m; totSoltos += soltos;
        uint32_t fge = 0;
        for (const Quad &q : whole) fge += q.fge;
        Row row{sys, &s, nVu, nNat, m, soltos, fge};
        if (m) {
            std::vector<int> qp;
            matchQuads(whole, nq, &qp);
            for (size_t j = 0; j < whole.size(); ++j) {
                if (qp[j] < 0) continue;
                const Quad &a = whole[j], &b = nq[size_t(qp[j])];
                if (!a.hasC || !b.hasC) continue;
                if (s.type == wotm::FX_ARC || s.type == wotm::FX_ARC2 || s.type == wotm::FX_CHAIN) {
                    // arc smaller than 2 px: the 1 px tolerance matches one step's quad with its neighbor and the color comes out as the next point's
                    float x0 = a.x[0], x1 = a.x[0], y0 = a.y[0], y1 = a.y[0];
                    for (int k = 1; k < a.n; ++k) { x0 = std::min(x0, a.x[k]); x1 = std::max(x1, a.x[k]); y0 = std::min(y0, a.y[k]); y1 = std::max(y1, a.y[k]); }
                    if (x1 - x0 < 2.f && y1 - y0 < 2.f) continue;
                }
                ++row.comp;
                bool dc = false, da = false;
                for (int k = 0; k < a.n; ++k) {
                    int bb = 0;
                    float d = 1e30f;
                    for (int e = 0; e < b.n; ++e)
                        if (const float de = std::max(std::fabs(a.x[k] - b.x[e]), std::fabs(a.y[k] - b.y[e])); de < d) { d = de; bb = e; }
                    bool c1 = false;
                    for (int ch = 0; ch < 3; ++ch) c1 = c1 || std::abs(int(a.c[k][ch]) - int(b.c[bb][ch])) > 4;
                    const bool a1 = std::abs(int(a.c[k][3]) - int(b.c[bb][3])) > 4;
                    if ((c1 || a1) && !row.cor && !row.alfa && !dc && !da) { std::memcpy(row.vuC, a.c[k], 4); std::memcpy(row.natC, b.c[bb], 4); }
                    dc = dc || c1;
                    da = da || a1;
                }
                row.cor += dc;
                row.alfa += da;
            }
        }
        rows.push_back(row);
        if (g_dump) {
            for (const Quad &q : whole)   // the compared ones (chosen parity; without the loose triangles), with the RGBA color of corner 0
                std::fprintf(g_dump, "fx%u,vuq,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%02x%02x%02x%02x\n", sys, q.n, q.x[0], q.y[0], q.x[1], q.y[1], q.x[2],
                             q.y[2], q.n == 4 ? q.x[3] : 0.f, q.n == 4 ? q.y[3] : 0.f, q.c[0][0], q.c[0][1], q.c[0][2], q.c[0][3]);
            if (vit != fxVu.end())   // attributes of the VU1 triangles (UV, color, ALPHA, TEX0), in XGKICK order
                for (const auto &kv : vit->second)
                    for (const Tri &t : kv.second.tris)
                        std::fprintf(g_dump, "fx%u,vut,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.4f,%.4f,%.4f,%.4f,%.4f,%.4f,%08x,%08x,%08x,%x,%d,%llx,%x,%u\n", sys,
                                     t.x[0], t.y[0], t.x[1], t.y[1], t.x[2], t.y[2], t.u[0], t.v[0], t.u[1], t.v[1], t.u[2], t.v[2],
                                     *(const uint32_t *)t.c[0], *(const uint32_t *)t.c[1], *(const uint32_t *)t.c[2], t.alpha, int(t.abe),
                                     (unsigned long long)t.tex0, t.test, t.prog);
            for (const Quad &q : nq)
                std::fprintf(g_dump, "fx%u,natq,4,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%d\n", sys, q.x[0], q.y[0], q.x[1], q.y[1], q.x[2], q.y[2], q.x[3], q.y[3],
                             q.idx);
            std::fprintf(g_dump, "fxsys,%u,%u,%u,%u,%d,%d\n", sys, chain, s.preset, s.type, s.n, cam);
            // comparison camera (W 4x4 by rows, eye) and the raw particles: offline tooling projects any point like projectPt
            const Cam &c = s.cams[cam];
            std::fprintf(g_dump, "fxcam,%u", sys);
            for (int r = 0; r < 4; ++r)
                for (int k = 0; k < 4; ++k) std::fprintf(g_dump, ",%.6g", c.W[r][k]);
            std::fprintf(g_dump, ",%.6g,%.6g,%.6g\n", c.eye[0], c.eye[1], c.eye[2]);
            std::fprintf(g_dump, "fxmat,%u", sys);   // prevViewMat (16) and worldToScreenMat (16), raw
            for (float f : s.prevMat) std::fprintf(g_dump, ",%.7g", f);
            for (float f : s.curMat) std::fprintf(g_dump, ",%.7g", f);
            std::fprintf(g_dump, "\n");
            for (size_t k = 0; k < s.raw.size(); ++k) {
                std::fprintf(g_dump, "fxp,%u,%zu", sys, k);
                for (float f : s.raw[k]) std::fprintf(g_dump, ",%.6g", f);
                std::fprintf(g_dump, ",%08x\n", k < s.rawColor.size() ? s.rawColor[k] : 0u);
            }
        }
    }
    const double pct = totVu ? 100.0 * totM / totVu : 0.0;
    std::fprintf(stderr, "[vu1check] g=%u particles: systems vu %u native %u both %u | quads vu %u native %u matched %.1f%% missing %u extra %u | "
                         "loose triangles on vu %u | GS SPRITE %u (%u into a texture, %u on screen, %u outside fx[]) | VU1 vertices not compared: point %u\n",
                 g_frame, sysVu, sysNat, sysBoth, totVu, totNat, pct, totVu - totM, totNat - totM, totSoltos, g_vuRectsKicked, g_vuRectsRtt, uint32_t(g_vuRects.size()),
                 rectsOrphan, g_vuPrimVerts[0]);
    // per type (vis.attribs; 0 = a system the native side does not have): matched / VU1 quads / native quads, and how many VU1 ones have FGE
    std::map<uint32_t, std::array<uint32_t, 4>> byType;
    for (const Row &r : rows) {
        auto &t = byType[r.s->n ? r.s->type : 0u];
        t[0] += r.m; t[1] += r.vu; t[2] += r.nat; t[3] += r.fge;
    }
    if (!byType.empty()) {
        std::string line;
        char part[112];
        for (const auto &kv : byType) {
            std::snprintf(part, sizeof part, " %u %s %u/%u/%u fge %u", kv.first, kv.first ? wotm::fxTypeName(kv.first) : "-", kv.second[0], kv.second[1],
                          kv.second[2], kv.second[3]);
            line += part;
        }
        std::fprintf(stderr, "[vu1check]   particles by type (matched/vu/native, fge = VU1 ones with PRIM.FGE):%s\n", line.c_str());
    }
    {   // color of the matched particles: total, per type and the systems with the most differences (1st difference: VU1 and native RGBA)
        uint32_t comp = 0, cor = 0, alfa = 0;
        std::map<uint32_t, std::array<uint32_t, 3>> byT;
        for (const Row &r : rows) {
            comp += r.comp; cor += r.cor; alfa += r.alfa;
            if (r.comp) { auto &t = byT[r.s->n ? r.s->type : 0u]; t[0] += r.comp; t[1] += r.cor; t[2] += r.alfa; }
        }
        std::string line;
        char part[112];
        for (const auto &kv : byT) {
            std::snprintf(part, sizeof part, " %u %s %u/%u/%u", kv.first, kv.first ? wotm::fxTypeName(kv.first) : "-", kv.second[1], kv.second[2], kv.second[0]);
            line += part;
        }
        std::fprintf(stderr, "[vu1check] g=%u particles color: %u matched with color | color differs %u | alpha differs %u | by type (color/alpha/matched):%s\n", g_frame, comp, cor,
                     alfa, line.c_str());
        std::vector<const Row *> worst;
        for (const Row &r : rows)
            if (r.cor + r.alfa) worst.push_back(&r);
        std::sort(worst.begin(), worst.end(), [](const Row *a, const Row *b) { return a->cor + a->alfa > b->cor + b->alfa; });
        for (size_t i = 0; i < worst.size() && i < 6; ++i) {
            const Row &r = *worst[i];
            std::fprintf(stderr, "[vu1check]   color fx %u preset %u type %u %s: %u matched, color %u alpha %u (vu %02x%02x%02x%02x native %02x%02x%02x%02x)\n", r.sys,
                         r.s->preset, r.s->type, r.s->n ? wotm::fxTypeName(r.s->type) : "-", r.comp, r.cor, r.alfa, r.vuC[0], r.vuC[1], r.vuC[2], r.vuC[3],
                         r.natC[0], r.natC[1], r.natC[2], r.natC[3]);
        }
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.vu + a.nat - 2 * a.m > b.vu + b.nat - 2 * b.m; });
    for (size_t i = 0; i < rows.size() && i < 8; ++i) {
        const Row &r = rows[i];
        const uint32_t diff = r.vu + r.nat - 2 * r.m;
        if (diff < 2 || 10 * diff < std::max(r.vu, r.nat)) continue;
        std::fprintf(stderr, "[vu1check]   fx %u preset %u type %u %s n=%d%s: vu %u native %u matched %u (loose %u)\n", r.sys, r.s->preset, r.s->type,
                     r.s->n ? wotm::fxTypeName(r.s->type) : "-", r.s->n, r.s->n && !r.s->built ? " no packet" : "", r.vu, r.nat, r.m, r.soltos);
        if ((!r.s->n || !r.vu) && g_rdram) {   // only on one side: raw state now (type, numParticles, info.attribs, emit.info.attribs, active?)
            const wotm::Ram ram{g_rdram, PS2_RAM_SIZE};
            const uint32_t b = kFx0 + r.sys * wotm::fxaddr::stride;
            bool listed = false;
            for (int32_t i = int32_t(ram.u32(wotm::fxaddr::fxActiveTop)), guard = 0; i >= 0 && i < 100 && guard < 100; ++guard) {
                if (uint32_t(i) == r.sys) { listed = true; break; }
                i = int32_t(ram.u32(wotm::fxaddr::fxList + 12 * uint32_t(i) + 8));
            }
            std::fprintf(stderr, "[vu1check]     raw: type %u n %u attribs %x emit %x preset %u active %d\n", ram.u32(b + wotm::fxoff::type) & 0xFF,
                         ram.u32(b + wotm::fxoff::numParticles), ram.u32(b + wotm::fxoff::infoAttribs), ram.u32(b + 0x2D80), ram.u32(b + wotm::fxoff::fxId), int(listed));
        }
    }
}

// [vu1check] lines line: XGKICK segments (PRIM 1/2) per fx[] system against the native FX_LINE (pos -> pos - vel, collectFx).
// Matches both ends within 1 px (in either order), with whichever camera matches more; "head only matches" = the particle's head (pos)
// within 1 px and the other end not (the tail comes from a different calculation). Among matched ones, color (RGB off by more than 4) and alpha (off by more than 4) of each end.
void compareLines()
{
    if (g_vuLines.empty()) {
        bool any = false;
        for (const FxMap &m : g_fxNats)
            for (const auto &kv : m)
                for (const auto &l : kv.second.lines) any = any || !l.empty();
        if (!any) return;
    }
    std::map<uint32_t, std::vector<Seg>> vuBy;   // system -> VU1 segments
    std::map<uint32_t, uint32_t> chainOf;        // system -> chain (the last program's)
    uint32_t orphan = 0;
    for (const auto &pl : g_vuLines) {
        const int sys = fxOf(pl.first);
        if (sys < 0) { ++orphan; continue; }
        vuBy[uint32_t(sys)].push_back(pl.second);
        chainOf[uint32_t(sys)] = g_progChain[pl.first - 1];
    }
    int lastFx = -1;
    for (size_t c = 0; c < g_fxChain.size(); ++c)
        if (g_fxChain[c]) lastFx = int(c);
    std::vector<uint32_t> keys;
    for (const auto &kv : vuBy) keys.push_back(kv.first);
    if (lastFx >= 0)
        for (const auto &kv : g_fxNats[size_t(lastFx)])
            if (kv.second.type == wotm::FX_LINE) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    auto near1 = [](float ax, float ay, float bx, float by) { return std::fabs(ax - bx) <= 1.f && std::fabs(ay - by) <= 1.f; };
    uint32_t totVu = 0, totNat = 0, totM = 0, totHead = 0, cor = 0, alfa = 0;
    uint8_t exVu[4] = {}, exNat[4] = {};
    bool ex = false;
    std::string worst;
    for (uint32_t sys : keys) {
        const std::vector<Seg> &vu = vuBy[sys];
        const size_t chain = chainOf.count(sys) ? chainOf[sys] : size_t(std::max(lastFx, 0));
        if (chain >= g_fxNats.size()) continue;
        const auto nit = g_fxNats[chain].find(sys);
        static const FxNat kNone;
        const FxNat &s = nit != g_fxNats[chain].end() ? nit->second : kNone;
        // best camera: most whole segments matched
        uint32_t bestM = 0, bestHead = 0;
        int bestCam = 0;
        std::vector<int> bestPair, bestFlip;
        for (int ci = 0; ci < kFxCams; ++ci) {
            const std::vector<Seg> &nat = s.lines[ci];
            if (ci && nat.empty()) continue;
            std::vector<char> used(nat.size(), 0);
            std::vector<int> pair(vu.size(), -1), flip(vu.size(), 0);
            uint32_t m = 0, head = 0;
            for (size_t i = 0; i < vu.size(); ++i) {
                const Seg &a = vu[i];
                int hd = -1;
                for (size_t j = 0; j < nat.size() && pair[i] < 0; ++j) {
                    if (used[j]) continue;
                    const Seg &b = nat[j];
                    const bool same = near1(a.x[0], a.y[0], b.x[0], b.y[0]) && near1(a.x[1], a.y[1], b.x[1], b.y[1]);
                    const bool rev = near1(a.x[0], a.y[0], b.x[1], b.y[1]) && near1(a.x[1], a.y[1], b.x[0], b.y[0]);
                    if (same || rev) { pair[i] = int(j); flip[i] = !same; used[j] = 1; ++m; }
                    else if (hd < 0 && (near1(a.x[0], a.y[0], b.x[0], b.y[0]) || near1(a.x[1], a.y[1], b.x[0], b.y[0]))) hd = int(j);
                }
                if (pair[i] < 0 && hd >= 0) ++head;
            }
            if (!ci || m > bestM) { bestM = m; bestHead = head; bestCam = ci; bestPair.swap(pair); bestFlip.swap(flip); }
        }
        const std::vector<Seg> &nat = s.lines[bestCam];
        totVu += uint32_t(vu.size()); totNat += uint32_t(nat.size()); totM += bestM; totHead += bestHead;
        for (size_t i = 0; i < vu.size(); ++i) {
            if (bestPair[i] < 0) continue;
            const Seg &a = vu[i], &b = nat[size_t(bestPair[i])];
            bool dc = false, da = false;
            for (int e = 0; e < 2; ++e) {
                const uint8_t *vc = a.c[e], *nc = b.c[bestFlip[i] ? 1 - e : e];
                for (int ch = 0; ch < 3; ++ch) dc = dc || std::abs(int(vc[ch]) - int(nc[ch])) > 4;
                da = da || std::abs(int(vc[3]) - int(nc[3])) > 4;
                if ((dc || da) && !ex) { std::memcpy(exVu, vc, 4); std::memcpy(exNat, nc, 4); ex = true; }
            }
            cor += dc; alfa += da;
        }
        if (vu.size() + nat.size() > 2 * bestM) {
            char b[160];
            std::snprintf(b, sizeof b, " | fx %u preset %u type %u: vu %zu native %zu matched %u (head only %u)", sys, s.preset, s.type, vu.size(), nat.size(), bestM, bestHead);
            if (worst.size() < 600) worst += b;
        }
        if (g_dump) {
            for (const Seg &a : vu) std::fprintf(g_dump, "line,vu,%u,%.3f,%.3f,%.3f,%.3f,%02x%02x%02x%02x,%02x%02x%02x%02x\n", sys, a.x[0], a.y[0], a.x[1], a.y[1],
                                                a.c[0][0], a.c[0][1], a.c[0][2], a.c[0][3], a.c[1][0], a.c[1][1], a.c[1][2], a.c[1][3]);
            for (const Seg &a : nat) std::fprintf(g_dump, "line,nat,%u,%.3f,%.3f,%.3f,%.3f,%02x%02x%02x%02x,%02x%02x%02x%02x\n", sys, a.x[0], a.y[0], a.x[1], a.y[1],
                                                  a.c[0][0], a.c[0][1], a.c[0][2], a.c[0][3], a.c[1][0], a.c[1][1], a.c[1][2], a.c[1][3]);
            // like in compareFx: comparison camera, raw matrices and particles (offline tooling redoes the projection of any end)
            const Cam &c = s.cams[bestCam];
            std::fprintf(g_dump, "linecam,%u", sys);
            for (int r = 0; r < 4; ++r)
                for (int k = 0; k < 4; ++k) std::fprintf(g_dump, ",%.6g", c.W[r][k]);
            std::fprintf(g_dump, ",%.6g,%.6g,%.6g\nlinemat,%u", c.eye[0], c.eye[1], c.eye[2], sys);
            for (float f : s.prevMat) std::fprintf(g_dump, ",%.7g", f);
            for (float f : s.curMat) std::fprintf(g_dump, ",%.7g", f);
            std::fprintf(g_dump, "\n");
            for (size_t k = 0; k < s.raw.size(); ++k) {
                std::fprintf(g_dump, "linep,%u,%zu", sys, k);
                for (float f : s.raw[k]) std::fprintf(g_dump, ",%.6g", f);
                std::fprintf(g_dump, "\n");
            }
        }
    }
    std::fprintf(stderr, "[vu1check] g=%u lines: XGKICK %u segments (%u outside fx[]) | vu %u native %u matched %u, head only matches %u | color differs %u alpha differs %u "
                         "(vu %02x%02x%02x%02x native %02x%02x%02x%02x)%s\n",
                 g_frame, g_vuLinesKicked, orphan, totVu, totNat, totM, totHead, cor, alfa, exVu[0], exVu[1], exVu[2], exVu[3], exNat[0], exNat[1], exNat[2], exNat[3],
                 worst.c_str());
}

// [vu1check] texels line: for each pair (XGKICK texture read from VRAM, native object and slot) of the triangles matched with the same
// texture, the GS texels (decodeVram) against the ones the native side decodes (Scene::decodeTex, raw alpha, without the color bleeding into
// transparent texels that textureId does). Compares RGB, and alpha only with TEX0.TCC = 1; with alpha 0 on both sides RGB does not count (the GS does
// not show it). Per pair: equal (max difference <= 1), close (<= 8: rounding, e.g. CT16's 5 bits) or differ. Not counted:
// the menu movie (texId 999) and the screen texture. In a fight the textures already live in the snapshot's VRAM (no upload in the frame); "with
// upload" counts the pairs whose texture (and palette, if indexed) the frame uploaded before drawing.
using TexelKey = std::tuple<int32_t, uint32_t, uint8_t, uint8_t>;   // index into g_refTex, object node, slot, TCC
void texStr(uint64_t v, char *s, size_t n);
std::map<uint32_t, uint32_t> texIdsByTbp(const uint8_t *rdram);
void compareTexels(const std::map<TexelKey, uint32_t> &pairs)
{
    if (!g_vram || pairs.empty()) return;
    const wotm::Ram ram{g_rdram, PS2_RAM_SIZE};
    g_scene->loadResources(ram, "", nullptr);
    const std::map<uint32_t, uint32_t> texIds = texIdsByTbp(g_rdram);
    uint32_t n = 0, tris = 0, iguais = 0, perto = 0, diferem = 0, outra = 0, tam = 0, semUp = 0, semClut = 0, fora = 0, vramFalhou = 0, natFalhou = 0;
    struct Worst { double pct; int maxd; uint32_t id, node, slot, tris, x, y, vc, nc, w, h, nw, nh; uint64_t tex0; };
    std::vector<Worst> worst;
    struct NatTex { bool ok = false; uint32_t w = 0, h = 0; std::vector<uint32_t> px; };
    std::map<std::pair<uint32_t, uint8_t>, NatTex> natCache;
    for (const auto &kv : pairs) {
        const auto [ri, node, slot, tcc] = kv.first;
        const RefTex &r = g_refTex[size_t(ri)];
        ++n;
        tris += kv.second;
        semUp += r.upTex && r.upClut;
        semClut += !r.upClut;
        if (!r.ok) { ++vramFalhou; continue; }
        if (wotm::Scene::texKey(ram, node, slot) == wotm::Scene::kScreenKey || wotm::Scene::objectTexId(ram, node, slot) == 999) { ++fora; continue; }
        NatTex &nt = natCache[{node, slot}];
        if (!nt.ok && nt.px.empty()) nt.ok = g_scene->decodeTex(ram, node, nt.px, nt.w, nt.h, slot, true) && nt.px.size() >= size_t(nt.w) * nt.h;
        const wotm::gs::Tex0 t = wotm::gs::Tex0::decode(r.tex0);
        const auto idIt = texIds.find(t.tbp0);
        const uint32_t id = idIt != texIds.end() ? idIt->second : wotm::Scene::objectTexId(ram, node, slot);
        if (!nt.ok) { ++natFalhou; continue; }
        if (nt.w != r.w || nt.h != r.h) {
            ++tam;
            worst.push_back({200.0, 255, id, node, slot, kv.second, 0, 0, 0, 0, r.w, r.h, nt.w, nt.h, r.tex0});
            continue;
        }
        int maxd = 0;
        size_t bad = 0;
        uint32_t wx = 0, wy = 0, wvc = 0, wnc = 0;
        for (uint32_t y = 0; y < r.h; ++y)
            for (uint32_t x = 0; x < r.w; ++x) {
                const uint32_t vc = r.px[size_t(y) * r.w + x], nc = nt.px[size_t(y) * r.w + x];
                const bool clear = tcc && (vc >> 24) == 0 && (nc >> 24) == 0;
                int d = 0;
                if (!clear)
                    for (int ch = 0; ch < 3; ++ch) d = std::max(d, std::abs(int((vc >> (8 * ch)) & 255) - int((nc >> (8 * ch)) & 255)));
                if (tcc) d = std::max(d, std::abs(int(vc >> 24) - int(nc >> 24)));
                if (d > 8) ++bad;
                if (d > maxd) { maxd = d; wx = x; wy = y; wvc = vc; wnc = nc; }
            }
        static const char *dbg = std::getenv("WOTM_VU1CHECK_TEXELS");   // diagnostic: <tex id> prints the palettes (VRAM and native) and the indices of row 0
        if (dbg && uint32_t(std::atoi(dbg)) == id) {
            const bool p8 = t.psm == wotm::gs::T8 || t.psm == wotm::gs::T8H;
            std::string a = "VRAM:", b = "native (.RTX):";
            char e[16];
            const auto pit = g_scene->rtxPal.find(t.cbp);
            for (uint32_t i = 0; i < (p8 ? 32u : 16u); ++i) {
                const uint32_t x = p8 ? (i & 7) | ((i & 0x10) >> 1) : (i & 7), y = p8 ? ((i & 0xE0) >> 4) | ((i & 8) >> 3) : (i >> 3);
                std::snprintf(e, sizeof e, " %08x", wotm::gs::rd32({g_vram, PS2_GS_VRAM_SIZE}, wotm::gs::addrCT32(t.cbp, 1, x, y)));
                a += e;
                const size_t k = p8 ? ((i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1)) : i;
                std::snprintf(e, sizeof e, " %08x", pit != g_scene->rtxPal.end() && k < pit->second.size() ? pit->second[k] : 0u);
                b += e;
            }
            std::fprintf(stderr, "[vu1check]   texels tex %u node %x slot %u tcc %u cpsm %u csa %u: palette %s | %s\n", id, node, slot, unsigned(tcc), t.cpsm, t.csa,
                         a.c_str(), b.c_str());
            for (int side = 0; side < 2; ++side) {   // texels_<id>_{vram,nat}.png in the current directory (raw alpha doubled to 0..255)
                std::vector<uint32_t> img(side ? nt.px : r.px);
                for (uint32_t &c : img) c = (c & 0x00FFFFFFu) | (std::min<uint32_t>(255, (c >> 24) * 2) << 24);
                Image im{img.data(), int(r.w), int(r.h), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                char name[64];
                std::snprintf(name, sizeof name, "texels_%u_%s.png", id, side ? "nat" : "vram");
                ExportImage(im, name);
            }
        }
        if (maxd <= 1) ++iguais;
        else if (maxd <= 8) ++perto;
        else {
            // Another texInfo texture uploaded to the same DBP that matches the VRAM (same palette and size): the VRAM holds that one, not the
            // object's (the two share the location and this one's upload did not happen before the snapshot). In that case the reference is what is wrong.
            uint32_t other = 0;
            for (uint32_t oid = 1; oid < 1000 && !other; ++oid) {
                const uint32_t ptr = ram.u32(wotm::addr::texInfo + 16 * oid) & 0x0FFFFFFF;
                if (oid == id || !ptr || !ram.ok(ptr, 0x80) || (ram.u32(ptr + 0x34) & 0x3FFF) != t.tbp0) continue;
                std::vector<uint32_t> op;
                uint32_t ow = 0, oh = 0;
                if (!g_scene->decodeUploadTex(ram, ptr, t, op, ow, oh, false, true) || ow != r.w || oh != r.h || op.size() < r.px.size()) continue;
                size_t diff = 0;
                for (size_t k = 0; k < r.px.size(); ++k) {
                    const uint32_t a = r.px[k], b = op[k];
                    if (tcc ? a != b : (a & 0xFFFFFFu) != (b & 0xFFFFFFu)) ++diff;
                }
                if (diff * 100 <= r.px.size()) other = oid;
            }
            if (other) { ++outra; continue; }
            ++diferem;
            worst.push_back({100.0 * double(bad) / double(size_t(r.w) * r.h), maxd, id, node, slot, kv.second, wx, wy, wvc, wnc, r.w, r.h, nt.w, nt.h, r.tex0});
        }
    }
    std::fprintf(stderr, "[vu1check] g=%u texels: %u texture x object pairs (%u matched tris) | equal %u, close %u, differ %u, size differs %u, VRAM holds another "
                         "texture of the same TBP %u | excluded: movie/screen %u, VRAM unreadable %u, native has no texture %u | uploaded in the frame %u (palette not uploaded %u) | uploads in the "
                         "frame: XGKICK %u DIRECT %u GIF channel %u\n",
                 g_frame, n, tris, iguais, perto, diferem, tam, outra, fora, vramFalhou, natFalhou, semUp, semClut, g_upPath[1], g_upPath[2], g_upPath[3]);
    std::sort(worst.begin(), worst.end(), [](const Worst &a, const Worst &b) { return a.pct * double(a.tris) > b.pct * double(b.tris); });
    for (size_t i = 0; i < worst.size() && i < 6; ++i) {
        const Worst &w = worst[i];
        char ts[96];
        texStr(w.tex0, ts, sizeof ts);
        if (w.pct > 100.0)
            std::fprintf(stderr, "[vu1check]   texels tex %u (%s) node %x slot %u: size vu %ux%u native %ux%u (%u tris)\n", w.id, ts, w.node, w.slot, w.w, w.h, w.nw, w.nh, w.tris);
        else
            std::fprintf(stderr, "[vu1check]   texels tex %u (%s) node %x slot %u: %.1f%% of texels differ (> 8), worst %d at (%u, %u): vu %08x native %08x (%u tris)\n", w.id,
                         ts, w.node, w.slot, w.pct, w.maxd, w.x, w.y, w.vc, w.nc, w.tris);
    }
}

// Attributes of a matched pair (Tri: GS units; see the comment on Tri). Counts triangles:
//  - uv: some vertex off by more than 1 texel (TW/TH of the VU1's TEX0); uvVolta: the same whole-number offset (wrapping) on all of them;
//  - cor: some RGB channel off by more than 4; corTeto: only because the VU1 goes above 0x80;
//  - alfa: with ABE on both, vertex alpha off by more than 4;
//  - abe: PRIM.ABE differs from MeshData::abe; alpha: with ABE on both, ALPHA_1/2 differs from the native equivalent;
//  - tfx: texture with TFX != MODULATE or TCC != RGBA (the native side always modulates with the texture's alpha); fge: with fog;
//  - fgeDifere: PRIM.FGE differs from Vtx::fge; neblina: with FGE on both, some vertex's F off by more than 4.
struct Attr {
    uint32_t n = 0, uv = 0, uvVolta = 0, cor = 0, corTeto = 0, alfa = 0, abe = 0, alpha = 0, tfx = 0, fge = 0, fgeDifere = 0, neblina = 0;
    uint8_t vuF = 0, natF = 0;                    // 1st differing F
    float du = 0.f, dv = 0.f;                     // 1st differing uv: offset in texels (VU1 - native) at the worst vertex
    uint8_t vuC[4] = {}, natC[4] = {};            // 1st differing color (or alpha)
    uint16_t vuAlpha = 0, natAlpha = 0;           // 1st differing ALPHA
    void add(const Attr &o) {
        n += o.n; uv += o.uv; uvVolta += o.uvVolta; cor += o.cor; corTeto += o.corTeto; alfa += o.alfa; abe += o.abe; alpha += o.alpha;
        tfx += o.tfx; fge += o.fge; fgeDifere += o.fgeDifere; neblina += o.neblina;
    }
    uint32_t diffs() const { return uv + cor + alfa + abe + alpha + fgeDifere + neblina; }
};

void compareAttr(const Tri &t, const Tri &n, Attr &at)
{
    ++at.n;
    int map[3];
    for (int a = 0; a < 3; ++a) {
        float best = 1e30f;
        for (int b = 0; b < 3; ++b) {
            const float d = std::max(std::fabs(t.x[a] - n.x[b]), std::fabs(t.y[a] - n.y[b]));
            if (d < best) { best = d; map[a] = b; }
        }
    }
    const bool textured = t.tex && t.tex != kTexUnknown;
    if (textured) {
        const float tw = float(1u << ((t.tex0 >> 26) & 0xF)), th = float(1u << ((t.tex0 >> 30) & 0xF));
        float worst = 0.f, du = 0.f, dv = 0.f;
        bool wrap = true;
        float wu = 0.f, wv = 0.f;
        for (int a = 0; a < 3; ++a) {
            const float eu = (t.u[a] - n.u[map[a]]) * tw, ev = (t.v[a] - n.v[map[a]]) * th;
            if (std::max(std::fabs(eu), std::fabs(ev)) > worst) { worst = std::max(std::fabs(eu), std::fabs(ev)); du = eu; dv = ev; }
            const float iu = std::round(t.u[a] - n.u[map[a]]), iv = std::round(t.v[a] - n.v[map[a]]);
            if (!a) { wu = iu; wv = iv; }
            wrap = wrap && iu == wu && iv == wv && std::fabs(eu - iu * tw) <= 1.f && std::fabs(ev - iv * th) <= 1.f;
        }
        if (worst > 1.f) {
            if (wrap) ++at.uvVolta;
            else if (!at.uv++) { at.du = du; at.dv = dv; }
        }
        const uint32_t tcc = uint32_t((t.tex0 >> 34) & 1), tfx = uint32_t((t.tex0 >> 35) & 3);
        if (tfx != 0 || tcc != 1) ++at.tfx;
    }
    bool cor = false, teto = false, alfa = false;
    for (int a = 0; a < 3; ++a) {
        const uint8_t *vc = t.c[a], *nc = n.c[map[a]];
        for (int k = 0; k < 3; ++k)
            if (std::abs(int(vc[k]) - int(nc[k])) > 4) (vc[k] > 0x80 && nc[k] <= 0x80 ? teto : cor) = true;
        if (t.abe && n.abe && std::abs(int(vc[3]) - int(nc[3])) > 4) alfa = true;
        if ((cor || alfa) && !at.cor && !at.alfa) { std::memcpy(at.vuC, vc, 4); std::memcpy(at.natC, nc, 4); }
    }
    at.cor += cor;
    at.corTeto += teto && !cor;
    at.alfa += alfa;
    at.abe += t.abe != n.abe;
    if (t.abe && n.abe && t.alpha != kAlphaUnknown && t.alpha != n.alpha && !at.alpha++) { at.vuAlpha = t.alpha; at.natAlpha = n.alpha; }
    at.fge += t.fge;
    at.fgeDifere += t.fge != n.fge;
    if (t.fge && n.fge)
        for (int a = 0; a < 3; ++a)
            if (std::abs(int(t.f[a]) - int(n.f[map[a]])) > 4) {
                if (!at.neblina) { at.vuF = t.f[a]; at.natF = n.f[map[a]]; }
                ++at.neblina;
                break;
            }
}

// GS SPRITEs of OBJECT (objType 2, wotm::ObjSprite) against the native side: projected center -/+ (512, 256) * side / W, like
// microprogram 0x2E58, with each pass's camera (whichever matches more wins). [vu1check] object sprites line.
void compareObjSprites(const std::vector<Quad> &vu)
{
    std::vector<Quad> nat[2];
    for (int ci = 0; ci < 2; ++ci) {
        const Cam &cam = g_cam[ci];
        for (const auto &kv : g_nats[0])
            for (const auto &s : kv.second.sprites) {
                const float d[3] = {s[0] - cam.eye[0], s[1] - cam.eye[1], s[2] - cam.eye[2]};
                const float w = cam.W[3][0] * d[0] + cam.W[3][1] * d[1] + cam.W[3][2] * d[2] + cam.W[3][3];
                float x, y;
                if (!(w >= 1.f) || !projectPt(cam, s.data(), x, y)) continue;
                const float hx = 512.f * s[3] / w, hy = 256.f * s[3] / w;
                const Quad q{{x - hx, x + hx, x + hx, x - hx}, {y - hy, y - hy, y + hy, y + hy}, 4};
                bool in = true;
                for (int k = 0; k < 4; ++k) in = in && q.x[k] >= kX0 && q.x[k] < kX1 && q.y[k] >= kY0 && q.y[k] < kY1;
                if (in) nat[ci].push_back(q);
            }
    }
    if (vu.empty() && nat[0].empty() && nat[1].empty()) return;
    const uint32_t m0 = matchQuads(vu, nat[0]), m1 = matchQuads(vu, nat[1]);
    const int ci = m1 > m0 ? 1 : 0;
    std::fprintf(stderr, "[vu1check] g=%u object sprites: vu %zu native %zu matched %u (pass %d camera)\n", g_frame, vu.size(), nat[ci].size(),
                 std::max(m0, m1), ci);
    if (g_dump)
        for (int s = 0; s < 2; ++s)
            for (const Quad &q : s ? nat[ci] : vu)
                std::fprintf(g_dump, "objspr,%s,%.3f,%.3f,%.3f,%.3f\n", s ? "nat" : "vu", q.x[0], q.y[0], q.x[2], q.y[2]);
}

// Texture as text: TBP, PSM, size, CLUT (or "no texture" / "tex ?").
void texStr(uint64_t v, char *s, size_t n)
{
    if (!v) { std::snprintf(s, n, "no texture"); return; }
    if (v == kTexUnknown) { std::snprintf(s, n, "tex ?"); return; }
    const wotm::gs::Tex0 t = wotm::gs::Tex0::decode(v);
    std::snprintf(s, n, "tbp %x psm %u %ux%u cbp %x csa %u", t.tbp0, t.psm, 1u << t.tw, 1u << t.th, t.cbp, t.csa);
}

// ---- Inventory ("[vu1check] inv" lines): what each side draws with no counterpart on the other, per owner ----
// Reference: VU1 triangles and SPRITEs with no native object and no fx[] (grouped by MSCAL source) and every PATH2/PATH3 primitive,
// grouped by (path, type, texture, FRAME, ABE). Native: objects from the 1st collection with triangles whose polyPkt the VU1 did not touch,
// including sky and icons (skip). "On both sides" per object already comes out in the comparison (missing/extra). Screen in pixels (0..640 x
// 0..224, corner of the GS screen); "FULL SCREEN" = a box covering the screen (fade, clear); "into texture" = FRAME outside the screen buffers.
struct InvGroup { uint32_t n = 0; float x0 = 1e9f, y0 = 1e9f, x1 = -1e9f, y1 = -1e9f; uint32_t src0 = ~0u, src1 = 0; uint8_t rgba[4] = {}; };
using InvKey = std::tuple<uint8_t, uint8_t, uint64_t, uint16_t, bool, uint32_t>;   // path, type (3 tri / 6 sprite), texture, FBP, ABE, source (VU1: MSCAL & ~0xFF)

// Name of the ELF data item containing the address (wotm::symtab, generated by tools/native_syms.py): "name+0x10"; "" if none (heap).
const char *symOf(uint32_t a, char *buf, size_t n)
{
    const wotm::symtab::Entry *e = wotm::symtab::find(a);
    if (!e) { buf[0] = 0; return buf; }
    if (a == e->addr) std::snprintf(buf, n, "%s", e->name);
    else std::snprintf(buf, n, "%s+0x%x", e->name, a - e->addr);
    return buf;
}

// Texture id (index into texInfo, the number the native side uses) from the TEX0's TBP0: the upload packet of texInfo[id] carries the DBP in
// BITBLTBUF (+0x30, bits 32..45) and the PSM (bits 56..61). Resident menu textures (texInfo points to something else) are left out.
std::map<uint32_t, uint32_t> texIdsByTbp(const uint8_t *rdram)
{
    std::map<uint32_t, uint32_t> out;
    if (!rdram) return out;
    const wotm::Ram ram{rdram, PS2_RAM_SIZE};
    for (uint32_t id = 1; id < 1000; ++id) {
        const uint32_t ptr = ram.u32(wotm::addr::texInfo + 16 * id) & 0x0FFFFFFF;
        if (!ptr || !ram.ok(ptr, 0x80)) continue;
        const uint32_t hi = ram.u32(ptr + 0x34);   // high word of BITBLTBUF: DBP 0..13, DBW 16..21, DPSM 24..29
        const uint32_t dbp = hi & 0x3FFF, dpsm = (hi >> 24) & 0x3F;
        if (dpsm > 0x34 || ram.u16(ptr + 0x50) == 0) continue;   // does not look like an upload packet
        out.emplace(dbp, id);
    }
    return out;
}

void inventory(const std::map<uint32_t, std::map<uint32_t, std::vector<Tri>>> &vuBy, const std::map<uint32_t, uint32_t> &skipVu,
               const std::vector<const Tri *> &orphanTris, const std::vector<size_t> &orphanRects)
{
    const std::map<uint32_t, uint32_t> texIds = texIdsByTbp(g_rdram);
    auto texName = [&](uint64_t tex, char *s, size_t n) {
        texStr(tex, s, n);
        if (!tex || tex == kTexUnknown) return;
        const auto it = texIds.find(uint32_t(tex & 0x3FFF));
        if (it != texIds.end()) {
            char t[96];
            std::snprintf(t, sizeof t, "tex %u (%s)", it->second, s);
            std::snprintf(s, n, "%s", t);
        }
    };
    // HUD (Scene::hud): by id, with the VU1 triangles that landed in its packets (the per-object comparison includes them).
    {
        std::map<uint16_t, std::pair<uint32_t, uint32_t>> hud;   // id -> (objects, VU1 tris)
        if (!g_nats.empty())
            for (const auto &kv : g_nats[0])
                if (kv.second.hudId) {
                    auto &h = hud[kv.second.hudId];
                    ++h.first;
                    const auto vb = vuBy.find(kv.first);
                    if (vb != vuBy.end()) for (const auto &c : vb->second) h.second += uint32_t(c.second.size());
                }
        uint32_t tris = 0;
        for (const auto &kv : hud) tris += kv.second.second;
        if (!hud.empty()) {
            std::string s;
            char b[48];
            size_t shown = 0;
            for (const auto &kv : hud) {
                if (!kv.second.second) continue;   // the VU1 did not draw this id in this frame (off screen or hidden)
                if (shown++ >= 40) { s += " ..."; break; }
                std::snprintf(b, sizeof b, " %x:%u", unsigned(kv.first), kv.second.second);
                s += b;
            }
            std::fprintf(stderr, "[vu1check] inv   HUD: %zu ids, %u VU1 tris; by id (id:tris):%s\n", hud.size(), tris, s.c_str());
        }
    }
    std::map<InvKey, InvGroup> groups;
    auto add = [&](const InvKey &k, float x0, float y0, float x1, float y1, uint32_t src, const uint8_t *rgba) {
        InvGroup &g = groups[k];
        if (!g.n && rgba) std::memcpy(g.rgba, rgba, 4);
        ++g.n;
        g.x0 = std::min(g.x0, x0); g.y0 = std::min(g.y0, y0); g.x1 = std::max(g.x1, x1); g.y1 = std::max(g.y1, y1);
        if (src) { g.src0 = std::min(g.src0, src); g.src1 = std::max(g.src1, src); }
    };
    auto origin = [&](uint32_t prog) { return g_progs[prog - 1].empty() ? 0u : g_progs[prog - 1].back() & ~0xFFu; };
    uint32_t nVuTris = 0, nVuRects = 0, nP2 = 0, nP3 = 0, nRtt = 0;
    for (const Tri *t : orphanTris) {
        add({1, 3, t->tex, t->fbp, t->abe, origin(t->prog)}, std::min({t->x[0], t->x[1], t->x[2]}), std::min({t->y[0], t->y[1], t->y[2]}),
            std::max({t->x[0], t->x[1], t->x[2]}), std::max({t->y[0], t->y[1], t->y[2]}), 0, t->c[0]);
        ++nVuTris;
    }
    for (size_t i : orphanRects) {
        if (i >= g_vuRectInfo.size()) continue;
        const RefPrim &r = g_vuRectInfo[i];
        add({1, 6, r.tex, r.fbp, r.abe, origin(g_vuRects[i].first)}, r.x0, r.y0, r.x1, r.y1, 0, r.rgba);
        ++nVuRects;
    }
    for (const RefPrim &r : g_ref23) {
        add({r.path, uint8_t(r.kind == 6 ? 6 : 3), r.tex, r.fbp, r.abe, 0}, r.x0, r.y0, r.x1, r.y1, r.src, r.rgba);
        ++(r.path == 2 ? nP2 : nP3);
        if (r.fbp != 0xFFFF && r.fbp != 0x46 && r.fbp != 0x8C) ++nRtt;
    }
    // Native with no reference: objects from the 1st collection whose polyPkt no VU1 triangle touched
    struct NatOnly { uint32_t pp; const NatObj *o; uint32_t tris, onScreen; float x0, y0, x1, y1; uint8_t aMax = 0; };   // aMax: highest vertex alpha (0 = invisible: the native side "draws" it but it does not show)
    std::vector<NatOnly> natOnly;
    uint32_t bothObjs = 0, natOnlyTris = 0, natOnlySkip = 0;
    if (!g_nats.empty())
        for (const auto &kv : g_nats[0]) {
            const NatObj &o = kv.second;
            if (vuBy.count(kv.first) || skipVu.count(kv.first)) { ++bothObjs; continue; }
            if (o.tris.empty()) continue;
            const std::vector<Tri> pr = project(o.tris, g_cam[0], kv.first);
            NatOnly r{kv.first, &o, uint32_t(o.tris.size()), uint32_t(pr.size()), 1e9f, 1e9f, -1e9f, -1e9f};
            for (const Tri &t : pr)
                for (int c = 0; c < 3; ++c) {
                    r.x0 = std::min(r.x0, t.x[c]); r.y0 = std::min(r.y0, t.y[c]); r.x1 = std::max(r.x1, t.x[c]); r.y1 = std::max(r.y1, t.y[c]);
                    r.aMax = std::max(r.aMax, t.c[c][3]);
                }
            natOnlyTris += r.onScreen;
            natOnlySkip += o.skip;
            natOnly.push_back(r);
        }
    uint32_t refPrims = 0;
    for (const auto &kv : groups) refPrims += kv.second.n;
    std::fprintf(stderr, "[vu1check] inv g=%u reference only: %zu groups, %u primitives (unowned vu1 %u tris + %u sprites; path2 %u; path3 %u; into a texture %u) | "
                         "native only: %zu objects, %u tris on screen (sky/icons %u) | objects on both sides %u\n",
                 g_frame, groups.size(), refPrims, nVuTris, nVuRects, nP2, nP3, nRtt, natOnly.size(), natOnlyTris, natOnlySkip, bothObjs);
    std::vector<std::pair<InvKey, InvGroup>> gs(groups.begin(), groups.end());
    std::sort(gs.begin(), gs.end(), [](const auto &a, const auto &b) { return a.second.n > b.second.n; });
    constexpr size_t kMaxLines = 24;
    for (size_t i = 0; i < gs.size() && i < kMaxLines; ++i) {
        const InvKey &k = gs[i].first;
        const InvGroup &g = gs[i].second;
        char tex[128], where[160] = "", src[192] = "", sym[96];
        texName(std::get<2>(k), tex, sizeof tex);
        if (std::get<0>(k) == 1) {
            symOf(std::get<5>(k), sym, sizeof sym);
            std::snprintf(where, sizeof where, "vu1 mscal %x%s%s%s", std::get<5>(k), sym[0] ? " (" : "", sym, sym[0] ? ")" : "");
        } else std::snprintf(where, sizeof where, "path%u", unsigned(std::get<0>(k)));
        if (g.src0 != ~0u) {
            symOf(g.src0, sym, sizeof sym);
            std::snprintf(src, sizeof src, " src %x-%x%s%s%s", g.src0, g.src1, sym[0] ? " (" : "", sym, sym[0] ? ")" : "");
        }
        const uint16_t fbp = std::get<3>(k);
        const bool full = g.x0 <= kX0 + 2.f && g.y0 <= kY0 + 2.f && g.x1 >= kX1 - 2.f && g.y1 >= kY1 - 2.f;
        std::fprintf(stderr, "[vu1check] inv   ref %s %s x%u: %s fbp %x abe %d color %02x%02x%02x%02x screen %.0f,%.0f-%.0f,%.0f%s%s%s\n", where,
                     std::get<1>(k) == 6 ? "sprites" : "tris", g.n, tex, unsigned(fbp), int(std::get<4>(k)), g.rgba[0], g.rgba[1], g.rgba[2], g.rgba[3],
                     g.x0 - kX0, g.y0 - kY0, g.x1 - kX0, g.y1 - kY0, full ? " FULL SCREEN" : "",
                     fbp != 0xFFFF && fbp != 0x46 && fbp != 0x8C ? " into texture" : "", src);
    }
    if (gs.size() > kMaxLines) std::fprintf(stderr, "[vu1check] inv   ref ... and %zu more groups\n", gs.size() - kMaxLines);
    std::sort(natOnly.begin(), natOnly.end(), [](const NatOnly &a, const NatOnly &b) { return a.onScreen > b.onScreen; });
    size_t shown = 0;
    for (const NatOnly &r : natOnly) {
        if (!r.onScreen) continue;   // native object entirely off screen: not interesting
        if (shown++ >= kMaxLines) break;
        std::fprintf(stderr, "[vu1check] inv   nat pkt %x node %x tex %u items %d%s%s: tris %u (on screen %u) max alpha %02x screen %.0f,%.0f-%.0f,%.0f flags %x\n", r.pp, r.o->node,
                     unsigned(r.o->tex), r.o->items, r.o->skinned ? " skinned" : "", r.o->skip ? " sky/icon" : "", r.tris, r.onScreen, r.aMax,
                     r.x0 - kX0, r.y0 - kY0, r.x1 - kX0, r.y1 - kY0, g_rdram ? unsigned(g_rdram[r.o->node + 0xC] | (g_rdram[r.o->node + 0xD] << 8)) : 0u);
    }
    if (shown > kMaxLines) std::fprintf(stderr, "[vu1check] inv   nat ... and %zu more objects\n", shown - kMaxLines);
}

void compare()
{
    if (g_progs.empty() || g_nats.empty()) {
        std::fprintf(stderr, "[vu1check] g=%u: no MSCAL in the frame (did hierFlushObjQ not fire the DMA?)\n", g_frame);
        inventory({}, {}, {}, {});
        return;
    }
    // VU1 triangles per (object, chain); the ones with no native object are grouped by MSCAL source
    std::map<uint32_t, std::map<uint32_t, std::vector<Tri>>> vuBy;
    std::map<uint32_t, uint32_t> orphan;
    FxVuMap fxVu;   // particles: per system (fx[]) and chain
    uint32_t fxTris = 0, rectsOrphan = 0;
    std::vector<Quad> objRects;   // GS SPRITEs of OBJECT (ObjSprite)
    std::vector<const Tri *> orphanTriRefs;   // inventory: VU1 triangles and SPRITEs with no native object and no fx[]
    std::vector<size_t> orphanRects;
    std::map<uint32_t, uint32_t> skipVu;   // inventory: VU1 triangles per skip object (sky, icons): only counted
    for (size_t ri = 0; ri < g_vuRects.size(); ++ri) {
        const auto &r = g_vuRects[ri];
        if (const int sys = fxOf(r.first); sys >= 0) {
            if (!flareSys(sys)) fxVu[uint32_t(sys)][g_progChain[r.first - 1]].rects.push_back(r.second);
        }
        else if (objectOf(g_nats[g_progChain[r.first - 1]], r.first)) objRects.push_back(r.second);
        else {
            ++rectsOrphan;
            orphanRects.push_back(ri);
            if (g_dump) {   // program sources (DMA chain segments) of the SPRITE outside fx[]: map them with the .map
                std::fprintf(g_dump, "rectorf,%u", r.first);
                for (uint32_t a : g_progs[r.first - 1]) std::fprintf(g_dump, ",%x", a);
                std::fprintf(g_dump, "\n");
            }
        }
    }
    // Render-to-texture: triangles drawn into a FRAME outside the two screen buffers (FBP 0x46 and 0x8C, 640x224) do not go to the
    // screen; they are left out. That is the projected-shadow frame (FBP 0x1F3 = tbp 0x3E60), which receives the untextured monster
    // silhouettes from psBlkDma; some receiver does not always use that texture in the same frame (start of the fight).
    uint32_t rtt = 0;
    for (const Tri &t : g_vu) {
        if (t.fbp != 0xFFFF && t.fbp != 0x46 && t.fbp != 0x8C) { ++rtt; continue; }
        const uint32_t chain = g_progChain[t.prog - 1];
        const uint32_t key = objectOf(g_nats[chain], t.prog);
        if (key) {
            if (!g_nats[chain].at(key).skip) vuBy[key][chain].push_back(t);
            else ++skipVu[key];
            continue;
        }
        if (const int sys = fxOf(t.prog); sys >= 0) {
            fxVu[uint32_t(sys)][chain].tris.push_back(t);
            ++fxTris;
            continue;
        }
        ++orphan[g_progs[t.prog - 1].empty() ? 0u : g_progs[t.prog - 1].back() & ~0xFFu];
        orphanTriRefs.push_back(&t);
        if (g_dump) std::fprintf(g_dump, "0,orf,0,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llx,%x\n", t.prog, t.x[0], t.y[0], t.x[1], t.y[1], t.x[2], t.y[2],
                                 (unsigned long long)t.tex, unsigned(t.fbp));
    }
    if (g_dump) {   // CPU lights: paraL (0x6DFC00, directions x32 in the columns) and vu1LightColor (D_0025B238, colors/128 and ambient)
        std::fprintf(g_dump, "lights");
        for (uint32_t a : {wotm::sym::paraL, 0x25B238u})
            for (int k = 0; k < 16; ++k) { float f; std::memcpy(&f, g_rdram + a + 4 * k, 4); std::fprintf(g_dump, ",%g", f); }
        float spec;
        std::memcpy(&spec, g_rdram + wotm::sym::specularColor, 4);   // specularColor
        std::fprintf(g_dump, ",%g", spec);
        std::fprintf(g_dump, "\n");
    }
    if (g_dump)
        for (size_t i = 0; i < g_progs.size(); ++i) {
            std::fprintf(g_dump, "prog,%zu,%u", i + 1, g_progChain[i]);
            for (uint32_t a : g_progs[i]) std::fprintf(g_dump, ",%x", a);
            std::fprintf(g_dump, "\n");
            if (i < g_progHdr.size()) {
                std::fprintf(g_dump, "hdr,%zu,%x", i + 1, g_progHdr[i].addr);
                for (int k = 0; k < 8; ++k)
                    for (int c = 0; c < 4; ++c) std::fprintf(g_dump, ",%g", g_progHdr[i].q[k][c]);
                std::fprintf(g_dump, "\n");
                if (g_progHdr[i].addr == 0x15D8u) {   // point lights: q169-q171 (-light in object space, transposed), q172-q174 (colors), q175 (falloffs)
                    std::fprintf(g_dump, "ptl,%zu", i + 1);
                    for (int k = 0; k < 7; ++k)
                        for (int c = 0; c < 4; ++c) std::fprintf(g_dump, ",%g", g_progHdr[i].pt[k][c]);
                    std::fprintf(g_dump, "\n");
                }
            }
        }
    // texDifere: matched with a different texture; vuTex/natTex/natSlot: the object's most common differing pair
    struct Row { uint32_t pp; const NatObj *o; uint32_t vu, nat, falta, sobra, costas, texDifere; uint64_t vuTex, natTex; uint8_t natSlot; Attr at; };
    std::vector<Row> rows;
    uint32_t totVu = 0, totNat = 0, totFalta = 0, totSobra = 0, totCostas = 0, totRep = 0, objVu = 0, objNat = 0, objBoth = 0, bad = 0;
    uint32_t totTexDifere = 0, totTexUnknown = 0, totShadowOnly = 0, totRecorte = 0;
    Attr attrTot;
    std::map<TexelKey, uint32_t> texels;   // pairs (VRAM texture, object, slot, TCC) of the ones matched with the same texture: compareTexels
    std::vector<uint32_t> keys;   // objects from the 1st collection and the ones the VU1 drew
    for (const auto &kv : g_nats[0]) keys.push_back(kv.first);
    for (const auto &kv : vuBy) keys.push_back(kv.first);
    std::sort(keys.begin(), keys.end());
    keys.erase(std::unique(keys.begin(), keys.end()), keys.end());
    for (uint32_t key : keys) {
        // the object's reference collection: the one from the chain that brought the most of its triangles; with none, the 1st
        uint32_t chain = 0;
        std::vector<Tri> vu;
        const auto vit = vuBy.find(key);
        if (vit != vuBy.end()) {
            size_t most = 0;
            for (const auto &kv : vit->second) {
                vu.insert(vu.end(), kv.second.begin(), kv.second.end());
                if (kv.second.size() > most) { most = kv.second.size(); chain = kv.first; }
            }
        }
        const auto oit = g_nats[chain].find(key);
        if (oit == g_nats[chain].end() || oit->second.skip) continue;
        const NatObj *op = &oit->second;
        // camera: the one from the hier that built the packet (copied into it); a pass 0 chain can go out through the interrupt already inside pass 1,
        // so whichever matches more wins. Collection: the chain's and, if not everything matches, the 1st chain's (the alpha OT only goes out at the end
        // of the frame, when the animation of parts attached to the monsters may already have advanced; the packet carries the hier's matrices).
        std::vector<Tri> nat, natCulled;
        std::vector<int> vuPair;
        std::vector<char> natHit;
        uint32_t m = 0;
        bool first = true;
        const NatObj *cands[3] = {op, chain && g_nats[0].count(key) ? &g_nats[0].at(key) : nullptr,
                                  g_natPass1.count(key) ? &g_natPass1.at(key) : nullptr};
        for (const NatObj *cand : cands) {
            if (!cand || (!first && m >= vu.size())) continue;
            for (int ci = 0; ci < 2; ++ci) {
                if (!first && m >= vu.size()) break;
                std::vector<Tri> c1;
                std::vector<Tri> n1 = project(cand->tris, g_cam[ci], key, &c1);
                std::vector<int> p1;
                std::vector<char> h1;
                const uint32_t m1 = match(vu, n1, p1, h1);
                if (first || m1 > m) { m = m1; nat.swap(n1); natCulled.swap(c1); vuPair.swap(p1); natHit.swap(h1); op = cand; }
                first = false;
            }
        }
        const NatObj &o = *op;
        bad += o.badVerts;
        // Repeated passes: the VU1 redraws the same triangle (the projected shadow re-sends every object that receives it, once per
        // monster, in the alpha OT: psAttachDmaToAlpha). What is left over and matches an already-matched native one counts as repeated, not missing.
        uint32_t rep = 0;
        for (int round = 0; round < 4 && m + rep < vu.size(); ++round) {
            std::vector<size_t> idx;
            std::vector<Tri> rest;
            for (size_t i = 0; i < vu.size(); ++i)
                if (vuPair[i] == -1) { idx.push_back(i); rest.push_back(vu[i]); }
            std::vector<int> pair2;
            std::vector<char> hit2;
            if (!match(rest, nat, pair2, hit2)) break;
            for (size_t j = 0; j < rest.size(); ++j)
                if (pair2[j] >= 0) { vuPair[idx[j]] = -2; ++rep; }   // -2 = repeated (left out of the texture comparison)
        }
        // Back faces via clipping: a triangle that leaves the clipping band (CLIP/FCAND in the microprogram) goes down the clipping path,
        // which does not cull back faces; the native side culls them (Scene::backFacing) and they stay hidden behind the front faces.
        uint32_t rec = 0;
        if (m + rep < vu.size() && !natCulled.empty()) {
            std::vector<size_t> idx;
            std::vector<Tri> rest;
            for (size_t i = 0; i < vu.size(); ++i)
                if (vuPair[i] == -1) { idx.push_back(i); rest.push_back(vu[i]); }
            std::vector<int> pair2;
            std::vector<char> hit2;
            match(rest, natCulled, pair2, hit2);
            for (size_t j = 0; j < rest.size(); ++j)
                if (pair2[j] >= 0) { vuPair[idx[j]] = -3; ++rec; }   // -3 = back face via clipping
        }
        totRecorte += rec;
        // texture of the matched ones: the GS's at the XGKICK against the one the native side chose for the section's slot
        // Matched only through the shadow: a PSMCT32 texture the native object does not use (the projected-shadow frame, tbp 3e60) on a triangle
        // that no base pass matched; it is left out of "texture differs".
        uint32_t texDifere = 0;
        std::map<std::tuple<uint64_t, uint64_t, uint8_t>, uint32_t> texPairs;
        std::vector<uint64_t> natTexs;
        for (const Tri &n : nat) natTexs.push_back(n.tex);
        std::sort(natTexs.begin(), natTexs.end());
        natTexs.erase(std::unique(natTexs.begin(), natTexs.end()), natTexs.end());
        for (size_t i = 0; i < vu.size(); ++i) {
            if (vuPair[i] < 0) continue;
            const Tri &n = nat[size_t(vuPair[i])];
            if (vu[i].tex == kTexUnknown) ++totTexUnknown;
            else if (!sameTex(vu[i].tex, n.tex)) {
                const bool shadow = vu[i].tex && wotm::gs::Tex0::decode(vu[i].tex).psm == 0 &&
                                    std::none_of(natTexs.begin(), natTexs.end(), [&](uint64_t nt) { return sameTex(vu[i].tex, nt); });
                if (shadow) { ++totShadowOnly; continue; }
                ++texDifere; ++texPairs[{vu[i].tex, n.tex, n.slot}];
            }
        }
        std::tuple<uint64_t, uint64_t, uint8_t> texWorst{};
        uint32_t texWorstN = 0;
        for (const auto &kv : texPairs)
            if (kv.second > texWorstN) { texWorstN = kv.second; texWorst = kv.first; }
        totTexDifere += texDifere;
        // Attributes of the matched ones (except repeated and shadow-only): each VU1 vertex with the nearest native vertex on screen.
        Attr at{};
        for (size_t i = 0; i < vu.size(); ++i) {
            if (vuPair[i] < 0) continue;
            const Tri &t = vu[i], &n = nat[size_t(vuPair[i])];
            if (t.tex && t.tex != kTexUnknown && !sameTex(t.tex, n.tex)) continue;   // different texture already counted (or shadow)
            compareAttr(t, n, at);
            if (t.refTex >= 0 && n.slot != wotm::kSlotNoTex) ++texels[{t.refTex, o.node, n.slot, uint8_t((t.tex0 >> 34) & 1)}];
        }
        attrTot.add(at);
        if (vu.empty() && nat.empty()) continue;
        uint32_t vuBack = 0, costas = 0, sobra = 0;
        for (const Tri &t : vu) vuBack += area2(t) > 0.f;
        const bool culled = vuBack * 50 <= vu.size();
        for (size_t i = 0; i < nat.size(); ++i)
            if (!natHit[i]) ++((culled && area2(nat[i]) > 0.f) ? costas : sobra);
        const uint32_t nVu = uint32_t(vu.size()), nNat = uint32_t(nat.size()) - costas, falta = nVu - m - rep - rec;
        objVu += nVu > 0; objNat += nNat > 0; objBoth += nVu && nNat;
        totVu += nVu; totNat += nNat; totFalta += falta; totSobra += sobra; totCostas += costas; totRep += rep;
        rows.push_back({key, &o, nVu - rep, nNat, falta, sobra, costas, texDifere, std::get<0>(texWorst), std::get<1>(texWorst), std::get<2>(texWorst), at});
        if (g_dump) {
            // after the FBP: u, v and RGBA (hex) of each vertex, ABE, ALPHA, fog, the full TEX0 and TEST_1 (both VU1 only)
            auto row = [&](const char *side, int hit, const Tri &t) {
                std::fprintf(g_dump, "%x,%s,%d,%u,%.3f,%.3f,%.3f,%.3f,%.3f,%.3f,%llx,%x", key, side, hit, t.prog, t.x[0], t.y[0], t.x[1], t.y[1], t.x[2],
                             t.y[2], (unsigned long long)t.tex, unsigned(t.fbp));
                for (int k = 0; k < 3; ++k)
                    std::fprintf(g_dump, ",%.5f,%.5f,%02x%02x%02x%02x", t.u[k], t.v[k], t.c[k][0], t.c[k][1], t.c[k][2], t.c[k][3]);
                std::fprintf(g_dump, ",%d,%x,%d,%llx,%x\n", int(t.abe), unsigned(t.alpha), int(t.fge), (unsigned long long)t.tex0, t.test);   // TEST: ffffffff = native/not seen
            };
            for (size_t i = 0; i < vu.size(); ++i) row("vu", vuPair[i] >= 0, vu[i]);
            for (size_t i = 0; i < nat.size(); ++i) row("nat", natHit[i], nat[i]);
            std::fprintf(g_dump, "obj,%u,%x,%x,%u,%d,%d,%x,%x\n", chain, key, o.node, unsigned(o.tex), o.skinned, o.items, o.span,
                         unsigned(g_rdram[o.node + 0xC] | (g_rdram[o.node + 0xD] << 8)));   // OBJECT flags
        }
    }
    uint32_t orphanTris = 0;
    for (const auto &kv : orphan) orphanTris += kv.second;
    const double pct = totVu ? 100.0 * (totVu - totRep - totFalta) / (totVu - totRep) : 0.0;
    std::fprintf(stderr, "[vu1check] g=%u objects vu %u native %u both %u | tris vu %u (repeated %u) native %u matched %.1f%% missing %u extra %u "
                         "(back faces %u) | back faces via clipping %u | texture differs %u, no TEX0 %u, shadow only %u | XGKICK %u tris, %u off screen, %u into a texture (shadow) | particles %u tris, no object %u tris, chains %u (native collection %.0f + %.0f ms), programs %zu, "
                         "non-finite vertices %u, VIF1 out of step %u\n",
                 g_frame, objVu, objNat, objBoth, totVu, totRep, totNat, pct, totFalta, totSobra, totCostas, totRecorte, totTexDifere, totTexUnknown, totShadowOnly, g_vuKicked, g_vuOff, rtt, fxTris, orphanTris, g_chains,
                 g_walkMs, g_projMs, g_progs.size(), bad, g_wotmVif1Bad);
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.falta + a.sobra > b.falta + b.sobra; });
    int shown = 0;
    for (const Row &r : rows) {
        if (shown >= 8 || r.falta + r.sobra < 4 || 10 * (r.falta + r.sobra) < std::max(r.vu, r.nat)) continue;
        ++shown;
        std::fprintf(stderr, "[vu1check]   pkt %x node %x tex %u%s items %d: vu %u native %u missing %u extra %u (back faces %u) flags %x\n", r.pp,
                     r.o->node, unsigned(r.o->tex), r.o->skinned ? " skinned" : "", r.o->items, r.vu, r.nat, r.falta, r.sobra, r.costas,
                     unsigned(g_rdram[r.o->node + 0xC] | (g_rdram[r.o->node + 0xD] << 8)));   // OBJECT flags (0x8000 = alpha OT)
    }
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.texDifere > b.texDifere; });
    for (size_t i = 0; i < rows.size() && i < 8 && rows[i].texDifere; ++i) {
        const Row &r = rows[i];
        char a[96], b[96];
        texStr(r.vuTex, a, sizeof a);
        texStr(r.natTex, b, sizeof b);
        std::fprintf(stderr, "[vu1check]   texture pkt %x node %x tex %u%s: %u of %u matched; most common: vu (%s) native slot %u (%s)\n", r.pp,
                     r.o->node, unsigned(r.o->tex), r.o->skinned ? " skinned" : "", r.texDifere, r.vu - r.falta, a, unsigned(r.natSlot), b);
    }
    std::fprintf(stderr, "[vu1check] g=%u attributes: %u matched | uv differs %u (whole wrap %u) | color differs %u (only above 0x80 %u) | vertex alpha differs %u | "
                         "ABE differs %u | ALPHA differs %u | nonstandard TFX/TCC %u | fog %u (FGE differs %u, F differs %u)\n",
                 g_frame, attrTot.n, attrTot.uv, attrTot.uvVolta, attrTot.cor, attrTot.corTeto, attrTot.alfa, attrTot.abe, attrTot.alpha, attrTot.tfx, attrTot.fge,
                 attrTot.fgeDifere, attrTot.neblina);
    std::sort(rows.begin(), rows.end(), [](const Row &a, const Row &b) { return a.at.diffs() > b.at.diffs(); });
    for (size_t i = 0; i < rows.size() && i < 8 && rows[i].at.diffs(); ++i) {
        const Row &r = rows[i];
        const Attr &a = r.at;
        std::fprintf(stderr, "[vu1check]   attributes pkt %x node %x tex %u%s flags %x: %u matched; uv %u (%+.1f %+.1f texels) color %u alpha %u (vu %02x%02x%02x%02x native %02x%02x%02x%02x) "
                             "ABE %u ALPHA %u (vu %02x native %02x) tfx %u fog %u (FGE differs %u, F differs %u: vu %u native %u)\n",
                     r.pp, r.o->node, unsigned(r.o->tex), r.o->skinned ? " skinned" : "", unsigned(g_rdram[r.o->node + 0xC] | (g_rdram[r.o->node + 0xD] << 8)), a.n,
                     a.uv, a.du, a.dv, a.cor, a.alfa, a.vuC[0], a.vuC[1], a.vuC[2], a.vuC[3], a.natC[0], a.natC[1], a.natC[2], a.natC[3], a.abe, a.alpha,
                     unsigned(a.vuAlpha), unsigned(a.natAlpha), a.tfx, a.fge, a.fgeDifere, a.neblina, unsigned(a.vuF), unsigned(a.natF));
    }
    std::vector<std::pair<uint32_t, uint32_t>> orph(orphan.begin(), orphan.end());
    std::sort(orph.begin(), orph.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    for (size_t i = 0; i < orph.size() && i < 3; ++i)
        if (orph[i].second >= 8) std::fprintf(stderr, "[vu1check]   VU1 only: MSCAL at %x, %u tris\n", orph[i].first, orph[i].second);
    compareTexels(texels);
    compareFx(fxVu, rectsOrphan);
    compareLines();
    compareObjSprites(objRects);
    compareFlare();
    inventory(vuBy, skipVu, orphanTriRefs, orphanRects);
}

// hier entry (0x2029B0): a0 = view, a1 = pass, ra = where it came from (rtMain: 0x188750 pass 0, 0x18891C pass 1).
// Starts a sampled frame at the entry of hier(0, 0): resets the check state and turns on the DMA tap.
void arm(const uint8_t *rdram, uint32_t frame)
{
    g_rdram = rdram;
    g_frame = frame;
    g_pass = 0;
    g_cam[0] = readCam(rdram);
    g_nats.clear();
    g_natPass1.clear();
    g_fxNats.clear();
    g_fxChain.clear();
    g_progs.clear();
    g_progChain.clear();
    g_progHdr.clear();
    g_vu.clear();
    g_vuAll.clear();
    g_vuKicked = g_vuOff = 0;
    std::memset(g_vuPrimVerts, 0, sizeof g_vuPrimVerts);
    g_vuRects.clear();
    g_vuFlare.clear();
    g_flareVuIn.clear();
    g_ref23.clear();
    g_vuLines.clear();
    g_vuLinesKicked = 0;
    g_refTex.clear();
    g_refTexIdx.clear();
    g_upSeq.clear();
    g_upCount = 0;
    std::memset(g_upPath, 0, sizeof g_upPath);
    g_bitbltDbp = 0;
    g_vuRectInfo.clear();
    g_vuRectsKicked = g_vuRectsRtt = 0;
    g_tex0[0] = g_tex0[1] = kTexUnknown;
    g_fbp[0] = g_fbp[1] = 0xFFFF;
    g_alpha[0] = g_alpha[1] = kAlphaUnknown;
    g_test[0] = g_test[1] = 0xFFFFFFFFu;
    g_fogF = 255;
    g_chains = 0;
    g_walkMs = g_projMs = 0.0;
    g_armed = true;
    g_wotmVu1Tap = true;
}

// End of the sampled frame: compares (with the WOTM_VU1CHECK_DUMP CSV, if requested) and turns off the tap.
void finish()
{
    g_wotmVu1Tap = false;
    g_armed = false;
    if (const char *path = std::getenv("WOTM_VU1CHECK_DUMP")) {
        char name[1024];
        std::snprintf(name, sizeof name, "%s_%u.csv", path, g_frame);
        g_dump = std::fopen(name, "w");
    }
    if (const char *pre = std::getenv("WOTM_VU1CMP_RAM"); pre && g_rdram) {   // RAM at the frame's hier(0,0) entry (raw, like the ram_*.bin files)
        char name[1024];
        std::snprintf(name, sizeof name, "%s_%u.bin", pre, g_frame);
        if (FILE *f = std::fopen(name, "wb")) { std::fwrite(g_rdram, 1, PS2_RAM_SIZE, f); std::fclose(f); }
    }
    compare();
    if (g_dump) { std::fclose(g_dump); g_dump = nullptr; }
}

// ---- Snapshot of the sampled frame (WOTM_VU1CHECK_SNAP=<prefix>) and replay without the game (ps2EntryRunner --vu1cmp <file>) ----
// What the VU1 sends to the GS is a pure function of some state: the RAM at the moment of each chain (the native layer reads it), VU1/VIF1
// memory and registers at frame entry, and the bytes of each DMA chain (the VIF1 only reads the copy, never the RAM). The file stores that as
// records in the order they happened, several frames in the same file (<prefix>_<g_frame of the first>.vu1snap): 'A' (1st frame:
// VU1/VIF1 state, full RAM and VRAM), 'a' (following frames: VU1/VIF1 state and only the 4 KB pages of RAM and VRAM that
// changed since the previous record), 'C' (chain: changed pages, segment sources and the bytes), 'P' (hier(0,1) entry:
// changed pages), 'S' (hier(0,0) exit: changed pages; absent in old files), 'E' (end of frame). Between frames of the same stage almost everything is the same, so the following ones cost a few MB.
// WOTM_VU1CHECK_SNAP_MAX=<MB> (default 2048) stops writing once past the cap. The replay redoes the sequence inside the runtime, without the game,
// and prints the same [vu1check] output as the live check.
namespace snap
{
constexpr char kMagic[8] = {'W', 'V', 'U', '1', 'S', 'N', 'P', '4'};
constexpr size_t kGsRegsBytes = 15 * sizeof(uint64_t);   // GSRegisters from PMODE to BGCOLOR (the plain fields; CSR/vsyncTick are atomic and left out)
struct Hdr { char magic[8]; uint32_t frame, ramSize, vramSize, vu1State, vifRegs; };
constexpr uint32_t kPage = 4096;
FILE *g_out = nullptr;
bool g_inFrame = false;
std::vector<uint8_t> g_prev, g_prevVram;   // RAM and VRAM as they were at the last record (for the changed pages)
PS2Runtime *g_rt = nullptr;
uint64_t g_bytes = 0, g_max = 2048ull << 20;
uint32_t g_frames = 0;
std::string g_prefix;   // WOTM_VU1CHECK_SNAP; empty = no snapshot
bool g_perShot = false; // photos (WOTM_FOTOS): one file per frame, <photo>.vu1snap, closed at the end of the frame and with no cap
std::string g_file;     // file being written

void put(const void *p, size_t n)
{
    if (!g_out) return;
    if (std::fwrite(p, 1, n, g_out) != n) { std::fprintf(stderr, "[vu1check] error writing the snapshot\n"); std::fclose(g_out); g_out = nullptr; }
    g_bytes += n;
}
template <class T> void putv(const T &v) { put(&v, sizeof v); }

void putDelta(const uint8_t *cur, std::vector<uint8_t> &prev)
{
    std::vector<uint32_t> pages;
    const uint32_t n = uint32_t(prev.size() / kPage);
    for (uint32_t pg = 0; pg < n; ++pg)
        if (std::memcmp(cur + size_t(pg) * kPage, prev.data() + size_t(pg) * kPage, kPage) != 0) pages.push_back(pg);
    putv(uint32_t(pages.size()));
    for (uint32_t pg : pages) {
        putv(pg);
        put(cur + size_t(pg) * kPage, kPage);
        std::memcpy(prev.data() + size_t(pg) * kPage, cur + size_t(pg) * kPage, kPage);
    }
}

void putState()
{
    putv(g_rt->vu1().state());
    putv(g_rt->cpu().vu0_fbrst);
    putv(g_rt->memory().vif1_regs);
    put(g_rt->memory().getVU1Code(), PS2_VU1_CODE_SIZE);
    put(g_rt->memory().getVU1Data(), PS2_VU1_DATA_SIZE);
    put(&g_rt->memory().gs_regs, kGsRegsBytes);   // PMODE/DISPFB/DISPLAY: which buffer the GS presents (the replay's reference image)
}

void begin(const uint8_t *ram, uint32_t frame)   // hier(0, 0) entry of the sampled frame
{
    if ((g_prefix.empty() && !g_perShot) || !g_rt) return;
    if (g_out && g_bytes >= g_max && !g_perShot) {
        std::fprintf(stderr, "[vu1check] snapshot stopped at the cap (WOTM_VU1CHECK_SNAP_MAX, %llu MB) with %u frames\n", (unsigned long long)(g_max >> 20), g_frames);
        std::fclose(g_out);
        g_out = nullptr;
        return;
    }
    if (!g_out) {
        if (g_bytes >= g_max && !g_perShot) return;   // already stopped at the cap
        char name[1024];
        if (g_perShot) std::snprintf(name, sizeof name, "%s", wotmFotoSnapPath().c_str());
        else std::snprintf(name, sizeof name, "%s_%u.vu1snap", g_prefix.c_str(), frame);
        g_out = std::fopen(name, "wb");
        if (!g_out) { std::fprintf(stderr, "[vu1check] could not write %s\n", name); return; }
        g_file = name;
        Hdr h{};
        std::memcpy(h.magic, kMagic, 8);
        h.frame = frame;
        h.ramSize = PS2_RAM_SIZE;
        h.vramSize = uint32_t(PS2_GS_VRAM_SIZE);
        h.vu1State = sizeof(VU1State);
        h.vifRegs = sizeof(VIFRegisters);
        putv(h);
        putv(uint8_t('A'));
        putv(frame);
        putState();
        put(ram, PS2_RAM_SIZE);
        put(g_rt->memory().getGSVRAM(), PS2_GS_VRAM_SIZE);
        g_prev.assign(ram, ram + PS2_RAM_SIZE);
        g_prevVram.assign(g_rt->memory().getGSVRAM(), g_rt->memory().getGSVRAM() + PS2_GS_VRAM_SIZE);
        std::fprintf(stderr, "[vu1check] snapshot: %s\n", name);
    } else {
        putv(uint8_t('a'));
        putv(frame);
        putState();
        putDelta(ram, g_prev);
        putDelta(g_rt->memory().getGSVRAM(), g_prevVram);
    }
    g_inFrame = true;
    ++g_frames;
}

void chain(const uint8_t *ram, const std::pair<uint32_t, uint32_t> *segs, size_t n, const uint8_t *data, uint32_t bytes)
{
    if (!g_out || !g_inFrame) return;
    putv(uint8_t('C'));
    putDelta(ram, g_prev);
    putv(uint32_t(n));
    put(segs, n * sizeof *segs);
    putv(bytes);
    put(data, bytes);
}

void pass1(const uint8_t *ram)
{
    if (!g_out || !g_inFrame) return;
    putv(uint8_t('P'));
    putDelta(ram, g_prev);
}

// hier(0, 0) exit: the RAM the live native drawing collects (wotmCaptureScene). The bone palettes are rewritten in the middle of the
// traversal, so the one from frame entry puts the monsters in a different pose in the replay's native image.
void sceneExit(const uint8_t *ram)
{
    if (!g_out || !g_inFrame) return;
    putv(uint8_t('S'));
    putDelta(ram, g_prev);
}

// GIF channel (PATH3) packet: the drawing environment (FRAME/ZBUF/XYOFFSET/SCISSOR, sceGsPutDrawEnv) and uploads the game sends outside
// the VIF1 chain. Only the replay's reference image needs them; the numeric comparison does not.
void gif(const uint8_t *data, uint32_t bytes)
{
    if (!g_out || !g_inFrame) return;
    putv(uint8_t('G'));
    putv(bytes);
    put(data, bytes);
}

void end()   // end of the sampled frame; the file stays open for the session's next frames
{
    if (!g_out || !g_inFrame) return;
    putv(uint8_t('E'));
    g_inFrame = false;
    if (g_out && g_perShot) { std::fclose(g_out); g_out = nullptr; }   // photo: its own file, the next one starts with 'A'
    else if (g_out) std::fflush(g_out);   // the process is killed at the end of the batch: whatever is already in the file counts
}
} // namespace snap

// Each GIF channel (PATH3) packet with the tap on: goes to the snapshot and to the inventory.
void gifDmaTap(const uint8_t *data, uint32_t bytes)
{
    snap::gif(data, bytes);
    path3Tap(data, bytes);
}

// Trigger (WOTM_VU1CHECK_WHEN=<cond>[,<cond>], any one is enough) and burst (WOTM_VU1CHECK_BURST=<k> consecutive frames per firing):
//  - fx:<visual type>: some live particle system (numParticles > 0) with vis.attribs & 0xFF == type (3 sprite, 7 arc, 13 debris...),
//    only from g_frame 30 on. On the frame the system is born its packet may not be built yet: use a burst;
//  - preset:<fxId>: same, by the system's preset (fxoff::fxId; names in tools/fx.py): tells the volcano smoke apart from a building's;
//  - frame:<a>[-<b>]: g_frame within the range.
// Firing is edge-triggered: the condition went from false to true (the event was born). A permanent emitter (volcano smoke) fires
// once; an explosion, on every new explosion. The period (WOTM_VU1CHECK) is the minimum rest between firings and WOTM_VU1CHECK_SHOTS=<n>
// limits the session's firings (0 = no limit).
//  - mem:<address>[=<value>]: the RAM word (u32) at that address equals <value> (fires on entering it); without <value>, fires on every
//    change of the word. With currScreen (0x6F8464): mem:0x6F8464 records one frame of each menu screen; mem:0x6F8464=1 only the in-game one.
//  - The F8 key in the runtime window records a frame immediately, with no condition or rest.
struct Cond { enum Kind { Fx, Preset, Frame, Mem } kind; uint32_t a, b; bool anyChange = false; uint32_t prev = 0; bool seen = false; };
std::vector<Cond> g_when;
uint32_t g_burst = 1, g_burstLeft = 0, g_shots = 0, g_shotsDone = 0;
bool g_condPrev = false;
bool g_keyOnly = false;   // photos (WOTM_FOTOS): only F8 fires, with no period or conditions
bool g_keyShot = false;   // the sampled frame in progress was requested by F8

// Level conditions (fx, preset, frame, mem:=value): true while they hold; firing is on the rising edge (fireNow). mem: conditions without a
// value are change conditions: `changed` comes out true on the evaluation where the word changed, and that already is the edge (two changes
// in consecutive evaluations fire twice).
bool condTrue(const uint8_t *rdram, uint32_t frame, bool &changed)
{
    const wotm::Ram ram{rdram, PS2_RAM_SIZE};
    bool any = false;   // all of them are evaluated (mem: conditions without a value store the last value seen)
    changed = false;
    for (Cond &c : g_when) {
        if (c.kind == Cond::Frame) { any = any || (frame >= c.a && frame <= c.b); continue; }
        if (c.kind == Cond::Mem) {
            if (!ram.ok(c.a, 4)) continue;
            const uint32_t v = ram.u32(c.a);
            if (c.anyChange) { changed = changed || (c.seen && v != c.prev); c.prev = v; c.seen = true; }
            else any = any || v == c.b;
            continue;
        }
        if (frame < 30) continue;   // like the periodic sampling: at the start of a stage fx[] has live systems with nothing drawn (arc at g_frame 8)
        for (uint32_t i = 1; i < wotm::fxaddr::count && !any; ++i) {
            const uint32_t b = wotm::fxaddr::fx + i * wotm::fxaddr::stride;
            if (int32_t(ram.u32(b + wotm::fxoff::numParticles)) <= 0) continue;
            if (!(ram.u32(b + wotm::fxoff::infoAttribs) & 0x400)) continue;   // only with the packet built (alive but out of view does not draw)
            if (c.kind == Cond::Fx ? (ram.u32(b + wotm::fxoff::type) & 0xFF) == c.a : ram.u32(b + wotm::fxoff::fxId) == c.a) any = true;
        }
    }
    return any;
}

// True only on the frame where some condition starts to hold (rising edge). With no conditions: always.
bool fireNow(const uint8_t *rdram, uint32_t frame)
{
    if (g_when.empty()) return true;
    bool changed = false;
    const bool now = condTrue(rdram, frame, changed);
    const bool fire = changed || (now && !g_condPrev);
    g_condPrev = now;
    return fire;
}

void parseWhen(const char *s)
{
    for (const char *p = s; p && *p;) {
        const char *e = std::strchr(p, ',');
        const std::string tok(p, e ? size_t(e - p) : std::strlen(p));
        p = e ? e + 1 : nullptr;
        if (tok.rfind("fx:", 0) == 0) g_when.push_back({Cond::Fx, uint32_t(std::strtoul(tok.c_str() + 3, nullptr, 0)), 0});
        else if (tok.rfind("preset:", 0) == 0) g_when.push_back({Cond::Preset, uint32_t(std::strtoul(tok.c_str() + 7, nullptr, 0)), 0});
        else if (tok.rfind("frame:", 0) == 0) {
            char *dash = nullptr;
            const uint32_t a = uint32_t(std::strtoul(tok.c_str() + 6, &dash, 0));
            g_when.push_back({Cond::Frame, a, dash && *dash == '-' ? uint32_t(std::strtoul(dash + 1, nullptr, 0)) : a});
        } else if (tok.rfind("mem:", 0) == 0) {
            char *eq = nullptr;
            const uint32_t a = uint32_t(std::strtoul(tok.c_str() + 4, &eq, 0));
            Cond c{Cond::Mem, a, 0};
            if (eq && *eq == '=') c.b = uint32_t(std::strtoul(eq + 1, nullptr, 0));
            else c.anyChange = true;
            g_when.push_back(c);
        } else std::fprintf(stderr, "[vu1check] unknown condition in WOTM_VU1CHECK_WHEN: %s\n", tok.c_str());
    }
}

// Any caller of hier(0, 0) marks a frame (rtMain in a fight; userintMain, ShellRtLoop and FadeScreen in the menus and during loading):
// the sampled frame runs from the entry of one hier(0, 0) to the entry of the next, with or without pass 1 in between (menus may not have one).
uint32_t g_hierCalls = 0;   // calls to hier(0, 0) since the last sampled frame (trigger diagnostic)
void onHier(const uint8_t *rdram, uint32_t view, uint32_t pass, uint32_t ra)
{
    if (view != 0) return;
    uint32_t frame;
    std::memcpy(&frame, rdram + wotm::sym::g_frame, 4);   // g_frame (only advances in a fight; stays still in the menus)
    if (pass == 1) {
        if (g_armed) { snap::pass1(rdram); g_pass = 1; g_cam[1] = readCam(rdram); collectNative(rdram, g_natPass1); }
        return;
    }
    ++g_hierCalls;
    if (g_armed) {   // sampled frame complete (passes and the interrupt's chains)
        uint32_t inPkt = 0, inAlpha = 0;   // packets not yet dispatched to the VU1 (the alpha OT goes out through the DMA interrupt; main.cpp forces it in a fight)
        std::memcpy(&inPkt, rdram + wotm::sym::objsInPacket, 4);
        std::memcpy(&inAlpha, rdram + wotm::sym::objsInAlphaPacket, 4);
        std::fprintf(stderr, "[vu1check] end of sampled frame: hier(0,0) from %x, pass 1 %s, %u chains, objsInPacket %u objsInAlphaPacket %u\n", ra,
                     g_pass == 1 ? "yes" : "no", g_chains, inPkt, inAlpha);
        snap::end();
        finish();
        if (g_keyShot && wotmFotosOn()) {   // photo: stop here, with the photographed frame on screen, for the note
            g_keyShot = false;
            wotmFotoDone(rdram, g_frame, snap::g_file);
            wotmPauseBlock(snap::g_rt);
        }
        g_keyShot = false;
    }
    const auto now = std::chrono::steady_clock::now();
    const bool key = g_wotmShotKey.exchange(false);   // F8: immediately, with no condition, rest or firing limit
    // evaluated every frame, so the edge is not lost during the rest; with no conditions (period), only in a fight (g_frame >= 30)
    const bool fire = key || (!g_keyOnly && (g_when.empty() ? frame >= 30 : fireNow(rdram, frame)));
    if (!g_burstLeft && fire && (key || (now >= g_next && (!g_shots || g_shotsDone < g_shots)))) { g_burstLeft = g_burst; ++g_shotsDone; }
    if (g_burstLeft) {
        --g_burstLeft;
        g_next = now + std::chrono::seconds(g_period);
        uint32_t scr = 0;
        std::memcpy(&scr, rdram + wotm::sym::currScreen, 4);
        std::fprintf(stderr, "[vu1check] sampled frame: g_frame %u, currScreen %u, hier(0,0) from %x (%u calls since the previous one)%s\n", frame, scr, ra,
                     g_hierCalls, key ? ", by the F8 key" : "");
        g_hierCalls = 0;
        g_keyShot = key;
        arm(rdram, frame);
        snap::begin(rdram, frame);
    }
}

PS2Runtime::RecompiledFunction g_origHier = nullptr;
void hierChecked(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == wotm::fn::hier__Fii)
        onHier(rdram, getRegU32(ctx, 4), getRegU32(ctx, 5), getRegU32(ctx, 31));
    g_origHier(rdram, ctx, runtime);
}

// texmActivateTexture(texId, tex0, pkt): a miss in the stage's texture cache. Writes the slot's DBP into the BITBLTBUF of the upload packet
// (texInfo[id][0] + 0x30) and queues it on hier's VU1 chain (pktAddVu1Tex). Outside the sampled frame that chain never goes to the
// DMA (main.cpp's hierFlushObjQ only zeroes the counters), and the upload was lost: the snapshot's VRAM kept the old texture
// in the slot and the reference image came out with a different texture (`d5039fb:docs/passos/VU1CHECK.md`, "Texels"; found with WOTM_GSUPLOADLOG). With the snapshot
// on, the hook sends the same packet (sceGsLoadImage: A+D with BITBLTBUF/TRXPOS/TRXREG/TRXDIR and the image) straight to the GS. The
// mipmaps (texmSetMipMapTbl) are still left out. On the sampled frame the chain really goes to the DMA and the hook does nothing.
PS2Runtime::RecompiledFunction g_origTexActivate = nullptr;
void texActivateChecked(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    const uint32_t id = getRegU32(ctx, 4);
    g_origTexActivate(rdram, ctx, runtime);
    if (g_wotmVu1Tap || id >= 1000) return;
    const uint32_t pkt = (*(const uint32_t *)(rdram + wotm::sym::texInfo + 16 * id) & 0x01FFFFFFu) + 0x20;
    uint32_t p = pkt;
    for (int tag = 0; tag < 4 && p + 16 <= PS2_RAM_SIZE; ++tag) {   // GIF: tags up to EOP (A+D PACKED, then IMAGE)
        uint64_t t;
        std::memcpy(&t, rdram + p, 8);
        const uint32_t nloop = uint32_t(t & 0x7FFF), flg = uint32_t(t >> 58) & 3, nreg = uint32_t(t >> 60) ? uint32_t(t >> 60) : 16u;
        p += 16 + (flg == 0 ? nloop * nreg * 16 : flg == 2 ? nloop * 16 : (nloop * nreg + 1) / 2 * 16);
        if (t & 0x8000) break;
    }
    if (p <= pkt || p > PS2_RAM_SIZE || p - pkt > 0x100000) return;
    runtime->memory().submitGifPacket(GifPathId::Path2, rdram + pkt, p - pkt, true);
}
} // namespace wotm_vu1check

// hier(0, 0) exit, the same point as wotmCaptureScene (wotm_overrides.cpp, hierSceneExit): the snapshot's 'S' record
void wotmVu1SceneExit(const uint8_t *rdram) { wotm_vu1check::snap::sceneExit(rdram); }

void wotmVu1CheckInstall(PS2Runtime &runtime)
{
    // WOTM_FOTOS=<folder> (or 1): only F8 fires, and each firing is a photo (wotm_foto.inc) with its own snapshot. It takes precedence over
    // WOTM_VU1CHECK (period, conditions and WOTM_VU1CHECK_SNAP are left out).
    const char *e = std::getenv("WOTM_VU1CHECK"), *fotos = std::getenv("WOTM_FOTOS");
    if (fotos && !*fotos) fotos = nullptr;
    if (!fotos && (!e || std::atoi(e) <= 0)) return;
    if (!std::getenv("PS2X_NO_VU1")) {   // main.cpp's hierFlushObjQ (only with PS2X_NO_VU1) is what releases the DMA on the sampled frame
        std::fprintf(stderr, "[vu1check] off: native mode only (PS2X_NO_VU1)\n");
        return;
    }
    if (!(wotm_vu1check::g_origHier = runtime.lookupFunction(wotm::fn::hier__Fii))) return;
    if (fotos && !wotmFotoInit(fotos)) fotos = nullptr;
    if (fotos) {
        wotm_vu1check::g_keyOnly = true;
        wotm_vu1check::snap::g_perShot = true;
    } else if (const char *sp = std::getenv("WOTM_VU1CHECK_SNAP")) wotm_vu1check::snap::g_prefix = sp;
    if (!fotos && (!e || std::atoi(e) <= 0)) return;
    wotm_vu1check::snap::g_rt = &runtime;
    g_wotmGifDmaTap = &wotm_vu1check::gifDmaTap;
    if ((fotos || std::getenv("WOTM_VU1CHECK_SNAP")) && !std::getenv("PS2X_GS_UPLOADS")) {
        // The snapshot's VRAM needs the textures and CLUTs: with PS2X_NO_GS the GS only processes uploads (without rasterizing) when
        // PS2X_GS_UPLOADS is on, and the VIF1 lets the PATH2 packets through for that. Read by the GS/VIF1 the first time they run,
        // which is still after this point.
#ifdef _WIN32
        _putenv_s("PS2X_GS_UPLOADS", "1");
#else
        setenv("PS2X_GS_UPLOADS", "1", 1);
#endif
        std::fprintf(stderr, "[vu1check] snapshot on: texture uploads go to the GS (PS2X_GS_UPLOADS=1) for the file's VRAM\n");
    }
    if ((fotos || std::getenv("WOTM_VU1CHECK_SNAP")) &&
        (wotm_vu1check::g_origTexActivate = runtime.lookupFunction(wotm::fn::texmActivateTexture__FiP6QwDataP9Vu1DmaPkt)))
        runtime.registerFunction(wotm::fn::texmActivateTexture__FiP6QwDataP9Vu1DmaPkt, &wotm_vu1check::texActivateChecked);   // cache uploads outside the sampled frame
    wotm_vu1check::g_period = e ? std::max(1, std::atoi(e)) : 1;
    if (const char *w = std::getenv("WOTM_VU1CHECK_WHEN"); w && !fotos) wotm_vu1check::parseWhen(w);
    if (const char *b = std::getenv("WOTM_VU1CHECK_BURST")) wotm_vu1check::g_burst = std::max(1u, uint32_t(std::strtoul(b, nullptr, 10)));
    if (const char *n = std::getenv("WOTM_VU1CHECK_SHOTS")) wotm_vu1check::g_shots = uint32_t(std::strtoul(n, nullptr, 10));
    if (const char *m = std::getenv("WOTM_VU1CHECK_SNAP_MAX")) wotm_vu1check::snap::g_max = uint64_t(std::strtoull(m, nullptr, 10)) << 20;
    if (!wotm_vu1check::g_when.empty() || wotm_vu1check::g_burst > 1 || wotm_vu1check::g_shots)
        std::fprintf(stderr, "[vu1check] trigger: %s (edge-triggered), burst of %u frame(s), up to %u firing(s)\n",
                     std::getenv("WOTM_VU1CHECK_WHEN") ? std::getenv("WOTM_VU1CHECK_WHEN") : "period", wotm_vu1check::g_burst, wotm_vu1check::g_shots);
    wotm_vu1check::g_next = std::chrono::steady_clock::now() + std::chrono::seconds(wotm_vu1check::g_period);
    g_wotmVif1Keep = true;
    g_wotmGifTap = &wotm_vu1check::gifTap;
    g_wotmMscalTap = &wotm_vu1check::mscalTap;
    g_wotmChainTap = &wotm_vu1check::chainTap;
    runtime.registerFunction(wotm::fn::hier__Fii, &wotm_vu1check::hierChecked);
    if (fotos) std::fprintf(stderr, "[vu1check] F8 only (photos)\n");
    else std::fprintf(stderr, "[vu1check] comparing VU1 vs native every %d s\n", wotm_vu1check::g_period);
}

// `ps2EntryRunner --vu1cmp <file.vu1snap>... [--dump <prefix>]`: redoes the check from snapshots (WOTM_VU1CHECK_SNAP) without the game, all in one run.
// Brings up the runtime only for memory, VIF1 and VU1 (the raylib window comes along in initialize), restores the state from the 'A' record and
// replays each chain in the recorded order, with the same native collection and the same comparison as the live check.
// Frame images, next to the snapshot: <base>_<g_frame>_ref.png (the runtime's software GS, fed by the same VU1 and PATH2
// packets: an approximation of the PS2's GS, not the GS itself), _nat.png (the native layer, through the same function as the live game, without the
// FPS counter), _diff.png (pixels that differ by more than 24 in some channel, in red over the darkened reference) and _trio.png
// (the three side by side, 640x448 each). A guide for the eye; the criterion is still the numeric table.
// ---- Per-pixel attribution (replay with images): who drew each differing pixel, on each side ("[vu1check] inv px" lines) ----
// One owner map per pixel of the GS screen (640x224) per side, filled in drawing order (the last one wins; no Z, an approximation):
// reference = all XGKICK triangles and SPRITEs in the screen buffer (whether or not entirely on screen) and the PATH2/3 primitives; native = the
// projected triangles of the 1st collection's objects (pass 0 camera; sky/icons marked), the particle quads, the HUD on top and the text. Each differing
// pixel becomes a pair (owner on the reference, owner on the native side), counted; "none" = background, clear or a leftover framebuffer.
namespace wotm_vu1check
{
struct PxOwner { uint8_t kind; uint32_t a, b; };   // 0 none; 1 object (pkt, tex); 2 HUD (id); 3 fx (system, type); 4 unowned MSCAL (source); 5 path (path, type)
struct PxMap {
    static constexpr int W = 640, H = 224;
    std::vector<uint16_t> id = std::vector<uint16_t>(size_t(W) * H, 0);
    std::vector<PxOwner> owners{{0, 0, 0}};
    std::map<std::tuple<uint8_t, uint32_t, uint32_t>, uint16_t> index;
    uint16_t owner(uint8_t kind, uint32_t a, uint32_t b) {
        auto it = index.find({kind, a, b});
        if (it != index.end()) return it->second;
        if (owners.size() >= 65535) return 0;
        owners.push_back({kind, a, b});
        return index[{kind, a, b}] = uint16_t(owners.size() - 1);
    }
    void tri(uint16_t o, const float *x, const float *y) {   // GS coordinates (2048 at the center)
        float px[3], py[3];
        for (int k = 0; k < 3; ++k) { px[k] = x[k] - kX0; py[k] = y[k] - kY0; }
        const int x0 = std::max(0, int(std::floor(std::min({px[0], px[1], px[2]})))), x1 = std::min(W - 1, int(std::ceil(std::max({px[0], px[1], px[2]}))));
        const int y0 = std::max(0, int(std::floor(std::min({py[0], py[1], py[2]})))), y1 = std::min(H - 1, int(std::ceil(std::max({py[0], py[1], py[2]}))));
        if (x0 > x1 || y0 > y1) return;
        const float area = (px[1] - px[0]) * (py[2] - py[0]) - (px[2] - px[0]) * (py[1] - py[0]);
        if (std::fabs(area) < 1e-6f) return;
        for (int yy = y0; yy <= y1; ++yy)
            for (int xx = x0; xx <= x1; ++xx) {
                const float cx = xx + 0.5f, cy = yy + 0.5f;
                const float w0 = ((px[1] - cx) * (py[2] - cy) - (px[2] - cx) * (py[1] - cy)) / area;
                const float w1 = ((px[2] - cx) * (py[0] - cy) - (px[0] - cx) * (py[2] - cy)) / area;
                const float w2 = 1.f - w0 - w1;
                if (w0 >= 0.f && w1 >= 0.f && w2 >= 0.f) id[size_t(yy) * W + xx] = o;
            }
    }
    void rect(uint16_t o, float ax, float ay, float bx, float by) {
        const int x0 = std::max(0, int(std::floor(std::min(ax, bx) - kX0))), x1 = std::min(W - 1, int(std::ceil(std::max(ax, bx) - kX0)) - 1);
        const int y0 = std::max(0, int(std::floor(std::min(ay, by) - kY0))), y1 = std::min(H - 1, int(std::ceil(std::max(ay, by) - kY0)) - 1);
        for (int yy = y0; yy <= y1; ++yy)
            for (int xx = x0; xx <= x1; ++xx) id[size_t(yy) * W + xx] = o;
    }
};

inline bool screenFbp(uint16_t fbp) { return fbp == 0xFFFF || fbp == 0x46 || fbp == 0x8C; }

// Owner of a VU1 program (like the comparison: native object by polyPkt, else fx[], else the MSCAL source)
PxOwner ownerOfProg(uint32_t prog)
{
    const uint32_t chain = g_progChain[prog - 1];
    if (chain < g_nats.size())
        if (const uint32_t key = objectOf(g_nats[chain], prog)) {
            const NatObj &o = g_nats[chain].at(key);
            if (o.hudId) return {2, o.hudId, 0};
            return {1, key, o.tex};
        }
    if (const int sys = fxOf(prog); sys >= 0) {
        uint32_t type = 0;
        if (!g_fxNats.empty()) { const auto it = g_fxNats[0].find(uint32_t(sys)); if (it != g_fxNats[0].end()) type = it->second.type; }
        return {3, uint32_t(sys), type};
    }
    return {4, g_progs[prog - 1].empty() ? 0u : g_progs[prog - 1].back() & ~0xFFu, 0};
}

void ownerName(const PxOwner &o, char *s, size_t n)
{
    char sym[96];
    switch (o.kind) {
    case 1: std::snprintf(s, n, "pkt %x tex %u", o.a, o.b); break;
    case 2: std::snprintf(s, n, "HUD %x", o.a); break;
    case 3: std::snprintf(s, n, "fx %u type %u", o.a, o.b); break;
    case 4: symOf(o.a, sym, sizeof sym); std::snprintf(s, n, "vu1 mscal %x%s%s%s", o.a, sym[0] ? " (" : "", sym, sym[0] ? ")" : ""); break;
    case 5: std::snprintf(s, n, "path%u %s", o.a, o.b == 6 ? "sprites" : "tris"); break;
    case 6: std::snprintf(s, n, "sky/icon pkt %x tex %u", o.a, o.b); break;
    case 7: std::snprintf(s, n, "text (font)"); break;
    case 8: std::snprintf(s, n, "HUD ScreenPolys"); break;
    default: std::snprintf(s, n, "none"); break;
    }
}

// diff: W x H image (640 x 448) with d[0] == 255 on the differing pixels (vu1cmpImages). Prints the most frequent owner pairs.
void attributePixels(const Image &diff, int W, int H)
{
    if (g_nats.empty()) return;
    PxMap ref, nat;
    // PATH3 first: the GIF channel clear comes before the chains on the PS2 (without the VU1 it gets recorded later; see the replay of the 'G' records)
    for (const RefPrim &r : g_ref23)
        if (r.path == 3 && screenFbp(r.fbp)) ref.rect(ref.owner(5, r.path, r.kind == 6 ? 6 : 3), r.x0, r.y0, r.x1, r.y1);   // box (PATH2/3 triangles are rare)
    for (const PxTri &t : g_vuAll)
        if (screenFbp(t.fbp)) { const PxOwner o = ownerOfProg(t.prog); ref.tri(ref.owner(o.kind, o.a, o.b), t.x, t.y); }
    for (size_t i = 0; i < g_vuRects.size(); ++i) {
        const PxOwner o = ownerOfProg(g_vuRects[i].first);
        const Quad &q = g_vuRects[i].second;
        ref.rect(ref.owner(o.kind, o.a, o.b), q.x[0], q.y[0], q.x[2], q.y[2]);
    }
    for (const RefPrim &r : g_ref23)
        if (r.path == 2 && screenFbp(r.fbp)) ref.rect(ref.owner(5, r.path, r.kind == 6 ? 6 : 3), r.x0, r.y0, r.x1, r.y1);
    for (const auto &kv : g_nats[0]) {
        const NatObj &o = kv.second;
        if (o.hudId || o.tris.empty()) continue;   // HUD: after the particles (below); sky/icons are included (kind 6)
        const uint16_t id = nat.owner((o.skip || o.sky) ? 6 : 1, kv.first, o.tex);
        for (const Tri &t : project(o.tris, g_cam[0], kv.first)) nat.tri(id, t.x, t.y);
    }
    if (!g_fxNats.empty())
        for (const auto &kv : g_fxNats[0]) {
            const uint16_t id = nat.owner(3, kv.first, kv.second.type);
            for (const Quad &q : kv.second.quads[1]) { nat.tri(id, q.x, q.y); const float x2[3] = {q.x[0], q.x[2], q.x[3]}, y2[3] = {q.y[0], q.y[2], q.y[3]}; nat.tri(id, x2, y2); }
        }
    for (const auto &kv : g_nats[0])   // HUD: the native side draws it last in the 3D scene, with no depth
        if (kv.second.hudId) {
            const uint16_t id = nat.owner(2, kv.second.hudId, 0);
            for (const Tri &t : project(kv.second.tris, g_cam[0], kv.first)) nat.tri(id, t.x, t.y);
        }
    if (g_rdram) {   // the native side's ScreenPolys HUD (readScreenPolys, like the replay's takeSnapshot), before the text as in rtMain
        std::vector<wotm::TextQuad> polys;
        wotm::readScreenPolys(wotm::Ram{g_rdram, PS2_RAM_SIZE}, polys);
        const uint16_t id = nat.owner(8, 0, 0);
        for (const wotm::TextQuad &t : polys) nat.rect(id, t.x0 + kX0, t.y0 + kY0, t.x1 + kX0, t.y1 + kY0);
    }
    if (g_scene && g_rdram) {   // native text (font hooks; in the replay, fontPacket): glyphs in pixels of the 640x224 screen
        if (!wotm_live::liveText(g_scene->text))
            g_scene->updateText(wotm::Ram{g_rdram, PS2_RAM_SIZE});
        const uint16_t id = nat.owner(7, 0, 0);
        for (const wotm::TextQuad &t : g_scene->text) nat.rect(id, t.x0 + kX0, t.y0 + kY0, t.x1 + kX0, t.y1 + kY0);
    }
    std::map<std::pair<uint16_t, uint16_t>, uint32_t> pairs;
    uint32_t total = 0, refOnly = 0, natOnly = 0, none = 0, both = 0;
    const uint8_t *d = static_cast<const uint8_t *>(diff.data);
    for (int y = 0; y < H; ++y)
        for (int x = 0; x < W; ++x, d += 4) {
            if (d[0] != 255) continue;
            const int gx = x * PxMap::W / W, gy = y * PxMap::H / H;
            const uint16_t r = ref.id[size_t(gy) * PxMap::W + gx], n = nat.id[size_t(gy) * PxMap::W + gx];
            ++total;
            ++pairs[{r, n}];
            if (r && !n) ++refOnly; else if (!r && n) ++natOnly; else if (!r && !n) ++none; else ++both;
        }
    if (!total) return;
    std::fprintf(stderr, "[vu1check] inv px g=%u: %u differing pixels: only the reference paints %u, only the native side paints %u, both %u, none %u (background/clear/leftover framebuffer)\n",
                 g_frame, total, refOnly, natOnly, both, none);
    std::vector<std::pair<std::pair<uint16_t, uint16_t>, uint32_t>> v(pairs.begin(), pairs.end());
    std::sort(v.begin(), v.end(), [](const auto &a, const auto &b) { return a.second > b.second; });
    char a[160], b[160];
    for (size_t i = 0; i < v.size() && i < 14; ++i) {
        ownerName(ref.owners[v[i].first.first], a, sizeof a);
        ownerName(nat.owners[v[i].first.second], b, sizeof b);
        std::fprintf(stderr, "[vu1check] inv px   ref %s | nat %s: %u px (%.1f%%)\n", a, b, v[i].second, 100.0 * v[i].second / total);
    }
}
} // namespace wotm_vu1check

static uint16_t g_frameFbp = 0xFFFF;   // screen buffer the frame draws into (deduced during warm-up; 0xFFFF = use the triangles' FRAME)
static void vu1cmpImages(const char *snapPath, uint32_t frame, PS2Runtime &runtime, const uint8_t *ram)
{
    constexpr int W = 640, H = 448;
    std::string base(snapPath);
    if (base.size() > 8 && base.compare(base.size() - 8, 8, ".vu1snap") == 0) base.resize(base.size() - 8);
    base += "_" + std::to_string(frame);

    Image ref{};
    {
        std::vector<uint8_t> px;
        uint32_t w = 0, h = 0, dfbp = 0, sfbp = 0;
        bool pref = false;
        // The snapshot's DISPFB shows the previous frame's buffer (double buffering; the game only swaps it at vsync, after the frame). What
        // matters is the buffer this frame drew into: the FRAME most used by the VU1 triangles on screen.
        {
            std::map<uint16_t, uint32_t> byFbp;
            for (const wotm_vu1check::Tri &t : wotm_vu1check::g_vu) if (t.fbp != 0xFFFF) ++byFbp[t.fbp];
            uint16_t best = 0xFFFF;
            uint32_t n = 0;
            for (const auto &kv : byFbp) if (kv.second > n) { n = kv.second; best = kv.first; }
            if (g_frameFbp != 0xFFFF) best = g_frameFbp;
            if (best != 0xFFFF) {
                GSRegisters &gr = runtime.memory().gs_regs;
                gr.dispfb1 = (gr.dispfb1 & ~0x1FFull) | best;
                gr.dispfb2 = (gr.dispfb2 & ~0x1FFull) | best;
            }
        }
        runtime.gs().latchHostPresentationFrame();
        const bool got = runtime.gs().copyLatchedHostPresentationFrame(px, w, h, &dfbp, &sfbp, &pref);
        {
            extern std::atomic<unsigned long long> g_wotmGifPackets, g_wotmGifBytes;
            const GSRegisters &gr = runtime.memory().gs_regs;
            std::fprintf(stderr, "[vu1cmp] gs: packets %llu (%llu bytes), frame %ux%u dispfb %x source %x preferred %d, pmode %llx dispfb1 %llx dispfb2 %llx\n",
                         g_wotmGifPackets.load(), g_wotmGifBytes.load(), w, h, dfbp, sfbp, int(pref), (unsigned long long)gr.pmode, (unsigned long long)gr.dispfb1, (unsigned long long)gr.dispfb2);
        }
        if (got && w && h && px.size() >= size_t(w) * h * 4) {
            ref = GenImageColor(int(w), int(h), BLACK);
            std::memcpy(ref.data, px.data(), size_t(w) * h * 4);
            ImageResizeNN(&ref, W, H);
        } else {
            ref = GenImageColor(W, H, BLACK);
            std::fprintf(stderr, "[vu1cmp] no GS frame for the reference image (does the snapshot's DISPFB not point to a drawn buffer?)\n");
        }
    }

    wotm_live::g_hideFps = true;
    wotm_live::g_localCollect = true;   // no game running: the scene comes from the snapshot's RAM, not from the EE thread's capture
    // Lens flare: at frame entry the list is already in world coordinates (readFlares discards it); use the one the VU1 received
    // (g_flareVuIn) like the live game's capture, with each sprite's texId and the GS depth read from the frame's RAM
    if (!wotm_vu1check::g_flareVuIn.empty()) {
        wotm::Particles::FlareFrame f = wotm_vu1check::g_flareVuIn.back();
        const wotm::Ram r{const_cast<uint8_t *>(ram), PS2_RAM_SIZE};
        const uint32_t p = r.u32(wotm::sym::lensFlarePacket);
        if (r.ok(p, 0x2EE0))
            for (int k = 0; k < f.nspr; ++k) f.spr[k].texId = r.u16(p + 0x2C40 + 2 * uint32_t(k));
        f.zScale = r.f32(wotm::sym::viewScreenMats + 0x28);
        f.zOff = r.f32(wotm::sym::viewScreenMats + 0x2C);
        auto &slot = wotm::Particles::flareSlot();
        std::lock_guard<std::mutex> g(slot.m);
        slot.f = std::move(f);
        slot.used = true;
        slot.fresh = true;   // without it collectFlares treats the frame as having no particleDraw and the native image came out without flares
    }
    {   // menu big-screen movie (texId 999): with no player in the replay, the frame the player left in VRAM (CT32 at 0x3BA0, width
        // 320, 256x128; wotm_fmv.hpp), the way the reference GS samples it
        static Texture2D movie{};
        std::vector<uint32_t> px;
        uint32_t mw = 0, mh = 0;
        const uint64_t tex0 = 0x3BA0ull | (5ull << 14) | (8ull << 26) | (7ull << 30);
        if (wotm::gs::decodeTexture({runtime.memory().getGSVRAM(), PS2_GS_VRAM_SIZE}, tex0, px, mw, mh)) {
            if (!movie.id) {
                Image im{px.data(), int(mw), int(mh), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                movie = LoadTextureFromImage(im);
            } else UpdateTexture(movie, px.data());
            wotm_live::g_replayMovieTex = []() { return movie.id; };
        }
    }
    if (const char *tl = std::getenv("WOTM_VU1CMP_VRAMTEX")) {   // diagnostic: <tex0 hex>[,..] writes <snapshot>_vram_<tex0>.png from VRAM
        for (const char *s = tl; *s;) {
            char *end = nullptr;
            const uint64_t t0 = std::strtoull(s, &end, 16);
            if (end == s) break;
            std::vector<uint32_t> px;
            uint32_t mw = 0, mh = 0;
            if (wotm::gs::decodeTexture({runtime.memory().getGSVRAM(), PS2_GS_VRAM_SIZE}, t0, px, mw, mh)) {
                Image im{px.data(), int(mw), int(mh), 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
                char name[1024];
                std::snprintf(name, sizeof name, "%s_vram_%llx.png", snapPath, (unsigned long long)t0);
                ExportImage(im, name);
            } else std::fprintf(stderr, "[vu1cmp] VRAMTEX %llx: decodeTexture failed\n", (unsigned long long)t0);
            s = *end == ',' ? end + 1 : end;
        }
    }
    wotm_live::g_force43 = true;   // the GS reference is 4:3 (and the frame is 960x720, renderHeight); the crop below assumes that
    bool natOk = false;
    for (int pass = 0; pass < 2; ++pass) {   // the 1st draw of a stage only loads (resources, snapshot A/B); the 2nd draws
        BeginDrawing();
        ClearBackground(BLACK);
        natOk = wotmDrawNative(ram);
        EndDrawing();
    }
    Image nat = LoadImageFromScreen();
    ImageFormat(&nat, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    {   // crops the 4:3 letterbox of wotmDrawNative (960x720 render texture fitted to the window)
        const float sw = float(GetScreenWidth()), sh = float(GetScreenHeight());
        const float k = std::min(sw / 960.f, sh / 720.f);
        ImageCrop(&nat, Rectangle{(sw - 960.f * k) * 0.5f, (sh - 720.f * k) * 0.5f, 960.f * k, 720.f * k});
        ImageResize(&nat, W, H);
    }
    if (!natOk) std::fprintf(stderr, "[vu1cmp] the native layer did not draw this frame (scene with no stage?)\n");
    // WOTM_VU1CMP_WIDE=1 (diagnostic): the native frame again at the live game's aspect ratio (WOTM_ASPECT, default the window's) and
    // at WOTM_RES height, written as <base>_wide.png; for checking what only shows up in widescreen
    if (const char *w = std::getenv("WOTM_VU1CMP_WIDE"); w && std::atoi(w) > 0) {
        wotm_live::g_force43 = false;
        for (int pass = 0; pass < 2; ++pass) {
            BeginDrawing();
            ClearBackground(BLACK);
            wotmDrawNative(ram);
            EndDrawing();
        }
        const std::string wide = base + "_wide.png";
        if (wotm_live::exportFrame(wide.c_str())) std::fprintf(stderr, "[vu1cmp] widescreen: %s\n", wide.c_str());
        wotm_live::g_force43 = true;
    }

    Image diff = GenImageColor(W, H, BLACK);
    uint32_t differ = 0;
    {
        const uint8_t *r = static_cast<const uint8_t *>(ref.data), *n = static_cast<const uint8_t *>(nat.data);
        uint8_t *d = static_cast<uint8_t *>(diff.data);
        for (int i = 0; i < W * H; ++i, r += 4, n += 4, d += 4) {
            int m = 0;
            for (int c = 0; c < 3; ++c) m = std::max(m, std::abs(int(r[c]) - int(n[c])));
            if (m > 24) { ++differ; d[0] = 255; d[1] = uint8_t(255 - m); d[2] = 0; }
            else { const uint8_t g = uint8_t((r[0] + r[1] + r[2]) / 12); d[0] = d[1] = d[2] = g; }
            d[3] = 255;
        }
    }
    wotm_vu1check::attributePixels(diff, W, H);   // "inv px" lines: owner of each differing pixel, on both sides
    Image trio = GenImageColor(3 * W + 8, H, DARKGRAY);
    const Rectangle src{0.f, 0.f, float(W), float(H)};
    ImageDraw(&trio, ref, src, Rectangle{0.f, 0.f, float(W), float(H)}, WHITE);
    ImageDraw(&trio, nat, src, Rectangle{float(W + 4), 0.f, float(W), float(H)}, WHITE);
    ImageDraw(&trio, diff, src, Rectangle{float(2 * W + 8), 0.f, float(W), float(H)}, WHITE);
    ExportImage(ref, (base + "_ref.png").c_str());
    ExportImage(nat, (base + "_nat.png").c_str());
    ExportImage(diff, (base + "_diff.png").c_str());
    ExportImage(trio, (base + "_trio.png").c_str());
    UnloadImage(ref);
    UnloadImage(nat);
    UnloadImage(diff);
    UnloadImage(trio);
    std::fprintf(stderr, "[vu1cmp] images: %s_{ref,nat,diff,trio}.png; differing pixels %u (%.1f%%)\n", base.c_str(), differ, 100.0 * differ / (W * H));
}

static int vu1cmpBody(PS2Runtime &runtime, const char *path, const char *dump, uint32_t onlyFrame, bool png)
{
    using namespace wotm_vu1check;
    // Marks the start of each file in the output (several files in one run; tools\vu1cmp.ps1 and corpus.ps1 split on it)
    std::fprintf(stderr, "[vu1cmp] file %s\n", path);
    g_wotmGsFeed = png;   // with images, the runtime's software GS receives the frame's packets (PATH2 and XGKICK)
    if (dump) {
#ifdef _WIN32
        _putenv_s("WOTM_VU1CHECK_DUMP", dump);
#else
        setenv("WOTM_VU1CHECK_DUMP", dump, 1);
#endif
    }
    FILE *f = std::fopen(path, "rb");
    if (!f) { std::fprintf(stderr, "[vu1cmp] could not open %s\n", path); return 1; }
    auto get = [&](void *p, size_t n) { return std::fread(p, 1, n, f) == n; };
    snap::Hdr h{};
    if (!get(&h, sizeof h) || std::memcmp(h.magic, snap::kMagic, 8) != 0) { std::fprintf(stderr, "[vu1cmp] %s is not a snapshot\n", path); return 1; }
    // RAM smaller than the build's (snapshots from before the 64 MB RAM, 2026-10-19): it goes at the start and the rest stays zeroed
    if (h.ramSize == 0 || h.ramSize > PS2_RAM_SIZE || h.vramSize != PS2_GS_VRAM_SIZE || h.vu1State != sizeof(VU1State) || h.vifRegs != sizeof(VIFRegisters)) {
        std::fprintf(stderr, "[vu1cmp] snapshot from a different runtime build (state sizes differ)\n");
        return 1;
    }
    // The runtime (OpenGL window and audio) is created once for all files: the software GS goes back to a fresh runtime's state
    // for each file (the warm-up assumes a zeroed GS); RAM, VRAM, VU1 and VIF1 come from the snapshot itself
    runtime.gs().reset();
    runtime.vu1().reset();
    wotm_live::g_replayFresh = true;   // the native layer also starts from scratch (scene, resources, previous frame)
    {   // and the lens flares: the slot is only filled on a frame with a flare, and a file without flares inherited the previous one's
        auto &slot = wotm::Particles::flareSlot();
        std::lock_guard<std::mutex> g(slot.m);
        slot.f = wotm::Particles::FlareFrame{};
        slot.used = false;
        slot.fresh = false;
    }
    {
        extern std::atomic<unsigned long long> g_wotmGifPackets, g_wotmGifBytes;
        g_wotmGifPackets = 0;
        g_wotmGifBytes = 0;
    }
    PS2Memory &mem = runtime.memory();
    uint8_t *ram = mem.getRDRAM();
    g_vram = png ? mem.getGSVRAM() : nullptr;   // texels: only with the GS receiving the frame's uploads
    g_wotmVif1Keep = true;
    g_wotmGifTap = &gifTap;
    g_wotmMscalTap = &mscalTap;
    g_wotmChainTap = &chainTap;
    auto applyDelta = [&](uint8_t *base, size_t size) {   // pages that changed (RAM or VRAM)
        uint32_t n = 0;
        if (!get(&n, 4)) return false;
        for (uint32_t i = 0; i < n; ++i) {
            uint32_t pg = 0;
            if (!get(&pg, 4) || size_t(pg) * snap::kPage >= size || !get(base + size_t(pg) * snap::kPage, snap::kPage)) return false;
        }
        return true;
    };
    auto getState = [&]() {
        runtime.vu1().reset();
        const bool ok = get(&runtime.vu1().state(), sizeof(VU1State)) && get(&runtime.cpu().vu0_fbrst, 4) && get(&mem.vif1_regs, sizeof(VIFRegisters)) &&
                        get(mem.getVU1Code(), PS2_VU1_CODE_SIZE) && get(mem.getVU1Data(), PS2_VU1_DATA_SIZE) && get(&mem.gs_regs, snap::kGsRegsBytes);
        mem.markVU1CodeModified();
        return ok;
    };
    uint32_t chains = 0, frame = 0, framesDone = 0, framesSeen = 0;
    bool ok = true, inFrame = false, skip = false;   // skip: a frame that is not the requested one (--frame): only applies the deltas
    // The frame's GIF channel packets ('G'): this frame's drawing environment was sent before the hier(0,0) entry, that is,
    // in the previous frame's window; what this frame's window carries is the next one's, with the same XYOFFSET/SCISSOR/ZBUF. For the
    // reference image, the frame's 'G' records are applied to the GS before the chains (read ahead from the file) and again in recorded order.
    std::vector<std::vector<uint8_t>> gifs;
    std::vector<uint8_t> vramRec(PS2_GS_VRAM_SIZE);   // VRAM as recorded in the file (the 'a' deltas apply to it, not to the replay's)
    if (png && std::getenv("WOTM_VU1CHECK_TEXREC")) g_vram = vramRec.data();   // diagnostic: texels from the recorded VRAM (frame start), not the GS's at draw time
    // RAM for the native image: the one at hier(0, 0) exit ('S' record), the same one the live drawing collects; in old files, without
    // 'S', it stays the frame-entry one (bone palettes from before the traversal: a moving monster comes out in a different pose)
    std::vector<uint8_t> frameRam;
    bool sceneRam = false;   // frameRam is already this frame's 'S' one
#ifdef _WIN32
    auto tell = [&]() { return _ftelli64(f); };
    auto seek = [&](int64_t off, int whence) { return _fseeki64(f, off, whence) == 0; };
#else
    auto tell = [&]() { return int64_t(ftello(f)); };
    auto seek = [&](int64_t off, int whence) { return fseeko(f, off, whence) == 0; };
#endif
    // GS warm-up for the reference image: the software GS starts zeroed, and part of its state (registers the game
    // only sends at the end of the frame, for the next frame) comes from the previous frame; without this the file's first frame comes out without the world.
    // Runs the frame's chains and GIF channel packets once through the GS only (without the check's taps), restores the VRAM and VU1/VIF1
    // to the entry state and leaves the rest (GS registers) as it ended up.
    std::vector<uint8_t> vramIn(PS2_GS_VRAM_SIZE), vu1CodeIn(PS2_VU1_CODE_SIZE), vu1DataIn(PS2_VU1_DATA_SIZE);
    auto warmUp = [&]() {
        gifs.clear();
        const int64_t pos = tell();
        std::memcpy(vramIn.data(), mem.getGSVRAM(), PS2_GS_VRAM_SIZE);
        std::memcpy(vu1CodeIn.data(), mem.getVU1Code(), PS2_VU1_CODE_SIZE);
        std::memcpy(vu1DataIn.data(), mem.getVU1Data(), PS2_VU1_DATA_SIZE);
        const VU1State vuIn = runtime.vu1().state();
        const VIFRegisters vifIn = mem.vif1_regs;
        g_wotmVu1Tap = false;   // no taps: XGKICK and PATH2 go straight to the GS (g_wotmGsFeed)
        std::vector<uint8_t> data;
        for (;;) {
            uint8_t type = 0;
            if (!get(&type, 1) || type == 'E' || type == 'A' || type == 'a') break;
            uint32_t n = 0, bytes = 0;
            bool more = true;
            auto skipDelta = [&]() { if (!get(&n, 4)) return false; return seek(int64_t(n) * (4 + int64_t(snap::kPage)), SEEK_CUR); };
            if (type == 'C') {
                more = skipDelta() && get(&n, 4) && seek(int64_t(n) * 8, SEEK_CUR) && get(&bytes, 4);
                if (more) { data.resize(bytes); more = get(data.data(), bytes); }
                if (more) mem.processVIF1Data(data.data(), bytes);
            }
            else if (type == 'P' || type == 'S') more = skipDelta();
            else if (type == 'G') {
                more = get(&bytes, 4);
                if (more) { gifs.emplace_back(bytes); more = get(gifs.back().data(), bytes); }
                if (more) mem.submitGifPacket(GifPathId::Path3, gifs.back().data(), bytes, true);
            }
            else more = false;
            if (!more) break;
        }
        seek(pos, SEEK_SET);
        std::memcpy(mem.getGSVRAM(), vramIn.data(), PS2_GS_VRAM_SIZE);
        std::memcpy(mem.getVU1Code(), vu1CodeIn.data(), PS2_VU1_CODE_SIZE);
        std::memcpy(mem.getVU1Data(), vu1DataIn.data(), PS2_VU1_DATA_SIZE);
        mem.markVU1CodeModified();
        runtime.vu1().reset();
        runtime.vu1().state() = vuIn;
        mem.vif1_regs = vifIn;
        // The VRAM is back to the entry state, with the previous frame's Z still there (the clear for this frame went out in the previous
        // frame's window, which the file does not have for the 1st frame). This frame's GIF channel packets clear the Z (shared) for the
        // next one: reapplied now, they leave the Z clear for the real pass; the FRAME they set is swapped by the chain itself.
        for (const auto &g : gifs) mem.submitGifPacket(GifPathId::Path3, g.data(), uint32_t(g.size()), true);
        // The FRAME in effect now is the one from the window's last environment, the next frame's. This frame's is the first FRAME that the
        // window's GIF channel packets write (the game re-sends it after the render-to-texture); if there is only the next frame's, it is the other
        // screen buffer. Forces FRAME_1/FRAME_2 to it, and that buffer is what the reference image shows.
        uint64_t first = 0, last = 0;
        for (const auto &g : gifs)
            for (uint32_t pos = 0; pos + 16 <= g.size();) {
                uint64_t lo, hi;
                std::memcpy(&lo, g.data() + pos, 8);
                std::memcpy(&hi, g.data() + pos + 8, 8);
                pos += 16;
                const uint32_t nloop = uint32_t(lo & 0x7FFF), flg = uint32_t((lo >> 58) & 3), nreg = uint32_t((lo >> 60) & 0xF) ? uint32_t((lo >> 60) & 0xF) : 16u;
                if (flg == 0) {
                    for (uint32_t l = 0; l < nloop; ++l)
                        for (uint32_t r = 0; r < nreg && pos + 16 <= g.size(); ++r, pos += 16)
                            if (uint32_t((hi >> (4 * r)) & 0xF) == 0xE) {
                                uint64_t v0, v1;
                                std::memcpy(&v0, g.data() + pos, 8);
                                std::memcpy(&v1, g.data() + pos + 8, 8);
                                if ((v1 & 0xFF) == 0x4C) { if (!first) first = v0; last = v0; }
                            }
                } else if (flg == 1) pos += ((nloop * nreg + 1) / 2) * 16;
                else pos += nloop * 16;
            }
        if (last) {
            uint64_t fr = first != last ? first : ((last & ~0x1FFull) | ((last & 0x1FF) == 0x46 ? 0x8Cu : 0x46u));
            runtime.gs().writeRegister(0x4C, fr);
            runtime.gs().writeRegister(0x4D, fr);
            g_frameFbp = uint16_t(fr & 0x1FF);
        } else g_frameFbp = 0xFFFF;
        g_wotmVu1Tap = true;
    };
    for (;;) {
        uint8_t type = 0;
        if (!get(&type, 1)) { ok = !inFrame; break; }   // end of file between frames = normal; in the middle of one = truncated
        if (type == 'A' || type == 'a') {
            if (inFrame) { ok = false; break; }
            ok = get(&frame, 4) && getState();
            // The recorded VRAM is kept separately (vramRec): the 'a' delta only carries the pages the game changed, and during capture (no GS) the
            // framebuffer and Z barely change; applied straight to the GS's VRAM, it left in them what the replay drew in the previous frame,
            // and the stale Z hid the world (black reference image from the file's 2nd frame on; with --frame it came out right).
            if (ok && type == 'A' && h.ramSize < PS2_RAM_SIZE) std::memset(ram + h.ramSize, 0, PS2_RAM_SIZE - h.ramSize);
            if (ok) ok = type == 'A' ? (get(ram, h.ramSize) && get(vramRec.data(), PS2_GS_VRAM_SIZE))
                                     : (applyDelta(ram, PS2_RAM_SIZE) && applyDelta(vramRec.data(), PS2_GS_VRAM_SIZE));
            if (ok) std::memcpy(mem.getGSVRAM(), vramRec.data(), PS2_GS_VRAM_SIZE);
            if (!ok) break;
            inFrame = true;
            ++framesSeen;
            skip = onlyFrame && frame != onlyFrame;
            chains = 0;
            if (!skip) {
                if (png) { warmUp(); frameRam.assign(ram, ram + PS2_RAM_SIZE); sceneRam = false; }   // replaced by the 'S' one, if any
                arm(ram, frame);
            }
            continue;
        }
        if (!inFrame) { ok = false; break; }
        if (type == 'C') {
            uint32_t n = 0, bytes = 0;
            std::vector<std::pair<uint32_t, uint32_t>> segs;
            std::vector<uint8_t> data;
            ok = applyDelta(ram, PS2_RAM_SIZE) && get(&n, 4);
            if (ok) { segs.resize(n); ok = get(segs.data(), n * sizeof segs[0]) && get(&bytes, 4); }
            if (ok) { data.resize(bytes); ok = get(data.data(), bytes); }
            if (!ok) break;
            if (skip) continue;
            ++chains;
            mem.m_vif1ChainSrc = segs.empty() ? nullptr : &segs;
            if (mem.m_vif1ChainSrc) chainTap(segs.data(), segs.size(), data.data(), bytes);
            mem.processVIF1Data(data.data(), bytes);
            mem.m_vif1ChainSrc = nullptr;
        } else if (type == 'P') {
            ok = applyDelta(ram, PS2_RAM_SIZE);
            if (ok && !skip) { g_pass = 1; g_cam[1] = readCam(ram); collectNative(ram, g_natPass1); }
            if (!ok) break;
            if (ok && !skip && !g_flareVuIn.empty()) {
                // The hier(0, 1) entry comes right after particleDraw: the list as the live game copies it (readFlares) against the one
                // the VU1 received. A difference here = the live drawing uses different data than the replay (that is how the uvMax read one qword ahead slipped through).
                wotm::Particles::FlareFrame ff;
                wotm::Particles::readFlares(wotm::Ram{ram, PS2_RAM_SIZE}, ff);
                const auto &vf = g_flareVuIn.back();
                uint32_t spr = 0, ent = 0;
                for (int k = 0; k < std::min(ff.nspr, vf.nspr); ++k) {
                    const auto &a = ff.spr[k], &b = vf.spr[k];
                    spr += a.tex0 != b.tex0 || a.t != b.t || a.half != b.half || a.uvMax != b.uvMax || std::memcmp(a.rgba, b.rgba, sizeof a.rgba) != 0;
                }
                for (size_t i = 0; i < std::min(ff.e.size(), vf.e.size()); ++i)
                    ent += ff.e[i].x != vf.e[i].x || ff.e[i].y != vf.e[i].y || ff.e[i].intensity != vf.e[i].intensity;
                if (spr || ent || ff.nspr != vf.nspr || ff.e.size() != vf.e.size())
                    std::fprintf(stderr, "[vu1check] g=%u live lens flare differs from the VU1: sprites %d/%d (%u different), inputs %zu/%zu (%u different)\n", frame,
                                 ff.nspr, vf.nspr, spr, ff.e.size(), vf.e.size(), ent);
            }
        } else if (type == 'S') {
            ok = applyDelta(ram, PS2_RAM_SIZE);
            if (!ok) break;
            if (!skip && png && !sceneRam) { std::memcpy(frameRam.data(), ram, PS2_RAM_SIZE); sceneRam = true; }
        } else if (type == 'G') {
            uint32_t bytes = 0;
            std::vector<uint8_t> data;
            ok = get(&bytes, 4);
            if (ok) { data.resize(bytes); ok = get(data.data(), bytes); }
            if (!ok) break;
            static const bool s_listG = std::getenv("WOTM_VU1CMP_LISTG") != nullptr;   // diagnostic: lists the registers of each GIF channel packet
            if (s_listG && !skip) {   // the packet's GIF tags: NLOOP, format and the A+D registers (with the value)
                std::string s;
                char b[96];
                for (uint32_t pos = 0; pos + 16 <= bytes;) {
                    uint64_t lo, hi;
                    std::memcpy(&lo, data.data() + pos, 8);
                    std::memcpy(&hi, data.data() + pos + 8, 8);
                    pos += 16;
                    const uint32_t nloop = uint32_t(lo & 0x7FFF), flg = uint32_t((lo >> 58) & 3), nreg = uint32_t((lo >> 60) & 0xF) ? uint32_t((lo >> 60) & 0xF) : 16u;
                    std::snprintf(b, sizeof b, " [nloop %u flg %u nreg %u%s]", nloop, flg, nreg, (lo >> 15) & 1 ? " eop" : "");
                    s += b;
                    if (flg == 0) {
                        for (uint32_t l = 0; l < nloop; ++l)
                            for (uint32_t r = 0; r < nreg && pos + 16 <= bytes; ++r, pos += 16) {
                                const uint32_t reg = uint32_t((hi >> (4 * r)) & 0xF);
                                uint64_t v0, v1;
                                std::memcpy(&v0, data.data() + pos, 8);
                                std::memcpy(&v1, data.data() + pos + 8, 8);
                                if (reg == 0xE) std::snprintf(b, sizeof b, " %02llx=%llx", (unsigned long long)(v1 & 0xFF), (unsigned long long)v0);
                                else std::snprintf(b, sizeof b, " r%x", reg);
                                s += b;
                            }
                    } else if (flg == 1) pos += ((nloop * nreg + 1) / 2) * 16;
                    else pos += nloop * 16;
                }
                std::fprintf(stderr, "[vu1cmp] G %u bytes (after %u chains):%s\n", bytes, chains, s.c_str());
            }
            if (!skip) path3Tap(data.data(), bytes);   // inventory (no RAM source: the file only stores the bytes)
            // Reapplied in recorded order (besides the warm-up) are only the packets with no vertices (drawing environment: XYOFFSET, SCISSOR,
            // FRAME...), which the fight needs. The ones that draw (the next frame's clear, two full-screen SPRITEs) are left out:
            // without the VU1 the VIF1 chain is only dispatched late, so they get recorded after the chains and, on the GS, erased the buffer
            // the chains had just drawn (in the menus the image came out black; on the PS2 the clear comes first). WOTM_VU1CMP_GORDER=1 reapplies
            // all of them (diagnostic).
            static const bool s_gOrder = std::getenv("WOTM_VU1CMP_GORDER") && std::atoi(std::getenv("WOTM_VU1CMP_GORDER")) > 0;
            auto hasKick = [&]() {   // some XYZ2/XYZF2 (PACKED or REGLIST) in the packet
                for (uint32_t pos = 0; pos + 16 <= bytes;) {
                    uint64_t lo, hi;
                    std::memcpy(&lo, data.data() + pos, 8);
                    std::memcpy(&hi, data.data() + pos + 8, 8);
                    pos += 16;
                    const uint32_t nloop = uint32_t(lo & 0x7FFF), flg = uint32_t((lo >> 58) & 3), nreg = uint32_t((lo >> 60) & 0xF) ? uint32_t((lo >> 60) & 0xF) : 16u;
                    if (flg == 0 || flg == 1) {
                        for (uint32_t r = 0; r < nreg; ++r) {
                            const uint32_t reg = uint32_t((hi >> (4 * r)) & 0xF);
                            if (nloop && (reg == 0x4 || reg == 0x5)) return true;
                        }
                        pos += flg == 0 ? nloop * nreg * 16 : ((nloop * nreg + 1) / 2) * 16;
                    } else pos += nloop * 16;
                    if ((lo >> 15) & 1) break;
                }
                return false;
            };
            if (!skip && png && (s_gOrder || !hasKick())) mem.submitGifPacket(GifPathId::Path3, data.data(), bytes, true);
        } else if (type == 'E') {
            inFrame = false;
            if (skip) continue;
            std::fprintf(stderr, "[vu1cmp] frame g_frame %u (%u chains, %zu GIF channel packets)\n", frame, chains, gifs.size());
            finish();
            ++framesDone;
            if (png) vu1cmpImages(path, frame, runtime, frameRam.data());
        } else { ok = false; break; }
    }
    std::fclose(f);
    if (!ok) std::fprintf(stderr, "[vu1cmp] snapshot truncated or corrupt at frame %u (%u chains read); %u complete frame(s) before it\n", frame, chains, framesDone);
    if (onlyFrame && !framesDone) std::fprintf(stderr, "[vu1cmp] frame %u is not in the file (%u frames)\n", onlyFrame, framesSeen);
    std::fprintf(stderr, "[vu1cmp] %s: %u frame(s) compared\n", path, framesDone);
    return framesDone ? 0 : 1;
}

// The live check runs on the EE thread; the main thread has a 1 MB stack and overflows it (collection, VU1, comparison). The body runs
// on a thread with a large stack. No windows.h (it clashes with raylib's names): only the two kernel32 symbols that are missing.
#ifdef _WIN32
#include <process.h>
extern "C" __declspec(dllimport) unsigned long __stdcall WaitForSingleObject(void *handle, unsigned long ms);
extern "C" __declspec(dllimport) int __stdcall CloseHandle(void *handle);
#else
#define __stdcall
#endif
struct Vu1CmpJob { std::vector<const char *> paths; const char *dump; uint32_t frame; bool png; int rc; };
// All files in a single run (bringing up the runtime, window and audio cost more than the comparison). rc 1 if any failed.
static unsigned __stdcall vu1cmpThread(void *p)
{
    Vu1CmpJob *j = static_cast<Vu1CmpJob *>(p);
    std::unique_ptr<PS2Runtime> rtp(new PS2Runtime());
    if (!rtp->initialize("WotM vu1cmp")) { j->rc = 1; return 0; }
    j->rc = 0;
    for (const char *path : j->paths)
        if (vu1cmpBody(*rtp, path, j->dump, j->frame, j->png) != 0) j->rc = 1;
    return 0;
}

int wotmVu1CmpMain(int argc, char **argv)
{
    Vu1CmpJob job{{}, nullptr, 0, true, 1};
    for (int i = 2; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--dump") && i + 1 < argc) job.dump = argv[++i];
        else if (!std::strcmp(argv[i], "--frame") && i + 1 < argc) job.frame = uint32_t(std::strtoul(argv[++i], nullptr, 10));
        else if (!std::strcmp(argv[i], "--nopng")) job.png = false;
        else job.paths.push_back(argv[i]);
    }
    if (job.paths.empty()) { std::fprintf(stderr, "usage: ps2EntryRunner --vu1cmp <file.vu1snap>... [--frame <g_frame>] [--dump <prefix>] [--nopng]\n"); return 2; }
#ifdef _WIN32
    const uintptr_t h = _beginthreadex(nullptr, 512u << 20, &vu1cmpThread, &job, 0, nullptr);
    if (!h) { vu1cmpThread(&job); return job.rc; }
    WaitForSingleObject(reinterpret_cast<void *>(h), 0xFFFFFFFFul);
    CloseHandle(reinterpret_cast<void *>(h));
#else
    vu1cmpThread(&job);
#endif
    return job.rc;
}
#elif !PS2X_HW_EMU
// Game executable (wotm): no check and no photos; warns if they are requested
void wotmVu1CheckInstall(PS2Runtime &)
{
    if (std::getenv("WOTM_VU1CHECK") || std::getenv("WOTM_FOTOS"))
        std::fprintf(stderr, "[vu1check] WOTM_VU1CHECK/WOTM_FOTOS ignored: the check and the photos only exist in ps2EntryRunner (the test executable)\n");
}
void wotmVu1SceneExit(const uint8_t *) {}
int wotmVu1CmpMain(int, char **) { std::fprintf(stderr, "--vu1cmp only in ps2EntryRunner (the test executable)\n"); return 1; }
#else
void wotmVu1CheckInstall(PS2Runtime &) {}
void wotmVu1SceneExit(const uint8_t *) {}
int wotmVu1CmpMain(int, char **) { std::fprintf(stderr, "runtime without the native layer (WOTM_HAVE_NATIVE): no --vu1cmp\n"); return 1; }
#endif
