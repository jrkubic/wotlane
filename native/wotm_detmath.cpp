// ABOUTME: wotm_detmath.hpp's functions: a port of newlib's float fdlibm. Compiled on its own with the strict float rules (the runtime
// ABOUTME: patch and native/tests/CMakeLists.txt), so no caller's fast-math flags reach it.
#include "wotm_detmath.hpp"

// A port of newlib's float fdlibm (libm/math: sf_sin.c, sf_cos.c, kf_sin.c, kf_cos.c, ef_rem_pio2.c, kf_rem_pio2.c, sf_atan.c,
// ef_atan2.c), the family the retail game's own sinf/cosf come from: sinf/cosf equal the retail values bit for bit
// (wotm_trig_goldens.hpp). Only IEEE-exact operations are used (+ - * /, fabsf, floorf, scalbnf, int conversions), so the results only
// depend on each operation being rounded on its own: this file must be compiled without contraction into FMA and without fast math
// (/fp:precise, -ffp-contract=off -fno-fast-math).
//
// ====================================================
// Copyright (C) 1993 by Sun Microsystems, Inc. All rights reserved.
//
// Developed at SunPro, a Sun Microsystems, Inc. business.
// Permission to use, copy, modify, and distribute this
// software is freely granted, provided that this notice
// is preserved.
// ====================================================
// Conversion to float by Ian Lance Taylor, Cygnus Support, ian@cygnus.com.

#include <cmath>
#include <cstdint>
#include <cstring>

namespace wotm::detmath
{
    namespace
    {
        inline int32_t word(float f)
        {
            int32_t w;
            std::memcpy(&w, &f, 4);
            return w;
        }
        inline float fromWord(int32_t w)
        {
            float f;
            std::memcpy(&f, &w, 4);
            return f;
        }
        inline bool isFinite(int32_t ix) { return ix < 0x7f800000; }   // ix = |x|'s word
        inline bool isNan(int32_t ix) { return ix > 0x7f800000; }
        inline bool isInfinite(int32_t ix) { return ix == 0x7f800000; }

        inline float kernelSin(float x, float y, int iy)   // iy = 0 when y is zero
        {
            constexpr float half = 5.0000000000e-01f, S1 = -1.6666667163e-01f, S2 = 8.3333337680e-03f, S3 = -1.9841270114e-04f,
                            S4 = 2.7557314297e-06f, S5 = -2.5050759689e-08f, S6 = 1.5896910177e-10f;
            const int32_t ix = word(x) & 0x7fffffff;
            if (ix < 0x32000000 && static_cast<int>(x) == 0)   // |x| < 2**-27
                return x;
            const float z = x * x, v = z * x, r = S2 + z * (S3 + z * (S4 + z * (S5 + z * S6)));
            if (iy == 0)
                return x + v * (S1 + z * r);
            return x - ((z * (half * y - v * r) - y) - v * S1);
        }

        inline float kernelCos(float x, float y)
        {
            constexpr float one = 1.0f, C1 = 4.1666667908e-02f, C2 = -1.3888889225e-03f, C3 = 2.4801587642e-05f,
                            C4 = -2.7557314297e-07f, C5 = 2.0875723372e-09f, C6 = -1.1359647598e-11f;
            const int32_t ix = word(x) & 0x7fffffff;
            if (ix < 0x32000000 && static_cast<int>(x) == 0)   // |x| < 2**-27
                return one;
            const float z = x * x, r = z * (C1 + z * (C2 + z * (C3 + z * (C4 + z * (C5 + z * C6)))));
            if (ix < 0x3e99999a)   // |x| < 0.3
                return one - (0.5f * z - (z * r - x * y));
            const float qx = ix > 0x3f480000 ? 0.28125f : fromWord(ix - 0x01000000);   // x > 0.78125 ? 0.28125 : x/4
            const float hz = 0.5f * z - qx, a = one - qx;
            return a - (hz - (z * r - x * y));
        }

