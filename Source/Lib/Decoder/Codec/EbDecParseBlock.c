/*
* Copyright(c) 2019 Netflix, Inc.
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

#include "EbDefinitions.h"
#include "EbPictureBufferDesc.h"

#include "EbSvtAv1Dec.h"
#include "EbDecHandle.h"
#include "EbDecBitReader.h"
#include "EbObuParse.h"

#include "EbDecParseHelper.h"
#include "EbTransforms.h"

#include "EbDecNbr.h"
#include "EbDecPicMgr.h"
#include "EbDecUtils.h"

void dec_setup_ref_mv_list(
    EbDecHandle *dec_handle, PartitionInfo_t *pi, MvReferenceFrame ref_frame,
    CandidateMv_dec ref_mv_stack[][MAX_REF_MV_STACK_SIZE],
    IntMv_dec mv_ref_list[][MAX_MV_REF_CANDIDATES], IntMv_dec *gm_mv_candidates,
    int mi_row, int mi_col, int16_t *mode_context, MvCount *mv_cnt);

#if ENABLE_ENTROPY_TRACE
FILE* temp_fp;
int enable_dump;
#endif

#define MV_BORDER (16 << 3)  // Allow 16 pels in 1/8th pel units
#define MAX_DIFFWTD_MASK_BITS 1
#define READ_REF_BIT(pname) \
  svt_read_symbol(r, get_pred_cdf_##pname(pi), 2, ACCT_STR)
#define SQR_BLOCK_SIZES 6
#define INTRABC_DELAY_PIXELS 256

#define NELEMENTS(x) (int)(sizeof(x) / sizeof(x[0]))

typedef AomCdfProb(*base_cdf_arr)[CDF_SIZE(4)];
typedef AomCdfProb(*br_cdf_arr)[CDF_SIZE(BR_CDF_SIZE)];

typedef struct txb_ctx {
    int txb_skip_ctx;
    int dc_sign_ctx;
} TXB_CTX;

static const int16_t k_eob_group_start[12] = { 0,  1,  2,  3,   5,   9,
                                        17, 33, 65, 129, 257, 513 };
static const int16_t k_eob_offset_bits[12] = { 0, 0, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9 };

extern const int8_t av1_nz_map_ctx_offset_4x4[16];/* = {
  0, 1, 6, 6, 1, 6, 6, 21, 6, 6, 21, 21, 6, 21, 21, 21,
};*/

extern const int8_t av1_nz_map_ctx_offset_8x8[64];/* = {
  0,  1,  6,  6,  21, 21, 21, 21, 1,  6,  6,  21, 21, 21, 21, 21,
  6,  6,  21, 21, 21, 21, 21, 21, 6,  21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
};*/

extern const int8_t av1_nz_map_ctx_offset_16x16[256];/* = {
  0,  1,  6,  6,  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 1,  6,  6,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 6,  6,  21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 6,  21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21,
};*/

extern const int8_t av1_nz_map_ctx_offset_32x32[1024];/* = {
  0,  1,  6,  6,  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 1,  6,  6,  21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 6,  6,  21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 6,  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
};*/

extern const int8_t av1_nz_map_ctx_offset_8x4[32];/* = {
  0,  16, 6,  6,  21, 21, 21, 21, 16, 16, 6,  21, 21, 21, 21, 21,
  16, 16, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21,
};*/

extern const int8_t av1_nz_map_ctx_offset_8x16[128];/* = {
  0,  11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 6,  6,  21,
  21, 21, 21, 21, 21, 6,  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
};*/

extern const int8_t av1_nz_map_ctx_offset_16x8[128];/* = {
  0,  16, 6,  6,  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 6,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
};*/

extern const int8_t av1_nz_map_ctx_offset_16x32[512];/* = {
  0,  11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
  11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 6,  6,  21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 6,  21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
};*/

extern const int8_t av1_nz_map_ctx_offset_32x16[512];/* = {
  0,  16, 6,  6,  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 6,  21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
};*/

extern const int8_t av1_nz_map_ctx_offset_32x64[1024];/* = {
  0,  11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
  11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
  11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11,
  11, 11, 11, 11, 11, 11, 11, 6,  6,  21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 6,  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
};*/

extern const int8_t av1_nz_map_ctx_offset_64x32[1024];/* = {
  0,  16, 6,  6,  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 6,  21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16,
  16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
};*/

extern const int8_t av1_nz_map_ctx_offset_4x16[64];/* = {
  0,  11, 11, 11, 11, 11, 11, 11, 6,  6,  21, 21, 6,  21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
};*/

extern const int8_t av1_nz_map_ctx_offset_16x4[64];/* = {
  0,  16, 6,  6,  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  16, 16, 6,  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
};*/

extern const int8_t av1_nz_map_ctx_offset_8x32[256];/* = {
  0,  11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 11, 6,  6,  21,
  21, 21, 21, 21, 21, 6,  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21,
};*/

extern const int8_t av1_nz_map_ctx_offset_32x8[256];/* = {
  0,  16, 6,  6,  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 6,  21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 16, 16, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 16, 16, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21, 21,
  21, 21, 21, 21, 21, 21, 21, 21, 21,
};*/

extern  int8_t av1_ref_frame_type(const MvReferenceFrame *const rf);
extern void av1_set_ref_frame(MvReferenceFrame *rf, int8_t ref_frame_type);


static const int8_t *av1_nz_map_ctx_offset[19] = {
  av1_nz_map_ctx_offset_4x4,    // TX_4x4
  av1_nz_map_ctx_offset_8x8,    // TX_8x8
  av1_nz_map_ctx_offset_16x16,  // TX_16x16
  av1_nz_map_ctx_offset_32x32,  // TX_32x32
  av1_nz_map_ctx_offset_32x32,  // TX_32x32
  av1_nz_map_ctx_offset_4x16,   // TX_4x8
  av1_nz_map_ctx_offset_8x4,    // TX_8x4
  av1_nz_map_ctx_offset_8x32,   // TX_8x16
  av1_nz_map_ctx_offset_16x8,   // TX_16x8
  av1_nz_map_ctx_offset_16x32,  // TX_16x32
  av1_nz_map_ctx_offset_32x16,  // TX_32x16
  av1_nz_map_ctx_offset_32x64,  // TX_32x64
  av1_nz_map_ctx_offset_64x32,  // TX_64x32
  av1_nz_map_ctx_offset_4x16,   // TX_4x16
  av1_nz_map_ctx_offset_16x4,   // TX_16x4
  av1_nz_map_ctx_offset_8x32,   // TX_8x32
  av1_nz_map_ctx_offset_32x8,   // TX_32x8
  av1_nz_map_ctx_offset_16x32,  // TX_16x64
  av1_nz_map_ctx_offset_64x32,  // TX_64x16
};

#define NZ_MAP_CTX_0 SIG_COEF_CONTEXTS_2D
#define NZ_MAP_CTX_5 (NZ_MAP_CTX_0 + 5)
#define NZ_MAP_CTX_10 (NZ_MAP_CTX_0 + 10)

#define MVREF_ROW_COLS 3

const int nz_map_ctx_offset_1d[32] = {
  NZ_MAP_CTX_0,  NZ_MAP_CTX_5,  NZ_MAP_CTX_10, NZ_MAP_CTX_10, NZ_MAP_CTX_10,
  NZ_MAP_CTX_10, NZ_MAP_CTX_10, NZ_MAP_CTX_10, NZ_MAP_CTX_10, NZ_MAP_CTX_10,
  NZ_MAP_CTX_10, NZ_MAP_CTX_10, NZ_MAP_CTX_10, NZ_MAP_CTX_10, NZ_MAP_CTX_10,
  NZ_MAP_CTX_10, NZ_MAP_CTX_10, NZ_MAP_CTX_10, NZ_MAP_CTX_10, NZ_MAP_CTX_10,
  NZ_MAP_CTX_10, NZ_MAP_CTX_10, NZ_MAP_CTX_10, NZ_MAP_CTX_10, NZ_MAP_CTX_10,
  NZ_MAP_CTX_10, NZ_MAP_CTX_10, NZ_MAP_CTX_10, NZ_MAP_CTX_10, NZ_MAP_CTX_10,
  NZ_MAP_CTX_10, NZ_MAP_CTX_10,
};

static int div_mult[32] = { 0,    16384, 8192, 5461, 4096, 3276, 2730, 2340,
                            2048, 1820,  1638, 1489, 1365, 1260, 1170, 1092,
                            1024, 963,   910,  862,  819,  780,  744,  712,
                            682,  655,   630,  606,  585,  564,  546,  528 };

static const MV_dec kZeroMv = { 0, 0 };

static INLINE int get_nz_mag(const uint8_t *const levels,
    const int bwl, const TxClass tx_class)
{
    int mag;

#define CLIP_MAX3(x) x > 3 ? 3 : x

    // Note: AOMMIN(level, 3) is useless for decoder since level < 3.
    mag = CLIP_MAX3(levels[1]);                         // { 0, 1 }
    mag += CLIP_MAX3(levels[(1 << bwl) + TX_PAD_HOR]);  // { 1, 0 }

    if (tx_class == TX_CLASS_2D) {
        mag += CLIP_MAX3(levels[(1 << bwl) + TX_PAD_HOR + 1]);          // { 1, 1 }
        mag += CLIP_MAX3(levels[2]);                                    // { 0, 2 }
        mag += CLIP_MAX3(levels[(2 << bwl) + (2 << TX_PAD_HOR_LOG2)]);  // { 2, 0 }
    }
    else if (tx_class == TX_CLASS_VERT) {
        mag += CLIP_MAX3(levels[(2 << bwl) + (2 << TX_PAD_HOR_LOG2)]);  // { 2, 0 }
        mag += CLIP_MAX3(levels[(3 << bwl) + (3 << TX_PAD_HOR_LOG2)]);  // { 3, 0 }
        mag += CLIP_MAX3(levels[(4 << bwl) + (4 << TX_PAD_HOR_LOG2)]);  // { 4, 0 }
    }
    else {
        mag += CLIP_MAX3(levels[2]);  // { 0, 2 }
        mag += CLIP_MAX3(levels[3]);  // { 0, 3 }
        mag += CLIP_MAX3(levels[4]);  // { 0, 4 }
    }

    return mag;
}

static INLINE int get_nz_map_ctx_from_stats(
    const int stats, const int coeff_idx,  // raster order
    const int bwl, const TxSize tx_size, const TxClass tx_class)
{
    if ((tx_class | coeff_idx) == 0) return 0;
    int ctx = (stats + 1) >> 1;
    ctx = AOMMIN(ctx, 4);
    switch (tx_class)
    {
    case TX_CLASS_2D:
        return ctx + av1_nz_map_ctx_offset[tx_size][coeff_idx];
    case TX_CLASS_HORIZ: {
        const int row = coeff_idx >> bwl;
        const int col = coeff_idx - (row << bwl);
        return ctx + nz_map_ctx_offset_1d[col];
    }
    case TX_CLASS_VERT: {
        const int row = coeff_idx >> bwl;
        return ctx + nz_map_ctx_offset_1d[row];
    }
    default: break;
    }
    return 0;
}

static INLINE int has_nearmv(PredictionMode mode) {
    return (mode == NEARMV || mode == NEAR_NEARMV ||
            mode == NEAR_NEWMV || mode == NEW_NEARMV);
}

void palette_mode_info(/*EbDecHandle *dec_handle,
    int mi_row, int mi_col, SvtReader *r*/)
{
    //TO-DO
    assert(0);
}

void filter_intra_mode_info(EbDecHandle *dec_handle,
    PartitionInfo_t *xd, SvtReader *r)
{
    ModeInfo_t *const mbmi = xd->mi;
    FilterIntraModeInfo_t *filter_intra_mode_info =
        &mbmi->filter_intra_mode_info;
    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    FRAME_CONTEXT *frm_ctx = &parse_ctxt->cur_tile_ctx;

    if (filter_intra_allowed(dec_handle, mbmi)) {
        filter_intra_mode_info->use_filter_intra = svt_read_symbol(
            r, frm_ctx->filter_intra_cdfs[mbmi->sb_type], 2, ACCT_STR);
        if (filter_intra_mode_info->use_filter_intra) {
            filter_intra_mode_info->filter_intra_mode = svt_read_symbol(
                r, frm_ctx->filter_intra_mode_cdf, FILTER_INTRA_MODES, ACCT_STR);
        }
    }
    else
        filter_intra_mode_info->use_filter_intra = 0;
}

uint8_t read_cfl_alphas(FRAME_CONTEXT *ec_ctx, SvtReader *r,
    uint8_t *signs_out)
{
    const int joint_sign =
        svt_read_symbol(r, ec_ctx->cfl_sign_cdf, CFL_JOINT_SIGNS, "cfl:signs");
    uint8_t idx = 0;
    // Magnitudes are only coded for nonzero values
    if (CFL_SIGN_U(joint_sign) != CFL_SIGN_ZERO) {
        AomCdfProb *cdf_u = ec_ctx->cfl_alpha_cdf[CFL_CONTEXT_U(joint_sign)];
        idx = svt_read_symbol(r, cdf_u, CFL_ALPHABET_SIZE, "cfl:alpha_u")
            << CFL_ALPHABET_SIZE_LOG2;
    }
    if (CFL_SIGN_V(joint_sign) != CFL_SIGN_ZERO) {
        AomCdfProb *cdf_v = ec_ctx->cfl_alpha_cdf[CFL_CONTEXT_V(joint_sign)];
        idx += svt_read_symbol(r, cdf_v, CFL_ALPHABET_SIZE, "cfl:alpha_v");
    }
    *signs_out = joint_sign;
    return idx;
}

void read_cdef(EbDecHandle *dec_handle, SvtReader *r, PartitionInfo_t *xd,
    int mi_col, int mi_row, int8_t *cdef_strength)
{
    ModeInfo_t *const mbmi = xd->mi;
    if (mbmi->skip_mode || dec_handle->frame_header.coded_lossless ||
        !dec_handle->seq_header.enable_cdef || dec_handle->frame_header.allow_intrabc)
    {
        return;
    }

    int cdf_size = block_size_wide[BLOCK_64X64] >> 2;
    const int mask = ~(cdf_size - 1);
    const int index = dec_handle->seq_header.sb_size == BLOCK_128X128
        ? !!(mi_col & mask) + 2 * !!(mi_row & mask) : 0;
    if (cdef_strength[index] == -1)
        svt_read_literal(r, dec_handle->frame_header.CDEF_params.cdef_bits, ACCT_STR);
}

int read_delta_qindex(EbDecHandle *dec_handle, SvtReader *r,
    ModeInfo_t *const mbmi, int mi_col, int mi_row)
{
    int sign, abs, reduced_delta_qindex = 0;
    BlockSize bsize = mbmi->sb_type;
    const int b_col = mi_col & (dec_handle->seq_header.sb_mi_size - 1);
    const int b_row = mi_row & (dec_handle->seq_header.sb_mi_size - 1);
    const int read_delta_q_flag = (b_col == 0 && b_row == 0);

    if ((bsize != dec_handle->seq_header.sb_size || mbmi->skip == 0) &&
        read_delta_q_flag)
    {
        ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
        abs = svt_read_symbol(r, parse_ctxt->cur_tile_ctx.delta_q_cdf,
            DELTA_Q_PROBS + 1, ACCT_STR);

        if (abs == DELTA_Q_SMALL) {
            const int rem_bits = svt_read_literal(r, 3, ACCT_STR) + 1;
            const int thr = (1 << rem_bits) + 1;
            abs = svt_read_literal(r, rem_bits, ACCT_STR) + thr;
        }

        if (abs)
            sign = svt_read_bit(r, ACCT_STR);
        else
            sign = 1;
        reduced_delta_qindex = sign ? -abs : abs;
    }
    return reduced_delta_qindex;
}

int read_delta_lflevel(EbDecHandle *dec_handle, SvtReader *r,
    AomCdfProb *cdf, ModeInfo_t *mbmi, int mi_col, int mi_row)
{
    int reduced_delta_lflevel = 0;
    const BlockSize bsize = mbmi->sb_type;
    const int b_col = mi_col & (dec_handle->seq_header.sb_mi_size - 1);
    const int b_row = mi_row & (dec_handle->seq_header.sb_mi_size - 1);
    const int read_delta_lf_flag = (b_col == 0 && b_row == 0);

    if ((bsize != dec_handle->seq_header.sb_size || mbmi->skip == 0) &&
        read_delta_lf_flag)
    {
        int abs = svt_read_symbol(r, cdf, DELTA_LF_PROBS + 1, ACCT_STR);
        if (abs == DELTA_LF_SMALL) {
            const int rem_bits = svt_read_literal(r, 3, ACCT_STR) + 1;
            const int thr = (1 << rem_bits) + 1;
            abs = svt_read_literal(r, rem_bits, ACCT_STR) + thr;
        }
        const int sign = abs ? svt_read_bit(r, ACCT_STR) : 1;
        reduced_delta_lflevel = sign ? -abs : abs;
    }
    return reduced_delta_lflevel;
}

int seg_feature_active(SegmentationParams *seg, int segment_id,
    SEG_LVL_FEATURES feature_id)
{
    return seg->segmentation_enabled && seg->feature_data[segment_id][feature_id];
}

int read_skip(EbDecHandle *dec_handle, PartitionInfo_t *xd,
    int segment_id, SvtReader *r)
{
    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    uint8_t segIdPreSkip = dec_handle->frame_header.segmentation_params.seg_id_pre_skip;
    if (segIdPreSkip && seg_feature_active(&dec_handle->frame_header.segmentation_params,
        segment_id, SEG_LVL_SKIP))
    {
        return 1;
    }
    else {
        const int above_skip = xd->above_mbmi ? xd->above_mbmi->skip : 0;
        const int left_skip = xd->left_mbmi ? xd->left_mbmi->skip : 0;
        int ctx = above_skip + left_skip;
        return svt_read_symbol(r, parse_ctxt->cur_tile_ctx.skip_cdfs[ctx], 2, ACCT_STR);
    }
}

int read_skip_mode(EbDecHandle *dec_handle, PartitionInfo_t *xd, int segment_id,
    SvtReader *r)
{
    SegmentationParams *seg = &dec_handle->frame_header.segmentation_params;
    if (seg_feature_active(seg, segment_id, SEG_LVL_SKIP) ||
        seg_feature_active(seg, segment_id, SEG_LVL_REF_FRAME) ||
        seg_feature_active(seg, segment_id, SEG_LVL_GLOBALMV) ||
        !dec_handle->frame_header.skip_mode_params.skip_mode_present ||
        block_size_wide[xd->mi->sb_type] < 8 ||
        block_size_high[xd->mi->sb_type] < 8)
    {
        return 0;
    }
    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    int above_skip_mode = xd->above_mbmi ? xd->above_mbmi->skip_mode : 0;
    int left_skip_mode = xd->left_mbmi ? xd->left_mbmi->skip_mode : 0;
    int ctx = above_skip_mode + left_skip_mode;
    return svt_read_symbol(r, parse_ctxt->cur_tile_ctx.skip_mode_cdfs[ctx], 2, ACCT_STR);
}

// If delta q is present, reads delta_q index.
// Also reads delta_q loop filter levels, if present.
static void read_delta_params(EbDecHandle *dec_handle, SvtReader *r,
    PartitionInfo_t *xd, const int mi_row, const int mi_col)
{
    ParseCtxt       *parse_ctxt     = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    DeltaQParams    *delta_q_params = &dec_handle->frame_header.delta_q_params;
    DeltaLFParams   *delta_lf_params = &dec_handle->frame_header.delta_lf_params;
    SBInfo          *sb_info = xd->sb_info;

    if (delta_q_params->delta_q_present) {
        ModeInfo_t *const mbmi = &xd->mi[0];
        int current_qindex;
        int base_qindex = dec_handle->frame_header.quantization_params.base_q_idx;

        assert(0);
        //delta_q_res should be left shifted instead of being multiplied - check logic
        int delta_q = read_delta_qindex(dec_handle, r, mbmi, mi_col, mi_row);
        sb_info->sb_delta_q[0] = delta_q * delta_q_params->delta_q_res;
        current_qindex = base_qindex + sb_info->sb_delta_q[0];
        /* Normative: Clamp to [1,MAXQ] to not interfere with lossless mode */
        /* TODO: Should add cur q idx and remove this repetitive? */
        sb_info->sb_delta_q[0] = clamp(current_qindex, 1, MAXQ) - base_qindex;

        FRAME_CONTEXT *const ec_ctx = &parse_ctxt->cur_tile_ctx;

        if (delta_lf_params->delta_lf_present) {
            LoopFilterParams lf_params = dec_handle->frame_header.loop_filter_params;
            if (delta_lf_params->delta_lf_multi) {
                EbColorConfig *color_info = &dec_handle->seq_header.color_config;
                int num_planes = color_info->mono_chrome ? 1 : MAX_MB_PLANE;
                const int frame_lf_count =
                    num_planes > 1 ? FRAME_LF_COUNT : FRAME_LF_COUNT - 2;
                for (int lf_id = 0; lf_id < frame_lf_count; ++lf_id) {
                    int tmp_lvl;
                    int base_lvl = lf_params.loop_filter_level[lf_id];
                    assert(0);
                    //delta_lf_res should be left shifted instead of being multiplied - check logic
                    sb_info->sb_delta_lf[lf_id] =
                        read_delta_lflevel(dec_handle, r, ec_ctx->delta_lf_multi_cdf[lf_id],
                                           mbmi, mi_col, mi_row) *
                                           delta_lf_params->delta_lf_res;
                    tmp_lvl = base_lvl + sb_info->sb_delta_lf[lf_id];
                    tmp_lvl = clamp(tmp_lvl, -MAX_LOOP_FILTER, MAX_LOOP_FILTER);
                    sb_info->sb_delta_lf[lf_id] = tmp_lvl - base_lvl;
                }
            } else {
                int tmp_lvl;
                int base_lvl = lf_params.loop_filter_level[0];
                sb_info->sb_delta_lf[0] =
                                read_delta_lflevel(dec_handle, r, ec_ctx->delta_lf_cdf,
                                                    mbmi, mi_col, mi_row) *
                                delta_lf_params->delta_lf_res;
                tmp_lvl = base_lvl + sb_info->sb_delta_lf[0];
                tmp_lvl = clamp(tmp_lvl, -MAX_LOOP_FILTER, MAX_LOOP_FILTER);
                sb_info->sb_delta_lf[0] = tmp_lvl - base_lvl;
            }
        }
    }
}

int is_directional_mode(PredictionMode mode) {
    return mode >= V_PRED && mode <= D67_PRED;
}

int intra_angle_info(SvtReader *r, AomCdfProb *cdf, PredictionMode mode, BlockSize bsize) {
    int angleDeltaY = 0;
    if (use_angle_delta(bsize) && is_directional_mode(mode)) {
        const int sym = svt_read_symbol(r, cdf, 2 * MAX_ANGLE_DELTA + 1, ACCT_STR);
        angleDeltaY = sym - MAX_ANGLE_DELTA;
    }
    return angleDeltaY;
}

int get_segment_id(FrameHeader *frm_info, uint8_t *segment_ids,
    BlockSize bsize, uint32_t mi_row, uint32_t mi_col)
{
    const int mi_offset = mi_row * frm_info->mi_cols + mi_col;
    const uint32_t bw = mi_size_wide[bsize];
    const uint32_t bh = mi_size_high[bsize];
    const int xmis = AOMMIN(frm_info->mi_cols - mi_col, bw);
    const int ymis = AOMMIN(frm_info->mi_rows - mi_row, bh);
    int x, y, segment_id = MAX_SEGMENTS-1;

    for (y = 0; y < ymis; ++y)
        for (x = 0; x < xmis; ++x)
            segment_id =
            AOMMIN(segment_id, segment_ids[mi_offset + y * frm_info->mi_cols + x]);

    assert(segment_id >= 0 && segment_id < MAX_SEGMENTS-1);
    return segment_id;
}

static int read_segment_id(EbDecHandle *dec_handle, PartitionInfo_t *xd, uint32_t mi_row,
    uint32_t mi_col, SvtReader *r, int skip)
{
    int cdf_num = 0;
    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;

    int prev_ul = -1;  // top left segment_id
    int prev_l = -1;   // left segment_id
    int prev_u = -1;   // top segment_id
    int pred = -1;

    uint8_t *seg_maps = parse_ctxt->parse_nbr4x4_ctxt.segment_maps;
    if ((xd->up_available) && (xd->left_available)) {
        prev_ul = get_segment_id(&dec_handle->frame_header, seg_maps, BLOCK_4X4, mi_row - 1,
            mi_col - 1);
    }
    if (xd->up_available) {
        prev_u = get_segment_id(&dec_handle->frame_header, seg_maps, BLOCK_4X4, mi_row - 1,
            mi_col);
    }
    if (xd->left_available) {
        prev_l = get_segment_id(&dec_handle->frame_header, seg_maps, BLOCK_4X4, mi_row,
            mi_col - 1);
    }
    if ((prev_ul == prev_u) && (prev_ul == prev_l))
        cdf_num = 2;
    else if ((prev_ul == prev_u) || (prev_ul == prev_l) || (prev_u == prev_l))
        cdf_num = 1;

    // If 2 or more are identical returns that as predictor, otherwise prev_l.
    if (prev_u == -1)  // edge case
        pred = prev_l == -1 ? 0 : prev_l;
    else if (prev_l == -1)  // edge case
        pred = prev_u;
    else
        pred = (prev_ul == prev_u) ? prev_u : prev_l;

    if (skip) return pred;

    FRAME_CONTEXT *ec_ctx = &parse_ctxt->cur_tile_ctx;
    SegmentationParams *seg = &(dec_handle->frame_header.segmentation_params);

    struct segmentation_probs *segp = &ec_ctx->seg;
    AomCdfProb *pred_cdf = segp->spatial_pred_seg_cdf[cdf_num];

    int coded_id = svt_read_symbol(r, pred_cdf, MAX_SEGMENTS, ACCT_STR);
    return neg_deinterleave(coded_id, pred, seg->last_active_seg_id + 1);
}

int intra_segment_id(EbDecHandle *dec_handle, PartitionInfo_t *xd, int mi_row, int mi_col,
    int bsize, SvtReader *r, int skip)
{
    SegmentationParams *seg = &dec_handle->frame_header.segmentation_params;
    int segment_id = 0;

    if (seg->segmentation_enabled) {
        const int mi_offset = mi_row * dec_handle->frame_header.mi_cols + mi_col;
        const int bw = mi_size_wide[bsize];
        const int bh = mi_size_high[bsize];
        const int x_mis = AOMMIN((int32_t)(dec_handle->frame_header.mi_cols - mi_col), bw);
        const int y_mis = AOMMIN((int32_t)(dec_handle->frame_header.mi_rows - mi_row), bh);
        segment_id = read_segment_id(dec_handle, xd, mi_row, mi_col, r, skip);
        set_segment_id(dec_handle, mi_offset, x_mis, y_mis, segment_id);
    }
    return segment_id;
}

