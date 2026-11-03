// Particles and effects (dust, fire, sparks, explosions, trails) read from EE RAM and drawn with raylib.
//
// In retail, `particlePrepDraw` (particleMove + each type's *PacketBuild) builds one VIF packet per system and `particleDraw`
// (0x21B378; the loop that waits on VU1 is at 0x21B8A0) sends those packets to the VU1 particle microcode, which expands
// each particle into a sprite/line/strip. Here the cut is before VU1: we read each system's state (`_ParticleType`,
// include/hieri_types.h) and draw it directly. Details and what is still a hypothesis: docs/FORMATOS.md ("Particles and effects").
//
// Confirmed in the CPU code (asm of common/particle and common/partdraw):
//   - fx[100] at 0x5BA280 (0x2EE0 bytes each); active list: fxActiveTop (0x6F88EC) -> fxList[i].next (+8, 12 bytes per entry), -1 = end
//   - per particle: pos[i] (FVECTOR; w = size, radius: particleFOVcalcMaxRadius uses maxSize + expansionRate*lifetime),
//     vel[i] (FVECTOR), age[i] (int16, in fields); numParticles at +0x2ED0
//   - only drawn with info.attribs & 0x400 (packet built), numParticles >= 1 (sprine: >= 2) and type != 11
//   - type = vis.attribs (+0x2E90): 1 line, 2 sweepTrail, 3 sprite, 4 texSprite, 5 stripe, 6 strite, 8 sprine, 9 kite,
//     13 frag, 15 dustTrail, 16 wake (particlePrepDraw's jump table)
//   - color (loadColors): RGBA table at 0x256C10 (0x80 = 1.0), vis.numColors colors starting at vis.colorIndex; t = age * colorsPerFrame,
//     color = lerp(color[t >> 12], color[(t >> 12) + 1], (t & 0xFFF) / 4096); 0 colors = 0x80808080, 1 color = constant
//   - texture: TextureInfo[vis.texInfoIndex] (0x5B9E80: numTextures, texIndex, du, dv); texId[texIndex] (0x6DFAB0, u16) indexes the
//     global texInfo and TEX0[texIndex] (0x6DF6B0, u64) is the TEX0 register (psm/CBP). texSprite picks the frame per particle:
//     attribs&1 random, attribs&2 uvI[i] >> 2, else (age * mapsPerFrame) >> 12 (attribs&4: holds on the last one, else wraps)
//   - blending: vis.alphaContext is the GS ALPHA register (0x44 = (Cs-Cd)*As+Cd, 0x48 = Cs*As+Cd additive)
//   - order: farthest to nearest by the depth of emit.ParticleToEmitVec (+0x2CA0) + vis.priority; opaque ones (alpha 0) first
// Read from the VU1 particle microcode (load packet at 0x253060, default context at 0x256B90; sprite calls 0x59, line 0x0B,
// strite 0x151):
//   - sprite: VU1 sends a GS SPRITE (screen-aligned rectangle) with half-side = pos.w / 2 * focus / depth, capped at
//     focus / 2 (measured by the VU1 vs native check on sprite and texSprite: the old reading, the whole pos.w, gave twice the size), i.e.
//     pos.w is the side in world units and the half-side never exceeds half the depth. The low bits of pos.w index the UV
//     corners (3 normal, 1 mirrors U, 2 mirrors V, 0 both); the color comes from the table by age, as in loadColors
//   - line: from pos to pos - vel (the tail is projected with the previous frame's matrix: camera motion blur)
//   - strite: see Particles::striteTriangle (tail with the previous frame's matrix, prevViewMat; ST that inscribes the disc in the triangle and
//     alpha scaled by the trail length)
// Debris (frag, 13) doesn't go through here: Scene::collectParticles/drawSprites (8x8 cell atlas; pos.w = side and low bits = cell/mirroring).
// Sprine: Particles::sprineQuads (sprites along the microprogram's curve). Still approximate: the strips (stripe, sweepTrail) join
// consecutive particles; kite becomes a sprite (wake and dustTrail follow the microprogram: wakeVu, dustTrailVu);
// the lines' tail uses the current matrix.
#pragma once
#include "wotm_syms.h"   // wotm::sym / wotm::fn: ELF addresses by name (tools/native_syms.py --gen)

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <condition_variable>
#include <cstring>
#include <deque>
#include <functional>
#include <mutex>
#include <thread>
#include <memory>
#include <unordered_map>
#include <vector>

#include "raylib.h"
#include "rlgl.h"

// GL 1.1 (opengl32), only to read depth for the lens flare occlusion test; raylib doesn't expose depth reads
#ifdef _WIN32
extern "C" __declspec(dllimport) void __stdcall glReadPixels(int x, int y, int w, int h, unsigned format, unsigned type, void *data);
#else
extern "C" void glReadPixels(int x, int y, int w, int h, unsigned format, unsigned type, void *data);
#endif

namespace wotm {

// Before the depth glReadPixels (lens flare occlusion test): with MSAA (wotm_msaa.hpp) drawing goes to a multisample framebuffer,
// which glReadPixels can't read; whoever uses it sets here the depth resolve. Null: reads the current framebuffer.
inline void (*g_beforeDepthRead)() = nullptr;

// A view's depth read without the window thread waiting (split screen, Scene::drawFlaresFor; lens flare occlusion test).
// Any GL read on the window thread (glMapBuffer, glClientWaitSync, glGet*) makes it wait for the driver to catch up: on the
// NVIDIA driver, with threaded optimization, it runs ~6 ms behind with 4 views on the Ryzen 5 5500, and each wait undoes the parallelism between
// the window, the driver and the GPU (redrawing 4 views went from ~16 to ~25 ms). So the window only copies the view's depth on the GPU
// (glBlitFramebuffer into its own renderbuffer, two per view in rotation), sets a fence and moves on. A helper thread, with a shared
// GL context (hidden GLFW window), waits on the fence on the GPU, reads the copy into a PBO, waits for the read and hands over the result;
// the waits are all its own. The test uses the newest ready copy (1 or 2 redraws behind), as long as it is at most kMaxAge requests
// old. Without the GL 3.2 functions or without the helper context, ok() stays false and the flare falls back to the direct read.
extern "C" {
extern void *(*glad_glFenceSync)(unsigned, unsigned);
extern unsigned (*glad_glClientWaitSync)(void *, unsigned, unsigned long long);
extern void (*glad_glWaitSync)(void *, unsigned, unsigned long long);
extern void (*glad_glDeleteSync)(void *);
extern void (*glad_glFlush)(void);
extern void (*glad_glGenBuffers)(int, unsigned *);
extern void (*glad_glBindBuffer)(unsigned, unsigned);
extern void (*glad_glBufferData)(unsigned, ptrdiff_t, const void *, unsigned);
extern void *(*glad_glMapBuffer)(unsigned, unsigned);
extern unsigned char (*glad_glUnmapBuffer)(unsigned);
extern void (*glad_glGenFramebuffers)(int, unsigned *);
extern void (*glad_glBindFramebuffer)(unsigned, unsigned);
extern void (*glad_glGenRenderbuffers)(int, unsigned *);
extern void (*glad_glDeleteRenderbuffers)(int, const unsigned *);
extern void (*glad_glBindRenderbuffer)(unsigned, unsigned);
extern void (*glad_glRenderbufferStorage)(unsigned, unsigned, int, int);
extern void (*glad_glFramebufferRenderbuffer)(unsigned, unsigned, unsigned, unsigned);
extern void (*glad_glBlitFramebuffer)(int, int, int, int, int, int, int, int, unsigned, unsigned);
extern void (*glad_glDrawBuffer)(unsigned);
extern void (*glad_glReadBuffer)(unsigned);
// GLFW (static in raylib): the helper context
void *glfwGetCurrentContext(void);
void glfwWindowHint(int, int);
void *glfwCreateWindow(int, int, const char *, void *, void *);
void glfwMakeContextCurrent(void *);
}
// The copy's renderbuffer must have the format of the depth being read: raylib's render texture one (unsized GL_DEPTH_COMPONENT) or,
// with MSAA, the one resolved by g_beforeDepthRead (GL_DEPTH_COMPONENT24). Whoever turns MSAA on says here whether it is in use.
inline bool (*g_depthReadIsMsaa)() = nullptr;

class AsyncDepth {
    struct Slot {
        unsigned rb = 0, fbo = 0;          // the copy's renderbuffer and its framebuffer (window context)
        int w = 0, h = 0;
        unsigned fmt = 0;
        unsigned gen = 0;                  // changes when the renderbuffer is remade (the helper thread remakes its framebuffer)
        std::atomic<int> busy{0};          // 1 = request queued or being read by the helper thread
    };
    struct Job { AsyncDepth *d; int slot; unsigned rb, gen; int w, h; void *fence; unsigned long serial; };

    // Helper thread with the shared context; one for all views
    class Worker {
    public:
        static Worker &get() { static Worker w; return w; }
        bool ok() {   // on the window thread, with its context current
            if (tried_) return ok_;
            tried_ = true;
            void *main = glfwGetCurrentContext();
            if (!main) return false;
            constexpr int kVisible = 0x00020004, kFocused = 0x00020001;
            glfwWindowHint(kVisible, 0);
            glfwWindowHint(kFocused, 0);
            ctx_ = glfwCreateWindow(16, 16, "wotm depth", nullptr, main);
            glfwWindowHint(kVisible, 1);
            glfwWindowHint(kFocused, 1);
            glfwMakeContextCurrent(main);
            if (!ctx_) {
                std::fprintf(stderr, "[flare] no helper GL context: lens flare occlusion uses a direct read in split screen\n");
                return false;
            }
            std::thread(&Worker::run, this).detach();
            ok_ = true;
            return true;
        }
        void post(const Job &j) {
            { std::lock_guard<std::mutex> lk(m_); q_.push_back(j); }
            cv_.notify_one();
        }

