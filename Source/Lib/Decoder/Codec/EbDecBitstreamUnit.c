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

/*A range decoder.
  This is an entropy decoder based upon \cite{Mar79}, which is itself a
   rediscovery of the FIFO arithmetic code introduced by \cite{Pas76}.
  It is very similar to arithmetic encoding, except that encoding is done with
   digits in any base, instead of with bits, and so it is faster when using
   larger bases (i.e.: a byte).
  The author claims an average waste of $\frac{1}{2}\log_b(2b)$ bits, where $b$
   is the base, longer than the theoretical optimum, but to my knowledge there
   is no published justification for this claim.
  This only seems true when using near-infinite precision arithmetic so that
   the process is carried out with no rounding errors.

  An excellent description of implementation details is available at
   http://www.arturocampos.com/ac_range.html
  A recent work \cite{MNW98} which proposes several changes to arithmetic
   encoding for efficiency actually re-discovers many of the principles
   behind range encoding, and presents a good theoretical analysis of them.

  End of stream is handled by writing out the smallest number of bits that
   ensures that the stream will be correctly decoded regardless of the value of
   any subsequent bits.
  od_ec_dec_tell() can be used to determine how many bits were needed to decode
   all the symbols thus far; other data can be packed in the remaining bits of
   the input buffer.
  @PHDTHESIS{Pas76,
    author="Richard Clark Pasco",
    title="Source coding algorithms for fast data compression",
    school="Dept. of Electrical Engineering, Stanford University",
    address="Stanford, CA",
    month=May,
    year=1976,
    URL="http://www.richpasco.org/scaffdc.pdf"
  }
  @INPROCEEDINGS{Mar79,
   author="Martin, G.N.N.",
   title="Range encoding: an algorithm for removing redundancy from a digitised
    message",
   booktitle="Video & Data Recording Conference",
   year=1979,
   address="Southampton",
   month=Jul,
   URL="http://www.compressconsult.com/rangecoder/rngcod.pdf.gz"
  }
  @ARTICLE{MNW98,
   author="Alistair Moffat and Radford Neal and Ian H. Witten",
   title="Arithmetic Coding Revisited",
   journal="{ACM} Transactions on Information Systems",
   year=1998,
   volume=16,
   number=3,
   pages="256--294",
   month=Jul,
   URL="http://researchcommons.waikato.ac.nz/bitstream/handle/10289/78/content.pdf"
  }*/

// Commented it because it is included in EbDecBitstreamUnit.h file.
//#include "EbBitstreamUnit.h"
#include "EbDecBitstreamUnit.h"

/********************************************************************************************************************************/
/********************************************************************************************************************************/
/********************************************************************************************************************************/
/********************************************************************************************************************************/
// entdec.c from AOM

/*Initializes the decoder.
  buf: The input buffer to use.
  storage: The size in bytes of the input buffer.*/
static void od_ec_dec_init(od_ec_dec    *dec,
                    const unsigned char *buf,
                    uint32_t            storage)
{
    dec->buf = buf;
    dec->tell_offs = 10 - (OD_EC_WINDOW_SIZE - 8);
    dec->end = buf + storage;
    dec->bptr = buf;
    dec->dif = ((od_ec_window)1 << (OD_EC_WINDOW_SIZE - 1)) - 1;
    dec->rng = 0x8000;
    dec->cnt = -15;
    od_ec_dec_refill(dec);
}

/*Decode a single binary value.
  f: The probability that the bit is one, scaled by 32768.
  Return: The value decoded (0 or 1).*/
int od_ec_decode_bool_q15(od_ec_dec *dec, unsigned f) {
  od_ec_window dif;
  od_ec_window vw;
  unsigned r;
  unsigned r_new;
  unsigned v;
  int ret;
  assert(0 < f);
  assert(f < 32768U);
  dif = dec->dif;
  r = dec->rng;
  assert(dif >> (OD_EC_WINDOW_SIZE - 16) < r);
  assert(32768U <= r);
  v = ((r >> 8) * (uint32_t)(f >> EC_PROB_SHIFT) >> (7 - EC_PROB_SHIFT));
  v += EC_MIN_PROB;
  vw = (od_ec_window)v << (OD_EC_WINDOW_SIZE - 16);
  ret = 1;
  r_new = v;
  if (dif >= vw) {
    r_new = r - v;
    dif -= vw;
    ret = 0;
  }
  return od_ec_dec_normalize(dec, dif, r_new, ret);
}

/*Decodes a symbol given an inverse cumulative distribution function (CDF)
   table in Q15.
  icdf: CDF_PROB_TOP minus the CDF, such that symbol s falls in the range
         [s > 0 ? (CDF_PROB_TOP - icdf[s - 1]) : 0, CDF_PROB_TOP - icdf[s]).
        The values must be monotonically non-increasing, and icdf[nsyms - 1]
         must be 0.
  nsyms: The number of symbols in the alphabet.
         This should be at most 16.
  Return: The decoded symbol s.*/
int od_ec_decode_cdf_q15(od_ec_dec *dec, const uint16_t *icdf, int nsyms) {
  od_ec_window dif;
  unsigned r;
  unsigned c;
  unsigned u;
  unsigned v;
  int ret;
  (void)nsyms;
  dif = dec->dif;
  r = dec->rng;
  const int N = nsyms - 1;

  assert(dif >> (OD_EC_WINDOW_SIZE - 16) < r);
  assert(icdf[nsyms - 1] == OD_ICDF(CDF_PROB_TOP));
  assert(32768U <= r);
  assert(7 - EC_PROB_SHIFT - CDF_SHIFT >= 0);
  c = (unsigned)(dif >> (OD_EC_WINDOW_SIZE - 16));
  v = r;
  ret = -1;
  do {
    u = v;
    v = ((r >> 8) * (uint32_t)(icdf[++ret] >> EC_PROB_SHIFT) >>
         (7 - EC_PROB_SHIFT - CDF_SHIFT));
    v += EC_MIN_PROB * (N - ret);
  } while (c < v);
  assert(v < u);
  assert(u <= r);
  r = u - v;
  dif -= (od_ec_window)v << (OD_EC_WINDOW_SIZE - 16);
  return od_ec_dec_normalize(dec, dif, r, ret);
}

/********************************************************************************************************************************/
/********************************************************************************************************************************/
/********************************************************************************************************************************/
/********************************************************************************************************************************/
// daalaboolreader.c from AOM

int aom_daala_reader_init(DaalaReader_t *r, const uint8_t *buffer, int size) {
  if (size && !buffer)
    return 1;
  r->buffer_end = buffer + size;
  r->buffer = buffer;
  od_ec_dec_init(&r->ec, buffer, size);
#if CONFIG_ACCOUNTING
  r->accounting = NULL;
#endif
  return 0;
}

const uint8_t *aom_daala_reader_find_begin(DaalaReader_t *r) {
  return r->buffer;
}

const uint8_t *aom_daala_reader_find_end(DaalaReader_t *r) {
  return r->buffer_end;
}