PredictionMode getMode(PredictionMode yMode, int refList) {

    PredictionMode compMode = GLOBALMV;
    if (yMode == NEW_NEWMV) compMode = NEWMV;
    if (refList == 0) {
        if (yMode < NEAREST_NEARESTMV)
            compMode = yMode;
        else if (yMode == NEW_NEARESTMV || yMode == NEW_NEARMV)
            compMode = NEWMV;
        else if (yMode == NEAREST_NEARESTMV || yMode == NEAREST_NEWMV)
            compMode = NEARESTMV;
        else if (yMode == NEAR_NEARMV || yMode == NEAR_NEWMV)
            compMode = NEARMV;
    }
    else {
        if (yMode == NEAREST_NEWMV || yMode == NEAR_NEWMV)
            compMode = NEWMV;
        else if (yMode == NEAREST_NEARESTMV || yMode == NEW_NEARESTMV)
            compMode = NEARESTMV;
        else if (yMode == NEAR_NEARMV || yMode == NEW_NEARMV)
            compMode = NEARMV;
    }
    return compMode;
}

int read_mv_component(SvtReader *r, NmvComponent *mvcomp, int use_subpel, int usehp) {
    int mag, d, fr, hp;
    const int sign = svt_read_symbol(r, mvcomp->sign_cdf, 2, ACCT_STR);
    const int mv_class =
        svt_read_symbol(r, mvcomp->classes_cdf, MV_CLASSES, ACCT_STR);
    const int class0 = mv_class == MV_CLASS_0;

    // Integer part
    if (class0) {
        d = svt_read_symbol(r, mvcomp->class0_cdf, CLASS0_SIZE, ACCT_STR);
        mag = 0;
    }
    else {
        d = 0;
        for (int i = 0; i < mv_class; ++i)
            d |= svt_read_symbol(r, mvcomp->bits_cdf[i], 2, ACCT_STR) << i;
        mag = CLASS0_SIZE << (mv_class + 2);
    }

    fr = use_subpel ? svt_read_symbol(r, class0 ? mvcomp->class0_fp_cdf[d] :
        mvcomp->fp_cdf, MV_FP_SIZE, ACCT_STR) : 3;

    hp = usehp ? svt_read_symbol(r, class0 ? mvcomp->class0_hp_cdf :
        mvcomp->hp_cdf, 2, ACCT_STR) : 1;

    // Result
    mag += ((d << 3) | (fr << 1) | hp) + 1;
    return sign ? -mag : mag;
}

void read_mv(SvtReader *r, MV_dec *mv, MV_dec *ref, NmvContext *ctx,
    int intra_bc, MvSubpelPrecision precision) {
    MV_dec diff = kZeroMv;

    const MvJointType joint_type =
        (MvJointType)svt_read_symbol(r, &ctx->joints_cdf[intra_bc], MV_JOINTS, ACCT_STR);

    if (mv_joint_vertical(joint_type))
        diff.row = read_mv_component(r, &ctx->comps[0], precision > MV_SUBPEL_NONE,
            precision > MV_SUBPEL_LOW_PRECISION);

    if (mv_joint_horizontal(joint_type))
        diff.col = read_mv_component(r, &ctx->comps[1], precision > MV_SUBPEL_NONE,
            precision > MV_SUBPEL_LOW_PRECISION);

    mv->row = ref->row + diff.row;
    mv->col = ref->col + diff.col;
}

static INLINE int assign_mv(EbDecHandle *dec_handle, PartitionInfo_t *pi,
    IntMv_dec mv[2], IntMv_dec *global_mvs, IntMv_dec ref_mv[2],
    IntMv_dec nearest_mv[2], IntMv_dec near_mv[2], int mi_row,
    int mi_col, int is_compound, int allow_hp, SvtReader *r)
{
    ModeInfo_t *mbmi = pi->mi;
    BlockSize bsize = mbmi->sb_type;
    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    TileInfo *tile = &parse_ctxt->cur_tile_info;

    if (dec_handle->frame_header.force_integer_mv)
        allow_hp = MV_SUBPEL_NONE;

    switch (mbmi->mode) {
    case NEWMV: {
        NmvContext *const nmvc = &parse_ctxt->cur_tile_ctx.nmvc;
        read_mv(r, &mv[0].as_mv, &ref_mv[0].as_mv, nmvc, mbmi->use_intrabc, allow_hp);
        break;
    }
    case NEARESTMV: {
      mv[0].as_int = nearest_mv[0].as_int;
      break;
    }
    case NEARMV: {
      mv[0].as_int = near_mv[0].as_int;
      break;
    }
    case GLOBALMV: {
      mv[0].as_int = global_mvs[0].as_int;
      break;
    }
    case NEW_NEWMV: {
      assert(is_compound);
      for (int i = 0; i < 2; ++i) {
          NmvContext *const nmvc = &parse_ctxt->cur_tile_ctx.nmvc;
          read_mv(r, &mv[i].as_mv, &ref_mv[i].as_mv, nmvc, mbmi->use_intrabc, allow_hp);
      }
      break;
    }
    case NEAREST_NEARESTMV: {
      assert(is_compound);
      mv[0].as_int = nearest_mv[0].as_int;
      mv[1].as_int = nearest_mv[1].as_int;
      break;
    }
    case NEAR_NEARMV: {
      assert(is_compound);
      mv[0].as_int = near_mv[0].as_int;
      mv[1].as_int = near_mv[1].as_int;
      break;
    }
    case NEW_NEARESTMV: {
    NmvContext *const nmvc = &parse_ctxt->cur_tile_ctx.nmvc;
    read_mv(r, &mv[0].as_mv, &ref_mv[0].as_mv, nmvc, mbmi->use_intrabc, allow_hp);
      assert(is_compound);
      mv[1].as_int = nearest_mv[1].as_int;
      break;
    }
    case NEAREST_NEWMV: {
      mv[0].as_int = nearest_mv[0].as_int;
      NmvContext *const nmvc = &parse_ctxt->cur_tile_ctx.nmvc;
      read_mv(r, &mv[1].as_mv, &ref_mv[1].as_mv, nmvc, mbmi->use_intrabc, allow_hp);
      assert(is_compound);
      break;
    }
    case NEAR_NEWMV: {
      mv[0].as_int = near_mv[0].as_int;
      NmvContext *const nmvc = &parse_ctxt->cur_tile_ctx.nmvc;
      read_mv(r, &mv[1].as_mv, &ref_mv[1].as_mv, nmvc, mbmi->use_intrabc, allow_hp);
      assert(is_compound);
      break;
    }
    case NEW_NEARMV: {
    NmvContext *const nmvc = &parse_ctxt->cur_tile_ctx.nmvc;
    read_mv(r, &mv[0].as_mv, &ref_mv[0].as_mv, nmvc, mbmi->use_intrabc, allow_hp);
      assert(is_compound);
      mv[1].as_int = near_mv[1].as_int;
      break;
    }
    case GLOBAL_GLOBALMV: {
      assert(is_compound);
      mv[0].as_int = global_mvs[0].as_int;
      mv[1].as_int = global_mvs[1].as_int;
      break;
    }
    default: { return 0; }
    }

    int ret = is_mv_valid(&mv[0].as_mv);
    if (is_compound)
        ret = ret && is_mv_valid(&mv[1].as_mv);
    return ret;
}
void av1_find_mv_refs(EbDecHandle *dec_handle, PartitionInfo_t *pi,
    MvReferenceFrame ref_frame, CandidateMv_dec ref_mv_stack[][MAX_REF_MV_STACK_SIZE],
    IntMv_dec mv_ref_list[][MAX_MV_REF_CANDIDATES], IntMv_dec global_mvs[2],
    int mi_row, int mi_col, int16_t *mode_context, MvCount *mv_cnt)
{
    BlockSize bsize = pi->mi->sb_type;
    MvReferenceFrame rf[2];
    av1_set_ref_frame(rf, ref_frame);

    /* setup global mv process */
    global_mvs[0].as_int = 0;
    global_mvs[1].as_int = 0;
    if (ref_frame != INTRA_FRAME) {
        EbDecPicBuf *buf = dec_handle->cur_pic_buf[0];
        global_mvs[0].as_int = gm_get_motion_vector(&buf->global_motion[rf[0]],
            dec_handle->frame_header.allow_high_precision_mv, bsize,
            mi_col, mi_row, dec_handle->frame_header.force_integer_mv).as_int;

        global_mvs[1].as_int = (rf[1] != NONE_FRAME) ?
            gm_get_motion_vector(&buf->global_motion[rf[1]],
            dec_handle->frame_header.allow_high_precision_mv, bsize,
            mi_col, mi_row, dec_handle->frame_header.force_integer_mv).as_int : 0;
    }
    dec_setup_ref_mv_list(dec_handle, pi, ref_frame, ref_mv_stack, mv_ref_list,
        global_mvs, mi_row, mi_col, mode_context, mv_cnt);
}

void intra_frame_mode_info(EbDecHandle *dec_handle, PartitionInfo_t *xd, int mi_row,
    int mi_col, SvtReader *r, int8_t *cdef_strength)
{
    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    ModeInfo_t *const mbmi = xd->mi;
    const ModeInfo_t *above_mi = xd->above_mbmi;
    const ModeInfo_t *left_mi = xd->left_mbmi;
    const BlockSize bsize = mbmi->sb_type;
    SegmentationParams *const seg = &dec_handle->frame_header.segmentation_params;
    EbColorConfig color_config = dec_handle->seq_header.color_config;
    uint8_t     *lossless_array = &dec_handle->frame_header.lossless_array[0];
    const int allow_hp = dec_handle->frame_header.allow_high_precision_mv;
    IntMv_dec mv[2];
    IntMv_dec ref_mvs[MODE_CTX_REF_FRAMES][MAX_MV_REF_CANDIDATES] = { { { 0 } } };
    int16_t inter_mode_ctx[MODE_CTX_REF_FRAMES];
    MvReferenceFrame ref_frame = av1_ref_frame_type(mbmi->ref_frame);
    MvCount *mv_cnt = (MvCount*)malloc(sizeof(MvCount));

    if (seg->seg_id_pre_skip) {
        mbmi->segment_id =
            intra_segment_id(dec_handle, xd, mi_row, mi_col, bsize, r, 0);
    }

    mbmi->skip = read_skip(dec_handle, xd, mbmi->segment_id, r);

    if (!seg->seg_id_pre_skip) {
        mbmi->segment_id =
            intra_segment_id(dec_handle, xd, mi_row, mi_col, bsize, r, mbmi->skip);
    }

    read_cdef(dec_handle, r, xd, mi_col, mi_row, cdef_strength);

    read_delta_params(dec_handle, r, xd, mi_row, mi_col);

    mbmi->ref_frame[0] = INTRA_FRAME;
    mbmi->ref_frame[1] = NONE_FRAME;

    mbmi->use_intrabc = 0;
    if (allow_intrabc(dec_handle))
        mbmi->use_intrabc = svt_read_symbol(r, parse_ctxt->cur_tile_ctx.intrabc_cdf,
            2, ACCT_STR);

    if (mbmi->use_intrabc) {
        mbmi->mode = DC_PRED;
        mbmi->uv_mode = UV_DC_PRED;
        mbmi->motion_mode = SIMPLE_TRANSLATION;
        mbmi->compound_mode = COMPOUND_AVERAGE;
        dec_handle->frame_header.interpolation_filter = BILINEAR;
        IntMv_dec global_mvs[2];
        av1_find_mv_refs(dec_handle, xd, ref_frame, xd->ref_mv_stack,
            ref_mvs, global_mvs, mi_row, mi_col,
            inter_mode_ctx, mv_cnt);
        assert(0);
    }
    else {
        AomCdfProb *y_mode_cdf = get_y_mode_cdf(&parse_ctxt->cur_tile_ctx,
            above_mi, left_mi);
        mbmi->mode = read_intra_mode(r, y_mode_cdf);
        mbmi->angle_delta[PLANE_TYPE_Y] = intra_angle_info(r,
            &parse_ctxt->cur_tile_ctx.angle_delta_cdf[mbmi->mode - V_PRED][0],
            mbmi->mode, bsize);
        const int has_chroma =
                dec_is_chroma_reference(mi_row, mi_col, bsize,
                    color_config.subsampling_x, color_config.subsampling_y);
        if (has_chroma) {
            mbmi->uv_mode = read_intra_mode_uv(&parse_ctxt->cur_tile_ctx,
                r, is_cfl_allowed(xd, &color_config, lossless_array), mbmi->mode);
            if (mbmi->uv_mode == UV_CFL_PRED) {
                mbmi->cfl_alpha_idx = read_cfl_alphas(&parse_ctxt->cur_tile_ctx,
                    r, &mbmi->cfl_alpha_signs);
            }
            mbmi->angle_delta[PLANE_TYPE_UV] = intra_angle_info(r,
                &parse_ctxt->cur_tile_ctx.angle_delta_cdf[mbmi->uv_mode - V_PRED][0],
                dec_get_uv_mode(mbmi->uv_mode), bsize);
        }

        if (allow_palette(dec_handle->frame_header.allow_screen_content_tools, bsize))
            palette_mode_info(/*dec_handle, mi_row, mi_col, r*/);
        filter_intra_mode_info(dec_handle, xd, r);
    }
    free(mv_cnt);
}

static INLINE int get_pred_context_seg_id(const PartitionInfo_t *xd) {
    const ModeInfo_t *const above_mi = xd->above_mbmi;
    const ModeInfo_t *const left_mi = xd->left_mbmi;
    const int above_sip = (above_mi != NULL) ? above_mi->seg_id_predicted : 0;
    const int left_sip = (left_mi != NULL) ? left_mi->seg_id_predicted : 0;

    return above_sip + left_sip;
}

void update_seg_ctx(ParseNbr4x4Ctxt *ngr_ctx, int blk_col,
    int w4, int h4, int seg_id_predicted)
{
    uint8_t *const above_seg_ctx = ngr_ctx->above_seg_pred_ctx + blk_col;
    uint8_t *const left_seg_ctx = ngr_ctx->left_seg_pred_ctx;

    memset(above_seg_ctx, seg_id_predicted, w4);
    memset(left_seg_ctx, seg_id_predicted, h4);
}

int read_inter_segment_id(EbDecHandle *dec_handle, PartitionInfo_t *xd,
                        uint32_t mi_row, uint32_t mi_col, int preskip, SvtReader *r)
{
    SegmentationParams *seg = &dec_handle->frame_header.segmentation_params;
    ModeInfo_t *const mbmi = xd->mi;
    FrameHeader *frame_header = &dec_handle->frame_header;
    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    const int mi_offset = mi_row * frame_header->mi_cols + mi_col;
    const uint32_t bw = mi_size_wide[mbmi->sb_type];
    const uint32_t bh = mi_size_high[mbmi->sb_type];

    const int x_mis = AOMMIN(frame_header->mi_cols - mi_col, bw);
    const int y_mis = AOMMIN(frame_header->mi_rows - mi_row, bh);

    if (!seg->segmentation_enabled) return 0;  // Default for disabled segmentation

    int predictedSegmentId = get_segment_id(frame_header,
        parse_ctxt->parse_nbr4x4_ctxt.segment_maps, mbmi->sb_type, mi_row, mi_col);

    if (!seg->segmentation_update_map)
        return predictedSegmentId;

    int segment_id;
    if (preskip) {
        if (!seg->seg_id_pre_skip)
          return 0;
    }
    else {
        if (mbmi->skip) {
            mbmi->seg_id_predicted = 0;
            update_seg_ctx(&parse_ctxt->parse_nbr4x4_ctxt,
                mi_col, bw, bh, mbmi->seg_id_predicted);
            segment_id = read_segment_id(dec_handle, xd, mi_row, mi_col, r, 1);
            set_segment_id(dec_handle, mi_offset, x_mis, y_mis, segment_id);
            return segment_id;
        }
    }

    if (seg->segmentation_temporal_update) {
        const int ctx = get_pred_context_seg_id(xd);
        ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
        struct segmentation_probs *const segp = &parse_ctxt->cur_tile_ctx.seg;
        mbmi->seg_id_predicted = svt_read_symbol(r, segp->pred_cdf[ctx], 2, ACCT_STR);
        if (mbmi->seg_id_predicted)
            segment_id = predictedSegmentId;
        else
            segment_id = read_segment_id(dec_handle, xd, mi_row, mi_col, r, 0);
        update_seg_ctx(&parse_ctxt->parse_nbr4x4_ctxt,
            mi_col, bw, bh, mbmi->seg_id_predicted);
    }
    else
        segment_id = read_segment_id(dec_handle, xd, mi_row, mi_col, r, 0);
    set_segment_id(dec_handle, mi_offset, x_mis, y_mis, segment_id);

    return segment_id;
}

static PredictionMode read_inter_compound_mode(EbDecHandle *dec_handle,
    SvtReader *r, int16_t ctx)
{
    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    const int mode =
        svt_read_symbol(r, parse_ctxt->cur_tile_ctx.inter_compound_mode_cdf[ctx],
            INTER_COMPOUND_MODES, ACCT_STR);
    assert(is_inter_compound_mode(NEAREST_NEARESTMV + mode));
    return NEAREST_NEARESTMV + mode;
}

static INLINE uint8_t get_drl_ctx(const CandidateMv_dec *ref_mv_stack, int ref_idx) {

    if (ref_mv_stack[ref_idx].weight >= REF_CAT_LEVEL &&
        ref_mv_stack[ref_idx + 1].weight < REF_CAT_LEVEL)
    {
        return 1;
    }

    if (ref_mv_stack[ref_idx].weight < REF_CAT_LEVEL &&
        ref_mv_stack[ref_idx + 1].weight < REF_CAT_LEVEL)
    {
        return 2;
    }

    return 0;
}

static INLINE void svt_collect_neighbors_ref_counts(PartitionInfo_t *pi) {

    ZERO_ARRAY(&pi->neighbors_ref_counts[0],
        sizeof(pi->neighbors_ref_counts[0])*REF_FRAMES);

    uint8_t *const ref_counts = pi->neighbors_ref_counts;

    const ModeInfo_t *const above_mbmi = pi->above_mbmi;
    const ModeInfo_t *const left_mbmi = pi->left_mbmi;
    const int above_in_image = pi->up_available;
    const int left_in_image = pi->left_available;

    // Above neighbor
    if (above_in_image && dec_is_inter_block(above_mbmi)) {
        ref_counts[above_mbmi->ref_frame[0]]++;
        if (has_second_ref(above_mbmi))
            ref_counts[above_mbmi->ref_frame[1]]++;
    }

    // Left neighbor
    if (left_in_image && dec_is_inter_block(left_mbmi)) {
        ref_counts[left_mbmi->ref_frame[0]]++;
        if (has_second_ref(left_mbmi))
            ref_counts[left_mbmi->ref_frame[1]]++;
    }
}

static int32_t get_pred_context_comp_ref_p(PartitionInfo_t *pi) {
    const uint8_t *const ref_counts = &pi->neighbors_ref_counts[0];

    // Count of LAST + LAST2
    const int32_t last_last2_count = ref_counts[LAST_FRAME] + ref_counts[LAST2_FRAME];
    // Count of LAST3 + GOLDEN
    const int32_t last3_gld_count =
        ref_counts[LAST3_FRAME] + ref_counts[GOLDEN_FRAME];

    const int32_t pred_context = (last_last2_count == last3_gld_count) ? 1 :
        ((last_last2_count < last3_gld_count) ? 0 : 2);

    assert(pred_context >= 0 && pred_context < REF_CONTEXTS);
    return pred_context;
}

static int32_t get_pred_context_comp_bwdref_p(PartitionInfo_t *pi) {
    const uint8_t *const ref_counts = &pi->neighbors_ref_counts[0];

    // Counts of BWDREF, ALTREF2, or ALTREF frames (B, A2, or A)
    const int32_t brfarf2_count =
        ref_counts[BWDREF_FRAME] + ref_counts[ALTREF2_FRAME];
    const int32_t arf_count = ref_counts[ALTREF_FRAME];

    const int32_t pred_context =
        (brfarf2_count == arf_count) ? 1 : ((brfarf2_count < arf_count) ? 0 : 2);

    assert(pred_context >= 0 && pred_context < REF_CONTEXTS);
    return pred_context;
}

static int32_t get_pred_context_comp_bwdref_p1(PartitionInfo_t *pi) {
    const uint8_t *const ref_counts = &pi->neighbors_ref_counts[0];

    // Count of BWDREF frames (B)
    const int32_t brf_count = ref_counts[BWDREF_FRAME];
    // Count of ALTREF2 frames (A2)
    const int32_t arf2_count = ref_counts[ALTREF2_FRAME];

    const int32_t pred_context =
        (brf_count == arf2_count) ? 1 : ((brf_count < arf2_count) ? 0 : 2);

    assert(pred_context >= 0 && pred_context < REF_CONTEXTS);
    return pred_context;
}

int get_pred_context_uni_comp_ref_p2(PartitionInfo_t *pi) {
    const uint8_t *const ref_counts = &pi->neighbors_ref_counts[0];

    // Count of LAST3
    const int last3_count = ref_counts[LAST3_FRAME];
    // Count of GOLDEN
    const int gld_count = ref_counts[GOLDEN_FRAME];

    const int pred_context =
        (last3_count == gld_count) ? 1 : ((last3_count < gld_count) ? 0 : 2);

    assert(pred_context >= 0 && pred_context < UNI_COMP_REF_CONTEXTS);
    return pred_context;
}

int get_pred_context_uni_comp_ref_p1(PartitionInfo_t *pi) {
    const uint8_t *const ref_counts = &pi->neighbors_ref_counts[0];

    // Count of LAST2
    const int last2_count = ref_counts[LAST2_FRAME];
    // Count of LAST3 or GOLDEN
    const int last3_or_gld_count =
        ref_counts[LAST3_FRAME] + ref_counts[GOLDEN_FRAME];

    const int pred_context = (last2_count == last3_or_gld_count)
        ? 1
        : ((last2_count < last3_or_gld_count) ? 0 : 2);

    assert(pred_context >= 0 && pred_context < UNI_COMP_REF_CONTEXTS);
    return pred_context;
}

int get_pred_context_uni_comp_ref_p(PartitionInfo_t *pi) {
    const uint8_t *const ref_counts = &pi->neighbors_ref_counts[0];

    // Count of forward references (L, L2, L3, or G)
    const int frf_count = ref_counts[LAST_FRAME] + ref_counts[LAST2_FRAME] +
        ref_counts[LAST3_FRAME] + ref_counts[GOLDEN_FRAME];
    // Count of backward references (B or A)
    const int brf_count = ref_counts[BWDREF_FRAME] + ref_counts[ALTREF2_FRAME] +
        ref_counts[ALTREF_FRAME];

    const int pred_context =
        (frf_count == brf_count) ? 1 : ((frf_count < brf_count) ? 0 : 2);

    assert(pred_context >= 0 && pred_context < UNI_COMP_REF_CONTEXTS);
    return pred_context;
}

int32_t get_pred_context_single_ref_p1(PartitionInfo_t *pi) {
    const uint8_t *const ref_counts = &pi->neighbors_ref_counts[0];

    // Count of forward reference frames
    const int32_t fwd_count = ref_counts[LAST_FRAME] + ref_counts[LAST2_FRAME] +
        ref_counts[LAST3_FRAME] + ref_counts[GOLDEN_FRAME];
    // Count of backward reference frames
    const int32_t bwd_count = ref_counts[BWDREF_FRAME] + ref_counts[ALTREF2_FRAME] +
        ref_counts[ALTREF_FRAME];

    const int32_t pred_context =
        (fwd_count == bwd_count) ? 1 : ((fwd_count < bwd_count) ? 0 : 2);

    assert(pred_context >= 0 && pred_context < REF_CONTEXTS);
    return pred_context;
}

static int32_t get_pred_context_last3_or_gld(PartitionInfo_t *pi) {
    const uint8_t *const ref_counts = &pi->neighbors_ref_counts[0];

    // Count of LAST3
    const int32_t last3_count = ref_counts[LAST3_FRAME];
    // Count of GOLDEN
    const int32_t gld_count = ref_counts[GOLDEN_FRAME];

    const int32_t pred_context =
        (last3_count == gld_count) ? 1 : ((last3_count < gld_count) ? 0 : 2);

    assert(pred_context >= 0 && pred_context < REF_CONTEXTS);
    return pred_context;
}

static int32_t get_pred_context_single_ref_p4(PartitionInfo_t *pi) {
    const uint8_t *const ref_counts = &pi->neighbors_ref_counts[0];

    // Count of LAST
    const int32_t last_count = ref_counts[LAST_FRAME];
    // Count of LAST2
    const int32_t last2_count = ref_counts[LAST2_FRAME];

    const int32_t pred_context =
        (last_count == last2_count) ? 1 : ((last_count < last2_count) ? 0 : 2);

    assert(pred_context >= 0 && pred_context < REF_CONTEXTS);
    return pred_context;
}

