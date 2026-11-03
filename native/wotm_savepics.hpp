// Images for the save list (NATIVO.md, "Save in JSON"): on the left of the modal, the wide picture of the save's level with the
// monster's logo on top of it, as in the menu's level and monster selection. They come from SHELL/SHELL.TEX and SHELL/SHELL.RTX on
// the disc (the same pair the menu loads; after a fight it is no longer in memory, so it is read straight from the disc).
//
// - Logos (256x128, T8): texIds 121..129 and 52 (Raptros). Wide pictures (512x256): two 256x256 T8 tiles side by side; each level
//   also has a square picture in the tile just before them, which is not used here.
// - Palettes: 16x16 CT32 entries in the .RTX, allocated in texture order (4 blocks per T8, 1 per T4); the "tbp" in the tables below
//   is the entry's tbp in the file. Verified with tools/tex.py: the palettes in the file that an .NGP binds follow the same order;
//   the ones used here are not bound by any .NGP (the menu switches them in code).
// - Texture rows are stored bottom to top (the game draws with V flipped).
//
// The modal (wotm_save.inc, EE thread) posts the list every frame (post); the window draws it between the high-priority list (the
// McBackground frame) and the text (draw, in wotm_live.inc). With no recent post (page closed) nothing is drawn.
// Define WOTM_SAVEPICS_NO_DRAW before the include to get only the queue, without raylib (wotm_overrides.cpp).
#pragma once
#include <algorithm>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <string>
#include <vector>

namespace wotm::savepics {

enum Kind : uint8_t { kLevel = 0, kLogo = 1, kMonster = 2 };

// In game-screen coordinates (640 x 224, the window's 4:3 area)
struct Pic {
    Kind kind;
    uint32_t id;   // level (kLevels in wotm_save.inc), monster (sel >> 5) or, for kMonster, monster | costume << 8
    float x0, y0, x1, y1;
    uint8_t a;     // 255 = opaque
};

struct Tile {
    uint16_t texId, palTbp;
};
// Level -> wide picture (left, right tiles). Indices into kLevels (Shell::m_levelNum): 1 central .. 11 capitol, 14 minisf, 15 volcano
// (12, 13 and 16 have no picture in SHELL.TEX: level selection shows another level's picture for them; wotm_levelsel.inc).
struct LevelPic {
    uint32_t level;
    Tile l, r;
};
constexpr LevelPic kLevelPics[] = {
    {1, {99, 308}, {100, 312}},    // Midtown Park
    {2, {118, 384}, {119, 388}},   // Gamblers Gulch
    {3, {102, 320}, {103, 324}},   // Rosdale Canyon
    {4, {108, 344}, {109, 348}},   // Metro City
    {5, {93, 284}, {94, 288}},     // Century Airfield
    {6, {84, 248}, {85, 252}},     // Atomic Island
    {7, {111, 356}, {112, 360}},   // Baytown
    {8, {105, 332}, {106, 336}},   // Club Caldera
    {9, {114, 368}, {115, 372}},   // Tsunopolis
    {10, {88, 264}, {89, 268}},    // UFO
    {11, {96, 296}, {97, 300}},    // Capitol
    {14, {86, 256}, {87, 260}},    // Mini Baytown
    {15, {90, 272}, {91, 276}},    // Volcano
};
// Monster (index into kMonsters) -> logo
struct LogoPic {
    uint32_t monster;
    Tile t;
};
constexpr LogoPic kLogoPics[] = {
    {1, {122, 400}},   // Congar
    {2, {127, 420}},   // Robo-47
    {3, {129, 428}},   // Togera
    {4, {128, 424}},   // Ultra-V
    {5, {126, 416}},   // Preytor
    {7, {123, 404}},   // Kineticlops
    {8, {125, 412}},   // Magmo
    {9, {52, 156}},    // Raptros
    {10, {124, 408}},  // Agamo
    {11, {121, 396}},  // Zorgulon
};

inline bool hasLevel(uint32_t level) {
    for (const LevelPic &p : kLevelPics)
        if (p.level == level) return true;
    return false;
}
inline bool hasLogo(uint32_t monster) {
    for (const LogoPic &p : kLogoPics)
        if (p.monster == monster) return true;
    return false;
}
// Monster (index into kMonsters) -> file name prefix in MON/ (MONKEY0.NGP...: the digit is the costume, Shell::m_costume)
inline const char *monsterFile(uint32_t monster) {
    static const char *const k[] = {nullptr, "MONKEY", "ROBOT", "LIZARD", "SHOGUN", "MANTIS", nullptr, "ENERGY", "LAVA", "DRAGON", "ROCK", "ALIEN",
                                           nullptr, "ASSBOSS", "JELLY", "FINAL"};   // 13-15: bosses, custom monster bases (wotm_mods.hpp)
    return monster < std::size(k) ? k[monster] : nullptr;
}

// ---- queue from the modal (EE thread) to the window ----
struct Channel {
    std::mutex m;
    std::vector<Pic> pics;
    std::chrono::steady_clock::time_point at{};
};
inline Channel &channel() {
    static Channel c;
    return c;
}
inline void post(std::vector<Pic> pics) {
    Channel &c = channel();
    std::lock_guard<std::mutex> lk(c.m);
    c.pics = std::move(pics);
    c.at = std::chrono::steady_clock::now();
}
inline void clear() {
    Channel &c = channel();
    std::lock_guard<std::mutex> lk(c.m);
    c.pics.clear();
}

} // namespace wotm::savepics