        // Payne-Hanek reduction for large arguments; x[] holds 8-bit integers (the float version), prec 2 = 2 floats of result
        inline int kernelRemPio2(const float *x, float *y, int e0, int nx, int prec, const int32_t *ipio2)
        {
            static const int initJk[] = {4, 7, 9};
            static const float PIo2[] = {
                1.5703125000e+00f, 4.5776367188e-04f, 2.5987625122e-05f, 7.5437128544e-08f, 6.0026650317e-11f, 7.3896444519e-13f,
                5.3845816694e-15f, 5.6378512969e-18f, 8.3009228831e-20f, 3.2756352257e-22f, 6.3331015649e-25f,
            };
            constexpr float zero = 0.0f, one = 1.0f, two8 = 2.5600000000e+02f, twon8 = 3.9062500000e-03f;
            int32_t jz, jx, jv, jp, jk, carry, n, iq[20], i, j, k, m, q0, ih;
            float z, fw, f[20], fq[20], q[20];

            jk = initJk[prec];
            jp = jk;
            jx = nx - 1;
            jv = (e0 - 3) / 8;
            if (jv < 0)
                jv = 0;
            q0 = e0 - 8 * (jv + 1);
            j = jv - jx;
            m = jx + jk;
            for (i = 0; i <= m; i++, j++)
                f[i] = j < 0 ? zero : static_cast<float>(ipio2[j]);
            for (i = 0; i <= jk; i++)
            {
                for (j = 0, fw = 0.0f; j <= jx; j++)
                    fw += x[j] * f[jx + i - j];
                q[i] = fw;
            }
            jz = jk;
        recompute:
            for (i = 0, j = jz, z = q[jz]; j > 0; i++, j--)   // distill q[] into iq[] reversingly
            {
                fw = static_cast<float>(static_cast<int32_t>(twon8 * z));
                iq[i] = static_cast<int32_t>(z - two8 * fw);
                z = q[j - 1] + fw;
            }
            z = std::scalbn(z, static_cast<int>(q0));
            z -= 8.0f * std::floor(z * 0.125f);   // trim off integer >= 8
            n = static_cast<int32_t>(z);
            z -= static_cast<float>(n);
            ih = 0;
            if (q0 > 0)   // need iq[jz-1] to determine n
            {
                i = iq[jz - 1] >> (8 - q0);
                n += i;
                iq[jz - 1] -= i << (8 - q0);
                ih = iq[jz - 1] >> (7 - q0);
            }
            else if (q0 == 0)
                ih = iq[jz - 1] >> 7;
            else if (z >= 0.5f)
                ih = 2;
            if (ih > 0)   // q > 0.5
            {
                n += 1;
                carry = 0;
                for (i = 0; i < jz; i++)   // compute 1-q
                {
                    j = iq[i];
                    if (carry == 0)
                    {
                        if (j != 0)
                        {
                            carry = 1;
                            iq[i] = 0x100 - j;
                        }
                    }
                    else
                        iq[i] = 0xff - j;
                }
                if (q0 > 0)   // rare case: chance is 1 in 12
                {
                    if (q0 == 1)
                        iq[jz - 1] &= 0x7f;
                    else if (q0 == 2)
                        iq[jz - 1] &= 0x3f;
                }
                if (ih == 2)
                {
                    z = one - z;
                    if (carry != 0)
                        z -= std::scalbn(one, static_cast<int>(q0));
                }
            }
            if (z == zero)   // check if recomputation is needed
            {
                j = 0;
                for (i = jz - 1; i >= jk; i--)
                    j |= iq[i];
                if (j == 0)
                {
                    for (k = 1; iq[jk - k] == 0; k++)   // k = number of terms needed
                    {
                    }
                    for (i = jz + 1; i <= jz + k; i++)   // add q[jz+1] to q[jz+k]
                    {
                        f[jx + i] = static_cast<float>(ipio2[jv + i]);
                        for (j = 0, fw = 0.0f; j <= jx; j++)
                            fw += x[j] * f[jx + i - j];
                        q[i] = fw;
                    }
                    jz += k;
                    goto recompute;
                }
            }
            if (z == 0.0f)   // chop off zero terms
            {
                jz -= 1;
                q0 -= 8;
                while (iq[jz] == 0)
                {
                    jz--;
                    q0 -= 8;
                }
            }
            else   // break z into 8-bit if necessary
            {
                z = std::scalbn(z, -static_cast<int>(q0));
                if (z >= two8)
                {
                    fw = static_cast<float>(static_cast<int32_t>(twon8 * z));
                    iq[jz] = static_cast<int32_t>(z - two8 * fw);
                    jz += 1;
                    q0 += 8;
                    iq[jz] = static_cast<int32_t>(fw);
                }
                else
                    iq[jz] = static_cast<int32_t>(z);
            }
            fw = std::scalbn(one, static_cast<int>(q0));   // convert the integer "bit" chunks to floating point
            for (i = jz; i >= 0; i--)
            {
                q[i] = fw * static_cast<float>(iq[i]);
                fw *= twon8;
            }
            for (i = jz; i >= 0; i--)   // PIo2[0..jp] * q[jz..0]
            {
                for (fw = 0.0f, k = 0; k <= jp && k <= jz - i; k++)
                    fw += PIo2[k] * q[i + k];
                fq[jz - i] = fw;
            }
            // compress fq[] into y[] (prec 1 and 2; sinf/cosf only use 2)
            fw = 0.0f;
            for (i = jz; i >= 0; i--)
                fw += fq[i];
            y[0] = ih == 0 ? fw : -fw;
            fw = fq[0] - fw;
            for (i = 1; i <= jz; i++)
                fw += fq[i];
            y[1] = ih == 0 ? fw : -fw;
            return n & 7;
        }