void read_ref_frames(EbDecHandle *dec_handle, PartitionInfo_t *const pi,
    SvtReader *r)
{
    int segment_id = pi->mi->segment_id;
    MvReferenceFrame *ref_frame = pi->mi->ref_frame;
    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    AomCdfProb *cdf;
    SegmentationParams *seg_params = &dec_handle->frame_header.segmentation_params;
    if (pi->mi->skip_mode) {
        ref_frame[0] = (MvReferenceFrame)(LAST_FRAME +
            dec_handle->frame_header.skip_mode_params.skip_mode_frame[0]);
        ref_frame[1] = (MvReferenceFrame)(LAST_FRAME +
            dec_handle->frame_header.skip_mode_params.skip_mode_frame[1]);
    }
    else if (seg_feature_active(seg_params, segment_id, SEG_LVL_REF_FRAME)) {
        ref_frame[0] = (MvReferenceFrame)get_segdata(seg_params, segment_id,
            SEG_LVL_REF_FRAME);
        ref_frame[1] = NONE_FRAME;
    }
    else if (seg_feature_active(seg_params, segment_id, SEG_LVL_SKIP) ||
        seg_feature_active(seg_params, segment_id, SEG_LVL_GLOBALMV))
    {
        ref_frame[0] = LAST_FRAME;
        ref_frame[1] = NONE_FRAME;
    }
    else {
        ReferenceMode mode = SINGLE_REFERENCE;
        int bw4 = mi_size_wide[pi->mi->sb_type];
        int bh4 = mi_size_high[pi->mi->sb_type];
        if (dec_handle->frame_header.reference_mode == REFERENCE_MODE_SELECT
            && (AOMMIN(bw4, bh4) >= 2))
        {
            const int ctx = get_reference_mode_context(pi);
            mode = (ReferenceMode)svt_read_symbol(r,
                parse_ctxt->cur_tile_ctx.comp_inter_cdf[ctx], 2, ACCT_STR);
        }

        if (mode == COMPOUND_REFERENCE) {
            int pred_context;
            const int ctx = get_comp_reference_type_context(pi);
            const CompReferenceType comp_ref_type =
                (CompReferenceType)svt_read_symbol(
                    r, parse_ctxt->cur_tile_ctx.comp_ref_type_cdf[ctx], 2, ACCT_STR);

            if (comp_ref_type == UNIDIR_COMP_REFERENCE) {
                pred_context = get_pred_context_uni_comp_ref_p(pi);
                uint16_t bit = (uint16_t)svt_read_symbol(r,
                    parse_ctxt->cur_tile_ctx.uni_comp_ref_cdf[pred_context][0],
                    2, ACCT_STR);
                if (bit) {
                    ref_frame[0] = BWDREF_FRAME;
                    ref_frame[1] = ALTREF_FRAME;
                }
                else {
                    pred_context = get_pred_context_uni_comp_ref_p1(pi);
                    uint16_t bit1 = (uint16_t)svt_read_symbol(r,
                        parse_ctxt->cur_tile_ctx.uni_comp_ref_cdf[pred_context][1],
                        2, ACCT_STR);
                    if (bit1) {
                        pred_context = get_pred_context_uni_comp_ref_p2(pi);
                        uint16_t bit2 = (uint16_t)svt_read_symbol(r,
                            parse_ctxt->cur_tile_ctx.uni_comp_ref_cdf[pred_context][2],
                            2, ACCT_STR);
                        if (bit2) {
                            ref_frame[0] = LAST_FRAME;
                            ref_frame[1] = GOLDEN_FRAME;
                        }
                        else {
                            ref_frame[0] = LAST_FRAME;
                            ref_frame[1] = LAST3_FRAME;
                        }
                    }
                    else {
                        ref_frame[0] = LAST_FRAME;
                        ref_frame[1] = LAST2_FRAME;
                    }
                }
                return;
            }

            assert(comp_ref_type == BIDIR_COMP_REFERENCE);

            const int idx = 1;
            pred_context = get_pred_context_comp_ref_p(pi);
            uint16_t bit = (uint16_t)svt_read_symbol(r,
                parse_ctxt->cur_tile_ctx.comp_ref_cdf[pred_context][0], 2, ACCT_STR);
            // Decode forward references.
            if (!bit) {
                uint16_t bit1 = (uint16_t)svt_read_symbol(r, parse_ctxt->cur_tile_ctx.
                    comp_ref_cdf[get_pred_context_single_ref_p4(pi)][1], 2, ACCT_STR);
                ref_frame[!idx] = bit1 ? LAST2_FRAME : LAST_FRAME;
            }
            else {
                uint16_t bit2 = (uint16_t)svt_read_symbol(r, parse_ctxt->cur_tile_ctx.
                    comp_ref_cdf[get_pred_context_last3_or_gld(pi)][2], 2, ACCT_STR);
                ref_frame[!idx] = bit2 ? GOLDEN_FRAME : LAST3_FRAME;
            }

            // Decode backward references.
            pred_context = get_pred_context_comp_bwdref_p(pi);
            uint16_t bit_bwd = (uint16_t)svt_read_symbol(r, parse_ctxt->cur_tile_ctx.
                comp_bwdref_cdf[pred_context][0], 2, ACCT_STR);
            if (!bit_bwd) {
                pred_context = get_pred_context_comp_bwdref_p1(pi);
                uint16_t bit1_bwd = (uint16_t)svt_read_symbol(r, parse_ctxt->cur_tile_ctx.
                    comp_bwdref_cdf[pred_context][1], 2, ACCT_STR);
                ref_frame[idx] = bit1_bwd ? ALTREF2_FRAME : BWDREF_FRAME;
            }
            else {
                ref_frame[idx] = ALTREF_FRAME;
            }
        }
        else if (mode == SINGLE_REFERENCE) {

            cdf = parse_ctxt->cur_tile_ctx.
                single_ref_cdf[get_pred_context_single_ref_p1(pi)][0];
            const int32_t bit0 = svt_read_symbol(r, cdf, 2, ACCT_STR);

            if (bit0) {
                cdf = parse_ctxt->cur_tile_ctx.
                    single_ref_cdf[get_pred_context_comp_bwdref_p(pi)][1];
                const int32_t bit1 = svt_read_symbol(r, cdf, 2, ACCT_STR);
                if (!bit1) {
                    cdf = parse_ctxt->cur_tile_ctx.
                        single_ref_cdf[get_pred_context_comp_bwdref_p1(pi)][5];
                    const int32_t bit5 = svt_read_symbol(r, cdf, 2, ACCT_STR);
                    ref_frame[0] = bit5 ? ALTREF2_FRAME : BWDREF_FRAME;
                }
                else {
                    ref_frame[0] = ALTREF_FRAME;
                }
            }
            else {
                cdf = parse_ctxt->cur_tile_ctx.
                    single_ref_cdf[get_pred_context_comp_ref_p(pi)][2];
                const int32_t bit2 = svt_read_symbol(r, cdf, 2, ACCT_STR);
                if (bit2) {
                    cdf = parse_ctxt->cur_tile_ctx.
                        single_ref_cdf[get_pred_context_last3_or_gld(pi)][4];
                    const int32_t bit4 = svt_read_symbol(r, cdf, 2, ACCT_STR);
                    ref_frame[0] = bit4 ? GOLDEN_FRAME : LAST3_FRAME;
                }
                else {
                    cdf = parse_ctxt->cur_tile_ctx.
                        single_ref_cdf[get_pred_context_single_ref_p4(pi)][3];
                    const int32_t bit3 = svt_read_symbol(r, cdf, 2, ACCT_STR);
                    ref_frame[0] = bit3 ? LAST2_FRAME : LAST_FRAME;
                }
            }

            ref_frame[1] = NONE_FRAME;
        }
        else
            assert(0 && "Invalid prediction mode.");
    }
}
static void read_drl_idx(EbDecHandle *dec_handle, PartitionInfo_t *pi,
    ModeInfo_t *mbmi, SvtReader *r, int num_mv_found)
{
    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    uint8_t ref_frame_type = av1_ref_frame_type(mbmi->ref_frame);
    mbmi->ref_mv_idx = 0;
    if (mbmi->mode == NEWMV || mbmi->mode == NEW_NEWMV) {
        for (int idx = 0; idx < 2; ++idx) {
            if (num_mv_found > idx + 1) {
                uint8_t drl_ctx = get_drl_ctx(pi->ref_mv_stack[ref_frame_type], idx);
                int drl_idx = svt_read_symbol(r, parse_ctxt->cur_tile_ctx.
                    drl_cdf[drl_ctx], 2, ACCT_STR);
                mbmi->ref_mv_idx = idx;
                if (!drl_idx) return;
                mbmi->ref_mv_idx = idx + 1;
            }
        }
    }
    if (have_nearmv_in_inter_mode(mbmi->mode)) {
        //mbmi->ref_mv_idx = 1;
        for (int idx = 1; idx < 3; ++idx) {
            if (num_mv_found > idx + 1) {
                uint8_t drl_ctx = get_drl_ctx(pi->ref_mv_stack[ref_frame_type], idx);
                int drl_idx = svt_read_symbol(r, parse_ctxt->cur_tile_ctx.
                    drl_cdf[drl_ctx], 2, ACCT_STR);
                mbmi->ref_mv_idx = idx + drl_idx - 1;
                //mbmi->ref_mv_idx = idx;
                if (!drl_idx) return;
                //mbmi->ref_mv_idx = idx + 1;
            }
        }
    }
}

static INLINE void integer_mv_precision(MV_dec *mv) {
    int mod = (mv->row % 8);
    if (mod != 0) {
        mv->row -= mod;
        if (abs(mod) > 4) {
            if (mod > 0) {
                mv->row += 8;
            }
            else {
                mv->row -= 8;
            }
        }
    }

    mod = (mv->col % 8);
    if (mod != 0) {
        mv->col -= mod;
        if (abs(mod) > 4) {
            if (mod > 0) {
                mv->col += 8;
            }
            else {
                mv->col -= 8;
            }
        }
    }
}

static INLINE void lower_mv_precision(MV_dec *mv, int allow_hp, int is_integer) {
    if (is_integer)
        integer_mv_precision(mv);
    else {
        if (!allow_hp) {
            if (mv->row & 1) mv->row += (mv->row > 0 ? -1 : 1);
            if (mv->col & 1) mv->col += (mv->col > 0 ? -1 : 1);
        }
    }
}
/* TODO: Harmonize*/
static void svt_find_best_ref_mvs(int allow_hp, IntMv_dec *mvlist,
    IntMv_dec *nearest_mv, IntMv_dec *near_mv, int is_integer)
{
  int i;
  // Make sure all the candidates are properly clamped etc
  for (i = 0; i < MAX_MV_REF_CANDIDATES; ++i) {
    lower_mv_precision(&mvlist[i].as_mv, allow_hp, is_integer);
  }
  *nearest_mv = mvlist[0];
  *near_mv = mvlist[1];
}

static INLINE int is_inside(TileInfo *tile, int mi_col, int mi_row) {
    return (mi_col >= tile->mi_col_start && mi_col < tile->mi_col_end &&
            mi_row >= tile->mi_row_start && mi_row < tile->mi_row_end);
}

static INLINE int is_global_mv_block(const ModeInfo_t *const mbmi,
    TransformationType type)
{
    const PredictionMode mode = mbmi->mode;
    const BlockSize bsize = mbmi->sb_type;
    const int block_size_allowed =
        AOMMIN(block_size_wide[bsize], block_size_high[bsize]) >= 8;
    return (mode == GLOBALMV || mode == GLOBAL_GLOBALMV) && type > TRANSLATION &&
        block_size_allowed;
}

void lower_precision(FrameHeader *frm_header, int16_t *cand) {
    if (frm_header->force_integer_mv) {
        int a = abs(*cand);
        int a_int = (a + 3) >> 3;
        if (*cand > 0)
           *cand = a_int << 3;
        else
            *cand = -(a_int << 3);
    }
    else {
        if (*cand & 1) {
            if (*cand > 0) *cand--;
            else *cand++;
        }
    }
}

int has_newmv(PredictionMode mode) {
    return (mode == NEWMV ||
        mode == NEW_NEWMV ||
        mode == NEAR_NEWMV ||
        mode == NEW_NEARMV ||
        mode == NEAREST_NEWMV ||
        mode == NEW_NEARESTMV);
}


static void add_ref_mv_candidate(EbDecHandle *dec_handle,
    const ModeInfo_t *const candidate, const MvReferenceFrame rf[2],
    uint8_t *num_mv_found, uint8_t *found_match, uint8_t *newmv_count,
    CandidateMv_dec *ref_mv_stack, IntMv_dec *gm_mv_candidates, int weight)
{
    //FrameHeader *frm_header = &dec_handle->frame_header;
    if (!dec_is_inter_block(candidate)) return;  // for intrabc
    int index = 0, ref;
    assert(weight % 2 == 0);

    EbDecPicBuf *buf = dec_handle->cur_pic_buf[0];
    if (rf[1] == NONE_FRAME) {
        // single reference frame
        for (ref = 0; ref < 2; ++ref) {
            if (candidate->ref_frame[ref] == rf[0]) {
                IntMv_dec this_refmv;
                if (is_global_mv_block(candidate, buf->global_motion[rf[0]].gm_type))
                    this_refmv = gm_mv_candidates[0];
                else
                    this_refmv = candidate->mv[ref];

                //if (!frm_header->allow_high_precision_mv) {
                //    lower_precision(frm_header, &this_refmv.as_mv.row);
                //    lower_precision(frm_header, &this_refmv.as_mv.col);
                //}

                for (index = 0; index < *num_mv_found; ++index)
                    if (ref_mv_stack[index].this_mv.as_int == this_refmv.as_int) break;

                if (index < *num_mv_found) ref_mv_stack[index].weight += weight;

                // Add a new item to the list.
                if (index == *num_mv_found && *num_mv_found < MAX_REF_MV_STACK_SIZE) {
                    ref_mv_stack[index].this_mv.as_int = this_refmv.as_int;
                    ref_mv_stack[index].weight = weight;
                    ++(*num_mv_found);
                }
                if (has_newmv(candidate->mode))++*newmv_count;
                ++*found_match;
            }
        }
    }
    else {
        // compound reference frame
        if (candidate->ref_frame[0] == rf[0] && candidate->ref_frame[1] == rf[1]) {
            IntMv_dec this_refmv[2];
            for (ref = 0; ref < 2; ++ref) {
                if (is_global_mv_block(candidate, buf->global_motion[rf[ref]].gm_type))
                    this_refmv[ref] = gm_mv_candidates[ref];
                else
                    this_refmv[ref] = candidate->mv[ref];

                /*if (!frm_header->allow_high_precision_mv) {
                    lower_precision(frm_header, &this_refmv[ref].as_mv.row);
                    lower_precision(frm_header, &this_refmv[ref].as_mv.col);
                }*/
            }

            //*found_match = 1;

            for (index = 0; index < *num_mv_found; ++index)
                if ((ref_mv_stack[index].this_mv.as_int == this_refmv[0].as_int) &&
                    (ref_mv_stack[index].comp_mv.as_int == this_refmv[1].as_int))
                    break;

            if (index < *num_mv_found) ref_mv_stack[index].weight += weight;

            // Add a new item to the list.
            if (index == *num_mv_found && *num_mv_found < MAX_REF_MV_STACK_SIZE) {
                ref_mv_stack[index].this_mv.as_int = this_refmv[0].as_int;
                ref_mv_stack[index].comp_mv.as_int = this_refmv[1].as_int;
                ref_mv_stack[index].weight = weight;
                ++(*num_mv_found);
            }
            if (has_newmv(candidate->mode)) ++*newmv_count;
            ++*found_match;
        }
    }
}


static void scan_row_mbmi(EbDecHandle *dec_handle, PartitionInfo_t *pi,
    int delta_row, int mi_row, int mi_col, const MvReferenceFrame rf[2],
    CandidateMv_dec *ref_mv_stack, uint8_t *num_mv_found, uint8_t *found_match,
    uint8_t *newmv_count, IntMv_dec *gm_mv_candidates, int max_row_offset,
    int *processed_rows)
{
    int bw4 = mi_size_wide[pi->mi->sb_type];
    ParseCtxt *parse_ctx = (ParseCtxt*)dec_handle->pv_parse_ctxt;
    FrameHeader *frm_header = &dec_handle->frame_header;
    int end4 = AOMMIN(AOMMIN(bw4, frm_header->mi_cols - mi_col), 16);
    int delta_col = 0;
    int use_step_16 = (bw4 >= 16);
    const int n8_w_8 = mi_size_wide[BLOCK_8X8];
    const int n8_w_16 = mi_size_wide[BLOCK_16X16];

    if (abs(delta_row) > 1) {
        delta_col = 1;
        if ((mi_col & 0x01) && bw4 < n8_w_8) --delta_col;
    }

    for (int i = 0; i < end4;) {
        int mv_row = mi_row + delta_row;
        int mv_col = mi_col + delta_col + i;
        if (!is_inside(&parse_ctx->cur_tile_info, mv_col, mv_row))
            break;
        ModeInfo_t *candidate = get_cur_mode_info(dec_handle,
            mv_row, mv_col, pi->sb_info);
        int len = AOMMIN(bw4, mi_size_wide[candidate->sb_type]);
        const int n4_w = mi_size_wide[candidate->sb_type];
        if (use_step_16)
            len = AOMMAX(n8_w_16, len);
        else if (abs(delta_row) > 1)
            len = AOMMAX(n8_w_8, len);

        int weight = 2;
        if (bw4 >= n8_w_8 && bw4 <= n4_w) {
            int inc = AOMMIN(-max_row_offset + delta_row + 1,
                mi_size_high[candidate->sb_type]);
            // Obtain range used in weight calculation.
            weight = AOMMAX(weight, inc);
            *processed_rows = inc - delta_row - 1;
        }
        add_ref_mv_candidate(dec_handle, candidate, rf, num_mv_found, found_match,
            newmv_count, ref_mv_stack, gm_mv_candidates, len * weight);

        i += len;
    }
}

static void scan_col_mbmi(EbDecHandle *dec_handle, PartitionInfo_t *pi,
    int delta_col, int mi_row, int mi_col, const MvReferenceFrame rf[2],
    CandidateMv_dec *ref_mv_stack, uint8_t *num_mv_found, uint8_t *found_match,
    uint8_t *newmv_count, IntMv_dec *gm_mv_candidates, int max_col_offset,
    int *processed_cols)
{
    int bh4 = mi_size_high[pi->mi->sb_type];
    ParseCtxt *parse_ctx = (ParseCtxt*)dec_handle->pv_parse_ctxt;
    FrameHeader *frm_header = &dec_handle->frame_header;
    int end4 = AOMMIN(AOMMIN(bh4, frm_header->mi_rows - mi_row), 16);
    int delta_row = 0;
    int use_step_16 = (bh4 >= 16);
    const int n8_h_8 = mi_size_high[BLOCK_8X8];

    if (abs(delta_col) > 1) {
        delta_row = 1;
        if ((mi_row & 0x01) && bh4 < n8_h_8) --delta_row;
    }

    for (int i = 0; i < end4;) {
        int mv_row = mi_row + delta_row + i;
        int mv_col = mi_col + delta_col;
        if (!is_inside(&parse_ctx->cur_tile_info, mv_col, mv_row))
            break;
        ModeInfo_t *candidate = get_cur_mode_info(dec_handle,
            mv_row, mv_col, pi->sb_info);
        int len = AOMMIN(bh4, mi_size_high[candidate->sb_type]);
        const int n4_h = mi_size_high[candidate->sb_type];
        if (abs(delta_col) > 1)
            len = AOMMAX(2, len);
        if (use_step_16)
            len = AOMMAX(4, len);

        int weight = 2;
        if (bh4 >= n8_h_8 && bh4 <= n4_h) {
            int inc = AOMMIN(-max_col_offset + delta_col + 1,
                mi_size_wide[candidate->sb_type]);
            // Obtain range used in weight calculation.
            weight = AOMMAX(weight, inc);
            *processed_cols = inc - delta_col - 1;
        }

        add_ref_mv_candidate(dec_handle, candidate, rf, num_mv_found, found_match,
            newmv_count, ref_mv_stack, gm_mv_candidates, len * weight);

        i += len;
    }
}

static void scan_blk_mbmi(EbDecHandle *dec_handle, PartitionInfo_t *pi,
    int delta_row, int delta_col, const int mi_row, const int mi_col,
    const MvReferenceFrame rf[2], CandidateMv_dec *ref_mv_stack,
    uint8_t *found_match, uint8_t *newmv_count, IntMv_dec *gm_mv_candidates,
    uint8_t num_mv_found[MODE_CTX_REF_FRAMES])
{
    ParseCtxt *parse_ctx = (ParseCtxt*)dec_handle->pv_parse_ctxt;

    int mv_row = mi_row + delta_row;
    int mv_col = mi_col + delta_col;
    int weight = 4;

    if (is_inside(&parse_ctx->cur_tile_info, mv_col, mv_row)) {
        ModeInfo_t * candidate = get_cur_mode_info(dec_handle,
            mv_row, mv_col, pi->sb_info);

        add_ref_mv_candidate(dec_handle, candidate, rf, num_mv_found, found_match,
            newmv_count, ref_mv_stack, gm_mv_candidates, weight);
    }  // Analyze a single 8x8 block motion information.
}

static void get_mv_projection(MV_dec *output, MV_dec ref, int num, int den) {
    den = AOMMIN(den, MAX_FRAME_DISTANCE);
    num = num > 0 ? AOMMIN(num, MAX_FRAME_DISTANCE)
        : AOMMAX(num, -MAX_FRAME_DISTANCE);
    const int mv_row =
        ROUND_POWER_OF_TWO_SIGNED(ref.row * num * div_mult[den], 14);
    const int mv_col =
        ROUND_POWER_OF_TWO_SIGNED(ref.col * num * div_mult[den], 14);
    const int clamp_max = MV_UPP - 1;
    const int clamp_min = MV_LOW + 1;
    output->row = (int16_t)clamp(mv_row, clamp_min, clamp_max);
    output->col = (int16_t)clamp(mv_col, clamp_min, clamp_max);
}

static int add_tpl_ref_mv(EbDecHandle *dec_handle, int mi_row, int mi_col,
    MvReferenceFrame ref_frame, int blk_row, int blk_col,
    IntMv_dec *gm_mv_candidates, uint8_t *num_mv_found,
    CandidateMv_dec ref_mv_stacks[][MAX_REF_MV_STACK_SIZE], int16_t *mode_context)
{
    uint8_t idx;
    ParseCtxt *parse_ctx = (ParseCtxt*)dec_handle->pv_parse_ctxt;
    FrameHeader *frm_header = &dec_handle->frame_header;
    int mv_row = (mi_row + blk_row) | 1;
    int mv_col = (mi_col + blk_col) | 1;

    if (!is_inside(&parse_ctx->cur_tile_info, mv_col, mv_row)) return 0;

    int x8 = mv_col >> 1;
    int y8 = mv_row >> 1;

    MvReferenceFrame rf[2];
    av1_set_ref_frame(rf, ref_frame);

    const TemporalMvRef *tpl_mvs = dec_handle->master_frame_buf.tpl_mvs +
                                    y8 * (frm_header->mi_stride >> 1) + x8;
    const IntMv_dec prev_frame_mvs = tpl_mvs->mf_mv0;

    if (rf[1] == NONE_FRAME) {
        EbDecPicBuf *cur_buf = dec_handle->cur_pic_buf[0];
        int cur_frame_index = cur_buf->order_hint;
        EbDecPicBuf *buf_0 = get_ref_frame_buf(dec_handle, rf[0]);
        int frame0_index = buf_0->order_hint;
        int cur_offset_0 = get_relative_dist(&dec_handle->seq_header.order_hint_info,
            cur_frame_index, frame0_index);
        CandidateMv_dec *ref_mv_stack = ref_mv_stacks[rf[0]];

        if (prev_frame_mvs.as_mv.row == INVALID_MV)
            return 0;

        IntMv_dec this_refmv;
        /*get_mv_projection(&this_refmv.as_mv, prev_frame_mvs.as_mv,
            cur_offset_0, prev_frame_mvs->ref_frame_offset);*/

        lower_mv_precision(&this_refmv.as_mv, frm_header->allow_high_precision_mv,
            frm_header->force_integer_mv);

        if (blk_row == 0 && blk_col == 0) {
            if (abs(this_refmv.as_mv.row - gm_mv_candidates[0].as_mv.row) >= 16 ||
                abs(this_refmv.as_mv.col - gm_mv_candidates[0].as_mv.col) >= 16)
                /*zero_mv_ctxt*/
                mode_context[ref_frame] |= (1 << GLOBALMV_OFFSET);
        }

        for (idx = 0; idx < *num_mv_found; ++idx)
            if (this_refmv.as_int == ref_mv_stack[idx].this_mv.as_int) break;

        if (idx < *num_mv_found)
            ref_mv_stack[idx].weight += 2;
        else if (*num_mv_found < MAX_REF_MV_STACK_SIZE) {
            ref_mv_stack[idx].this_mv.as_int = this_refmv.as_int;
            ref_mv_stack[idx].weight = 2 ;
            ++(*num_mv_found);
        }
        return 1;
    }
    else {
        // Process compound inter mode
        EbDecPicBuf *cur_buf = dec_handle->cur_pic_buf[0];
        int cur_frame_index = cur_buf->order_hint;
        const EbDecPicBuf *buf_0 = get_ref_frame_buf(dec_handle, rf[0]);
        int frame0_index = buf_0->order_hint;

        int cur_offset_0 = get_relative_dist(&dec_handle->seq_header.order_hint_info,
            cur_frame_index, frame0_index);
        const EbDecPicBuf *buf_1 = get_ref_frame_buf(dec_handle, rf[1]);
        int frame1_index = buf_1->order_hint;
        int cur_offset_1 = get_relative_dist(&dec_handle->seq_header.order_hint_info,
            cur_frame_index, frame1_index);

        CandidateMv_dec *ref_mv_stack = ref_mv_stacks[ref_frame];
        if (prev_frame_mvs.as_int == INVALID_MV)
            return 0;

        IntMv_dec this_refmv;
        IntMv_dec comp_refmv;
        /*get_mv_projection(&this_refmv.as_mv, prev_frame_mvs.as_mv,
            cur_offset_0, prev_frame_mvs->ref_frame_offset);
        get_mv_projection(&comp_refmv.as_mv, prev_frame_mvs.as_mv,
            cur_offset_1, prev_frame_mvs->ref_frame_offset);*/

        lower_mv_precision(&this_refmv.as_mv, frm_header->allow_high_precision_mv,
            frm_header->force_integer_mv);
        lower_mv_precision(&comp_refmv.as_mv, frm_header->allow_high_precision_mv,
            frm_header->force_integer_mv);

        if (blk_row == 0 && blk_col == 0) {
            if (abs(this_refmv.as_mv.row - gm_mv_candidates[0].as_mv.row) >= 16 ||
                abs(this_refmv.as_mv.col - gm_mv_candidates[0].as_mv.col) >= 16 ||
                abs(comp_refmv.as_mv.row - gm_mv_candidates[1].as_mv.row) >= 16 ||
                abs(comp_refmv.as_mv.col - gm_mv_candidates[1].as_mv.col) >= 16)
                /*zero_mv_ctxt*/
                mode_context[ref_frame] |= (1 << GLOBALMV_OFFSET);
        }

        for (idx = 0; idx < *num_mv_found; ++idx)
            if (this_refmv.as_int == ref_mv_stack[idx].this_mv.as_int &&
                comp_refmv.as_int == ref_mv_stack[idx].comp_mv.as_int)
                break;

        if (idx < *num_mv_found)
            ref_mv_stack[idx].weight += 2 ;
        else if (*num_mv_found < MAX_REF_MV_STACK_SIZE) {
            ref_mv_stack[idx].this_mv.as_int = this_refmv.as_int;
            ref_mv_stack[idx].comp_mv.as_int = comp_refmv.as_int;
            ref_mv_stack[idx].weight = 2;
            ++(*num_mv_found);
        }
    }
    return 1;
}

// Note: motion_filed_projection finds motion vectors of current frame's
// reference frame, and projects them to current frame. To make it clear,
// let's call current frame's reference frame as start frame.
// Call Start frame's reference frames as reference frames.
// Call ref_offset as frame distances between start frame and its reference
// frames.
static int motion_field_projection(EbDecHandle *dec_handle,
    MvReferenceFrame start_frame, int dir)
{
    TemporalMvRef *tpl_mvs_base = dec_handle->master_frame_buf.tpl_mvs;
    int ref_offset[REF_FRAMES] = { 0 };

    const EbDecPicBuf *const start_frame_buf =
        get_ref_frame_buf(dec_handle, start_frame);
    if (start_frame_buf == NULL) return 0;

    /* TODO: add for 2nd frame onwards */
    return 0;
#if 0
    if (start_frame_buf->frame_type == KEY_FRAME ||
        start_frame_buf->frame_type == INTRA_ONLY_FRAME)
        return 0;

    if (start_frame_buf->mi_rows != cm->mi_rows ||
        start_frame_buf->mi_cols != cm->mi_cols)
        return 0;

    const int start_frame_order_hint = start_frame_buf->order_hint;
    const unsigned int *const ref_order_hints =
        &start_frame_buf->ref_order_hints[0];
    const int cur_order_hint = cm->cur_frame->order_hint;
    int start_to_current_frame_offset = get_relative_dist(
        &cm->seq_params.order_hint_info, start_frame_order_hint, cur_order_hint);

    for (MvReferenceFrame rf = LAST_FRAME; rf <= INTER_REFS_PER_FRAME; ++rf) {
        ref_offset[rf] = get_relative_dist(&cm->seq_params.order_hint_info,
            start_frame_order_hint,
            ref_order_hints[rf - LAST_FRAME]);
        }

    if (dir == 2) start_to_current_frame_offset = -start_to_current_frame_offset;

    MV_REF *mv_ref_base = start_frame_buf->mvs;
    const int mvs_rows = (cm->mi_rows + 1) >> 1;
    const int mvs_cols = (cm->mi_cols + 1) >> 1;

    for (int blk_row = 0; blk_row < mvs_rows; ++blk_row) {
        for (int blk_col = 0; blk_col < mvs_cols; ++blk_col) {
            MV_REF *mv_ref = &mv_ref_base[blk_row * mvs_cols + blk_col];
            MV fwd_mv = mv_ref->mv.as_mv;

            if (mv_ref->ref_frame > INTRA_FRAME) {
                int_mv this_mv;
                int mi_r, mi_c;
                const int ref_frame_offset = ref_offset[mv_ref->ref_frame];

                int pos_valid =
                    abs(ref_frame_offset) <= MAX_FRAME_DISTANCE &&
                    ref_frame_offset > 0 &&
                    abs(start_to_current_frame_offset) <= MAX_FRAME_DISTANCE;

                if (pos_valid) {
                    get_mv_projection(&this_mv.as_mv, fwd_mv,
                        start_to_current_frame_offset, ref_frame_offset);
                    pos_valid = get_block_position(cm, &mi_r, &mi_c, blk_row, blk_col,
                        this_mv.as_mv, dir >> 1);
                    }

                if (pos_valid) {
                    const int mi_offset = mi_r * (cm->mi_stride >> 1) + mi_c;

                    tpl_mvs_base[mi_offset].mfmv0.as_mv.row = fwd_mv.row;
                    tpl_mvs_base[mi_offset].mfmv0.as_mv.col = fwd_mv.col;
                    tpl_mvs_base[mi_offset].ref_frame_offset = ref_frame_offset;
                    }
                }
            }
        }

    return 1;
#endif
}