#ifndef WOTM_SAVEPICS_NO_DRAW
#include "wotm_disc.hpp"
#include "wotm_model.hpp"   // 3D monster (kMonster)
#include "raylib.h"
#include <map>
#include <memory>

namespace wotm::savepics {

// ---- disc reading (window thread) ----
namespace detail {
// .TEX/.RTX are stored in a single-file zip with the magic "IE\3\4" (zipInflateAll)
inline bool unzipIe(std::vector<uint8_t> &d) { return model::unzipIe(d); }
struct Packet {
    size_t off;
    uint16_t texId, tbp;
    uint32_t psm, w, h;
};
// _texfile header {numSizes, headerQw, ...}; 16-byte packets {size, pad, texId, tbp, bits} (tools/tex.py, parse)
inline std::vector<Packet> packets(const std::vector<uint8_t> &d) {
    std::vector<Packet> out;
    if (d.size() < 8) return out;
    uint32_t headerQw = 0;
    std::memcpy(&headerQw, d.data() + 4, 4);
    size_t off = size_t(headerQw) * 16;
    while (off + 16 <= d.size() && out.size() < 4096) {
        uint32_t size, bits;
        uint16_t texId, tbp;
        std::memcpy(&size, d.data() + off, 4);
        std::memcpy(&texId, d.data() + off + 8, 2);
        std::memcpy(&tbp, d.data() + off + 10, 2);
        std::memcpy(&bits, d.data() + off + 12, 4);
        out.push_back(Packet{off, texId, tbp, bits & 0x3F, 1u << ((bits >> 8) & 15), 1u << ((bits >> 12) & 15)});
        if (size == 0) break;
        const size_t next = off + size_t(size >> 2) * 16;
        if (next <= off || next > d.size()) break;
        off = next;
    }
    return out;
}
} // namespace detail

class Drawer {
public:
    // Window thread, OUTSIDE the frame's BeginTextureMode (before it): renders the list's 3D monster (kMonster) into its own
    // RenderTexture, playing the idle animation from the moment the row was selected. Same arguments as draw.
    void prepare(const std::filesystem::path &discRoot, const std::string &iso, float w, float rh) {
        const Pic *mp = nullptr, *lp = nullptr;
        Pic pic{}, lev{};
        {
            Channel &c = channel();
            std::lock_guard<std::mutex> lk(c.m);
            if (!c.pics.empty() && std::chrono::steady_clock::now() - c.at <= std::chrono::milliseconds(250))
                for (const Pic &p : c.pics) {
                    if (p.kind == kMonster) { pic = p; mp = &pic; }
                    if (p.kind == kLevel) { lev = p; lp = &lev; }
                }
        }
        monReady_ = false;
        if (!mp) return;
        if (!model_ || monId_ != pic.id) {
            monId_ = pic.id;
            monT0_ = GetTime();
            model_.reset();
            const char *pre = monsterFile(pic.id & 0xFF);
            // costume (4+ = mod skin: MON/<PREFIX><n>.* comes from mods/, wotm_mods.hpp) and the fallback disc costume (the skin's base)
            const uint32_t costume = (pic.id >> 8) & 0xFF, base = (pic.id >> 16) & 3;
            if (pre) {
                auto m = std::make_unique<model::Model>();
                if (m->load(discRoot, iso, pre + std::to_string(costume)) || m->load(discRoot, iso, pre + std::to_string(base)) ||
                    m->load(discRoot, iso, std::string(pre) + "0"))
                    model_ = std::move(m), model_->computeFraming(model::kIdle);   // fixed box: the camera does not follow the idle animation
                else
                    std::fprintf(stderr, "[wotm] save list: MON/%s%u.NGP not found; no 3D monster\n", pre, costume);
            }
        }
        if (!model_) return;
        const float frame = float((GetTime() - monT0_) * 60.0);
        if (!model_->pose(model::kIdle, frame)) model_->pose(-1, 0.f);
        // render at 2x the on-window size (DrawTexturePro scales it down with bilinear filtering, which smooths the edges)
        const float sx = w / 640.f, sy = rh / 224.f;
        const int W = std::clamp(int((pic.x1 - pic.x0) * sx * 2.f), 16, 1536), H = std::clamp(int((pic.y1 - pic.y0) * sy * 2.f), 16, 1536);
        // Background: the part of the level picture under the monster's box, drawn opaque (the fade comes from the tint in draw), so
        // that GS blend modes acting on the destination (darken, subtract) have something to darken. Outside the picture the
        // background stays transparent.
        const Texture2D *lt = nullptr;
        if (lp && load(discRoot, iso))
            for (size_t i = 0; i < std::size(kLevelPics); ++i)
                if (kLevelPics[i].level == lp->id && level_[i].id) lt = &level_[i];
        const float kx = float(W) / (pic.x1 - pic.x0), ky = float(H) / (pic.y1 - pic.y0);
        if (!fxTried_) {   // Agamo's fire and lens flare: the level's particle textures (LVL/CENTRAL), loaded once
            fxTried_ = true;
            if (!fx_.load(discRoot, iso)) std::fprintf(stderr, "[wotm] save list: LVL/CENTRAL.TEX/.RTX not found; no fire or lens flare\n");
            renderer_.setFx(&fx_);
        }
        const Framing fr = framingOf((monId_ & 0xFF) | ((monId_ >> 16) & 3) << 8);   // framing of the disc costume (the base one)
        renderer_.shiftX = fr.shiftX;
        renderer_.shiftY = fr.shiftY;
        renderer_.fwd = fr.fwd;
        renderer_.render(*model_, W, H, kYaw, kPitch, fr.zoom, [&] {
            if (!lt) return;
            DrawTexturePro(*lt, Rectangle{0.f, 0.f, float(lt->width), float(lt->height)},
                           Rectangle{(lev.x0 - pic.x0) * kx, (lev.y0 - pic.y0) * ky, (lev.x1 - lev.x0) * kx, (lev.y1 - lev.y0) * ky},
                           Vector2{0.f, 0.f}, 0.f, WHITE);
        });
        monReady_ = true;
    }

