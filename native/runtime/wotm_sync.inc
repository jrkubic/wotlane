// ABOUTME: Deterministic mode and network match: fixed step and virtual clock (WOTM_FIXEDSTEP), fixed seed (WOTM_SEED), per-frame
// ABOUTME: input (delay, record/replay, network WOTM_NET), state hash and RAM dump. Included by wotm_overrides.cpp; runs at the frame boundary.
//
// Frame boundary: entry of timerWaitForMinUpdateRate (waitMinRateByVblanks), with the game's frame finished and the next one not yet
// started. The frame index restarts at the entry of Shell::LoadLevelFiles (levelLoad), so two runs with the same WOTM_MATCH compare
// the same frame of the fight even when their boots took different times.
//   WOTM_FIXEDSTEP=<n>: every frame is exactly n fields (updateRate = 60 / n, dt = n / 60) instead of the vblanks measured by the host
//     clock. Without it dt depends on how long the frame took on this machine.
//   WOTM_STATEHASH=<file>: per frame writes {index, g_frame, total hash, 256 hashes of 128 KB blocks} of the 32 MB the game sees
//     (tools/statediff.py compares two runs and reports the first frame and the blocks that differ).
//   WOTM_STATEDUMP=<index>: writes the game's RAM (32 MB) at that frame to state_<index>.bin in the current folder (the discroot).
//   WOTM_CALLTRACE=<file>[,<index>]: register hashes at every guest call while frame <index> is computed (see callTraced).
//   WOTM_CALLDUMP=<call>[,<call>...]: with WOTM_CALLTRACE, the raw VU0 and FPU registers at those calls of the trace (see dumpRegisters).
//   WOTM_PADLOG=<frames>: logs every scePad* call the game makes in its first <frames> frames (see padLogged).
//   WOTM_NET=<host>:<port>, WOTM_NETTOKEN=<token>, WOTM_NETNAME=<name>: network match through wotm_server (see "Network match" below).
namespace wotm_sync
{
    constexpr uint32_t kGuestRam = 32u * 1024u * 1024u, kBlocks = 256u, kBlockSize = kGuestRam / kBlocks;

    struct FrameHash
    {
        uint32_t index = 0, gFrame = 0;
        uint64_t total = 0;
        uint64_t blocks[kBlocks] = {};
    };

    uint64_t hashBytes(const uint8_t *p, size_t n)
    {
        uint64_t h = 0xcbf29ce484222325ull;   // FNV-1a over 64-bit words
        const uint64_t *w = reinterpret_cast<const uint64_t *>(p);
        for (size_t i = 0; i < n / 8u; ++i)
        {
            h ^= w[i];
            h *= 0x100000001b3ull;
        }
        return h;
    }

    // WOTM_SEED=<n>: fixed seed for both of the game's random generators (srand and gRand, Mersenne Twister). mathfRandInit(0) takes
    // it from the CD clock (second + minute * 60), so two runs (or two PCs) started the fight with different random sequences.
    constexpr uint32_t kRandInit = wotm::fn::mathfRandInit__Fii;
    PS2Runtime::RecompiledFunction g_origRandInit = nullptr;
    long long g_seed = -1;

    void randInitSeeded(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        if (ctx->pc == kRandInit && g_seed >= 0)
        {
            std::fprintf(stderr, "[sync] seed %lld (the game asked for %d)\n", g_seed, static_cast<int32_t>(GPR_U32(ctx, 4)));
            ctx->r[4] = _mm_set_epi64x(0, g_seed);
        }
        g_origRandInit(rdram, ctx, runtime);
    }

    // Per-frame input (the base of the netcode): scePadRead(port, slot, data) returns 32 bytes per controller (controller = port + 2 *
    // slot, players 1..8) and the game reads each controller twice per frame. With the fixed step, each controller's state (buttons and
    // sticks, wotm_net::PadState) comes from a per-frame timeline (native/net/wotm_lockstep.hpp): local devices are sampled at the frame
    // boundary and apply WOTM_INPUTDELAY frames later (the lockstep delay; default 0), and both reads of a frame see the same state.
    // The runtime formats it like a real controller (setPadOverrideState: mode and pressure come from the port's state).
    //   WOTM_INPUTREC=<file>: writes {frame, controller, state} of the input applied in every frame since the level load.
    //   WOTM_INPUTPLAY=<file>: the game sees the recorded input (by frame index) instead of the devices'.
    constexpr uint32_t kPadRead = wotm::fn::scePadRead;
    constexpr int kControllers = 8;   // 2 ports x 4 multitap slots: players 1..8 (5..8 on the AIs, wotm_multi.inc)
    struct InputRecord
    {
        uint32_t index;
        uint32_t controller;
        wotm_net::PadState state;
    };
    PS2Runtime *g_runtime = nullptr;
    PS2Runtime::RecompiledFunction g_origPadRead = nullptr;
    uint32_t g_inputDelay = 0;
    wotm_net::InputTimeline g_inputs(kControllers, 0);
    FILE *g_inputRec = nullptr;
    std::vector<InputRecord> g_inputPlay;
    bool g_levelLoaded = false;   // a level has been loaded (recording and replay count its frames)
    uint32_t g_index = 0;         // frame since the level load
    int g_mods = 0;               // gameplay modifiers of the current fight (wotm::modifiers bits; wotm_modifiers.inc latches them)
    bool g_loggedLocal = false, g_loggedApplied = false;