void svt_setup_motion_field(EbDecHandle *dec_handle) {

    OrderHintInfo *order_hint_info = &dec_handle->
                                        seq_header.order_hint_info;

    memset(dec_handle->master_frame_buf.ref_frame_side, 0,
        sizeof(dec_handle->master_frame_buf.ref_frame_side));
    if (!order_hint_info->enable_order_hint) return;

    TemporalMvRef *tpl_mvs_base = dec_handle->master_frame_buf.tpl_mvs;
    int size = ((dec_handle->frame_header.mi_rows + MAX_MIB_SIZE) >> 1) *
                (dec_handle->frame_header.mi_stride >> 1);
    for (int idx = 0; idx < size; ++idx) {
        tpl_mvs_base[idx].mf_mv0.as_int = INVALID_MV;
        tpl_mvs_base[idx].ref_frame_offset = 0;
    }

    const int cur_order_hint = dec_handle->cur_pic_buf[0]->order_hint;

    const EbDecPicBuf *ref_buf[INTER_REFS_PER_FRAME];
    int ref_order_hint[INTER_REFS_PER_FRAME];

    for (int ref_frame = LAST_FRAME; ref_frame <= ALTREF_FRAME; ref_frame++) {
        const int ref_idx = ref_frame - LAST_FRAME;
        const EbDecPicBuf *const buf = get_ref_frame_buf(dec_handle, ref_frame);
        int order_hint = 0;

        if (buf != NULL) order_hint = buf->order_hint;

        ref_buf[ref_idx] = buf;
        ref_order_hint[ref_idx] = order_hint;

        if (get_relative_dist(order_hint_info, order_hint, cur_order_hint) > 0)
            dec_handle->master_frame_buf.ref_frame_side[ref_frame] = 1;
        else if (order_hint == cur_order_hint)
            dec_handle->master_frame_buf.ref_frame_side[ref_frame] = -1;
    }

    int ref_stamp = MFMV_STACK_SIZE - 1;

    return; /* TODO: Removed! */

    if (ref_buf[LAST_FRAME - LAST_FRAME] != NULL) {
        assert(0); //add ref_order_hints population
        const int alt_of_lst_order_hint =
            ref_buf[LAST_FRAME - LAST_FRAME]
            ->ref_order_hints[ALTREF_FRAME - LAST_FRAME];

        const int is_lst_overlay =
            (alt_of_lst_order_hint == ref_order_hint[GOLDEN_FRAME - LAST_FRAME]);
        if (!is_lst_overlay) motion_field_projection(dec_handle, LAST_FRAME, 2);
        --ref_stamp;
    }

    if (get_relative_dist(order_hint_info,
        ref_order_hint[BWDREF_FRAME - LAST_FRAME],
        cur_order_hint) > 0) {
        if (motion_field_projection(dec_handle, BWDREF_FRAME, 0)) --ref_stamp;
    }

    if (get_relative_dist(order_hint_info,
        ref_order_hint[ALTREF2_FRAME - LAST_FRAME],
        cur_order_hint) > 0) {
        if (motion_field_projection(dec_handle, ALTREF2_FRAME, 0)) --ref_stamp;
    }

    if (get_relative_dist(order_hint_info,
        ref_order_hint[ALTREF_FRAME - LAST_FRAME],
        cur_order_hint) > 0 &&
        ref_stamp >= 0)
        if (motion_field_projection(dec_handle, ALTREF_FRAME, 0)) --ref_stamp;

    if (ref_stamp >= 0) motion_field_projection(dec_handle, LAST2_FRAME, 2);
}

static int check_sb_border(const int mi_row, const int mi_col,
    const int row_offset, const int col_offset) {
    const int sb_mi_size = mi_size_wide[BLOCK_64X64];
    const int row = (mi_row & (sb_mi_size - 1)) + row_offset;
    const int col = (mi_col & (sb_mi_size - 1)) + col_offset;

    return (row >= 0 || row < sb_mi_size || col >= 0 || col < sb_mi_size);
}

static void add_extra_mv_candidate(ModeInfo_t * candidate, EbDecHandle *dec_handle,
    MvReferenceFrame *rf, uint8_t *num_mv_found, IntMv_dec ref_id[2][2],
    int ref_id_count[2], IntMv_dec ref_diff[2][2], int ref_diff_count[2])
{
    FrameHeader *frm_header = &dec_handle->frame_header;
    for (int rf_idx = 0; rf_idx < 2; ++rf_idx) {
        MvReferenceFrame can_rf = candidate->ref_frame[rf_idx];
        if (can_rf > INTRA_FRAME){
            for (int cmp_idx = 0; cmp_idx < 2; ++cmp_idx) {
                if (can_rf == rf[cmp_idx] && ref_id_count[cmp_idx] < 2) {
                    ref_id[cmp_idx][ref_id_count[cmp_idx]] = candidate->mv[rf_idx];
                    ++ref_id_count[cmp_idx];
                }
                else if (ref_diff_count[cmp_idx] < 2) {
                    IntMv_dec this_mv = candidate->mv[rf_idx];
                    if (frm_header->ref_frame_sign_bias[can_rf] !=
                        frm_header->ref_frame_sign_bias[rf[cmp_idx]])
                    {
                        this_mv.as_mv.row = -this_mv.as_mv.row;
                        this_mv.as_mv.col = -this_mv.as_mv.col;
                    }
                    ref_diff[cmp_idx][ref_diff_count[cmp_idx]] = this_mv;
                    ++ref_diff_count[cmp_idx];
                }
            }
        }
    }
}

static void process_single_ref_mv_candidate( ModeInfo_t * candidate,
    EbDecHandle *dec_handle, MvReferenceFrame ref_frame,
    uint8_t refmv_count[MODE_CTX_REF_FRAMES],
    CandidateMv_dec ref_mv_stack[][MAX_REF_MV_STACK_SIZE])
{
    FrameHeader *frm_header = &dec_handle->frame_header;
    for (int rf_idx = 0; rf_idx < 2; ++rf_idx) {
        if (candidate->ref_frame[rf_idx] > INTRA_FRAME) {
            IntMv_dec this_mv = candidate->mv[rf_idx];
            if (frm_header->ref_frame_sign_bias[candidate->ref_frame[rf_idx]] !=
                frm_header->ref_frame_sign_bias[ref_frame])
            {
                this_mv.as_mv.row = -this_mv.as_mv.row;
                this_mv.as_mv.col = -this_mv.as_mv.col;
            }
            int stack_idx;
            for (stack_idx = 0; stack_idx < refmv_count[ref_frame]; ++stack_idx) {
                const IntMv_dec stack_mv = ref_mv_stack[ref_frame][stack_idx].this_mv;
                if (this_mv.as_int == stack_mv.as_int) break;
            }

            if (stack_idx == refmv_count[ref_frame]) {
                ref_mv_stack[ref_frame][stack_idx].this_mv = this_mv;

                // TODO(jingning): Set an arbitrary small number here. The weight
                // doesn't matter as long as it is properly initialized.
                ref_mv_stack[ref_frame][stack_idx].weight = 2;
                ++refmv_count[ref_frame];
            }
        }
    }
}

static int16_t clamp_mv_row(EbDecHandle *dec_handle, PartitionInfo_t *pi,
    int mi_row, int row, int border)
{
    FrameHeader *frame_info = &dec_handle->frame_header;
    int bh4 = mi_size_high[pi->mi->sb_type];
    int mbToTopEdge = -((mi_row * MI_SIZE) * 8);
    int mbToBottomEdge = ((frame_info->mi_rows - bh4 - mi_row) * MI_SIZE) * 8;
    return CLIP3(mbToTopEdge - border, mbToBottomEdge + border, row);
}

static int16_t clamp_mv_col(EbDecHandle *dec_handle, PartitionInfo_t *pi,
    int mi_col, int col, int border)
{
    FrameHeader *frame_info = &dec_handle->frame_header;
    int bw4 = mi_size_wide[pi->mi->sb_type];
    int mbToLeftEdge = -((mi_col * MI_SIZE) * 8);
    int mbToRightEdge = ((frame_info->mi_cols - bw4 - mi_col) * MI_SIZE) * 8;
    return CLIP3(mbToLeftEdge - border, mbToRightEdge + border, col);
}

/* TODO: Harmonize with Encoder. */
static int has_top_right(EbDecHandle *dec_handle, PartitionInfo_t *pi,
    int mi_row, int mi_col, int bs)
{
    int n4_w = mi_size_wide[pi->mi->sb_type];
    int n4_h = mi_size_high[pi->mi->sb_type];
    const int sb_mi_size = mi_size_wide[dec_handle->seq_header.sb_size];
    const int mask_row = mi_row & (sb_mi_size - 1);
    const int mask_col = mi_col & (sb_mi_size - 1);

    if (bs > mi_size_wide[BLOCK_64X64]) return 0;
    int has_tr = !((mask_row & bs) && (mask_col & bs));

    assert(bs > 0 && !(bs & (bs - 1)));

    while (bs < sb_mi_size) {
        if (mask_col & bs) {
            if ((mask_col & (2 * bs)) && (mask_row & (2 * bs))) {
                has_tr = 0;
                break;
            }
        }
        else {
            break;
        }
        bs <<= 1;
    }

    if (n4_w < n4_h)
        if (!pi->is_sec_rect) has_tr = 1;
    if (n4_w > n4_h)
        if (pi->is_sec_rect) has_tr = 0;
    if (pi->mi->partition == PARTITION_VERT_A) {
        if (n4_w == n4_h)
            if (mask_row & bs) has_tr = 0;
    }
    return has_tr;
}

/* TODO: Harmonize with Encoder. Should move to new file. */
static void dec_setup_ref_mv_list(
    EbDecHandle *dec_handle, PartitionInfo_t *pi, MvReferenceFrame ref_frame,
    CandidateMv_dec ref_mv_stack[][MAX_REF_MV_STACK_SIZE],
    IntMv_dec mv_ref_list[][MAX_MV_REF_CANDIDATES], IntMv_dec *gm_mv_candidates,
    int mi_row, int mi_col, int16_t *mode_context,  MvCount *mv_cnt)
{
    int n4_w = mi_size_wide[pi->mi->sb_type];
    int n4_h = mi_size_high[pi->mi->sb_type];
    const int bs = AOMMAX(n4_w, n4_h);
    MvReferenceFrame rf[2];

    ParseCtxt *parse_ctx = (ParseCtxt*)dec_handle->pv_parse_ctxt;
    FrameHeader *frame_info = &dec_handle->frame_header;
    EbColorConfig *color_cfg = &dec_handle->seq_header.color_config;
    const TileInfo *const tile = &parse_ctx->cur_tile_info;
    int max_row_offset = 0, max_col_offset = 0;
    const int row_adj = (n4_h < mi_size_high[BLOCK_8X8]) && (mi_row & 0x01);
    const int col_adj = (n4_w < mi_size_wide[BLOCK_8X8]) && (mi_col & 0x01);
    int processed_rows = 0;
    int processed_cols = 0;

    av1_set_ref_frame(rf, ref_frame);
    mode_context[ref_frame] = 0;

    // Find valid maximum row/col offset.
    if (pi->up_available) {
        max_row_offset = -(MVREF_ROW_COLS << 1) + row_adj;

        if (n4_h < mi_size_high[BLOCK_8X8])
            max_row_offset = -(2 << 1) + row_adj;

        max_row_offset = clamp(max_row_offset, tile->mi_row_start - mi_row,
            tile->mi_row_end - mi_row - 1);
    }

    if (pi->left_available) {
        max_col_offset = -(MVREF_ROW_COLS << 1) + col_adj;

        if (n4_w < mi_size_wide[BLOCK_8X8])
            max_col_offset = -(2 << 1) + col_adj;

        max_col_offset = clamp(max_col_offset, tile->mi_col_start - mi_col,
            tile->mi_col_end - mi_col - 1);
    }
    //mv_cnt->newmv_count = 0;
    //mv_cnt->num_mv_found = 0;
    memset(mv_cnt, 0, sizeof(*mv_cnt));
    //mv_cnt->found_match = 0;
    //mv_cnt->found_above_match = 0;
    //mv_cnt->found_left_match = 0;

    // Scan the first above row mode info. row_offset = -1;
    if (abs(max_row_offset) >= 1) {
        scan_row_mbmi(dec_handle, pi, -1, mi_row, mi_col, rf, ref_mv_stack[ref_frame],
            &mv_cnt->num_mv_found[ref_frame], &mv_cnt->found_above_match,
            &mv_cnt->newmv_count, gm_mv_candidates, max_row_offset, &processed_rows);
    }

    // Scan the first left column mode info. col_offset = -1;
    if (abs(max_col_offset) >= 1) {
        scan_col_mbmi(dec_handle, pi, -1, mi_row, mi_col, rf, ref_mv_stack[ref_frame],
            &mv_cnt->num_mv_found[ref_frame], &mv_cnt->found_left_match,
            &mv_cnt->newmv_count, gm_mv_candidates, max_col_offset, &processed_cols);
    }

    int32_t sub_x = color_cfg->subsampling_x;
    int32_t sub_y = color_cfg->subsampling_y;
    const int right_available =
        mi_col + (max_col_offset << sub_x) < tile->mi_col_end;

    if (has_top_right(dec_handle, pi, mi_row, mi_col, bs)) {
        scan_blk_mbmi(dec_handle, pi, -1, n4_w, mi_row, mi_col, rf,
            ref_mv_stack[ref_frame], &mv_cnt->found_above_match, &mv_cnt->newmv_count,
            gm_mv_candidates, &mv_cnt->num_mv_found[ref_frame]);
    }

    const uint8_t nearest_match = (mv_cnt->found_above_match > 0) +
                                  (mv_cnt->found_left_match  > 0);
    const uint8_t num_nearest = mv_cnt->num_mv_found[ref_frame];
    const uint8_t num_new = mv_cnt->newmv_count;

    for (int idx = 0; idx < num_nearest; ++idx)
        ref_mv_stack[ref_frame][idx].weight += REF_CAT_LEVEL;

    if (frame_info->use_ref_frame_mvs) {
        int is_available = 0;
        const int voffset = AOMMAX(mi_size_high[BLOCK_8X8], n4_h);
        const int hoffset = AOMMAX(mi_size_wide[BLOCK_8X8], n4_w);
        const int blk_row_end = AOMMIN(n4_h, mi_size_high[BLOCK_64X64]);
        const int blk_col_end = AOMMIN(n4_w, mi_size_wide[BLOCK_64X64]);

        const int tpl_sample_pos[3][2] = {
          { voffset, -2 },
          { voffset, hoffset },
          { voffset - 2, hoffset },
        };
        const int allow_extension = (n4_h >= mi_size_high[BLOCK_8X8]) &&
            (n4_h < mi_size_high[BLOCK_64X64]) &&
            (n4_w >= mi_size_wide[BLOCK_8X8]) &&
            (n4_w < mi_size_wide[BLOCK_64X64]);

        const int step_h = (n4_h >= mi_size_high[BLOCK_64X64])
            ? mi_size_high[BLOCK_16X16]
            : mi_size_high[BLOCK_8X8];
        const int step_w = (n4_w >= mi_size_wide[BLOCK_64X64])
            ? mi_size_wide[BLOCK_16X16]
            : mi_size_wide[BLOCK_8X8];

        for (int blk_row = 0; blk_row < blk_row_end; blk_row += step_h) {
            for (int blk_col = 0; blk_col < blk_col_end; blk_col += step_w) {

                int ret = add_tpl_ref_mv(dec_handle, mi_row, mi_col,
                    ref_frame, blk_row, blk_col, gm_mv_candidates,
                    mv_cnt->num_mv_found, ref_mv_stack, mode_context);
                if (blk_row == 0 && blk_col == 0) is_available = ret;
            }
        }

        if (is_available == 0) mode_context[ref_frame] |= (1 << GLOBALMV_OFFSET);

        if (allow_extension) {
            for (int i = 0; i < 3; ++i) {
                const int blk_row = tpl_sample_pos[i][0];
                const int blk_col = tpl_sample_pos[i][1];

                if (check_sb_border(mi_row, mi_col, blk_row, blk_col)) {
                    add_tpl_ref_mv(dec_handle, mi_row, mi_col, ref_frame, blk_row,
                        blk_col, gm_mv_candidates, mv_cnt->num_mv_found,
                        ref_mv_stack, mode_context);
                }
            }
        }
    }

    // Scan the second outer area.
    scan_blk_mbmi(dec_handle, pi, -1, -1, mi_row, mi_col, rf,
        ref_mv_stack[ref_frame], &mv_cnt->found_above_match, &mv_cnt->newmv_count,
        gm_mv_candidates, &mv_cnt->num_mv_found[ref_frame]);

    for (int idx = 2; idx <= MVREF_ROW_COLS; ++idx) {
        const int row_offset = -(idx << 1) + 1 + row_adj;
        const int col_offset = -(idx << 1) + 1 + col_adj;
        if (abs(row_offset) <= abs(max_row_offset) && abs(row_offset) > processed_rows) {
            scan_row_mbmi(dec_handle, pi, row_offset, mi_row, mi_col, rf,
                ref_mv_stack[ref_frame], &mv_cnt->num_mv_found[ref_frame],
                &mv_cnt->found_above_match, &mv_cnt->newmv_count,
                gm_mv_candidates, max_row_offset, &processed_rows);
        }

        if (abs(col_offset) <= abs(max_col_offset) && abs(col_offset) > processed_cols) {
            scan_col_mbmi(dec_handle, pi, col_offset, mi_row, mi_col, rf,
                ref_mv_stack[ref_frame], &mv_cnt->num_mv_found[ref_frame],
                &mv_cnt->found_left_match, &mv_cnt->newmv_count,
                gm_mv_candidates, max_col_offset, &processed_cols);
        }
    }

    /* sorting process*/
    int start = 0;
    int end = num_nearest;
    while (end > start) {
        int new_end = start;
        for (int idx = start + 1; idx < end; ++idx) {
            if (ref_mv_stack[ref_frame][idx - 1].weight <
                ref_mv_stack[ref_frame][idx].weight) {
                CandidateMv_dec tmp_mv = ref_mv_stack[ref_frame][idx - 1];
                ref_mv_stack[ref_frame][idx - 1] = ref_mv_stack[ref_frame][idx];
                ref_mv_stack[ref_frame][idx] = tmp_mv;
                new_end = idx;
            }
        }
        end = new_end;
    }

    start = num_nearest;
    end = mv_cnt->num_mv_found[ref_frame];
    while (end > start) {
        int new_end = start;
        for (int idx = start + 1; idx < end; ++idx) {
            if (ref_mv_stack[ref_frame][idx - 1].weight <
                ref_mv_stack[ref_frame][idx].weight) {
                CandidateMv_dec tmp_mv = ref_mv_stack[ref_frame][idx - 1];
                ref_mv_stack[ref_frame][idx - 1] = ref_mv_stack[ref_frame][idx];
                ref_mv_stack[ref_frame][idx] = tmp_mv;
                new_end = idx;
            }
        }
        end = new_end;
    }

    /* extra search process */
    if (mv_cnt->num_mv_found[ref_frame] < MAX_MV_REF_CANDIDATES) {
        IntMv_dec ref_id[2][2], ref_diff[2][2];
        int ref_id_count[2] = { 0 }, ref_diff_count[2] = { 0 };

            int mi_width = AOMMIN(16, n4_w);
        mi_width = AOMMIN(mi_width, frame_info->mi_cols - mi_col);
            int mi_height = AOMMIN(16, n4_h);
        mi_height = AOMMIN(mi_height, frame_info->mi_rows - mi_row);
        int mi_size = AOMMIN(mi_width, mi_height);

        for (int pass = 0; pass < 2; pass++) {
            int idx = 0;
            while (idx < mi_size &&
                mv_cnt->num_mv_found[ref_frame] < MAX_MV_REF_CANDIDATES)
            {
                int mv_row, mv_col;
                if (pass == 0) { mv_row = mi_row - 1; mv_col = mi_col + idx; }
                else { mv_row = mi_row + idx; mv_col = mi_col - 1; }

                if (!is_inside(&parse_ctx->cur_tile_info, mv_col, mv_row)) break;

                ModeInfo_t *nbr = get_cur_mode_info(dec_handle,
                    mv_row, mv_col, pi->sb_info);

                if (rf[1] != NONE_FRAME)
                    add_extra_mv_candidate(nbr, dec_handle, rf,
                        &mv_cnt->num_mv_found[ref_frame], ref_id,
                        ref_id_count, ref_diff, ref_diff_count);
                else
                    process_single_ref_mv_candidate(nbr, dec_handle, ref_frame,
                        mv_cnt->num_mv_found, ref_mv_stack);

                idx += pass ? mi_size_high[nbr->sb_type] : mi_size_wide[nbr->sb_type];
            }
        }

        if (rf[1] > NONE_FRAME) {
            IntMv_dec comp_list[3][2];

            for (int idx = 0; idx < 2; ++idx) {
                int comp_idx = 0;
                for (int list_idx = 0; list_idx < ref_id_count[idx];
                    ++list_idx, comp_idx++) {
                    comp_list[comp_idx][idx] = ref_id[idx][list_idx];
                }

                for (int list_idx = 0; list_idx < ref_diff_count[idx]
                    && comp_idx < 2; ++list_idx, ++comp_idx)
                {
                    comp_list[comp_idx][idx] = ref_diff[idx][list_idx];
                }

                for (; comp_idx < 2; ++comp_idx)
                    comp_list[comp_idx][idx] = gm_mv_candidates[idx];
            }

            if (mv_cnt->num_mv_found[ref_frame]) {
                assert(mv_cnt->num_mv_found[ref_frame] == 1);
                if (comp_list[0][0].as_int ==
                    ref_mv_stack[ref_frame][0].this_mv.as_int &&
                    comp_list[0][1].as_int ==
                    ref_mv_stack[ref_frame][0].comp_mv.as_int)
                {
                    ref_mv_stack[ref_frame][mv_cnt->num_mv_found[ref_frame]].this_mv =
                            comp_list[1][0];
                    ref_mv_stack[ref_frame][mv_cnt->num_mv_found[ref_frame]].comp_mv =
                            comp_list[1][1];
                }
                else {
                    ref_mv_stack[ref_frame][mv_cnt->num_mv_found[ref_frame]].this_mv =
                            comp_list[0][0];
                    ref_mv_stack[ref_frame][mv_cnt->num_mv_found[ref_frame]].comp_mv =
                            comp_list[0][1];
                }
                ref_mv_stack[ref_frame][mv_cnt->num_mv_found[ref_frame]].weight = 2;
                ++mv_cnt->num_mv_found[ref_frame];
            }
            else {
                for (int idx = 0; idx < MAX_MV_REF_CANDIDATES; ++idx) {
                    ref_mv_stack[ref_frame][mv_cnt->num_mv_found[ref_frame]]
                        .this_mv = comp_list[idx][0];
                    ref_mv_stack[ref_frame][mv_cnt->num_mv_found[ref_frame]]
                        .comp_mv = comp_list[idx][1];
                    ref_mv_stack[ref_frame][mv_cnt->num_mv_found[ref_frame]]
                        .weight = 2;
                    ++mv_cnt->num_mv_found[ref_frame];
                }
            }
        }
        else
        {
            for (int idx = mv_cnt->num_mv_found[ref_frame]; idx < 2; idx++) {
                ref_mv_stack[ref_frame][idx].this_mv = gm_mv_candidates[0];
            }
        }
    }

    /* context and clamping process */
    int num_lists = rf[1] > NONE_FRAME ? 2 : 1;
    for (int list = 0; list < num_lists; list++) {
        for (int idx = 0; idx < mv_cnt->num_mv_found[ref_frame]; idx++) {
            IntMv_dec refMv = ref_mv_stack[list][idx].this_mv;
            refMv.as_mv.row = clamp_mv_row(dec_handle, pi, mi_row,
                refMv.as_mv.row, MV_BORDER + n4_h * 8);
            refMv.as_mv.col = clamp_mv_col(dec_handle, pi, mi_col,
                refMv.as_mv.col, MV_BORDER + n4_w * 8);
            ref_mv_stack[list][idx].this_mv = refMv;
        }
    }

    const uint8_t ref_match_count = (mv_cnt->found_above_match > 0) +
                                    (mv_cnt->found_left_match  > 0);
    switch (nearest_match) {
    case 0:
        mode_context[ref_frame] |= 0;
        if (ref_match_count >= 1) mode_context[ref_frame] |= 1;
        if (ref_match_count == 1)
        mode_context[ref_frame] |= (1 << REFMV_OFFSET);
        else if (ref_match_count >= 2)
        mode_context[ref_frame] |= (2 << REFMV_OFFSET);
        break;
    case 1:
        mode_context[ref_frame] |= (num_new > 0) ? 2 : 3;
        if (ref_match_count == 1)
        mode_context[ref_frame] |= (3 << REFMV_OFFSET);
        else if (ref_match_count >= 2)
        mode_context[ref_frame] |= (4 << REFMV_OFFSET);
        break;
    case 2:
    default:
        if (num_new >= 1)
        mode_context[ref_frame] |= 4;
        else
        mode_context[ref_frame] |= 5;

        mode_context[ref_frame] |= (5 << REFMV_OFFSET);
        break;
    }

    if (rf[1] == NONE_FRAME && mv_ref_list != NULL) {
      for (int idx = mv_cnt->num_mv_found[ref_frame];
          idx < MAX_MV_REF_CANDIDATES; ++idx)
        mv_ref_list[rf[0]][idx].as_int = gm_mv_candidates[0].as_int;

        for (int idx = 0; idx < AOMMIN(MAX_MV_REF_CANDIDATES,
            mv_cnt->num_mv_found[ref_frame]); ++idx)
        {
            mv_ref_list[rf[0]][idx].as_int =
                ref_mv_stack[ref_frame][idx].this_mv.as_int;
        }
    }
}

