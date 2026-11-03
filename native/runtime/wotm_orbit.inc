// wotm_orbit.inc: modern (orbit) camera; plan and stages in
// 75f5be1:docs/investigacoes/2026-10-22/camera_orbital_plano.md. Included in wotm_overrides.cpp, which calls in here from the hooks it already has:
//   - Cameras::Update (the free camera wrapper): orbitPre before the original, orbitPost after;
//   - PadFlags::interpretInputs (the lock-on one): orbitInputEnter on entry, orbitInputLeave once the function has actually returned;
//   - Camera::shakeCamera: stores the frame's shake, which the orbit adds to its own position;
//   - TheGame::SetOkToUnify: CloseCam = 0 clears everyone's m_camUnify (never enters the unified view), like wotm_split4.inc.
//
// Stage 1 (basic orbit). Applies to the camera with POV 0 (normalUpdate) that follows a human player's monster in that player's view,
// outside the unified view (except camera 2 on POV 0, the grab camera), mode 9 (Crush), the free camera and WOTM_AIVSAI (batches and corpus
// keep the game camera). The game
// keeps running normalUpdate underneath (its shake is reused); after it the orbit rebuilds the whole pose:
//   - position: the POV offset (D10, only that: the game's sway and tilt change with the monster's state and caused jolts)
//     rotated by our angle around the monster (az = heading from the monster to the camera) instead of the heading of its back; the right
//     stick turns az (X) and elevation (Y); the pivot follows the monster with a short, bounded delay;
//   - yaw and pitch: toward a point ~120 above the pivot (where the game camera aims), so raising the camera tilts by the right
//     amount; when locked on, that point moves toward the target;
//   - recenter (CamRecenter): ~1.5 s without touching the right stick, with the monster walking, az slowly returns behind its back;
//   - the game is neutralized only where it fights the orbit: rear sector lock (E60 = 1), return to the back (DA4 = DF4 = 0).
// When turning on (first frame, coming back from another POV, from the unified view or from the free camera) az starts from where the other
// mode left it and the pose slides from the game's to the orbit's over ~20 frames: no jump. A game cut (D98) with the orbit already on (round
// restart, respawn) restarts az from behind the back.
//
// Controls: with the orbit driving the player's view, interpretInputs runs with PadFlags +0x16E4/+0x16E8 = 1 (screen-relative
// configuration, updateInput*ConfigSR, which reads this camera's _cs) and the game's values are restored on exit; the right stick
// bytes (GamePad 0x10..0x13: left, right, up, down) go to the orbit and reach the game zeroed (in CR they would turn the monster,
// in SR too, when the left stick is still). Takes effect immediately, even while walking.
namespace
{
    namespace orbit
    {
        constexpr uint32_t kCameras = wotm::sym::x_7Cameras_m_cameras, kNumCameras = wotm::sym::x_7Cameras_m_numCameras, kCamStride = 0xEB0u,
                           kUnified = wotm::sym::gUseUnifiedView, kGameMode = kGame + 0x1203C8u, kSlots = kGame + 0xB80u,
                           kSlotStride = 0x11190u, kNumSlots = kGame + 0x1203D4u;
        // Camera (75f5be1:docs/investigacoes/2026-10-22/camera_normalupdate.md)
        constexpr uint32_t kCamCs = 0xCF0u, kCamMonster = 0xCF4u, kCamPov = 0xCF8u, kCamOffset = 0xD10u,
                           kCamIdle = 0xDA4u, kCamDrag = 0xDF4u, kCamCut = 0xD98u,
                           kCamSectorLock = 0xE60u;
        // Monster
        constexpr uint32_t kMonCs = 0xCu, kMonDead = 0xE8u, kMonPinTarget = 0x6C04u, kMonCameraView = 0x6CD8u, kMonCamUnify = 0x7970u;
        // PadFlags
        constexpr uint32_t kPfOwner = 0x1810u, kPfConfigActive = 0x16E4u, kPfConfigWanted = 0x16E8u, kPfRingIndex = 0x16F0u;
        constexpr int kMaxCams = 4;
        constexpr float kPi = 3.14159265f;
        constexpr float kLockRate = 0.15f;   // locked target: angle and aim per frame (~6 frames; 0.25 was too abrupt for the owner)

        struct Cam
        {
            bool active = false;     // the orbit wrote the pose on the previous frame
            uint32_t mon = 0;        // monster being followed when it turned on
            float az = 0.f;          // heading from the monster to the camera
            float elev = 0.f;        // elevation added by the stick (radians)
            float pivot[3] = {};     // point it orbits around (follows the monster with a delay)
            float lastMon[3] = {};
            float blend = 0.f;       // 1 = game pose, 0 = orbit pose (smooth entry)
            int still = 0;           // frames without touching the right stick
            bool cutSeen = false;    // D98 set on entry to this frame
            float shake[3] = {};     // shakeCamera offset on this frame
            float nudge = 0.f;       // right stick deflection while a target is locked
            bool locked = false;
            int away = 0;            // consecutive frames with the monster moving away from the camera
            float lockW = 0.f;       // weight of the aim at the locked target (0..1)
            float lockAim[3] = {};   // aim offset toward the target (the last one, while the weight drops after release)
            float reach = 1.f;       // fraction of the free distance (collision: shrinks immediately, comes back slowly)
            float lift = 0.f;        // height added to stay above the ground (smoothed on the way down)
            uint32_t lastPin = 0;    // m_pinTarget seen on the previous frame (log)
        } g_cam[kMaxCams];

        struct Input
        {
            bool driving = false;    // the orbit drives this player's view (orbitPost)
            bool forced = false;     // SR configuration imposed on the interpretInputs in progress
            bool pending = false;    // forced, but the function hasn't returned yet (time slice): restore on the next entry
            uint32_t pf = 0, saved4 = 0, saved8 = 0;
            float stick[2] = {};     // right stick from the last frame (x right, y up), -1..1
            float tilt = 1.f;        // left stick deflection (0..1) on entry to the forced interpretInputs
        } g_in[4];

