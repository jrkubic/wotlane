// Autosave icon (NATIVO.md, "Save in JSON"): the 3D film reel the game writes as its memory card icon
// (MEMCRD/ICON.ICO on the disc) spins in the bottom-right corner for 1 s on every write of save.json (wotm_save.inc bumps
// g_wotmSaveCount on the EE thread; the window draws it here, from inside wotmDrawScene).
//
// ICON.ICO format (PS2 save icon, checked against this file): 20-byte header (0x00010000, number of animation shapes,
// texture type, 1.0f, vertex count); per vertex, the position in each shape (4 x s16, 1.0 = 4096, y pointing down), the
// normal (4 x s16), u and v (2 x s16, 0..4096) and the color (RGBA, 0x80 = 1.0); triangles in sequence. Then the animation (20 bytes and,
// per frame, shape + key count + 8-byte keys) and the 128x128 texture in 1555 (bit 3 of the type: compressed, u32 size and
// pairs of u16: n < 0x8000 repeats the next value n times, n >= 0x8000 copies 0x10000 - n values). This one has a single shape.
#pragma once
#include "wotm_disc.hpp"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstdlib>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

#include "raylib.h"
#include "rlgl.h"

extern std::atomic<uint32_t> g_wotmSaveCount;   // wotm_save.inc (wotm_overrides.cpp)

namespace wotm::saveicon {

struct Vtx {
    float x, y, z, u, v;
    uint8_t c[4];
};

struct Model {
    std::vector<Vtx> v;
    std::vector<uint32_t> rgba;   // 128 x 128
    float center[3] = {0, 0, 0}, radius = 1.0f;
};

inline bool parse(const std::vector<uint8_t> &d, Model &m) {
    auto u32 = [&](size_t o) { uint32_t x; std::memcpy(&x, &d[o], 4); return x; };
    auto s16 = [&](size_t o) { int16_t x; std::memcpy(&x, &d[o], 2); return x; };
    auto u16 = [&](size_t o) { uint16_t x; std::memcpy(&x, &d[o], 2); return x; };
    if (d.size() < 20 || u32(0) != 0x00010000u) return false;
    const uint32_t shapes = u32(4), texType = u32(8), nv = u32(16);
    if (shapes == 0 || shapes > 16 || nv == 0 || nv > 100000) return false;
    const size_t stride = 8 * size_t(shapes) + 16;
    size_t o = 20;
    if (o + stride * nv > d.size()) return false;
    m.v.resize(nv);
    float lo[3] = {1e9f, 1e9f, 1e9f}, hi[3] = {-1e9f, -1e9f, -1e9f};
    for (uint32_t i = 0; i < nv; ++i, o += stride) {
        Vtx &t = m.v[i];
        t.x = s16(o) / 4096.0f;
        t.y = -s16(o + 2) / 4096.0f;   // the icon's y points down: rotate 180 degrees around x (y and z flip sign), without mirroring
        t.z = -s16(o + 4) / 4096.0f;
        const size_t uv = o + 8 * shapes + 8;
        t.u = s16(uv) / 4096.0f;
        t.v = s16(uv + 2) / 4096.0f;
        for (int k = 0; k < 4; ++k) t.c[k] = uint8_t(std::min(255, d[uv + 4 + k] * 2));   // 0x80 = 1.0
        const float p[3] = {t.x, t.y, t.z};
        for (int k = 0; k < 3; ++k) lo[k] = std::min(lo[k], p[k]), hi[k] = std::max(hi[k], p[k]);
    }
    for (int k = 0; k < 3; ++k) m.center[k] = (lo[k] + hi[k]) * 0.5f;
    m.radius = 0.5f * std::sqrt((hi[0] - lo[0]) * (hi[0] - lo[0]) + (hi[1] - lo[1]) * (hi[1] - lo[1]) + (hi[2] - lo[2]) * (hi[2] - lo[2]));
    // animation: 20 bytes, and per frame shape + keys
    if (o + 20 > d.size()) return false;
    const uint32_t frames = u32(o + 16);
    o += 20;
    for (uint32_t f = 0; f < frames && f < 256; ++f) {
        if (o + 8 > d.size()) return false;
        o += 8 + 8 * size_t(u32(o + 4));
    }
    std::vector<uint16_t> px;
    px.reserve(128 * 128);
    if (texType & 8) {
        if (o + 4 > d.size()) return false;
        const size_t end = std::min(d.size(), o + 4 + u32(o));
        o += 4;
        while (o + 2 <= end && px.size() < 128 * 128) {
            const uint16_t n = u16(o);
            o += 2;
            if (n & 0x8000) {
                for (uint32_t k = 0; k < 0x10000u - n && o + 2 <= end; ++k, o += 2) px.push_back(u16(o));
            } else if (o + 2 <= end) {
                px.insert(px.end(), n, u16(o));
                o += 2;
            }
        }
    } else {
        for (; o + 2 <= d.size() && px.size() < 128 * 128; o += 2) px.push_back(u16(o));
    }
    px.resize(128 * 128, 0);
    m.rgba.resize(128 * 128);
    for (size_t i = 0; i < px.size(); ++i) {
        const uint32_t c = px[i], r = (c & 31) << 3, g = ((c >> 5) & 31) << 3, b = ((c >> 10) & 31) << 3;
        m.rgba[i] = (r | r >> 5) | (g | g >> 5) << 8 | (b | b >> 5) << 16 | 0xFF000000u;
    }
    return true;
}

// Window thread. `discRoot`/`iso`: the disc; `cardDir`: the game's folder on the memory card (copy of the icon the game wrote), as a fallback.
class Presenter {
public:
    void present(const std::filesystem::path &discRoot, const std::string &iso, const std::filesystem::path &cardDir) {
        const uint32_t n = g_wotmSaveCount.load(std::memory_order_relaxed);
        const double now = GetTime();
        if (!init_) {
            init_ = true;
            seen_ = n;
            test_ = std::getenv("WOTM_SAVEICON_TEST") != nullptr;
        }
        if (n != seen_ || (test_ && now - t0_ > 3.0)) {
            seen_ = n;
            t0_ = now;
        }
        const double t = now - t0_;
        if (t0_ < 0.0 || t > kShow) return;
        if (!load(discRoot, iso, cardDir)) return;
        const float fade = float(std::min({1.0, t / kFade, (kShow - t) / kFade}));

        const int size = std::max(64, GetScreenHeight() / 7);
        if (rt_.id == 0 || rt_.texture.width != size) {
            if (rt_.id) UnloadRenderTexture(rt_);
            rt_ = LoadRenderTexture(size, size);
            SetTextureFilter(rt_.texture, TEXTURE_FILTER_BILINEAR);
        }
        BeginTextureMode(rt_);
        rlEnableDepthMask();   // the native scene may have left it off: without it the clear doesn't clear depth and the icon doesn't write it
        ClearBackground(BLANK);
        Camera3D cam{};
        const float dist = model_.radius * 3.2f;
        cam.position = {0.0f, model_.radius * 0.35f, dist};
        cam.target = {0.0f, 0.0f, 0.0f};
        cam.up = {0.0f, 1.0f, 0.0f};
        cam.fovy = 40.0f;
        cam.projection = CAMERA_PERSPECTIVE;
        const double nearWas = rlGetCullDistanceNear(), farWas = rlGetCullDistanceFar();
        rlSetClipPlanes(0.05, double(dist) * 4.0);
        BeginMode3D(cam);
        rlDisableBackfaceCulling();
        rlPushMatrix();
        rlRotatef(float(t * 300.0), 0.0f, 1.0f, 0.0f);   // almost one turn per second
        rlTranslatef(-model_.center[0], -model_.center[1], -model_.center[2]);
        rlBegin(RL_TRIANGLES);
        rlSetTexture(tex_.id);   // after rlBegin: an rlBegin with a new mode opens the draw with the default texture
        for (const Vtx &v : model_.v) {
            rlColor4ub(v.c[0], v.c[1], v.c[2], 255);
            rlTexCoord2f(v.u, v.v);
            rlVertex3f(v.x, v.y, v.z);
        }
        rlEnd();
        rlSetTexture(0);
        rlPopMatrix();
        rlEnableBackfaceCulling();
        EndMode3D();
        EndTextureMode();
        rlSetClipPlanes(nearWas, farWas);

        const float margin = GetScreenHeight() * 0.03f;
        const Rectangle src{0.0f, 0.0f, float(size), -float(size)};
        const Rectangle dst{GetScreenWidth() - margin - size, GetScreenHeight() - margin - size, float(size), float(size)};
        DrawTexturePro(rt_.texture, src, dst, Vector2{0.0f, 0.0f}, 0.0f, Fade(WHITE, fade));
    }

private:
    static constexpr double kShow = 1.0, kFade = 0.15;