static uint16_t compound_mode_ctx_map[3][COMP_NEWMV_CTXS] = {
    { 0, 1, 1, 1, 1 },
    { 1, 2, 3, 4, 4 },
    { 4, 4, 5, 6, 7 },
};

static INLINE int16_t svt_mode_context_analyzer(
    const int16_t *const mode_context, const MvReferenceFrame *const rf)
{
    const int8_t ref_frame = av1_ref_frame_type(rf);

    if (rf[1] <= INTRA_FRAME) return mode_context[ref_frame];

    const int16_t newmv_ctx = mode_context[ref_frame] & NEWMV_CTX_MASK;
    const int16_t refmv_ctx =
        (mode_context[ref_frame] >> REFMV_OFFSET) & REFMV_CTX_MASK;

    const int16_t comp_ctx = compound_mode_ctx_map[refmv_ctx >> 1][AOMMIN(
        newmv_ctx, COMP_NEWMV_CTXS - 1)];
    return comp_ctx;
}

void read_interintra_mode(EbDecHandle *dec_handle,
    ModeInfo_t *mbmi, SvtReader *r)
{
    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    FRAME_CONTEXT *frm_ctx = &parse_ctxt->cur_tile_ctx;
    BlockSize bsize = mbmi->sb_type;
    if (dec_handle->seq_header.enable_interintra_compound
        && !mbmi->skip_mode && is_interintra_allowed(mbmi))
    {
        const int bsize_group = size_group_lookup[bsize];
        mbmi->is_inter_intra =
            svt_read_symbol(r, frm_ctx->interintra_cdf[bsize_group], 2, ACCT_STR);
        assert(mbmi->ref_frame[1] == NONE_FRAME);
        if (mbmi->is_inter_intra) {
            mbmi->interintra_mode.interintra_mode = (InterIntraMode)svt_read_symbol(
                r, frm_ctx->interintra_mode_cdf[bsize_group], INTERINTRA_MODES,
                ACCT_STR);
            mbmi->ref_frame[1] = INTRA_FRAME;
            mbmi->angle_delta[PLANE_TYPE_Y] = 0;
            mbmi->angle_delta[PLANE_TYPE_UV] = 0;
            mbmi->filter_intra_mode_info.use_filter_intra = 0;
            if (is_interintra_wedge_used(bsize)) {
                mbmi->interintra_mode.wedge_interintra = svt_read_symbol(
                    r, frm_ctx->wedge_interintra_cdf[bsize], 2, ACCT_STR);
                if (mbmi->interintra_mode.wedge_interintra) {
                    mbmi->interintra_mode.interintra_wedge_index =
                        svt_read_symbol(r, frm_ctx->wedge_idx_cdf[bsize], 16, ACCT_STR);
                }
            }
        }
    }
}

int has_overlappable_cand(EbDecHandle *dec_handle, PartitionInfo_t *pi,
    int mi_row, int mi_col)
{
    ParseCtxt *parse_ctx = (ParseCtxt*)dec_handle->pv_parse_ctxt;
    const TileInfo *const tile = &parse_ctx->cur_tile_info;
    ModeInfo_t *mbmi = pi->mi;
    if (!is_motion_variation_allowed_bsize(mbmi->sb_type)) return 0;

    if (pi->up_available) {
        int w4 = mi_size_wide[mbmi->sb_type];
        int x4 = mi_col;
            while (x4 < AOMMIN(tile->mi_col_end, mi_col + w4)) {
                ModeInfo_t *top_nb_mode = get_cur_mode_info(dec_handle,
                    mi_row - 1, x4 | 1, pi->sb_info);
                x4 += AOMMAX(2, mi_size_wide[top_nb_mode->sb_type] >> 2);
                if(dec_is_inter_block(top_nb_mode))
                    return 1;
            }
    }
    if (pi->left_available) {
        int h4 = mi_size_high[mbmi->sb_type];
        int y4 = mi_row;
            while (y4 < AOMMIN(tile->mi_row_end, mi_row + h4)) {
                ModeInfo_t *left_nb_mode = get_cur_mode_info(dec_handle,
                    y4 | 1, mi_col - 1, pi->sb_info);
                y4 += AOMMAX(2, mi_size_high[left_nb_mode->sb_type] >> 2);
                if (dec_is_inter_block(left_nb_mode))
                    return 1;
            }
    }
    return 0;
}

static INLINE void add_samples(ModeInfo_t *mbmi, int *pts, int *pts_inref,
    int row_offset, int sign_r, int col_offset, int sign_c)
{
    int bw = block_size_wide[mbmi->sb_type];
    int bh = block_size_high[mbmi->sb_type];
    int x = col_offset * MI_SIZE + sign_c * AOMMAX(bw, MI_SIZE) / 2 - 1;
    int y = row_offset * MI_SIZE + sign_r * AOMMAX(bh, MI_SIZE) / 2 - 1;

    pts[0] = (x * 8);
    pts[1] = (y * 8);
    pts_inref[0] = (x * 8) + mbmi->mv[0].as_mv.col;
    pts_inref[1] = (y * 8) + mbmi->mv[0].as_mv.row;
}

int find_warp_samples(EbDecHandle *dec_handle, PartitionInfo_t *pi,
    int mi_row, int mi_col, int *pts, int *pts_inref)
{
    ModeInfo_t *const mbmi0 = pi->mi;
    int ref_frame = mbmi0->ref_frame[0];
    int up_available = pi->up_available;
    int left_available = pi->left_available;
    int i, mi_step = 1, np = 0;

    ParseCtxt *parse_ctx = (ParseCtxt*)dec_handle->pv_parse_ctxt;
    TileInfo *tile = &parse_ctx->cur_tile_info;
    int do_tl = 1;
    int do_tr = 1;
    int b4_w = mi_size_wide[pi->mi->sb_type];
    int b4_h = mi_size_high[pi->mi->sb_type];

    // scan the nearest above rows
    if (up_available) {
        int mi_row_offset = -1;
        ModeInfo_t *mbmi = get_cur_mode_info(dec_handle,
            mi_row - 1, mi_col, pi->sb_info);
        uint8_t n4_w = mi_size_wide[mbmi->sb_type];

        if (b4_w <= n4_w) {
            // Handle "current block width <= above block width" case.
            int col_offset = -mi_col % n4_w;

            if (col_offset < 0) do_tl = 0;
            if (col_offset + n4_w > b4_w) do_tr = 0;

            if (mbmi->ref_frame[0] == ref_frame && mbmi->ref_frame[1] == NONE_FRAME) {
                add_samples(mbmi, pts, pts_inref, 0, -1, col_offset, 1);
                pts += 2;
                pts_inref += 2;
                np++;
                if (np >= LEAST_SQUARES_SAMPLES_MAX) return LEAST_SQUARES_SAMPLES_MAX;
            }
        }
        else {
            // Handle "current block width > above block width" case.
            for (i = 0; i < AOMMIN(b4_w, tile->mi_col_end - mi_col); i += mi_step) {
                int mi_col_offset = i;
                mbmi = get_cur_mode_info(dec_handle,
                    mi_row - 1, mi_col + i, pi->sb_info);
                n4_w = mi_size_wide[mbmi->sb_type];
                mi_step = AOMMIN(b4_w, n4_w);

                if (mbmi->ref_frame[0] == ref_frame &&
                    mbmi->ref_frame[1] == NONE_FRAME) {
                    add_samples(mbmi, pts, pts_inref, 0, -1, i, 1);
                    pts += 2;
                    pts_inref += 2;
                    np++;
                    if (np >= LEAST_SQUARES_SAMPLES_MAX) return LEAST_SQUARES_SAMPLES_MAX;
                }
            }
        }
    }
    assert(np <= LEAST_SQUARES_SAMPLES_MAX);

    // scan the nearest left columns
    if (left_available) {

        ModeInfo_t *mbmi = get_cur_mode_info(dec_handle,
            mi_row, mi_col - 1, pi->sb_info);
        uint8_t n4_h = mi_size_high[mbmi->sb_type];

        if (b4_h <= n4_h) {
            // Handle "current block height <= above block height" case.
            int row_offset = -mi_row % n4_h;

            if (row_offset < 0) do_tl = 0;

            if (mbmi->ref_frame[0] == ref_frame && mbmi->ref_frame[1] == NONE_FRAME) {
                add_samples(mbmi, pts, pts_inref, row_offset, 1, 0, -1);
                pts += 2;
                pts_inref += 2;
                np++;
                if (np >= LEAST_SQUARES_SAMPLES_MAX) return LEAST_SQUARES_SAMPLES_MAX;
            }
        }
        else {
            // Handle "current block height > above block height" case.
            for (i = 0; i < AOMMIN(b4_h, tile->mi_row_end - mi_row); i += mi_step) {
                int mi_row_offset = i;
                mbmi = get_cur_mode_info(dec_handle,
                    mi_row + i, mi_col - 1, pi->sb_info);
                n4_h = mi_size_high[mbmi->sb_type];
                mi_step = AOMMIN(b4_h, n4_h);

                if (mbmi->ref_frame[0] == ref_frame &&
                    mbmi->ref_frame[1] == NONE_FRAME) {
                    add_samples(mbmi, pts, pts_inref, i, 1, 0, -1);
                    pts += 2;
                    pts_inref += 2;
                    np++;
                    if (np >= LEAST_SQUARES_SAMPLES_MAX) return LEAST_SQUARES_SAMPLES_MAX;
                }
            }
        }
    }
    assert(np <= LEAST_SQUARES_SAMPLES_MAX);

    // Top-left block
    if (do_tl && left_available && up_available) {

        ModeInfo_t *mbmi = get_cur_mode_info(dec_handle,
            mi_row - 1, mi_col - 1, pi->sb_info);

        if (mbmi->ref_frame[0] == ref_frame && mbmi->ref_frame[1] == NONE_FRAME) {
            add_samples(mbmi, pts, pts_inref, 0, -1, 0, -1);
            pts += 2;
            pts_inref += 2;
            np++;
            if (np >= LEAST_SQUARES_SAMPLES_MAX) return LEAST_SQUARES_SAMPLES_MAX;
        }
    }
    assert(np <= LEAST_SQUARES_SAMPLES_MAX);

    // Top-right block
    if (do_tr &&
        has_top_right(dec_handle, pi, mi_row, mi_col, AOMMAX(b4_w, b4_h))) {

        int mv_row = mi_row - 1;
        int mv_col = mi_col + b4_w;

        if (is_inside(tile, mv_col, mv_row)) {
            ModeInfo_t *mbmi =
                get_cur_mode_info(dec_handle,
                    mv_row, mv_col, pi->sb_info);

            if (mbmi->ref_frame[0] == ref_frame && mbmi->ref_frame[1] == NONE_FRAME) {
                add_samples(mbmi, pts, pts_inref, 0, -1, b4_h, 1);
                np++;
                if (np >= LEAST_SQUARES_SAMPLES_MAX) return LEAST_SQUARES_SAMPLES_MAX;
            }
        }
    }
    assert(np <= LEAST_SQUARES_SAMPLES_MAX);

    return np;
}


static INLINE MotionMode is_motion_mode_allowed(EbDecHandle *dec_handle, GlobalMotionParams *gm_params, PartitionInfo_t *pi, int mi_row,
    int mi_col, int allow_warped_motion)
{
    ModeInfo_t *mbmi = pi->mi;
    if (dec_handle->frame_header.force_integer_mv == 0) {
        const TransformationType gm_type = gm_params[mbmi->ref_frame[0]].gm_type;
        if (is_global_mv_block(mbmi, gm_type)) return SIMPLE_TRANSLATION;
    }
    if ((block_size_wide[mbmi->sb_type] >= 8 && block_size_high[mbmi->sb_type] >= 8) &&
        (mbmi->mode >= NEARESTMV && mbmi->mode < MB_MODE_COUNT)
        && mbmi->ref_frame[1] != INTRA_FRAME && !has_second_ref(mbmi)) {
        if (!has_overlappable_cand(dec_handle, pi, mi_row, mi_col))
            return SIMPLE_TRANSLATION;
        assert(!has_second_ref(mbmi));

        if (pi->num_samples >= 1 &&
            (allow_warped_motion /*&&
                !av1_is_scaled(xd->block_ref_scale_factors[0])*/)) {
            if (dec_handle->frame_header.force_integer_mv) {
                return OBMC_CAUSAL;
            }
            return WARPED_CAUSAL;
        }
        return OBMC_CAUSAL;
    }
    else {
        return SIMPLE_TRANSLATION;
    }
}

MotionMode read_motion_mode(EbDecHandle *dec_handle,
    PartitionInfo_t *pi, int mi_row, int mi_col, SvtReader *r)
{
    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    FRAME_CONTEXT *frm_ctx = &parse_ctxt->cur_tile_ctx;
    FrameHeader *frame_info = &dec_handle->frame_header;
    int allow_warped_motion = frame_info->allow_high_precision_mv;
    ModeInfo_t *mbmi = pi->mi;

    if (dec_handle->frame_header.is_motion_mode_switchable == 0) return SIMPLE_TRANSLATION;
    if (mbmi->skip_mode) return SIMPLE_TRANSLATION;

    const MotionMode last_motion_mode_allowed =
        is_motion_mode_allowed(dec_handle,
            dec_handle->cur_pic_buf[0]->global_motion, pi,
            mi_row, mi_col, allow_warped_motion);
    int motion_mode;

    if (last_motion_mode_allowed == SIMPLE_TRANSLATION) return SIMPLE_TRANSLATION;

    if (last_motion_mode_allowed == OBMC_CAUSAL) {
        motion_mode =
            svt_read_symbol(r, frm_ctx->obmc_cdf[mbmi->sb_type], 2, ACCT_STR);
        return (MotionMode)(motion_mode);
    }
    else {
        motion_mode =
            svt_read_symbol(r, frm_ctx->motion_mode_cdf[mbmi->sb_type],
                MOTION_MODES, ACCT_STR);
        return (MotionMode)(motion_mode);
    }
}

int get_comp_index_context(EbDecHandle *dec_handle, PartitionInfo_t *pi) {
    ModeInfo_t *mbmi = pi->mi;
    SeqHeader *seq_params = &dec_handle->seq_header;
    FrameHeader *frm_header = &dec_handle->frame_header;

    int bck_frame_index = 0, fwd_frame_index = 0;
    int cur_frame_index = frm_header->order_hint;

    EbDecPicBuf *bck_buf = get_ref_frame_buf(dec_handle, mbmi->ref_frame[0]);
    EbDecPicBuf *fwd_buf = get_ref_frame_buf(dec_handle, mbmi->ref_frame[1]);

    if (bck_buf != NULL) bck_frame_index = bck_buf->order_hint;
    if (fwd_buf != NULL) fwd_frame_index = fwd_buf->order_hint;

    int fwd = abs(get_relative_dist(&seq_params->order_hint_info,
        fwd_frame_index, cur_frame_index));
    int bck = abs(get_relative_dist(&seq_params->order_hint_info,
        cur_frame_index, bck_frame_index));

    const ModeInfo_t *const above_mi = pi->above_mbmi;
    const ModeInfo_t *const left_mi = pi->left_mbmi;

    int above_ctx = 0, left_ctx = 0;
    const int offset = (fwd == bck);

    if (above_mi != NULL) {
        if (has_second_ref(above_mi))
            above_ctx = above_mi->compound_mode;
        else if (above_mi->ref_frame[0] == ALTREF_FRAME)
            above_ctx = 1;
    }

    if (left_mi != NULL) {
        if (has_second_ref(left_mi))
            left_ctx = left_mi->compound_mode;
        else if (left_mi->ref_frame[0] == ALTREF_FRAME)
            left_ctx = 1;
    }

    return above_ctx + left_ctx + 3 * offset;
}

void read_compound_type(EbDecHandle *dec_handle, PartitionInfo_t *pi,
    int mi_row, int mi_col, SvtReader *r)
{
    ModeInfo_t *mbmi = pi->mi;
    BlockSize bsize = mbmi->sb_type;
    mbmi->inter_compound.comp_group_idx = 0;
    mbmi->inter_compound.compound_idx = 1;
    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    FRAME_CONTEXT *frm_ctx = &parse_ctxt->cur_tile_ctx;

    if (mbmi->skip_mode) mbmi->inter_compound.type = COMPOUND_AVERAGE;

    if (has_second_ref(mbmi)) {
        // Read idx to indicate current compound inter prediction mode group
        const int masked_compound_used = is_any_masked_compound_used(bsize) &&
            dec_handle->seq_header.enable_masked_compound;

        if (masked_compound_used) {
            const int ctx_comp_group_idx = get_comp_group_idx_context(pi);
            mbmi->inter_compound.comp_group_idx = svt_read_symbol(
                r, frm_ctx->comp_group_idx_cdf[ctx_comp_group_idx], 2, ACCT_STR);
        }

        if (mbmi->inter_compound.comp_group_idx == 0) {
            if (dec_handle->seq_header.order_hint_info.enable_jnt_comp) {
                const int comp_index_ctx = get_comp_index_context(dec_handle, pi);
                mbmi->inter_compound.compound_idx = svt_read_symbol(
                    r, frm_ctx->compound_index_cdf[comp_index_ctx], 2, ACCT_STR);
                mbmi->inter_compound.type =
                    mbmi->inter_compound.compound_idx ? COMPOUND_AVERAGE : COMPOUND_DISTWTD;
            }
            else {
                // Distance-weighted compound is disabled, so always use average
                mbmi->inter_compound.compound_idx = 1;
                mbmi->inter_compound.type = COMPOUND_AVERAGE;
            }
        }
        else {
            assert(dec_handle->frame_header.reference_mode != SINGLE_REFERENCE &&
                is_inter_compound_mode(mbmi->mode) &&
                mbmi->motion_mode == SIMPLE_TRANSLATION);
            assert(masked_compound_used);

            // compound_diffwtd, wedge
            if (is_interinter_compound_used(COMPOUND_WEDGE, bsize))
                mbmi->inter_compound.type = svt_read_symbol(r,
                    frm_ctx->compound_type_cdf[bsize],
                    MASKED_COMPOUND_TYPES, ACCT_STR);
            else
                mbmi->inter_compound.type = COMPOUND_DIFFWTD;

            if (mbmi->inter_compound.type == COMPOUND_WEDGE) {
                assert(is_interinter_compound_used(COMPOUND_WEDGE, bsize));
                mbmi->inter_compound.wedge_index =
                    svt_read_symbol(r, frm_ctx->wedge_idx_cdf[bsize], 16, ACCT_STR);
                mbmi->inter_compound.wedge_sign = svt_read_bit(r, ACCT_STR);
            }
            else {
                assert(mbmi->inter_compound.type == COMPOUND_DIFFWTD);
                mbmi->inter_compound.mask_type =
                    svt_read_literal(r, MAX_DIFFWTD_MASK_BITS, ACCT_STR);
            }
        }
    }
    else {
        if (mbmi->is_inter_intra)
            mbmi->inter_compound.type = mbmi->interintra_mode.wedge_interintra ?
            COMPOUND_WEDGE : COMPOUND_INTRA;
        else
            mbmi->inter_compound.type = COMPOUND_AVERAGE;
    }
}

static InterpFilter get_ref_filter_type(const ModeInfo_t *ref_mbmi,
    int dir, MvReferenceFrame ref_frame)
{
    return ((ref_mbmi->ref_frame[0] == ref_frame ||
        ref_mbmi->ref_frame[1] == ref_frame)
        ? av1_extract_interp_filter(ref_mbmi->interp_filters, dir & 0x01)
        : SWITCHABLE_FILTERS);
}


int get_context_interp(PartitionInfo_t *pi, int dir) {
    const ModeInfo_t *const mbmi = pi->mi;
    const int ctx_offset =
        (mbmi->ref_frame[1] > INTRA_FRAME) * INTER_FILTER_COMP_OFFSET;
    assert(dir == 0 || dir == 1);
    const MvReferenceFrame ref_frame = mbmi->ref_frame[0];

    int filter_type_ctx = ctx_offset + (dir & 0x01) * INTER_FILTER_DIR_OFFSET;
    int left_type = SWITCHABLE_FILTERS;
    int above_type = SWITCHABLE_FILTERS;

    if (pi->left_available)
        left_type = get_ref_filter_type(pi->left_mbmi, dir, ref_frame);

    if (pi->up_available)
        above_type =
        get_ref_filter_type(pi->above_mbmi, dir, ref_frame);

    if (left_type == above_type) {
        filter_type_ctx += left_type;
    }
    else if (left_type == SWITCHABLE_FILTERS) {
        assert(above_type != SWITCHABLE_FILTERS);
        filter_type_ctx += above_type;
    }
    else if (above_type == SWITCHABLE_FILTERS) {
        assert(left_type != SWITCHABLE_FILTERS);
        filter_type_ctx += left_type;
    }
    else {
        filter_type_ctx += SWITCHABLE_FILTERS;
    }

    return filter_type_ctx;
}

static INLINE int is_nontrans_global_motion(PartitionInfo_t *pi,
    GlobalMotionParams *gm_params)
{
    int ref;
    ModeInfo_t * mbmi = pi->mi;
    // First check if all modes are GLOBALMV
    if (mbmi->mode != GLOBALMV && mbmi->mode != GLOBAL_GLOBALMV) return 0;

    if (AOMMIN(mi_size_wide[mbmi->sb_type], mi_size_high[mbmi->sb_type]) < 2)
        return 0;

    // Now check if all global motion is non translational
    for (ref = 0; ref < 1 + has_second_ref(mbmi); ++ref) {
        if (gm_params[mbmi->ref_frame[ref]].gm_type == TRANSLATION) return 0;
    }
    return 1;
}

static INLINE int av1_is_interp_needed(PartitionInfo_t *pi,
    GlobalMotionParams *gm_params)
{
    ModeInfo_t * mbmi = pi->mi;
    if (mbmi->skip_mode) return 0;
    if (mbmi->motion_mode == WARPED_CAUSAL) return 0;
    if (is_nontrans_global_motion(pi, gm_params)) return 0;
    return 1;
}

static INLINE InterpFilters make_interp_filters(InterpFilter y_filter,
    InterpFilter x_filter) {
    uint16_t y16 = y_filter & 0xffff;
    uint16_t x16 = x_filter & 0xffff;
    return y16 | ((uint32_t)x16 << 16);
}

static INLINE InterpFilters broadcast_interp_filter(InterpFilter filter) {
    return make_interp_filters(filter, filter);
}


static INLINE InterpFilter unswitchable_filter(InterpFilter filter) {
    return filter == SWITCHABLE ? EIGHTTAP_REGULAR : filter;
}

static INLINE void set_default_interp_filters(
    ModeInfo_t *mbmi, InterpFilter frame_interp_filter) {
    mbmi->interp_filters =
        broadcast_interp_filter(unswitchable_filter(frame_interp_filter));
}

static INLINE PredictionMode compound_ref0_mode(PredictionMode mode) {
    static PredictionMode lut[] = {
      MB_MODE_COUNT,  // DC_PRED
      MB_MODE_COUNT,  // V_PRED
      MB_MODE_COUNT,  // H_PRED
      MB_MODE_COUNT,  // D45_PRED
      MB_MODE_COUNT,  // D135_PRED
      MB_MODE_COUNT,  // D113_PRED
      MB_MODE_COUNT,  // D157_PRED
      MB_MODE_COUNT,  // D203_PRED
      MB_MODE_COUNT,  // D67_PRED
      MB_MODE_COUNT,  // SMOOTH_PRED
      MB_MODE_COUNT,  // SMOOTH_V_PRED
      MB_MODE_COUNT,  // SMOOTH_H_PRED
      MB_MODE_COUNT,  // PAETH_PRED
      MB_MODE_COUNT,  // NEARESTMV
      MB_MODE_COUNT,  // NEARMV
      MB_MODE_COUNT,  // GLOBALMV
      MB_MODE_COUNT,  // NEWMV
      NEARESTMV,      // NEAREST_NEARESTMV
      NEARMV,         // NEAR_NEARMV
      NEARESTMV,      // NEAREST_NEWMV
      NEWMV,          // NEW_NEARESTMV
      NEARMV,         // NEAR_NEWMV
      NEWMV,          // NEW_NEARMV
      GLOBALMV,       // GLOBAL_GLOBALMV
      NEWMV,          // NEW_NEWMV
    };
    assert(NELEMENTS(lut) == MB_MODE_COUNT);
    assert(is_inter_compound_mode(mode));
    return lut[mode];
}

static INLINE PredictionMode compound_ref1_mode(PredictionMode mode) {
    static PredictionMode lut[] = {
      MB_MODE_COUNT,  // DC_PRED
      MB_MODE_COUNT,  // V_PRED
      MB_MODE_COUNT,  // H_PRED
      MB_MODE_COUNT,  // D45_PRED
      MB_MODE_COUNT,  // D135_PRED
      MB_MODE_COUNT,  // D113_PRED
      MB_MODE_COUNT,  // D157_PRED
      MB_MODE_COUNT,  // D203_PRED
      MB_MODE_COUNT,  // D67_PRED
      MB_MODE_COUNT,  // SMOOTH_PRED
      MB_MODE_COUNT,  // SMOOTH_V_PRED
      MB_MODE_COUNT,  // SMOOTH_H_PRED
      MB_MODE_COUNT,  // PAETH_PRED
      MB_MODE_COUNT,  // NEARESTMV
      MB_MODE_COUNT,  // NEARMV
      MB_MODE_COUNT,  // GLOBALMV
      MB_MODE_COUNT,  // NEWMV
      NEARESTMV,      // NEAREST_NEARESTMV
      NEARMV,         // NEAR_NEARMV
      NEWMV,          // NEAREST_NEWMV
      NEARESTMV,      // NEW_NEARESTMV
      NEWMV,          // NEAR_NEWMV
      NEARMV,         // NEW_NEARMV
      GLOBALMV,       // GLOBAL_GLOBALMV
      NEWMV,          // NEW_NEWMV
    };
    assert(NELEMENTS(lut) == MB_MODE_COUNT);
    assert(is_inter_compound_mode(mode));
    return lut[mode];
}