    // Window thread, inside the native frame's BeginTextureMode. `x0`/`w`: the game screen's 4:3 area in pixels (gsScreenRect);
    // `rh`: frame height.
    void draw(const std::filesystem::path &discRoot, const std::string &iso, float x0, float w, float rh) {
        std::vector<Pic> pics;
        {
            Channel &c = channel();
            std::lock_guard<std::mutex> lk(c.m);
            if (c.pics.empty() || std::chrono::steady_clock::now() - c.at > std::chrono::milliseconds(250)) return;
            pics = c.pics;
        }
        if (!load(discRoot, iso)) return;
        const float sx = w / 640.f, sy = rh / 224.f;
        for (const Pic &p : pics) {
            const Texture2D *t = nullptr;
            if (p.kind == kMonster) {
                if (!monReady_) continue;
                // premultiplied color (Renderer): the fade scales color and alpha together
                const Texture2D &mt = renderer_.target().texture;
                BeginBlendMode(BLEND_ALPHA_PREMULTIPLY);
                DrawTexturePro(mt, Rectangle{0.f, 0.f, float(mt.width), -float(mt.height)},
                               Rectangle{x0 + p.x0 * sx, p.y0 * sy, (p.x1 - p.x0) * sx, (p.y1 - p.y0) * sy}, Vector2{0.f, 0.f}, 0.f,
                               Color{p.a, p.a, p.a, p.a});
                EndBlendMode();
                continue;
            }
            if (p.kind == kLevel) {
                for (size_t i = 0; i < std::size(kLevelPics); ++i)
                    if (kLevelPics[i].level == p.id) t = &level_[i];
            } else {
                for (size_t i = 0; i < std::size(kLogoPics); ++i)
                    if (kLogoPics[i].monster == p.id) t = &logo_[i];
            }
            if (!t || !t->id) continue;
            DrawTexturePro(*t, Rectangle{0.f, 0.f, float(t->width), float(t->height)},
                           Rectangle{x0 + p.x0 * sx, p.y0 * sy, (p.x1 - p.x0) * sx, (p.y1 - p.y0) * sy}, Vector2{0.f, 0.f}, 0.f,
                           Color{255, 255, 255, p.a});
        }
    }