        // x - n*pi/2 as y[0] + y[1]; returns n
        inline int32_t remPio2(float x, float *y)
        {
            static const int32_t twoOverPi[] = {   // 396 hex digits (476 decimal) of 2/pi
                0xA2, 0xF9, 0x83, 0x6E, 0x4E, 0x44, 0x15, 0x29, 0xFC, 0x27, 0x57, 0xD1, 0xF5, 0x34, 0xDD, 0xC0, 0xDB, 0x62, 0x95, 0x99,
                0x3C, 0x43, 0x90, 0x41, 0xFE, 0x51, 0x63, 0xAB, 0xDE, 0xBB, 0xC5, 0x61, 0xB7, 0x24, 0x6E, 0x3A, 0x42, 0x4D, 0xD2, 0xE0,
                0x06, 0x49, 0x2E, 0xEA, 0x09, 0xD1, 0x92, 0x1C, 0xFE, 0x1D, 0xEB, 0x1C, 0xB1, 0x29, 0xA7, 0x3E, 0xE8, 0x82, 0x35, 0xF5,
                0x2E, 0xBB, 0x44, 0x84, 0xE9, 0x9C, 0x70, 0x26, 0xB4, 0x5F, 0x7E, 0x41, 0x39, 0x91, 0xD6, 0x39, 0x83, 0x53, 0x39, 0xF4,
                0x9C, 0x84, 0x5F, 0x8B, 0xBD, 0xF9, 0x28, 0x3B, 0x1F, 0xF8, 0x97, 0xFF, 0xDE, 0x05, 0x98, 0x0F, 0xEF, 0x2F, 0x11, 0x8B,
                0x5A, 0x0A, 0x6D, 0x1F, 0x6D, 0x36, 0x7E, 0xCF, 0x27, 0xCB, 0x09, 0xB7, 0x4F, 0x46, 0x3F, 0x66, 0x9E, 0x5F, 0xEA, 0x2D,
                0x75, 0x27, 0xBA, 0xC7, 0xEB, 0xE5, 0xF1, 0x7B, 0x3D, 0x07, 0x39, 0xF7, 0x8A, 0x52, 0x92, 0xEA, 0x6B, 0xFB, 0x5F, 0xB1,
                0x1F, 0x8D, 0x5D, 0x08, 0x56, 0x03, 0x30, 0x46, 0xFC, 0x7B, 0x6B, 0xAB, 0xF0, 0xCF, 0xBC, 0x20, 0x9A, 0xF4, 0x36, 0x1D,
                0xA9, 0xE3, 0x91, 0x61, 0x5E, 0xE6, 0x1B, 0x08, 0x65, 0x99, 0x85, 0x5F, 0x14, 0xA0, 0x68, 0x40, 0x8D, 0xFF, 0xD8, 0x80,
                0x4D, 0x73, 0x27, 0x31, 0x06, 0x06, 0x15, 0x56, 0xCA, 0x73, 0xA8, 0xC9, 0x60, 0xE2, 0x7B, 0xC0, 0x8C, 0x6B,
            };
            static const int32_t npio2Hw[] = {   // n*pi/2 for n = 1..32, single precision with the last 8 bits forced to 0
                0x3fc90f00, 0x40490f00, 0x4096cb00, 0x40c90f00, 0x40fb5300, 0x4116cb00, 0x412fed00, 0x41490f00,
                0x41623100, 0x417b5300, 0x418a3a00, 0x4196cb00, 0x41a35c00, 0x41afed00, 0x41bc7e00, 0x41c90f00,
                0x41d5a000, 0x41e23100, 0x41eec200, 0x41fb5300, 0x4203f200, 0x420a3a00, 0x42108300, 0x4216cb00,
                0x421d1400, 0x42235c00, 0x4229a500, 0x422fed00, 0x42363600, 0x423c7e00, 0x4242c700, 0x42490f00,
            };
            // invpio2: 24 bits of 2/pi; pio2_1: first 17 bits of pi/2, pio2_1t: pi/2 - pio2_1; pio2_2: second 17 bits,
            // pio2_2t: pi/2 - (pio2_1+pio2_2); pio2_3: third 17 bits, pio2_3t: pi/2 - (pio2_1+pio2_2+pio2_3)
            constexpr float zero = 0.0f, half = 5.0000000000e-01f, two8 = 2.5600000000e+02f, invpio2 = 6.3661980629e-01f,
                            pio2_1 = 1.5707855225e+00f, pio2_1t = 1.0804334124e-05f, pio2_2 = 1.0804273188e-05f,
                            pio2_2t = 6.0770999344e-11f, pio2_3 = 6.0770943833e-11f, pio2_3t = 6.1232342629e-17f;
            float z, w, t, r, fn, tx[3];
            int32_t i, j, n;
            const int32_t hx = word(x), ix = hx & 0x7fffffff;
            if (ix <= 0x3f490fd8)   // |x| ~<= pi/4, no reduction needed
            {
                y[0] = x;
                y[1] = 0;
                return 0;
            }
            if (ix < 0x4016cbe4)   // |x| < 3pi/4, special case with n = +-1
            {
                if (hx > 0)
                {
                    z = x - pio2_1;
                    if ((ix & 0xfffffff0) != 0x3fc90fd0)   // 24+24 bit pi OK
                    {
                        y[0] = z - pio2_1t;
                        y[1] = (z - y[0]) - pio2_1t;
                    }
                    else   // near pi/2, use 24+24+24 bit pi
                    {
                        z -= pio2_2;
                        y[0] = z - pio2_2t;
                        y[1] = (z - y[0]) - pio2_2t;
                    }
                    return 1;
                }
                z = x + pio2_1;
                if ((ix & 0xfffffff0) != 0x3fc90fd0)
                {
                    y[0] = z + pio2_1t;
                    y[1] = (z - y[0]) + pio2_1t;
                }
                else
                {
                    z += pio2_2;
                    y[0] = z + pio2_2t;
                    y[1] = (z - y[0]) + pio2_2t;
                }
                return -1;
            }
            if (ix <= 0x43490f80)   // |x| ~<= 2^7*(pi/2), medium size
            {
                t = std::fabs(x);
                n = static_cast<int32_t>(t * invpio2 + half);
                fn = static_cast<float>(n);
                r = t - fn * pio2_1;
                w = fn * pio2_1t;   // 1st round good to 40 bits
                if (n < 32 && (ix & 0xffffff00) != npio2Hw[n - 1])
                    y[0] = r - w;   // quick check: no cancellation
                else
                {
                    j = ix >> 23;
                    y[0] = r - w;
                    uint32_t high = static_cast<uint32_t>(word(y[0]));
                    i = j - static_cast<int32_t>((high >> 23) & 0xff);
                    if (i > 8)   // 2nd iteration needed, good to 57 bits
                    {
                        t = r;
                        w = fn * pio2_2;
                        r = t - w;
                        w = fn * pio2_2t - ((t - r) - w);
                        y[0] = r - w;
                        high = static_cast<uint32_t>(word(y[0]));
                        i = j - static_cast<int32_t>((high >> 23) & 0xff);
                        if (i > 25)   // 3rd iteration, 74 bits: covers all possible cases
                        {
                            t = r;
                            w = fn * pio2_3;
                            r = t - w;
                            w = fn * pio2_3t - ((t - r) - w);
                            y[0] = r - w;
                        }
                    }
                }
                y[1] = (r - y[0]) - w;
                if (hx < 0)
                {
                    y[0] = -y[0];
                    y[1] = -y[1];
                    return -n;
                }
                return n;
            }
            if (!isFinite(ix))   // all other (large) arguments
            {
                y[0] = y[1] = x - x;
                return 0;
            }
            const int e0 = static_cast<int>((ix >> 23) - 134);   // z = scalbn(|x|, ilogb(x) - 7)
            z = fromWord(ix - (static_cast<int32_t>(e0) << 23));
            for (i = 0; i < 2; i++)
            {
                tx[i] = static_cast<float>(static_cast<int32_t>(z));
                z = (z - tx[i]) * two8;
            }
            tx[2] = z;
            int nx = 3;
            while (tx[nx - 1] == zero)   // skip zero terms
                nx--;
            n = kernelRemPio2(tx, y, e0, nx, 2, twoOverPi);
            if (hx < 0)
            {
                y[0] = -y[0];
                y[1] = -y[1];
                return -n;
            }
            return n;
        }
    }

