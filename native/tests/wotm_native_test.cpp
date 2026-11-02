// ABOUTME: Unit tests for the pure parts of the native layer (no game, no window): frame interpolation from wotm_interp.hpp (matrices,
// ABOUTME: camera, cuts, projection), the settings profile for network matches and the mod digest. Build and run: tools\test_native.bat
#include "wotm_interp.hpp"
#include "wotm_settings.hpp"
#include "wotm_mods.hpp"
#include "wotm_detmath.hpp"
#include "wotm_trig_goldens.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <functional>
#include <random>
#include <vector>

namespace
{
    int g_failures = 0, g_checks = 0;

#define CHECK(expr)                                                                                                                  \
    do                                                                                                                               \
    {                                                                                                                                \
        ++g_checks;                                                                                                                  \
        if (!(expr))                                                                                                                 \
        {                                                                                                                            \
            ++g_failures;                                                                                                            \
            std::printf("  FAILED %s:%d: %s\n", __FILE__, __LINE__, #expr);                                                          \
        }                                                                                                                            \
    } while (0)

    struct Test
    {
        const char *name;
        std::function<void()> run;
    };
    std::vector<Test> &tests()
    {
        static std::vector<Test> t;
        return t;
    }
    struct Register
    {
        Register(const char *name, std::function<void()> run) { tests().push_back({name, std::move(run)}); }
    };
#define TEST(name)                                                                                                                   \
    void name();                                                                                                                     \
    Register reg_##name(#name, name);                                                                                                \
    void name()

    namespace ip = wotm::interp;
    using wotm::GameCamera;
    using wotm::M4;

    bool near(float a, float b, float eps = 1e-4f) { return std::fabs(a - b) <= eps * std::max(1.f, std::fabs(b)); }

    // rotation around Z (row vectors: row 0 = image of the X axis), per-row scale and translation
    M4 rotZ(float deg, float sx = 1.f, float sy = 1.f, float sz = 1.f, float tx = 0.f, float ty = 0.f, float tz = 0.f)
    {
        const float a = deg * 3.14159265f / 180.f, c = std::cos(a), s = std::sin(a);
        M4 m = wotm::ident();
        m.m[0][0] = c * sx, m.m[0][1] = s * sx;
        m.m[1][0] = -s * sy, m.m[1][1] = c * sy;
        m.m[2][2] = sz;
        m.m[3][0] = tx, m.m[3][1] = ty, m.m[3][2] = tz;
        return m;
    }

    float rowLen(const M4 &m, int r) { return std::sqrt(m.m[r][0] * m.m[r][0] + m.m[r][1] * m.m[r][1] + m.m[r][2] * m.m[r][2]); }

    float det3(const M4 &m)
    {
        return m.m[0][0] * (m.m[1][1] * m.m[2][2] - m.m[1][2] * m.m[2][1]) - m.m[0][1] * (m.m[1][0] * m.m[2][2] - m.m[1][2] * m.m[2][0]) +
               m.m[0][2] * (m.m[1][0] * m.m[2][1] - m.m[1][1] * m.m[2][0]);
    }

    M4 mulM4(const M4 &a, const M4 &b)   // a first, then b (like wotm::mul in wotm_native.hpp, which pulls in raylib)
    {
        M4 r{};
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                for (int k = 0; k < 4; ++k)
                    r.m[i][j] += a.m[i][k] * b.m[k][j];
        return r;
    }

    bool sameM4(const M4 &a, const M4 &b, float eps)
    {
        for (int i = 0; i < 4; ++i)
            for (int j = 0; j < 4; ++j)
                if (!near(a.m[i][j], b.m[i][j], eps))
                    return false;
        return true;
    }

    TEST(lerp_ends_are_the_inputs)
    {
        const M4 a = rotZ(10.f, 1.f, 1.f, 1.f, 5.f, 6.f, 7.f), b = rotZ(70.f, 1.f, 1.f, 1.f, 8.f, 9.f, 10.f);
        CHECK(sameM4(ip::lerpM4(a, b, 0.f), a, 1e-4f));
        CHECK(sameM4(ip::lerpM4(a, b, 1.f), b, 1e-4f));
    }

    TEST(lerp_rotates_without_shrinking)
    {
        const M4 r = ip::lerpM4(rotZ(0.f), rotZ(90.f), 0.5f), want = rotZ(45.f);
        for (int i = 0; i < 3; ++i)
            for (int j = 0; j < 3; ++j)
                CHECK(near(r.m[i][j], want.m[i][j], 1e-3f));
        CHECK(near(rowLen(r, 0), 1.f) && near(rowLen(r, 1), 1.f));   // a per-component lerp would give 0.707
    }

    TEST(lerp_keeps_per_row_scale_and_translation)
    {
        const M4 r = ip::lerpM4(rotZ(0.f, 2.f, 1.f, 3.f), rotZ(60.f, 4.f, 1.f, 3.f, 10.f, -20.f, 4.f), 0.5f);
        CHECK(near(rowLen(r, 0), 3.f, 1e-3f) && near(rowLen(r, 1), 1.f, 1e-3f) && near(rowLen(r, 2), 3.f, 1e-3f));
        CHECK(near(r.m[3][0], 5.f) && near(r.m[3][1], -10.f) && near(r.m[3][2], 2.f) && near(r.m[3][3], 1.f));
        CHECK(near(std::atan2(r.m[0][1], r.m[0][0]) * 180.f / 3.14159265f, 30.f, 1e-3f));
    }

    TEST(lerp_keeps_mirrored_matrices_mirrored)
    {
        const M4 r = ip::lerpM4(rotZ(0.f, -1.f), rotZ(40.f, -1.f), 0.5f);   // both mirrored in X
        CHECK(det3(r) < 0.f);
        CHECK(near(rowLen(r, 0), 1.f, 1e-3f) && near(rowLen(r, 1), 1.f, 1e-3f));
    }

    TEST(lerp_falls_back_to_components_on_shear)
    {
        M4 b = rotZ(0.f);
        b.m[1][0] = 0.8f;   // row 1 is not perpendicular to row 0
        const M4 r = ip::lerpM4(rotZ(0.f), b, 0.5f);
        CHECK(near(r.m[1][0], 0.4f) && near(r.m[0][0], 1.f) && near(r.m[1][1], 1.f));
    }

    TEST(slerp_takes_the_short_way)
    {
        const ip::Quat a{1.f, 0.f, 0.f, 0.f}, b{-0.9999f, 0.f, 0.f, 0.0141f};   // almost the same rotation, opposite sign
        CHECK(std::fabs(ip::slerp(a, b, 0.5f).w) > 0.999f);
    }

    GameCamera cam(float x, float y, float z, float yawDeg, float fovy = 60.f)
    {
        GameCamera c;
        const float a = yawDeg * 3.14159265f / 180.f;
        c.ok = true;
        c.pos[0] = x, c.pos[1] = y, c.pos[2] = z;
        c.fwd[0] = std::cos(a), c.fwd[1] = std::sin(a), c.fwd[2] = 0.f;
        c.up[0] = 0.f, c.up[1] = 0.f, c.up[2] = 1.f;
        c.fovy = fovy;
        return c;
    }

    TEST(camera_lerp_moves_turns_and_zooms)
    {
        const GameCamera r = ip::lerpCamera(cam(0, 0, 0, 0, 60.f), cam(10, 0, 2, 30, 70.f), 0.5f);
        CHECK(near(r.pos[0], 5.f) && near(r.pos[2], 1.f) && near(r.fovy, 65.f));
        CHECK(near(std::atan2(r.fwd[1], r.fwd[0]) * 180.f / 3.14159265f, 15.f, 1e-3f));
        CHECK(near(r.fwd[0] * r.fwd[0] + r.fwd[1] * r.fwd[1] + r.fwd[2] * r.fwd[2], 1.f));
    }

    TEST(camera_cuts_are_not_blended)
    {
        CHECK(!ip::cameraCut(cam(0, 0, 0, 0), cam(30, 0, 0, 20)));
        CHECK(ip::cameraCut(cam(0, 0, 0, 0), cam(500, 0, 0, 0)));            // position jump
        CHECK(ip::cameraCut(cam(0, 0, 0, 0), cam(0, 0, 0, 60)));             // sudden turn (dot < 0.7)
        CHECK(ip::cameraCut(cam(0, 0, 0, 0, 60.f), cam(0, 0, 0, 0, 70.f)));  // sudden zoom
        GameCamera bad = cam(0, 0, 0, 0);
        bad.ok = false;
        CHECK(ip::cameraCut(bad, cam(0, 0, 0, 0)));                          // unreadable camera: no blending
    }

    // camera -> world and the rigid inverse: round trip of a point and a direction, and C * inverse = identity
    TEST(camera_to_world_and_back)
    {
        const GameCamera c = cam(100, 50, 10, 25);
        const M4 C = ip::cameraToWorld(c), inv = ip::rigidInverse(C);
        const float p[3] = {130.f, 70.f, 12.f}, d[3] = {0.3f, -0.2f, 0.9f};
        float q[3], back[3];
        ip::toCam(C, p, true, q);
        ip::fromCam(C, q, true, back);
        CHECK(near(back[0], p[0], 1e-3f) && near(back[1], p[1], 1e-3f) && near(back[2], p[2], 1e-3f));
        ip::toCam(C, d, false, q);
        ip::fromCam(C, q, false, back);
        CHECK(near(back[0], d[0], 1e-3f) && near(back[1], d[1], 1e-3f) && near(back[2], d[2], 1e-3f));
        CHECK(sameM4(mulM4(C, inv), wotm::ident(), 1e-4f));
    }

    // projection with the game's focal length: a point straight ahead lands in the center, to the right gives x > 0, above gives
    // y < 0 (lines go down), behind the camera doesn't project; the focal length comes out of the world -> screen matrix
    TEST(project_matches_the_game_focus)
    {
        const GameCamera c = cam(0, 0, 0, 0);
        const M4 C = ip::cameraToWorld(c);
        ip::Focus f;
        float x = 0.f, y = 0.f;
        const float ahead[3] = {100.f, 0.f, 0.f}, right[3] = {100.f, -10.f, 0.f}, up[3] = {100.f, 0.f, 10.f}, behind[3] = {-100.f, 0.f, 0.f};
        CHECK(ip::project(C, f, ahead, x, y) && near(x, 0.f) && near(y, 0.f));
        CHECK(ip::project(C, f, right, x, y) && near(x, 38.4f, 1e-3f));   // right = forward x up: -Y with forward +X and up +Z
        CHECK(ip::project(C, f, up, x, y) && near(y, -18.048f, 1e-3f));
        CHECK(!ip::project(C, f, behind, x, y));
        // rows: 0 = fh*right + c*forward, 1 = -fv*up + c*forward, 3 = forward (the camera above: forward +X, right -Y, up +Z)
        const float M[16] = {2048.f, -384.f, 0.f, 0.f, 2048.f, 0.f, -180.48f, 0.f, 0.f, 0.f, 0.f, 0.f, 1.f, 0.f, 0.f, 0.f};
        ip::Focus g;
        CHECK(ip::focusOf(M, g) && near(g.fh, 384.f, 1e-3f) && near(g.fv, 180.48f, 1e-3f));
    }

    // Network match profile (wotm_settings.hpp): settings that change game state take the fixed profile on every PC, the
    // player's own values are kept and come back outside network play; video settings stay free
    TEST(netplay_profile_forces_gameplay_settings_only)
    {
        namespace st = wotm::settings;
        st::g_val[st::Camera].store(1);
        st::g_val[st::CamSens].store(9);
        st::g_val[st::LockToggle].store(1);
        st::g_val[st::RenderScale].store(150);
        st::g_netProfile.store(false);
        CHECK(st::get(st::Camera) == 1 && st::get(st::CamSens) == 9 && st::get(st::LockToggle) == 1);
        CHECK(!st::forced(st::Camera));
        st::g_netProfile.store(true);
        CHECK(st::get(st::Camera) == 0);
        CHECK(st::get(st::CamSens) == 5);
        CHECK(st::get(st::LockToggle) == 0);
        CHECK(st::get(st::CloseCam) == 1);
        CHECK(st::get(st::CamInvertY) == 0);
        CHECK(st::forced(st::Camera) && st::forced(st::CamSens));
        CHECK(st::get(st::RenderScale) == 150 && !st::forced(st::RenderScale));
        CHECK(st::g_val[st::Camera].load() == 1 && st::g_val[st::CamSens].load() == 9);   // the player's values are still stored
        st::g_netProfile.store(false);
        CHECK(st::get(st::Camera) == 1 && st::get(st::CamSens) == 9);
    }

    // Digest of the enabled mods (wotm_mods.hpp), which goes into the network match's settings hash: the same set of files gives the
    // same value in any creation order; changing the content or a name, or adding a mod, changes it; a disabled folder ('_') doesn't count
    TEST(mod_digest_covers_active_mods_only)
    {
        namespace fs = std::filesystem;
        const fs::path root = fs::temp_directory_path() / "wotm_modtest";
        auto put = [](const fs::path &p, const std::string &s) {
            fs::create_directories(p.parent_path());
            std::ofstream(p, std::ios::binary) << s;
        };
        fs::remove_all(root);
        const fs::path a = root / "a" / "mods", b = root / "b" / "mods";
        CHECK(wotm::mods::digest(a) == 0);   // no folder: no mods
        put(a / "skin" / "MON" / "LIZARD1.TEX", "tex");
        put(a / "skin" / "MON" / "LIZARD1.NGP", "ngp");
        put(b / "skin" / "MON" / "LIZARD1.NGP", "ngp");
        put(b / "skin" / "MON" / "LIZARD1.TEX", "tex");
        const uint64_t base = wotm::mods::digest(a);
        CHECK(base != 0);
        CHECK(wotm::mods::digest(b) == base);
        put(b / "_off" / "MON" / "MONKEY1.TEX", "x");
        CHECK(wotm::mods::digest(b) == base);
        put(b / "skin" / "MON" / "LIZARD1.TEX", "teX");
        CHECK(wotm::mods::digest(b) != base);
        put(b / "skin" / "MON" / "LIZARD1.TEX", "tex");
        CHECK(wotm::mods::digest(b) == base);
        put(b / "extra" / "MON" / "MONKEY1.TEX", "x");
        CHECK(wotm::mods::digest(b) != base);
        fs::remove_all(b / "extra");
        fs::rename(b / "skin" / "MON" / "LIZARD1.NGP", b / "skin" / "MON" / "LIZARD2.NGP");
        CHECK(wotm::mods::digest(b) != base);
        fs::remove_all(root);
    }
}

namespace
{
    namespace dm = wotm::detmath;