    // Wide level picture (512x256, top to bottom) or nullptr; used for the RANDOM mosaic in level selection (wotm_levelsel.inc)
    const Texture2D *levelTexture(const std::filesystem::path &discRoot, const std::string &iso, uint32_t level) {
        if (!load(discRoot, iso)) return nullptr;
        for (size_t i = 0; i < std::size(kLevelPics); ++i)
            if (kLevelPics[i].level == level && level_[i].id) return &level_[i];
        return nullptr;
    }

private:
    bool load(const std::filesystem::path &discRoot, const std::string &iso) {
        if (tried_) return ok_;
        tried_ = true;
        std::vector<uint8_t> tex, rtx;
        if (!disc::readFile(discRoot, iso, "SHELL/SHELL.TEX", tex) || !disc::readFile(discRoot, iso, "SHELL/SHELL.RTX", rtx) ||
            !detail::unzipIe(tex) || !detail::unzipIe(rtx)) {
            std::fprintf(stderr, "[wotm] save list: SHELL/SHELL.TEX or .RTX not found; no logos or level pictures\n");
            return false;
        }
        texPk_ = detail::packets(tex);
        rtxPk_ = detail::packets(rtx);
        int n = 0;
        for (size_t i = 0; i < std::size(kLevelPics); ++i) {
            std::vector<uint32_t> px(512 * 256, 0);
            const bool a = tile(tex, rtx, kLevelPics[i].l, px, 512, 0), b = tile(tex, rtx, kLevelPics[i].r, px, 512, 256);
            if (a && b) level_[i] = upload(px, 512, 256), ++n;
        }
        for (size_t i = 0; i < std::size(kLogoPics); ++i) {
            std::vector<uint32_t> px(256 * 128, 0);
            if (tile(tex, rtx, kLogoPics[i].t, px, 256, 0)) logo_[i] = upload(px, 256, 128), ++n;
        }
        std::fprintf(stderr, "[wotm] save list: %d of %zu images from SHELL.TEX\n", n, std::size(kLevelPics) + std::size(kLogoPics));
        ok_ = n > 0;
        return ok_;
    }

