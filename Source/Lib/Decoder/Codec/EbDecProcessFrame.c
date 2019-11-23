/*
* Copyright(c) 2019 Netflix, Inc.
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

// SUMMARY
//   Contains the Decode related functions

/**************************************
 * Includes
 **************************************/

#include "EbDefinitions.h"
#include "EbPictureBufferDesc.h"

#include "EbSvtAv1Dec.h"
#include "EbDecHandle.h"

#include "EbDecInverseQuantize.h"
#include "EbDecProcessFrame.h"
#include "EbDecProcessBlock.h"
#include "EbDecNbr.h"

/* decode partition */
static void decode_partition(DecModCtxt *dec_mod_ctxt,
                             uint32_t mi_row, uint32_t mi_col,
                             BlockSize bsize, SBInfo *sb_info)
{
    BlockSize  subsize;
    PartitionType partition;

    uint8_t num4x4 = mi_size_wide[bsize];
    uint32_t half_block_4x4 =(uint32_t)num4x4 >> 1;
    uint32_t quarter_block_4x4 = half_block_4x4 >> 1;

    uint32_t has_rows = (mi_row + half_block_4x4) < dec_mod_ctxt->frame_header->mi_rows;
    uint32_t has_cols = (mi_col + half_block_4x4) < dec_mod_ctxt->frame_header->mi_cols;

    if (mi_row >= dec_mod_ctxt->frame_header->mi_rows ||
        mi_col >= dec_mod_ctxt->frame_header->mi_cols) return;

    partition = get_partition(dec_mod_ctxt, dec_mod_ctxt->frame_header,
                              mi_row, mi_col, sb_info, bsize);

    subsize = Partition_Subsize[partition][bsize];
    BlockSize splitSize = Partition_Subsize[PARTITION_SPLIT][bsize];

#define DECODE_BLOCK(db_r, db_c, db_subsize)                \
decode_block(dec_mod_ctxt, db_r, db_c, db_subsize,          \
    &dec_mod_ctxt->cur_tile_info, sb_info)

#define DECODE_PARTITION(db_r, db_c, db_subsize)            \
decode_partition(dec_mod_ctxt, (db_r), (db_c),              \
                   (db_subsize), sb_info)

    switch ((int)partition) {
        case PARTITION_NONE:
            DECODE_BLOCK(mi_row, mi_col, subsize);
            break;
        case PARTITION_HORZ:
            DECODE_BLOCK(mi_row, mi_col, subsize);
            if (has_rows)
                DECODE_BLOCK(mi_row + half_block_4x4,
                             mi_col, subsize);
            break;
        case PARTITION_VERT:
            DECODE_BLOCK(mi_row, mi_col, subsize);
            if (has_cols)
                DECODE_BLOCK(mi_row, mi_col + half_block_4x4, subsize);
            break;
        case PARTITION_SPLIT:
            DECODE_PARTITION(mi_row, mi_col, subsize);
            DECODE_PARTITION(mi_row, mi_col + half_block_4x4, subsize);
            DECODE_PARTITION(mi_row + half_block_4x4, mi_col, subsize);
            DECODE_PARTITION(mi_row + half_block_4x4, mi_col + half_block_4x4, subsize);
            break;
        case PARTITION_HORZ_A:
            DECODE_BLOCK(mi_row, mi_col, splitSize);
            DECODE_BLOCK(mi_row, mi_col + half_block_4x4, splitSize);
            DECODE_BLOCK(mi_row + half_block_4x4, mi_col, subsize);
            break;
        case PARTITION_HORZ_B:
            DECODE_BLOCK(mi_row, mi_col, subsize);
            DECODE_BLOCK(mi_row + half_block_4x4, mi_col, splitSize);
            DECODE_BLOCK(mi_row + half_block_4x4, mi_col + half_block_4x4, splitSize);
            break;
        case PARTITION_VERT_A:
            DECODE_BLOCK(mi_row, mi_col, splitSize);
            DECODE_BLOCK(mi_row + half_block_4x4, mi_col, splitSize);
            DECODE_BLOCK(mi_row, mi_col + half_block_4x4, subsize);
            break;
        case PARTITION_VERT_B:
            DECODE_BLOCK(mi_row, mi_col, subsize);
            DECODE_BLOCK(mi_row, mi_col + half_block_4x4, splitSize);
            DECODE_BLOCK(mi_row + half_block_4x4, mi_col + half_block_4x4, splitSize);
            break;
        case PARTITION_HORZ_4:
            for (int i = 0; i < 4; ++i) {
                uint32_t this_mi_row = mi_row +  (i * quarter_block_4x4);
                if (i > 0 && this_mi_row >= dec_mod_ctxt->frame_header->mi_rows) break;
                DECODE_BLOCK(this_mi_row, mi_col, subsize);
                }
            break;
        case PARTITION_VERT_4:
            for (int i = 0; i < 4; ++i) {
                uint32_t this_mi_col = mi_col + (i * quarter_block_4x4);
                if (i > 0 && this_mi_col >= dec_mod_ctxt->frame_header->mi_cols) break;
                DECODE_BLOCK(mi_row, this_mi_col, subsize);
                }
            break;
        default: assert(0 && "Invalid partition type");
    }
}

