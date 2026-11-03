// wotm_levelsel.inc: stage select (game screen 8, screenLevelSelect) with the stages the retail game doesn't list plus a RANDOM entry
// (included by wotm_live.inc after wotm_padsel.inc; global scope of ps2_runtime.cpp). NATIVO.md, "Stage selection".
//
// EE thread (screenLevelSelect hook): on entry, if g_levelList is one of the four retail lists (PO_1PFreeForAll,
// PO_MPFreeForAll, PO_1PEndurance, PO_MPElimination), it is repointed to a copy at the top of RAM (kList) holding the retail
// stages in retail order, then the missing ones from 1 to 16, and last RANDOM (kRandom, stage 17 "dylan", a dev-team test stage with
// no files: the picture switch, Shell+0x5D0, has 24 children and levelUnlocked leaves it open). Its name in screenLevelSelect's
// table (GameLevelLongNames_006EEEF8) becomes "Random". Everything else is the game's own code: cursor, lock, sounds, confirm.
// With WOTM_HIGHRAM=0, elimination (10 monsters loaded at once) keeps only the retail stages and RANDOM.
// On changeScreen(8, 99) (wotm_padsel.inc) RANDOM becomes a stage drawn at random from the list, among the unlocked ones (minus the
// fourth entry when the game skips it: mode 2 with 2+ CPUs, mode 3 with a CPU; minus Training and Tokyo Deathmatch, test stages),
// before the load and without the narrator announcing the stage (a surprise).
//
// Window thread (prepare, before the scene is drawn): for stages with no art in SHELL.TEX, the picture switch shows another stage's
// picture (12 Training: Midtown Park; 13 Lake Powell: half of Rosdale Canyon; 16 Tokyo Deathmatch and 17: half of Tsunopolis). The
// picture object of those children draws with its own texture (Scene::nodeTexture), in scene order: the big-screen video keeps
// playing on top, as on the other stages. Object with two textures (stage 12's): left and right halves of the picture. Pictures:
// levelpics/<name>.png (training, lakepowell, tokyodeathmatch; random), searched in mods/<mod>/levelpics/, levelpics/ next to the
// ELF and levelpics/ at the repo root. RANDOM with no PNG is a darkened mosaic of SHELL.TEX's stage pictures (savepics) with a "?".
// Other stages with no PNG keep the game's picture.
#include <filesystem>
#include <random>