    private:
        void run() {
            glfwMakeContextCurrent(ctx_);
            struct Fb { unsigned fbo = 0, rb = 0, gen = ~0u; };
            std::unordered_map<const void *, Fb> fbs;   // per (view, slot)
            std::unordered_map<const void *, std::pair<unsigned, size_t>> pbos;
            for (;;) {
                Job j;
                {
                    std::unique_lock<std::mutex> lk(m_);
                    cv_.wait(lk, [&] { return !q_.empty(); });
                    j = q_.front();
                    q_.pop_front();
                }
                const void *key = &j.d->slots_[j.slot];
                Fb &fb = fbs[key];
                if (!fb.fbo) {
                    glad_glGenFramebuffers(1, &fb.fbo);
                    glad_glBindFramebuffer(kReadFb, fb.fbo);
                    glad_glDrawBuffer(0);
                    glad_glReadBuffer(0);
                }
                glad_glBindFramebuffer(kReadFb, fb.fbo);
                if (fb.rb != j.rb || fb.gen != j.gen) {
                    glad_glFramebufferRenderbuffer(kReadFb, kDepthAtt, kRenderbuffer, j.rb);
                    fb.rb = j.rb;
                    fb.gen = j.gen;
                }
                glad_glWaitSync(j.fence, 0, ~0ull);   // the window's copy finishes before the read (on the GPU)
                glad_glDeleteSync(j.fence);
                auto &pb = pbos[key];
                if (!pb.first) glad_glGenBuffers(1, &pb.first);
                const size_t bytes = size_t(j.w) * size_t(j.h) * 4;
                glad_glBindBuffer(kPack, pb.first);
                if (pb.second != bytes) {
                    glad_glBufferData(kPack, ptrdiff_t(bytes), nullptr, kStreamRead);
                    pb.second = bytes;
                }
                glReadPixels(0, 0, j.w, j.h, 0x1902 /* GL_DEPTH_COMPONENT */, 0x1406 /* GL_FLOAT */, nullptr);
                void *f2 = glad_glFenceSync(kGpuCommandsComplete, 0);
                glad_glFlush();
                unsigned r = 0;
                for (int tries = 0; tries < 50; ++tries) {
                    r = glad_glClientWaitSync(f2, kFlushBit, 100000000ull);   // 100 ms
                    if (r == kAlreadySignaled || r == kConditionSatisfied || r == kWaitFailed) break;
                }
                glad_glDeleteSync(f2);
                j.d->slots_[j.slot].busy = 0;   // read done: the window may overwrite the copy
                if (r == kAlreadySignaled || r == kConditionSatisfied) {
                    if (const void *p = glad_glMapBuffer(kPack, kReadOnly)) {
                        std::lock_guard<std::mutex> lk(j.d->m_);
                        j.d->res_.resize(bytes / 4);
                        std::memcpy(j.d->res_.data(), p, bytes);
                        j.d->resW_ = j.w;
                        j.d->resH_ = j.h;
                        j.d->resSerial_ = j.serial;
                    }
                    glad_glUnmapBuffer(kPack);
                }
                glad_glBindBuffer(kPack, 0);
                glad_glBindFramebuffer(kReadFb, 0);
            }
        }
        void *ctx_ = nullptr;
        bool tried_ = false, ok_ = false;
        std::mutex m_;
        std::condition_variable cv_;
        std::deque<Job> q_;
    };

public:
    bool ok() {
        if (!tried_) {
            tried_ = true;
            ok_ = glad_glFenceSync && glad_glClientWaitSync && glad_glWaitSync && glad_glDeleteSync && glad_glFlush && glad_glGenBuffers &&
                  glad_glBindBuffer && glad_glBufferData && glad_glMapBuffer && glad_glUnmapBuffer && glad_glGenFramebuffers &&
                  glad_glBindFramebuffer && glad_glGenRenderbuffers && glad_glDeleteRenderbuffers && glad_glBindRenderbuffer &&
                  glad_glRenderbufferStorage && glad_glFramebufferRenderbuffer && glad_glBlitFramebuffer && glad_glDrawBuffer &&
                  glad_glReadBuffer && Worker::get().ok();
            if (!ok_) std::fprintf(stderr, "[flare] missing GL 3.2 functions: lens flare occlusion uses a direct read in split screen\n");
        }
        return ok_;
    }
    // Takes the newest ready copy into cpu (without waiting); valid says whether there is a recent one
    void fetch(unsigned long redraw) {
        if (!ok()) { valid = false; return; }
        {
            std::lock_guard<std::mutex> lk(m_);
            if (resSerial_ > cpuSerial_ && resW_ > 0) {
                cpu.swap(res_);
                cw = resW_;
                ch = resH_;
                cpuSerial_ = resSerial_;
                resSerial_ = 0;
            }
        }
        valid = cpuSerial_ && redraw - cpuSerial_ <= kMaxAge;
    }
    // Copies on the GPU the depth of the framebuffer the view draws into (`fbo`, w x h) and asks the helper thread to read it. With no
    // free slot (the two previous copies still queued), it skips copying this time.
    void issue(int w, int h, unsigned fbo, unsigned long redraw) {
        if (!ok() || w <= 0 || h <= 0) return;
        Slot *s = nullptr;
        int k = 0;
        for (int i = 0; i < 2; ++i) {
            const int c = (next_ + i) & 1;
            if (!slots_[c].busy) { s = &slots_[c]; k = c; break; }
        }
        if (!s) return;
        next_ = k ^ 1;
        const bool msaa = g_depthReadIsMsaa && g_depthReadIsMsaa();
        const unsigned fmt = msaa ? kDepth24 : kDepthAny;
        if (!s->rb || s->w != w || s->h != h || s->fmt != fmt) {
            if (s->rb) glad_glDeleteRenderbuffers(1, &s->rb);
            glad_glGenRenderbuffers(1, &s->rb);
            glad_glBindRenderbuffer(kRenderbuffer, s->rb);
            glad_glRenderbufferStorage(kRenderbuffer, fmt, w, h);
            glad_glBindRenderbuffer(kRenderbuffer, 0);
            if (!s->fbo) glad_glGenFramebuffers(1, &s->fbo);
            glad_glBindFramebuffer(kFramebuffer, s->fbo);
            glad_glFramebufferRenderbuffer(kFramebuffer, kDepthAtt, kRenderbuffer, s->rb);
            glad_glDrawBuffer(0);
            glad_glReadBuffer(0);
            s->w = w;
            s->h = h;
            s->fmt = fmt;
            ++s->gen;
        }
        glad_glBindFramebuffer(kFramebuffer, fbo);
        if (g_beforeDepthRead) g_beforeDepthRead();   // MSAA: the resolved depth becomes the read source
        glad_glBindFramebuffer(kDrawFb, s->fbo);
        glad_glBlitFramebuffer(0, 0, w, h, 0, 0, w, h, kDepthBit, kNearest);
        glad_glBindFramebuffer(kFramebuffer, fbo);
        void *f = glad_glFenceSync(kGpuCommandsComplete, 0);
        glad_glFlush();   // the fence reaches the other context
        s->busy = 1;
        Worker::get().post({this, k, s->rb, s->gen, w, h, f, redraw});
    }
    std::vector<float> cpu;   // depth (0..1) of a recent redraw, rows bottom to top like GL
    int cw = 0, ch = 0;
    bool valid = false;

private:
    static constexpr unsigned kPack = 0x88EB, kReadOnly = 0x88B8, kStreamRead = 0x88E1;   // GL_PIXEL_PACK_BUFFER, GL_READ_ONLY, GL_STREAM_READ
    static constexpr unsigned kGpuCommandsComplete = 0x9117, kFlushBit = 1, kAlreadySignaled = 0x911A, kConditionSatisfied = 0x911C,
                              kWaitFailed = 0x911D;
    static constexpr unsigned kFramebuffer = 0x8D40, kReadFb = 0x8CA8, kDrawFb = 0x8CA9, kRenderbuffer = 0x8D41, kDepthAtt = 0x8D00,
                              kDepthAny = 0x1902 /* GL_DEPTH_COMPONENT */, kDepth24 = 0x81A6, kDepthBit = 0x100, kNearest = 0x2600;
    static constexpr unsigned long kMaxAge = 4;   // age, in redraws, of the cpu copy that is still used
    bool tried_ = false, ok_ = false;
    Slot slots_[2];
    int next_ = 0;
    unsigned long cpuSerial_ = 0;
    std::mutex m_;                 // res_* (written by the helper thread)
    std::vector<float> res_;
    int resW_ = 0, resH_ = 0;
    unsigned long resSerial_ = 0;
};

// Vertex normal for the Scene::draw shader: (fogBurnThru, 1, FGE + 2 * GS). rlNormal3f normalizes it; the shader reads the ratios
// over y. FGE = fog; GS = raw GS alpha in the texture and the vertex (As = At * Av / 16384). rlNormal3f(0, 0, 0) turns both off.
// PM (+4): the blend is additive or subtractive and the shader returns the color already times the whole As (gsBlend uses factor 1 on the source). The GS
// As goes up to 255/128 (At * Av / 128 saturated at 255 by the texture function); GL's SRC_ALPHA factor would cap it at 1. In the common blend
// ((Cs - Cd) As + Cd) an As > 1 would need a negative destination factor: extra passes (gsBlendOver1/2).
// Mode (+8 * mode): 1 = outputs alpha As / 2 and 2 = outputs white (the two passes of GsBlendFactors::doubled); 3, 4 and 5 = the three passes of
// the common blend with As > 1 (gsBlendCommon).
inline bool g_premulShader = false;   // the Scene::draw shader is active (without it the color would go out unmultiplied)
// The burn is clamped to +-4096: F saturates (0 or 255) long before that, and the AIRPORT sky dome has fogBurnThru 1e7, which left the
// normalized y below the shader's threshold (1e-6) and cleared every flag (the dome came out without GS mode, ~0.13 of the color).
inline void shaderNormalVals(float burn, bool fge, bool gs, bool pm, int mode, float out[3]) {
    out[0] = burn > 4096.f ? 4096.f : (burn < -4096.f ? -4096.f : burn);
    out[1] = 1.f;
    out[2] = (fge ? 1.f : 0.f) + (gs ? 2.f : 0.f) + (pm && g_premulShader ? 4.f : 0.f) + (g_premulShader ? 8.f * float(mode) : 0.f);
}
inline void shaderNormal(float burn, bool fge, bool gs, bool pm = false, int mode = 0) {
    float n[3];
    shaderNormalVals(burn, fge, gs, pm, mode, n);
    rlNormal3f(n[0], n[1], n[2]);
}
// GS blending from the low byte of the ALPHA register: color = (A - B) * C + D, 2-bit fields A, B, D (0 Cs, 1 Cd, 2 zero) and C (0 As,
// 1 Ad, 2 FIX; Ad and FIX don't appear in the measured objects and are treated as As). In GL it becomes Cs * (as * As + ks) + Cd * (ad * As + kd):
// 0x44 = (Cs - Cd) As + Cd (common), 0x48 = Cs As + Cd (additive), 0x42 = Cd - Cs As, 0x46 = Cd (1 - As) and 0x81 = (Cd - Cs) As (the
// Kineticlops colors). A 1 + As factor doesn't exist in GL and saturates at 1. With the As factor only on the source, the shader premultiplies (gsBlendPremul:
// shaderNormal with pm), and As goes above 1 as on the GS. With D = 0 and As on the destination (0x81) premultiplying isn't possible, and the GS As
// goes up to ~2 (texture 0xFF with vertex 0x80): doubled = draw with As / 2 (shaderNormal mode 1) and double the destination in a second
// pass of the same geometry (gsBlendDouble, mode 2). With D = Cd an As > 1 would only zero the factor, which GL already does.
struct GsBlendFactors { int src = RL_SRC_ALPHA, dst = RL_ONE_MINUS_SRC_ALPHA, eq = RL_FUNC_ADD; bool premul = false, doubled = false; };
inline GsBlendFactors gsBlendFactors(uint8_t alpha) {
    const int A = alpha & 3, B = (alpha >> 2) & 3, D = (alpha >> 6) & 3;
    struct Term { int f; bool neg; };
    auto term = [](int a, int k) -> Term {   // coefficient a * As + k (a in -1..1, k in 0..1) -> GL factor and sign
        if (a == 0) return {k ? RL_ONE : RL_ZERO, false};
        if (a > 0) return {k ? RL_ONE : RL_SRC_ALPHA, false};   // 1 + As: saturates
        return k ? Term{RL_ONE_MINUS_SRC_ALPHA, false} : Term{RL_SRC_ALPHA, true};
    };
    const Term s = term((A == 0) - (B == 0), D == 0), d = term((A == 1) - (B == 1), D == 1);
    GsBlendFactors r;
    r.src = s.f;
    r.dst = d.f;
    if (s.neg && d.neg) r.src = r.dst = RL_ZERO;   // only negative: the GS saturates at 0
    else if (s.neg) r.eq = RL_FUNC_REVERSE_SUBTRACT;
    else if (d.neg) r.eq = RL_FUNC_SUBTRACT;
    r.premul = r.src == RL_SRC_ALPHA && (r.dst == RL_ZERO || r.dst == RL_ONE);
    r.doubled = D == 2 && r.dst == RL_SRC_ALPHA;
    return r;
}
// Second pass of doubled: destination * 2 (white output times DST_COLOR, plus the destination)
inline void gsBlendDouble() {
    rlSetBlendFactors(RL_DST_COLOR, RL_ONE, RL_FUNC_ADD);
    rlSetBlendMode(RL_BLEND_CUSTOM);
}
// Common blend with As > 1 (texture 0xFF and vertex above 0x40: As = At * Av / 16384 up to 255/128). The GS does Cs As - Cd (As - 1),
// clamped to 0..255; in GL the destination factor would be negative. Three passes of the same geometry, each pixel deciding in the shader:
//  1. the common blend, with alpha 0 where As > 1 (the destination stays);
//  2. (gsBlendOver1, shaderNormal mode 4) only where As > 1: Cs * 1 - Cd * (1 - 1/As) = result / As (discards the rest);
//  3. (gsBlendOver2, mode 5) destination * As: output As - 1 times DST_COLOR, plus the destination (0 where As <= 1).
// The 8-bit framebuffer only rounds result / As (error up to As / 2 < 1). Common = factors SRC_ALPHA and 1 - SRC_ALPHA, add.
// The framebuffer alpha (read by the previous frame) ends up as in the common blend with As 1: the 2nd pass doesn't touch it and the 3rd sets it to 1.
inline bool gsBlendCommon(uint8_t alpha) {
    const GsBlendFactors f = gsBlendFactors(alpha);
    return f.src == RL_SRC_ALPHA && f.dst == RL_ONE_MINUS_SRC_ALPHA && f.eq == RL_FUNC_ADD;
}
inline void gsBlendOver1() {
    rlSetBlendFactorsSeparate(RL_ONE, RL_SRC_ALPHA, RL_ZERO, RL_ONE, RL_FUNC_SUBTRACT, RL_FUNC_ADD);
    rlSetBlendMode(RL_BLEND_CUSTOM_SEPARATE);
}
inline void gsBlendOver2() {
    rlSetBlendFactorsSeparate(RL_DST_COLOR, RL_ONE, RL_ONE, RL_ONE, RL_FUNC_ADD, RL_FUNC_ADD);
    rlSetBlendMode(RL_BLEND_CUSTOM_SEPARATE);
}
inline bool gsBlendPremul(uint8_t alpha) { return gsBlendFactors(alpha).premul; }
inline void gsBlend(uint8_t alpha) {
    const GsBlendFactors f = gsBlendFactors(alpha);
    rlSetBlendFactors(f.premul && g_premulShader ? RL_ONE : f.src, f.dst, f.eq);
    rlSetBlendMode(RL_BLEND_CUSTOM);
}
inline constexpr uint8_t kGsAlphaNormal = 0x44;   // (Cs - Cd) As + Cd
inline constexpr uint8_t kGsAlphaAdd = 0x48;      // Cs As + Cd
inline constexpr uint8_t kGsNoAbe = 0x0A;         // (0 - 0) As + Cs = Cs: PRIM.ABE 0, alpha only counts for the test (TEST_1)

// White texture with alpha 0x80 (GS 1.0): the "no texture" in GS mode (raylib's default one, alpha 255, would count as 2.0).
inline unsigned gsWhiteTexture() {
    static unsigned id = 0;
    if (!id) {
        uint32_t px = 0x80FFFFFFu;
        Image img{&px, 1, 1, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        id = LoadTextureFromImage(img).id;
    }
    return id;
}

// ---------------------------------------------------------------- vertices without rlgl's immediate mode
// drawList used to emit each vertex with rlColor4ub, rlTexCoord2f, rlNormal3f (which normalizes with a square root) and rlVertex3f: four raylib
// calls per vertex, ~40 ns per vertex on the Ryzen 3 3200G, half of the traversal time. VtxStream stores the same attributes
// interleaved in its own vector, and on flush uploads everything at once (glBufferData, a new buffer on every flush, without waiting on the GPU)
// and draws like rlDrawRenderBatch: the same shader (whichever the caller says rlgl has bound), the MVP (modelview x projection, the math
// of rlMatrixMultiply), colDiffuse 1 and texture0 on unit 0, the attributes at rlgl's batch locations, one glDrawArrays per command
// (mode and texture). GL state (blending, Z mask, alpha test uniform, CLAMP) applies at flush time, as in rlgl:
// callers flush before changing it. Without the GL 3 functions (glad), ok() is false and the caller falls back to rlgl.
extern "C" {
extern void (*glad_glGenVertexArrays)(int, unsigned *);
extern void (*glad_glBindVertexArray)(unsigned);
extern void (*glad_glGenBuffers)(int, unsigned *);
extern void (*glad_glBindBuffer)(unsigned, unsigned);
extern void (*glad_glBufferData)(unsigned, ptrdiff_t, const void *, unsigned);
extern void (*glad_glVertexAttribPointer)(unsigned, int, unsigned, unsigned char, int, const void *);
extern void (*glad_glEnableVertexAttribArray)(unsigned);
extern void (*glad_glUseProgram)(unsigned);
extern void (*glad_glUniformMatrix4fv)(int, int, unsigned char, const float *);
extern void (*glad_glUniform4f)(int, float, float, float, float);
extern void (*glad_glUniform1i)(int, int);
extern void (*glad_glActiveTexture)(unsigned);
extern void (*glad_glBindTexture)(unsigned, unsigned);
extern void (*glad_glDrawArrays)(unsigned, int, int);
extern void (*glad_glBufferSubData)(unsigned, ptrdiff_t, ptrdiff_t, const void *);
}
// Drawing helper threads: parallelFor(n, fn) calls fn(i) for i in [0, n), split between them and the caller, and returns when all of them
// are done. fn may only read what nobody writes during the call (no GL and no Scene caches). How many: WOTM_DRAWTHREADS
// (0 = none, everything on the caller; the default: helper threads lower the EE core's clock and it loses more than the window gains).
class JobPool {
public:
    static JobPool &get() { static JobPool *p = new JobPool; return *p; }   // never destroyed: the threads die with the process
    int workers() const { return int(th_.size()); }
    // Use the threads now? With WOTM_DRAWTHREADS_AB=<s> (measurement), alternates every s seconds between with and without, printing a
    // "[native] JobPool: with|without threads" line on each switch, to compare the two halves of the same fight
    bool active() {
        if (th_.empty()) return false;
        static const double ab = std::getenv("WOTM_DRAWTHREADS_AB") ? std::atof(std::getenv("WOTM_DRAWTHREADS_AB")) : 0.0;
        if (ab <= 0.0) return true;
        const double t = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
        const bool on = (long long)(t / ab) % 2 == 0;
        if (on != abLast_) { abLast_ = on; std::fprintf(stderr, "[native] JobPool: %s threads\n", on ? "with" : "without"); }
        return on;
    }
    template <class F> void parallelFor(int n, F &&fn) {
        if (n <= 0) return;
        if (th_.empty() || n == 1) { for (int i = 0; i < n; ++i) fn(i); return; }
        std::function<void(int)> f = std::forward<F>(fn);
        {
            std::lock_guard<std::mutex> lk(m_);
            fn_ = &f;
            n_ = n;
            next_.store(0, std::memory_order_relaxed);
            busy_ = int(th_.size());
            ++gen_;
        }
        cv_.notify_all();
        run();
        std::unique_lock<std::mutex> lk(m_);
        while (!done_.wait_for(lk, std::chrono::seconds(2), [&] { return busy_ == 0; }))
            std::fprintf(stderr, "[native] JobPool: waiting on %d threads for over 2 s (n %d, next %d, generation %llu)\n", busy_, n_,
                         next_.load(), (unsigned long long)gen_);
        fn_ = nullptr;
    }
private:
    JobPool() {
        int k;
        if (const char *e = std::getenv("WOTM_DRAWTHREADS")) k = std::atoi(e);
        else k = 0;
        for (int i = 0; i < k; ++i) { th_.emplace_back([this] { loop(); }); th_.back().detach(); }
        if (k > 0) std::fprintf(stderr, "[native] drawing: %d helper threads (WOTM_DRAWTHREADS)\n", k);
    }
    void run() {
        for (int i; (i = next_.fetch_add(1, std::memory_order_relaxed)) < n_;) (*fn_)(i);
    }
    void loop() {
        uint64_t seen = 0;
        for (;;) {
            {
                std::unique_lock<std::mutex> lk(m_);
                cv_.wait(lk, [&] { return gen_ != seen; });
                seen = gen_;
                if (quit_) return;
            }
            run();
            std::lock_guard<std::mutex> lk(m_);
            if (--busy_ == 0) done_.notify_one();
        }
    }
    std::vector<std::thread> th_;
    std::mutex m_;
    std::condition_variable cv_, done_;
    std::function<void(int)> *fn_ = nullptr;
    int n_ = 0, busy_ = 0;
    std::atomic<int> next_{0};
    uint64_t gen_ = 0;
    bool quit_ = false;
    bool abLast_ = false;
};

class VtxStream {
public:
    struct V { float x, y, z, u, v, nx, ny, nz; uint8_t c[4]; };
    static_assert(sizeof(V) == 36, "interleaved layout: position, UV, normal, color");

    bool ok() {
        if (!tried_) {
            tried_ = true;
            ok_ = glad_glGenVertexArrays && glad_glBindVertexArray && glad_glGenBuffers && glad_glBindBuffer && glad_glBufferData && glad_glBufferSubData &&
                  glad_glVertexAttribPointer && glad_glEnableVertexAttribArray && glad_glUseProgram && glad_glUniformMatrix4fv &&
                  glad_glUniform4f && glad_glUniform1i && glad_glActiveTexture && glad_glBindTexture && glad_glDrawArrays;
            if (ok_) {
                makeVao(vao_, vbo_);
            } else {
                std::fprintf(stderr, "[native] missing GL 3 functions: vertices go through rlgl's immediate mode\n");
            }
        }
        return ok_;
    }
    // The shader rlgl has bound right now (the caller's BeginShaderMode, or the default): the one for the following commands
    void setShader(unsigned id, const int *locs) { shader_ = id; locs_ = locs; }

    // Start of a command (like rlBegin + rlSetTexture): RL_TRIANGLES or RL_LINES. Stores the current shader and MVP (the flush may
    // come after the caller changes the matrices)
    void begin(int mode, unsigned tex) {
        const int m = curMvp();
        if (!cmds_.empty() && cmds_.back().kind == Cmd::Draw && cmds_.back().count == 0 && cmds_.back().job < 0 && !cmds_.back().res) {
            Cmd &c = cmds_.back();
            c.mode = mode; c.tex = tex; c.mvp = m; c.shader = shader_; c.locs = locs_;
            return;
        }
        cmds_.push_back({Cmd::Draw, mode, tex, int(v_.size()), 0, m, shader_, locs_, -1, -1});
    }
    // A GL state change (blending, alpha test, Z mask, CLAMP) in vertex order: with nothing queued, it runs now;
    // otherwise it is queued and runs at flush time, between the draws before and after it. One upload per flush instead of one per
    // state change (hundreds per view during the traversal).
    template <class F> void state(F &&fn) {
        if (empty()) { fn(); return; }
        ops_.emplace_back(std::forward<F>(fn));
        cmds_.push_back({Cmd::State, 0, 0, int(v_.size()), 0, -1, 0, nullptr, int(ops_.size()) - 1, -1});
    }
    // Deferred chunk: the vertices for this point of the open draw come later, in the buffer deferBuf(j) (filled by the requester, on
    // any thread, before the flush); the flush puts them in this spot, in order. Returns j.
    int defer() {
        if (cmds_.empty() || cmds_.back().kind != Cmd::Draw || cmds_.back().count != 0 || cmds_.back().job >= 0 || cmds_.back().res) reopen();
        const int j = nDefer_++;
        if (bufs_.size() < size_t(nDefer_)) bufs_.resize(size_t(nDefer_));
        bufs_[size_t(j)].clear();
        cmds_.back().job = j;
        return j;
    }
    std::vector<V> &deferBuf(int j) { return bufs_[size_t(j)]; }

    // Resident arena: vertices that stay on the GPU across views and frames (geometry that hasn't changed; Scene::drawList). arenaAlloc gives
    // the position of n vertices (-1 = full: the caller clears it with arenaReset, which orphans the buffer, outside of a list with arena draws
    // queued); arenaUpload writes them; resident draws a range at the current point of the queue, with the open command (texture, shader, MVP) and,
    // with cull, GL_CULL_FACE on (the same rule as Scene::backFacing: front face counter-clockwise).
    long arenaAlloc(size_t n) {
        if (!ok_) return -1;
        if (!arenaVao_) {
            makeVao(arenaVao_, arenaVbo_);
            glad_glBindBuffer(kArrayBuffer, arenaVbo_);
            glad_glBufferData(kArrayBuffer, ptrdiff_t(kArenaCap * sizeof(V)), nullptr, kStaticDraw);
            glad_glBindBuffer(kArrayBuffer, 0);
        }
        if (arenaUsed_ + n > kArenaCap) return -1;
        const long off = long(arenaUsed_);
        arenaUsed_ += n;
        return off;
    }
    void arenaUpload(long off, const V *data, size_t n) {
        if (!n) return;
        glad_glBindBuffer(kArrayBuffer, arenaVbo_);
        glad_glBufferSubData(kArrayBuffer, ptrdiff_t(size_t(off) * sizeof(V)), ptrdiff_t(n * sizeof(V)), data);
        glad_glBindBuffer(kArrayBuffer, 0);
        arenaUpBytes += double(n * sizeof(V));
    }
    void arenaReset() {
        arenaUsed_ = 0;
        if (!arenaVbo_) return;
        glad_glBindBuffer(kArrayBuffer, arenaVbo_);
        glad_glBufferData(kArrayBuffer, ptrdiff_t(kArenaCap * sizeof(V)), nullptr, kStaticDraw);
        glad_glBindBuffer(kArrayBuffer, 0);
    }
    void resident(long off, int count, bool cull) {
        if (count <= 0) return;
        Cmd d{};
        for (size_t i = cmds_.size(); i-- > 0;)
            if (cmds_[i].kind == Cmd::Draw) { d = cmds_[i]; break; }
        d.kind = Cmd::Draw; d.first = int(off); d.count = count; d.job = -1; d.res = 1; d.cull = cull ? 1 : 0;
        cmds_.push_back(d);
        ++nRes_;
    }
    static constexpr size_t kArenaCap = size_t(1) << 20;   // vertices (36 MB)
    static inline double arenaUpBytes = 0.0;               // WOTM_DRAWSTATS: upload to the arena
    // Draw commands with vertices since the last reset (WOTM_DRAWSTATS)
    long draws() const { return draws_; }
    void resetDraws() { draws_ = 0; }
    void color(uint8_t r, uint8_t g, uint8_t b, uint8_t a) { cur_.c[0] = r; cur_.c[1] = g; cur_.c[2] = b; cur_.c[3] = a; }
    void texcoord(float u, float v) { cur_.u = u; cur_.v = v; }
    void normal(float x, float y, float z) {   // like rlNormal3f (no transform: 3D mode doesn't use rlPushMatrix here)
        if (x == lastIn_[0] && y == lastIn_[1] && z == lastIn_[2]) return;   // same as before (almost always: it's per item, not per vertex)
        lastIn_[0] = x; lastIn_[1] = y; lastIn_[2] = z;
        const float length = sqrtf(x * x + y * y + z * z);
        if (length != 0.0f) { const float il = 1.0f / length; x *= il; y *= il; z *= il; }
        cur_.nx = x; cur_.ny = y; cur_.nz = z;
    }
    void vertex(float x, float y, float z) {
        cur_.x = x; cur_.y = y; cur_.z = z;
        if (cmds_.back().kind != Cmd::Draw || cmds_.back().job >= 0 || cmds_.back().res) reopen();   // after a state change, a deferred chunk or the arena: the same draw continues
        v_.push_back(cur_);
        ++cmds_.back().count;
    }
    bool empty() const { return v_.empty() && nDefer_ == 0 && nRes_ == 0; }
    size_t vertices() const { return v_.size(); }

    // Uploads and draws what has accumulated, with the queued state changes in between; clears the queue
    void flush() {
        const V *up = v_.data();   // what gets uploaded: v_, or the merge with the deferred chunks (mbuf_)
        size_t upN = v_.size();
        if (nDefer_) {   // the deferred chunks in their place (the draw commands are in vertex order); the copy runs in parallel
            copies_.clear();
            size_t total = 0;
            for (Cmd &c : cmds_) {
                if (c.kind != Cmd::Draw) { c.first = int(total); continue; }
                if (c.res) continue;   // arena range: left out of the merge
                const V *src = c.job >= 0 ? bufs_[size_t(c.job)].data() : v_.data() + c.first;
                if (c.job >= 0) c.count = int(bufs_[size_t(c.job)].size());
                c.job = -1;
                c.first = int(total);
                if (c.count) copies_.push_back({src, size_t(c.count), total});
                total += size_t(c.count);
            }
            if (total > mcap_) { mcap_ = total + total / 4; mbuf_.reset(new V[mcap_]); }   // not zeroed (a vector would zero ~10 MB)
            V *dst = mbuf_.get();
            JobPool::get().parallelFor(int(copies_.size()), [this, dst](int i) {
                const Copy &k = copies_[size_t(i)];
                std::memcpy(dst + k.dst, k.src, k.n * sizeof(V));
            });
            up = dst;
            upN = total;
            nDefer_ = 0;
        }
        if ((!upN && !nRes_) || !ok_) {   // no vertices: only the state changes (in order)
            for (const Cmd &c : cmds_) if (c.kind == Cmd::State) ops_[size_t(c.op)]();
            v_.clear(); cmds_.clear(); ops_.clear(); mvps_.clear();
            nRes_ = 0;
            return;
        }
        rlDrawRenderBatchActive();   // whatever rlgl has queued goes out first (ordering)
        const auto t0 = std::chrono::steady_clock::now();
        if (upN) {
        // a new empty buffer (orphaned: the GPU may still be reading the previous one) and the data via glBufferSubData. With glBufferData
        // carrying the data (several MB per flush), on the RX 570 driver split-screen quadrants 3 and 4 occasionally came out without the stage
        // (any GL state read, which syncs the driver, hid the defect)
        glad_glBindBuffer(kArrayBuffer, vbo_);
        glad_glBufferData(kArrayBuffer, ptrdiff_t(upN * sizeof(V)), nullptr, kStreamDraw);
        glad_glBufferSubData(kArrayBuffer, 0, ptrdiff_t(upN * sizeof(V)), up);
        glad_glBindBuffer(kArrayBuffer, 0);
        }
        const auto t1 = std::chrono::steady_clock::now();
        upBytes += double(upN * sizeof(V));
        // GL state the draws here use; a queued state change (rlgl functions) may switch the program and the VAO
        bool bound = false;
        unsigned prog = 0, vao = 0;
        int mvp = -1;
        bool cullOn = false;   // drawList turns GL_CULL_FACE off; only arena ranges with cull turn it on
        for (const Cmd &c : cmds_) {
            if (c.kind == Cmd::State) {
                ops_[size_t(c.op)]();
                bound = false;
                continue;
            }
            if (!c.count) continue;
            const unsigned p = c.shader ? c.shader : rlGetShaderIdDefault();
            const int *locs = c.locs ? c.locs : rlGetShaderLocsDefault();
            if (!bound || p != prog) {
                glad_glUseProgram(p);
                if (p != prog) mvp = -1;
                prog = p;
                // (matProjection/matView/matModel/matNormal: the drawing shaders don't have them)
                glad_glUniform4f(locs[RL_SHADER_LOC_COLOR_DIFFUSE], 1.0f, 1.0f, 1.0f, 1.0f);
                glad_glUniform1i(locs[RL_SHADER_LOC_MAP_DIFFUSE], 0);
                vao = 0;
                glad_glActiveTexture(kTexture0);
                bound = true;
            }
            const unsigned wantVao = c.res ? arenaVao_ : vao_;
            if (vao != wantVao) { glad_glBindVertexArray(wantVao); vao = wantVao; }
            if (bool(c.cull) != cullOn) { cullOn = c.cull != 0; if (cullOn) rlEnableBackfaceCulling(); else rlDisableBackfaceCulling(); }
            if (c.mvp != mvp) {
                glad_glUniformMatrix4fv(locs[RL_SHADER_LOC_MATRIX_MVP], 1, 0, mvps_[size_t(c.mvp)].m);
                mvp = c.mvp;
            }
            glad_glBindTexture(kTexture2D, c.tex);
            glad_glDrawArrays(unsigned(c.mode), c.first, c.count);
            ++draws_;
        }
        if (cullOn) rlDisableBackfaceCulling();
        glad_glBindTexture(kTexture2D, 0);
        glad_glBindVertexArray(0);
        glad_glUseProgram(0);
        nRes_ = 0;
        const auto t2 = std::chrono::steady_clock::now();
        upMs += std::chrono::duration<double, std::milli>(t1 - t0).count();
        drawMs += std::chrono::duration<double, std::milli>(t2 - t1).count();
        v_.clear();
        cmds_.clear();
        ops_.clear();
        mvps_.clear();
    }
    static inline double upMs = 0.0, drawMs = 0.0, upBytes = 0.0;   // WOTM_DRAWSTATS: upload and draws of the flushes

private:
    struct Cmd {
        enum Kind { Draw, State } kind;
        int mode; unsigned tex; int first, count;
        int mvp;              // index into mvps_
        unsigned shader; const int *locs;
        int op;               // index into ops_ (State)
        int job;              // deferred chunk (defer): index into bufs_, -1 = vertices in v_
        uint8_t res = 0;      // resident arena range (first is in the arena)
        uint8_t cull = 0;     // with GL_CULL_FACE (arena range of a section with culling)
    };
    struct Mvp { float m[16]; };
    void reopen() {
        for (size_t i = cmds_.size(); i-- > 0;)
            if (cmds_[i].kind == Cmd::Draw) {
                Cmd d = cmds_[i];
                d.first = int(v_.size()); d.count = 0; d.job = -1; d.res = 0; d.cull = 0;
                cmds_.push_back(d);
                return;
            }
        begin(RL_TRIANGLES, rlGetTextureIdDefault());
    }
    // The current MVP (rlMatrixMultiply(modelview, projection), in rlMatrixToFloat order), reusing the last one if it's the same
    int curMvp() {
        const Matrix a = rlGetMatrixModelview(), b = rlGetMatrixProjection();
        const Mvp x{{
            a.m0 * b.m0 + a.m1 * b.m4 + a.m2 * b.m8 + a.m3 * b.m12,     a.m0 * b.m1 + a.m1 * b.m5 + a.m2 * b.m9 + a.m3 * b.m13,
            a.m0 * b.m2 + a.m1 * b.m6 + a.m2 * b.m10 + a.m3 * b.m14,    a.m0 * b.m3 + a.m1 * b.m7 + a.m2 * b.m11 + a.m3 * b.m15,
            a.m4 * b.m0 + a.m5 * b.m4 + a.m6 * b.m8 + a.m7 * b.m12,     a.m4 * b.m1 + a.m5 * b.m5 + a.m6 * b.m9 + a.m7 * b.m13,
            a.m4 * b.m2 + a.m5 * b.m6 + a.m6 * b.m10 + a.m7 * b.m14,    a.m4 * b.m3 + a.m5 * b.m7 + a.m6 * b.m11 + a.m7 * b.m15,
            a.m8 * b.m0 + a.m9 * b.m4 + a.m10 * b.m8 + a.m11 * b.m12,   a.m8 * b.m1 + a.m9 * b.m5 + a.m10 * b.m9 + a.m11 * b.m13,
            a.m8 * b.m2 + a.m9 * b.m6 + a.m10 * b.m10 + a.m11 * b.m14,  a.m8 * b.m3 + a.m9 * b.m7 + a.m10 * b.m11 + a.m11 * b.m15,
            a.m12 * b.m0 + a.m13 * b.m4 + a.m14 * b.m8 + a.m15 * b.m12, a.m12 * b.m1 + a.m13 * b.m5 + a.m14 * b.m9 + a.m15 * b.m13,
            a.m12 * b.m2 + a.m13 * b.m6 + a.m14 * b.m10 + a.m15 * b.m14, a.m12 * b.m3 + a.m13 * b.m7 + a.m14 * b.m11 + a.m15 * b.m15}};
        if (!mvps_.empty() && std::memcmp(mvps_.back().m, x.m, sizeof x.m) == 0) return int(mvps_.size()) - 1;
        mvps_.push_back(x);
        return int(mvps_.size()) - 1;
    }
    void makeVao(unsigned &vao, unsigned &vbo) {
        glad_glGenVertexArrays(1, &vao);
        glad_glGenBuffers(1, &vbo);
        glad_glBindVertexArray(vao);
        glad_glBindBuffer(kArrayBuffer, vbo);
        // the fixed locations rlLoadShaderProgram binds in the shaders (glBindAttribLocation): the default shader has no
        // normal (location -1) and alphaShader_'s is 2
        const int loc[4] = {RL_DEFAULT_SHADER_ATTRIB_LOCATION_POSITION, RL_DEFAULT_SHADER_ATTRIB_LOCATION_TEXCOORD,
                            RL_DEFAULT_SHADER_ATTRIB_LOCATION_NORMAL, RL_DEFAULT_SHADER_ATTRIB_LOCATION_COLOR};
        const int size[4] = {3, 2, 3, 4};
        const size_t off[4] = {offsetof(V, x), offsetof(V, u), offsetof(V, nx), offsetof(V, c)};
        for (int a = 0; a < 4; ++a) {
            if (loc[a] < 0) continue;
            glad_glVertexAttribPointer(unsigned(loc[a]), size[a], a == 3 ? kUByte : kFloat, a == 3 ? 1 : 0, int(sizeof(V)),
                                       reinterpret_cast<const void *>(off[a]));
            glad_glEnableVertexAttribArray(unsigned(loc[a]));
        }
        glad_glBindVertexArray(0);
        glad_glBindBuffer(kArrayBuffer, 0);
    }
    static constexpr unsigned kArrayBuffer = 0x8892, kStreamDraw = 0x88E0, kStaticDraw = 0x88E8, kFloat = 0x1406, kUByte = 0x1401, kTexture2D = 0x0DE1,
                              kTexture0 = 0x84C0;
    std::vector<V> v_;
    std::unique_ptr<V[]> mbuf_;   // v_ merged with the deferred chunks (flush)
    size_t mcap_ = 0;
    std::vector<std::vector<V>> bufs_;   // deferred chunks (defer), reused across flushes
    struct Copy { const V *src; size_t n, dst; };
    std::vector<Copy> copies_;
    int nDefer_ = 0, nRes_ = 0;
    unsigned arenaVao_ = 0, arenaVbo_ = 0;
    size_t arenaUsed_ = 0;
    std::vector<Cmd> cmds_;
    std::vector<std::function<void()>> ops_;
    std::vector<Mvp> mvps_;
    long draws_ = 0;
    V cur_{};
    float lastIn_[3] = {0.f, 0.f, 0.f};   // input normal that produced cur_ (the initial 0,0,0 gives 0,0,0, as in rlgl)
    unsigned vao_ = 0, vbo_ = 0, shader_ = 0;
    const int *locs_ = nullptr;
    bool tried_ = false, ok_ = false;
};

// GPU time of the redraw (WOTM_DRAWSTATS): a GL_TIME_ELAPSED query around it, read 3 redraws later (already available)
extern "C" {
extern void (*glad_glGenQueries)(int, unsigned *);
extern void (*glad_glBeginQuery)(unsigned, unsigned);
extern void (*glad_glEndQuery)(unsigned);
extern void (*glad_glGetQueryObjectui64v)(unsigned, unsigned, unsigned long long *);
}
struct GpuTimer {
    static GpuTimer &get() { static GpuTimer g; return g; }
    double ms = 0.0;   // sum since the last reset
    long n = 0;
    void begin() {
        if (!glad_glGenQueries || !glad_glBeginQuery || !glad_glEndQuery || !glad_glGetQueryObjectui64v) return;
        if (!q_[0]) glad_glGenQueries(4, q_);
        if (used_[k_]) {
            unsigned long long ns = 0;
            glad_glGetQueryObjectui64v(q_[k_], 0x8866 /* GL_QUERY_RESULT */, &ns);
            ms += double(ns) * 1e-6;
            ++n;
        }
        glad_glBeginQuery(0x88BF /* GL_TIME_ELAPSED */, q_[k_]);
        open_ = true;
    }
    void end() {
        if (!open_) return;
        glad_glEndQuery(0x88BF);
        used_[k_] = true;
        k_ = (k_ + 1) & 3;
        open_ = false;
    }
    struct Scope {
        bool on;
        Scope() : on(enabled()) { if (on) get().begin(); }
        static bool enabled() { static const bool e = std::getenv("WOTM_DRAWSTATS") != nullptr; return e; }
        ~Scope() { if (on) get().end(); }
    };
private:
    unsigned q_[4] = {};
    bool used_[4] = {};
    int k_ = 0;
    bool open_ = false;
};

// EndMode3D inside a BeginTextureMode. raylib's asks GL for the active framebuffer (rlGetActiveFramebuffer, a glGetIntegerv)
// to apply the screen scale on framebuffer 0; on the NVIDIA driver, with threaded optimization, that read waits for the driver to drain
// its queue: ~1.5 ms per view (6 ms per 4-view redraw on the Ryzen 5 5500). The scale doesn't apply to a render texture.
inline void endMode3DInTexture() {
    rlDrawRenderBatchActive();
    rlMatrixMode(RL_PROJECTION);
    rlPopMatrix();
    rlMatrixMode(RL_MODELVIEW);
    rlLoadIdentity();
    rlDisableDepthTest();
}

// The GS screen (640 x 224, shown at 4:3) inside the native fbw x fbh frame: the central strip at full height. In widescreen
// (Hor+: the game's vertical FOV stays, the width grows) the extra space is on the sides; whatever the game places in GS pixels (text, lens
// flare, previous frame) stays in that strip.
struct GsScreenRect { float x0, w, h; };
// In split screen (ViewScope of a 320 x 224 view) the strip is the view's: 2:3 in the half frame.
inline GsScreenRect gsScreenRect(float fbw, float fbh) {
    const float w = std::min(fbw, fbh * curView().aspect());
    return {(fbw - w) * 0.5f, w, fbh};
}

namespace fxaddr {
constexpr uint32_t fx = wotm::sym::fx;           // _ParticleType fx[100]
constexpr uint32_t stride = 0x2EE0;           // sizeof(_ParticleType)
constexpr uint32_t count = 100;
constexpr uint32_t fxList = wotm::sym::fxList;       // 12 bytes per system: next at +8
constexpr uint32_t fxActiveTop = wotm::sym::fxActiveTop;  // first active one (-1 = none)
constexpr uint32_t textureInfo = wotm::sym::TextureInfo;  // TextureInfoType[128]: numTextures s16, texIndex u16, du u16, dv u16
constexpr uint32_t texId = wotm::sym::texId;        // u16[128]: index into the global texInfo
constexpr uint32_t tex0 = wotm::sym::TEX0;         // u64[128]: TEX0 of each particle texture
constexpr uint32_t colors = 0x00256C10;       // RGBA (0x80 = 1.0), indexed by vis.colorIndex
// D_0078F5A0 (particle.bss): float[5][16], the view matrix in the previous particleDraw(v). particleDraw copies view v's into
// each system's packet (fx + 0x2EC0) and at the end stores the current one (D_00256B90) there
constexpr uint32_t viewPrevMat = 0x0078F5A0;
}

// Offsets inside _ParticleType (include/hieri_types.h)
namespace fxoff {
constexpr uint32_t uvI = 0x2B40;          // u8[256]
constexpr uint32_t emitVec = 0x2CA0;      // emit.ParticleToEmitVec
constexpr uint32_t fxId = 0x2DDC;         // emit.fxId (looks like the particleInitFx preset index; not confirmed in a dump)
constexpr uint32_t infoAttribs = 0x2E30;  // info.attribs (0x400 = packet built this frame)
constexpr uint32_t minSize = 0x2E38;      // info.minSize (pedestrians: stamp width)
constexpr uint32_t maxSize = 0x2E3C;      // info.maxSize (pedestrians: stamp height)
constexpr uint32_t expansionRate = 0x2E44; // info.expansionRate
constexpr uint32_t lifetime = 0x2E4A;     // info.lifetime (s16, fields)
constexpr uint32_t useThisColor = 0x2E58; // info.useThisColor: vector (R, G, B 0..255; w = alpha scale) with attribs bit 0x200
constexpr uint32_t alphaFadeIn = 0x2E60;  // info.alphaFadeIn (dustTrail: fraction of the lifetime)
constexpr uint32_t alphaFadeOut = 0x2E64;
constexpr uint32_t minAlpha = 0x2E68;
constexpr uint32_t maxAlpha = 0x2E6C;
constexpr uint32_t preAgeFactor = 0x2D88; // emit.info.preAgeFactor
constexpr uint32_t mapsPerFrame = 0x2E74; // info.mapsPerFrame (4.12)
constexpr uint32_t colorsPerFrame = 0x2E78;
constexpr uint32_t type = 0x2E90;         // vis.attribs
constexpr uint32_t texMask = 0x2E94;      // vis.texMask (number of texSprite frames)
constexpr uint32_t texInfoIndex = 0x2E98;
constexpr uint32_t numColors = 0x2E9C;
constexpr uint32_t colorIndex = 0x2EA0;
constexpr uint32_t priority = 0x2EA4;
constexpr uint32_t alphaContext = 0x2EA8;
constexpr uint32_t pos = 0x2EB0;          // FVECTOR*
constexpr uint32_t vel = 0x2EB4;          // FVECTOR*
constexpr uint32_t age = 0x2EB8;          // int16*
constexpr uint32_t numParticles = 0x2ED0;
constexpr uint32_t maxParticles = 0x2ED4;
}

enum FxType : uint32_t {
    FX_LINE = 1, FX_SWEEPTRAIL = 2, FX_SPRITE = 3, FX_TEXSPRITE = 4, FX_STRIPE = 5, FX_STRITE = 6, FX_ARC = 7, FX_SPRINE = 8, FX_KITE = 9,
    FX_CHAIN = 12, FX_FRAG = 13, FX_PEDS = 14, FX_DUSTTRAIL = 15, FX_WAKE = 16, FX_ARC2 = 17,
};

inline const char *fxTypeName(uint32_t t) {
    switch (t) {
    case FX_LINE: return "line";
    case FX_SWEEPTRAIL: return "sweepTrail";
    case FX_SPRITE: return "sprite";
    case FX_TEXSPRITE: return "texSprite";
    case FX_STRIPE: return "stripe";
    case FX_STRITE: return "strite";
    case FX_SPRINE: return "sprine";
    case FX_KITE: return "kite";
    case FX_FRAG: return "frag";
    case FX_DUSTTRAIL: return "dustTrail";
    case FX_WAKE: return "wake";
    case FX_ARC: return "arc";
    case FX_ARC2: return "arc17";
    case FX_CHAIN: return "chain";
    case FX_PEDS: return "pedestrian";
    default: return "?";
    }
}

// A particle system ready to draw
struct FxSystem {
    uint32_t base = 0;     // address of the _ParticleType
    uint32_t index = 0;    // index into fx[]
    uint32_t type = 0;
    int n = 0;             // numParticles
    float key = 0;         // draw order (larger = farther = earlier)
};

// Camera axes for building the quads: right/up in the screen plane, back = -forward, depth(p) = depth . (p, 1)
// ox/oy: view x and y coordinates (ox . (p, 1)), in the same basis as right/up.
// Fog (Particles::setFog): W(p) = fogW . (p, 1) (row 3 of worldToScreenMat, with the eye), F = fogFA + fogFB / W; fogFB 0 = no fog.
struct FxView { float right[3], up[3], back[3], depth[4], ox[4], oy[4]; float fogW[4] = {0, 0, 0, 0}, fogFA = 0.f, fogFB = 0.f; };

class Particles {
public:
    // texture(texId, tex0) -> raylib texture id (0 = no texture); provided by Scene (the .RTX palettes)
    using TexFn = std::function<unsigned(uint32_t texId, uint64_t tex0)>;

    std::vector<FxSystem> systems;
    float wide = 1.f;   // screen width / 4:3 width (Scene::wide): view test with the widescreen side planes
    bool fovCull = true;     // collect culls by the game's view (inViewFov); false = paused free camera
    bool frameGate = true;   // collect yields no particles on a frame without particleDraw (flareSlot); false = re-collect for the paused free camera
    // Sprites the paused game doesn't have (photo mode, wotm_photo.inc: Agamo's fire simulated with wotm_flame.hpp when the emitter
    // was off screen at pause time). Drawn like a sprite system (type 3): half-side size / 2 capped at 1/4 of the depth,
    // RGBA color with 0x80 = 1, UV mirrored by sel (the 2 low bits of pos.w), texInfo texture, alphaContext blending.
    struct SynthSprite { float p[3]; float size; uint32_t rgba; uint8_t sel; };
    struct SynthSystem { uint32_t texInfo = 0, alphaContext = 0; std::vector<SynthSprite> sprites; };
    std::vector<SynthSystem> synth;

    // Composite dust/smoke texture: TEX0 with TBP 0x3F60 (VRAM 0xFD800, CT32 64x64) and no pixels in RAM (texId 0xFFFF; the game
    // composes it in VRAM, Scene::dustTexture). TexFn receives it with texId = kDustTexId. Four 32x32 cells; the sprite (VisualInfo
    // 105) and the dustTrail (1, 2 and 118) upload the cells' UV table; the sprite picks the entry by the low bits of pos.w and the
    // dustTrail by byte 2 of the age word (dustTrailVu).
    static constexpr uint32_t kDustTexId = 0xFFFF;
    static bool isDustTex(uint64_t tex0) { return (tex0 & 0x3FFF) == 0x3F60; }
    static uint64_t systemTex0(const Ram &ram, uint32_t b, uint32_t frame = 0) {
        const uint32_t k = (ram.u16(fxaddr::textureInfo + 8 * (ram.u32(b + fxoff::texInfoIndex) & 127) + 2) + frame) & 127;
        return uint64_t(ram.u32(fxaddr::tex0 + 8 * k)) | (uint64_t(ram.u32(fxaddr::tex0 + 8 * k + 4)) << 32);
    }

    // particleDraw's view test: viewInFOV(0, &FOVsphere (+0x2C80), &FOVextent (+0x2C90)), the box (center, half-extent) against
    // the 6 planes of worldCtx[0] (fovNormsTP at +0xF0, fovSigns at +0x170; src/common/view.cpp). False = retail doesn't draw the system.
    // wide > 1 (widescreen: screen width / 4:3 width): the side planes (planes[0][1] and [0][2] of viewSetFov, the +/- w/2 ones)
    // come from worldToScreenMat with the edge at |X - 2048| = 320 * wide GS pixels; near, top, bottom and far stay the game's.
    static bool inViewFov(const Ram &ram, uint32_t b, float wide = 1.f) {
        float c[3], e[3];
        for (int k = 0; k < 3; ++k) { c[k] = ram.f32(b + 0x2C80 + 4 * k); e[k] = ram.f32(b + 0x2C90 + 4 * k); }
        return inViewVolume(ram, c, e, 0.f, wide);
    }

    // Box (center c, half-extent e) plus a sphere of radius r around it, in world space, against the 6 planes of worldCtx[0]: viewInFOV
    // (r = 0) and VU0 program 0x0 that hierTraverseAsm runs on each GROUP and OBJECT (e = 0; vf30 = fovNorms . (center + eo) and
    // vf29 = fovNorms1 . (center + eo) against -radius, plane 1.z with the farClip from VU0 memory 0, like fovNormsTP[1][3][2]).
    // Unit planes; the term sum |n_k| e_k is the game's fovSigns.
    static bool inViewVolume(const Ram &ram, const float c[3], const float e[3], float r, float wide = 1.f) {
        const uint32_t w = viewCtxAddr();   // worldCtx[view]
        const bool ws = wide > 1.001f;
        for (uint32_t i = 0; i < 2; ++i)
            for (uint32_t j = 0; j < 3; ++j) {
                if (ws && i == 0 && j > 0) continue;
                const uint32_t n = w + 0xF0 + 64 * i + 16 * j;
                float d = r;
                for (uint32_t k = 0; k < 3; ++k) d += ram.f32(n + 4 * k) * c[k] + std::fabs(ram.f32(n + 4 * k)) * e[k];
                if (d < ram.f32(w + 0xF0 + 64 * i + 48 + 4 * j)) return false;
            }
        if (!ws) return true;
        // X = (M0 . d) / (M3 . d), d = (p - eye, 1). Inside the right edge: X - 2048 <= h  <=>  (h M3 - (M0 - 2048 M3)) . d >= 0;
        // left with the sign flipped. For the box, the most favorable corner: n . (c - eye) + n3 + sum |n_k| e_k >= 0.
        const uint32_t kW2S = viewW2S();
        float M0[4], M3[4], eye[3];
        for (int k = 0; k < 4; ++k) { M0[k] = ram.f32(kW2S + 4 * k); M3[k] = ram.f32(kW2S + 48 + 4 * k); }
        for (int k = 0; k < 3; ++k) eye[k] = -ram.f32(w + 0x20 + 4 * k);   // worldCtx[0].eo = -eye
        const float h = 0.5f * curView().w * wide, xc = curView().cx;   // half-width and center of the view in GS space
        for (float sgn : {1.f, -1.f}) {
            float n[4];
            for (int k = 0; k < 4; ++k) n[k] = h * M3[k] - sgn * (M0[k] - xc * M3[k]);
            float d = n[3] + r * std::sqrt(n[0] * n[0] + n[1] * n[1] + n[2] * n[2]);
            for (int k = 0; k < 3; ++k) d += n[k] * (c[k] - eye[k]) + std::fabs(n[k]) * e[k];
            if (!(d >= 0.f)) return false;
        }
        return true;
    }

    // Focal lengths of worldToScreenMat M (GS pixels per view unit at W = 1): the part of rows 0 and 1 perpendicular to row 3 (forward).
    // 384 and 180.48 in a fight.
    static bool screenFoci(const float M[4][4], float &fx, float &fy) {
        float n3 = 0.f, d03 = 0.f, d13 = 0.f;
        for (int k = 0; k < 3; ++k) { n3 += M[3][k] * M[3][k]; d03 += M[0][k] * M[3][k]; d13 += M[1][k] * M[3][k]; }
        if (!(n3 > 1e-6f)) return false;
        fx = 0.f; fy = 0.f;
        for (int k = 0; k < 3; ++k) {
            const float a0 = M[0][k] - d03 / n3 * M[3][k], a1 = M[1][k] - d13 / n3 * M[3][k];
            fx += a0 * a0; fy += a1 * a1;
        }
        fx = std::sqrt(fx); fy = std::sqrt(fy);
        return fx > 1e-6f && fy > 1e-6f;
    }

    // The packet's prevViewMat (fx + 0x2EC0: x/y/z/translation columns in world space) for the tail of lines and strites. particleDraw only
    // writes it (the view matrix, right before the packet) for systems that pass the 4:3 view test; the native side in widescreen also draws
    // the ones that are only in the side strip, and for those the matrix is from an old frame or linePacketInit garbage ((63, 0, 0, 0) on a
    // new system): the tail became a streak thousands of pixels long (yellow streaks on 3-MILE, photo 2026-10-18_232542/001 and the dump of
    // 2026-10-19). Outside the 4:3 view the tail uses the current view (no camera blur) and wind is left out (linePacketBuild
    // didn't run either). False if the matrix read is all zeros.
    // Split screen: there is a single packet per system and view 1's particleDraw writes its matrix over view 0's (the lines of
    // Robo-47's shot streaked across the left view, photo 2026-10-19_130401). There the matrix comes from fxaddr::viewPrevMat[view], which
    // the capture (shadowRam) fills with each view's previous-frame matrix.
    static bool prevViewMat(const Ram &ram, uint32_t b, const float M[4][4], const float eye[3], float P[16], float wind[3]) {
        const uint32_t pm = ram.u32(b + 0x2EC0);
        if (!inViewFov(ram, b)) {
            for (int r = 0; r < 4; ++r) {
                P[r] = M[r][0]; P[4 + r] = M[r][1]; P[8 + r] = M[r][2];
                P[12 + r] = M[r][3] - (M[r][0] * eye[0] + M[r][1] * eye[1] + M[r][2] * eye[2]);
            }
            if (wind) wind[0] = wind[1] = wind[2] = 0.f;
            return true;
        }
        if (!ram.ok(pm - 16, 80)) return false;
        bool any = false;
        if (curView().shared() || curView().view > 0) {   // the bottom strip with 3 players (view 2) is 640 wide
            const uint32_t vm = fxaddr::viewPrevMat + 64u * uint32_t(std::clamp(curView().view, 0, 3));
            for (int k = 0; k < 16; ++k) { P[k] = ram.f32(vm + 4 * uint32_t(k)); any = any || P[k] != 0.f; }
        }
        if (!any)
            for (int k = 0; k < 16; ++k) { P[k] = ram.f32(pm + 4 * uint32_t(k)); any = any || P[k] != 0.f; }
        if (wind) for (int a = 0; a < 3; ++a) wind[a] = ram.f32(pm - 16 + 4 * uint32_t(a));
        return any;
    }

    // Strite like the VU1 microprogram (entry 0xA88; verified by the check, error < 0.1 px). In GS pixels:
    //   H = M (pos) with the current worldToScreenMat; T = M' (pos - vel) with prevViewMat (fx + 0x2EC0: 4 qwords in the packet, columns
    //   x/y/z/translation in world coordinates): the tail lands where it was on the previous frame's screen (motion blur).
    //   A = 3T - 2H (tip), B = 3H - 2T (base), each divided by its own w. Direction m = (B - A) normalized in the space divided by the focal lengths
    //   (the matrix's fx, fy: 384 and 180.48 in a fight); the tip pulls back 0.5 pos.w, the base moves forward 0.25 pos.w and the half-width is 0.433 pos.w
    //   (0.866 / 2), in view units: pixel = focal * offset * pos.w / w.
    // The three points go back to world space through the current matrix, each at its own depth. c[0] = c[3] = tip, c[1]/c[2] = base.
    // alphaK: the microprogram multiplies the color's alpha by K / (len + K) (0x0DC0-0x0E30: ESQRT, DIV Q = vf24w / vf10x, MULq.w), with
    // len = |B/w - A/w| on screen (Y scaled by fx/fy, vf24z) and K = 4 vf27x pos.w / B.w = 2 fx pos.w / B.w (vf27x = fx / 2; measured against
    // VU1 in photo 2026-10-17_200206): a long trail gets fainter.
    static bool striteTriangle(const Ram &ram, uint32_t b, const float p[4], const float v[3], float c[4][3], float *alphaK = nullptr) {
        const uint32_t kW2S = viewW2S(), kEo = viewCtxAddr() + 0x20;   // worldToScreenMat[view], worldCtx[view].eo (= -eye)
        float M[4][4], eye[3];
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k) M[r][k] = ram.f32(kW2S + 16 * r + 4 * k);
        for (int k = 0; k < 3; ++k) eye[k] = -ram.f32(kEo + 4 * k);
        float P[16];
        if (!prevViewMat(ram, b, M, eye, P, nullptr)) return false;
        float H[4], T[4];
        const float d[3] = {p[0] - eye[0], p[1] - eye[1], p[2] - eye[2]}, t[3] = {p[0] - v[0], p[1] - v[1], p[2] - v[2]};
        for (int r = 0; r < 4; ++r) {
            H[r] = M[r][0] * d[0] + M[r][1] * d[1] + M[r][2] * d[2] + M[r][3];
            T[r] = P[12 + r] + P[r] * t[0] + P[4 + r] * t[1] + P[8 + r] * t[2];
        }
        float A[4], B[4];
        for (int r = 0; r < 4; ++r) { A[r] = 3.f * T[r] - 2.f * H[r]; B[r] = 3.f * H[r] - 2.f * T[r]; }
        if (!(A[3] > 1e-3f) || !(B[3] > 1e-3f)) return false;
        float fx, fy;
        if (!screenFoci(M, fx, fy)) return false;
        const float ax = A[0] / A[3], ay = A[1] / A[3], bx = B[0] / B[3], by = B[1] / B[3];
        float mx = (bx - ax) / fx, my = (by - ay) / fy;
        const float ml = std::sqrt(mx * mx + my * my);
        if (!(ml > 1e-9f)) return false;
        if (alphaK) {
            const float len = ml * fx, k = 2.f * fx * p[3] / B[3];
            *alphaK = len + k > 0.f ? k / (len + k) : 1.f;
        }
        mx /= ml; my /= ml;
        const float w = p[3], ka = 0.5f * w / A[3], kb = w / B[3];
        const float px[3] = {ax - ka * mx * fx, bx + kb * (0.25f * mx * fx - 0.4330127f * my * fx), bx + kb * (0.25f * mx * fx + 0.4330127f * my * fx)};
        const float py[3] = {ay - ka * my * fy, by + kb * (0.25f * my * fy + 0.4330127f * mx * fy), by + kb * (0.25f * my * fy - 0.4330127f * mx * fy)};
        const float pw[3] = {A[3], B[3], B[3]};
        // back to world space: rows 0, 1 and 3 of M applied to (q - eye) give (x w, y w, w)
        const float m00 = M[0][0], m01 = M[0][1], m02 = M[0][2], m10 = M[1][0], m11 = M[1][1], m12 = M[1][2], m30 = M[3][0], m31 = M[3][1], m32 = M[3][2];
        const float det = m00 * (m11 * m32 - m12 * m31) - m01 * (m10 * m32 - m12 * m30) + m02 * (m10 * m31 - m11 * m30);
        if (!(std::fabs(det) > 1e-12f)) return false;
        for (int k = 0; k < 3; ++k) {
            const float r0 = px[k] * pw[k] - M[0][3], r1 = py[k] * pw[k] - M[1][3], r3 = pw[k] - M[3][3];
            const float q0 = (r0 * (m11 * m32 - m12 * m31) - m01 * (r1 * m32 - m12 * r3) + m02 * (r1 * m31 - m11 * r3)) / det;
            const float q1 = (m00 * (r1 * m32 - m12 * r3) - r0 * (m10 * m32 - m12 * m30) + m02 * (m10 * r3 - r1 * m30)) / det;
            const float q2 = (m00 * (m11 * r3 - r1 * m31) - m01 * (m10 * r3 - r1 * m30) + r0 * (m10 * m31 - m11 * m30)) / det;
            const int dst = k == 0 ? 0 : k;
            c[dst][0] = eye[0] + q0; c[dst][1] = eye[1] + q1; c[dst][2] = eye[2] + q2;
        }
        for (int a = 0; a < 3; ++a) c[3][a] = c[0][a];
        return true;
    }

    // Line (FX_LINE) like the VU1 microprogram (MPG 0x0000; linePacketInit/Build in asm/common/partdraw.s):
    //  - head = pos with the current camera; tail = M' (pos - vel - wind), M' = prevViewMat (fx + 0x2EC0, 4 qwords of the packet, columns
    //    x/y/z/translation) and wind = q537 (right before it: field.wind.xyz + Gusts * field.wind.w, written by linePacketBuild). The tail
    //    goes back to world space through the current matrix, at its own depth (like striteTriangle): it lands where the tail was on the previous frame's screen;
    //  - color = table lerp at vel.w * (numColors - 1) / lifetime, + 0.5, truncated (not by age);
    //  - alpha = color alpha * pos.w * (fogFA + fogFB / W) / 255, capped at 255: fog goes into alpha (PRIM 0x49 has no FGE). Disappears with
    //    alpha < 1. The tail carries the RGB with alpha 0.
    // Verified by the check (ultrav_corda2: tail within 0.05 px of VU1 in the offline fit).
    static bool lineVu(const Ram &ram, uint32_t b, const FxView &view, int i, float head[3], float tail[3], uint32_t &col) {
        const uint32_t kW2S = viewW2S(), kEo = viewCtxAddr() + 0x20;
        const uint32_t pos = ram.u32(b + fxoff::pos), vel = ram.u32(b + fxoff::vel);
        if (!ram.ok(pos + 16 * uint32_t(i), 16) || !ram.ok(vel + 16 * uint32_t(i), 16)) return false;
        float p[4], v[4], wind[3], P[16], M[4][4], eye[3];
        for (int a = 0; a < 4; ++a) { p[a] = ram.f32(pos + 16 * uint32_t(i) + 4 * uint32_t(a)); v[a] = ram.f32(vel + 16 * uint32_t(i) + 4 * uint32_t(a)); }
        if (!std::isfinite(p[0]) || !std::isfinite(v[0])) return false;
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k) M[r][k] = ram.f32(kW2S + 16 * r + 4 * k);
        for (int k = 0; k < 3; ++k) eye[k] = -ram.f32(kEo + 4 * k);
        if (!prevViewMat(ram, b, M, eye, P, wind)) return false;
        // color: q520 + index (up to 16 entries; the ones past numColors have alpha 0)
        const uint32_t nc = ram.u32(b + fxoff::numColors), tab = fxaddr::colors + 4 * ram.u32(b + fxoff::colorIndex);
        const int life = int(ram.s16(b + fxoff::lifetime));
        const float rate = life != 0 && nc > 0 ? float(int(nc) - 1) / float(life) : 0.f;
        const float cp = v[3] * rate;
        if (!std::isfinite(cp)) return false;
        int k0 = int(cp);   // FTOI0: truncates
        const float fr = cp - float(k0);
        const int nEnt = int(std::min<uint32_t>(nc + 1, 16));
        auto entry = [&](int k, int sh) -> float {
            if (k < 0 || k >= nEnt) return 0.f;
            if (sh == 24 && k >= int(nc)) return 0.f;
            return float((ram.u32(tab + 4 * uint32_t(k)) >> sh) & 255);
        };
        float c[4];
        for (int a = 0; a < 4; ++a) c[a] = entry(k0, 8 * a) * (1.f - fr) + entry(k0 + 1, 8 * a) * fr + 0.5f;
        // depth of the head (W) for the fog in alpha
        float W = 0.f;
        for (int k = 0; k < 3; ++k) W += M[3][k] * (p[k] - eye[k]);
        W += M[3][3];
        if (!(W > 1e-3f)) return false;
        const float A = std::min(c[3] * p[3] * (view.fogFA + view.fogFB / W) / 255.f, 255.f);
        if (!(A >= 1.f)) return false;
        auto ub = [](float x) { return uint32_t(std::clamp(x, 0.f, 255.f)); };
        col = ub(c[0]) | ub(c[1]) << 8 | ub(c[2]) << 16 | ub(A) << 24;
        // tail: T = P (pos - vel - wind), back to world space through the current matrix at depth T.w
        const float t[3] = {p[0] - v[0] - wind[0], p[1] - v[1] - wind[1], p[2] - v[2] - wind[2]};
        float T[4];
        for (int r = 0; r < 4; ++r) T[r] = P[12 + r] + P[r] * t[0] + P[4 + r] * t[1] + P[8 + r] * t[2];
        if (!(T[3] > 1e-3f)) return false;
        const float m00 = M[0][0], m01 = M[0][1], m02 = M[0][2], m10 = M[1][0], m11 = M[1][1], m12 = M[1][2], m30 = M[3][0], m31 = M[3][1], m32 = M[3][2];
        const float det = m00 * (m11 * m32 - m12 * m31) - m01 * (m10 * m32 - m12 * m30) + m02 * (m10 * m31 - m11 * m30);
        if (!(std::fabs(det) > 1e-12f)) return false;
        const float r0 = T[0] - M[0][3], r1 = T[1] - M[1][3], r3 = T[3] - M[3][3];   // (x w, y w, w) = T.x, T.y, T.w
        const float q0 = (r0 * (m11 * m32 - m12 * m31) - m01 * (r1 * m32 - m12 * r3) + m02 * (r1 * m31 - m11 * r3)) / det;
        const float q1 = (m00 * (r1 * m32 - m12 * r3) - r0 * (m10 * m32 - m12 * m30) + m02 * (m10 * r3 - r1 * m30)) / det;
        const float q2 = (m00 * (m11 * r3 - r1 * m31) - m01 * (m10 * r3 - r1 * m30) + r0 * (m10 * m31 - m11 * m30)) / det;
        tail[0] = eye[0] + q0; tail[1] = eye[1] + q1; tail[2] = eye[2] + q2;
        for (int a = 0; a < 3; ++a) head[a] = p[a];
        return true;
    }