    // Network match (WOTM_NET=<host>:<port> and WOTM_NETTOKEN=<token>, given by the lobby: the launcher or tools/net_lobby.py). Every PC
    // runs the whole simulation in deterministic mode and only the inputs travel (native/net). The local controller (keyboard and first
    // controller, player 1's without network) drives the player the server assigned to this PC; the others come from the server.
    // Before the level load the input is neutral (the boot is the same everywhere), and a frame only starts once it has everyone's
    // inputs for it (netWaitFrame).
    std::unique_ptr<wotm_net::MatchClient> g_net;

    wotm_net::PadState inputFor(int controller, uint32_t frame)
    {
        if (g_net)
            return g_levelLoaded && controller < g_net->players() ? g_net->input(controller, frame) : wotm_net::PadState{};
        return g_inputs.get(controller, frame);
    }

    wotm_net::PadState sampleLocal(int controller)
    {
        uint8_t d[32];
        wotm_net::PadState s;
        if (g_runtime && g_runtime->padBackend().readState(controller & 1, controller >> 1, d, sizeof d))
        {
            s.buttons = static_cast<uint16_t>(d[2] | (d[3] << 8));
            s.rx = d[4];
            s.ry = d[5];
            s.lx = d[6];
            s.ly = d[7];
        }
        return s;
    }

    // start of frame `index`: samples the local controllers (they apply at index + delay)
    void sampleInputs(uint32_t index)
    {
        if (!g_inputPlay.empty())
            return;
        if (g_net)
        {
            if (!g_levelLoaded || g_net->status() != wotm_net::MatchClient::Status::Running)
                return;
            const wotm_net::PadState s = sampleLocal(0);
            g_net->pushLocal(index + g_inputDelay, s);
            if (!g_loggedLocal && s != wotm_net::PadState{})
            {
                g_loggedLocal = true;
                std::fprintf(stderr, "[sync] first local input: sample of frame %u applies at frame %u\n", index, index + g_inputDelay);
            }
            return;
        }
        for (int c = 0; c < kControllers; ++c)
        {
            const wotm_net::PadState s = sampleLocal(c);
            const uint32_t at = g_inputs.scheduleLocal(c, index, s);
            if (g_levelLoaded && !g_loggedLocal && s != wotm_net::PadState{})
            {
                g_loggedLocal = true;
                std::fprintf(stderr, "[sync] first local input: sample of frame %u applies at frame %u\n", index, at);
            }
        }
        g_inputs.discardBefore(index > 64u ? index - 64u : 0u);
    }

    void padReadLatched(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t port = GPR_U32(ctx, 4), slot = GPR_U32(ctx, 5);
        const bool ours = ctx->pc == kPadRead && port <= 1u && slot <= 3u;
        const wotm_net::PadState s = ours ? inputFor(static_cast<int>(port + 2u * slot), g_index) : wotm_net::PadState{};
        if (ours)
            ps2_stubs::setPadOverrideState(s.buttons, s.lx, s.ly, s.rx, s.ry);
        g_origPadRead(rdram, ctx, runtime);
        if (!ours)
            return;
        ps2_stubs::clearPadOverrideState();
        if (g_levelLoaded && !g_loggedApplied && s != wotm_net::PadState{})
        {
            g_loggedApplied = true;
            std::fprintf(stderr, "[sync] first non-neutral input applied at frame %u\n", g_index);
        }
    }

    int g_fixedStep = 0;
    uint32_t g_virtualTics = 0;   // the game's clock in fixed step (timer 1 ticks: 9619 per field), advanced in frame()
    uint32_t g_ticCalls = 0;      // the game's timerGetCurTics calls in this frame
    // timerGetCurTics in fixed step: the frame's virtual clock plus one quantum per call (64 ticks, ~0.1 ms). That way the loops that
    // wait for the clock to move (fadeOutMusicBlocking: 101 ms) finish, and the time the game measures depends only on the frame and
    // on how many times it asked, never on how long the host took.
    uint32_t callTics() { return g_virtualTics + 64u * g_ticCalls++; }

    // sceGsSyncV returns the field parity (the game keeps it in RAM); in fixed step it comes from a count of the calls
    constexpr uint32_t kSyncV = wotm::fn::sceGsSyncV;
    PS2Runtime::RecompiledFunction g_origSyncV = nullptr;
    uint32_t g_syncCalls = 0;