    // Decodes one T8 tile with its .RTX palette into `px` (rows of `stride` pixels) starting at column `col`, flipped to top-to-bottom
    bool tile(const std::vector<uint8_t> &tex, const std::vector<uint8_t> &rtx, Tile t, std::vector<uint32_t> &px, uint32_t stride, uint32_t col) {
        const detail::Packet *p = nullptr, *pal = nullptr;
        for (const detail::Packet &k : texPk_)
            if (k.texId == t.texId) p = &k;
        for (const detail::Packet &k : rtxPk_)
            if (k.tbp == t.palTbp && k.psm == 0 && k.w == 16 && k.h == 16) pal = &k;
        if (!p || !pal || p->psm != 0x13 || p->off + 0x80 + size_t(p->w) * p->h > tex.size() || pal->off + 0x10 + 1024 > rtx.size())
            return false;
        uint32_t clut[256];
        for (uint32_t i = 0; i < 256; ++i) {
            const uint32_t j = (i & 0xE7) | ((i & 0x08) << 1) | ((i & 0x10) >> 1);   // CSM1: bits 3 and 4 swapped
            uint32_t c;
            std::memcpy(&c, rtx.data() + pal->off + 0x10 + 4 * j, 4);
            const uint32_t a = c >> 24;
            clut[i] = (c & 0xFFFFFFu) | (a >= 0x80 ? 255u : a * 2u) << 24;   // GS alpha: 0x80 = 1.0
        }
        const uint8_t *src = tex.data() + p->off + 0x80;
        for (uint32_t y = 0; y < p->h; ++y)
            for (uint32_t x = 0; x < p->w && col + x < stride; ++x)
                px[size_t(p->h - 1 - y) * stride + col + x] = clut[src[size_t(y) * p->w + x]];
        return true;
    }

    static Texture2D upload(std::vector<uint32_t> &px, int w, int h) {
        Image img{px.data(), w, h, 1, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8};
        Texture2D t = LoadTextureFromImage(img);
        SetTextureFilter(t, TEXTURE_FILTER_BILINEAR);
        SetTextureWrap(t, TEXTURE_WRAP_CLAMP);
        return t;
    }

    std::vector<detail::Packet> texPk_, rtxPk_;
    Texture2D level_[std::size(kLevelPics)]{}, logo_[std::size(kLogoPics)]{};
    bool tried_ = false, ok_ = false;

public:   // framing is also used by the 3- and 4-player monster selection (wotm_padsel.inc)
    // 3D monster: turned 3/4 toward the right of the screen (yaw 0 = facing the camera), camera slightly above
    static constexpr float kYaw = 35.f, kPitch = 8.f;
    // Per-monster, per-costume framing correction (costume 0-3; -1 = all). Automatic framing uses the box's largest dimension, so
    // Togera's tail (3) and Raptros' wings (9) left both of them too far away; Togera's tail also pulls the box center off its body:
    // `fwd` moves the center toward the monster's front (model::Renderer::fwd), which works at any angle and frame aspect ratio.
    // The fire on Magmo's costume 3 (the fourth, counting from 1) and on Agamo's head (10) enlarge the box: zoom in and shift up a
    // little so the feet don't get cut off at the bottom. shiftX/shiftY are fractions of the frame (+ = right/up).
    struct Framing {
        uint32_t mon;
        int costume;
        float zoom, shiftX, shiftY, fwd;   // fwd is 0 when an initializer leaves it out (aggregate init)
    };
    static constexpr Framing kFraming[] = {
        {3, -1, 1.35f, 0.f, 0.f, 0.18f},   // Togera
        {8, 3, 1.25f, 0.f, 0.08f},         // Magmo, fire costume
        {9, -1, 1.6f, 0.f, 0.f},           // Raptros
        {10, -1, 1.15f, 0.f, 0.04f},       // Agamo
    };
    static Framing framingOf(uint32_t id) {
        const uint32_t mon = id & 0xFF;
        const int costume = int((id >> 8) & 3);
        for (const Framing &f : kFraming)
            if (f.mon == mon && (f.costume < 0 || f.costume == costume)) return f;
        return Framing{mon, -1, 1.f, 0.f, 0.f};
    }

private:
    std::unique_ptr<model::Model> model_;
    model::Renderer renderer_;
    model::FxTextures fx_;
    bool fxTried_ = false;
    uint32_t monId_ = ~0u;
    double monT0_ = 0.0;
    bool monReady_ = false;
};

inline Drawer &drawer() {
    static Drawer d;
    return d;
}

} // namespace wotm::savepics
#endif