namespace wotm_levelsel
{
namespace fs = std::filesystem;

constexpr int kRandom = 17;
constexpr uint32_t kShell = wotm::sym::shellInfo;
constexpr uint32_t kLevelNum = kShell + 0x2BB0, kNumAIs = kShell + 0x2BB8, kMode = kShell + 0x2C10, kLevelSw = kShell + 0x5D0;
constexpr uint32_t kList = PS2_RAM_SIZE - 0x300;   // LevelList {first, count, levels} and, at +0x10, up to 24 stages (the other top-of-RAM slots: -0x1000..-0x340)
constexpr uint32_t kListLevels = kList + 0x10;
constexpr uint32_t kLongNames = wotm::sym::GameLevelLongNames_006EEEF8;   // screenLevelSelect's char[28][18]
constexpr uint32_t kRetail[] = {wotm::sym::PO_1PFreeForAll, wotm::sym::PO_MPFreeForAll, wotm::sym::PO_1PEndurance, wotm::sym::PO_MPElimination};

static PS2Runtime::RecompiledFunction g_origLevelSelect = nullptr;

static uint32_t rd32(uint8_t *rdram, uint32_t a) { return FAST_READ32(a & 0x1FFFFFFFu); }
static void wr32(uint8_t *rdram, uint32_t a, uint32_t v) { FAST_WRITE32(a & 0x1FFFFFFFu, v); }

// The game's levelUnlocked: 10, 11, 14 and 15 by the unlock table (25 to 27 belong to the minigames, not in the lists)
static bool unlocked(uint8_t *rdram, int level)
{
    uint32_t off = 0;
    switch (level)
    {
    case 10: off = 0x768; break;
    case 11: off = 0x73C; break;
    case 14: off = 0x710; break;
    case 15: off = 0x6E4; break;
    default: return true;
    }
    return rd32(rdram, wotm::sym::monsterSelectMode + off) != 0;
}

// Extended copy of the retail list `src` at kList
static void extend(uint8_t *rdram, uint32_t src)
{
    const uint32_t first = rd32(rdram, src), count = std::min<uint32_t>(rd32(rdram, src + 4), 16u), levels = rd32(rdram, src + 8);
    std::vector<int> out;
    for (uint32_t i = 0; i < count; ++i)
        out.push_back(int(rd32(rdram, levels + 4 * i)));
    const char *h = std::getenv("WOTM_HIGHRAM");
    const bool low = h && h[0] == '0';
    if (!(src == wotm::sym::PO_MPElimination && low))
        for (int l = 1; l <= 16; ++l)
            if (std::find(out.begin(), out.end(), l) == out.end())
                out.push_back(l);
    out.push_back(kRandom);
    for (size_t i = 0; i < out.size(); ++i)
        wr32(rdram, kListLevels + 4 * uint32_t(i), uint32_t(out[i]));
    wr32(rdram, kList, first);
    wr32(rdram, kList + 4, uint32_t(out.size()));
    wr32(rdram, kList + 8, kListLevels);
    wr32(rdram, wotm::sym::g_levelList, kList);
    std::string s;
    for (int l : out)
        s += (s.empty() ? "" : " ") + std::to_string(l);
    std::fprintf(stderr, "[stages] list %x extended: %zu stages (%s)\n", src, out.size(), s.c_str());
}

static void levelSelectHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
{
    if (ctx->pc == wotm::fn::screenLevelSelect__Fv)
    {
        const uint32_t cur = rd32(rdram, wotm::sym::g_levelList);
        if (std::find(std::begin(kRetail), std::end(kRetail), cur) != std::end(kRetail))
            extend(rdram, cur);
        static const char kName[] = "Random";
        std::memcpy(rdram + ((kLongNames + 18u * kRandom) & 0x1FFFFFFFu), kName, sizeof kName);
    }
    g_origLevelSelect(rdram, ctx, runtime);
}

// changeScreen(8, 99): RANDOM becomes a randomly drawn stage (the game has already written m_levelNum). Called from wotm_padsel.inc,
// before the narrator; returns true when it drew one (the narrator doesn't announce the stage).
bool resolveRandom(uint8_t *rdram)
{
    if (int(rd32(rdram, kLevelNum)) != kRandom)
        return false;
    const uint32_t ll = rd32(rdram, wotm::sym::g_levelList);
    const uint32_t mode = rd32(rdram, kMode), ais = rd32(rdram, kNumAIs);
    const bool skip3 = (mode == 2 && ais >= 2) || (mode == 3 && ais > 0);   // screenLevelSelect skips the fourth entry
    std::vector<int> pool;
    if (ll == kList)
    {
        const uint32_t n = std::min<uint32_t>(rd32(rdram, kList + 4), 24u);
        for (uint32_t i = 0; i < n; ++i)
        {
            const int l = int(rd32(rdram, kListLevels + 4 * i));
            if (l != kRandom && l != 12 && l != 16 && !(skip3 && i == 3) && unlocked(rdram, l))   // 12 Training, 16 Tokyo Deathmatch: test stages
                pool.push_back(l);
        }
    }
    if (pool.empty())
        pool.push_back(1);
    static std::mt19937 rng{std::random_device{}()};
    const int pick = pool[std::uniform_int_distribution<size_t>(0, pool.size() - 1)(rng)];
    wr32(rdram, kLevelNum, uint32_t(pick));
    std::fprintf(stderr, "[stages] RANDOM: stage %d (of %zu)\n", pick, pool.size());
    return true;
}

// ------------------------------------------------------------------ window: own picture over the borrowed one

static const char *picName(int level)
{
    switch (level)
    {
    case 12: return "training";
    case 13: return "lakepowell";
    case 16: return "tokyodeathmatch";
    case kRandom: return "random";
    default: return nullptr;
    }
}

static fs::path findPic(const std::string &name)
{
    std::error_code ec;
    const std::string file = name + ".png";
    const std::string host = wotm::mods::Registry::get().hostPath("LEVELPICS/" + file);
    if (!host.empty())
        return host;
    const fs::path modsDir = wotm::mods::Registry::get().dir();
    const fs::path dirs[] = {modsDir.empty() ? fs::path{} : modsDir.parent_path() / "levelpics",
                             fs::path(__FILE__).parent_path().parent_path().parent_path() / "levelpics"};   // native/runtime -> repo root
    for (const fs::path &d : dirs)
        if (!d.empty() && fs::is_regular_file(d / file, ec))
            return d / file;
    return {};
}

struct Pic
{
    bool tried = false;
    Texture2D full{}, half[2]{};   // the whole picture and its left and right halves (object with two textures)
};
static Pic g_pics[24];

// Top-down image -> texture like the game's in Scene::draw: raw alpha (0x80 = opaque) and rows bottom to top (the game draws
// with V flipped; mod PNGs from tools/tex.py already come that way). Frees the image.
static Texture2D gameTexture(Image img)
{
    ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    ImageFlipVertical(&img);
    uint8_t *px = static_cast<uint8_t *>(img.data);
    for (size_t i = 0, n = size_t(img.width) * size_t(img.height); i < n; ++i)
        px[4 * i + 3] = 0xFF;   // like the SHELL pictures' palettes (up to 0xFF: drawing takes them above 1 and they come out brighter)
    Texture2D t = LoadTextureFromImage(img);
    UnloadImage(img);
    if (t.id)
    {
        GenTextureMipmaps(&t);
        SetTextureFilter(t, TEXTURE_FILTER_TRILINEAR);
        SetTextureWrap(t, TEXTURE_WRAP_REPEAT);   // the picture object's UV runs from -1 to 0 (the GS repeats the texture); with CLAMP it's a single color
    }
    return t;
}

// RANDOM mosaic when there's no PNG: 4 x 4 stage pictures from SHELL.TEX (savepics) in 1024x512, darkened, with a "?"
static Image mosaic()
{
    static const PS2Runtime::IoPaths &io = PS2Runtime::getIoPaths();
    std::vector<Image> tiles;
    for (uint32_t l = 1; l <= 16; ++l)
        if (const Texture2D *t = wotm::savepics::drawer().levelTexture(io.cdRoot, io.cdImage.string(), l))
            tiles.push_back(LoadImageFromTexture(*t));
    if (tiles.empty())
        return Image{};
    constexpr int W = 1024, H = 512, TW = 256, TH = 128;
    Image m = GenImageColor(W, H, BLACK);
    for (int i = 0; i < 16; ++i)
    {
        const Image &t = tiles[size_t(i * 5) % tiles.size()];   // repeats pictures when there are fewer than 16, without two identical ones side by side
        ImageDraw(&m, t, Rectangle{0.f, 0.f, float(t.width), float(t.height)}, Rectangle{float((i % 4) * TW), float((i / 4) * TH), float(TW), float(TH)},
                  WHITE);
    }
    for (Image &t : tiles)
        UnloadImage(t);
    ImageFormat(&m, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
    uint8_t *px = static_cast<uint8_t *>(m.data);
    for (size_t i = 0; i < size_t(W) * H; ++i)
        for (int c = 0; c < 3; ++c)
            px[4 * i + c] = uint8_t(px[4 * i + c] * 105 / 255);
    for (int i = 1; i < 4; ++i)
    {
        ImageDrawRectangle(&m, i * TW - 2, 0, 4, H, Color{0, 0, 0, 255});
        ImageDrawRectangle(&m, 0, i * TH - 2, W, 4, Color{0, 0, 0, 255});
    }
    const Font &f = wotm_padsel::uiFont();
    const float size = H * 0.85f;
    const Vector2 sz = MeasureTextEx(f, "?", size, 0.f);
    const Vector2 at{(W - sz.x) * 0.5f, (H - sz.y) * 0.5f};
    const float o = size * 0.04f;
    for (int k = 0; k < 8; ++k)
    {
        const float a = float(k) * 0.785398f;
        ImageDrawTextEx(&m, f, "?", Vector2{at.x + std::cos(a) * o, at.y + std::sin(a) * o}, size, 0.f, Color{12, 8, 4, 255});
    }
    ImageDrawTextEx(&m, f, "?", at, size, 0.f, Color{255, 214, 120, 255});
    return m;
}

static const Pic &picOf(int level)
{
    Pic &p = g_pics[level];
    if (p.tried)
        return p;
    p.tried = true;
    const char *n = picName(level);
    const fs::path f = n ? findPic(n) : fs::path{};
    Image img = f.empty() ? Image{} : LoadImage(f.string().c_str());
    const bool own = img.data != nullptr;
    if (!img.data && level == kRandom)
        img = mosaic();
    if (img.data)
    {
        ImageFormat(&img, PIXELFORMAT_UNCOMPRESSED_R8G8B8A8);
        const float w = float(img.width), h = float(img.height);
        p.half[0] = gameTexture(ImageFromImage(img, Rectangle{0.f, 0.f, w * 0.5f, h}));
        p.half[1] = gameTexture(ImageFromImage(img, Rectangle{w * 0.5f, 0.f, w * 0.5f, h}));
        p.full = gameTexture(img);
    }
    std::fprintf(stderr, "[stages] picture for stage %d: %s\n", level, own ? f.string().c_str() : p.full.id ? "mosaic of SHELL.TEX pictures" : "the game's");
    return p;
}

// The picture's OBJECT in child `which` of the switch (a GROUP with the OBJECT and a CHAR_INSTANCE); 0 = not found
static uint32_t picObject(const wotm::Ram &ram, uint32_t sw, int which)
{
    if (which >= ram.u8(sw + 0xB))
        return 0;
    const uint32_t child = ram.u32(sw + 0xC + 4u * uint32_t(which)) & 0x1FFFFFFFu;
    if (!ram.ok(child, 0x28) || (ram.u32(child) & 63) != 1 || ram.u16(child + 8) != 2)
        return 0;
    const uint32_t obj = ram.u32(child + 0x20) & 0x1FFFFFFFu, ci = ram.u32(child + 0x24) & 0x1FFFFFFFu;
    if (!ram.ok(obj, 0xA0) || (ram.u32(obj) & 63) != 0 || !ram.ok(ci, 4) || (ram.u32(ci) & 63) != 25)
        return 0;
    return obj;
}

// Picture object with a single texture (13, 16, 17): both halves of the mesh map the same texture from -1 to 0 (half of another
// stage's picture, repeated). Rebuilds U from the position along the longest axis (the width), in the direction it grew in each half.
static void wideUv(wotm::MeshData &md)
{
    if (md.tris.empty())
        return;
    float lo[3] = {1e30f, 1e30f, 1e30f}, hi[3] = {-1e30f, -1e30f, -1e30f};
    for (const wotm::Vtx &t : md.tris)
    {
        const float p[3] = {t.x, t.y, t.z};
        for (int a = 0; a < 3; ++a) { lo[a] = std::min(lo[a], p[a]); hi[a] = std::max(hi[a], p[a]); }
    }
    int ax = 0;
    for (int a = 1; a < 3; ++a)
        if (hi[a] - lo[a] > hi[ax] - lo[ax]) ax = a;
    const float span = hi[ax] - lo[ax];
    if (span <= 0.f)
        return;
    // direction: correlation of U with the position within each half (a sawtooth, but with the same sign)
    double cov = 0.0;
    float mu = 0.f;
    for (const wotm::Vtx &t : md.tris) mu += t.u;
    mu /= float(md.tris.size());
    for (const wotm::Vtx &t : md.tris)
    {
        const float p[3] = {t.x, t.y, t.z};
        const float f = (p[ax] - lo[ax]) / span;
        cov += double(t.u - mu) * double(f < 0.5f ? f : f - 0.5f);
    }
    for (wotm::Vtx &t : md.tris)
    {
        const float p[3] = {t.x, t.y, t.z};
        const float f = (p[ax] - lo[ax]) / span;
        t.u = cov >= 0.0 ? f - 1.f : -f;
    }
}

static constexpr int kOwn[] = {12, 13, 16, kRandom};
static uint32_t g_obj[24];      // picture object of each kOwn stage this frame (0 = outside the menu or not matched)
static uint16_t g_numTex[24];   // its texture count (2 for stage 12's: halves)

static unsigned nodeTexture(uint32_t node, uint32_t slot)
{
    for (const int l : kOwn)
        if (g_obj[l] && g_obj[l] == node)
        {
            const Pic &p = g_pics[l];
            if (g_numTex[l] >= 2)
                return slot < 2 ? p.half[slot].id : 0u;
            return slot == 0 ? p.full.id : 0u;
        }
    return 0;
}

// Window thread, before Scene::draw: finds the picture objects in the stage select switch and loads the pictures the first time.
// During a fight the Shell+0x5D0 pointer goes stale and RAM belongs to the stage: only the menu node's shape counts (a SWITCH with
// 24 children, each a GROUP with the OBJECT and a CHAR_INSTANCE) with the object in the scene snapshot.
void prepare(wotm::Scene &scene, const wotm::Ram &ram)
{
    scene.nodeTexture = &nodeTexture;
    for (const int l : kOwn)
        g_obj[l] = 0;
    const uint32_t sw = ram.u32(kLevelSw) & 0x1FFFFFFFu;
    if (!sw || !ram.ok(sw, 0x80) || (ram.u32(sw) & 63) != 6 || ram.u8(sw + 0xB) != 24)
        return;
    for (const int l : kOwn)
    {
        const uint32_t o = picObject(ram, sw, l);
        const uint16_t nt = o ? ram.u16(o + 0x5A) : 0;
        if (!o || nt < 1 || nt > 2)
            continue;
        bool drawn = false;
        for (const wotm::DrawItem &i : scene.items)
            if ((i.node & 0x1FFFFFFFu) == o) { drawn = true; break; }
        if (ram.u32(wotm::sym::currScreen) == 8)
            picOf(l);   // on entering the screen: all four loaded before they appear
        if (!drawn || !picOf(l).full.id)
            continue;
        if (nt == 1)
            scene.patchMesh(ram, o, wideUv);   // one texture on both halves: the whole picture end to end
        g_obj[l] = o;
        g_numTex[l] = nt;
        picOf(l);
    }
}
// "UNUSED" badge to the right of the stage name (screenWriteInstructions writes it centered at y 10 of the game screen) on stages the
// retail game doesn't offer (12, 13 and 16; not RANDOM). Placed from the frame's glyphs (Scene::text) at the top of the screen, with
// their alpha and height. Window thread, after the game's text.
void drawBadge(const std::vector<wotm::TextQuad> &text, const wotm::Ram &ram, int rw, int rh)
{
    if (ram.u32(wotm::sym::currScreen) != 8 || !(g_obj[12] || g_obj[13] || g_obj[16]))
        return;
    float x1 = -1e30f, y0 = 1e30f, y1 = -1e30f;
    int a = 0;
    for (const wotm::TextQuad &t : text)
        if (t.y1 < 60.f)
        {
            x1 = std::max(x1, std::max(t.x0, t.x1));
            y0 = std::min(y0, std::min(t.y0, t.y1));
            y1 = std::max(y1, std::max(t.y0, t.y1));
            a = std::max(a, int(t.a));
        }
    if (x1 < -1e29f || a == 0)
        return;
    const wotm::GsScreenRect gr = wotm::gsScreenRect(float(rw), float(rh));
    const float sx = gr.w / 640.f, sy = gr.h / 224.f;
    const float h = (y1 - y0) * sy * 0.62f, cy = (y0 + y1) * 0.5f * sy;
    const float size = h * 0.78f;
    const Font &f = wotm_padsel::uiFont();
    const char *label = "UNUSED";
    const Vector2 ts = MeasureTextEx(f, label, size, size * 0.08f);
    const float w = ts.x + h * 0.7f, x = gr.x0 + x1 * sx + h * 0.45f;
    const unsigned char al = static_cast<unsigned char>(std::min(255, a * 2));
    const Rectangle r{x, cy - h * 0.5f, w, h};
    DrawRectangleRounded(r, 0.35f, 6, Color{196, 52, 28, al});
    DrawRectangleRoundedLinesEx(r, 0.35f, 6, std::max(1.f, h * 0.07f), Color{255, 255, 255, al});
    DrawTextEx(f, label, Vector2{x + (w - ts.x) * 0.5f, cy - ts.y * 0.5f}, size, size * 0.08f, Color{255, 255, 255, al});
}
} // namespace wotm_levelsel

void wotmLevelSelInstall(PS2Runtime &runtime)
{
    using namespace wotm_levelsel;
    if ((g_origLevelSelect = runtime.lookupFunction(wotm::fn::screenLevelSelect__Fv)))
        runtime.registerFunction(wotm::fn::screenLevelSelect__Fv, &levelSelectHook);
}
