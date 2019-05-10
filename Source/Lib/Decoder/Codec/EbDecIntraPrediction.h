/*
* Copyright(c) 2019 Netflix, Inc.
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

#ifndef EbDecIntraPrediction_h
#define EbDecIntraPrediction_h

#ifdef __cplusplus
extern "C" {
#endif

// Do we need to save the luma pixels from the current block,
// for a possible future CfL prediction?
INLINE CFL_ALLOWED_TYPE store_cfl_required(const EbColorConfig *cc,
                                           const PartitionInfo_t  *xd,
                                            CflCtx *cfl_ctx)
{
    const ModeInfo_t *mbmi = xd->mi;

    if (cc->mono_chrome) return CFL_DISALLOWED;

    if (!xd->has_chroma) {
        // For non-chroma-reference blocks, we should always store the luma pixels,
        // in case the corresponding chroma-reference block uses CfL.
        // Note that this can only happen for block sizes which are <8 on
        // their shortest side, as otherwise they would be chroma reference
        // blocks.
        return CFL_ALLOWED;
    }

    // If this block has chroma information, we know whether we're
    // actually going to perform a CfL prediction
    return (CFL_ALLOWED_TYPE)(!dec_is_inter_block(mbmi) &&
        mbmi->uv_mode == UV_CFL_PRED);
}

void svt_av1_predict_intra(DecModCtxt *dec_mod_ctxt, PartitionInfo_t *part_info,
        int32_t plane, int32_t blk_mi_col, int32_t blk_mi_row,
        TxSize tx_size, TileInfo *td,
        uint8_t *blk_recon_buf, int32_t recon_strd,
        EB_BITDEPTH bit_depth, int32_t blk_mi_col_off, int32_t blk_mi_row_off);

void cfl_store_tx(PartitionInfo_t *xd, CflCtx *cfl_ctx, int row, int col, TxSize tx_size,
    block_size  bsize, EbColorConfig *cc, uint8_t *dst_buff,
    uint32_t dst_stride);

#ifdef __cplusplus
    }
#endif
#endif // EbDecIntraPrediction_h