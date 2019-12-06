/*
* Copyright(c) 2019 Intel Corporation
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

/*
 * Copyright (c) 2016, Alliance for Open Media. All rights reserved
 *
 * This source code is subject to the terms of the BSD 2 Clause License and
 * the Alliance for Open Media Patent License 1.0. If the BSD 2 Clause License
 * was not distributed with this source code in the LICENSE file, you can
 * obtain it at www.aomedia.org/license/software. If the Alliance for Open
 * Media Patent License 1.0 was not distributed with this source code in the
 * PATENTS file, you can obtain it at www.aomedia.org/license/patent.
 */

#ifndef CABAC_READER_SSE4_1_H_
#define CABAC_READER_SSE4_1_H_

#include <assert.h>
#include "smmintrin.h"

static const int nsymbs2speed[17] = { 0, 0, 1, 1, 2, 2, 2, 2, 2,
                                      2, 2, 2, 2, 2, 2, 2, 2 };

static int16_t num_array[8] = { 0, 1, 2, 3, 4, 5, 6, 7 };
DECLARE_ALIGNED(16, static int8_t, b_mask[5][16]) = {
                         { 0, 0, 0, 0 , 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                         { 0, 0, 0, 0 , 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                         { 1, 1, 0, 0 , 0, 0, 0, 0, 0, 0, 0, 0 , 0, 0, 0, 0},
                         { 1, 1, 1, 1 , 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0 },
                         { 1, 1, 1, 1 , 1, 1, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0} };

static AOM_FORCE_INLINE void dec_update_cdf_sse4(AomCdfProb *cdf, int8_t val, int nsymbs) {
    const unsigned count = cdf[nsymbs];
    const unsigned rate = 3 + (count >> 4) + nsymbs2speed[nsymbs];

    // load cdf
    __m128i cdf_1 = _mm_loadl_epi64((__m128i *)cdf);
    __m128i cdf_2 = cdf_1;
    __m128i num = _mm_load_si128((__m128i *)num_array);
    __m128i round = _mm_set1_epi16((int16_t)(1 << rate) - 1);

    // mask computation
    __m128i m2, m3;
    m3 = _mm_set1_epi16((int16_t)val); // val val val val  val val val val
    m2 = _mm_xor_si128(m3, m3); // 0 0 0 0  0 0 0 0
    m3 = _mm_cmpgt_epi16(m3, num); // -1 -1 0 0  0 0 0 0
    m2 = _mm_avg_epu16(m3, m2); // -32768 -32768 0  0 0 0 0
    round = _mm_and_si128(round, m3); // round round 0 0  0 0 0 0

    // cdf computation
    m3 = _mm_add_epi16(cdf_1, m2); // cdf + 0/(-1)
    m3 = _mm_add_epi16(m3, round); // (cdf + 0/(- 32768) + round)
    m2 = _mm_srai_epi16(m3, rate); // (cdf + 0/(- 32768) + round) >> rate
    cdf_1 = _mm_sub_epi16(cdf_1, m2); // cdf[i] += (cdf + 0/(- 32768) + round) >> rate + (-1)

    m2 = _mm_loadl_epi64((__m128i *)b_mask[nsymbs]);
    cdf_2 = _mm_blendv_epi8(cdf_1, cdf_2, m2);

    // store cdf
    _mm_storel_epi64((__m128i *)cdf, cdf_2);
    cdf[nsymbs] = count + (count < 32);
}

static int32_t n_ret[5/*nsymbol*/][4] = { { 0, 0, 0, 0 },
                                          { 0, -4, -8, -12 },
                                          { 4, 0, -4, -8 },
                                          { 8, 4, 0, -4 },
                                          { 12, 8, 4, 0 }
                                        };

static AOM_FORCE_INLINE int od_ec_decode_cdf_q15_sse4_1(od_ec_dec *dec,
    const uint16_t *icdf, int nsyms)
{
    od_ec_window dif;
    unsigned r;
    unsigned c;
    unsigned u;
    unsigned v;
    int ret = 0;
    (void)nsyms;
    dif = dec->dif;
    r = dec->rng;

    assert(dif >> (OD_EC_WINDOW_SIZE - 16) < r);
    assert(icdf[nsyms - 1] == OD_ICDF(CDF_PROB_TOP));
    assert(32768U <= r);
    assert(7 - EC_PROB_SHIFT - CDF_SHIFT >= 0);
    c = (unsigned)(dif >> (OD_EC_WINDOW_SIZE - 16));

    int cmp[4], val[4];
    __m128i m0 = _mm_set1_epi32(r >> 8);
    __m128i m1 = _mm_loadl_epi64((__m128i *)icdf); // cdf[0] cdf[1] cdf[2] cdf[3]
    m1 = _mm_cvtepu16_epi32(m1);
    m1 = _mm_srai_epi32(m1, 6); // (icdf[++ret] >> EC_PROB_SHIFT)
    m1 = _mm_mullo_epi32(m1, m0); // partial v - first line done
    m1 = _mm_srai_epi32(m1, 1); // ((r >> 8) * (uint32_t)(icdf[++ret] >> EC_PROB_SHIFT) >> 1

    m0 = _mm_load_si128((__m128i *)n_ret[nsyms]); // EC_MIN_PROB * (N - ret)
    m1 = _mm_add_epi32(m1, m0); // v += EC_MIN_PROB * (N - ret)

    m0 = _mm_set1_epi32(c);
    __m128i m2 = _mm_cmplt_epi32(m0, m1);
    _mm_store_si128((__m128i *)cmp, m2);
    _mm_store_si128((__m128i *)val, m1);
    u = r;
    v = val[0];

    for (int i = 0; i < 4; i++) {
        if (cmp[i] == 0) {
            if (i != 0) {
                u = val[i - 1];
                v = val[i];
                ret = i;
            }
            break;
        }
    }

    assert(v < u);
    assert(u <= r);
    r = u - v;
    dif -= (od_ec_window)v << (OD_EC_WINDOW_SIZE - 16);
    return od_ec_dec_normalize(dec, dif, r, ret);
}

#endif  // CABAC_READER_SSE4_1_H_