    void syncVParity(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const bool entry = ctx->pc == kSyncV;
        const uint32_t ra = GPR_U32(ctx, 31);
        g_origSyncV(rdram, ctx, runtime);
        if (entry && ctx->pc == ra)
            SET_GPR_U32(ctx, 2, g_syncCalls++ & 1u);
    }

    // sceCdReadClock(sceCdCLOCK *) in fixed step: a fixed date (00:00:00 on 01/01/03, BCD) instead of the PC's clock; the game uses it
    // in globalTimeString and, with seed 0, to seed the random generators
    constexpr uint32_t kReadClock = wotm::fn::sceCdReadClock;

    void readClockFixed(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t p = GPR_U32(ctx, 4) & 0x1FFFFFFFu;
        const uint8_t clock[8] = {0, 0x00, 0x00, 0x00, 0, 0x01, 0x01, 0x03};   // stat, second, minute, hour, -, day, month, year
        if (p + sizeof clock <= kGuestRam)
            std::memcpy(rdram + p, clock, sizeof clock);
        SET_GPR_U32(ctx, 2, 1u);
        ctx->pc = GPR_U32(ctx, 31);
    }
    FILE *g_hashFile = nullptr;
    long g_dumpAt = -1;

    // WOTM_CALLTRACE=<file>[,<index>]: while the game computes frame <index> (default 1, the first update after the frame-0 state)
    // writes one CallRecord per guest call. Registers only, no RAM: comparing two machines' files (calltrace.py in wotlane-coord),
    // the first record whose hashes differ names the function that computed the first different value (the one running since the
    // previous record). Threads are numbered in order of first call, the same on every machine with the one-thread EE scheduler.
    struct CallRecord
    {
        uint32_t index, target, ra, thread;
        uint64_t gpr, fpr, vu0;
    };
    FILE *g_traceFile = nullptr;
    uint32_t g_traceAt = 1;
    std::vector<const R5900Context *> g_traceThreads;
    // WOTM_CALLDUMP=<call>[,<call>...]: at those records of the trace, the raw VU0 and FPU registers in hex on stderr, to see which
    // register a differing vu0/fpr hash comes from (and whether it holds a NaN/infinity, which x86 and ARM encode differently)
    std::vector<uint32_t> g_dumpCalls;
    uint32_t g_traceCalls = 0;

    void dumpRegisters(const R5900Context *ctx, uint32_t call)
    {
        auto lanes = [](const __m128 &v, uint32_t (&w)[4]) { std::memcpy(w, &v, sizeof w); };
        uint32_t w[4];
        for (int k = 0; k < 32; ++k)
        {
            lanes(ctx->vu0_vf[k], w);
            std::fprintf(stderr, "[sync] call %u vf%02d %08x %08x %08x %08x\n", call, k, w[0], w[1], w[2], w[3]);
        }
        lanes(ctx->vu0_acc, w);
        std::fprintf(stderr, "[sync] call %u acc  %08x %08x %08x %08x\n", call, w[0], w[1], w[2], w[3]);
        uint32_t q, p, i;
        std::memcpy(&q, &ctx->vu0_q, 4);
        std::memcpy(&p, &ctx->vu0_p, 4);
        std::memcpy(&i, &ctx->vu0_i, 4);
        std::fprintf(stderr, "[sync] call %u q %08x p %08x i %08x\n", call, q, p, i);
        for (int k = 0; k < 32; k += 8)
        {
            uint32_t f[8];
            std::memcpy(f, &ctx->f[k], sizeof f);
            std::fprintf(stderr, "[sync] call %u f%02d-f%02d %08x %08x %08x %08x %08x %08x %08x %08x\n", call, k, k + 7, f[0], f[1], f[2],
                         f[3], f[4], f[5], f[6], f[7]);
        }
    }

    void callTraced(const R5900Context *ctx, uint32_t target)
    {
        if (!g_levelLoaded || g_index != g_traceAt || !g_traceFile)
            return;
        size_t thread = static_cast<size_t>(std::find(g_traceThreads.begin(), g_traceThreads.end(), ctx) - g_traceThreads.begin());
        if (thread == g_traceThreads.size())
            g_traceThreads.push_back(ctx);
        struct
        {
            __m128 vf[32], acc;
            float q, p, i, pad;
        } vu{};
        std::memcpy(vu.vf, ctx->vu0_vf, sizeof vu.vf);
        vu.acc = ctx->vu0_acc;
        vu.q = ctx->vu0_q;
        vu.p = ctx->vu0_p;
        vu.i = ctx->vu0_i;
        const CallRecord r{g_index, target, GPR_U32(ctx, 31), static_cast<uint32_t>(thread),
                           hashBytes(reinterpret_cast<const uint8_t *>(ctx->r), sizeof ctx->r),
                           hashBytes(reinterpret_cast<const uint8_t *>(ctx->f), sizeof ctx->f),
                           hashBytes(reinterpret_cast<const uint8_t *>(&vu), sizeof vu)};
        std::fwrite(&r, sizeof r, 1, g_traceFile);
        if (std::find(g_dumpCalls.begin(), g_dumpCalls.end(), g_traceCalls) != g_dumpCalls.end())
            dumpRegisters(ctx, g_traceCalls);
        ++g_traceCalls;
    }