    // Gathers the drawable systems (same tests as particleDraw). The order is set in draw (current camera).
    void collect(const Ram &ram) {
        systems.clear();
        uint32_t i = ram.u32(fxaddr::fxActiveTop);
        for (uint32_t guard = 0; i < fxaddr::count && guard < fxaddr::count; ++guard) {
            const uint32_t b = fxaddr::fx + i * fxaddr::stride;
            const uint32_t next = ram.u32(fxaddr::fxList + 12 * i + 8);
            FxSystem s;
            s.base = b;
            s.index = i;
            s.type = ram.u32(b + fxoff::type);
            s.n = int32_t(ram.u32(b + fxoff::numParticles));
            const int maxN = int32_t(ram.u32(b + fxoff::maxParticles));
            const bool built = (ram.u32(b + fxoff::infoAttribs) & 0x400) != 0;
            const int minN = s.type == FX_SPRINE ? 2 : 1;
            // particleDraw only builds the packet (0x400) for systems in view. When re-collecting for the paused free camera (fovCull false)
            // sprites outside the paused view are included too (Agamo's fire behind the camera): they only read pos, age and colors, not the
            // packet; the types with a header in the packet (wake, arc, sprine...) still require it to be built
            const bool plain = s.type == FX_SPRITE || s.type == FX_TEXSPRITE;
            if ((built || (!fovCull && plain)) && s.type != 11 && s.type != FX_FRAG && s.n >= minN && s.n <= std::max(maxN, 1) && s.n <= 1024 && ram.ok(ram.u32(b + fxoff::pos), 16 * uint32_t(s.n)) &&
                (!fovCull || inViewFov(ram, b, wide)))
                systems.push_back(s);
            i = next;
        }
        {   // live, a frame without particleDraw (menu, load): no particles, as on the PS2, where the packet only comes out of particleDraw.
            // fx[] still holds the last fight's systems (fx 81/86/90/93 in the menu after the campaign: green quads, photo 007);
            // captureFlares (right after each particleDraw) marks the frame, as for the lens flare
            FlareSlot &s = flareSlot();
            std::lock_guard<std::mutex> g(s.m);
            // re-collecting for the paused free camera (wotm_live.inc) runs after the frame's consumeFlares: there the frozen RAM is the
            // frame with particleDraw, and the cut wiped every fx[] particle (pedestrians, Agamo's fire) only while paused
            if (frameGate && s.used && !s.fresh) systems.clear();
        }
        static const bool list = std::getenv("WOTM_FXLIST") != nullptr;
        if (list) dump(ram);
        collectFlares(ram);
    }