        float rdF(uint8_t *rdram, uint32_t a) { float v; std::memcpy(&v, rdram + a, 4); return v; }
        void wrF(uint8_t *rdram, uint32_t a, float v) { std::memcpy(rdram + a, &v, 4); }
        uint32_t camAddr(int v) { return kCameras + kCamStride * uint32_t(v); }
        bool okAddr(uint32_t a, uint32_t n) { return a >= 0x1000u && a + n <= PS2_RAM_SIZE; }
        float wrapPi(float a)
        {
            while (a > kPi) a -= 2.f * kPi;
            while (a < -kPi) a += 2.f * kPi;
            return a;
        }
        float headingTo(float dx, float dy) { return wotm::detmath::atan2f(dx, dy); }   // 0 = +Y, positive toward +X

        bool enabled() { return wotm::settings::on(wotm::settings::Camera) && !g_aiVsAi && !g_wotmFreeCam.load(std::memory_order_relaxed); }

        // Camera v eligible on this frame; mon = monster, slot = player
        bool eligible(uint8_t *rdram, int v, uint32_t &mon, int &slot)
        {
            if (v >= kMaxCams || !enabled() || FAST_READ32(kGameMode) == 9u)
                return false;
            // unified view: only camera 2 on POV 0, which is the grab camera (StateGrappleLift: follows the grabber). The close one is POV 1,
            // the victory one is 7: those stay with the game
            const bool unified = FAST_READ32(kUnified) != 0u;
            if (unified && v != 2)
                return false;
            const uint32_t c = camAddr(v);
            if (FAST_READ32(c + kCamPov) != 0u)
                return false;
            mon = FAST_READ32(c + kCamMonster) & 0x1FFFFFFFu;
            const uint32_t cs = FAST_READ32(c + kCamCs) & 0x1FFFFFFFu;
            if (!okAddr(mon, kMonCamUnify + 4u) || !okAddr(cs, 0x70u) || !isPlayerMonster(rdram, FAST_READ32(c + kCamMonster), &slot))
                return false;
            if (!unified && static_cast<int32_t>(FAST_READ32(mon + kMonCameraView)) != v)
                return false;   // camera 2 following the grabber outside the unified view (1P without the close one etc.): the player's view is another one
            const uint32_t mcs = FAST_READ32(mon + kMonCs) & 0x1FFFFFFFu;
            return okAddr(mcs, 0x70u);
        }

        // Padless test: WOTM_ORBITSTICK="frame:frames:x:y[@player];..." sets the right stick to (x, y) (-1..1, y up) from the
        // given frame (counted in Cameras::Update calls) for that many frames
        void testStick(unsigned frame, int slot, float &sx, float &sy)
        {
            struct Step { unsigned f0, n; float x, y; int player; };   // player 0 = all
            static std::vector<Step> steps;
            static bool parsed = false;
            if (!parsed)
            {
                parsed = true;
                if (const char *e = std::getenv("WOTM_ORBITSTICK"))
                    for (const char *p = e; *p;)
                    {
                        Step s{};
                        if (std::sscanf(p, "%u:%u:%f:%f@%d", &s.f0, &s.n, &s.x, &s.y, &s.player) >= 4)
                            steps.push_back(s);
                        const char *semi = std::strchr(p, ';');
                        if (!semi)
                            break;
                        p = semi + 1;
                    }
            }
            for (const Step &s : steps)
                if (frame >= s.f0 && frame < s.f0 + s.n && (s.player == 0 || s.player == slot + 1))
                {
                    sx = s.x;
                    sy = s.y;
                }
        }

        // Mouseless test: WOTM_ORBITMOUSE="frame:frames:dx:dy;..." adds (dx, dy) pixels per frame (y down, like the mouse) to the
        // keyboard player's mouse, from the given frame for that many frames
        void testMouse(unsigned frame, float &mx, float &my)
        {
            struct Step { unsigned f0, n; float x, y; };
            static std::vector<Step> steps;
            static bool parsed = false;
            if (!parsed)
            {
                parsed = true;
                if (const char *e = std::getenv("WOTM_ORBITMOUSE"))
                    for (const char *p = e; *p;)
                    {
                        Step s{};
                        if (std::sscanf(p, "%u:%u:%f:%f", &s.f0, &s.n, &s.x, &s.y) == 4)
                            steps.push_back(s);
                        const char *semi = std::strchr(p, ';');
                        if (!semi)
                            break;
                        p = semi + 1;
                    }
            }
            for (const Step &s : steps)
                if (frame >= s.f0 && frame < s.f0 + s.n)
                {
                    mx += s.x;
                    my += s.y;
                }
        }

        // Monster::hasPinTarget (m_pinTarget with bit 2 of +4) and the target position (the _cs is at +0xC for both monster and building)
        bool pinTarget(uint8_t *rdram, uint32_t mon, float tp[3])
        {
            const uint32_t t = FAST_READ32(mon + kMonPinTarget) & 0x1FFFFFFFu;
            if (!okAddr(t, 0x10u))
                return false;
            uint16_t flags;
            std::memcpy(&flags, rdram + t + 4, 2);
            if (!(flags & 2u))
                return false;
            const uint32_t cs = FAST_READ32(t + kMonCs) & 0x1FFFFFFFu;
            if (!okAddr(cs, 0x20u))
                return false;
            for (int i = 0; i < 3; ++i)
                tp[i] = rdF(rdram, cs + 0x10 + 4 * i);
            return true;
        }

        int numCams(uint8_t *rdram) { return std::clamp(static_cast<int32_t>(FAST_READ32(kNumCameras)), 0, kMaxCams); }