    void callTraceInit(const char *spec)
    {
        std::string path(spec);
        if (const size_t comma = path.rfind(','); comma != std::string::npos)
        {
            g_traceAt = static_cast<uint32_t>(std::atol(path.c_str() + comma + 1));
            path.resize(comma);
        }
        if (!(g_traceFile = std::fopen(path.c_str(), "wb")))
            return (void)std::fprintf(stderr, "[sync] WOTM_CALLTRACE: cannot write %s\n", path.c_str());
        g_wotmCallTrace = &callTraced;
        std::fprintf(stderr, "[sync] call trace of frame %u into %s\n", g_traceAt, path.c_str());
        if (const char *e = std::getenv("WOTM_CALLDUMP"); e && *e)
            for (const char *s = e; *s;)
            {
                char *end = nullptr;
                g_dumpCalls.push_back(static_cast<uint32_t>(std::strtoul(s, &end, 10)));
                s = *end == ',' ? end + 1 : end + std::strlen(end);
            }
    }

    // frame boundary after frame `index`: the traced frame is complete
    void callTraceFrameDone(uint32_t index)
    {
        if (!g_traceFile || !g_levelLoaded || index != g_traceAt)   // boot frames count from 0 too, before the level load
            return;
        g_wotmCallTrace = nullptr;
        const long bytes = std::ftell(g_traceFile);
        std::fclose(g_traceFile);
        g_traceFile = nullptr;
        std::fprintf(stderr, "[sync] call trace of frame %u: %ld calls, %zu thread(s)\n", index, bytes / static_cast<long>(sizeof(CallRecord)),
                     g_traceThreads.size());
    }
    // WOTM_PADLOG=<frames>: logs the game's scePadGetState/InfoMode/SetMainMode/Read calls (port, slot, a2, a3, return value and the
    // first 8 bytes scePadRead wrote) during frames 0..<frames>-1 after the level load. The pad stubs answer from this machine (ports
    // open, pad mode, local devices): two machines in one match must log the same lines.
    uint32_t g_padLogFrames = 0;
    constexpr uint32_t kPadLogFns[] = {wotm::fn::scePadGetState, wotm::fn::scePadInfoMode, wotm::fn::scePadSetMainMode, wotm::fn::scePadRead};
    constexpr const char *kPadLogNames[] = {"scePadGetState", "scePadInfoMode", "scePadSetMainMode", "scePadRead"};
    PS2Runtime::RecompiledFunction g_padLogOrig[4] = {};

    template <int K>
    void padLogged(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t port = GPR_U32(ctx, 4), slot = GPR_U32(ctx, 5), a2 = GPR_U32(ctx, 6), a3 = GPR_U32(ctx, 7);
        g_padLogOrig[K](rdram, ctx, runtime);
        if (!g_levelLoaded || g_index >= g_padLogFrames)
            return;
        std::fprintf(stderr, "[padlog] frame %u %s(port %u, slot %u, %08x, %08x) -> %d", g_index, kPadLogNames[K], port, slot, a2, a3,
                     GPR_S32(ctx, 2));
        if (K == 3)
            std::fprintf(stderr, " data %08x %08x", Ps2FastRead32(rdram, a2), Ps2FastRead32(rdram, a2 + 4));
        std::fputc('\n', stderr);
    }

    template <int... K>
    void padLogInstall(PS2Runtime &runtime, std::integer_sequence<int, K...>)
    {
        const PS2Runtime::RecompiledFunction hooks[] = {&padLogged<K>...};
        for (int k = 0; k < 4; ++k)
            if ((g_padLogOrig[k] = runtime.lookupFunction(kPadLogFns[k])))
                runtime.registerFunction(kPadLogFns[k], hooks[k]);
        std::fprintf(stderr, "[sync] pad log of frames 0..%u (WOTM_PADLOG)\n", g_padLogFrames - 1u);
    }

    // WOTM_STATERING=<n>: copy of the RAM of the last n frames, written (state_<index>.bin) when the network match stops; a desync is
    // only seen at the next hash (up to 32 frames later), so the copy of the first differing frame is still here
    // WOTM_STATERING=<n>:<start>+<size>,...: only those ranges (the rest of the file is zeros); copying 32 MB per frame changes the
    // frame time enough to hide a desync that depends on it
    std::vector<std::vector<uint8_t>> g_ring;
    std::vector<uint32_t> g_ringIndex;
    std::vector<std::pair<uint32_t, uint32_t>> g_ringRanges;   // {start, size}