    // Lens flare (type 11): outside the ordinary particleDraw. Two type-11 fx[] (lensFlarePacket[view], 0x6F8908; particleInitAfter) whose
    // pos[] (fx + 0x2EB0) is the list HierLensFlareList[74] (48 bytes) of lensFlareList[view] (0x6F8900), with lensFlareIndex[view]
    // (0x6F88F8) entries. The list is fed by hierLightNode (lights with hasLensFlare: world pos, flags = the light's, intensity,
    // color, the beam's dotProd/beamWidth) and by particleAddLensFlareToList (systems with vis.lightColor & 0x1000000: flags 3, Zfudge,
    // intensity = lightIntensity^2, color = lightColor / 255).
    //  - particleLensFlareTestZbuffer (at the end of particleDraw, after VU1 draws the scene): projects each entry, drops the ones off
    //    screen or with W < 1 (compacting the list), writes pos = (GS X, Y, Z), distance = W (100 with lightAtInfinity, flags bit 7) and reads
    //    the 4x4 Z from the GS at (X - 2, Y) (sceGsStoreImage PSMZ32; origin X - 0x6C2, Y - 0x790).
    //  - particleLensFlareRender: visible sample = Z <= threshold. Light at infinity: all 16, threshold fptoui(1.03125 Z) + 1; local: the 4
    //    central ones, threshold fptoui(a + b / (W - max(Zfudge; 1.5))) (a, b = viewGetZBuffParams). intensity *= visible / samples; pos +=
    //    centroid of the visible ones - (2 or 1); with 0 < ID < 0x7FF (only lights at infinity: particleLensFlareAddToDeBounceList), intensity =
    //    average with the previous frame's (D_0078FF00); below 1/256 the flags byte is zeroed. Then lensFlarePacketBuild writes q44 =
    //    (center X, center Y, 18, n) and the UNPACK of 3n qwords of the list at q45.
    // In the runtime the emulated GS doesn't have the scene (no VU1); here the test is redone against the native depth (drawFlares), and the centroid
    // and fraction from the emulated GS are undone assuming full visibility there (see WOTM_FLARELOG to check).
    // Microprogram 0x2B20 (build/vu1/particle.txt), per entry (q45 + 3i):
    //  - only with flags & 1; Q = 1/distance; brightness = I Q Q (the second MULq reads the old Q: the DIV 1/beamWidth hasn't finished yet; with the
    //    new Q the particles, which have no beamWidth, would come out as garbage), size = sqrt(I) Q. With flags & 8 (directional): both times
    //    f / beamWidth, f = max(dotProd, 0) + beamWidth - 1 (|dotProd| with flags & 0x10, bidirectional); disappears if brightness f <= 0.
    //  - 18 SPRITEs (tables in the packet, q8-q25: TEX0, t, half-side with the UV selector in the 2 low bits; q26-q43: R, G, B, A). Sprite k
    //    at point C + t_k (S - C) on the line from the screen center C to the source S; corners +/- (h, h/2) GS px. k = 0: h = half-side size A,
    //    color min(255, RGB rgb). k > 0: h = half-side, color min(32, RGB rgb A brightness). Alpha 0x80, ALPHA 0x48 (additive), UV from (0.5; 0.5) at the
    //    - corner to uvMax at the + corner (uvMax = 31.5, 63.5 or 127.5 texels by the selector: 0x1F8, 0x3F8, 0x7F8 in 1/16). After a sprite
    //    with all three colors < 1, the entry drops out of the following ones.
    // Verified by the VU1 vs native check (entry read from VU1 memory at the MSCAL of 0x2B20): corners, color and UV of every sprite.
    struct FlareSprite { uint64_t tex0 = 0; uint32_t texId = 0; float t = 0.f, half = 0.f, uvMax = 0.f, rgba[4] = {0, 0, 0, 0}; };
    struct Flare { float x = 0, y = 0, z = 0, w = 0, zfudge = 0, dot = 0, bw = 0, rgb[3] = {0, 0, 0}, intensity = 0; uint32_t flags = 0, id = 0; };
    struct FlareFrame {
        float cx = 2048.f, cy = 2048.f, vw = 640.f, vh = 224.f, zScale = 0.f, zOff = 0.f;
        int nspr = 0;
        FlareSprite spr[18];
        std::vector<Flare> e;
    };
    FlareFrame flares;
    const AsyncDepth *depthAsync = nullptr;   // drawFlares: the view's asynchronous depth (split screen); nullptr = direct glReadPixels

    // The list is only ready (projected, with the visible fraction) between the end of particleDraw and the next frame's hier. Presentation
    // runs on another thread and almost always finds it already rebuilt by hier (in world coordinates, without the particle flares, which
    // particleAddLensFlareToList only adds inside particleDraw). The runtime calls captureFlares on the game thread right after
    // particleDraw; collect uses that copy. Without a capture (viewer over a dump) it reads RAM and only draws if the list is projected.
    // The copy is only valid for the particleDraw frame that made it (fresh; wotmCaptureFrame consumes it with consumeFlares). A frame without
    // particleDraw (menu, loading screen) has no flares, as on the PS2, where the packet only exists if particleDraw built it; previously
    // the last fight's list came back on the next LOAD screen (green/red light in the center when entering the match).
    // One copy per view (lensFlareIndex/lensFlarePacket[view & 1]; in split screen particleDraw(1) rebuilds view 1's list, and
    // with 3 and 4 players views 2 and 3's rebuilds views 0 and 1's). captureFlares runs inside the view's particleDraw (viewCurView),
    // before the next view reuses the list; collection takes the current view's (curView).
    struct FlareSlot { std::mutex m; FlareFrame f; bool used = false, fresh = false; };
    static FlareSlot &flareSlot(int view = curView().view) { static FlareSlot s[4]; return s[std::clamp(view, 0, 3)]; }
    static void captureFlares(const Ram &ram) {
        const int v = int(std::min<uint32_t>(ram.u32(wotm::sym::viewCurView), 3u));
        FlareFrame f;
        {
            ViewScope vs(readViewCtx(ram, v));
            readFlares(ram, f);
        }
        FlareSlot &s = flareSlot(v);
        std::lock_guard<std::mutex> g(s.m);
        s.f = std::move(f);
        s.used = true;
        s.fresh = true;
    }
    static void consumeFlares() {
        for (int v = 0; v < 4; ++v) {
            FlareSlot &s = flareSlot(v);
            std::lock_guard<std::mutex> g(s.m);
            s.fresh = false;
        }
    }
    void resetFlareDebounce() { flarePrev_.clear(); }

    // Shadow copy of what draw reads from RAM (docs/investigacoes/2026-10-17/09_magmo_impacto.md). The snapshot only stores each system's
    // count; positions, ages and packet headers used to be read from live RAM during presentation, and if the next frame's particleMove
    // had already killed particles, pos[n] was the packet terminator (0, 0, 0, huge w): a giant quad at the map origin.
    // Called only on the EE thread (wotmCaptureFrame), with freshly collected systems. Returns a RAM-sized buffer with the
    // ranges copied (the rest holds garbage from earlier uses, which draw never reads); nullptr = off (WOTM_FXLIVE=1).
    // prevMats (split screen): float[4][16], each view's matrix (0 to 3) on the previous frame, written to fxaddr::viewPrevMat
    // (prevViewMat); without it the range stays zeroed and the packet's is used
    static std::shared_ptr<const uint8_t> shadowRam(const Ram &ram, const std::vector<FxSystem> &systems, const float *prevMats = nullptr) {
        static const bool live = std::getenv("WOTM_FXLIVE") != nullptr;   // diagnostic: reads live RAM, the old way
        if (live || !ram.p || systems.empty()) return nullptr;
        // pool: a free buffer = only the pool holds it (the published snapshot, the presentation's and Scene's hold the others)
        static std::vector<std::shared_ptr<uint8_t>> pool;
        std::shared_ptr<uint8_t> buf;
        for (auto &p : pool)
            if (p.use_count() == 1) { buf = p; break; }
        if (!buf) {
            if (pool.size() >= 6) return nullptr;
            pool.emplace_back(new uint8_t[ram.size], std::default_delete<uint8_t[]>());
            buf = pool.back();
        }
        uint8_t *dst = buf.get();
        copyRanges(dst, ram, systems);
        if (prevMats) std::memcpy(dst + fxaddr::viewPrevMat, prevMats, 4 * 64);
        else std::memset(dst + fxaddr::viewPrevMat, 0, 4 * 64);
        return std::shared_ptr<const uint8_t>(buf, buf.get());
    }

    // The ranges draw reads (shadowRam), copied from src (live RAM or another shadow copy) into dst, of the same size
    static void copyRanges(uint8_t *dst, const Ram &ram, const std::vector<FxSystem> &systems) {
        auto copy = [&](uint32_t a, uint32_t n) {
            if (a < 0x1000 || a >= ram.size) return;
            n = uint32_t(std::min<uint64_t>(n, ram.size - a));
            std::memcpy(dst + a, ram.p + a, n);
        };
        copy(fxaddr::fx, fxaddr::count * fxaddr::stride);   // pos/vel/age and packets (wake, arc, sprine headers) live inside fx[i]
        copy(fxaddr::textureInfo, 128 * 8);
        copy(fxaddr::texId, 128 * 2);
        copy(fxaddr::tex0, 128 * 8);
        copy(addr::worldToScreenMat, 5 * 0x40);
        copy(addr::worldCtx, 5 * 0x1F0);
        copy(addr::vu1FogParams, 16);
        copy(fxaddr::viewPrevMat, 4 * 64);
        copy(wotm::sym::Gusts, 16);   // wind gusts (stepBack)
        const uint32_t lo = fxaddr::fx, hi = fxaddr::fx + fxaddr::count * fxaddr::stride;
        auto outside = [&](uint32_t a, uint32_t n) { return a < lo || uint64_t(a) + n > hi; };
        for (const FxSystem &s : systems) {
            const uint32_t b = s.base, n = uint32_t(std::max(s.n, 0)) + 1;
            copy(fxaddr::colors + 4 * ram.u32(b + fxoff::colorIndex), 4 * std::min<uint32_t>(ram.u32(b + fxoff::numColors), 256));
            // info.useThisColor (bit 0x200: dust trail, frag) points outside fx[] (the stage's trailColoring, 0x5B4F60 on VEGAS);
            // without the copy the stripe read garbage/zeros live (black color, alpha 0) and the trail vanished, although the replay (whole RAM) drew it
            if (ram.u32(b + fxoff::infoAttribs) & 0x200) copy(ram.u32(b + fxoff::useThisColor), 16);
            // pointers that point outside fx[] (never seen so far): the range draw reads around them
            const uint32_t pos = ram.u32(b + fxoff::pos), vel = ram.u32(b + fxoff::vel), age = ram.u32(b + fxoff::age), pm = ram.u32(b + 0x2EC0);
            if (outside(pos - 0x400, 0x400 + 16 * n)) copy(pos - 0x400, 0x400 + 16 * n);
            if (outside(vel, 16 * n)) copy(vel, 16 * n);
            if (outside(age, 4 * n)) copy(age, 4 * n);
            if (pm && outside(pm, 64)) copy(pm, 64);
        }
    }

    // One particleMove step in reverse (frame interpolation, Scene::showBlend). particleMove does, with f = the step's fields,
    // pos_B = pos_A + v f + wind f, and only afterwards adds gravity to the velocity (v_B = v + (0, 0, gravity f)); drag is already in
    // v_B. So pos_A = pos_B - f (v_B + wind - (0, 0, gravity f)). Per system: wind per field (field.wind; with bit 0x400 of
    // field.attribs, xyz + Gusts xyz * wind.w, else only z), gravity (field.gravity; dust, wake and kite move through dustMove and
    // wakeAndKiteMove, without it), how many moved (the first n - newParticles: the last ones were born this step and stay put) and z
    // pinned to the water level (wake and kite with bit 0x8000000 of emit.info.attribs). False = the system doesn't move this way: arcs, sprine,
    // chain and trail (points placed by the game) and field.attribs 0x1000 (stationary). A moving emitter (fire attached to the monster) is not
    // accounted for: the particle only steps back by its own velocity.
    struct StepBack { float wind[3] = {0, 0, 0}; float grav = 0.f; int moved = 0; bool keepZ = false; };
    static bool stepBack(const Ram &ram, uint32_t b, int n, StepBack &o) {
        constexpr uint32_t kField = 0x2DE0, kEmitAttribs = 0x2D80, kNewParticles = 0x2EDE;
        const uint32_t type = ram.u32(b + fxoff::type), fa = ram.u32(b + kField);
        if (type == FX_SWEEPTRAIL || type == FX_ARC || type == FX_ARC2 || type == FX_SPRINE || type == FX_CHAIN || (fa & 0x1000)) return false;
        const float ww = ram.f32(b + kField + 0x1C);
        o.wind[0] = o.wind[1] = 0.f;
        o.wind[2] = ram.f32(b + kField + 0x18);
        if (fa & 0x400)
            for (int k = 0; k < 3; ++k) o.wind[k] = ram.f32(b + kField + 0x10 + 4 * uint32_t(k)) + ram.f32(wotm::sym::Gusts + 4 * uint32_t(k)) * ww;
        const bool own = type == FX_DUSTTRAIL || type == FX_WAKE || type == FX_KITE;
        o.grav = own ? 0.f : ram.f32(b + kField + 4);
        o.keepZ = (type == FX_WAKE || type == FX_KITE) && (ram.u32(b + kEmitAttribs) & 0x8000000);
        o.moved = std::clamp(n - int(ram.s16(b + kNewParticles)), 0, n);
        for (float v : {o.wind[0], o.wind[1], o.wind[2], o.grav})
            if (!std::isfinite(v)) return false;
        return true;
    }
    // Step displacement (pos_B - pos_A) of a particle with velocity v
    static void stepOffset(const StepBack &o, const float v[3], float f, float out[3]) {
        for (int k = 0; k < 3; ++k) out[k] = f * (v[k] + o.wind[k]);
        out[2] -= o.grav * f * f;
        if (o.keepZ) out[2] = 0.f;
    }