void inter_block_mode_info(EbDecHandle *dec_handle, PartitionInfo_t* pi,
    int mi_row, int mi_col,  SvtReader *r)
{
    ModeInfo_t *mbmi = pi->mi;
    const BlockSize bsize = mbmi->sb_type;
    const int allow_hp = dec_handle->frame_header.allow_high_precision_mv;
    IntMv_dec ref_mvs[MODE_CTX_REF_FRAMES][MAX_MV_REF_CANDIDATES] = { { { 0 } } };
    int16_t inter_mode_ctx[MODE_CTX_REF_FRAMES];
    int pts[SAMPLES_ARRAY_SIZE], pts_inref[SAMPLES_ARRAY_SIZE];
    SegmentationParams *seg = &dec_handle->frame_header.segmentation_params;
    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    MvCount mv_cnt;

    /* TO-DO initialize palette info */

    svt_collect_neighbors_ref_counts(pi);

    read_ref_frames(dec_handle, pi, r);
    const int is_compound = has_second_ref(mbmi);

    MvReferenceFrame ref_frame = av1_ref_frame_type(mbmi->ref_frame);
    IntMv_dec global_mvs[2];
    av1_find_mv_refs(dec_handle, pi, ref_frame, pi->ref_mv_stack,
        ref_mvs, global_mvs, mi_row, mi_col,
        inter_mode_ctx, &mv_cnt);

#if EXTRA_DUMP
    if (first_frame) {
        printf("\n mi_row: %d mi_col: %d\n", mi_row, mi_col);
        /*for (int i = 0; i < MODE_CTX_REF_FRAMES; i++)
            for (int j = 0; j < MAX_REF_MV_STACK_SIZE; j++)
                printf("ref_mv_stack[%d][%d]=%d\t", i, j, pi->ref_mv_stack[i][j].this_mv.as_int);
        printf("\n");*/
        fflush(stdout);
    }
#endif

    int mode_ctx = svt_mode_context_analyzer(inter_mode_ctx, mbmi->ref_frame);
    mbmi->ref_mv_idx = 0;

    if (mbmi->skip_mode) {
        assert(is_compound);
        mbmi->mode = NEAREST_NEARESTMV;
    }
    else {
        if (seg_feature_active(seg, mbmi->segment_id, SEG_LVL_SKIP) ||
            seg_feature_active(seg, mbmi->segment_id, SEG_LVL_GLOBALMV))
            mbmi->mode = GLOBALMV;
        else {
            if (is_compound)
                mbmi->mode = read_inter_compound_mode(dec_handle, r, mode_ctx);
            else {
                int new_mv = svt_read_symbol(r, parse_ctxt->cur_tile_ctx.
                    newmv_cdf[mode_ctx & NEWMV_CTX_MASK], 2, ACCT_STR);
                if (new_mv) {
                    int zero_mv = svt_read_symbol(r,
                        parse_ctxt->cur_tile_ctx.zeromv_cdf
                        [(mode_ctx >> GLOBALMV_OFFSET) & GLOBALMV_CTX_MASK],
                        2, ACCT_STR);
                    if (zero_mv) {
                        int ref_mv = svt_read_symbol(r, parse_ctxt->cur_tile_ctx.
                            refmv_cdf[(mode_ctx >> REFMV_OFFSET) & REFMV_CTX_MASK],
                            2, ACCT_STR);
                        mbmi->mode = ref_mv ? NEARMV : NEARESTMV;
                    }
                    else
                        mbmi->mode = GLOBALMV;
                }
                else
                    mbmi->mode = NEWMV;
            }
            if (mbmi->mode == NEWMV || mbmi->mode == NEW_NEWMV ||
                has_nearmv(mbmi->mode))
                read_drl_idx(dec_handle, pi, mbmi, r, mv_cnt.num_mv_found[ref_frame]);
        }
    }

    IntMv_dec ref_mv[2];
    IntMv_dec nearestmv[2], nearmv[2];
    if (!is_compound && mbmi->mode != GLOBALMV) {
        svt_find_best_ref_mvs(allow_hp, ref_mvs[mbmi->ref_frame[0]], &nearestmv[0],
            &nearmv[0], dec_handle->frame_header.force_integer_mv);
    }
    if (is_compound && mbmi->mode != GLOBAL_GLOBALMV) {
        int ref_mv_idx = mbmi->ref_mv_idx + 1;
        nearestmv[0] = pi->ref_mv_stack[ref_frame][0].this_mv;
        nearestmv[1] = pi->ref_mv_stack[ref_frame][0].comp_mv;
        nearmv[0] = pi->ref_mv_stack[ref_frame][ref_mv_idx].this_mv;
        nearmv[1] = pi->ref_mv_stack[ref_frame][ref_mv_idx].comp_mv;
        lower_mv_precision(&nearestmv[0].as_mv, allow_hp,
            dec_handle->frame_header.force_integer_mv);
        lower_mv_precision(&nearestmv[1].as_mv, allow_hp,
            dec_handle->frame_header.force_integer_mv);
        lower_mv_precision(&nearmv[0].as_mv, allow_hp,
            dec_handle->frame_header.force_integer_mv);
        lower_mv_precision(&nearmv[1].as_mv, allow_hp,
            dec_handle->frame_header.force_integer_mv);
    }
    else if (mbmi->ref_mv_idx > 0 && mbmi->mode == NEARMV) {
        IntMv_dec cur_mv =
            pi->ref_mv_stack[mbmi->ref_frame[0]][1 + mbmi->ref_mv_idx].this_mv;
        nearmv[0] = cur_mv;
    }

    ref_mv[0] = nearestmv[0];
    ref_mv[1] = nearestmv[1];

    if (is_compound) {
        int ref_mv_idx = mbmi->ref_mv_idx;
        // Special case: NEAR_NEWMV and NEW_NEARMV modes use
        // 1 + mbmi->ref_mv_idx (like NEARMV) instead of
        // mbmi->ref_mv_idx (like NEWMV)
        if (mbmi->mode == NEAR_NEWMV || mbmi->mode == NEW_NEARMV)
            ref_mv_idx = 1 + mbmi->ref_mv_idx;

        if (compound_ref0_mode(mbmi->mode) == NEWMV)
            ref_mv[0] = pi->ref_mv_stack[ref_frame][ref_mv_idx].this_mv;

        if (compound_ref1_mode(mbmi->mode) == NEWMV)
            ref_mv[1] = pi->ref_mv_stack[ref_frame][ref_mv_idx].comp_mv;
    }
    else {
        if (mbmi->mode == NEWMV) {
            if (mv_cnt.num_mv_found[ref_frame] > 1)
                ref_mv[0] = pi->ref_mv_stack[ref_frame][mbmi->ref_mv_idx].this_mv;
        }
    }

    assign_mv(dec_handle, pi, mbmi->mv, global_mvs,
        ref_mv, nearestmv, nearmv, mi_row, mi_col,
        is_compound, allow_hp, r);

#if EXTRA_DUMP
    if (first_frame) {
        printf("\n mode %d MV %d %d \n", mbmi->mode, mbmi->mv[0].as_mv.row, mbmi->mv[0].as_mv.col);
        fflush(stdout);
    }
#endif
    read_interintra_mode(dec_handle, mbmi, r);

    pi->num_samples = find_warp_samples(dec_handle, pi, mi_row, mi_col, pts, pts_inref);

    mbmi->motion_mode = read_motion_mode(dec_handle, pi, mi_row, mi_col, r);

    read_compound_type(dec_handle, pi, mi_row, mi_col, r);

    if (!av1_is_interp_needed(pi, dec_handle->cur_pic_buf[0]->global_motion)) {
        set_default_interp_filters(mbmi,
            dec_handle->frame_header.interpolation_filter);
    }
    else {
        if (dec_handle->frame_header.interpolation_filter != SWITCHABLE) {
            mbmi->interp_filters =
                av1_broadcast_interp_filter(dec_handle->frame_header.interpolation_filter);
        }
        else {
            InterpFilter ref0_filter[2] = { EIGHTTAP_REGULAR, EIGHTTAP_REGULAR };
            for (int dir = 0; dir < 2; ++dir) {
                const int ctx = get_context_interp(pi, dir);
                ref0_filter[dir] = (InterpFilter)svt_read_symbol(
                    r, parse_ctxt->cur_tile_ctx.switchable_interp_cdf[ctx], SWITCHABLE_FILTERS, ACCT_STR);
                if (dec_handle->seq_header.enable_dual_filter == 0) {
                    ref0_filter[1] = ref0_filter[0];
                    break;
                }
            }
            mbmi->interp_filters =
                av1_make_interp_filters(ref0_filter[0], ref0_filter[1]);
        }
    }
}

void intra_block_mode_info(EbDecHandle *dec_handle, int mi_row,
    int mi_col, PartitionInfo_t* xd, ModeInfo_t *mbmi, SvtReader *r)
{
    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    const BlockSize bsize = mbmi->sb_type;
    mbmi->ref_frame[0] = INTRA_FRAME;
    mbmi->ref_frame[1] = NONE_FRAME;

    EbColorConfig *color_cfg = &dec_handle->seq_header.color_config;
    uint8_t     *lossless_array = &dec_handle->frame_header.lossless_array[0];

    mbmi->mode = read_intra_mode(r, parse_ctxt->cur_tile_ctx.y_mode_cdf[size_group_lookup[bsize]]);

    mbmi->angle_delta[PLANE_TYPE_Y] =
        intra_angle_info(r, &parse_ctxt->cur_tile_ctx.
            angle_delta_cdf[mbmi->mode - V_PRED][0], mbmi->mode, bsize);
    const int has_chroma =
        dec_is_chroma_reference(mi_row, mi_col, bsize, color_cfg->subsampling_x,
            color_cfg->subsampling_y);
    xd->has_chroma = has_chroma;
    if (has_chroma) {
        mbmi->uv_mode =
            read_intra_mode_uv(&parse_ctxt->cur_tile_ctx, r,
                is_cfl_allowed(xd, color_cfg, lossless_array), mbmi->mode);
        if (mbmi->uv_mode == UV_CFL_PRED) {
            mbmi->cfl_alpha_idx =
                read_cfl_alphas(&parse_ctxt->cur_tile_ctx, r, &mbmi->cfl_alpha_signs);
        }
        mbmi->angle_delta[PLANE_TYPE_UV] = intra_angle_info(r,
            &parse_ctxt->cur_tile_ctx.angle_delta_cdf[mbmi->uv_mode - V_PRED][0],
            mbmi->uv_mode, bsize);
    }

    if (allow_palette(dec_handle->frame_header.allow_screen_content_tools, bsize))
        palette_mode_info(/*dec_handle, mi_row, mi_col, r*/);

    filter_intra_mode_info(dec_handle, xd, r);
}

int read_is_inter(EbDecHandle* dec_handle, PartitionInfo_t * xd,
    int segment_id, SvtReader *r)
{
    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    int is_inter = 0;
    SegmentationParams *seg_params = &dec_handle->frame_header.segmentation_params;
    if (seg_feature_active(seg_params, segment_id, SEG_LVL_REF_FRAME))
        is_inter = get_segdata(seg_params, segment_id, SEG_LVL_REF_FRAME) != INTRA_FRAME;
    else if (seg_feature_active(seg_params, segment_id, SEG_LVL_GLOBALMV))
        is_inter = 1;
    else {
        const int ctx = get_intra_inter_context(xd);
        is_inter = svt_read_symbol(r, parse_ctxt->cur_tile_ctx.
            intra_inter_cdf[ctx], 2, ACCT_STR);
    }
    return is_inter;
}

void inter_frame_mode_info(EbDecHandle *dec_handle, PartitionInfo_t * pi,
    uint32_t mi_row, uint32_t mi_col, SvtReader *r, int8_t *cdef_strength)
{
    ModeInfo_t *mbmi = pi->mi;
    mbmi->use_intrabc = 0;
    int inter_block = 1;

    mbmi->mv[0].as_int = 0;
    mbmi->mv[1].as_int = 0;

    mbmi->segment_id = read_inter_segment_id(dec_handle, pi, mi_row, mi_col, 1, r);

    mbmi->skip_mode = read_skip_mode(dec_handle, pi, mbmi->segment_id, r);

    if (mbmi->skip_mode)
        mbmi->skip = 1;
    else
        mbmi->skip = read_skip(dec_handle, pi, mbmi->segment_id, r);

    if (!dec_handle->frame_header.segmentation_params.seg_id_pre_skip)
        mbmi->segment_id = read_inter_segment_id(dec_handle, pi, mi_row, mi_col, 0, r);

    dec_handle->frame_header.coded_lossless = dec_handle->frame_header.
        lossless_array[mbmi->segment_id];
    read_cdef(dec_handle, r, pi, mi_col, mi_row, cdef_strength);

    read_delta_params(dec_handle, r, pi, mi_row, mi_col);

    if (!mbmi->skip_mode)
        inter_block = read_is_inter(dec_handle, pi, mbmi->segment_id, r);

    if (inter_block)
        inter_block_mode_info(dec_handle, pi, mi_row, mi_col, r);
    else
        intra_block_mode_info(dec_handle, mi_row, mi_col, pi, mbmi, r);
}

void mode_info(EbDecHandle *decHandle, PartitionInfo_t *part_info, uint32_t mi_row,
    uint32_t mi_col, SvtReader *r, int8_t *cdef_strength)
{
    ModeInfo_t *mi = part_info->mi;
    mi->use_intrabc = 0;

    if (decHandle->frame_header.frame_type == KEY_FRAME
        || decHandle->frame_header.frame_type == INTRA_ONLY_FRAME)
    {
        intra_frame_mode_info(decHandle, part_info, mi_row, mi_col, r, cdef_strength);
    }
    else
        inter_frame_mode_info(decHandle, part_info, mi_row, mi_col, r, cdef_strength);
}

TxSize read_tx_size(EbDecHandle *dec_handle, PartitionInfo_t *xd,
                    int allow_select, SvtReader *r)
{
    ModeInfo_t *mbmi = xd->mi;
    const TxMode tx_mode = dec_handle->frame_header.tx_mode;
    const BlockSize bsize = xd->mi->sb_type;
    if (dec_handle->frame_header.lossless_array[mbmi->segment_id]) return TX_4X4;

    if (bsize > BLOCK_4X4 && allow_select && tx_mode == TX_MODE_SELECT) {
        const TxSize coded_tx_size = read_selected_tx_size(xd, r, dec_handle);
        return coded_tx_size;
    }
    assert(IMPLIES(tx_mode == ONLY_4X4, bsize == BLOCK_4X4));
    TxSize tx_size = max_txsize_rect_lookup[bsize];
    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    return tx_size;
}

static INLINE void txfm_nbr_update(uint8_t *above_ctx,
    uint8_t *left_ctx, TxSize tx_size, TxSize txb_size)
{
    BlockSize bsize = txsize_to_bsize[txb_size];
    int bh = mi_size_high[bsize];
    int bw = mi_size_wide[bsize];
    uint8_t txw = tx_size_wide[tx_size];
    uint8_t txh = tx_size_high[tx_size];
    memset(above_ctx, txw, bw);
    memset(left_ctx, txh, bh);
}

static INLINE TxSize av1_get_max_uv_txsize(BlockSize bsize, int subsampling_x,
    int subsampling_y)
{
    const BlockSize plane_bsize =
        get_plane_block_size(bsize, subsampling_x, subsampling_y);
    assert(plane_bsize < BlockSizeS_ALL);
    const TxSize uv_tx = max_txsize_rect_lookup[plane_bsize];
    return av1_get_adjusted_tx_size(uv_tx);
}

/* Update Chroma Transform Info for Inter Case! */
void update_chroma_trans_info(EbDecHandle *dec_handle,
    PartitionInfo_t *part_info, BlockSize bsize)
{
    ParseCtxt *parse_ctx = (ParseCtxt*)dec_handle->pv_parse_ctxt;
    ModeInfo_t *mbmi = part_info->mi;
    SBInfo     *sb_info = part_info->sb_info;
    EbColorConfig color_config = dec_handle->seq_header.color_config;

    int num_chroma_tus = 0, step_r, step_c;

    int sx = color_config.subsampling_x;
    int sy = color_config.subsampling_y;

    TransformInfo_t *chroma_trans_info = sb_info->sb_chroma_trans_info +
        mbmi->first_chroma_tu_offset;

    const TxSize max_tx_size = max_txsize_rect_lookup[bsize];
    const int bh = tx_size_high_unit[max_tx_size];
    const int bw = tx_size_wide_unit[max_tx_size];
    const int width = block_size_wide[bsize] >> tx_size_wide_log2[0];
    const int height = block_size_high[bsize] >> tx_size_high_log2[0];

    TxSize tx_size_uv = av1_get_max_uv_txsize(bsize, sx, sy);
    /* TODO: Make plane loop and avoid the unroll */
    for (int idy = 0; idy < height; idy += bh) {
        for (int idx = 0; idx < width; idx += bw) {

            if (color_config.mono_chrome)
                continue;

            /* Update Chroma Transform Info */
            if (!dec_is_chroma_reference(part_info->mi_row, part_info->mi_col,
                bsize, sx, sy))
                continue;

            step_r = tx_size_high_unit[tx_size_uv];
            step_c = tx_size_wide_unit[tx_size_uv];

            /* UV dim. of 4 special case! */
            const int unit_height = ROUND_POWER_OF_TWO(
                AOMMIN(bh + idy, bh), sy);
            const int unit_width = ROUND_POWER_OF_TWO(
                AOMMIN(bw + idx, bw), sx);
            /* TODO : Can cause prblm for incomplete SBs. Fix! */
            for (int blk_row = 0; blk_row < unit_height; blk_row += step_r) {
                for (int blk_col = 0; blk_col < unit_width; blk_col += step_c) {
                    chroma_trans_info->tx_size = tx_size_uv; // Chroma Cb
                    chroma_trans_info++;
                    num_chroma_tus++;

                    chroma_trans_info->tx_size = tx_size_uv; // Chroma Cr
                    chroma_trans_info++;
                }
            }
        }
    }

    mbmi->num_chroma_tus = num_chroma_tus;
    parse_ctx->first_chroma_tu_offset += 2 * num_chroma_tus;
}

TxSize find_tx_size(w, h) {
    int tx_sz;
    for (tx_sz = 0; tx_sz < TX_SIZES_ALL; tx_sz++)
        if (tx_size_wide[tx_sz] == w && tx_size_high[tx_sz] == h)
            break;
    return tx_sz;
}

int get_txfm_split_ctx(PartitionInfo_t *pi, ParseCtxt *parse_ctx,
    TxSize tx_size, int blk_row, int blk_col)
{
    int above = parse_ctx->parse_nbr4x4_ctxt.
        above_tx_wd[pi->mi_col + blk_col] < tx_size_wide[tx_size];
    int left = parse_ctx->parse_nbr4x4_ctxt.
        left_tx_ht[pi->mi_row - parse_ctx->sb_row_mi + blk_row] < tx_size_high[tx_size];
    int size = MIN(64, MAX(block_size_wide[pi->mi->sb_type],
                           block_size_high[pi->mi->sb_type]));

    TxSize max_tx_size = find_tx_size(size, size);
    TxSize tx_sz_sqr_up = txsize_sqr_up_map[tx_size];
    return ((tx_sz_sqr_up != max_tx_size) * 3 +
             (TX_SIZES - 1 - max_tx_size) * 6 + above + left);
}

void read_var_tx_size(EbDecHandle *dec_handle, PartitionInfo_t *pi, SvtReader *r,
    TxSize tx_size, int blk_row, int blk_col, int depth)
{
    ModeInfo_t *mbmi = pi->mi;
    ParseCtxt *parse_ctx = (ParseCtxt*)dec_handle->pv_parse_ctxt;
    const BlockSize bsize = mbmi->sb_type;
    const int max_blocks_high = max_block_high(pi, bsize, 0);
    const int max_blocks_wide = max_block_wide(pi, bsize, 0);
    int i, j, txfm_split = 0;

    if (blk_row >= max_blocks_high || blk_col >= max_blocks_wide) return;

    if (tx_size == TX_4X4 || depth == MAX_VARTX_DEPTH + 1)
        txfm_split = 0;
    else {
        int ctx = get_txfm_split_ctx(pi, parse_ctx, tx_size, blk_row, blk_col);
        txfm_split = svt_read_symbol(r, parse_ctx->cur_tile_ctx.txfm_partition_cdf[ctx],
            2, ACCT_STR);
    }

    int w4 = tx_size_wide_unit[tx_size];
    int h4 = tx_size_high_unit[tx_size];
    if (txfm_split) {
        TxSize sub_tx_sz = sub_tx_size_map[tx_size];
        int step_w = tx_size_wide_unit[sub_tx_sz];
        int step_h = tx_size_high_unit[sub_tx_sz];

        for (i = 0; i < h4; i += step_h)
            for (j = 0; j < w4; j += step_w)
                read_var_tx_size(dec_handle, pi, r, sub_tx_sz, blk_row + i,
                    blk_col + j, depth + 1);
    }
    else {
        parse_ctx->cur_luma_trans_info->tx_size = tx_size;
        parse_ctx->cur_luma_trans_info++;
        parse_ctx->cur_blk_luma_count++;
        update_tx_context(parse_ctx, pi, bsize, tx_size, blk_row, blk_col);
    }
}

/* Update Flat Transform Info for Intra Case! */
void update_flat_trans_info(EbDecHandle *dec_handle, PartitionInfo_t *part_info,
                            BlockSize bsize, TxSize tx_size)
{
    ParseCtxt *parse_ctx = (ParseCtxt*)dec_handle->pv_parse_ctxt;
    ModeInfo_t *mbmi = part_info->mi;
    SBInfo     *sb_info = part_info->sb_info;
    EbColorConfig color_config = dec_handle->seq_header.color_config;

    int num_luma_tus = 0;
    int num_chroma_tus = 0;

    int sx = color_config.subsampling_x;
    int sy = color_config.subsampling_y;

    TransformInfo_t *luma_trans_info = sb_info->sb_luma_trans_info +
                                    mbmi->first_luma_tu_offset;
    TransformInfo_t *chroma_trans_info = sb_info->sb_chroma_trans_info +
                                     mbmi->first_chroma_tu_offset;

    const TxSize max_tx_size = max_txsize_rect_lookup[bsize];
    const int bh = tx_size_high_unit[max_tx_size];
    const int bw = tx_size_wide_unit[max_tx_size];
    const int width = block_size_wide[bsize] >> tx_size_wide_log2[0];
    const int height = block_size_high[bsize] >> tx_size_high_log2[0];

    TxSize tx_size_uv = av1_get_max_uv_txsize(bsize, sx, sy);
    /* TODO: Make plane loop and avoid the unroll */
    for (int idy = 0; idy < height; idy += bh) {
        for (int idx = 0; idx < width; idx += bw) {
            /* Update Luma Transform Info */
            int stepr = tx_size_high_unit[tx_size];
            int stepc = tx_size_wide_unit[tx_size];

            /* TODO : Can cause prblm for incomplete SBs. Fix! */
            for (int blk_row = idy; blk_row < (idy + bh); blk_row += stepr) {
                for (int blk_col = idx; blk_col < (idx + bw); blk_col += stepc) {
                    luma_trans_info->tx_size = tx_size;
                    luma_trans_info++;
                    num_luma_tus++;
                }
            }

            if(color_config.mono_chrome)
                continue;

            /* Update Chroma Transform Info */
            if (!dec_is_chroma_reference(part_info->mi_row, part_info->mi_col,
                bsize, sx, sy))
                continue;

            stepr = tx_size_high_unit[tx_size_uv];
            stepc = tx_size_wide_unit[tx_size_uv];

            /* UV dim. of 4 special case! */
            const int unit_height = ROUND_POWER_OF_TWO(
                AOMMIN(bh + idy, bh), sy);
            const int unit_width = ROUND_POWER_OF_TWO(
                AOMMIN(bw + idx, bw), sx);
            /* TODO : Can cause prblm for incomplete SBs. Fix! */
            for (int blk_row = 0; blk_row < unit_height; blk_row += stepr) {
                for (int blk_col = 0; blk_col < unit_width; blk_col += stepc) {
                    chroma_trans_info->tx_size = tx_size_uv;
                    chroma_trans_info++;
                    num_chroma_tus++;
                }
            }
            for (int blk_row = 0; blk_row < unit_height; blk_row += stepr) {
                for (int blk_col = 0; blk_col < unit_width; blk_col += stepc) {
                    chroma_trans_info->tx_size = tx_size_uv;
                    chroma_trans_info++;
                }
            }
        }
    }

    mbmi->num_luma_tus = num_luma_tus;
    mbmi->num_chroma_tus = num_chroma_tus;

    parse_ctx->first_luma_tu_offset += num_luma_tus;
    parse_ctx->first_chroma_tu_offset += 2 * num_chroma_tus;
}

static INLINE void set_txfm_ctxs(ParseCtxt *parse_ctx, TxSize tx_size,
    int n4_w, int n4_h, int skip, const PartitionInfo_t *pi)
{
    ParseNbr4x4Ctxt *ngr_ctx = &parse_ctx->parse_nbr4x4_ctxt;
    int mi_row = pi->mi_row;
    int mi_col = pi->mi_col;
    uint8_t tx_wide = tx_size_wide[tx_size];
    uint8_t tx_high = tx_size_high[tx_size];
    uint8_t *const above_ctx = ngr_ctx->above_tx_wd + mi_col ;
    uint8_t *const left_ctx = ngr_ctx->left_tx_ht + (mi_row - parse_ctx->sb_row_mi);
    if (skip) {
        tx_wide = n4_w * MI_SIZE;
        tx_high = n4_h * MI_SIZE;
    }
    memset(above_ctx, tx_wide, n4_w);
    memset(left_ctx, tx_high, n4_h);
}

void read_block_tx_size(EbDecHandle *dec_handle, SvtReader *r,
    PartitionInfo_t *part_info, BlockSize bsize)
{
    ModeInfo_t *mbmi = part_info->mi;
    ParseCtxt *parse_ctx = (ParseCtxt*)dec_handle->pv_parse_ctxt;
    SBInfo     *sb_info = part_info->sb_info;
    EbColorConfig color_config = dec_handle->seq_header.color_config;

    int inter_block_tx = dec_is_inter_block(mbmi);

    if (dec_handle->frame_header.tx_mode == TX_MODE_SELECT && bsize > BLOCK_4X4 &&
        !mbmi->skip && inter_block_tx &&
        !dec_handle->frame_header.lossless_array[mbmi->segment_id])
    {
        const TxSize max_tx_size = max_txsize_rect_lookup[bsize];
        const int bh = tx_size_high_unit[max_tx_size];
        const int bw = tx_size_wide_unit[max_tx_size];
        const int width = block_size_wide[bsize] >> tx_size_wide_log2[0];
        const int height = block_size_high[bsize] >> tx_size_high_log2[0];

        // Current luma trans_info and offset initialization
        parse_ctx->cur_luma_trans_info = sb_info->sb_luma_trans_info +
                                         mbmi->first_luma_tu_offset;
        parse_ctx->cur_blk_luma_count = 0;

        // Luma trans_info update
        for (int idy = 0; idy < height; idy += bh)
            for (int idx = 0; idx < width; idx += bw)
                read_var_tx_size(dec_handle, part_info, r, max_tx_size, idy, idx, 0);

        // Chroma trans_info update
        update_chroma_trans_info(dec_handle, part_info, bsize);

        mbmi->num_luma_tus = parse_ctx->cur_blk_luma_count;
        parse_ctx->first_luma_tu_offset += parse_ctx->cur_blk_luma_count;
    }
    else {
        TxSize tx_size = read_tx_size(dec_handle, part_info,
            !mbmi->skip || !inter_block_tx, r);

        int b4_w = mi_size_wide[mbmi->sb_type];
        int b4_h = mi_size_high[mbmi->sb_type];

        set_txfm_ctxs(parse_ctx, tx_size, b4_w, b4_h,
            mbmi->skip && dec_is_inter_block(mbmi), part_info);

        /* Update Flat Transform Info */
        update_flat_trans_info(dec_handle, part_info, bsize, tx_size);
    }
}

BlockSize get_plane_residual_size(BlockSize bsize,
    int subsampling_x, int subsampling_y)
{
    if (bsize == BLOCK_INVALID) return BLOCK_INVALID;
    return ss_size_lookup[bsize][subsampling_x][subsampling_y];
}

TxSetType get_tx_set(TxSize tx_size, int is_inter, int use_reduced_set) {
    return get_ext_tx_set_type(tx_size, is_inter, use_reduced_set);
}