    uint32_t bitsOf(float f)
    {
        uint32_t u;
        std::memcpy(&u, &f, 4);
        return u;
    }
    float floatOf(uint32_t u)
    {
        float f;
        std::memcpy(&f, &u, 4);
        return f;
    }
    // distance in representable floats (same sign only, which is all the accuracy checks below need)
    uint32_t ulps(float a, float b)
    {
        const uint32_t x = bitsOf(a) & 0x7FFFFFFFu, y = bitsOf(b) & 0x7FFFFFFFu;
        return (bitsOf(a) >> 31) != (bitsOf(b) >> 31) ? (x + y) : (x > y ? x - y : y - x);
    }
    // fixed inputs for the accuracy and determinism checks: a small LCG, so every machine sees the same list
    std::vector<float> sweep(int n, float lo, float hi)
    {
        std::vector<float> v;
        uint32_t s = 12345u;
        for (int i = 0; i < n; ++i)
        {
            s = s * 1664525u + 1013904223u;
            v.push_back(lo + (hi - lo) * float(s >> 8) / float(1u << 24));
        }
        return v;
    }
}

TEST(detmath_sin_cos_equal_retail_bit_for_bit)
{
    int bad = 0;
    for (const wotm::TrigGolden &g : wotm::kTrigGoldens)
        bad += bitsOf(dm::sinf(floatOf(g.x))) != g.sin || bitsOf(dm::cosf(floatOf(g.x))) != g.cos;
    CHECK(bad == 0);
}

TEST(detmath_sin_cos_within_one_ulp)
{
    uint32_t worst = 0;
    for (float x : sweep(20000, -1000.f, 1000.f))
    {
        worst = std::max(worst, ulps(dm::sinf(x), float(std::sin(double(x)))));
        worst = std::max(worst, ulps(dm::cosf(x), float(std::cos(double(x)))));
    }
    CHECK(worst <= 1);
}

TEST(detmath_atan2_special_cases)
{
    const float pi = floatOf(0x40490FDBu), halfPi = floatOf(0x3FC90FDBu);
    CHECK(bitsOf(dm::atan2f(0.f, 1.f)) == 0u);
    CHECK(bitsOf(dm::atan2f(-0.f, 1.f)) == 0x80000000u);
    CHECK(dm::atan2f(0.f, -1.f) == pi);
    CHECK(dm::atan2f(-0.f, -1.f) == -pi);
    CHECK(dm::atan2f(1.f, 0.f) == halfPi);
    CHECK(dm::atan2f(-1.f, 0.f) == -halfPi);
    CHECK(dm::atan2f(5.f, -0.f) == halfPi);
}

TEST(detmath_atan2_within_one_ulp)
{
    const std::vector<float> ys = sweep(20000, -500.f, 500.f), xs = sweep(20001, -500.f, 500.f);
    uint32_t worst = 0;
    for (size_t i = 0; i < ys.size(); ++i)
        worst = std::max(worst, ulps(dm::atan2f(ys[i], xs[i + 1]), float(std::atan2(double(ys[i]), double(xs[i + 1])))));
    CHECK(worst <= 1);
}

// The point of wotm_detmath.hpp: the same bits on every compiler and CPU. The expected digest was measured on the Mac (clang,
// arm64); Windows (MSVC, x64) must print the same, or its build computes the camera differently and a network match desyncs.
TEST(detmath_same_bits_on_every_machine)
{
    uint64_t h = 1469598103934665603ull;   // FNV-1a over the result bits
    auto mix = [&h](float f) {
        const uint32_t u = bitsOf(f);
        for (int k = 0; k < 4; ++k)
            h = (h ^ ((u >> (8 * k)) & 0xFFu)) * 1099511628211ull;
    };
    const std::vector<float> a = sweep(50000, -20000.f, 20000.f), b = sweep(50001, -3.f, 3.f);
    for (size_t i = 0; i < a.size(); ++i)
    {
        mix(dm::sinf(a[i]));
        mix(dm::cosf(a[i]));
        mix(dm::atan2f(a[i], b[i + 1]));
        mix(dm::atan2f(b[i], b[i + 1]));
    }
    std::printf("  detmath digest %016llx\n", static_cast<unsigned long long>(h));
    CHECK(h == 0xa6c8104174bda8ccull);
}

int main()
{
    for (const Test &t : tests())
    {
        const int before = g_failures;
        t.run();
        std::printf("%s %s\n", g_failures == before ? "ok     " : "FAILED ", t.name);
    }
    std::printf("%d checks, %d failure(s)\n", g_checks, g_failures);
    return g_failures == 0 ? 0 : 1;
}