    // Shadow copy at the instant between A and B: B's ranges (copyRanges) with positions pulled back by back (1 - alpha) steps of
    // `fields`. xyz only: pos.w (size, with the low bits that pick the UV; age for dust and wake) and age stay as in B.
    // A pull-back larger than 400 units (garbage velocity) stays at B.
    static void blendRam(uint8_t *dst, const Ram &B, const std::vector<FxSystem> &systems, float back, float fields) {
        copyRanges(dst, B, systems);
        const Ram out{dst, B.size};
        for (const FxSystem &s : systems) {
            StepBack o;
            if (!stepBack(out, s.base, s.n, o) || !o.moved) continue;
            const uint32_t pos = out.u32(s.base + fxoff::pos), vel = out.u32(s.base + fxoff::vel);
            if (!out.ok(pos, 16 * uint32_t(o.moved)) || !out.ok(vel, 16 * uint32_t(o.moved))) continue;
            for (int i = 0; i < o.moved; ++i) {
                float v[3], d[3];
                std::memcpy(v, dst + vel + 16 * uint32_t(i), 12);
                stepOffset(o, v, fields, d);
                if (!std::isfinite(d[0] + d[1] + d[2]) || d[0] * d[0] + d[1] * d[1] + d[2] * d[2] > 400.f * 400.f) continue;
                float *p = reinterpret_cast<float *>(dst + pos + 16 * uint32_t(i));
                for (int k = 0; k < 3; ++k) p[k] -= back * d[k];
            }
        }
    }

    void collectFlares(const Ram &ram) {
        {
            FlareSlot &s = flareSlot();
            std::lock_guard<std::mutex> g(s.m);
            if (s.used) {   // live: no particleDraw this frame, no flares
                if (s.fresh) flares = s.f;
                else { flares.e.clear(); flares.nspr = 0; }
                logFlares();
                return;
            }
        }
        readFlares(ram, flares);
        logFlares();
    }
    void logFlares() const {
        static const bool log = std::getenv("WOTM_FLARELOG") != nullptr;
        if (log)
            for (const Flare &fl : flares.e)
                std::fprintf(stderr, "[flare] n %zu center %.0f %.0f | %.1f %.1f Z %.0f W %.1f id %u flags %02x dot %.3f bw %.3f rgb %.3f %.3f %.3f I %.4g\n",
                             flares.e.size(), flares.cx, flares.cy, fl.x, fl.y, fl.z, fl.w, fl.id, fl.flags & 0xFF, fl.dot, fl.bw, fl.rgb[0], fl.rgb[1],
                             fl.rgb[2], fl.intensity);
    }

    static void readFlares(const Ram &ram, FlareFrame &f) {
        const uint32_t view = uint32_t(std::clamp(curView().view, 0, 3)), v = view & 1u;   // lensFlareIndex/lensFlarePacket have 2 entries
        const uint32_t kIndex = wotm::sym::lensFlareIndex + 4 * v, kPacket = wotm::sym::lensFlarePacket + 4 * v, kViewDef = viewInfoAddr() + 4,
                       kScreenMats = wotm::sym::viewScreenMats + 0x40 * view;
        f.e.clear();
        f.nspr = 0;
        const uint32_t p = ram.u32(kPacket);
        if (!ram.ok(p, fxaddr::stride)) return;
        // q44 (center X, center Y, 18, n) is what lensFlarePacketBuild writes every frame: here it comes from the origin (viewGetCenter of view
        // 0, lensFlareIndex[0]), independent of the frame's packet; the sprite tables (q8-q43, 18 at q44 + 8) come from lensFlarePacketInit.
        const uint32_t pos = ram.u32(p + fxoff::pos), q44 = pos - 0x18, q8 = q44 - 36 * 16;
        const int n = int32_t(ram.u32(kIndex));
        const uint32_t def = ram.u32(kViewDef) & 0x0FFFFFFF;
        // n = 0: only the sprite table and the frame (photo mode rebuilds the list with the free camera)
        if (n < 0 || n > 74 || !ram.ok(q8, 37 * 16 + 48 * uint32_t(n)) || !ram.ok(def, 0x20)) return;
        f.cx = float(int32_t(ram.u32(def + 0x18)));
        f.cy = float(int32_t(ram.u32(def + 0x1C)));
        f.nspr = std::min<int>(18, std::max<int>(0, int32_t(ram.u32(q44 + 8))));
        if (f.nspr == 0) f.nspr = 18;   // lensFlarePacketBuild hasn't run yet (stage without a flare so far): the Init table is already there
        if (ram.u16(def + 4) && ram.u16(def + 6)) { f.vw = ram.u16(def + 4); f.vh = ram.u16(def + 6); }
        f.zScale = ram.f32(kScreenMats + 0x28);   // viewScreenMats[0][2][2] and [2][3]
        f.zOff = ram.f32(kScreenMats + 0x2C);
        const uint32_t gif = (p + 0xC + 15) & ~15u;   // lensFlarePacketInit: FLUSHA, UNPACK of 4 qwords at 0x110 (tag A+D, ALPHA, TEX0, tag SPRITE)
        for (int k = 0; k < f.nspr; ++k) {
            FlareSprite &s = f.spr[k];
            const uint32_t a = q8 + 16 * uint32_t(k), c = a + 18 * 16;
            s.tex0 = uint64_t(ram.u32(a)) | (uint64_t(ram.u32(a + 4)) << 32);
            s.t = ram.f32(a + 8);
            s.half = ram.f32(a + 12);
            // the UNPACK carries the 4 qwords starting at gif (= VU1 0x110, verified in the snapshot); one qword off and uvMax came from
            // the next record (127.5 on a 64 texture): the texture only covered half the sprite and the glow fell outside the eye
            s.uvMax = float(ram.u32(gif + 16 * (ram.u32(a + 12) & 3) + 12) & 0xFFFF) / 16.f;
            for (int j = 0; j < 4; ++j) s.rgba[j] = ram.f32(c + 4 * uint32_t(j));
            s.texId = ram.u16(p + 0x2C40 + 2 * uint32_t(k));
        }
        const float x0 = f.cx - 0.5f * f.vw, x1 = f.cx + 0.5f * f.vw, y0 = f.cy - 0.5f * f.vh, y1 = f.cy + 0.5f * f.vh;
        for (int i = 0; i < n; ++i) {
            const uint32_t e = pos + 48 * uint32_t(i);
            Flare fl;
            fl.x = ram.f32(e); fl.y = ram.f32(e + 4); fl.z = ram.f32(e + 8); fl.id = ram.u32(e + 0xC);
            fl.flags = ram.u32(e + 0x10); fl.zfudge = ram.f32(e + 0x10); fl.w = ram.f32(e + 0x14);
            fl.dot = ram.f32(e + 0x18); fl.bw = ram.f32(e + 0x1C);
            for (int j = 0; j < 3; ++j) fl.rgb[j] = ram.f32(e + 0x20 + 4 * uint32_t(j));
            fl.intensity = ram.f32(e + 0x2C);
            // off screen = list still in world coordinates (hier already rebuilt it and the Z test hasn't run): nothing to draw
            if (!(fl.x >= x0 - 3.f && fl.x <= x1 + 3.f && fl.y >= y0 - 3.f && fl.y <= y1 + 3.f)) { f.e.clear(); break; }
            f.e.push_back(fl);
        }
    }

    // An entry as VU1 receives it: position on the GS screen and intensity after the Z test
    struct FlareIn { float x, y, intensity; const Flare *fl; };
    // GS sprite (GS px): + corner (xa, ya) with UV (u0, v0), - corner (xb, yb) with (u1, v1); k = table row, entry = index into `in`
    struct FlareQuad { int k; size_t entry; float xa, ya, xb, yb, u0, v0, u1, v1; uint8_t rgb[3]; };

    // Microprogram 0x2B20 over the list (without the Z test): drawing and the VU1 vs native check use the same one
    template <class E> static void flareSprites(const FlareFrame &f, const std::vector<FlareIn> &in, E &&emit) {
        struct V { float bright, size; bool on; };
        std::vector<V> v(in.size());
        for (size_t i = 0; i < in.size(); ++i) {   // per-entry loop
            const Flare &fl = *in[i].fl;
            const float I = in[i].intensity;
            v[i].on = (fl.flags & 1) && fl.w != 0.f;
            if (!v[i].on) continue;
            const float q = 1.f / fl.w;
            v[i].bright = I * q * q;
            v[i].size = std::sqrt(std::max(I, 0.f)) * q;
            if (fl.flags & 8) {
                const float k = ((fl.flags & 0x10) ? std::fabs(fl.dot) : std::max(fl.dot, 0.f)) + fl.bw - 1.f;
                if (!(v[i].bright * k > 0.f) || fl.bw == 0.f) { v[i].on = false; continue; }
                v[i].bright = v[i].bright * k / fl.bw;
                v[i].size = v[i].size * k / fl.bw;
            }
        }
        auto ftoi4 = [](float a) { return std::trunc(a * 16.f) / 16.f; };
        for (int k = 0; k < f.nspr; ++k) {   // loop per table sprite, entries inside
            const FlareSprite &s = f.spr[k];
            const float tw = float(1u << ((s.tex0 >> 26) & 15)), th = float(1u << ((s.tex0 >> 30) & 15));
            for (size_t i = 0; i < in.size(); ++i) {
                if (!v[i].on) continue;
                const Flare &fl = *in[i].fl;
                const float px = f.cx + s.t * (in[i].x - f.cx), py = f.cy + s.t * (in[i].y - f.cy);
                float h, col[3];
                for (int j = 0; j < 3; ++j) col[j] = s.rgba[j] * fl.rgb[j];
                if (k == 0) h = s.half * v[i].size * s.rgba[3];
                else {
                    h = s.half;
                    for (float &c : col) c *= s.rgba[3] * v[i].bright;
                }
                if (col[0] < 1.f && col[1] < 1.f && col[2] < 1.f) v[i].on = false;   // the following sprites no longer draw this one
                const float cap = k == 0 ? 255.f : 32.f;
                FlareQuad q{k, i, ftoi4(px + h), ftoi4(py + 0.5f * h), ftoi4(px - h), ftoi4(py - 0.5f * h),
                            0.5f / tw, 0.5f / th, s.uvMax / tw, s.uvMax / th, {0, 0, 0}};
                for (int j = 0; j < 3; ++j) q.rgb[j] = uint8_t(std::clamp(col[j], 0.f, cap));
                emit(q);
            }
        }
    }