// decoding of the superblock
void decode_super_block(DecModCtxt *dec_mod_ctxt,
                        uint32_t mi_row, uint32_t mi_col,
                        SBInfo *sb_info)
{
    EbDecHandle *dec_handle = (EbDecHandle *)dec_mod_ctxt->dec_handle_ptr;
    SeqHeader *seq = dec_mod_ctxt->seq_header;

    /* Pointer updates */
    bool do_memset = true;
    int left_available = (mi_col > (uint32_t)dec_mod_ctxt->cur_tile_info.mi_col_start);
    if (left_available) {
        BlockModeInfo *left_mode = get_left_mode_info(dec_handle, mi_row, mi_col, sb_info);
        if (left_mode->skip && left_mode->sb_type == seq->sb_size) {
            do_memset = false;
        }
    }

    if (do_memset) {
        EbColorConfig *color_config = &seq->color_config;
        int32_t sb_size_log2 = seq->sb_size_log2;

        int32_t y_size = (1 << sb_size_log2) * (1 << sb_size_log2);
        int32_t iq_size = y_size +
            (color_config->subsampling_x ? y_size >> 2 : y_size) +
            (color_config->subsampling_y ? y_size >> 2 : y_size);

        memset(dec_mod_ctxt->sb_iquant_ptr, 0, iq_size *
            sizeof(*dec_mod_ctxt->sb_iquant_ptr));
    }

    dec_mod_ctxt->iquant_cur_ptr = dec_mod_ctxt->sb_iquant_ptr;

    /* SB level dequant update */
    update_dequant(dec_mod_ctxt, sb_info);

    /* Decode partition */
    decode_partition(dec_mod_ctxt, mi_row, mi_col,
        dec_mod_ctxt->seq_header->sb_size, sb_info);
}

#if MT_SUPPORT

EbErrorType decode_tile_row(DecModCtxt *dec_mod_ctxt,
    TilesInfo *tile_info, int32_t tile_col, int32_t mi_row)
{
    EbErrorType status = EB_ErrorNone;
    EbDecHandle *dec_handle_ptr = (EbDecHandle*)(dec_mod_ctxt->dec_handle_ptr);
    MasterFrameBuf *master_frame_buf = &dec_handle_ptr->master_frame_buf;
    CurFrameBuf    *frame_buf = &master_frame_buf->cur_frame_bufs[0];
    int32_t sb_row = (mi_row << 2) >> dec_mod_ctxt->seq_header->sb_size_log2;
    for (uint32_t mi_col = tile_info->tile_col_start_mi[tile_col];
        mi_col < tile_info->tile_col_start_mi[tile_col + 1];
        mi_col += dec_mod_ctxt->seq_header->sb_mi_size)

    {
        int32_t sb_col = (mi_col << MI_SIZE_LOG2) >>
            dec_mod_ctxt->seq_header->sb_size_log2;

        SBInfo  *sb_info = frame_buf->sb_info +
            (sb_row * master_frame_buf->sb_cols) + sb_col;

        dec_mod_ctxt->cur_coeff[AOM_PLANE_Y] = sb_info->sb_coeff[AOM_PLANE_Y];
        dec_mod_ctxt->cur_coeff[AOM_PLANE_U] = sb_info->sb_coeff[AOM_PLANE_U];
        dec_mod_ctxt->cur_coeff[AOM_PLANE_V] = sb_info->sb_coeff[AOM_PLANE_V];

        decode_super_block(dec_mod_ctxt, mi_row, mi_col, sb_info);
    }
    return status;
}

EbErrorType decode_tile(DecModCtxt *dec_mod_ctxt,
    TilesInfo *tile_info, int32_t tile_row, int32_t tile_col)
{
    EbErrorType status = EB_ErrorNone;
    for (uint32_t mi_row = tile_info->tile_row_start_mi[tile_row];
        mi_row < tile_info->tile_row_start_mi[tile_row + 1];
        mi_row += dec_mod_ctxt->seq_header->sb_mi_size)
    {
        EbColorConfig *color_config = &dec_mod_ctxt->seq_header->color_config;
        cfl_init(&dec_mod_ctxt->cfl_ctx, color_config);
        status = decode_tile_row(dec_mod_ctxt, tile_info, tile_col, mi_row);
    }
    return status;
}

EbErrorType start_decode_tile(EbDecHandle *dec_handle_ptr,
    DecModCtxt *dec_mod_ctxt, TilesInfo *tiles_info, int32_t tile_num)
{
    EbErrorType status = EB_ErrorNone;
    dec_mod_ctxt->frame_header = &dec_handle_ptr->frame_header;
    dec_mod_ctxt->seq_header = &dec_handle_ptr->seq_header;
    FrameHeader *frame_header = &dec_handle_ptr->frame_header;
    int32_t tile_row = tile_num / tiles_info->tile_cols;
    int32_t tile_col = tile_num % tiles_info->tile_cols;
    svt_tile_init(&dec_mod_ctxt->cur_tile_info, frame_header,
        tile_row, tile_col);
    status = decode_tile(dec_mod_ctxt, tiles_info, tile_row, tile_col);
    return status;
}
#endif