        // Collision (stage 3) through the game's own functions, called from here with the data in an area below the current stack:
        //   hdLosTest(from, to, _hdResult *res, bool) -> v0 = 1 clear; res: hit +0, surface +2, ids +4, point +0x10, normal +0x20
        //   hdHatTest(pos, normal, $f12 up, bool, $f13 down) -> $f0 = pos.z - ground (0 with no ground)
        struct Probe
        {
            uint8_t *rdram;
            R5900Context *ctx;
            PS2Runtime *rt;
            uint32_t base = 0;   // 0x100 bytes of data; the call stack starts right below
            bool ok = false;
            Probe(uint8_t *r, R5900Context *c, PS2Runtime *p) : rdram(r), ctx(c), rt(p)
            {
                const uint32_t sp = GPR_U32(ctx, 29) & 0x1FFFFFFFu;
                base = (sp - 0x1000u) & ~0xFu;
                ok = rt && sp > 0x10000u && okAddr(base - 0x1000u, 0x1100u);
            }
            bool call(uint32_t fn, std::initializer_list<uint32_t> a, std::initializer_list<float> f, uint32_t &v0, float &f0)
            {
                const PS2Runtime::RecompiledFunction fp = ok ? rt->lookupFunction(fn) : nullptr;
                if (!fp)
                    return false;
                const R5900Context saved = *ctx;
                int r = 4;
                for (uint32_t x : a)
                    ctx->r[r++] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(x)));
                int fr = 12;
                for (float x : f)
                    ctx->f[fr++] = x;
                ctx->r[29] = _mm_set_epi64x(0, static_cast<int64_t>(static_cast<int32_t>(base - 0x10u)));
                ctx->r[31] = _mm_set_epi64x(0, 0);
                ctx->pc = fn;
                fp(rdram, ctx, rt);
                const bool back = ctx->pc == 0u;
                v0 = GPR_U32(ctx, 2);
                f0 = ctx->f[0];
                *ctx = saved;
                return back;
            }
            void put(uint32_t a, const float v[3])
            {
                for (int i = 0; i < 3; ++i)
                    wrF(rdram, a + 4 * i, v[i]);
                wrF(rdram, a + 12, 0.f);
            }
            // segment a -> b; true if it hit (pt, nrm, ids, surface of the first contact)
            bool los(const float a[3], const float b[3], float pt[3], float nrm[3], uint32_t &ids, uint32_t &surf)
            {
                put(base, a);
                put(base + 0x10, b);
                std::memset(rdram + base + 0x20, 0, 0x30);
                uint32_t v0 = 1;
                float f0 = 0.f;
                if (!call(wotm::fn::hdLosTest__FP8_fvectorT0P9_hdResultb, {base, base + 0x10, base + 0x20, 0}, {}, v0, f0) || v0 != 0u ||
                    !rdram[base + 0x20])
                    return false;
                for (int i = 0; i < 3; ++i)
                {
                    pt[i] = rdF(rdram, base + 0x30 + 4 * i);
                    nrm[i] = rdF(rdram, base + 0x40 + 4 * i);
                }
                uint16_t s16;
                std::memcpy(&s16, rdram + base + 0x22, 2);
                surf = s16;
                ids = FAST_READ32(base + 0x24);
                return true;
            }
            // height of p above the ground right below (searches from `up` above to 4096 below); false with no ground
            bool hat(const float p[3], float up, float &above)
            {
                put(base, p);
                uint32_t v0 = 0;
                float f0 = 0.f;
                if (!call(wotm::fn::hdHatTest__FP8_fvectorT0fbf, {base, base + 0x10, 0}, {up, 4096.f}, v0, f0))
                    return false;
                above = f0;
                return f0 != 0.f || rdF(rdram, base + 0x18) != 1.f;   // no ground: 0 with the normal (0, 0, 1)
            }
        };

        void pre(uint8_t *rdram)
        {
            const int n = numCams(rdram);
            for (int v = 0; v < n; ++v)
            {
                Cam &s = g_cam[v];
                uint32_t mon = 0;
                int slot = 0;
                s.cutSeen = false;
                std::memset(s.shake, 0, sizeof s.shake);
                if (!eligible(rdram, v, mon, slot))
                    continue;
                const uint32_t c = camAddr(v);
                s.cutSeen = FAST_READ32(c + kCamCut) != 0u;
                FAST_WRITE32(c + kCamSectorLock, 1);       // no rear sector lock
                FAST_WRITE32(c + kCamIdle, 0);             // no return to the back
                FAST_WRITE32(c + kCamDrag, 0);
            }
        }

        // Ground and buildings (stage 3). pos = where the orbit wants the camera; the sight point is the pivot 120 above (the aim)
        void collide(Probe &pr, Cam &s, int v, float pos[3], bool logNow)
        {
            static const int mode = [] { const char *e = std::getenv("WOTM_CAMCOLLIDE"); return e ? std::atoi(e) : 2; }();
            if (!mode || !pr.ok)
                return;
            const float a[3] = {s.pivot[0], s.pivot[1], s.pivot[2] + 120.f};
            float pt[3], nrm[3], rpt[3], rnrm[3];
            uint32_t ids = 0, surf = 0, rids = 0, rsurf = 0;
            const bool fwd = pr.los(a, pos, pt, nrm, ids, surf);
            const bool rev = fwd && pr.los(pos, a, rpt, rnrm, rids, rsurf);
            float d[3], len2 = 0.f;
            for (int i = 0; i < 3; ++i)
            {
                d[i] = pos[i] - a[i];
                len2 += d[i] * d[i];
            }
            const float len = std::sqrt(std::max(len2, 1.f));
            float tHit = 1.f, tRev = 0.f;
            if (fwd)
                tHit = ((pt[0] - a[0]) * d[0] + (pt[1] - a[1]) * d[1] + (pt[2] - a[2]) * d[2]) / (len * len);
            if (rev)
                tRev = ((rpt[0] - a[0]) * d[0] + (rpt[1] - a[1]) * d[1] + (rpt[2] - a[2]) * d[2]) / (len * len);
            float above = 0.f;
            const bool ground = pr.hat(pos, 60.f, above);
            if (logNow || (fwd && std::getenv("WOTM_ORBITLOG") && std::atoi(std::getenv("WOTM_ORBITLOG")) >= 3))
                std::fprintf(stderr, "[orbit] camera %d: collision out %d t %.2f n %.2f,%.2f,%.2f ids %08X surf %04X | back %d t %.2f ids %08X | ground %d %.0f\n",
                             v, fwd, tHit, nrm[0], nrm[1], nrm[2], ids, surf, rev, tRev, rids, ground, above);
            if (mode < 2)
                return;   // =1 only measures (temporary, until we decide from the log)
            // Inside something: the outbound ray hits and the return ray (from the camera to the aim) doesn't leave anything after the outbound point. Behind a building,
            // with the camera outside, the return ray hits the back face (t greater than the outbound one): it stays, and the game makes the building
            // transparent (hierSetCameraLosPoint). Inside a building or below the ground: shrinks to just before the contact.
            const bool inside = fwd && (!rev || tRev <= tHit + 0.01f);
            const float want = inside ? std::max((tHit * len - 25.f) / len, 0.15f) : 1.f;
            s.reach = want < s.reach ? want : s.reach + (want - s.reach) * 0.05f;   // shrinks immediately, comes back slowly
            for (int i = 0; i < 3; ++i)
                pos[i] = a[i] + d[i] * s.reach;
            // ground: at least 30 above; comes down slowly when the ground drops
            float above2 = 0.f;
            const float need = pr.hat(pos, 60.f, above2) ? std::max(0.f, 30.f - above2) : 0.f;
            s.lift = std::max(need, s.lift - 3.f);
            pos[2] += s.lift;
        }

        // Jump meter (stage 4, with WOTM_ORBITLOG): the final pose of each view, frame by frame; a big jump (position or
        // yaw) becomes a line with the POV, the orbit and the unified view before and after. Also works with the classic camera (baseline).
        void jumpMeter(uint8_t *rdram, int n, unsigned frames)
        {
            struct Last { bool ok = false; float pos[3] = {}, heading = 0.f; uint32_t pov = 0; bool orbit = false; uint32_t unified = 0; };
            static Last last[kMaxCams];
            for (int v = 0; v < kMaxCams; ++v)
            {
                Last &l = last[v];
                if (v >= n)
                {
                    l.ok = false;
                    continue;
                }
                const uint32_t c = camAddr(v), cs = FAST_READ32(c + kCamCs) & 0x1FFFFFFFu;
                if (!okAddr(cs, 0x70u))
                {
                    l.ok = false;
                    continue;
                }
                float pos[3];
                for (int i = 0; i < 3; ++i)
                    pos[i] = rdF(rdram, cs + 0x10 + 4 * i);
                const float heading = rdF(rdram, cs + 0x68);
                const uint32_t pov = FAST_READ32(c + kCamPov), unified = FAST_READ32(kUnified);
                const bool orbit = g_cam[v].active;
                if (l.ok)
                {
                    const float dx = pos[0] - l.pos[0], dy = pos[1] - l.pos[1], dz = pos[2] - l.pos[2];
                    const float d = std::sqrt(dx * dx + dy * dy + dz * dz), dh = std::fabs(wrapPi(heading - l.heading)) * 57.2958f;
                    if (d > 60.f || dh > 20.f)
                        std::fprintf(stderr, "[orbit] camera %d: jump frame %u: %.0f distance, %.0f degrees (pov %d->%d, orbit %d->%d, unified %d->%d, cut %d)\n",
                                     v, frames, d, dh, static_cast<int32_t>(l.pov), static_cast<int32_t>(pov), l.orbit, orbit, l.unified, unified,
                                     FAST_READ32(c + kCamCut) != 0u);
                }
                l.ok = true;
                std::memcpy(l.pos, pos, sizeof pos);
                l.heading = heading;
                l.pov = pov;
                l.orbit = orbit;
                l.unified = unified;
            }
        }

        void post(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt)
        {
            static const bool log = std::getenv("WOTM_ORBITLOG") != nullptr;
            static const unsigned logEvery = log && std::atoi(std::getenv("WOTM_ORBITLOG")) >= 2 ? 15u : 60u;   // =2: four per second
            static unsigned frames = 0;
            ++frames;
            Probe probe(rdram, ctx, rt);
            const int n = numCams(rdram);
            bool driving[4] = {};
            bool wasActive[kMaxCams];
            for (int v = 0; v < kMaxCams; ++v)
                wasActive[v] = g_cam[v].active;
            // mouse: the cursor stays captured while some camera follows the keyboard player's monster (including the game's own: fall,
            // throw), outside the pause
            if (enabled() && FAST_READ32(wotm::sym::isPaused) == 0u)
                for (int v = 0; v < n && v < kMaxCams; ++v)
                    if (int ks = -1; isPlayerMonster(rdram, FAST_READ32(camAddr(v) + kCamMonster), &ks) &&
                                     ks == wotm::mouse::g_kbPlayer.load(std::memory_order_relaxed))
                        wotm::mouse::want();
            for (int v = 0; v < kMaxCams; ++v)
            {
                Cam &s = g_cam[v];
                uint32_t mon = 0;
                int slot = 0;
                if (v >= n || !eligible(rdram, v, mon, slot))
                {
                    if (s.active && log)
                        std::fprintf(stderr, "[orbit] camera %d: off (pov %d, unified %d)\n", v,
                                     v < n ? static_cast<int32_t>(FAST_READ32(camAddr(v) + kCamPov)) : -1, FAST_READ32(kUnified));
                    s.active = false;
                    continue;
                }
                const uint32_t c = camAddr(v), cs = FAST_READ32(c + kCamCs) & 0x1FFFFFFFu, mcs = FAST_READ32(mon + kMonCs) & 0x1FFFFFFFu;
                float mt[3], game[3];
                for (int i = 0; i < 3; ++i)
                {
                    mt[i] = rdF(rdram, mcs + 0x10 + 4 * i);
                    game[i] = rdF(rdram, cs + 0x10 + 4 * i);
                }
                const float monHeading = rdF(rdram, mcs + 0x68);
                Input &in = g_in[slot];
                const bool restart = !s.active || s.mon != mon;
                // camera switch with the same monster (grab: view 0 -> unified camera 2 and back): the orbit carries on with the
                // other camera's state, with no smooth entry and no restart
                int from = -1;
                if (restart)
                    for (int u = 0; u < kMaxCams; ++u)
                        if (u != v && wasActive[u] && g_cam[u].mon == mon)
                            from = u;
                if (from >= 0)
                {
                    const bool cut = s.cutSeen;
                    float shake[3];
                    std::memcpy(shake, s.shake, sizeof shake);
                    s = g_cam[from];
                    s.cutSeen = cut;
                    std::memcpy(s.shake, shake, sizeof shake);
                    g_cam[from].active = false;
                    if (log)
                        std::fprintf(stderr, "[orbit] camera %d: continues from camera %d (az %.1f)\n", v, from, s.az * 57.2958f);
                }
                else if (restart || s.cutSeen)
                {
                    // from where the game (or its cut) left the camera
                    const float dx = game[0] - mt[0], dy = game[1] - mt[1];
                    s.az = dx * dx + dy * dy > 1.f ? headingTo(dx, dy) : wrapPi(monHeading + kPi);
                    s.elev = 0.f;
                    std::memcpy(s.pivot, mt, sizeof mt);
                    std::memcpy(s.lastMon, mt, sizeof mt);
                    s.blend = restart && !s.cutSeen ? 1.f : 0.f;
                    s.still = 0;
                    s.nudge = 0.f;
                    s.locked = false;
                    s.reach = 1.f;
                    s.lift = 0.f;
                    s.mon = mon;
                    in.stick[0] = in.stick[1] = 0.f;
                    if (log)
                        std::fprintf(stderr, "[orbit] camera %d: on (player %d, az %.1f, monster heading %.1f%s)\n", v, slot + 1,
                                     s.az * 57.2958f, monHeading * 57.2958f, s.cutSeen ? ", game cut" : "");
                }
                s.active = true;
                driving[slot] = true;

                // right stick: ~170 degrees/s at most, at 60 frames/s
                float sx = in.stick[0], sy = in.stick[1];
                testStick(frames, slot, sx, sy);
                const bool stickMoved = std::fabs(sx) > 0.05f || std::fabs(sy) > 0.05f;
                s.still = stickMoved ? 0 : s.still + 1;
                // panel, CAMERA tab: sensitivity (5 = 1x, each point 20%) and invert the vertical axis
                const float sens = float(std::clamp(wotm::settings::get(wotm::settings::CamSens), 1, 10)) * 0.2f;
                sx *= sens;
                sy *= wotm::settings::on(wotm::settings::CamInvertY) ? -sens : sens;
                s.elev = std::clamp(s.elev - sy * 0.012f, -0.6f, 0.9f);   // ~40 degrees/s (horizontal is ~170)

                // mouse (stage 7, wotm_mouse.hpp): only in the keyboard player's orbit. Direct offset (angle per pixel, ~1000
                // pixels per 140 degrees at sensitivity 5), added after the stick; up looks up, like the stick.
                float mx = 0.f, my = 0.f;
                if (slot == wotm::mouse::g_kbPlayer.load(std::memory_order_relaxed))
                {
                    if (FAST_READ32(wotm::sym::isPaused) == 0u)
                        wotm::mouse::want();
                    wotm::mouse::take(mx, my);
                    testMouse(frames, mx, my);
                    mx *= 0.0025f * sens;
                    my *= 0.0025f * (wotm::settings::on(wotm::settings::CamInvertY) ? -sens : sens);
                    if (mx != 0.f || my != 0.f)
                    {
                        s.still = 0;
                        s.elev = std::clamp(s.elev + my, -0.6f, 0.9f);
                    }
                }
                const bool moved = stickMoved || mx != 0.f || my != 0.f;

                // locked target (stage 2): the camera moves quickly behind the monster on the line to the target (both on screen); the right
                // stick only shifts it a little (up to ~35 degrees) and it springs back on release. Unlocked, the angle continues from where it was.
                float tp[3];
                const bool locked = pinTarget(rdram, mon, tp) && (tp[0] - mt[0]) * (tp[0] - mt[0]) + (tp[1] - mt[1]) * (tp[1] - mt[1]) > 1.f;
                if (locked)
                {
                    s.nudge = moved ? std::clamp(s.nudge + sx * 0.04f + mx, -0.6f, 0.6f) : s.nudge * 0.9f;
                    const float want = wrapPi(headingTo(mt[0] - tp[0], mt[1] - tp[1]) + s.nudge);
                    s.az = wrapPi(s.az + wrapPi(want - s.az) * kLockRate);
                }
                else
                {
                    s.nudge = 0.f;
                    s.az = wrapPi(s.az + sx * 0.05f + mx);
                }
                if (locked != s.locked && log)
                    std::fprintf(stderr, "[orbit] camera %d: %s\n", v, locked ? "target locked" : "target released");
                s.locked = locked;
                if (const uint32_t pin = FAST_READ32(mon + kMonPinTarget); pin != s.lastPin)
                {
                    if (log)
                        std::fprintf(stderr, "[orbit] camera %d: target 0x%x -> 0x%x (unified %d, locked %d)\n", v, s.lastPin, pin,
                                     FAST_READ32(kUnified), locked);
                    s.lastPin = pin;
                }

                // recenter: only with the monster moving away from the camera (walking toward the front of the screen). Walking sideways or
                // backward the monster turns toward where it walks; bringing the camera behind its back would change "backward" and it would spin
                // endlessly (that was the crazy camera when releasing lock-on while walking backward, and the spinning by itself while walking sideways)
                const float vx = mt[0] - s.lastMon[0], vy = mt[1] - s.lastMon[1], speed = std::sqrt(vx * vx + vy * vy);
                std::memcpy(s.lastMon, mt, sizeof mt);
                if (!locked && wotm::settings::on(wotm::settings::CamRecenter) && s.still > 90 && speed > 0.5f)
                {
                    const float align = -(vx * wotm::detmath::sinf(s.az) + vy * wotm::detmath::cosf(s.az)) / speed;   // 1 = away from the camera (az is the heading from the monster to it)
                    // walking forward (monster heading matching the movement) and sustained: the monster's turn has its own movement
                    // (starting to walk sideways while facing the camera slides ~1 s away from it, at 2.5 per frame)
                    const float facing = (vx * wotm::detmath::sinf(monHeading) + vy * wotm::detmath::cosf(monHeading)) / speed;
                    s.away = align > 0.5f && facing > 0.7f ? s.away + 1 : 0;
                    if (s.away > 20)
                    {
                        const float back = headingTo(-vx, -vy);
                        const float rate = 0.025f * (align - 0.5f) / 0.5f;
                        s.az = wrapPi(s.az + wrapPi(back - s.az) * rate);
                        s.elev -= s.elev * rate;
                    }
                }
                else
                    s.away = 0;

                // pivot: follows the monster with a short, bounded delay (height slower: a jump doesn't shake the camera)
                for (int i = 0; i < 3; ++i)
                {
                    const float rate = i < 2 ? 0.3f : 0.12f, lim = i < 2 ? 40.f : 90.f;
                    s.pivot[i] += (mt[i] - s.pivot[i]) * rate;
                    s.pivot[i] = std::clamp(s.pivot[i], mt[i] - lim, mt[i] + lim);
                }

                // game offset in the monster's frame of reference (x right, y forward, z up): only the POV's D10 (fullscreen or
                // split). Without the game's sway or tilt: both change with the monster's state (turning in place and
                // starting to walk backward the game aims straight at the monster; locking/unlocking redoes a ramp; when locked, the sway raised
                // the camera to right above the monster), and with the camera at another angle they turned into jolts
                const float offY = rdF(rdram, c + kCamOffset + 4), offZ = rdF(rdram, c + kCamOffset + 8);
                const float back = -offY, len = std::sqrt(back * back + offZ * offZ);
                const float e0 = wotm::detmath::atan2f(offZ, back), e = std::clamp(e0 + s.elev, -0.25f, 1.45f);
                const float horiz = len * wotm::detmath::cosf(e), height = len * wotm::detmath::sinf(e);

                float pos[3] = {s.pivot[0] + wotm::detmath::sinf(s.az) * horiz, s.pivot[1] + wotm::detmath::cosf(s.az) * horiz, s.pivot[2] + height};
                collide(probe, s, v, pos, log && frames % logEvery == 0);
                if (s.blend > 0.001f)
                {
                    for (int i = 0; i < 3; ++i)
                        pos[i] += (game[i] - pos[i]) * s.blend;
                    s.blend *= 0.85f;
                }
                else
                    s.blend = 0.f;
                for (int i = 0; i < 3; ++i)
                    pos[i] += s.shake[i];

                WotmFreeCamPose p;
                std::memcpy(p.pos, pos, sizeof pos);
                // Aim: the pivot, ~120 above the feet (the game camera, 231 behind and 160 above looking 10 degrees down, aims there), so
                // raising the camera tilts by the right amount and the monster stays in the same place on screen. When locked, the aim moves toward the target (30%
                // of the way, up to 120 on the ground plane and 80 in height; with the target far away the monster went off the bottom of the screen); the weight fades in and out over
                // ~6 frames, with no jump on lock and release
                s.lockW = locked ? s.lockW + (1.f - s.lockW) * kLockRate : s.lockW * (1.f - kLockRate);
                if (locked)
                {
                    const float dx = tp[0] - mt[0], dy = tp[1] - mt[1], d = std::sqrt(dx * dx + dy * dy);
                    const float f = d > 1.f ? std::min(0.3f * d, 120.f) / d : 0.f;
                    s.lockAim[0] = dx * f;
                    s.lockAim[1] = dy * f;
                    s.lockAim[2] = std::clamp((tp[2] - mt[2]) * 0.3f, -80.f, 80.f);
                }
                const float aim[3] = {s.pivot[0] + s.lockAim[0] * s.lockW, s.pivot[1] + s.lockAim[1] * s.lockW,
                                      s.pivot[2] + 120.f + s.lockAim[2] * s.lockW};
                const float adx = aim[0] - pos[0], ady = aim[1] - pos[1];
                p.heading = headingTo(adx, ady);
                p.pitch = std::clamp(wotm::detmath::atan2f(aim[2] - pos[2], std::max(std::sqrt(adx * adx + ady * ady), 1.f)), -1.5f, 1.5f);
                float ax[3][3];
                wotmFreeCamAxes(p, ax[0], ax[1], ax[2]);
                for (int i = 0; i < 3; ++i)
                {
                    wrF(rdram, cs + 0x10 + 4 * i, p.pos[i]);
                    for (int r = 0; r < 3; ++r)
                        wrF(rdram, cs + 0x20 + 0x10 * r + 4 * i, ax[r][i]);
                }
                for (int r = 0; r < 3; ++r)
                    wrF(rdram, cs + 0x20 + 0x10 * r + 0xC, 0.f);
                wrF(rdram, cs + 0x60, p.pitch);
                wrF(rdram, cs + 0x64, 0.f);
                wrF(rdram, cs + 0x68, p.heading);
                if (log && frames % logEvery == 0)
                    std::fprintf(stderr, "[orbit] camera %d: frame %u az %.1f (back %.1f) elev %.1f dist %.0f alt %.0f stick %.2f,%.2f still %d reach %.2f ground +%.0f monster %.0f,%.0f\n", v, frames,
                                 s.az * 57.2958f, wrapPi(monHeading + kPi) * 57.2958f, s.elev * 57.2958f, horiz, height, sx, sy, s.still, s.reach, s.lift, mt[0], mt[1]);
            }
            for (int i = 0; i < 4; ++i)
                g_in[i].driving = driving[i];
            if (log)
                jumpMeter(rdram, n, frames);
        }

        // Camera::shakeCamera (a0 = camera): the frame's shake is what it added to cs->trans
        PS2Runtime::RecompiledFunction g_origShake = nullptr;
        void shakeHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t c = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
            int v = -1;
            uint32_t cs = 0;
            if (c >= kCameras && c < kCameras + kCamStride * kMaxCams && (c - kCameras) % kCamStride == 0u)
            {
                v = static_cast<int>((c - kCameras) / kCamStride);
                cs = FAST_READ32(c + kCamCs) & 0x1FFFFFFFu;
                if (!okAddr(cs, 0x20u))
                    v = -1;
            }
            float before[3] = {};
            if (v >= 0)
                for (int i = 0; i < 3; ++i)
                    before[i] = rdF(rdram, cs + 0x10 + 4 * i);
            const uint32_t ra = GPR_U32(ctx, 31);
            g_origShake(rdram, ctx, runtime);
            if (v >= 0 && ctx->pc == ra)
                for (int i = 0; i < 3; ++i)
                    g_cam[v].shake[i] = rdF(rdram, cs + 0x10 + 4 * i) - before[i];
        }

        // interpretInputs entry (a0 = PadFlags, a1 = GamePad). Returns the forced player (or -1)
        int inputEnter(uint8_t *rdram, uint32_t pf, uint32_t gp)
        {
            for (Input &in : g_in)
                if (in.pending && in.pf == pf)
                {
                    // the previous call didn't return through this hook: restore the game's values if nobody changed them
                    if (FAST_READ32(pf + kPfConfigActive) == 1u && FAST_READ32(pf + kPfConfigWanted) == 1u)
                    {
                        FAST_WRITE32(pf + kPfConfigActive, in.saved4);
                        FAST_WRITE32(pf + kPfConfigWanted, in.saved8);
                    }
                    in.pending = in.forced = false;
                }
            if (!okAddr(pf, kPfOwner + 4u) || !okAddr(gp, 0x14u))
                return -1;
            int slot = -1;
            const uint32_t owner = FAST_READ32(pf + kPfOwner);
            if (!isPlayerMonster(rdram, owner, &slot) || slot < 0 || slot > 3)
                return -1;
            Input &in = g_in[slot];
            uint8_t *b = rdram + gp;
            // mouse (wotm_mouse.hpp): the cursor stays captured for the keyboard player's whole fight (outside the pause), even when
            // another game camera takes over for a moment (fall, throw); with the orbit not driving, the movement is discarded
            if (enabled() && slot == wotm::mouse::g_kbPlayer.load(std::memory_order_relaxed))
            {
                // during the game's pause interpretInputs and Cameras::Update stop: with no signal, the window releases the cursor in ~150 ms
                if (FAST_READ32(wotm::sym::isPaused) == 0u)
                    wotm::mouse::want();
                if (!in.driving)
                    wotm::mouse::clear();
            }
            if (!in.driving || !enabled())
            {
                in.forced = false;
                in.stick[0] = in.stick[1] = 0.f;
                // with the modern camera the right stick doesn't turn the monster: in the close camera (and in the other POVs, where the
                // orbit rests) the game uses X to turn; Y (moving the head) still goes to the game
                if (enabled())
                    b[0x10] = b[0x11] = 0;
                return -1;
            }
            // right stick to the orbit, zeroed for the game
            in.stick[0] = (float(b[0x11]) - float(b[0x10])) / 255.f;
            in.stick[1] = (float(b[0x12]) - float(b[0x13])) / 255.f;
            {
                // radial dead zone: player 2's pad (idle) raised the camera by itself from the stick's slack
                const float mag = std::sqrt(in.stick[0] * in.stick[0] + in.stick[1] * in.stick[1]);
                const float scale = mag > 0.2f ? std::min(1.f, (mag - 0.2f) / 0.8f) / mag : 0.f;
                in.stick[0] *= scale;
                in.stick[1] *= scale;
            }
            b[0x10] = b[0x11] = b[0x12] = b[0x13] = 0;
            const uint32_t owner2 = owner & 0x1FFFFFFFu;
            const uint32_t e4 = FAST_READ32(pf + kPfConfigActive), e8 = FAST_READ32(pf + kPfConfigWanted);
            const bool dead = okAddr(owner2, kMonDead + 1u) && rdram[owner2 + kMonDead] != 0;
            if (dead || e4 != 0u || e8 != 0u)
            {
                in.forced = false;   // the game already asked for SR (Crush, end of glide) or is switching: go with it
                return -1;
            }
            // takes effect immediately, even while walking: waiting for the stick to be still left tank controls when leaving the close camera while walking
            // (InitCCMonsters puts the game back in CR) and the monster turned instead of walking until it stopped
            {
                // left stick deflection (bytes 0xC..0xF: left, right, up, down): SR normalizes the direction and loses it
                const float lx = (float(b[0xD]) - float(b[0xC])) / 255.f, ly = (float(b[0xE]) - float(b[0xF])) / 255.f;
                in.tilt = std::min(1.f, std::sqrt(lx * lx + ly * ly));
            }
            in.forced = true;
            in.pending = true;
            in.pf = pf;
            in.saved4 = e4;
            in.saved8 = e8;
            FAST_WRITE32(pf + kPfConfigActive, 1);
            FAST_WRITE32(pf + kPfConfigWanted, 1);
            return slot;
        }

        // interpretInputs exit (only when the function returned): restores the game's configuration
        void inputLeave(uint8_t *rdram, int slot)
        {
            if (slot < 0)
                return;
            Input &in = g_in[slot];
            if (!in.pending)
                return;
            if (FAST_READ32(in.pf + kPfConfigActive) == 1u && FAST_READ32(in.pf + kPfConfigWanted) == 1u)
            {
                FAST_WRITE32(in.pf + kPfConfigActive, in.saved4);
                FAST_WRITE32(in.pf + kPfConfigWanted, in.saved8);
                // half-deflected stick (gamepad, or Alt on the keyboard): SR writes forward/back and sideways with the normalized direction (always
                // full travel); computeTargetSpeed weights the speed by them, as in CR. Here they follow the deflection again.
                if (in.tilt < 0.98f)
                {
                    const uint32_t ring = FAST_READ32(in.pf + kPfRingIndex);
                    if (ring < 60u)
                    {
                        const uint32_t e = in.pf + ring * 0x5Eu;
                        for (const uint32_t off : {0x12u, 0x14u, 0x1Au, 0x1Cu})
                        {
                            uint16_t v;
                            std::memcpy(&v, rdram + e + off, 2);
                            v = static_cast<uint16_t>(std::lround(float(v) * in.tilt));
                            std::memcpy(rdram + e + off, &v, 2);
                        }
                    }
                }
            }
            else
                in.forced = false;   // the game changed the configuration midway (grab, death): its own wins
            in.pending = false;
        }

        // TheGame::SetOkToUnify (start of each frame): CloseCam = 0 -> nobody unifies
        PS2Runtime::RecompiledFunction g_origOkToUnify = nullptr;
        void okToUnifyHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31);
            g_origOkToUnify(rdram, ctx, runtime);
            if (ctx->pc != ra || wotm::settings::on(wotm::settings::CloseCam))
                return;
            const uint32_t slots = std::min<uint32_t>(FAST_READ32(kNumSlots), 16u);
            for (uint32_t i = 0; i < slots; ++i)
                FAST_WRITE32(kSlots + kSlotStride * i + kMonCamUnify, 0);
        }

        // Cameras::LeaveUnifiedTask (a0 = Cameras): with the pair separated (1P, no AI nearby), the game schedules leaving the close
        // camera, but the task only leaves with the monster idle (isIdle) or locked: switching to the normal camera while walking would change the controls
        // from screen-relative to tank. With the modern camera the controls stay screen-relative, so it leaves right away (except in free
        // fall), through LeaveUnifiedView itself, as the task would (with +0xE68 zeroed it doesn't remove the running task)
        PS2Runtime::RecompiledFunction g_origLeaveTask = nullptr;
        void leaveTaskHook(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
        {
            const uint32_t ra = GPR_U32(ctx, 31), self = GPR_U32(ctx, 4);
            g_origLeaveTask(rdram, ctx, runtime);
            if (ctx->pc != ra || GPR_U32(ctx, 2) == 0u || !enabled() || !FAST_READ32(kUnified))
                return;
            const uint32_t c2 = camAddr(2), mon = FAST_READ32(c2 + kCamMonster) & 0x1FFFFFFFu;
            if (!okAddr(mon, 0x284u) || rdram[mon + 0x280] != 0)   // m_freeFalling
                return;
            constexpr uint32_t kReason = wotm::sym::x_7Cameras_m_leaveReason, kAlpha = wotm::sym::x_7Cameras_m_currentFadeAlpha;
            const uint32_t reason = FAST_READ32(kReason), alpha = FAST_READ32(kAlpha), task = FAST_READ32(c2 + 0xE68u);
            FAST_WRITE32(c2 + 0xE68u, 0);
            Probe pr(rdram, ctx, runtime);
            uint32_t v0 = 0;
            float f0 = 0.f;
            const bool back = pr.call(wotm::fn::LeaveUnifiedView__7CamerasUi, {self, 0xBu}, {}, v0, f0);
            FAST_WRITE32(kReason, reason);
            FAST_WRITE32(kAlpha, alpha);
            if (!back || FAST_READ32(kUnified))
            {
                FAST_WRITE32(c2 + 0xE68u, task);   // didn't leave (mode 8): the task keeps waiting
                return;
            }
            FAST_WRITE32(wotm::sym::x_7Cameras_m_leaveTriggered, 0);
            ctx->r[2] = _mm_set_epi64x(0, 0);   // task finished
            static const bool log = std::getenv("WOTM_ORBITLOG") != nullptr;
            if (log)
                std::fprintf(stderr, "[orbit] close camera: leaving without waiting for the monster to stop\n");
        }

        void install(PS2Runtime &runtime)
        {
            constexpr uint32_t kShake = wotm::fn::shakeCamera__6Camera, kOkToUnify = wotm::fn::SetOkToUnify__7TheGame,
                               kLeaveTask = wotm::fn::LeaveUnifiedTask__7Cameras;
            if ((g_origShake = runtime.lookupFunction(kShake)))
                runtime.registerFunction(kShake, &shakeHook);
            if ((g_origOkToUnify = runtime.lookupFunction(kOkToUnify)))
                runtime.registerFunction(kOkToUnify, &okToUnifyHook);
            if ((g_origLeaveTask = runtime.lookupFunction(kLeaveTask)))
                runtime.registerFunction(kLeaveTask, &leaveTaskHook);
            std::fprintf(stderr, "[wotm] camera %s (WOTM_CAMERA), recenter %s, close camera %s (WOTM_CLOSECAM), sensitivity %d, Y axis %s%s\n",
                         wotm::settings::on(wotm::settings::Camera) ? "modern" : "classic",
                         wotm::settings::on(wotm::settings::CamRecenter) ? "on" : "off",
                         wotm::settings::on(wotm::settings::CloseCam) ? "on" : "off",
                         wotm::settings::get(wotm::settings::CamSens), wotm::settings::on(wotm::settings::CamInvertY) ? "inverted" : "normal",
                         g_aiVsAi ? "; with WOTM_AIVSAI it stays classic" : "");
        }
    }
}