    void ringKeep(const uint8_t *rdram, uint32_t index)
    {
        const size_t k = index % g_ring.size();
        g_ring[k].resize(kGuestRam);
        for (const auto &[at, size] : g_ringRanges)
            std::memcpy(g_ring[k].data() + at, rdram + at, size);
        g_ringIndex[k] = index;
    }

    void ringInit(const char *spec)
    {
        g_ring.resize(static_cast<size_t>(std::clamp(std::atoi(spec), 1, 64)));
        g_ringIndex.assign(g_ring.size(), 0u);
        for (const char *p = std::strchr(spec, ':'); p && *p; p = std::strchr(p, ','))
        {
            char *end = nullptr;
            const uint32_t at = static_cast<uint32_t>(std::strtoul(p + 1, &end, 0));
            const uint32_t size = end && *end == '+' ? static_cast<uint32_t>(std::strtoul(end + 1, &end, 0)) : 0u;
            if (size && at < kGuestRam)
                g_ringRanges.emplace_back(at, std::min(size, kGuestRam - at));
            p = end;
        }
        if (g_ringRanges.empty())
            g_ringRanges.emplace_back(0u, kGuestRam);
    }

    void ringWrite()
    {
        for (size_t k = 0; k < g_ring.size(); ++k)
        {
            if (g_ring[k].empty())
                continue;
            const std::string name = "state_" + std::to_string(g_ringIndex[k]) + ".bin";
            if (FILE *f = std::fopen(name.c_str(), "wb"))
            {
                std::fwrite(g_ring[k].data(), 1, g_ring[k].size(), f);
                std::fclose(f);
            }
        }
        std::fprintf(stderr, "[sync] RAM of the last %zu frames written (state_<index>.bin)\n", g_ring.size());
        g_ring.clear();
    }

    void loadInputPlay(const char *path)
    {
        if (FILE *f = std::fopen(path, "rb"))
        {
            InputRecord r;
            while (std::fread(&r, sizeof r, 1, f) == 1)
                g_inputPlay.push_back(r);
            std::fclose(f);
        }
        std::fprintf(stderr, "[sync] recorded input: %zu records from %s\n", g_inputPlay.size(), path);
    }

    // ---- network match ----
    bool stopRequested() { return g_runtime && g_runtime->isStopRequested(); }

    // the match can't go on (refused, desync, dropped connection): the message stays on screen and the game halts until the window closes
    void netFail(const std::string &why)
    {
        std::fprintf(stderr, "[net] stopped: %s\n", why.c_str());
        wotm_netstatus::setMessage(why + "  (close the window)");
        wotm_netstatus::g_waiting = true;
        if (!g_ring.empty())
            ringWrite();
        while (!stopRequested())
            std::this_thread::sleep_for(std::chrono::milliseconds(100));
    }

    std::string netNames(const std::vector<int> &players)
    {
        const std::vector<std::string> names = g_net->names();
        std::string s;
        for (int p : players)
            s += (s.empty() ? "" : ", ") + (p < static_cast<int>(names.size()) ? names[static_cast<size_t>(p)] : "player " + std::to_string(p + 1));
        return s;
    }

    // Entry of Shell::LoadLevelFiles: waits for everyone to reach the match (the server measures the ping and picks the delay)
    void netStart()
    {
        using Status = wotm_net::MatchClient::Status;
        wotm_netstatus::g_waiting = true;
        while (!g_net->waitStarted(100) && !stopRequested())
        {
            const Status s = g_net->status();
            if (s != Status::Connecting && s != Status::Waiting)
                return netFail(g_net->message());
            wotm_netstatus::setMessage(s == Status::Connecting ? g_net->message() : "Waiting for the other players to reach the match");
        }
        if (stopRequested())
            return;   // window closed before the match started
        wotm_netstatus::g_waiting = false;
        wotm_netstatus::setMessage("");
        g_inputDelay = g_net->delay();
        wotm_netstatus::g_delay = g_inputDelay;
        std::fprintf(stderr, "[net] start: player %d of %d, delay %u frames, ping %u ms\n", g_net->localPlayer() + 1, g_net->players(),
                     g_inputDelay, g_net->rttMs());
    }

    // End of the frame: the next one only starts with everyone's inputs for it. Stalled for more than 250 ms, the screen says who is
    // missing; afterwards shiftHostDeadlines drops the vsyncs that fell due during the stall (without that, they became a burst of
    // frames), and the game scheduler's vblank grid, which kept running, gives the next one on time.
    uint32_t g_netStalls = 0;
    double g_netStallMs = 0.0;