    // Draws the flares of the game's last frame in screen space (inside BeginMode3D, after the particles; the lens flare packet
    // goes out after them in particleDraw). Reads the depth already drawn for the occlusion test (particleLensFlareRender).
    void drawFlares(const TexFn &texture) {
        static const bool off = std::getenv("WOTM_NOFX") != nullptr || std::getenv("WOTM_NOFLARE") != nullptr;
        static const bool log0 = std::getenv("WOTM_FLARELOG") != nullptr;
        if (log0 && !flares.e.empty()) std::fprintf(stderr, "[flare] drawFlares: %zu entries, %d sprites\n", flares.e.size(), flares.nspr);
        if (off || flares.e.empty() || flares.nspr == 0) return;
        const FlareFrame &f = flares;
        rlDrawRenderBatchActive();
        if (g_beforeDepthRead) g_beforeDepthRead();
        const int fbw = rlGetFramebufferWidth(), fbh = rlGetFramebufferHeight();
        if (fbw <= 0 || fbh <= 0) return;
        const GsScreenRect gr = gsScreenRect(float(fbw), float(fbh));   // GS px -> native px (central 4:3 strip in widescreen)
        const float sx = gr.w / f.vw, sy = gr.h / f.vh, x0 = gr.x0;
        const double zn = rlGetCullDistanceNear(), zf = rlGetCullDistanceFar();
        // GS Z (24 bits) that retail would read at pixel (gx, gy) of the GS screen: a + b / W of the native depth; empty (background) = 0
        // split screen: depth from the previous redraw, without waiting on the GPU (AsyncDepth); otherwise the direct read
        const AsyncDepth *ad = depthAsync && depthAsync->valid && depthAsync->cw == fbw && depthAsync->ch == fbh ? depthAsync : nullptr;
        auto gsZ = [&](int gx, int gy) -> uint32_t {
            const int px = std::clamp(int(x0 + (float(gx) + 0.5f) * sx), 0, fbw - 1), py = std::clamp(int((float(gy) + 0.5f) * sy), 0, fbh - 1);
            float d = 1.f;
            if (ad)
                d = ad->cpu[size_t(fbh - 1 - py) * size_t(fbw) + size_t(px)];
            else
                glReadPixels(px, fbh - 1 - py, 1, 1, 0x1902 /* GL_DEPTH_COMPONENT */, 0x1406 /* GL_FLOAT */, &d);
            if (d >= 1.f) return 0u;
            const double ndc = 2.0 * d - 1.0, W = 2.0 * zn * zf / (zf + zn - ndc * (zf - zn));
            const double z = double(f.zScale) + double(f.zOff) / std::max(W, 1e-3);
            return uint32_t(std::clamp(z, 0.0, 16777215.0));
        };
        auto fptoui = [](float v) -> uint32_t { return v <= 0.f ? 0u : v >= 4294967040.f ? 0xFFFFFFFFu : uint32_t(v); };

        std::vector<FlareIn> vis;
        static const bool log = std::getenv("WOTM_FLARELOG") != nullptr;
        for (const Flare &fl : f.e) {
            if (!(fl.flags & 0xFF)) continue;
            const bool inf = (fl.flags & 0x80) != 0;
            // corner pixel of the 4x4 block: ((int) X - 0x6C2, (int) Y - 0x790), i.e. columns X-2..X+1 and rows Y..Y+3 (retail assumes
            // center 2048 and a 640 x 224 screen; TRXPOS verified in the snapshot). With the -2 also applied vertically, local light samples landed
            // two rows too high (Congar's eyebrow, the edge of Robo-47's lens) and the eye flare disappeared.
            const int bx = int(fptoui(fl.x)) - int(f.cx - 0.5f * f.vw) - 2, by = int(fptoui(fl.y)) - int(f.cy - 0.5f * f.vh);
            uint32_t thr;
            if (inf) thr = fptoui(fl.z * 1.03125f) + 1;
            else {
                const float fud = fl.zfudge > 1.5f ? fl.zfudge : 1.5f, d = fl.w - fud;
                thr = fptoui((f.zScale * d + f.zOff) / d);
            }
            const int lo = inf ? 0 : 1, hi = inf ? 4 : 3;
            int cnt = 0, sumC = 0, sumR = 0;
            uint32_t zMax = 0;   // nearest sample (WOTM_FLARELOG)
            for (int r = lo; r < hi; ++r)
                for (int c = lo; c < hi; ++c) {
                    const uint32_t z = gsZ(bx + c, by + r);
                    zMax = std::max(zMax, z);
                    if (z <= thr) { ++cnt; sumC += c; sumR += r; }
                }
            float frac = float(cnt) / float((hi - lo) * (hi - lo));
            if (fl.id && fl.id < 0x7FF) {   // debounce by ID (D_0078FF00): average with the previous frame's fraction
                float &prev = flarePrev_[fl.id];
                const float raw = frac;
                frac = 0.5f * (prev + raw);
                prev = raw;
            }
            const float I = fl.intensity * frac;
            if (log)
                std::fprintf(stderr, "[flare] draw %.1f %.1f visible %d threshold %u I %.4g | light W %.2f, nearest W %.2f | flags %02x dot %.3f bw %.3f\n", fl.x, fl.y, cnt, thr,
                             I, fl.w, zMax && float(zMax) != f.zScale ? f.zOff / (float(zMax) - f.zScale) : 0.f, fl.flags & 0xFF, fl.dot, fl.bw);
            if (cnt == 0 || I < 1.f / 256.f) continue;
            // retail already added the emulated GS's centroid (full visibility: 1.5 - 2 or 1.5 - 1); swap it for ours
            const float shift = 1.5f - (inf ? 2.f : 1.f);
            vis.push_back({fl.x - shift + (float(sumC) / float(cnt) - (inf ? 2.f : 1.f)), fl.y - shift + (float(sumR) / float(cnt) - (inf ? 2.f : 1.f)), I, &fl});
        }
        std::vector<FlareQuad> quads;
        float big = 0.f;   // largest half-side of sprite 0 (WOTM_FLARESHOT diagnostic)
        flareSprites(f, vis, [&](const FlareQuad &q) {
            quads.push_back(q);
            if (q.k == 0) big = std::max(big, std::fabs(q.xa - q.xb) * 0.5f);
        });
        if (quads.empty()) return;

        // Screen in pixels: the projection goes on the stack; the modelview is saved separately (rlPushMatrix in modelview turns on rlgl's
        // transform matrix, which only turns off when the stack empties in modelview; pushing both and popping the projection first
        // swapped the matrices and the HUD drawn afterwards disappeared)
        const Matrix mv3d = rlGetMatrixModelview();
        rlMatrixMode(RL_PROJECTION);
        rlPushMatrix();
        rlLoadIdentity();
        rlOrtho(0.0, double(fbw), double(fbh), 0.0, -1.0, 1.0);
        rlMatrixMode(RL_MODELVIEW);
        rlLoadIdentity();
        rlDisableDepthTest();
        rlDisableBackfaceCulling();
        gsBlend(kGsAlphaAdd);
        shaderNormal(0.f, false, true, true);
        const float ox = f.cx - 0.5f * f.vw, oy = f.cy - 0.5f * f.vh;
        int curK = -1;
        for (const FlareQuad &q : quads) {
            if (q.k != curK) {   // one batch per table sprite (its own texture)
                if (curK >= 0) { rlEnd(); rlDrawRenderBatchActive(); }
                curK = q.k;
                const FlareSprite &s = f.spr[q.k];
                unsigned tex = s.texId ? texture(s.texId, s.tex0) : 0;
                rlBegin(RL_QUADS);
                rlSetTexture(tex ? tex : blobTexture());
            }
            const float xa = x0 + (q.xa - ox) * sx, ya = (q.ya - oy) * sy, xb = x0 + (q.xb - ox) * sx, yb = (q.yb - oy) * sy;
            rlColor4ub(q.rgb[0], q.rgb[1], q.rgb[2], 0x80);
            rlTexCoord2f(q.u0, q.v0); rlVertex2f(xa, ya);
            rlTexCoord2f(q.u0, q.v1); rlVertex2f(xa, yb);
            rlTexCoord2f(q.u1, q.v1); rlVertex2f(xb, yb);
            rlTexCoord2f(q.u1, q.v0); rlVertex2f(xb, ya);
        }
        rlEnd();
        rlDrawRenderBatchActive();
        rlSetTexture(0);
        rlNormal3f(0.f, 0.f, 0.f);
        gsBlend(kGsAlphaNormal);
        rlEnableBackfaceCulling();
        rlEnableDepthTest();
        rlMatrixMode(RL_PROJECTION);
        rlPopMatrix();
        rlMatrixMode(RL_MODELVIEW);
        rlSetMatrixModelview(mv3d);
        // diagnostic: WOTM_FLARESHOT=<prefix> saves up to 8 frames (>= 1 s apart) with a flare whose half-side is >= 8 GS px
        static const char *shot = std::getenv("WOTM_FLARESHOT");
        static int shots = 0;
        static double lastShot = -10.0;
        if (shot && shots < 8 && big >= 8.f && GetTime() - lastShot >= 1.0) {
            lastShot = GetTime();
            unsigned char *px = rlReadScreenPixels(fbw, fbh);
            for (int i = 0; i < fbw * fbh; ++i) px[4 * i + 3] = 255;   // the frame's alpha is the blends', not opacity
            Image img{px, fbw, fbh, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
            char name[512];
            std::snprintf(name, sizeof name, "%s_%d.png", shot, shots++);
            ExportImage(img, name);
            RL_FREE(px);
            std::fprintf(stderr, "[flare] capture %s (half-side %.1f px, %zu entries)\n", name, big, vis.size());
        }
    }

    // Diagnostic (WOTM_FXLIST=1): one system per line plus its first particle
    void dump(const Ram &ram) const {
        std::fprintf(stderr, "[fx] %zu drawable systems\n", systems.size());
        for (const FxSystem &s : systems) {
            const uint32_t b = s.base, p = ram.u32(b + fxoff::pos), a = ram.u32(b + fxoff::age);
            const uint32_t ti = ram.u32(b + fxoff::texInfoIndex);
            const uint32_t tinfo = fxaddr::textureInfo + 8 * (ti & 127);
            std::fprintf(stderr, "[fx] #%u preset %u %s n=%d alpha=%02x colors=%u@%u texInfo=%u (n=%d idx=%u) cpf=%u life=%d | p0 %.1f %.1f %.1f size %.2f age %d\n",
                         s.index, ram.u32(b + fxoff::fxId), fxTypeName(s.type), s.n, ram.u32(b + fxoff::alphaContext),
                         ram.u32(b + fxoff::numColors), ram.u32(b + fxoff::colorIndex), ti, ram.s16(tinfo), ram.u16(tinfo + 2),
                         ram.u32(b + fxoff::colorsPerFrame), ram.s16(b + fxoff::lifetime),
                         ram.f32(p), ram.f32(p + 4), ram.f32(p + 8), ram.f32(p + 12), ram.s16(a));
        }
    }

    // Draws (inside BeginMode3D, after the opaque and translucent objects). Tests depth, doesn't write it.
    // eye: the camera eye (the fog W is measured from it, as on VU1)
    void draw(const Ram &ram, const TexFn &texture, const float eye[3]) {
        static const bool off = std::getenv("WOTM_NOFX") != nullptr;
        if (off || (systems.empty() && synth.empty())) return;
        const Matrix mv = rlGetMatrixModelview();
        FxView view{{mv.m0, mv.m4, mv.m8}, {mv.m1, mv.m5, mv.m9}, {mv.m2, mv.m6, mv.m10}, {-mv.m2, -mv.m6, -mv.m10, -mv.m14},
                    {mv.m0, mv.m4, mv.m8, mv.m12}, {mv.m1, mv.m5, mv.m9, mv.m13}};
        float row3[4];
        for (int k = 0; k < 4; ++k) row3[k] = ram.f32(viewW2S() + 48 + 4 * k);
        setFog(ram, view, row3, eye);
        // particleDraw: farthest to nearest by the depth of emit.ParticleToEmitVec + vis.priority; opaque ones (alpha 0) first
        for (FxSystem &s : systems) {
            if (ram.u32(s.base + fxoff::alphaContext) == 0) { s.key = 50000.f + ram.f32(s.base + fxoff::priority); continue; }
            const float x = ram.f32(s.base + fxoff::emitVec), y = ram.f32(s.base + fxoff::emitVec + 4), z = ram.f32(s.base + fxoff::emitVec + 8);
            s.key = -(mv.m2 * x + mv.m6 * y + mv.m10 * z + mv.m14) + ram.f32(s.base + fxoff::priority);
        }
        std::stable_sort(systems.begin(), systems.end(), [](const FxSystem &a, const FxSystem &b) { return a.key > b.key; });
        rlDrawRenderBatchActive();
        rlDisableDepthMask();
        rlDisableBackfaceCulling();
        for (const FxSystem &s : systems) drawSystem(ram, s, texture, view);
        for (const SynthSystem &s : synth) drawSynth(ram, s, texture, view);
        rlDrawRenderBatchActive();
        rlNormal3f(0.f, 0.f, 0.f);   // no fog for what comes next (overlay)
        rlSetBlendMode(BLEND_ALPHA);
        rlEnableDepthMask();
        rlEnableBackfaceCulling();
    }

    // RGBA color (0x80 = 1.0) of a particle with age `age`, like loadColors
    static uint32_t colorAt(const Ram &ram, uint32_t b, int age) {
        const uint32_t nc = ram.u32(b + fxoff::numColors);
        const uint32_t tab = fxaddr::colors + 4 * ram.u32(b + fxoff::colorIndex);
        if (nc == 0) return 0x80808080u;
        if (nc == 1) return ram.u32(tab);
        const uint32_t t = uint32_t(std::max(age, 0)) * ram.u32(b + fxoff::colorsPerFrame);
        uint32_t k = t >> 12, f = t & 0xFFF;
        if (k >= nc - 1) { k = nc - 2; f = 0x1000; }   // retail doesn't clamp (the particle dies first); here it's just for safety
        const uint32_t c0 = ram.u32(tab + 4 * k), c1 = ram.u32(tab + 4 * (k + 1));
        uint32_t out = 0;
        for (int sh = 0; sh < 32; sh += 8) {
            const int a = int((c0 >> sh) & 255), z = int((c1 >> sh) & 255);
            out |= uint32_t((a + (((z - a) * int(f)) >> 12)) & 255) << sh;
        }
        return out;
    }


    // dustTrail (type 15), microprogram 0x800 with the dustTrailPacketInit header at 0x20C-0x20E. The age word (32 bits per
    // particle, particleEmit is15) is the color: RGB in bytes 0-2 (the 2 low bits of byte 2 pick the UV cell) and the size
    // roll a in byte 3 (from min*255/max to 255); pos.w is the normalized age (0..1). Per particle, with W = depth:
    //  - half-side = exp*life*w/2 + (min + (max-min)*a/255) * (1 - k + k*a/255) / 2, k = emit.preAgeFactor (+0x2D88); the cap of
    //    0.375 * W is in forEachQuad;
    //  - alpha = min(W - 128, minAlpha + (maxAlpha-minAlpha)*a/255) * min(1, w/alphaFadeIn, (1-w)/alphaFadeOut), truncated;
    //  - dropped with W <= 128 (MAC Sw|Zw of W - 128) or alpha < 8 (F <= 0 and corners outside the band are left to fogged and the screen:
    //    with the size cap, a center on screen never has a corner outside the 4096 band).
    struct DustTrailVu { bool visible; float half; uint32_t cell; uint32_t rgba; };
    static DustTrailVu dustTrailVu(const Ram &ram, uint32_t b, int i, float w, float W) {
        DustTrailVu d{false, 0.f, 0, 0};
        if (!(W > 128.f)) return d;
        const uint32_t word = ram.u32(ram.u32(b + fxoff::age) + 4 * uint32_t(i));
        const float a = float(word >> 24) / 255.f;
        const float mn = ram.f32(b + fxoff::minSize), mx = ram.f32(b + fxoff::maxSize), k = ram.f32(b + fxoff::preAgeFactor);
        d.half = 0.5f * ram.f32(b + fxoff::expansionRate) * float(ram.s16(b + fxoff::lifetime)) * w + 0.5f * (mn + (mx - mn) * a) * (1.f - k + k * a);
        const float fin = ram.f32(b + fxoff::alphaFadeIn), fout = ram.f32(b + fxoff::alphaFadeOut);
        const float amin = ram.f32(b + fxoff::minAlpha), amax = ram.f32(b + fxoff::maxAlpha);
        const float fade = std::min({1.f, fin > 0.f ? w / fin : 1.f, fout > 0.f ? (1.f - w) / fout : 1.f});   // DIV by 0 on the VU: maximum
        const float alpha = std::min(W - 128.f, amin + (amax - amin) * a) * fade;
        if (!(alpha >= 8.f)) return d;
        d.visible = true;
        d.cell = (word >> 16) & 3;
        d.rgba = (word & 0x00FFFFFFu) | (uint32_t(std::min(alpha, 255.f)) << 24);
        return d;
    }

    // wake (16): wakePacketInit and microprogram 0x3470. One triangle per particle with the data from the packet, which sits right before
    // pos[] (UNPACK of 25 qwords at q264; q264 = pos - 0x19C):
    //  - q264-q279: 8 sets of ST (STQ) of an equilateral triangle that circumscribes the texture's [0,1]^2 square (vertices
    //    (-0.366; 0), (1.366; 0), (0.5; 1.5) in different orders): v0 = q[264+k].xy, v1 = q[264+k].zw, v2 = q[272+k].xy, k = bits & 7;
    //  - q282: RGB color (integers, 0x80 = 1.0) from colors[colorIndex], the same on all three vertices (fxId 0x93 changes it every frame);
    //  - q283-q285: size (x, z) and alpha (y, w) curves over the age w from pos.w (0..1): up to the transition (q283.z / q283.w) a
    //    parabola q285 + q284*w + q283*w^2, then the line q285.zw + q284.zw*w (size maxSize + expansion; alpha from minAlpha rising
    //    128/alphaFadeIn per lifetime up to the line that falls 128/alphaFadeOut and reaches maxAlpha at w = 1);
    //  - q287, q288: axes A = 0.5 * row 0 and B = -0.25 * row 0 + 0.433 * row 2 of the emitter's StaticMat (fx+0x2D20; with
    //    info.attribs & 0x4000, world X and Y). Vertices pos + S*A, pos + S*B and pos - S*(A + B).
    // The 12 low bits of pos.w are fixed per particle (wakeEmit; wakeAndKiteMove preserves the 0xFFF mask): bits 3-11 scale the
    // size, S = (bits & 0xFF8) / 4096 * size(w); bits 0-2 pick the ST set. Alpha = min(alpha(w), 128) truncated (the GS
    // takes the low byte). Dropped with W > 4000 (MAC Sw|Zw of W - 4000).
    struct WakeVu { bool visible; float c[3][3]; float uv[3][2]; uint32_t rgba; };
    static WakeVu wakeVu(const Ram &ram, uint32_t b, int i, float W) {
        WakeVu r{};
        const uint32_t pos = ram.u32(b + fxoff::pos), q264 = pos - 0x19C, pw = pos + 16 * uint32_t(i);
        if (!(W <= 4000.f) || !ram.ok(q264, 0x19C) || !ram.ok(pw, 16)) return r;
        auto q = [&](int k, int c) { return ram.f32(q264 + 16 * uint32_t(k - 264) + 4 * uint32_t(c)); };
        const float w = ram.f32(pw + 12);
        const uint32_t bits = ram.u32(pw + 12) & 0xFFFF;
        const float w2 = w * w;
        const float size = q(283, 2) - w < 0.f ? q(285, 2) + q(284, 2) * w : q(285, 0) + q(284, 0) * w + q(283, 0) * w2;
        const float alpha = q(283, 3) - w < 0.f ? q(285, 3) + q(284, 3) * w : q(285, 1) + q(284, 1) * w + q(283, 1) * w2;
        const float S = float(bits & 0xFF8) / 4096.f * size;
        float A[3], B[3];
        for (int a = 0; a < 3; ++a) { A[a] = q(287, a) * S; B[a] = q(288, a) * S; }
        for (int a = 0; a < 3; ++a) {
            const float p = ram.f32(pw + 4 * uint32_t(a));
            r.c[0][a] = p + A[a]; r.c[1][a] = p + B[a]; r.c[2][a] = p - A[a] - B[a];
        }
        const int k = int(bits & 7);
        r.uv[0][0] = q(264 + k, 0); r.uv[0][1] = q(264 + k, 1);
        r.uv[1][0] = q(264 + k, 2); r.uv[1][1] = q(264 + k, 3);
        r.uv[2][0] = q(272 + k, 0); r.uv[2][1] = q(272 + k, 1);
        const float am = std::min(alpha, 128.f);
        const int ai = std::isfinite(am) ? int(am) : 0;
        const uint32_t q282 = q264 + 16 * 18;
        r.rgba = (ram.u32(q282) & 255) | ((ram.u32(q282 + 4) & 255) << 8) | ((ram.u32(q282 + 8) & 255) << 16) | (uint32_t(ai & 255) << 24);
        r.visible = std::isfinite(S);
        return r;
    }

    static bool isStrip(uint32_t type) { return type == FX_STRIPE || type == FX_SWEEPTRAIL; }

    // Particle fog: the microprograms' common context (qwords 0-7, from 0x256B90) carries fogFA in q4.w and fogFB in q5.w, the
    // same as vu1FogParams; VU1 computes F = min(fogFA + fogFB / W, 255) and drops the particle with F < 1 (CLIP), with or without FGE.
    // Measured on METRO (fogFA -107.9, fogFB 326,600: W > ~3,000): the pedestrians disappear there (VU1CHECK.md, "Explained").
    static void setFog(const Ram &ram, FxView &v, const float row3[4], const float eye[3]) {
        v.fogFA = ram.f32(addr::vu1FogParams);
        v.fogFB = ram.f32(addr::vu1FogParams + 12);
        v.fogW[3] = row3[3];
        for (int a = 0; a < 3; ++a) { v.fogW[a] = row3[a]; v.fogW[3] -= row3[a] * eye[a]; }
    }
    // A point (world) fully in the fog: F < 1
    static bool foggedAt(const FxView &v, const float p[3]) {
        if (v.fogFB == 0.f) return false;
        const float w = v.fogW[0] * p[0] + v.fogW[1] * p[1] + v.fogW[2] * p[2] + v.fogW[3];
        return w > 1e-3f && v.fogFA + v.fogFB / w < 1.f;
    }
    // A particle fully in the fog (at the center of the quad)
    static bool fogged(const FxView &v, const float c[4][3]) {
        float p[3];
        for (int a = 0; a < 3; ++a) p[a] = 0.25f * (c[0][a] + c[1][a] + c[2][a] + c[3][a]);
        return foggedAt(v, p);
    }

    // VU random generator (RINIT/RNEXT, like PCSX2): R in [1, 2) with a 23-bit LFSR
    static uint32_t vuRinit(uint32_t bits) { return 0x3F800000u | (bits & 0x7FFFFFu); }
    static float vuRnext(uint32_t &R) {
        const uint32_t x = (R >> 4) & 1, y = (R >> 22) & 1;
        R = (((R << 1) ^ x ^ y) & 0x7FFFFFu) | 0x3F800000u;
        float f;
        std::memcpy(&f, &R, 4);
        return f;
    }

    // Electric arc (types 7 and 17; electricArcPacketInit/Build/Update, microprogram 0x19F8, read from build/vu1/particle.txt).
    // Packet: pos[] (fx +0x2EB0) in chains (head: low w = 1 + links; link: low w = 5 << (level - 1), or 0 = straight; head 0
    // ends it); before it the header (pos - 0x28: minSize, seed, segInitU, 138 + CLUToffset, expansionRate, jag, segDeltaU,
    // numColors - 1) and 16 RGBA colors as float 0..255 (pos - 0x13C), which electricArcPacketUpdate rewrites.
    //  - RINIT(seed) once per packet. A level-L link from a to b: 2^L sub-segments; amp = jag * |b - a|, halved at each level; in each
    //    gap, mid = 0.5a + r * amp - 1.5 * amp + 0.5b, r = (rx, ry, rz) in a sliding window of RNEXT (three at the start of the link; after
    //    each mid, rx = ry, ry = rz, rz = RNEXT).
    //  - Ribbon (subroutine 0x1F70) over the S + 1 points, in isotropic screen coordinates with y down (VU1 corrects y by the ratio
    //    of the focal lengths): ghosts 2P0 - P1 and 2PS - PS-1 (homogeneous), unit t_in and t_out, n = (t.y, -t.x); v = n_in + n_out +
    //    (t_out - t_in) if t_in x t_out < 0, else + (t_in - t_out) (not renormalized). Vertices p +/- s_k / 2 * v in the view plane,
    //    s_k = minSize + exp * k / S; A (+) with V = 1, B (-) with V = 0; U_k = segInitU + segDeltaU * (k - S / 2) (perspective, STQ);
    //    color = table lerp at clut + k * (nc - 1) / S, + 0.5, truncated (the same on both vertices).
    //  - Clipping (0x2210-0x2288): CLIP of each strip vertex, A and B, against the band (GS x and y in [0, 4096], z = W >= 1), and the ADC
    //    comes from the flags of the last 4 vertices. Result: every strip triangle (A_k, B_k, A_k+1) and (B_k, A_k+1, B_k+1) with any
    //    vertex outside disappears. The F < 1 test is in forEachQuad (by the center).
    // g(k, corners[4][3] (A_k, B_k, B_k+1, A_k+1), uv[4][2], color[2] (RGBA 0x80 = 1.0, at k and k + 1), mask): bit 0 = triangle
    // (A_k, B_k, A_k+1) = corners 0, 1, 3; bit 1 = (B_k, A_k+1, B_k+1) = corners 1, 3, 2 (the strip's diagonal is B_k - A_k+1).
    // Chain (type 12; chainCreate/chainPacketInit/Build, microprogram 0x1D00, which calls the same ribbon 0x1F70): the same header
    // somewhere else (pos - 0x4C: minSize already multiplied by 0.5 by the EE, -, segInitU, clut 642; pos - 0x3C: expansionRate, -,
    // segDeltaU, numColors - 1) and 2 colors at pos - 0x28. Each chain: a head with the point count in w (integer); no
    // subdivision (the points go straight to the ribbon). Analysis: git show 75f7fd4:docs/investigacoes/2026-10-17/01_corda_ultrav.md.
    template <class G> static void arcQuads(const Ram &ram, const FxSystem &s, const FxView &view, G &&g) {
        const uint32_t pos = ram.u32(s.base + fxoff::pos);
        const bool chain = s.type == FX_CHAIN;
        const uint32_t hdr = chain ? 0x4C : 0x28, cols = chain ? 0x28 : 0x13C;
        const int lastCol = chain ? 1 : 15;
        if (s.n < 3 || !ram.ok(pos - std::max(hdr, cols), std::max(hdr, cols) + 16 * uint32_t(s.n))) return;
        const float minSize = ram.f32(pos - hdr), segInitU = ram.f32(pos - hdr + 8), clut = chain ? 0.f : ram.f32(pos - 0x1C) - 138.f;
        const float expRate = ram.f32(pos - hdr + 0x10), jag = chain ? 0.f : ram.f32(pos - 0x14), segDeltaU = ram.f32(pos - hdr + 0x18),
                    ncm1 = ram.f32(pos - hdr + 0x1C);
        uint32_t R = vuRinit(chain ? 0u : ram.u32(pos - 0x24));
        struct V3 { float x, y, z; };
        auto at = [&](int i) { return V3{ram.f32(pos + 16 * uint32_t(i)), ram.f32(pos + 16 * uint32_t(i) + 4), ram.f32(pos + 16 * uint32_t(i) + 8)}; };
        auto w16 = [&](int i) { return int(ram.u16(pos + 16 * uint32_t(i) + 12)); };
        auto color = [&](float cpos) {   // RGBA color (0x80 = 1.0) at position cpos of the table (lerp, + 0.5, truncated)
            const int k0 = std::clamp(int(cpos), 0, lastCol), k1 = std::min(k0 + 1, lastCol);
            const float fr = cpos - float(int(cpos));
            uint32_t out = 0;
            for (int c = 0; c < 4; ++c) {
                const float a = ram.f32(pos - cols + 16 * uint32_t(k0) + 4 * uint32_t(c)), b = ram.f32(pos - cols + 16 * uint32_t(k1) + 4 * uint32_t(c));
                const float v = a + 0.5f - a * fr + b * fr;
                out |= uint32_t(std::clamp(int(v), 0, 255)) << (8 * c);
            }
            return out;
        };
        // isotropic screen (y down) and VU1's W (row 3 of worldToScreenMat; without the game camera, the depth)
        auto proj = [&](const V3 &p, float &x, float &y) {
            const float z = view.depth[0] * p.x + view.depth[1] * p.y + view.depth[2] * p.z + view.depth[3];
            const float iz = 1.f / (std::fabs(z) > 1e-6f ? z : 1e-6f);
            x = (view.ox[0] * p.x + view.ox[1] * p.y + view.ox[2] * p.z + view.ox[3]) * iz;
            y = -(view.oy[0] * p.x + view.oy[1] * p.y + view.oy[2] * p.z + view.oy[3]) * iz;
        };
        auto wOf = [&](const V3 &p) {
            if (view.fogW[0] == 0.f && view.fogW[1] == 0.f && view.fogW[2] == 0.f)
                return view.depth[0] * p.x + view.depth[1] * p.y + view.depth[2] * p.z + view.depth[3];
            return view.fogW[0] * p.x + view.fogW[1] * p.y + view.fogW[2] * p.z + view.fogW[3];
        };
        // VU1 clipping band on the GS screen (worldToScreenMat with the eye, like striteTriangle)
        float M[4][4], eye[3];
        for (int r = 0; r < 4; ++r)
            for (int k = 0; k < 4; ++k) M[r][k] = ram.f32(viewW2S() + 16 * uint32_t(r) + 4 * uint32_t(k));
        for (int k = 0; k < 3; ++k) eye[k] = -ram.f32(viewCtxAddr() + 0x20 + 4 * uint32_t(k));   // worldCtx[view].eo = -eye
        auto outOfBand = [&](const float c[3]) {
            const float d[3] = {c[0] - eye[0], c[1] - eye[1], c[2] - eye[2]};
            float h[4];
            for (int r = 0; r < 4; ++r) h[r] = M[r][0] * d[0] + M[r][1] * d[1] + M[r][2] * d[2] + M[r][3];
            if (!(h[3] >= 1.f)) return true;
            const float x = h[0] / h[3], y = h[1] / h[3];
            return !(x >= 0.f && x <= 4096.f && y >= 0.f && y <= 4096.f);
        };
        std::vector<V3> P;
        int i = 0;
        while (i < s.n) {
            const int links = w16(i) - 1;
            if (links < 0 || i + links >= s.n) break;   // head 0 (end) or truncated chain
            P.clear();
            P.push_back(at(i));
            for (int l = 1; l <= links; ++l) {
                const V3 b = at(i + l);
                const int wl = chain ? 0 : w16(i + l);   // chain: the links' w isn't read
                if (wl == 0) { P.push_back(b); continue; }
                int h = wl / 5;   // half-distance (in points) of the first level: 2^(L - 1)
                if (h < 1 || h > 64) { P.push_back(b); continue; }
                const size_t base = P.size() - 1, end = base + size_t(2 * h);
                P.resize(end + 1);
                P[end] = b;
                const V3 a = P[base];
                const float dx = b.x - a.x, dy = b.y - a.y, dz = b.z - a.z;
                float amp = jag * std::sqrt(dx * dx + dy * dy + dz * dz);
                float rx = vuRnext(R), ry = vuRnext(R), rz = vuRnext(R);
                for (; h >= 1; h /= 2, amp *= 0.5f)
                    for (size_t j = base; j < end; j += size_t(2 * h)) {
                        const V3 &pa = P[j], &pb = P[j + size_t(2 * h)];
                        V3 &m = P[j + size_t(h)];
                        m.x = 0.5f * pa.x + rx * amp - 1.5f * amp + 0.5f * pb.x;
                        m.y = 0.5f * pa.y + ry * amp - 1.5f * amp + 0.5f * pb.y;
                        m.z = 0.5f * pa.z + rz * amp - 1.5f * amp + 0.5f * pb.z;
                        rx = ry; ry = rz; rz = vuRnext(R);
                    }
            }
            i += links + 1;
            const int S = int(P.size()) - 1;
            if (S < 1) continue;
            // points on screen, with the ghosts at the ends
            const size_t np = P.size();
            std::vector<float> sx(np + 2), sy(np + 2);
            const V3 g0{2.f * P[0].x - P[1].x, 2.f * P[0].y - P[1].y, 2.f * P[0].z - P[1].z};
            const V3 gn{2.f * P[np - 1].x - P[np - 2].x, 2.f * P[np - 1].y - P[np - 2].y, 2.f * P[np - 1].z - P[np - 2].z};
            proj(g0, sx[0], sy[0]);
            for (size_t k = 0; k < np; ++k) proj(P[k], sx[k + 1], sy[k + 1]);
            proj(gn, sx[np + 1], sy[np + 1]);
            std::vector<float> ox(np), oy(np);   // v of each point (isotropic screen, y down)
            std::vector<char> ok(np);
            // chain: 0x1D00 walks from P[n-1] to P[0] when W(P[0]) < W(P[n-1]) (it starts at the farther end; seen in the VU1
            // trace of ultrav_corda); here that only changes the zero-product branch, below
            const bool rev = chain && wOf(P[0]) < wOf(P[np - 1]);
            for (size_t k = 0; k < np; ++k) {
                float tix = sx[k + 1] - sx[k], tiy = sy[k + 1] - sy[k], tox = sx[k + 2] - sx[k + 1], toy = sy[k + 2] - sy[k + 1];
                const float li = std::sqrt(tix * tix + tiy * tiy), lo = std::sqrt(tox * tox + toy * toy);
                // chain: a zero-length tangent becomes the zero vector (on the VU, 0 * RSQRT(0) = 0; the end points coincide)
                ok[k] = (chain ? li > 1e-12f || lo > 1e-12f : li > 1e-12f && lo > 1e-12f) && wOf(P[k]) >= 1.f;
                if (!ok[k]) continue;
                const float ri = li > 1e-12f ? 1.f / li : 0.f, ro = lo > 1e-12f ? 1.f / lo : 0.f;
                tix *= ri; tiy *= ri; tox *= ro; toy *= ro;
                const float cross = tix * toy - tox * tiy;
                // at exactly zero (zero tangent at the end of the chain) the VU sees +0, "not negative": on this side that is the cross < 0
                // branch when the chain is walked in the same order, and the other one when reversed (rev)
                const float sgn = cross < 0.f ? 1.f : cross > 0.f ? -1.f : (chain && !rev ? 1.f : -1.f);
                ox[k] = tiy + toy + sgn * (tox - tix);
                oy[k] = -tix - tox + sgn * (toy - tiy);            }
            float cs[4][3], uv[4][2];
            const float u0 = segInitU - segDeltaU * 0.5f * float(S);
            auto corner = [&](size_t k, float side, float out[3]) {
                // VU1 offsets by v * s * (half the focal length, q5.xy) / W pixels: in the view plane, s / 2 (measured: ratio 0.5 against s)
                // chain: half of that (measured in photo 2026-10-17_224038/002: with the packet's q265.x = 10, VU1 gives width 0.25 * 10 * v)
                const float sz = (chain ? 0.25f : 0.5f) * (minSize + expRate * float(k) / float(S));
                const float wx = side * sz * ox[k], wy = side * sz * oy[k];   // in the view plane: screen y down = -up
                const float p[3] = {P[k].x, P[k].y, P[k].z};
                for (int a = 0; a < 3; ++a) out[a] = p[a] + view.right[a] * wx - view.up[a] * wy;
            };
            for (size_t k = 0; k + 1 < np; ++k) {
                if (!ok[k] || !ok[k + 1]) continue;
                corner(k, 1.f, cs[0]); corner(k, -1.f, cs[1]); corner(k + 1, -1.f, cs[2]); corner(k + 1, 1.f, cs[3]);
                const bool o[4] = {outOfBand(cs[0]), outOfBand(cs[1]), outOfBand(cs[2]), outOfBand(cs[3])};
                const int mask = (!o[0] && !o[1] && !o[3] ? 1 : 0) | (!o[1] && !o[3] && !o[2] ? 2 : 0);
                if (!mask) continue;
                const float ua = u0 + segDeltaU * float(k), ub = u0 + segDeltaU * float(k + 1);
                uv[0][0] = ua; uv[0][1] = 1.f; uv[1][0] = ua; uv[1][1] = 0.f;
                uv[2][0] = ub; uv[2][1] = 0.f; uv[3][0] = ub; uv[3][1] = 1.f;
                const uint32_t col[2] = {color(clut + float(k) * ncm1 / float(S)), color(clut + float(k + 1) * ncm1 / float(S))};
                g(int(k), cs, uv, col, mask);
            }
        }
    }

    // Sprine (type 8; sprinePacketInit/Build, microprogram 0x2770 in build/vu1/particle.txt). The packet sits before pos[] (qword 8):
    // RGBA colors (V4-8, q264-295), UV (V4-16 in 1/16 texel, q296-299: u0, v0, u1, v1), and q300-q306 in a 7-qword UNPACK that ends
    // at pos - 4: q300.x = preAgeFactor (pa); q301 = (h (nc - 1) / life, h exp, -, f6); q302 = (minSize, N, (N + 1) >> 1, sprites per
    // batch; all three integers); q303 and q304 = forward-difference weights (x h^2); q305 = M2 (row 2 of ParticleToEmitMat);
    // q306 = ((n - 1) N, age[n - 1] N, n - 1; integer, float, integer). h = 1/N.
    //  - One segment per pair A = pos[i], B = pos[i - 1] (i from n - 1 to 1), with Pm = pos[i + 1] and Pn = pos[i - 2]; ghosts: on the 1st segment
    //    Pm = B - 2 M2 ((B - A) . M2) (B reflected on A's plane with normal M2), on the last Pn = 2 pos[0] - pos[1].
    //  - D1 = q303 . (Pm, A, B, Pn), D2 = q304 . (...), D2' = q304 reversed (w, z, y, x) . (...). p = A; on each step: emit at p,
    //    p += D1, D1 += D2; if after that (N + 1) >> 1 steps remain (and N > 2), D1 += c (D2' - D2), D2 = D2' (c = 0.5 with even N,
    //    0.125 odd). N steps per segment; pos[0] is not emitted.
    //  - Global step s (aN = q306.y + s): r = current generator value; j = 1 + pa (2 - r); side = (minSize + q301.y aN) (1 + (r - 1) f6);
    //    color = table at min(31, q301.x aN j) (lerp, + 0.5, truncated; on the 1st step of a segment that doesn't start a batch, j counts twice).
    //    Then RNEXT, and the step's UV is q296 + (bits of r & 3). RINIT(pos[n - 1].w) and one RNEXT at the start; RINIT(pos[i].w) on each segment
    //    (it only reseeds the generator; the current r carries on).
    //  - Screen-aligned SPRITE: center p +/- (192, 90.24) side / W px, i.e. half-side side / 2 in the view plane. Disappears with W < 1 or
    //    F < 1 (the F test is in forEachQuad).
    // g(step, corners[4][3] ((-,-), (+,-), (+,+), (-,+) on screen), uv[4][2], RGBA color (0x80 = 1.0)).
    template <class G> static void sprineQuads(const Ram &ram, const FxSystem &s, const FxView &view, G &&g) {
        const uint32_t pos = ram.u32(s.base + fxoff::pos);
        const uint32_t q300 = pos - 0x74;
        if (s.n < 2 || !ram.ok(q300 - 0x24 - 0x80 - 4, 0x200) || !ram.ok(pos, 16 * uint32_t(s.n))) return;
        if ((ram.u32(q300 - 4) & 0x7F0003FFu) != (0x6C000000u | 300u) || (ram.u32(pos - 4) & 0x3FF) != 8) return;   // UNPACK of q300 and of pos
        const uint32_t uvAt = q300 - 4 - 0x20, colAt = uvAt - 4 - 0x80;   // data of the UNPACKs of q296 (4 x V4-16) and q264 (32 x V4-8)
        if ((ram.u32(uvAt - 4) & 0x3FF) != 296 || (ram.u32(colAt - 4) & 0x3FF) != 264) return;
        auto q = [&](int k, int c) { return ram.f32(q300 + 16 * uint32_t(k - 300) + 4 * uint32_t(c)); };
        auto qi = [&](int k, int c) { return int(ram.u32(q300 + 16 * uint32_t(k - 300) + 4 * uint32_t(c)) & 0xFFFF); };
        const float pa = q(300, 0), cPos = q(301, 0), cExp = q(301, 1), f6 = q(301, 3), minSize = q(302, 0);
        const int N = qi(302, 1), half = qi(302, 2), batch = qi(302, 3), total = qi(306, 0);
        if (N < 2 || N > 64 || batch < 1 || total != (s.n - 1) * N) return;
        float W3[4], W4[4];
        for (int c = 0; c < 4; ++c) { W3[c] = q(303, c); W4[c] = q(304, c); }
        const float M2[3] = {q(305, 0), q(305, 1), q(305, 2)};
        const float cc = (N & 1) ? 0.125f : 0.5f;
        const uint64_t t0 = systemTex0(ram, s.base);
        const float tw = 16.f * float(1u << ((t0 >> 26) & 15)), th = 16.f * float(1u << ((t0 >> 30) & 15));
        struct V3 { float v[3]; };
        auto P = [&](int i) { return V3{{ram.f32(pos + 16 * uint32_t(i)), ram.f32(pos + 16 * uint32_t(i) + 4), ram.f32(pos + 16 * uint32_t(i) + 8)}}; };
        auto wbits = [&](int i) { return ram.u32(pos + 16 * uint32_t(i) + 12); };
        auto color = [&](float cpos) {
            const int k0 = std::clamp(int(cpos), 0, 31), k1 = std::min(k0 + 1, 31);
            const float fr = cpos - float(k0);
            uint32_t out = 0;
            for (int c = 0; c < 4; ++c) {
                const float a = ram.u8(colAt + 4 * uint32_t(k0) + uint32_t(c)), b = ram.u8(colAt + 4 * uint32_t(k1) + uint32_t(c));
                out |= uint32_t(std::clamp(int(a + 0.5f - a * fr + b * fr), 0, 255)) << (8 * c);
            }
            return out;
        };
        auto wOf = [&](const float p[3]) {
            if (view.fogW[0] == 0.f && view.fogW[1] == 0.f && view.fogW[2] == 0.f)
                return view.depth[0] * p[0] + view.depth[1] * p[1] + view.depth[2] * p[2] + view.depth[3];
            return view.fogW[0] * p[0] + view.fogW[1] * p[1] + view.fogW[2] * p[2] + view.fogW[3];
        };
        uint32_t R = vuRinit(wbits(s.n - 1));
        float r = vuRnext(R);
        float aN = q(306, 1);
        int step = 0;
        V3 Pm, A = P(s.n - 1), B = P(s.n - 2), Pn;
        {
            float d = 0.f;
            for (int a = 0; a < 3; ++a) d += (B.v[a] - A.v[a]) * M2[a];
            for (int a = 0; a < 3; ++a) Pm.v[a] = B.v[a] - M2[a] * (2.f * d);
        }
        for (int i = s.n - 1; i >= 1; --i) {
            if (i >= 2) Pn = P(i - 2);
            else for (int a = 0; a < 3; ++a) Pn.v[a] = 2.f * B.v[a] - A.v[a];
            float D1[3], D2[3], D2b[3], p[3];
            for (int a = 0; a < 3; ++a) {
                D1[a] = Pm.v[a] * W3[0] + A.v[a] * W3[1] + B.v[a] * W3[2] + Pn.v[a] * W3[3];
                D2[a] = Pm.v[a] * W4[0] + A.v[a] * W4[1] + B.v[a] * W4[2] + Pn.v[a] * W4[3];
                D2b[a] = Pm.v[a] * W4[3] + A.v[a] * W4[2] + B.v[a] * W4[1] + Pn.v[a] * W4[0];
                p[a] = A.v[a];
            }
            R = vuRinit(wbits(i));
            const bool twiceJ = step % batch != 0;   // 1st step of a segment that doesn't start a batch
            for (int k = 0; k < N; ++k, ++step) {
                const float j = 1.f + pa * (2.f - r);
                const float side = (minSize + cExp * aN) * (1.f + (r - 1.f) * f6);
                const float cpos = std::min(31.f, cPos * aN * j * (k == 0 && twiceJ ? j : 1.f));
                r = vuRnext(R);
                const uint32_t uvq = uvAt + 8 * (R & 3);
                const float u0 = ram.u16(uvq) / tw, v0 = ram.u16(uvq + 2) / th, u1 = ram.u16(uvq + 4) / tw, v1 = ram.u16(uvq + 6) / th;
                if (wOf(p) >= 1.f) {
                    const float h = 0.5f * side;
                    float cs[4][3];
                    for (int a = 0; a < 3; ++a) {
                        const float rx = view.right[a] * h, uy = view.up[a] * h;   // screen y down = -up
                        cs[0][a] = p[a] - rx + uy; cs[1][a] = p[a] + rx + uy;
                        cs[2][a] = p[a] + rx - uy; cs[3][a] = p[a] - rx - uy;
                    }
                    const float uv[4][2] = {{u0, v0}, {u1, v0}, {u1, v1}, {u0, v1}};
                    g(step, cs, uv, color(cpos));
                }
                aN += 1.f;
                for (int a = 0; a < 3; ++a) { p[a] += D1[a]; D1[a] += D2[a]; }
                if (N > 2 && N - 1 - k == half)
                    for (int a = 0; a < 3; ++a) { D1[a] = D1[a] - cc * D2[a] + cc * D2b[a]; D2[a] = D2b[a]; }
            }
            Pm = A; A = B; B = Pn;
        }
    }

    // The system's geometry in world coordinates, one quad at a time: f(i, corners[4][3], uv[4][2]), i = particle (for strips,
    // the span from i to i + 1; corners 0 and 1 at i, 2 and 3 at i + 1). Lines (FX_LINE) don't go through here. Used by draw and by the VU1 check.
    template <class F> static void forEachQuad(const Ram &ram, const FxSystem &s, const FxView &view, F &&emit) {
        auto f = [&](int i, const float c[4][3], const float uv[4][2]) { if (!fogged(view, c)) emit(i, c, uv); };
        const uint32_t b = s.base;
        const uint32_t pos = ram.u32(b + fxoff::pos), vel = ram.u32(b + fxoff::vel);
        const bool haveVel = ram.ok(vel, 16 * uint32_t(s.n));
        const float *back = view.back;
        auto P = [&](int i, float out[4]) { for (int c = 0; c < 4; ++c) out[c] = ram.f32(pos + 16 * uint32_t(i) + 4 * uint32_t(c)); };
        auto V = [&](int i, float out[3]) { for (int c = 0; c < 3; ++c) out[c] = haveVel ? ram.f32(vel + 16 * uint32_t(i) + 4 * uint32_t(c)) : 0.f; };
        float c[4][3], uv[4][2];
        if (s.type == FX_LINE) return;
        if (s.type == FX_ARC || s.type == FX_ARC2 || s.type == FX_CHAIN) {
            // only the quads with both triangles (on VU1 an unpaired triangle counts as loose)
            arcQuads(ram, s, view, [&](int k, const float cc[4][3], const float uu[4][2], const uint32_t *, int mask) { if (mask == 3) f(k, cc, uu); });
            return;
        }
        if (s.type == FX_SPRINE) {
            sprineQuads(ram, s, view, [&](int k, const float cc[4][3], const float uu[4][2], uint32_t) { f(k, cc, uu); });
            return;
        }
        if (isStrip(s.type)) {
            // A strip joining consecutive particles, with half-width |pos.w|. As on VU1 (measured by the check on stripe: the edge at
            // each end comes out perpendicular to the strip on screen), end i opens perpendicular, on screen, to the direction between its neighbors
            // (i - 1 and i + 1), with the half-width in the view plane (projects to |pos.w| * focal / depth).
            // stripe: stripePacketBuild sends VU1 the age of the newest particle (n - 1), and the width is reduced by how much it
            // has already grown: with the emitter stopped the whole strip thins out. Measured by the check (preset 106): half-width =
            // |pos.w| - |pos.w[n - 1]| + minSize (with the emitter active, the newest has age ~0 and gives |pos.w|).
            // Polyline of the particles (stripe, sweepTrail), each point with its half-width and particle (for the color in draw).
            struct Pt { float p[3], hw, u; int seg; };
            std::vector<Pt> pts;
            pts.reserve(size_t(s.n));
            {
                float wHead = 0.f;
                if (s.type == FX_STRIPE) { float ph[4]; P(s.n - 1, ph); wHead = std::fabs(ph[3]) - ram.f32(b + fxoff::minSize); }
                // stripe: U from the packet header (pos - 0x28, stripePacketInit/Build): +0x8 = U of the newest particle (build
                // scrolls it every frame), +0x18 = step per particle (segmentDeltaU / rate); U_i = U0 + step (n - 1 - i), outside
                // 0..1 (the GS repeats). Verified in photo 2026-10-18_025733/001 (fx 16: -0.580 + 8/64 at particle 60 of 69).
                const uint32_t posA = ram.u32(b + fxoff::pos);
                const bool vuU = s.type == FX_STRIPE && ram.ok(posA - 0x28, 0x28);
                const float u0 = vuU ? ram.f32(posA - 0x20) : 0.f, du = vuU ? ram.f32(posA - 0x10) : 0.f;
                for (int i = 0; i < s.n; ++i) {
                    float q[4];
                    P(i, q);
                    if (!std::isfinite(q[0])) continue;
                    const float u = vuU ? u0 + du * float(s.n - 1 - i) : float(i) / float(std::max(s.n - 1, 1));
                    pts.push_back({{q[0], q[1], q[2]}, std::max(std::fabs(q[3]) - wHead, 0.f), u, i});
                }
            }
            auto scr = [&](const float *q, float &x, float &y) {
                const float z = view.depth[0] * q[0] + view.depth[1] * q[1] + view.depth[2] * q[2] + view.depth[3];
                const float iz = 1.f / std::max(z, 1e-3f);
                x = (view.ox[0] * q[0] + view.ox[1] * q[1] + view.ox[2] * q[2] + view.ox[3]) * iz;
                y = (view.oy[0] * q[0] + view.oy[1] * q[1] + view.oy[2] * q[2] + view.oy[3]) * iz;
            };
            auto off = [&](size_t i, float o[3]) {
                if (s.type == FX_STRIPE) {
                    // The stripe microprogram's joint (0x1708, routine 0x1F70/0x2118), in the screen plane with y corrected by the aspect
                    // (the same as the view's x/z, y/z): d1 = current - previous, d2 = next - current (at the ends, the missing neighbor is the
                    // reflection 2*end - neighbor), u = d/|d|, p(u) = (u.y, -u.x) and, on the side of the bend (sign of d1 x d2),
                    //   v = p(u1) + p(u2) + (u1 - u2)   if d1.x*d2.y - d2.x*d1.y >= 0, else   p(u1) + p(u2) - (u1 - u2)
                    // and the end opens v * half-width / 2 (on a straight line, |v| = 2). On a tight bend the end opens almost along the
                    // path; the native side used the perpendicular to (next - previous), and ExploSmokeTrail's zigzag trails
                    // came out offset by up to ~16 px (VU1 vs native check: 88% of quads matched).
                    // The ends' ghost (2 * end - neighbor) is built in homogeneous coordinates, before the divide by W: since the
                    // projection is affine before it, that is the world reflection, projected (reflecting on screen changed the end's direction and width).
                    const size_t last = pts.size() - 1;
                    if (last == 0) return false;
                    float xc, yc, xp, yp, xn, yn;
                    scr(pts[i].p, xc, yc);
                    auto ghost = [&](size_t end, size_t nb, float &gx, float &gy) {
                        float g[3];
                        for (int a = 0; a < 3; ++a) g[a] = 2.f * pts[end].p[a] - pts[nb].p[a];
                        scr(g, gx, gy);
                    };
                    if (i > 0) scr(pts[i - 1].p, xp, yp); else ghost(0, 1, xp, yp);
                    if (i < last) scr(pts[i + 1].p, xn, yn); else ghost(last, last - 1, xn, yn);
                    const float d1x = xc - xp, d1y = yc - yp, d2x = xn - xc, d2y = yn - yc;
                    const float l1 = std::sqrt(d1x * d1x + d1y * d1y), l2 = std::sqrt(d2x * d2x + d2y * d2y);
                    if (!(l1 > 1e-9f) || !(l2 > 1e-9f)) return false;
                    const float u1x = d1x / l1, u1y = d1y / l1, u2x = d2x / l2, u2y = d2y / l2;
                    const float sg = (d1x * d2y - d2x * d1y) >= 0.f ? 1.f : -1.f;
                    const float vx = u1y + u2y + sg * (u1x - u2x), vy = -u1x - u2x + sg * (u1y - u2y);
                    // same orientation as the old formula on a straight line (corners 0/3 on one side, 1/2 on the other)
                    const float nx = -0.5f * vx * pts[i].hw, ny = -0.5f * vy * pts[i].hw;
                    for (int a = 0; a < 3; ++a) o[a] = view.right[a] * nx + view.up[a] * ny;
                    return true;
                }
                float xa, ya, xb, yb;
                scr(pts[i > 0 ? i - 1 : 0].p, xa, ya); scr(pts[std::min(i + 1, pts.size() - 1)].p, xb, yb);
                float nx = -(yb - ya), ny = xb - xa;
                const float l = std::sqrt(nx * nx + ny * ny);
                if (!(l > 1e-9f)) return false;
                nx *= pts[i].hw / l; ny *= pts[i].hw / l;
                for (int a = 0; a < 3; ++a) o[a] = view.right[a] * nx + view.up[a] * ny;
                return true;
            };
            for (size_t i = 0; i + 1 < pts.size(); ++i) {
                const Pt &a0 = pts[i], &a1 = pts[i + 1];
                if (a1.seg != a0.seg + 1) continue;   // an invalid particle in the middle: no bridge
                float o0[3], o1[3];
                if (!off(i, o0) || !off(i + 1, o1)) continue;
                for (int a = 0; a < 3; ++a) {
                    c[0][a] = a0.p[a] - o0[a]; c[1][a] = a0.p[a] + o0[a];
                    c[2][a] = a1.p[a] + o1[a]; c[3][a] = a1.p[a] - o1[a];
                }
                uv[0][0] = a0.u; uv[0][1] = 0.f; uv[1][0] = a0.u; uv[1][1] = 1.f;
                uv[2][0] = a1.u; uv[2][1] = 1.f; uv[3][0] = a1.u; uv[3][1] = 0.f;
                f(a0.seg, c, uv);
            }
            return;
        }
        if (s.type == FX_PEDS) {
            // Pedestrians (pedsCreate/stampPacketInit, PedGroup): an upright stamp along world Z, base at pos, width info.minSize along
            // the camera's horizontal and height info.maxSize (measured by the VU1 vs native check: the on-screen tilt follows the perspective
            // of a vertical axis and the center sits half a height above pos). pos.w is not a size: it is float(color variant) (scatter)
            // with the low half-word replaced by the animation frame (update). The sheet has the frames along U (texMask + 1) and the
            // variants along V (vis.numColors), du/dv from TextureInfo in 1/16 texel.
            const float W = ram.f32(b + fxoff::minSize), H = ram.f32(b + fxoff::maxSize);
            float rh[3] = {view.right[0], view.right[1], 0.f};
            const float rl = std::sqrt(rh[0] * rh[0] + rh[1] * rh[1]);
            if (rl < 1e-6f) return;
            rh[0] /= rl; rh[1] /= rl;
            const uint32_t tinfo = fxaddr::textureInfo + 8 * (ram.u32(b + fxoff::texInfoIndex) & 127);
            const uint32_t k = ram.u16(tinfo + 2) & 127;
            const uint64_t t0 = uint64_t(ram.u32(fxaddr::tex0 + 8 * k)) | (uint64_t(ram.u32(fxaddr::tex0 + 8 * k + 4)) << 32);
            const float texW = float(1u << ((t0 >> 26) & 15)), texH = float(1u << ((t0 >> 30) & 15));
            const uint32_t frames = ram.u32(b + fxoff::texMask) + 1, rows = std::max<uint32_t>(ram.u32(b + fxoff::numColors), 1);
            const float du = float(ram.u16(tinfo + 4) >> 4) / float(frames) / texW, dv = float(ram.u16(tinfo + 6) >> 4) / float(rows) / texH;
            for (int i = 0; i < s.n; ++i) {
                float p[4];
                P(i, p);
                uint32_t wbits;
                std::memcpy(&wbits, &p[3], 4);
                if (int32_t(wbits) < 0 || !std::isfinite(p[0])) continue;   // pedsTurnOff: w = -1
                const uint32_t vbits = wbits & 0xFFFF0000u;
                float variant;
                std::memcpy(&variant, &vbits, 4);
                const float u0 = float((wbits & 0xFFFF) % frames) * du, v0 = float(uint32_t(std::max(variant, 0.f)) % rows) * dv;
                for (int a = 0; a < 3; ++a) {
                    c[0][a] = p[a] - rh[a] * W * 0.5f; c[1][a] = p[a] + rh[a] * W * 0.5f;
                    c[2][a] = c[1][a]; c[3][a] = c[0][a];
                }
                c[2][2] += H; c[3][2] += H;
                // VU1 (prog 0x1373): V grows upward (feet at v0, head at v0 + dv; the sheet stores the figures upside
                // down), with stampPacketInit's half-texel inset (0.5/(du>>4); here the region is the whole texture)
                const float hu = 0.5f / texW, hv = 0.5f / texH;
                uv[0][0] = u0 + hu;      uv[0][1] = v0 + hv;
                uv[1][0] = u0 + du - hu; uv[1][1] = v0 + hv;
                uv[2][0] = u0 + du - hu; uv[2][1] = v0 + dv - hv;
                uv[3][0] = u0 + hu;      uv[3][1] = v0 + dv - hv;
                f(i, c, uv);
            }
            return;
        }
        // Sprites (sprite, texSprite, dustTrail, frag, kite, wake, strite): one quad per particle
        const bool dust = isDustTex(systemTex0(ram, b));
        for (int i = 0; i < s.n; ++i) {
            float p[4];
            P(i, p);
            if (!std::isfinite(p[0]) || !std::isfinite(p[3])) continue;
            // sprite (0x2C8) and texSprite (0xF18) microcode: on-screen half-side = vf27.xy * pos.w / W, with vf27.xy = half the focal length
            // (192; 90.24), capped by MINI at vf17.xy = vf27.xy / 2 (96; 45.12 px). In world space: half-side = pos.w / 2 and cap = depth
            // / 4 (with cap / 2 Magmo's explosion and WhiteExplosionFx came out up to twice the size; check: every VU1 quad
            // at 192 x 90.25 px). The other types that land here still use the whole pos.w and cap / 2 (not verified).
            const float depth = view.depth[0] * p[0] + view.depth[1] * p[1] + view.depth[2] * p[2] + view.depth[3];
            if (s.type == FX_WAKE) {   // microprogram 0x3470 (wakeVu): the triangle and the packet's ST; the quad degenerates (corner 3 = corner 0)
                const WakeVu wv = wakeVu(ram, b, i, depth);
                if (!wv.visible) continue;
                for (int k = 0; k < 4; ++k) {
                    const int v = k < 3 ? k : 0;
                    for (int a = 0; a < 3; ++a) c[k][a] = wv.c[v][a];
                    uv[k][0] = wv.uv[v][0]; uv[k][1] = wv.uv[v][1];
                }
                f(i, c, uv);
                continue;
            }
            const bool spr = s.type == FX_SPRITE || s.type == FX_TEXSPRITE;
            float half = (spr ? 0.5f : 1.f) * std::fabs(p[3]), cap = spr ? 0.25f : 0.5f;
            uint32_t dustCell = 0;
            if (s.type == FX_DUSTTRAIL) {
                const DustTrailVu d = dustTrailVu(ram, b, i, p[3], depth);
                if (!d.visible) continue;
                half = d.half;
                dustCell = d.cell;
                cap = 0.375f;
            }
            const float r = depth > 0.f ? std::min(half, cap * depth) : half;
            float ax[3] = {view.right[0] * r, view.right[1] * r, view.right[2] * r}, ay[3] = {view.up[0] * r, view.up[1] * r, view.up[2] * r};
            float cpos[3] = {p[0], p[1], p[2]};
            if (s.type == FX_STRITE) {
                // strite microcode: head pos and tail pos - vel projected, and the strip stretched 2x on each side
                // (from pos + 2*vel to pos - 3*vel); the width comes from pos.w. Here in the screen plane, centered at pos - vel/2.
                float v[3];
                V(i, v);
                const float vd = v[0] * back[0] + v[1] * back[1] + v[2] * back[2];
                float vs[3] = {v[0] - back[0] * vd, v[1] - back[1] * vd, v[2] - back[2] * vd};
                const float vl = std::sqrt(vs[0] * vs[0] + vs[1] * vs[1] + vs[2] * vs[2]);
                if (vl > 1e-4f) {
                    for (int a = 0; a < 3; ++a) vs[a] /= vl;
                    const float side[3] = {vs[1] * back[2] - vs[2] * back[1], vs[2] * back[0] - vs[0] * back[2], vs[0] * back[1] - vs[1] * back[0]};
                    const float len = 2.5f * vl + r * 0.5f;
                    for (int a = 0; a < 3; ++a) { ax[a] = vs[a] * len; ay[a] = side[a] * r * 0.5f; cpos[a] = p[a] - 0.5f * v[a]; }
                }
            }
            // UV set: the low bits of pos.w index the corners spritePacketInit uploads (VU 0x168): 3 normal, 1 mirrors U,
            // 2 mirrors V, 0 both. With info.attribs & 0x2000 there are 16 sets (rotations too); here only the 4 mirrorings.
            uint32_t wbits;
            std::memcpy(&wbits, &p[3], 4);
            const bool fu = (wbits & 2) == 0, fv = (wbits & 1) == 0;
            float u0 = fu ? 1.f : 0.f, u1 = fu ? 0.f : 1.f, v0 = fv ? 1.f : 0.f, v1 = fv ? 0.f : 1.f;
            if (s.type == FX_DUSTTRAIL) {
                // dustTrailPacketInit: table of 4 cells (corners (9, 9) to (du/2 - 9, du/2 - 9) plus du/2 in U by cell & 1 and in V
                // by cell >> 1, in 1/16 texel; du from TextureInfo on both axes); the VU picks by byte 2 of the age word & 3
                const uint32_t tinfo = fxaddr::textureInfo + 8 * (ram.u32(b + fxoff::texInfoIndex) & 127);
                const uint64_t t0 = systemTex0(ram, b);
                const float du = float(ram.u16(tinfo + 4)), tw = 16.f * float(1u << ((t0 >> 26) & 15)), th = 16.f * float(1u << ((t0 >> 30) & 15));
                const float h = std::floor(du * 0.5f);   // cell 0: 9 to h - 9; cell with the bit: h + 9 to du - 9
                u0 = ((dustCell & 1) ? h + 9.f : 9.f) / tw; u1 = ((dustCell & 1) ? du - 9.f : h - 9.f) / tw;
                v0 = ((dustCell >> 1) ? h + 9.f : 9.f) / th; v1 = ((dustCell >> 1) ? du - 9.f : h - 9.f) / th;
            } else if (dust) {   // cell wbits & 3 of the composite texture: (9, 9, 503, 503) + 512 * (k & 1, k >> 1), in 1/16 texel of 64
                const float cu = float(wbits & 1) * 0.5f, cv = float((wbits >> 1) & 1) * 0.5f;
                u0 = cu + 9.f / 1024.f; u1 = cu + 503.f / 1024.f; v0 = cv + 9.f / 1024.f; v1 = cv + 503.f / 1024.f;
            }
            const float sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
            for (int k = 0; k < 4; ++k)
                for (int a = 0; a < 3; ++a) c[k][a] = cpos[a] + ax[a] * sx[k] + ay[a] * sy[k];
            // VU1 sends strite and wake as ONE triangle per particle (VU1 vs native check); here the quad degenerates (corner 3 = corner 0).
            uv[0][0] = u0; uv[0][1] = v1; uv[1][0] = u1; uv[1][1] = v1;
            uv[2][0] = u1; uv[2][1] = v0; uv[3][0] = u0; uv[3][1] = v0;
            if (s.type == FX_STRITE) {
                float v[3];
                V(i, v);
                if (striteTriangle(ram, b, p, v, c)) {
                    // The microprogram's ST (check, photo 2026-10-17_200206): tip (0.5; 1.5), base (1.366; 0) and (-0.366; 0). The equilateral
                    // triangle circumscribes the texture square: the disc is inscribed (radius 0.5) and the rest falls outside 0..1 (particle textures use CLAMP)
                    uv[0][0] = uv[3][0] = 0.5f; uv[0][1] = uv[3][1] = 1.5f;
                    uv[1][0] = 1.3660254f; uv[1][1] = 0.f;
                    uv[2][0] = -0.3660254f; uv[2][1] = 0.f;
                } else {   // without the game camera: tip between corners 0 and 3 (old approximation)
                    for (int a = 0; a < 3; ++a) c[0][a] = c[3][a] = cpos[a] - ax[a];
                }
            }
            f(i, c, uv);
        }
    }

private:
    unsigned blob_ = 0;   // fallback texture (soft disc) when the system's texture doesn't decode
    std::unordered_map<uint32_t, float> flarePrev_;   // lens flare debounce: visible fraction from the previous frame, per ID

    unsigned blobTexture() {
        if (blob_) return blob_;
        const int n = 64;
        std::vector<uint32_t> px(size_t(n * n));
        for (int y = 0; y < n; ++y)
            for (int x = 0; x < n; ++x) {
                const float dx = (x + 0.5f) / n * 2.f - 1.f, dy = (y + 0.5f) / n * 2.f - 1.f;
                const float r = std::sqrt(dx * dx + dy * dy);
                const float a = std::max(0.f, 1.f - r);
                px[size_t(y * n + x)] = 0x00FFFFFFu | (uint32_t(a * a * 128.f) << 24);   // GS alpha: 0x80 = 1.0
            }
        Image img{px.data(), n, n, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        const Texture2D t = LoadTextureFromImage(img);
        if (t.id) { SetTextureFilter(t, TEXTURE_FILTER_BILINEAR); SetTextureWrap(t, TEXTURE_WRAP_CLAMP); }   // CLAMP like the game's (strite uses UV outside 0..1)
        blob_ = t.id;
        return blob_;
    }

    // The system's GS ALPHA register (vis.alphaContext; gsBlend). 0 = opaque system (drawn first, see draw): keeps the common blend.
    static uint8_t blendFor(uint32_t alpha) { return alpha ? uint8_t(alpha & 0xFF) : kGsAlphaNormal; }

    // Raw GS RGBAQ (0x80 = 1.0): the shader's GS mode (shaderNormal) does texel * Cv / 128 and At * Av / 16384
    static void vcolor(uint32_t c) { rlColor4ub(uint8_t(c & 255), uint8_t((c >> 8) & 255), uint8_t((c >> 16) & 255), uint8_t(c >> 24)); }

    // texSprite texture frame (texSpritePacketBuild)
    static uint32_t texFrame(const Ram &ram, uint32_t b, int i, int age) {
        const uint32_t frames = ram.u32(b + fxoff::texMask), attr = ram.u32(b + fxoff::infoAttribs);
        if (frames == 0) return 0;
        if (attr & 1) return uint32_t(i * 2654435761u >> 16) % frames;   // retail: rand() every frame; here fixed per particle
        if (attr & 2) return (ram.u8(b + fxoff::uvI + uint32_t(i)) >> 2) % frames;
        const uint32_t f = (uint32_t(std::max(age, 0)) * ram.u32(b + fxoff::mapsPerFrame)) >> 12;
        if (attr & 4) return std::min(f, frames - 1);
        return f % frames;
    }

public:
    // Color (GS RGBAQ, 0x80 = 1.0) of quad i from forEachQuad as drawSystem draws it: col[0] on corners 0 and 1, col[1] on 2 and 3
    // (only strips change from one end to the other). Not for arc, chain and sprine (their color comes from arcQuads/sprineQuads).
    static void quadColor(const Ram &ram, const FxSystem &s, const FxView &view, int i, uint32_t col[2]) {
        const uint32_t b = s.base;
        const uint32_t pos = ram.u32(b + fxoff::pos), vel = ram.u32(b + fxoff::vel), age = ram.u32(b + fxoff::age);
        const bool haveVel = ram.ok(vel, 16 * uint32_t(s.n)), haveAge = ram.ok(age, 2 * uint32_t(s.n));
        auto A = [&](int k) { return haveAge ? int(ram.s16(age + 2 * uint32_t(k))) : 0; };
        // pedestrians: vis.colorIndex is the RGBA color itself (stampPacketInit uploads it byte by byte); numColors counts the sheet's variants
        uint32_t c0 = s.type == FX_PEDS ? ram.u32(b + fxoff::colorIndex) : colorAt(ram, b, A(i));
        if (s.type == FX_DUSTTRAIL) {   // the microprogram's color and alpha (dustTrailVu): the age word, not the color table
            float p[3];
            for (int a = 0; a < 3; ++a) p[a] = ram.f32(pos + 16 * uint32_t(i) + 4 * uint32_t(a));
            const float W = view.depth[0] * p[0] + view.depth[1] * p[1] + view.depth[2] * p[2] + view.depth[3];
            c0 = dustTrailVu(ram, b, i, ram.f32(pos + 16 * uint32_t(i) + 12), W).rgba;
        } else if (s.type == FX_WAKE) {   // fixed color from the packet and alpha from the microprogram's curve (wakeVu)
            float p[3];
            for (int a = 0; a < 3; ++a) p[a] = ram.f32(pos + 16 * uint32_t(i) + 4 * uint32_t(a));
            c0 = wakeVu(ram, b, i, view.depth[0] * p[0] + view.depth[1] * p[1] + view.depth[2] * p[2] + view.depth[3]).rgba;
        } else if (s.type == FX_STRITE && haveVel) {
            // color by position vel.w in the table (microprogram 0xA88: index FTOI0(vel.w), lerp, + 0.5; vel.w is the age with per-particle
            // spread, not age * colorsPerFrame) and alpha scaled by the trail length (striteTriangle, alphaK)
            float p[4], v[4], tri[4][3], k = 1.f;
            for (int a = 0; a < 4; ++a) p[a] = ram.f32(pos + 16 * uint32_t(i) + 4 * uint32_t(a));
            for (int a = 0; a < 4; ++a) v[a] = ram.f32(vel + 16 * uint32_t(i) + 4 * uint32_t(a));
            const uint32_t nc = ram.u32(b + fxoff::numColors), tab = fxaddr::colors + 4 * ram.u32(b + fxoff::colorIndex);
            float cf[4] = {128.5f, 128.5f, 128.5f, 128.5f};
            if (nc > 0 && std::isfinite(v[3]) && v[3] >= 0.f) {
                const int k0 = std::min(int(v[3]), int(nc) - 1), k1 = std::min(k0 + 1, int(nc) - 1);   // only numColors entries in the packet
                const float fr = std::min(v[3] - float(k0), 1.f);
                const uint32_t e0 = ram.u32(tab + 4 * uint32_t(k0)), e1 = ram.u32(tab + 4 * uint32_t(k1));
                for (int a = 0; a < 4; ++a) cf[a] = float((e0 >> (8 * a)) & 255) * (1.f - fr) + float((e1 >> (8 * a)) & 255) * fr + 0.5f;
            }
            if (!striteTriangle(ram, b, p, v, tri, &k)) k = 1.f;
            auto ub = [](float x) { return uint32_t(std::clamp(x, 0.f, 255.f)); };   // FTOI0 truncates
            c0 = ub(cf[0]) | ub(cf[1]) << 8 | ub(cf[2]) << 16 | ub(cf[3] * k) << 24;
        }
        col[0] = c0;
        col[1] = isStrip(s.type) ? colorAt(ram, b, A(i + 1)) : c0;
        // stripePacketInit: with info.attribs & 0x200 the RGB of every color in the packet comes from the info.useThisColor vector (the stage's
        // trailColoring on the dust trail) and alpha is the table's times the vector's w. Only stripe (frag has its own version in Scene).
        if (s.type == FX_STRIPE && (ram.u32(b + fxoff::infoAttribs) & 0x200)) {
            const uint32_t cp = ram.u32(b + fxoff::useThisColor);
            if (ram.ok(cp, 16)) {
                auto ub = [](float v) { return uint32_t(std::clamp(v + 0.5f, 0.f, 255.f)); };   // lerp of equal values + 0.5, FTOI0
                const uint32_t rgb = ub(ram.f32(cp)) | ub(ram.f32(cp + 4)) << 8 | ub(ram.f32(cp + 8)) << 16;
                const float w = ram.f32(cp + 12);
                for (int k = 0; k < 2; ++k) col[k] = rgb | ub(float(col[k] >> 24) * w - 0.5f) << 24;
            }
        }
    }

    // forEachQuad with the color drawing uses on each quad: emit(i, corners, uv, color of corners 0 and 1, color of corners 2 and 3).
    // Same geometry and same culling as forEachQuad (arc and chain only with both triangles). Used by the VU1 check.
    template <class F> static void forEachQuadColored(const Ram &ram, const FxSystem &s, const FxView &view, F &&emit) {
        if (s.type == FX_LINE) return;
        if (s.type == FX_ARC || s.type == FX_ARC2 || s.type == FX_CHAIN) {
            arcQuads(ram, s, view, [&](int k, const float c[4][3], const float uv[4][2], const uint32_t *col, int mask) {
                if (mask == 3 && !fogged(view, c)) emit(k, c, uv, col[0], col[1]);
            });
            return;
        }
        if (s.type == FX_SPRINE) {
            sprineQuads(ram, s, view, [&](int k, const float c[4][3], const float uv[4][2], uint32_t col) { if (!fogged(view, c)) emit(k, c, uv, col, col); });
            return;
        }
        forEachQuad(ram, s, view, [&](int i, const float c[4][3], const float uv[4][2]) {
            uint32_t col[2];
            quadColor(ram, s, view, i, col);
            emit(i, c, uv, col[0], col[1]);
        });
    }

private:
    // Synthetic system (synth): drawSystem/forEachQuad's sprite path with the particles from here
    void drawSynth(const Ram &ram, const SynthSystem &s, const TexFn &texture, const FxView &view) {
        if (s.sprites.empty()) return;
        const uint32_t tinfo = fxaddr::textureInfo + 8 * (s.texInfo & 127);
        const uint32_t k = ram.u16(tinfo + 2) & 127;
        const uint32_t id = ram.u16(fxaddr::texId + 2 * k);
        const uint64_t t0 = uint64_t(ram.u32(fxaddr::tex0 + 8 * k)) | (uint64_t(ram.u32(fxaddr::tex0 + 8 * k + 4)) << 32);
        unsigned tex = (ram.s16(tinfo) > 0 && id) ? texture(id, t0) : 0;
        if (!tex) tex = blobTexture();
        rlDrawRenderBatchActive();
        const uint8_t blend = blendFor(s.alphaContext);
        gsBlend(blend);
        shaderNormal(0.f, true, true, gsBlendPremul(blend));
        rlBegin(RL_QUADS);
        rlSetTexture(tex);
        for (const SynthSprite &q : s.sprites) {
            const float depth = view.depth[0] * q.p[0] + view.depth[1] * q.p[1] + view.depth[2] * q.p[2] + view.depth[3];
            const float half = 0.5f * std::fabs(q.size), r = depth > 0.f ? std::min(half, 0.25f * depth) : half;
            float c[4][3];
            const float sx[4] = {-1, 1, 1, -1}, sy[4] = {-1, -1, 1, 1};
            for (int k = 0; k < 4; ++k)
                for (int a = 0; a < 3; ++a) c[k][a] = q.p[a] + r * (view.right[a] * sx[k] + view.up[a] * sy[k]);
            if (fogged(view, c)) continue;
            const bool fu = (q.sel & 2) == 0, fv = (q.sel & 1) == 0;
            const float u0 = fu ? 1.f : 0.f, u1 = 1.f - u0, v0 = fv ? 1.f : 0.f, v1 = 1.f - v0;
            const float uv[4][2] = {{u0, v1}, {u1, v1}, {u1, v0}, {u0, v0}};
            for (int j = 0; j < 4; ++j) {
                vcolor(q.rgba);
                rlTexCoord2f(uv[j][0], uv[j][1]);
                rlVertex3f(c[j][0], c[j][1], c[j][2]);
            }
        }
        rlEnd();
        rlSetTexture(0);
    }

    void drawSystem(const Ram &ram, const FxSystem &s, const TexFn &texture, const FxView &view) {
        const uint32_t b = s.base;
        const uint32_t pos = ram.u32(b + fxoff::pos), vel = ram.u32(b + fxoff::vel), age = ram.u32(b + fxoff::age);
        const uint32_t ti = ram.u32(b + fxoff::texInfoIndex) & 127;
        const uint32_t tinfo = fxaddr::textureInfo + 8 * ti;
        const int16_t numTex = ram.s16(tinfo);
        const uint32_t texIndex = ram.u16(tinfo + 2);
        const bool haveVel = ram.ok(vel, 16 * uint32_t(s.n));
        const bool haveAge = ram.ok(age, 2 * uint32_t(s.n));

        auto texFor = [&](uint32_t frame) -> unsigned {
            const uint32_t k = (texIndex + frame) & 127;
            const uint32_t id = ram.u16(fxaddr::texId + 2 * k);
            const uint64_t t0 = uint64_t(ram.u32(fxaddr::tex0 + 8 * k)) | (uint64_t(ram.u32(fxaddr::tex0 + 8 * k + 4)) << 32);
            unsigned gl = (numTex > 0 && id) ? texture(id, t0) : isDustTex(t0) ? texture(kDustTexId, t0) : 0;
            return gl ? gl : blobTexture();
        };

        rlDrawRenderBatchActive();
        const uint8_t blend = blendFor(ram.u32(b + fxoff::alphaContext));
        gsBlend(blend);
        // fog in the Scene::draw shader: the particles' XGKICK carries PRIM.FGE on every type measured by the VU1 check (sprite,
        // texSprite, stripe, strite, arc, sprine, frag, pedestrian, dustTrail); GS alpha (textures with raw alpha); draw resets it at the end
        shaderNormal(0.f, true, true, gsBlendPremul(blend));
        auto A = [&](int i) { return haveAge ? int(ram.s16(age + 2 * uint32_t(i))) : 0; };

        if (s.type == FX_LINE) {
            shaderNormal(0.f, false, true, gsBlendPremul(blend));   // PRIM 0x49 without FGE: the fog is already in the alpha (lineVu)
            rlBegin(RL_LINES);
            rlSetTexture(gsWhiteTexture());   // no texture in GS mode: alpha 0x80
            for (int i = 0; i < s.n; ++i) {
                float h[3], t[3];
                uint32_t c;
                if (!lineVu(ram, b, view, i, h, t, c)) continue;
                vcolor(c);
                rlVertex3f(h[0], h[1], h[2]);
                vcolor(c & 0x00FFFFFFu);
                rlVertex3f(t[0], t[1], t[2]);
            }
            rlEnd();
            return;
        }

        if (s.type == FX_ARC || s.type == FX_ARC2 || s.type == FX_CHAIN) {   // ribbon with each point's color (arcQuads), as triangles along the strip's diagonal
            rlBegin(RL_TRIANGLES);   // rlBegin before rlSetTexture (see the pitfall in wotm_native.hpp)
            const unsigned tex = texFor(0);
            rlSetTexture(tex);
            // chain: U runs from segInitU - 0.15 (n - 1) to segInitU (outside 0..1) and the GS repeats ChainTex (the twisted rope of the
            // reference); particle textures use CLAMP, so REPEAT on S only while drawing this system
            const bool repS = s.type == FX_CHAIN;
            if (repS) rlTextureParameters(tex, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_REPEAT);
            arcQuads(ram, s, view, [&](int, const float c[4][3], const float uv[4][2], const uint32_t col[2], int mask) {
                if (fogged(view, c)) return;
                static const int tri[2][3] = {{0, 1, 3}, {1, 3, 2}};
                for (int t = 0; t < 2; ++t) {
                    if (!(mask & (1 << t))) continue;
                    for (int k : tri[t]) {
                        vcolor(k < 2 ? col[0] : col[1]);
                        rlTexCoord2f(uv[k][0], uv[k][1]);
                        rlVertex3f(c[k][0], c[k][1], c[k][2]);
                    }
                }
            });
            rlEnd();
            if (repS) { rlDrawRenderBatchActive(); rlTextureParameters(tex, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_CLAMP); }
            rlSetTexture(0);
            return;
        }

        if (s.type == FX_SPRINE) {   // the curve's sprites with each step's color (sprineQuads)
            rlBegin(RL_QUADS);
            rlSetTexture(texFor(0));
            sprineQuads(ram, s, view, [&](int, const float c[4][3], const float uv[4][2], uint32_t col) {
                if (fogged(view, c)) return;
                for (int k = 0; k < 4; ++k) {
                    vcolor(col);
                    rlTexCoord2f(uv[k][0], uv[k][1]);
                    rlVertex3f(c[k][0], c[k][1], c[k][2]);
                }
            });
            rlEnd();
            rlSetTexture(0);
            return;
        }

        // strips: one texture and a color per end; sprites: texture (texSprite frame) and color per particle
        const bool strip = isStrip(s.type), perFrame = s.type == FX_TEXSPRITE;
        const unsigned stripTex = strip ? texFor(0) : 0;
        const bool repS = s.type == FX_STRIPE && stripTex != 0;   // U outside 0..1 (packet header): REPEAT on S only while drawing this system
        if (repS) rlTextureParameters(stripTex, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_REPEAT);
        unsigned cur = ~0u;
        bool open = false;
        forEachQuad(ram, s, view, [&](int i, const float c[4][3], const float uv[4][2]) {
            const int ag = A(i);
            const unsigned tex = strip ? stripTex : texFor(perFrame ? texFrame(ram, b, i, ag) : 0);
            if (tex != cur || !open) {
                if (open) { rlEnd(); rlDrawRenderBatchActive(); }
                rlBegin(RL_QUADS);   // rlBegin before rlSetTexture (see the pitfall in wotm_native.hpp)
                rlSetTexture(tex);
                cur = tex;
                open = true;
            }
            uint32_t col[2];
            quadColor(ram, s, view, i, col);
            for (int k = 0; k < 4; ++k) {
                vcolor(col[k < 2 ? 0 : 1]);
                rlTexCoord2f(uv[k][0], uv[k][1]);
                rlVertex3f(c[k][0], c[k][1], c[k][2]);
            }
        });
        if (open) rlEnd();
        if (repS) { rlDrawRenderBatchActive(); rlTextureParameters(stripTex, RL_TEXTURE_WRAP_S, RL_TEXTURE_WRAP_CLAMP); }
        rlSetTexture(0);
    }
};

}  // namespace wotm