void parse_transform_type(EbDecHandle *dec_handle, PartitionInfo_t *xd,
     TxSize tx_size, SvtReader *r, TransformInfo_t *trans_info)
{
    ModeInfo_t *mbmi = xd->mi;

    TxType *tx_type = &trans_info->txk_type;
    *tx_type = DCT_DCT;

    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    FRAME_CONTEXT *frm_ctx = &parse_ctxt->cur_tile_ctx;

    // No need to read transform type if block is skipped.
    if (mbmi->skip || seg_feature_active(&dec_handle->frame_header.
        segmentation_params, mbmi->segment_id, SEG_LVL_SKIP))
        return;

    const int qindex = dec_handle->frame_header.quantization_params.base_q_idx;
    if (qindex == 0) return;

    const int inter_block = dec_is_inter_block(mbmi);
    const TxSetType tx_set_type =
        get_ext_tx_set_type(tx_size, inter_block,
            dec_handle->frame_header.reduced_tx_set);
    if (av1_num_ext_tx_set[tx_set_type] > 1) {
        const int eset =
            get_ext_tx_set(tx_size, inter_block,
                dec_handle->frame_header.reduced_tx_set);
        // eset == 0 should correspond to a set with only DCT_DCT and
        // there is no need to read the tx_type
        assert(eset != 0);

        const TxSize square_tx_size = txsize_sqr_map[tx_size];
        if (inter_block) {
            *tx_type = av1_ext_tx_inv[tx_set_type][svt_read_symbol(
                r, frm_ctx->inter_ext_tx_cdf[eset][square_tx_size],
                av1_num_ext_tx_set[tx_set_type], ACCT_STR)];
        }
        else {
            const PredictionMode intra_mode =
                mbmi->filter_intra_mode_info.use_filter_intra
                ? fimode_to_intradir[mbmi->filter_intra_mode_info
                .filter_intra_mode]
                : mbmi->mode;
            *tx_type = av1_ext_tx_inv[tx_set_type][svt_read_symbol(
                r, frm_ctx->intra_ext_tx_cdf[eset][square_tx_size][intra_mode],
                av1_num_ext_tx_set[tx_set_type], ACCT_STR)];
        }
    }
}

const ScanOrder* get_scan(TxSize tx_size, TxSize tx_type) {
    return &av1_scan_orders[tx_size][tx_type];
}

static INLINE int av1_get_txk_type_index(BlockSize bsize,
    int blk_row, int blk_col)
{
    TxSize txs = max_txsize_rect_lookup[bsize];
    for (int level = 0; level < MAX_VARTX_DEPTH; ++level)
        txs = sub_tx_size_map[txs];
    const int tx_w_log2 = tx_size_wide_log2[txs] - MI_SIZE_LOG2;
    const int tx_h_log2 = tx_size_high_log2[txs] - MI_SIZE_LOG2;
    const int bw_uint_log2 = mi_size_wide_log2[bsize];
    const int stride_log2 = bw_uint_log2 - tx_w_log2;
    const int index =
        ((blk_row >> tx_h_log2) << stride_log2) + (blk_col >> tx_w_log2);
    assert(index < TXK_TYPE_BUF_LEN);
    return index;
}

TxType compute_tx_type(PlaneType plane_type,
    const PartitionInfo_t *xd, uint32_t blk_row,
    uint32_t blk_col, TxSize tx_size,
    int reduced_tx_set, EbColorConfig *color_cfg,
    uint8_t *lossless_array, TransformInfo_t *trans_info)
{
    const ModeInfo_t *const mbmi = xd->mi;
    const TxSetType tx_set_type =
        get_ext_tx_set_type(tx_size, dec_is_inter_block(mbmi), reduced_tx_set);

    TxType tx_type = DCT_DCT;
    if (lossless_array[mbmi->segment_id] || txsize_sqr_up_map[tx_size] > TX_32X32)
        tx_type = DCT_DCT;
    else {
        if (plane_type == PLANE_TYPE_Y || dec_is_inter_block(mbmi)){
            tx_type = trans_info->txk_type;
        }
        else {
            // In intra mode, uv planes don't share the same prediction mode as y
            // plane, so the tx_type should not be shared
            tx_type = intra_mode_to_tx_type(mbmi, PLANE_TYPE_UV);
        }
    }
    assert(tx_type < TX_TYPES);
    if (!av1_ext_tx_used[tx_set_type][tx_type]) return DCT_DCT;
    return tx_type;
}

static INLINE int is_interintra_wedge_used(BlockSize sb_type) {
    return wedge_params_lookup[sb_type].bits > 0;
}

static INLINE int is_comp_ref_allowed(BlockSize bsize) {
    return AOMMIN(block_size_wide[bsize], block_size_high[bsize]) >= 8;
}

static INLINE int is_masked_compound_type(CompoundType type) {
    return (type == COMPOUND_WEDGE || type == COMPOUND_DIFFWTD);
}

static INLINE int is_interinter_compound_used(CompoundType type, BlockSize sb_type) {
    const int comp_allowed = is_comp_ref_allowed(sb_type);
    switch (type) {
    case COMPOUND_AVERAGE:
    case COMPOUND_DISTWTD:
    case COMPOUND_DIFFWTD: return comp_allowed;
    case COMPOUND_WEDGE:
        return comp_allowed && wedge_params_lookup[sb_type].bits > 0;
    default: assert(0); return 0;
    }
}

static INLINE int is_any_masked_compound_used(BlockSize sb_type) {
    CompoundType comp_type;
    int i;
    if (!is_comp_ref_allowed(sb_type)) return 0;
    for (i = 0; i < COMPOUND_TYPES; i++) {
        comp_type = (CompoundType)i;
        if (is_masked_compound_type(comp_type) &&
            is_interinter_compound_used(comp_type, sb_type))
            return 1;
    }
    return 0;
}

static INLINE int get_comp_group_idx_context(const PartitionInfo_t *xd) {
    const ModeInfo_t *const above_mi = xd->above_mbmi;
    const ModeInfo_t *const left_mi = xd->left_mbmi;
    int above_ctx = 0, left_ctx = 0;

    if (above_mi) {
        if (has_second_ref(above_mi))
            above_ctx = above_mi->inter_compound.comp_group_idx;
        else if (above_mi->ref_frame[0] == ALTREF_FRAME)
            above_ctx = 3;
    }
    if (left_mi) {
        if (has_second_ref(left_mi))
            left_ctx = left_mi->inter_compound.comp_group_idx;
        else if (left_mi->ref_frame[0] == ALTREF_FRAME)
            left_ctx = 3;
    }

    return AOMMIN(5, above_ctx + left_ctx);
}

static INLINE int is_mv_valid(const MV *mv) {
    return mv->row > MV_LOW && mv->row < MV_UPP && mv->col > MV_LOW &&
        mv->col < MV_UPP;
}

static AOM_FORCE_INLINE int get_br_ctx_eob(const int c,  // raster order
    const int bwl, const TxClass tx_class)
{
    const int row = c >> bwl;
    const int col = c - (row << bwl);
    if (c == 0) return 0;
    if ((tx_class == TX_CLASS_2D && row < 2 && col < 2) ||
        (tx_class == TX_CLASS_HORIZ && col == 0) ||
        (tx_class == TX_CLASS_VERT && row == 0))
        return 7;
    return 14;
}

void update_coeff_ctx(EbDecHandle *dec_handle, int plane, PartitionInfo_t *pi,
    TxSize tx_size, uint32_t blk_row, uint32_t blk_col, int above_off,
    int left_off, int cul_level, int dc_val)
{
    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    ParseNbr4x4Ctxt *ngr_ctx = &parse_ctxt->parse_nbr4x4_ctxt;

    uint8_t suby = plane ? dec_handle->seq_header.color_config.subsampling_y : 0;
    uint8_t subx = plane ? dec_handle->seq_header.color_config.subsampling_x : 0;

    uint8_t *const above_dc_ctx = ngr_ctx->above_dc_ctx[plane] + blk_col;
    uint8_t *const left_dc_ctx = ngr_ctx->left_dc_ctx[plane] +
        (blk_row - (parse_ctxt->sb_row_mi >> suby));

    uint8_t *const above_level_ctx = ngr_ctx->above_level_ctx[plane] + blk_col;
    uint8_t *const left_level_ctx = ngr_ctx->left_level_ctx[plane] +
        (blk_row - (parse_ctxt->sb_row_mi >> suby));

    const int txs_wide = tx_size_wide_unit[tx_size];
    const int txs_high = tx_size_high_unit[tx_size];

    if (pi->mb_to_right_edge < 0) {
        int plane_bsize = (pi->mi->sb_type == BLOCK_INVALID) ? BLOCK_INVALID :
            ss_size_lookup[pi->mi->sb_type][subx][suby];
        const int blocks_wide = max_block_wide(pi, plane_bsize, subx);
        const int above_contexts = AOMMIN(txs_wide, (blocks_wide - above_off));

        memset(above_dc_ctx, dc_val, above_contexts);
        memset(above_dc_ctx + above_contexts, 0, (txs_wide - above_contexts));
        memset(above_level_ctx, cul_level, above_contexts);
        memset(above_level_ctx + above_contexts, 0, (txs_wide - above_contexts));
    }
    else {
        memset(above_dc_ctx, dc_val, txs_wide);
        memset(above_level_ctx, cul_level, txs_wide);
    }

    if (pi->mb_to_bottom_edge < 0) {
        int plane_bsize = (pi->mi->sb_type == BLOCK_INVALID) ? BLOCK_INVALID :
            ss_size_lookup[pi->mi->sb_type][subx][suby];
        const int blocks_high = max_block_high(pi, plane_bsize, suby);
        const int left_contexts = AOMMIN(txs_high, (blocks_high - left_off));

        memset(left_dc_ctx, dc_val, left_contexts);
        memset(left_dc_ctx + left_contexts, 0, txs_high - left_contexts);
        memset(left_level_ctx, cul_level, left_contexts);
        memset(left_level_ctx + left_contexts, 0, txs_high - left_contexts);
    }
    else {
        memset(left_dc_ctx, dc_val, txs_high);
        memset(left_level_ctx, cul_level, txs_high);
    }
}

static INLINE int rec_eob_pos(const int eob_token, const int extra) {
    int eob = k_eob_group_start[eob_token];
    if (eob > 2)
        eob += extra;
    return eob;
}

static INLINE int get_lower_levels_ctx_2d(const uint8_t *levels,
    int coeff_idx, int bwl, TxSize tx_size)
{
    assert(coeff_idx > 0);
    int mag;
    levels = levels + get_padded_idx(coeff_idx, bwl);
    mag = AOMMIN(levels[1], 3);                                     // { 0, 1 }
    mag += AOMMIN(levels[(1 << bwl) + TX_PAD_HOR], 3);              // { 1, 0 }
    mag += AOMMIN(levels[(1 << bwl) + TX_PAD_HOR + 1], 3);          // { 1, 1 }
    mag += AOMMIN(levels[2], 3);                                    // { 0, 2 }
    mag += AOMMIN(levels[(2 << bwl) + (2 << TX_PAD_HOR_LOG2)], 3);  // { 2, 0 }

    const int ctx = AOMMIN((mag + 1) >> 1, 4);
    return ctx + av1_nz_map_ctx_offset[tx_size][coeff_idx];
}
static INLINE int get_lower_levels_ctx(const uint8_t *levels,
    int coeff_idx, int bwl, TxSize tx_size, TxClass tx_class)
{
    const int stats =
        get_nz_mag(levels + get_padded_idx(coeff_idx, bwl), bwl, tx_class);
    return get_nz_map_ctx_from_stats(stats, coeff_idx, bwl, tx_size, tx_class);
}

static int read_golomb(SvtReader *r) {
    int x = 1;
    int length = 0;
    int i = 0;

    while (!i) {
        i = svt_read_bit(r, ACCT_STR);
        ++length;
        if (length > 20) {
            printf("Invalid length in read_golomb");
            break;
        }
    }

    for (i = 0; i < length - 1; ++i) {
        x <<= 1;
        x += svt_read_bit(r, ACCT_STR);
    }

    return x - 1;
}

static INLINE int get_br_ctx(const uint8_t *const levels,
    const int c,  // raster order
    const int bwl, const TxClass tx_class)
{
    const int row = c >> bwl;
    const int col = c - (row << bwl);
    const int stride = (1 << bwl) + TX_PAD_HOR;
    const int pos = row * stride + col;
    int mag = levels[pos + 1];
    mag += levels[pos + stride];
    switch (tx_class) {
    case TX_CLASS_2D:
        mag += levels[pos + stride + 1];
        mag = AOMMIN((mag + 1) >> 1, 6);
        if (c == 0) return mag;
        if ((row < 2) && (col < 2)) return mag + 7;
        break;
    case TX_CLASS_HORIZ:
        mag += levels[pos + 2];
        mag = AOMMIN((mag + 1) >> 1, 6);
        if (c == 0) return mag;
        if (col == 0) return mag + 7;
        break;
    case TX_CLASS_VERT:
        mag += levels[pos + (stride << 1)];
        mag = AOMMIN((mag + 1) >> 1, 6);
        if (c == 0) return mag;
        if (row == 0) return mag + 7;
        break;
    default: break;
    }

    return mag + 14;
}

static INLINE int get_br_ctx_2d(const uint8_t *const levels,
    const int c,  // raster order
    const int bwl)
{
    assert(c > 0);
    const int row = c >> bwl;
    const int col = c - (row << bwl);
    const int stride = (1 << bwl) + TX_PAD_HOR;
    const int pos = row * stride + col;
    int mag = AOMMIN(levels[pos + 1], MAX_BASE_BR_RANGE) +
        AOMMIN(levels[pos + stride], MAX_BASE_BR_RANGE) +
        AOMMIN(levels[pos + 1 + stride], MAX_BASE_BR_RANGE);
    mag = AOMMIN((mag + 1) >> 1, 6);
    if ((row | col) < 2) return mag + 7;
    return mag + 14;
}

static INLINE void read_coeffs_reverse_2d(SvtReader *r, TxSize tx_size,
    int start_si, int end_si, const int16_t *scan, int bwl,
    uint8_t *levels, base_cdf_arr base_cdf, br_cdf_arr br_cdf)
{
    for (int c = end_si; c >= start_si; --c) {
        const int pos = scan[c];
        const int coeff_ctx = get_lower_levels_ctx_2d(levels, pos, bwl, tx_size);
        const int nsymbs = 4;
        int level = svt_read_symbol(r, base_cdf[coeff_ctx], nsymbs, ACCT_STR);
        if (level > NUM_BASE_LEVELS) {
            const int br_ctx = get_br_ctx_2d(levels, pos, bwl);
            AomCdfProb *cdf = br_cdf[br_ctx];
            for (int idx = 0; idx < COEFF_BASE_RANGE; idx += BR_CDF_SIZE - 1) {
                const int k = svt_read_symbol(r, cdf, BR_CDF_SIZE, ACCT_STR);
                level += k;
                if (k < BR_CDF_SIZE - 1) break;
            }
        }
        levels[get_padded_idx(pos, bwl)] = level;
    }
}

static INLINE void read_coeffs_reverse(SvtReader *r, TxSize tx_size,
    TxClass tx_class, int start_si, int end_si, const int16_t *scan,
    int bwl, uint8_t *levels, base_cdf_arr base_cdf, br_cdf_arr br_cdf)
{
    for (int c = end_si; c >= start_si; --c) {
        const int pos = scan[c];
        const int coeff_ctx =
            get_lower_levels_ctx(levels, pos, bwl, tx_size, tx_class);
        const int nsymbs = 4;
        int level = svt_read_symbol(r, base_cdf[coeff_ctx], nsymbs, ACCT_STR);
        if (level > NUM_BASE_LEVELS) {
            const int br_ctx = get_br_ctx(levels, pos, bwl, tx_class);
            AomCdfProb *cdf = br_cdf[br_ctx];
            for (int idx = 0; idx < COEFF_BASE_RANGE; idx += BR_CDF_SIZE - 1) {
                const int k = svt_read_symbol(r, cdf, BR_CDF_SIZE, ACCT_STR);
                level += k;
                if (k < BR_CDF_SIZE - 1) break;
            }
        }
        levels[get_padded_idx(pos, bwl)] = level;
    }
}

uint16_t parse_coeffs(EbDecHandle *dec_handle, PartitionInfo_t *xd, SvtReader *r,
    uint32_t blk_row, uint32_t blk_col, int above_off, int left_off, int plane,
    int txb_skip_ctx, int dc_sign_ctx, TxSize tx_size, int32_t *coeff_buf,
    TransformInfo_t *trans_info)
{
    const int width = get_txb_wide(tx_size);
    const int height = get_txb_high(tx_size);

    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;
    FRAME_CONTEXT *frm_ctx = &parse_ctxt->cur_tile_ctx;

    TxSize txs_ctx = (TxSize)((txsize_sqr_map[tx_size] +
        txsize_sqr_up_map[tx_size] + 1) >> 1);
    PlaneType plane_type = (plane == 0) ? PLANE_TYPE_Y : PLANE_TYPE_UV;
    int cul_level = 0;
    int dc_val = 0;
    uint8_t levels_buf[TX_PAD_2D];
    uint8_t *const levels = set_levels(levels_buf, width);
    EbColorConfig *color_cfg = &dec_handle->seq_header.color_config;

    const int all_zero = svt_read_symbol(
        r, frm_ctx->txb_skip_cdf[txs_ctx][txb_skip_ctx], 2, ACCT_STR);

    const int bwl = get_txb_bwl(tx_size);

    uint16_t eob = 0;
    uint16_t max_scan_line = 0;
    if (all_zero) {
        if (plane == 0) {
            trans_info->txk_type = DCT_DCT;
            trans_info->cbf      = 0;
        }

        update_coeff_ctx(dec_handle, plane, xd, tx_size, blk_row, blk_col,
            above_off, left_off, cul_level, dc_val);

        return 0;
    }

    if (plane == AOM_PLANE_Y)
        parse_transform_type(dec_handle, xd, tx_size, r, trans_info);

    uint8_t     *lossless_array = &dec_handle->frame_header.lossless_array[0];
    TransformInfo_t *trans_buf = (dec_is_inter_block(xd->mi) && plane) ?
        parse_ctxt->inter_trans_chroma : trans_info;
    trans_info->txk_type = compute_tx_type(plane_type, xd, blk_row, blk_col,
        tx_size, dec_handle->frame_header.reduced_tx_set,
        color_cfg, lossless_array, trans_buf);
    const ScanOrder *scan_order = get_scan(tx_size, trans_info->txk_type);
    const int16_t *const scan = scan_order->scan;
    const int eob_multi_size = txsize_log2_minus4[tx_size];

    const TxClass tx_class = tx_type_to_class[trans_info->txk_type];
    const int eob_multi_ctx = (tx_class == TX_CLASS_2D) ? 0 : 1;

    int eob_extra = 0;
    int eob_pt = 1;

    switch (eob_multi_size) {
    case 0:
        eob_pt =
            svt_read_symbol(r, frm_ctx->eob_flag_cdf16[plane_type][eob_multi_ctx],
                5, ACCT_STR) + 1;
        break;
    case 1:
        eob_pt =
            svt_read_symbol(r, frm_ctx->eob_flag_cdf32[plane_type][eob_multi_ctx],
                6, ACCT_STR) + 1;
        break;
    case 2:
        eob_pt =
            svt_read_symbol(r, frm_ctx->eob_flag_cdf64[plane_type][eob_multi_ctx],
                7, ACCT_STR) + 1;
        break;
    case 3:
        eob_pt =
            svt_read_symbol(r, frm_ctx->eob_flag_cdf128[plane_type][eob_multi_ctx],
                8, ACCT_STR) + 1;
        break;
    case 4:
        eob_pt =
            svt_read_symbol(r, frm_ctx->eob_flag_cdf256[plane_type][eob_multi_ctx],
                9, ACCT_STR) + 1;
        break;
    case 5:
        eob_pt =
            svt_read_symbol(r, frm_ctx->eob_flag_cdf512[plane_type][eob_multi_ctx],
                10, ACCT_STR) + 1;
        break;
    default:
        eob_pt = svt_read_symbol(
            r, frm_ctx->eob_flag_cdf1024[plane_type][eob_multi_ctx], 11,
            ACCT_STR) + 1;
        break;
    }

    int eob_shift = k_eob_offset_bits[eob_pt];
    if (eob_shift > 0) {
        const int eob_ctx = eob_pt;
        int bit = svt_read_symbol(
            r, frm_ctx->eob_extra_cdf[txs_ctx][plane_type][eob_ctx], 2, ACCT_STR);
        if (bit)
            eob_extra += (1 << (eob_shift - 1));
        for (int i = 1; i < eob_shift; i++) {
            bit = svt_read_bit(r, ACCT_STR);
            if (bit)
                eob_extra += (1 << (eob_shift - 1 - i));
        }
    }
    eob = rec_eob_pos(eob_pt, eob_extra);

    if (eob > 1) {
        memset(levels_buf, 0,
            sizeof(*levels_buf) *
            ((width + TX_PAD_HOR) * (height + TX_PAD_VER) + TX_PAD_END));
    }

    int i = eob - 1;
    const int pos = scan[i];
    const int coeff_ctx = get_lower_levels_ctx_eob(bwl, height, i);
    const int nsymbs = 3;
    AomCdfProb *cdf =
        frm_ctx->coeff_base_eob_cdf[txs_ctx][plane_type][coeff_ctx];
    int level = svt_read_symbol(r, cdf, nsymbs, ACCT_STR) + 1;
    if (level > NUM_BASE_LEVELS) {
        const int br_ctx = get_br_ctx_eob(pos, bwl, tx_class);
        cdf = frm_ctx->coeff_br_cdf[AOMMIN(txs_ctx, TX_32X32)][plane_type][br_ctx];
        for (int idx = 0; idx < COEFF_BASE_RANGE/ (BR_CDF_SIZE - 1); idx ++) {
            int coeff_br = svt_read_symbol(r, cdf, BR_CDF_SIZE, ACCT_STR);
            level += coeff_br;
            if (coeff_br < BR_CDF_SIZE - 1) break;
        }
    }
    levels[get_padded_idx(pos, bwl)] = level;

    if (eob > 1) {
        base_cdf_arr base_cdf = frm_ctx->coeff_base_cdf[txs_ctx][plane_type];
        br_cdf_arr br_cdf =
            frm_ctx->coeff_br_cdf[AOMMIN(txs_ctx, TX_32X32)][plane_type];
        if (tx_class == TX_CLASS_2D) {
            read_coeffs_reverse_2d(r, tx_size, 1, eob - 1 - 1, scan, bwl, levels,
                base_cdf, br_cdf);
            read_coeffs_reverse(r, tx_size, tx_class, 0, 0, scan, bwl, levels,
                base_cdf, br_cdf);
        }
        else {
            read_coeffs_reverse(r, tx_size, tx_class, 0, eob - 1 - 1, scan, bwl,
                levels, base_cdf, br_cdf);
        }
    }
#if SVT_DEC_COEFF_DEBUG
    {
        int16_t    *cur_coeff = (int16_t *)coeff_buf;
        /* cur_coeff[0] used for debug */
        cur_coeff[1] = eob;
    }
#else
    coeff_buf[0] = eob;
#endif

    for (int c = 0; c < eob; ++c) {
        const int pos = scan[c];
        uint8_t sign = 0;
        TranLow level = levels[get_padded_idx(pos, bwl)];
        if (level) {
            max_scan_line = AOMMAX(max_scan_line, pos);
            if (c == 0) {
                sign = svt_read_symbol(r, frm_ctx->dc_sign_cdf[plane_type][dc_sign_ctx],
                    2, ACCT_STR);
            }
            else
                sign = svt_read_bit(r, ACCT_STR);
            if (level >= MAX_BASE_BR_RANGE)
                level += read_golomb(r);
            if (c == 0) dc_val = sign ? 1 : 2;

        level &= 0xfffff;
        cul_level += level;
        }
        coeff_buf[c + 1] = sign ? -level : level;
    }

    cul_level = AOMMIN(COEFF_CONTEXT_MASK, cul_level);

    update_coeff_ctx(dec_handle, plane, xd, tx_size, blk_row, blk_col,
        above_off, left_off, cul_level, dc_val);

    trans_info->cbf = 1; assert(eob);

    return eob;
}

void palette_tokens(int plane) {
    assert(plane == 0 || plane == 1);
    (void)plane;
    // TO-DO
}

void get_palette_color_context()
{
    //TO-DO
}

#define CHECK_BACKWARD_REFS(ref_frame) \
  (((ref_frame) >= BWDREF_FRAME) && ((ref_frame) <= ALTREF_FRAME))
#define IS_BACKWARD_REF_FRAME(ref_frame) CHECK_BACKWARD_REFS(ref_frame)

int get_reference_mode_context(const PartitionInfo_t *xd) {
    int ctx;
    const ModeInfo_t *const above_mbmi = xd->above_mbmi;
    const ModeInfo_t *const left_mbmi = xd->left_mbmi;
    const int has_above = xd->up_available;
    const int has_left = xd->left_available;

    // Note:
    // The mode info data structure has a one element border above and to the
    // left of the entries corresponding to real macroblocks.
    // The prediction flags in these dummy entries are initialized to 0.
    if (has_above && has_left) {  // both edges available
        if (!has_second_ref(above_mbmi) && !has_second_ref(left_mbmi))
            // neither edge uses comp pred (0/1)
            ctx = IS_BACKWARD_REF_FRAME(above_mbmi->ref_frame[0]) ^
            IS_BACKWARD_REF_FRAME(left_mbmi->ref_frame[0]);
        else if (!has_second_ref(above_mbmi))
            // one of two edges uses comp pred (2/3)
            ctx = 2 + (IS_BACKWARD_REF_FRAME(above_mbmi->ref_frame[0]) ||
                !dec_is_inter_block(above_mbmi));
        else if (!has_second_ref(left_mbmi))
            // one of two edges uses comp pred (2/3)
            ctx = 2 + (IS_BACKWARD_REF_FRAME(left_mbmi->ref_frame[0]) ||
                !dec_is_inter_block(left_mbmi));
        else  // both edges use comp pred (4)
            ctx = 4;
    }
    else if (has_above || has_left) {  // one edge available
        const ModeInfo_t *edge_mbmi = has_above ? above_mbmi : left_mbmi;

        if (!has_second_ref(edge_mbmi))
            // edge does not use comp pred (0/1)
            ctx = IS_BACKWARD_REF_FRAME(edge_mbmi->ref_frame[0]);
        else
            // edge uses comp pred (3)
            ctx = 3;
    }
    else {  // no edges available (1)
        ctx = 1;
    }
    assert(ctx >= 0 && ctx < COMP_INTER_CONTEXTS);
    return ctx;
}

int partition_plane_context(int mi_row,
    int mi_col, BlockSize bsize, ParseCtxt *parse_ctxt)
{
    const uint8_t *above_ctx = parse_ctxt->parse_nbr4x4_ctxt.above_part_wd + mi_col;
    const uint8_t *left_ctx =
        parse_ctxt->parse_nbr4x4_ctxt.left_part_ht +
        ((mi_row- parse_ctxt->sb_row_mi) & MAX_MIB_MASK);

    // Minimum partition point is 8x8. Offset the bsl accordingly.
    int bsl = mi_size_wide_log2[bsize] - mi_size_wide_log2[BLOCK_8X8];
    int above = (*above_ctx >> bsl) & 1, left = (*left_ctx >> bsl) & 1;

    assert(mi_size_wide_log2[bsize] == mi_size_high_log2[bsize]);
    assert(bsl >= 0);

    return (left * 2 + above) + bsl * PARTITION_PLOFFSET;
}