    void netWaitFrame(uint32_t frame)
    {
        wotm_netstatus::g_pingMs = g_net->rttMs();
        if (g_net->waitFrame(frame, 0))
            return;
        const auto t0 = std::chrono::steady_clock::now();
        wotm_netstatus::g_waiting = true;
        bool shown = false;
        while (!g_net->waitFrame(frame, 50) && !stopRequested())
        {
            if (g_net->status() != wotm_net::MatchClient::Status::Running)
                return netFail(g_net->message());
            if (std::chrono::steady_clock::now() - t0 > std::chrono::milliseconds(250))
            {
                wotm_netstatus::setMessage("Waiting for " + netNames(g_net->missing(frame)));
                shown = true;
            }
        }
        if (shown)
            wotm_netstatus::setMessage("");
        wotm_netstatus::g_waiting = false;
        const auto waited = std::chrono::steady_clock::now() - t0;
        g_runtime->eeScheduler().shiftHostDeadlines(waited);
        ++g_netStalls;
        g_netStallMs += std::chrono::duration<double, std::milli>(waited).count();
        if ((g_netStalls & 63u) == 1u)
            std::fprintf(stderr, "[net] frame %u: %u waits for the network, %.0f ms in total\n", frame, g_netStalls, g_netStallMs);
    }

    // Hash of the game's RAM for the server to compare between PCs: 1 MB per frame, 32 frames per hash (spreads the cost)
    constexpr uint32_t kNetHashFrames = 32u, kNetHashSlice = kGuestRam / kNetHashFrames;
    uint64_t g_netHash = 0;
    uint32_t g_netHashes = 0;

    void netHash(const uint8_t *rdram, uint32_t index)
    {
        const uint32_t k = index % kNetHashFrames;
        if (k == 0u)
            g_netHash = 0xcbf29ce484222325ull;
        g_netHash = (g_netHash ^ hashBytes(rdram + k * kNetHashSlice, kNetHashSlice)) * 0x100000001b3ull;
        if (k != kNetHashFrames - 1u)
            return;
        g_net->sendHash(index, g_netHash);
        if ((g_netHashes++ % 16u) == 0u)
            std::fprintf(stderr, "[net] hash of frame %u: %016llx\n", index, static_cast<unsigned long long>(g_netHash));
    }

    // Build identity (every PC needs the same exe): wotm.buildid next to the exe, written by tools\build_runtime.ps1
    std::string netBuildId()
    {
        char path[1024] = {};
        const unsigned long n = GetModuleFileNameA(nullptr, path, sizeof path);
        std::string dir(path, n);
        dir = dir.substr(0, dir.find_last_of("\\/") + 1);
        std::string id;
        if (FILE *f = std::fopen((dir + "wotm.buildid").c_str(), "rb"))
        {
            char buf[128] = {};
            const size_t r = std::fread(buf, 1, sizeof buf - 1, f);
            std::fclose(f);
            id.assign(buf, r);
            while (!id.empty() && (id.back() == '\n' || id.back() == '\r' || id.back() == ' '))
                id.pop_back();
        }
        return id.empty() ? "sem-buildid" : id;   // sentinel shared with tools/net_lobby.py and the launcher: keep the three equal
    }

    // Settings that change the game state (docs/plans/determinism-audit.md 1.7): the same on every PC or the server refuses
    std::string netConfigHash()
    {
        static const char *const kVars[] = {"WOTM_MATCH", "WOTM_PLAYERS", "WOTM_AIVSAI", "WOTM_RATEMIN", "WOTM_FIXEDSTEP", "WOTM_SEED",
                                            "WOTM_HIGHRAM", "WOTM_ENEMYBARS", "WOTM_CHEATS", "WOTM_CLOAK", "WOTM_SAVEJSON",
                                            "WOTM_FREECAM", "WOTM_KILLTARGET", "WOTM_30HZ", "WOTM_MODE", "PS2X_FMV", "PS2X_SKIP_MOVIES", "PS2X_SKIP_CMOVIE", "PS2X_NOOP"};
        std::string all;
        for (const char *v : kVars)
        {
            const char *e = std::getenv(v);
            all += std::string(v) + "=" + (e ? e : "") + ";";
        }
        // the enabled mods change the game data: the same set (names and contents) on every PC, or the server refuses
        char mods[17];
        std::snprintf(mods, sizeof mods, "%016llx", static_cast<unsigned long long>(wotm::mods::digest(wotm::mods::Registry::get().dir())));
        all += std::string("mods=") + mods + ";";
        std::fprintf(stderr, "[net] mods: %s\n", mods);
        // gameplay modifiers (the lobby's WOTM_MODIFIERS) in canonical order: "turbo,jump" and "jump,turbo" are the same match
        int modBits = 0;
        const char *modList = std::getenv("WOTM_MODIFIERS");
        wotm::modifiers::parse(modList ? modList : "", modBits);
        all += "modifiers=" + wotm::modifiers::format(modBits) + ";";
        all.resize((all.size() + 7u) & ~size_t(7), '\0');   // hashBytes reads 8-byte words
        char out[17];
        std::snprintf(out, sizeof out, "%016llx", static_cast<unsigned long long>(hashBytes(reinterpret_cast<const uint8_t *>(all.data()), all.size())));
        return out;
    }