    float sinf(float x)
    {
        const int32_t ix = word(x) & 0x7fffffff;
        if (ix <= 0x3f490fd8)   // |x| ~< pi/4
            return kernelSin(x, 0.0f, 0);
        if (!isFinite(ix))   // sin(Inf or NaN) is NaN
            return x - x;
        float y[2];
        switch (remPio2(x, y) & 3)
        {
        case 0: return kernelSin(y[0], y[1], 1);
        case 1: return kernelCos(y[0], y[1]);
        case 2: return -kernelSin(y[0], y[1], 1);
        default: return -kernelCos(y[0], y[1]);
        }
    }

    float cosf(float x)
    {
        const int32_t ix = word(x) & 0x7fffffff;
        if (ix <= 0x3f490fd8)   // |x| ~< pi/4
            return kernelCos(x, 0.0f);
        if (!isFinite(ix))   // cos(Inf or NaN) is NaN
            return x - x;
        float y[2];
        switch (remPio2(x, y) & 3)
        {
        case 0: return kernelCos(y[0], y[1]);
        case 1: return -kernelSin(y[0], y[1], 1);
        case 2: return -kernelCos(y[0], y[1]);
        default: return kernelSin(y[0], y[1], 1);
        }
    }

    float atanf(float x)
    {
        static const float atanhi[] = {4.6364760399e-01f, 7.8539812565e-01f, 9.8279368877e-01f, 1.5707962513e+00f};   // atan(0.5, 1, 1.5, inf)
        static const float atanlo[] = {5.0121582440e-09f, 3.7748947079e-08f, 3.4473217170e-08f, 7.5497894159e-08f};
        static const float aT[] = {
            3.3333334327e-01f, -2.0000000298e-01f, 1.4285714924e-01f, -1.1111110449e-01f, 9.0908870101e-02f, -7.6918758452e-02f,
            6.6610731184e-02f, -5.8335702866e-02f, 4.9768779427e-02f, -3.6531571299e-02f, 1.6285819933e-02f,
        };
        constexpr float one = 1.0f, huge = 1.0e30f;
        const int32_t hx = word(x), ix = hx & 0x7fffffff;
        int id;
        if (ix >= 0x50800000)   // |x| >= 2^34
        {
            if (isNan(ix))
                return x + x;
            return hx > 0 ? atanhi[3] + atanlo[3] : -atanhi[3] - atanlo[3];
        }
        if (ix < 0x3ee00000)   // |x| < 0.4375
        {
            if (ix < 0x31000000 && huge + x > one)   // |x| < 2^-29
                return x;
            id = -1;
        }
        else
        {
            x = std::fabs(x);
            if (ix < 0x3f980000)   // |x| < 1.1875
            {
                if (ix < 0x3f300000)   // 7/16 <= |x| < 11/16
                {
                    id = 0;
                    x = (2.0f * x - one) / (2.0f + x);
                }
                else   // 11/16 <= |x| < 19/16
                {
                    id = 1;
                    x = (x - one) / (x + one);
                }
            }
            else if (ix < 0x401c0000)   // |x| < 2.4375
            {
                id = 2;
                x = (x - 1.5f) / (one + 1.5f * x);
            }
            else   // 2.4375 <= |x| < 2^66
            {
                id = 3;
                x = -1.0f / x;
            }
        }
        const float z = x * x, w = z * z;
        // the sum of aT[i] z**(i+1) for i = 0..10, split into odd and even polynomials
        const float s1 = z * (aT[0] + w * (aT[2] + w * (aT[4] + w * (aT[6] + w * (aT[8] + w * aT[10])))));
        const float s2 = w * (aT[1] + w * (aT[3] + w * (aT[5] + w * (aT[7] + w * aT[9]))));
        if (id < 0)
            return x - x * (s1 + s2);
        const float r = atanhi[id] - ((x * (s1 + s2) - atanlo[id]) - x);
        return hx < 0 ? -r : r;
    }

