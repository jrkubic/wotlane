// ABOUTME: WOTM_TRIGCHECK=1: on the first sinf call the game runs its own sinf/cosf (reduction, Payne-Hanek and kernels, as the runtime
// ABOUTME: links them) over a table of retail values and exits with 0 if all match bit for bit. Included by wotm_overrides.cpp.
//
// The expected values come from the retail sinf/cosf (0x22D5C8/0x22D4C0) run in unicorn with cvt.w.s truncating like the R5900. The same
// result on every compiler and CPU is what lets Windows and Mac play together: the game's sine and cosine can't come from the system
// libm (Microsoft's and Apple's differ in the last bit).

#include "wotm_trig_goldens.hpp"

using wotm::TrigGolden;
using wotm::kTrigGoldens;

namespace
{
    PS2Runtime::RecompiledFunction g_origSinfForCheck = nullptr;

    // A one-argument float function of the game ($f12 -> $f0) called from inside a hook; the context is restored afterwards.
    uint32_t trigGuestCall(uint8_t *rdram, R5900Context *ctx, PS2Runtime *rt, PS2Runtime::RecompiledFunction f, uint32_t fn, uint32_t xBits)
    {
        const R5900Context saved = *ctx;
        std::memcpy(&ctx->f[12], &xBits, sizeof xBits);
        ctx->r[31] = _mm_set_epi64x(0, 0);
        ctx->pc = fn;
        f(rdram, ctx, rt);
        uint32_t out;
        std::memcpy(&out, &ctx->f[0], sizeof out);
        *ctx = saved;
        return out;
    }

    void sinfTrigCheck(uint8_t *rdram, R5900Context *ctx, PS2Runtime *runtime)
    {
        const PS2Runtime::RecompiledFunction cosf = runtime->lookupFunction(wotm::fn::cosf);
        int bad = 0;
        for (const TrigGolden &g : kTrigGoldens)
        {
            const uint32_t s = trigGuestCall(rdram, ctx, runtime, g_origSinfForCheck, wotm::fn::sinf, g.x);
            const uint32_t c = trigGuestCall(rdram, ctx, runtime, cosf, wotm::fn::cosf, g.x);
            if (s != g.sin || c != g.cos)
            {
                ++bad;
                std::fprintf(stderr, "[trigcheck] x=%08X: sin %08X (retail %08X), cos %08X (retail %08X)\n", g.x, s, g.sin, c, g.cos);
            }
        }
        const int total = int(sizeof kTrigGoldens / sizeof kTrigGoldens[0]);
        std::fprintf(stderr, "[trigcheck] %d of %d values match retail\n", total - bad, total);
        std::fflush(stderr);
        // _Exit, not exit: this runs on the EE thread while the runtime's other threads are alive, and exit's static destructors
        // (HostState's joinable std::thread) call std::terminate, so the process would abort instead of returning 0/1
        std::_Exit(bad == 0 ? 0 : 1);
    }
}

void wotmTrigCheckInstall(PS2Runtime &runtime)
{
    if (!std::getenv("WOTM_TRIGCHECK") || !(g_origSinfForCheck = runtime.lookupFunction(wotm::fn::sinf)))
        return;
    runtime.registerFunction(wotm::fn::sinf, &sinfTrigCheck);
    std::fprintf(stderr, "[trigcheck] game sine and cosine checked against retail on the first sinf call (WOTM_TRIGCHECK)\n");
}