    void netInit(const char *addr)
    {
        std::string a(addr);
        const size_t colon = a.rfind(':');
        wotm_net::ClientConfig c;
        c.host = colon == std::string::npos ? a : a.substr(0, colon);
        c.port = colon == std::string::npos ? wotm_net::proto::kDefaultPort : static_cast<uint16_t>(std::atoi(a.c_str() + colon + 1));
        const char *token = std::getenv("WOTM_NETTOKEN"), *name = std::getenv("WOTM_NETNAME");
        c.token = token ? token : "";
        c.name = name && *name ? name : "player";
        c.buildId = netBuildId();
        c.configHash = netConfigHash();
        g_net = std::make_unique<wotm_net::MatchClient>();
        std::string err;
        if (!g_net->start(c, err))
        {
            std::fprintf(stderr, "[net] couldn't start the network: %s\n", err.c_str());
            g_net.reset();
            return;
        }
        wotm_netstatus::g_active = true;
        wotm_netstatus::setMessage("Connecting to " + c.host + ":" + std::to_string(c.port));
        std::fprintf(stderr, "[net] connecting to %s:%u as %s (build %s, config %s)\n", c.host.c_str(), c.port, c.name.c_str(),
                     c.buildId.c_str(), c.configHash.c_str());
    }

    // rtPauseRT (Start or controller unplugged) calls rtReturnToShell(2, field) with the FIELD bit of the GS CSR, which the runtime's
    // scheduler flips on every host vblank: in the same frame one PC stored 1 and the other 0 (gRtReturn + 4) and the pause went on
    // differently. In fixed step the field comes from the virtual parity, the same one sceGsSyncV returns (syncVParity). It's the only
    // reader of the bit that keeps the value in the game's RAM (the others only program the GS: ieGsPutDispEnv, setPMODE, movies)
    constexpr uint32_t kRtReturnToShell = wotm::fn::rtReturnToShell__Fii, kRtPauseRT = wotm::fn::rtPauseRT__FUi;
    constexpr uint32_t kRtPauseRTSize = 0x278u;
    PS2Runtime::RecompiledFunction g_origRtReturn = nullptr;

    void rtReturnFixedField(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const uint32_t ra = GPR_U32(ctx, 31);
        if (ctx->pc == kRtReturnToShell && ra - kRtPauseRT < kRtPauseRTSize)
            SET_GPR_U32(ctx, 5, g_syncCalls & 1u);
        g_origRtReturn(rdram, ctx, runtime);
    }

    void init(PS2Runtime &runtime)
    {
        g_runtime = &runtime;
        if (const char *e = std::getenv("WOTM_SEED"); e && *e && (g_origRandInit = runtime.lookupFunction(kRandInit)))
        {
            g_seed = std::clamp(std::atoll(e), 1LL, 0x7FFFFFFFLL);   // 0 would make the game read the clock
            runtime.registerFunction(kRandInit, &randInitSeeded);
        }
        if (const char *e = std::getenv("WOTM_FIXEDSTEP"); e && *e)
        {
            g_fixedStep = std::clamp(std::atoi(e), 1, 4);
            if ((g_origSyncV = runtime.lookupFunction(kSyncV)))
                runtime.registerFunction(kSyncV, &syncVParity);
            if ((g_origRtReturn = runtime.lookupFunction(kRtReturnToShell)))
                runtime.registerFunction(kRtReturnToShell, &rtReturnFixedField);
            runtime.registerFunction(kReadClock, &readClockFixed);
            if ((g_origPadRead = runtime.lookupFunction(kPadRead)))
                runtime.registerFunction(kPadRead, &padReadLatched);
            if (const char *r = std::getenv("WOTM_INPUTREC"); r && *r)
                g_inputRec = std::fopen(r, "wb");
            if (const char *p = std::getenv("WOTM_INPUTPLAY"); p && *p)
                loadInputPlay(p);
            if (const char *d = std::getenv("WOTM_INPUTDELAY"); d && *d)
                g_inputDelay = static_cast<uint32_t>(std::clamp(std::atoi(d), 0, 30));
            g_inputs = wotm_net::InputTimeline(kControllers, g_inputDelay);
            sampleInputs(0);
            std::fprintf(stderr, "[sync] input delay: %u frame(s)\n", g_inputDelay);
        }
        if (const char *e = std::getenv("WOTM_NET"); e && *e)
        {
            if (g_fixedStep)
                netInit(e);
            else
                std::fprintf(stderr, "[net] WOTM_NET needs WOTM_FIXEDSTEP (the launcher and tools/net_lobby.py set it): network off\n");
        }
        if (const char *e = std::getenv("WOTM_STATEHASH"); e && *e)
            g_hashFile = std::fopen(e, "wb");
        if (const char *e = std::getenv("WOTM_STATEDUMP"); e && *e)
            g_dumpAt = std::atol(e);
        if (const char *e = std::getenv("WOTM_STATERING"); e && *e)
            ringInit(e);
        if (const char *e = std::getenv("WOTM_CALLTRACE"); e && *e)
            callTraceInit(e);
        if (const char *e = std::getenv("WOTM_PADLOG"); e && (g_padLogFrames = static_cast<uint32_t>(std::atol(e))) > 0)
            padLogInstall(runtime, std::make_integer_sequence<int, 4>{});
        if (g_fixedStep || g_hashFile || g_dumpAt >= 0)
            std::fprintf(stderr, "[sync] fixed step %d field(s), hash %s, dump at frame %ld\n", g_fixedStep, g_hashFile ? "on" : "off", g_dumpAt);
    }