    float atan2f(float y, float x)
    {
        constexpr float tiny = 1.0e-30f, zero = 0.0f, pi_o_4 = 7.8539818525e-01f, pi_o_2 = 1.5707963705e+00f,
                        pi = 3.1415927410e+00f, pi_lo = -8.7422776573e-08f;
        const int32_t hx = word(x), ix = hx & 0x7fffffff, hy = word(y), iy = hy & 0x7fffffff;
        if (isNan(ix) || isNan(iy))
            return x + y;
        if (hx == 0x3f800000)   // x = 1.0
            return atanf(y);
        const int m = ((hy >> 31) & 1) | ((hx >> 30) & 2);   // 2*sign(x) + sign(y)
        if (iy == 0)   // y = 0
        {
            switch (m)
            {
            case 0:
            case 1: return y;   // atan(+-0, +anything) = +-0
            case 2: return pi + tiny;   // atan(+0, -anything) = pi
            default: return -pi - tiny;   // atan(-0, -anything) = -pi
            }
        }
        if (ix == 0)   // x = 0
            return hy < 0 ? -pi_o_2 - tiny : pi_o_2 + tiny;
        if (isInfinite(ix))
        {
            if (isInfinite(iy))
            {
                switch (m)
                {
                case 0: return pi_o_4 + tiny;
                case 1: return -pi_o_4 - tiny;
                case 2: return 3.0f * pi_o_4 + tiny;
                default: return -3.0f * pi_o_4 - tiny;
                }
            }
            switch (m)
            {
            case 0: return zero;
            case 1: return -zero;
            case 2: return pi + tiny;
            default: return -pi - tiny;
            }
        }
        if (isInfinite(iy))
            return hy < 0 ? -pi_o_2 - tiny : pi_o_2 + tiny;
        const int32_t k = (iy - ix) >> 23;
        float z;
        if (k > 60)   // |y/x| > 2**60
            z = pi_o_2 + 0.5f * pi_lo;
        else if (hx < 0 && k < -60)   // |y|/x < -2**60
            z = 0.0f;
        else
            z = atanf(std::fabs(y / x));
        switch (m)
        {
        case 0: return z;   // atan(+, +)
        case 1: return fromWord(word(z) ^ static_cast<int32_t>(0x80000000u));   // atan(-, +)
        case 2: return pi - (z - pi_lo);   // atan(+, -)
        default: return (z - pi_lo) - pi;   // atan(-, -)
        }
    }
}