AomCdfProb cdf_element_prob(AomCdfProb *cdf, size_t element) {
    assert(cdf != NULL);
    return (element > 0 ? cdf[element - 1] : CDF_PROB_TOP) - cdf[element];
}

void partition_gather_horz_alike(AomCdfProb *out, AomCdfProb *in, BlockSize bsize) {
    (void)bsize;
    out[0] = CDF_PROB_TOP;
    out[0] -= cdf_element_prob(in, PARTITION_HORZ);
    out[0] -= cdf_element_prob(in, PARTITION_SPLIT);
    out[0] -= cdf_element_prob(in, PARTITION_HORZ_A);
    out[0] -= cdf_element_prob(in, PARTITION_HORZ_B);
    out[0] -= cdf_element_prob(in, PARTITION_VERT_A);
    if (bsize != BLOCK_128X128) out[0] -= cdf_element_prob(in, PARTITION_HORZ_4);
    out[0] = AOM_ICDF(out[0]);
    out[1] = AOM_ICDF(CDF_PROB_TOP);
}

void partition_gather_vert_alike(AomCdfProb *out, AomCdfProb *in, BlockSize bsize) {
    (void)bsize;
    out[0] = CDF_PROB_TOP;
    out[0] -= cdf_element_prob(in, PARTITION_VERT);
    out[0] -= cdf_element_prob(in, PARTITION_SPLIT);
    out[0] -= cdf_element_prob(in, PARTITION_HORZ_A);
    out[0] -= cdf_element_prob(in, PARTITION_VERT_A);
    out[0] -= cdf_element_prob(in, PARTITION_VERT_B);
    if (bsize != BLOCK_128X128) out[0] -= cdf_element_prob(in, PARTITION_VERT_4);
    out[0] = AOM_ICDF(out[0]);
    out[1] = AOM_ICDF(CDF_PROB_TOP);
}

PartitionType parse_partition_type(uint32_t blk_row, uint32_t blk_col, SvtReader *reader,
    BlockSize bsize, int has_rows, int has_cols, EbDecHandle *dec_handle)
{
    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle->pv_parse_ctxt;

    int partition_cdf_length = bsize <= BLOCK_8X8 ? PARTITION_TYPES :
        (bsize == BLOCK_128X128 ? EXT_PARTITION_TYPES - 2 : EXT_PARTITION_TYPES);
    int ctx = partition_plane_context(blk_row, blk_col, bsize, parse_ctxt);

    if (bsize < BLOCK_8X8) return PARTITION_NONE;
    else if (has_rows && has_cols)
    {
        return (PartitionType)svt_read_symbol(
            reader, parse_ctxt->cur_tile_ctx.partition_cdf[ctx],
            partition_cdf_length, ACCT_STR);
    }
    else if (has_cols)
    {
        assert(bsize > BLOCK_8X8);
        AomCdfProb cdf[2];
        partition_gather_vert_alike(cdf, parse_ctxt->cur_tile_ctx.
            partition_cdf[ctx], bsize);
        assert(cdf[1] == AOM_ICDF(CDF_PROB_TOP));
        return svt_read_cdf(reader, cdf, 2, ACCT_STR) ? PARTITION_SPLIT : PARTITION_HORZ;
    }
    else if (has_rows)
    {
        assert(has_rows && !has_cols);
        assert(bsize > BLOCK_8X8);
        AomCdfProb cdf[2];
        partition_gather_horz_alike(cdf, parse_ctxt->cur_tile_ctx.
            partition_cdf[ctx], bsize);
        assert(cdf[1] == AOM_ICDF(CDF_PROB_TOP));
        return svt_read_cdf(reader, cdf, 2, ACCT_STR) ? PARTITION_SPLIT : PARTITION_VERT;
    }
    return PARTITION_INVALID;
}

static INLINE void dec_get_txb_ctx(int plane_bsize, const TxSize tx_size,
    const int plane, int blk_row, int blk_col, EbDecHandle *dec_handle,
    TXB_CTX *const txb_ctx)
{
#define MAX_TX_SIZE_UNIT 16

    ParseCtxt *parse_ctx = (ParseCtxt*)dec_handle->pv_parse_ctxt;
    ParseNbr4x4Ctxt *nbr_ctx = &parse_ctx->parse_nbr4x4_ctxt;
    EbColorConfig *clr_cfg = &dec_handle->seq_header.color_config;
    int txb_w_unit = tx_size_wide_unit[tx_size];
    int txb_h_unit = tx_size_high_unit[tx_size];
    uint8_t suby   = plane ? clr_cfg->subsampling_y : 0;

    int dc_sign = 0;
    int k = 0;
    uint8_t *above_dc_ctx = nbr_ctx->above_dc_ctx[plane] + blk_col;
    uint8_t *left_dc_ctx = nbr_ctx->left_dc_ctx[plane] +
        (blk_row - (parse_ctx->sb_row_mi>> suby));

    do {
        const unsigned int sign = above_dc_ctx[k];
        if (sign == 1)
            dc_sign--;
        else if (sign == 2)
            dc_sign++;
    } while (++k < txb_w_unit);

    k = 0;
    do {
        const unsigned int sign = left_dc_ctx[k];
        if (sign == 1)
            dc_sign--;
        else if (sign == 2)
            dc_sign++;
    } while (++k < txb_h_unit);

    if (dc_sign < 0)
        txb_ctx->dc_sign_ctx = 1;
    else if (dc_sign > 0)
        txb_ctx->dc_sign_ctx = 2;
    else
        txb_ctx->dc_sign_ctx = 0;

    uint8_t *above_level_ctx = nbr_ctx->above_level_ctx[plane] + blk_col;
    uint8_t *left_level_ctx = nbr_ctx->left_level_ctx[plane] +
        (blk_row - (parse_ctx->sb_row_mi >> suby));

    if (plane == 0) {
        if (plane_bsize == txsize_to_bsize[tx_size])
            txb_ctx->txb_skip_ctx = 0;
        else {
            static const uint8_t skip_contexts[5][5] = { { 1, 2, 2, 2, 3 },
                                                         { 1, 4, 4, 4, 5 },
                                                         { 1, 4, 4, 4, 5 },
                                                         { 1, 4, 4, 4, 5 },
                                                         { 1, 4, 4, 4, 6 } };
            int top = 0;
            int left = 0;

            k = 0;
            do {
                top |= above_level_ctx[k];
            } while (++k < txb_w_unit);

            k = 0;
            do {
                left |= left_level_ctx[k];
            } while (++k < txb_h_unit);
            const int max = AOMMIN(top | left, 4);
            const int min = AOMMIN(AOMMIN(top, left), 4);

            txb_ctx->txb_skip_ctx = skip_contexts[min][max];
        }
    }
    else {
        int above = 0, left = 0;
        k = 0;
        do {
                above |= above_level_ctx[k];
                above |= above_dc_ctx[k];
        } while (++k < txb_w_unit);

        k = 0;
        do {
            left |= left_level_ctx[k];
            left |= left_dc_ctx[k];
        } while (++k < txb_h_unit);

        const int ctx_offset = (num_pels_log2_lookup[plane_bsize] >
            num_pels_log2_lookup[txsize_to_bsize[tx_size]])
            ? 10
            : 7;
        int ctx = (above != 0) + (left != 0);
        txb_ctx->txb_skip_ctx = ctx + ctx_offset;
    }
#undef MAX_TX_SIZE_UNIT
}

uint16_t parse_transform_block(EbDecHandle *dec_handle,
    PartitionInfo_t *pi, SvtReader *r, int32_t *coeff,
    TransformInfo_t *trans_info, int plane, int blk_col,
    int blk_row, BlockSize mi_row, BlockSize mi_col,
    TxSize tx_size, int skip)
{
    uint16_t eob = 0 , sub_x, sub_y;
    BlockSize bsize = pi->mi->sb_type;

    sub_x = (plane > 0) ? dec_handle->seq_header.color_config.subsampling_x : 0;
    sub_y = (plane > 0) ? dec_handle->seq_header.color_config.subsampling_y : 0;

    uint32_t start_x = (mi_col >> sub_x) + blk_col;
    uint32_t start_y = (mi_row >> sub_y) + blk_row;

    if ( start_x >= (dec_handle->frame_header.mi_cols >> sub_x) ||
         start_y >= (dec_handle->frame_header.mi_rows >> sub_y) )
        return eob;

    if (!skip) {
        int plane_bsize = (bsize == BLOCK_INVALID) ? BLOCK_INVALID :
            ss_size_lookup[bsize][sub_x][sub_y];
        TXB_CTX txb_ctx;
        dec_get_txb_ctx(plane_bsize, tx_size, plane,
                    start_y, start_x,
                    dec_handle, &txb_ctx);

        eob = parse_coeffs(dec_handle, pi, r, start_y, start_x, blk_col,
            blk_row, plane, txb_ctx.txb_skip_ctx, txb_ctx.dc_sign_ctx,
            tx_size, coeff, trans_info);
    }
    else{
        update_coeff_ctx(dec_handle, plane, pi, tx_size,
            start_y, start_x, blk_col, blk_row, 0, 0);
    }
    return eob;
}

/* Gives the pointer to current block's transform info. Will work only for Intra */
static INLINE TransformInfo_t* get_cur_trans_info_intra(int plane,
    SBInfo  *sb_info, ModeInfo_t *mi)
{
    return (plane == 0) ? (sb_info->sb_luma_trans_info + mi->first_luma_tu_offset) :
                          (sb_info->sb_chroma_trans_info +
                           mi->first_chroma_tu_offset + (plane-1));
}

/* TODO: Clean the logic! */
static INLINE TxSize av1_get_tx_size(int plane, PartitionInfo_t *pi,
    int sub_x, int sub_y)
{
    /*const */ModeInfo_t *mbmi = pi->mi;
    if (plane == 0) return (get_cur_trans_info_intra(plane, pi->sb_info,mbmi)->tx_size);
    return av1_get_max_uv_txsize(mbmi->sb_type, sub_x, sub_y);
}

void parse_residual(EbDecHandle *dec_handle, PartitionInfo_t *pi, SvtReader *r,
                    int mi_row, int mi_col, BlockSize mi_size)
{
    ParseCtxt   *parse_ctx = (ParseCtxt*)dec_handle->pv_parse_ctxt;
    TxSize      tx_size;
    EbColorConfig *color_info = &dec_handle->seq_header.color_config;
    int num_planes = color_info->mono_chrome ? 1 : MAX_MB_PLANE;
    ModeInfo_t *mode = pi->mi;

    int skip     = mode->skip;
    int lossless = (&dec_handle->frame_header.lossless_array[0])[mode->segment_id];

    const int max_blocks_wide = max_block_wide(pi, mi_size, 0);
    const int max_blocks_high = max_block_high(pi, mi_size, 0);
    const BlockSize max_unit_bsize = BLOCK_64X64;
    int mu_blocks_wide = block_size_wide[max_unit_bsize] >> tx_size_wide_log2[0];
    int mu_blocks_high = block_size_high[max_unit_bsize] >> tx_size_high_log2[0];
    mu_blocks_wide = AOMMIN(max_blocks_wide, mu_blocks_wide);
    mu_blocks_high = AOMMIN(max_blocks_high, mu_blocks_high);
    TransformInfo_t *trans_info = NULL, *next_trans_chroma = NULL,
        *next_trans_luma = NULL;

    for (int row = 0; row < max_blocks_high; row += mu_blocks_high) {
        for (int col = 0; col < max_blocks_wide; col += mu_blocks_wide) {
            for (int plane = 0; plane < num_planes; ++plane) {
                int sub_x = (plane > 0) ? color_info->subsampling_x : 0;
                int sub_y = (plane > 0) ? color_info->subsampling_y : 0;

                if (!dec_is_chroma_reference(mi_row, mi_col, mi_size, sub_x, sub_y))
                    continue;
                if (plane == 0 || !dec_is_inter_block(mode)) {
                    if (row == 0 && col == 0)
                        trans_info = get_cur_trans_info_intra(plane, pi->sb_info, mode);
                    else
                        trans_info = plane ? next_trans_chroma : next_trans_luma;
                }

                if(dec_is_inter_block(mode) && !plane)
                    parse_ctx->inter_trans_chroma = trans_info;


                if (lossless)
                    tx_size = TX_4X4;
                else
                    tx_size = av1_get_tx_size(plane, pi, sub_x, sub_y);

                int step_x = tx_size_high[tx_size] >> 2;
                int step_y = tx_size_wide[tx_size] >> 2;

                const int unit_height = ROUND_POWER_OF_TWO(
                    AOMMIN(mu_blocks_high + row, max_blocks_high), sub_y);
                const int unit_width = ROUND_POWER_OF_TWO(
                    AOMMIN(mu_blocks_wide + col, max_blocks_wide), sub_x);

                for (int blk_row = row >> sub_y; blk_row < unit_height;
                    blk_row += step_x)
                {
                    for (int blk_col = col >> sub_x; blk_col < unit_width;
                        blk_col += step_y)
                    {
                        int32_t *coeff = plane ? parse_ctx->cur_chroma_coeff_buf :
                                                    parse_ctx->cur_luma_coeff_buf;
#if SVT_DEC_COEFF_DEBUG
                        {
                        uint8_t  *cur_coeff = (uint8_t*)coeff;
                        uint8_t  cur_loc = (mi_row + blk_row) & 0xFF;
                        cur_coeff[0] = cur_loc;
                        cur_loc = (mi_col + blk_col) & 0xFF;
                        cur_coeff[1] = cur_loc;
                        }
#endif
                        int32_t eob = parse_transform_block(dec_handle, pi, r, coeff,
                            trans_info, plane,
                            blk_col, blk_row, mi_row, mi_col,
                            tx_size, skip);

                        if (eob != 0) {
                            plane ? (parse_ctx->cur_chroma_coeff_buf += (eob + 1))
                                  : (parse_ctx->cur_luma_coeff_buf += (eob + 1));
                            trans_info->cbf = 1;
                            }
                        else
                            trans_info->cbf = 0;

                        // increment transform pointer
                        trans_info++;
                    } //for blk_col
                } //for blk_row

                /* Remembers trans_info for next transform block
                   within a block of 128xH / Wx128 */
                if (plane == 0)
                    next_trans_luma = trans_info;
                else
                    next_trans_chroma = trans_info;
            }//for plane
        }
    }//intra/inter
}

void parse_block(EbDecHandle *dec_handle, uint32_t mi_row, uint32_t mi_col,
    SvtReader *r, BlockSize subsize, TileInfo *tile, SBInfo *sb_info,
    PartitionType partition)
{
    ParseCtxt *parse_ctx = (ParseCtxt*)dec_handle->pv_parse_ctxt;

    ModeInfo_t *mode = parse_ctx->cur_mode_info;

    int8_t      *cdef_strength = sb_info->sb_cdef_strength;

    int bw4 = mi_size_wide[subsize];
    int bh4 = mi_size_high[subsize];

    uint32_t mi_cols = (&dec_handle->frame_header)->mi_cols;
    uint32_t mi_rows = (&dec_handle->frame_header)->mi_rows;

    /* TODO: Can move to a common init fun for parse & decode */
    PartitionInfo_t part_info;
    part_info.mi        = mode;
    part_info.sb_info   = sb_info;
    part_info.mi_row = mi_row;
    part_info.mi_col = mi_col;

    mode->partition = partition;
    /* TU offset update from parse ctxt info of previous block */
    mode->first_luma_tu_offset  = parse_ctx->first_luma_tu_offset;
    mode->first_chroma_tu_offset= parse_ctx->first_chroma_tu_offset;
#if MODE_INFO_DBG
    mode->mi_row = mi_row;
    mode->mi_col = mi_col;
#endif
    EbColorConfig color_config = dec_handle->seq_header.color_config;
    if (bh4 == 1 && color_config.subsampling_y && (mi_row & 1) == 0)
        part_info.has_chroma = 0;
    else if (bw4 == 1 && color_config.subsampling_x && (mi_col & 1) == 0)
        part_info.has_chroma = 0;
    else
        part_info.has_chroma = color_config.mono_chrome ? 1: MAX_MB_PLANE;

    /* TODO : tile->tile_rows boundary condn check is wrong */
    part_info.up_available = ((int32_t)mi_row > tile->mi_row_start);
    part_info.left_available = ((int32_t)mi_col > tile->mi_col_start);
    part_info.chroma_up_available = part_info.up_available;
    part_info.chroma_left_available = part_info.left_available;

    part_info.mb_to_right_edge = ((int32_t)mi_cols - bw4 - mi_col) * MI_SIZE * 8;
    part_info.mb_to_bottom_edge = ((int32_t)mi_rows - bh4 - mi_row) * MI_SIZE * 8;

    part_info.is_sec_rect = 0;
    if (bw4 < bh4) {
        if (!((mi_col + bw4) & (bh4 - 1))) part_info.is_sec_rect = 1;
    }

    if (bw4 > bh4)
        if (mi_row & (bw4 - 1)) part_info.is_sec_rect = 1;

    if (part_info.has_chroma)
    {
        if (bh4 == 1 && color_config.subsampling_y) {
            part_info.chroma_up_available =
                (int32_t)(mi_row - 1) > tile->mi_row_start;
        }
        if (bw4 == 1 && color_config.subsampling_x) {
            part_info.chroma_left_available =
                (int32_t)(mi_col - 1) > tile->mi_col_start;
        }
    }
    else
    {
        part_info.chroma_up_available = 0;
        part_info.chroma_left_available = 0;
    }

    if (part_info.up_available)
        part_info.above_mbmi = get_top_mode_info(dec_handle, mi_row, mi_col, sb_info);
    else
        part_info.above_mbmi = NULL;
    if (part_info.left_available)
        part_info.left_mbmi = get_left_mode_info(dec_handle, mi_row, mi_col, sb_info);
    else
        part_info.left_mbmi = NULL;
    mode->sb_type = subsize;
    mode_info(dec_handle, &part_info, mi_row, mi_col, r, cdef_strength);

    /* Replicating same chroma mode for block pairs or 4x4 blks
       when chroma is present in last block*/
    if(0 == parse_ctx->prev_blk_has_chroma) {
        /* if the previous block does not have chroma info then         */
        /* current uv mode is stores in the previous ModeInfo structre. */
        /* this is done to simplify neighbour access for mode deviation */
        if (mode->sb_type != BLOCK_4X4) {
            //2 partition case
            mode[-1].uv_mode = mode->uv_mode;
            assert(part_info.has_chroma != 0);
        }
        else {
            /* wait unitl the last 4x4 block is parsed */
            if (part_info.has_chroma) {
                //4 partition case
                mode[-1].uv_mode = mode->uv_mode;
                mode[-2].uv_mode = mode->uv_mode;
                mode[-3].uv_mode = mode->uv_mode;
            }
        }
    }

    /* current block's has_chroma info is stored for useage in next block */
    parse_ctx->prev_blk_has_chroma = part_info.has_chroma;

    if (!dec_is_inter_block(mode)) {
        for (int plane = 0;
            plane < AOMMIN(2, dec_handle->seq_header.
                            color_config.mono_chrome ? 1 : MAX_MB_PLANE);
            ++plane) {
            palette_tokens(plane);
        }
    }

    read_block_tx_size(dec_handle, r, &part_info, subsize);

    parse_residual(dec_handle, &part_info, r, mi_row, mi_col, subsize);

    /* Update block level MI map */
    update_block_nbrs(dec_handle, mi_row, mi_col, subsize);
    parse_ctx->cur_mode_info_cnt++;
    parse_ctx->cur_mode_info++;
}

static INLINE void update_partition_context(ParseCtxt *parse_ctx,
    int mi_row, int mi_col, BlockSize subsize, BlockSize bsize)
{
    ParseNbr4x4Ctxt *ngr_ctx = &parse_ctx->parse_nbr4x4_ctxt;
    uint8_t *const above_ctx = ngr_ctx->above_part_wd + mi_col;
    uint8_t *const left_ctx = ngr_ctx->left_part_ht +
        ((mi_row- parse_ctx->sb_row_mi) & MAX_MIB_MASK);

    const int bw = mi_size_wide[bsize];
    const int bh = mi_size_high[bsize];
    memset(above_ctx, partition_context_lookup[subsize].above, bw);
    memset(left_ctx, partition_context_lookup[subsize].left, bh);
}

static INLINE void update_ext_partition_context(ParseCtxt *parse_ctx, int mi_row,
    int mi_col, BlockSize subsize, BlockSize bsize, PartitionType partition)
{
    if (bsize >= BLOCK_8X8) {
        const int hbs = mi_size_wide[bsize] / 2;
        BlockSize bsize2 = Partition_Subsize[PARTITION_SPLIT][bsize];
        switch (partition) {
        case PARTITION_SPLIT:
            if (bsize != BLOCK_8X8) break;
        case PARTITION_NONE:
        case PARTITION_HORZ:
        case PARTITION_VERT:
        case PARTITION_HORZ_4:
        case PARTITION_VERT_4:
            update_partition_context(parse_ctx, mi_row, mi_col, subsize, bsize);
            break;
        case PARTITION_HORZ_A:
            update_partition_context(parse_ctx, mi_row, mi_col, bsize2, subsize);
            update_partition_context(parse_ctx, mi_row + hbs, mi_col, subsize, subsize);
            break;
        case PARTITION_HORZ_B:
            update_partition_context(parse_ctx, mi_row, mi_col, subsize, subsize);
            update_partition_context(parse_ctx, mi_row + hbs, mi_col, bsize2, subsize);
            break;
        case PARTITION_VERT_A:
            update_partition_context(parse_ctx, mi_row, mi_col, bsize2, subsize);
            update_partition_context(parse_ctx, mi_row, mi_col + hbs, subsize, subsize);
            break;
        case PARTITION_VERT_B:
            update_partition_context(parse_ctx, mi_row, mi_col, subsize, subsize);
            update_partition_context(parse_ctx, mi_row, mi_col + hbs, bsize2, subsize);
            break;
        default: assert(0 && "Invalid partition type");
        }
    }
}

void parse_partition(EbDecHandle *dec_handle, uint32_t blk_row,
    uint32_t blk_col, SvtReader *reader, BlockSize bsize, SBInfo *sb_info)
{
    ParseCtxt *parse_ctx = (ParseCtxt*)dec_handle->pv_parse_ctxt;

    if (blk_row >= dec_handle->frame_header.mi_rows ||
        blk_col >= dec_handle->frame_header.mi_cols)
        return;

    int num4x4 = mi_size_wide[bsize];
    int half_block_4x4 = num4x4 >> 1;
    int quarter_block_4x4 = half_block_4x4 >> 1;

    int has_rows = (blk_row + half_block_4x4) < dec_handle->frame_header.mi_rows;
    int has_cols = (blk_col + half_block_4x4) < dec_handle->frame_header.mi_cols;

    PartitionType partition;

    partition = (bsize < BLOCK_8X8) ? PARTITION_NONE
        : parse_partition_type(blk_row, blk_col, reader, bsize,
            has_rows, has_cols, dec_handle);
    int subSize = Partition_Subsize[(int)partition][bsize];
    int splitSize = Partition_Subsize[PARTITION_SPLIT][bsize];

#define PARSE_BLOCK(db_r, db_c, db_subsize)                 \
parse_block(dec_handle, db_r, db_c, reader, db_subsize,     \
    &parse_ctx->cur_tile_info, sb_info, partition);

#define PARSE_PARTITION(db_r, db_c, db_subsize)                 \
  parse_partition(dec_handle, (db_r), (db_c), reader,           \
                   (db_subsize), sb_info)

    switch ((int)partition) {
    case PARTITION_NONE: PARSE_BLOCK(blk_row, blk_col, subSize); break;
    case PARTITION_HORZ:
        PARSE_BLOCK(blk_row, blk_col, subSize);
        if (has_rows) PARSE_BLOCK(blk_row + half_block_4x4, blk_col, subSize);
        break;
    case PARTITION_VERT:
        PARSE_BLOCK(blk_row, blk_col, subSize);
        if (has_cols) PARSE_BLOCK(blk_row, blk_col + half_block_4x4, subSize);
        break;
    case PARTITION_SPLIT:
        PARSE_PARTITION(blk_row, blk_col, subSize);
        PARSE_PARTITION(blk_row, blk_col + half_block_4x4, subSize);
        PARSE_PARTITION(blk_row + half_block_4x4, blk_col, subSize);
        PARSE_PARTITION(blk_row + half_block_4x4, blk_col + half_block_4x4, subSize);
        break;
    case PARTITION_HORZ_A:
        PARSE_BLOCK(blk_row, blk_col, splitSize);
        PARSE_BLOCK(blk_row, blk_col + half_block_4x4, splitSize);
        PARSE_BLOCK(blk_row + half_block_4x4, blk_col, subSize);
        break;
    case PARTITION_HORZ_B:
        PARSE_BLOCK(blk_row, blk_col, subSize);
        PARSE_BLOCK(blk_row + half_block_4x4, blk_col, splitSize);
        PARSE_BLOCK(blk_row + half_block_4x4, blk_col + half_block_4x4, splitSize);
        break;
    case PARTITION_VERT_A:
        PARSE_BLOCK(blk_row, blk_col, splitSize);
        PARSE_BLOCK(blk_row + half_block_4x4, blk_col, splitSize);
        PARSE_BLOCK(blk_row, blk_col + half_block_4x4, subSize);
        break;
    case PARTITION_VERT_B:
        PARSE_BLOCK(blk_row, blk_col, subSize);
        PARSE_BLOCK(blk_row, blk_col + half_block_4x4, splitSize);
        PARSE_BLOCK(blk_row + half_block_4x4, blk_col + half_block_4x4, splitSize);
        break;
    case PARTITION_HORZ_4:
        for (int i = 0; i < 4; ++i) {
            uint32_t this_blk_row = blk_row + (uint32_t)(i * quarter_block_4x4);
            if (i > 0 && this_blk_row >= dec_handle->frame_header.mi_rows) break;
            PARSE_BLOCK(this_blk_row, blk_col, subSize);
        }
        break;
    case PARTITION_VERT_4:
        for (int i = 0; i < 4; ++i) {
            uint32_t this_blk_col = blk_col + (uint32_t)(i * quarter_block_4x4);
            if (i > 0 && this_blk_col >= dec_handle->frame_header.mi_cols) break;
            PARSE_BLOCK(blk_row, this_blk_col, subSize);
        }
        break;
    default: assert(0 && "Invalid partition type");
    }
    update_ext_partition_context(parse_ctx, blk_row, blk_col,
        subSize, bsize, partition);
}

void parse_super_block(EbDecHandle *dec_handle,
    uint32_t blk_row, uint32_t blk_col, SBInfo *sbInfo)
{
    ParseCtxt *parse_ctx = (ParseCtxt*)dec_handle->pv_parse_ctxt;
    SvtReader *reader = &parse_ctx->r;
#if ENABLE_ENTROPY_TRACE
    enable_dump =  1;
#endif
    parse_partition(dec_handle, blk_row, blk_col, reader,
        dec_handle->seq_header.sb_size, sbInfo);
}