    // Entry of Shell::LoadLevelFiles (loadLevelFilesTapped): the frame index restarts right here, before the fight's first frame
    // (that frame's pad reads use index 0 in both runs, however long the boot took)
    void levelLoad()
    {
        if (g_net && g_levelLoaded)
            std::fprintf(stderr, "[net] WARNING: second level load in the network match (the frame index restarts; it will desync)\n");
        g_index = 0;
        g_levelLoaded = true;
        if (!g_fixedStep)
            return;
        if (g_net)
            netStart();
        g_inputs = wotm_net::InputTimeline(kControllers, g_inputDelay);
        for (const InputRecord &r : g_inputPlay)
            g_inputs.set(static_cast<int>(r.controller), r.index, r.state);
        sampleInputs(0);
    }

    // fieldCounterAtStart/fieldCounter: D_006F8DC0/D_006F8DBC; fieldsForAverage: D_006F8DE4 (fields since timerSetFrameStart)
    void frame(uint8_t *rdram, uint32_t fieldCounter, uint32_t fieldCounterAtStart, uint32_t fieldsForAverage)
    {
        if (g_fixedStep)
        {
            const uint32_t step = static_cast<uint32_t>(wotm::modifiers::fieldsPerFrame(g_fixedStep, g_mods));
            Ps2FastWrite32(rdram, fieldCounter, Ps2FastRead32(rdram, fieldCounterAtStart) + step);
            Ps2FastWrite32(rdram, fieldsForAverage, Ps2FastRead32(rdram, fieldsForAverage) + step);
            g_virtualTics += step * 9619u;
            g_ticCalls = 0;
            // sound advances by the frame's real length, on the game thread (when each sound ends becomes a function of the frames);
            // in a turbo fight that is 1 vsync for 2 fields, so sound keeps its normal speed instead of overfilling the output
            wotmSoundFrame(static_cast<unsigned>(wotm::modifiers::vsyncsPerFrame(g_fixedStep, g_mods)));
            if (g_inputRec && g_levelLoaded)
            {
                for (int c = 0; c < kControllers; ++c)
                {
                    const InputRecord r{g_index, static_cast<uint32_t>(c), inputFor(c, g_index)};
                    std::fwrite(&r, sizeof r, 1, g_inputRec);
                }
                if ((g_index & 63u) == 0u)
                    std::fflush(g_inputRec);
            }
            sampleInputs(g_index + 1u);
        }
        const uint32_t index = g_index++;
        callTraceFrameDone(index);
        if (g_hashFile)
        {
            FrameHash fh;
            fh.index = index;
            fh.gFrame = Ps2FastRead32(rdram, wotm::sym::g_frame);
            for (uint32_t b = 0; b < kBlocks; ++b)
                fh.blocks[b] = hashBytes(rdram + b * kBlockSize, kBlockSize);
            fh.total = hashBytes(reinterpret_cast<const uint8_t *>(fh.blocks), sizeof fh.blocks);
            std::fwrite(&fh, sizeof fh, 1, g_hashFile);
            if ((index & 63u) == 0u)
                std::fflush(g_hashFile);
        }
        if (!g_ring.empty() && g_levelLoaded)
            ringKeep(rdram, index);
        if (g_dumpAt >= 0 && index == static_cast<uint32_t>(g_dumpAt))
        {
            const std::string name = "state_" + std::to_string(index) + ".bin";
            if (FILE *f = std::fopen(name.c_str(), "wb"))
            {
                std::fwrite(rdram, 1, kGuestRam, f);
                std::fclose(f);
                std::fprintf(stderr, "[sync] RAM of frame %u in %s\n", index, name.c_str());
            }
        }
        if (g_net && g_levelLoaded && g_fixedStep)
        {
            netHash(rdram, index);
            netWaitFrame(g_index);
        }
    }
}