    bool load(const std::filesystem::path &discRoot, const std::string &iso, const std::filesystem::path &cardDir) {
        if (tried_) return tex_.id != 0;
        tried_ = true;
        std::vector<uint8_t> d;
        const char *from = "disc";
        if (!wotm::disc::readFile(discRoot, iso, "MEMCRD/ICON.ICO", d)) {
            from = "memory card";
            if (!wotm::disc::readFile(cardDir, {}, "ICON.ICO", d)) d.clear();
        }
        if (d.empty() || !parse(d, model_)) {
            std::fprintf(stderr, "[wotm] save icon: MEMCRD/ICON.ICO %s; no icon\n", d.empty() ? "not found" : "in an unexpected format");
            return false;
        }
        Image img{model_.rgba.data(), 128, 128, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        tex_ = LoadTextureFromImage(img);
        SetTextureFilter(tex_, TEXTURE_FILTER_BILINEAR);
        std::fprintf(stderr, "[wotm] save icon: ICON.ICO from the %s, %zu vertices\n", from, model_.v.size());
        return tex_.id != 0;
    }

    Model model_;
    Texture2D tex_{};
    RenderTexture2D rt_{};
    bool tried_ = false, init_ = false, test_ = false;
    uint32_t seen_ = 0;
    double t0_ = -1.0;
};

inline Presenter &presenter() {
    static Presenter p;
    return p;
}

} // namespace wotm::saveicon
