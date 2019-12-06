/*
* Copyright(c) 2019 Netflix, Inc.
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

// SUMMARY
//   Contains the Decode MT process related functions

/**************************************
 * Includes
 **************************************/

#include "EbDefinitions.h"
#include "EbPictureBufferDesc.h"

#include "EbSvtAv1Dec.h"
#include "EbDecHandle.h"
#include "EbDecMemInit.h"

#include "EbObuParse.h"
#include "EbDecParseFrame.h"
#include "EbDecProcessFrame.h"
#include "EbDecLF.h"

#include "EbDecBitstream.h"
#include "EbTime.h"

#include "EbDecInverseQuantize.h"

#include <stdlib.h>

void* dec_all_stage_kernel(void *input_ptr);
/*ToDo : Remove all these replications */
void eb_av1_loop_filter_frame_init(FrameHeader *frm_hdr,
    LoopFilterInfoN *lfi, int32_t plane_start, int32_t plane_end);
void dec_loop_filter_row(
    EbDecHandle *dec_handle_ptr,
    EbPictureBufferDesc *recon_picture_buf, LFCtxt *lf_ctxt,
    LoopFilterInfoN *lf_info, uint32_t y_lcu_index,
    int32_t plane_start, int32_t plane_end);

EbErrorType DecDummyCtor(
    DecMTNode *context_ptr,
    EbPtr object_init_data_ptr)
{
    context_ptr->node_index = *(uint32_t *)object_init_data_ptr;

    return EB_ErrorNone;
}

EbErrorType DecDummyCreator(
    EbPtr *object_dbl_ptr,
    EbPtr object_init_data_ptr)
{
    DecMTNode* obj;

    *object_dbl_ptr = NULL;
    EB_NEW(obj, DecDummyCtor, object_init_data_ptr);
    *object_dbl_ptr = obj;

    return EB_ErrorNone;
}

/************************************
* System Resource Managers & Fifos
************************************/
EbErrorType DecSystemResourceInit(EbDecHandle *dec_handle_ptr,
                                    TilesInfo *tiles_info)
{
    EbErrorType return_error = EB_ErrorNone;
    DecMTFrameData  *dec_mt_frame_data =
        &dec_handle_ptr->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;

    int32_t num_tiles = tiles_info->tile_cols * tiles_info->tile_rows;

    assert(dec_handle_ptr->dec_config.threads > 1);

    /************************************
    * System Resource Managers & Fifos
    ************************************/
    dec_handle_ptr->start_thread_process = EB_FALSE;

    /* Parse Q */
    uint32_t    node_idx = 0;

    EB_NEW(dec_mt_frame_data->parse_tile_resource_ptr,
        eb_system_resource_ctor,
        num_tiles, /* object_total_count */
        1, /* producer procs cnt : 1 Q per cnt is created inside, so kept 1*/
        1, /* consumer prcos cnt : 1 Q per cnt is created inside, so kept 1*/
        &dec_mt_frame_data->parse_tile_producer_fifo_ptr, /* producer_fifo */
        &dec_mt_frame_data->parse_tile_consumer_fifo_ptr, /* consumer_fifo */
        EB_TRUE, /* Full Queue*/
        DecDummyCreator,
        &node_idx,
        NULL);

    /* Recon queue */
    EB_NEW(dec_mt_frame_data->recon_tile_resource_ptr,
        eb_system_resource_ctor,
        num_tiles, /* object_total_count */
        1, /* producer procs cnt : 1 Q per cnt is created inside, so kept 1*/
        1, /* consumer prcos cnt : 1 Q per cnt is created inside, so kept 1*/
        &dec_mt_frame_data->recon_tile_producer_fifo_ptr, /* producer_fifo */
        &dec_mt_frame_data->recon_tile_consumer_fifo_ptr, /* consumer_fifo */
        EB_TRUE, /* Full Queue*/
        DecDummyCreator,
        &node_idx,
        NULL);

    int32_t sb_size_h = block_size_high[dec_handle_ptr->seq_header.sb_size];
    uint32_t picture_height_in_sb =
        (dec_handle_ptr->seq_header.max_frame_height + sb_size_h - 1) / sb_size_h;

    EB_NEW(dec_mt_frame_data->lf_frame_info.lf_resource_ptr,
        eb_system_resource_ctor,
        picture_height_in_sb, /* object_total_count */
        1, /* producer procs cnt : 1 Q per cnt is created inside, so kept 1*/
        1, /* consumer prcos cnt : 1 Q per cnt is created inside, so kept 1*/
        &dec_mt_frame_data->lf_frame_info.lf_row_producer_fifo_ptr, /* producer_fifo */
        &dec_mt_frame_data->lf_frame_info.lf_row_consumer_fifo_ptr, /* consumer_fifo */
        EB_TRUE, /* Full Queue*/
        DecDummyCreator,
        &node_idx,
        NULL);

    /************************************
    * Contexts
    ************************************/
    EB_MALLOC_DEC(int32_t *, dec_mt_frame_data->lf_frame_info.
      sb_lf_completed_in_row, picture_height_in_sb * sizeof(int32_t), EB_N_PTR);

    dec_mt_frame_data->temp_mutex = eb_create_mutex();

    dec_mt_frame_data->start_parse_frame = EB_FALSE;

    dec_mt_frame_data->start_decode_frame = EB_FALSE;

    dec_mt_frame_data->start_lf_frame = EB_FALSE;
    dec_mt_frame_data->num_threads_lfed = 0;

    /************************************
    * Thread Handles
    ************************************/

    /* Decode Library Threads */
    /* ToDo: change this logic. Only for Parse */
    uint32_t num_lib_threads =
        (int32_t)dec_handle_ptr->dec_config.threads - 1;
    if (num_lib_threads > 0) {
        DecThreadCtxt *thread_ctxt_pa;

        EB_MALLOC_DEC(DecThreadCtxt *, thread_ctxt_pa,
            num_lib_threads * sizeof(DecThreadCtxt), EB_N_PTR);
        for (uint32_t i = 0; i < num_lib_threads; i++) {
            thread_ctxt_pa[i].thread_cnt     = i + 1;
            thread_ctxt_pa[i].dec_handle_ptr = dec_handle_ptr;
            init_dec_mod_ctxt(dec_handle_ptr, &thread_ctxt_pa[i].dec_mod_ctxt);
        }
        EB_CREATE_THREAD_ARRAY(dec_handle_ptr->decode_thread_handle_array,
            num_lib_threads, dec_all_stage_kernel, (void **)&thread_ctxt_pa);
    }

    EB_MALLOC_DEC(uint32_t *, dec_mt_frame_data->sb_recon_row_map,
        picture_height_in_sb  * tiles_info->tile_cols *
        sizeof(uint32_t), EB_N_PTR);
    return return_error;
}

void svt_av1_queue_parse_jobs(EbDecHandle *dec_handle_ptr,
                              TilesInfo   *tiles_info,
                              ObuHeader   *obu_header,
                              bitstrm_t   *bs,
                              uint32_t tg_start, uint32_t tg_end)
{
    size_t tile_size;
    DecMTFrameData  *dec_mt_frame_data =
        &dec_handle_ptr->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;
    EbObjectWrapper *parse_results_wrapper_ptr;

    MasterParseCtxt *master_parse_ctxt =
        (MasterParseCtxt *)dec_handle_ptr->pv_master_parse_ctxt;
    ParseTileData   *parse_tile_data = master_parse_ctxt->parse_tile_data;

    for (uint32_t tile_num = tg_start; tile_num <= tg_end; tile_num++) {

        if (tile_num == tg_end)
            tile_size = obu_header->payload_size;
        else {
            tile_size = dec_get_bits_le(bs, tiles_info->tile_size_bytes) + 1;
            obu_header->payload_size -= (tiles_info->tile_size_bytes + tile_size);
        }
        PRINT_FRAME("tile_size", (tile_size));

        // Assign to ParseCtxt
        parse_tile_data[tile_num].data      = get_bitsteam_buf(bs);
        parse_tile_data[tile_num].data_end  = bs->buf_max;
        parse_tile_data[tile_num].tile_size = tile_size;

        dec_bits_init(bs, (get_bitsteam_buf(bs) + tile_size), obu_header->payload_size);

        // Get Empty Parse Tile Job
        eb_get_empty_object(dec_mt_frame_data->parse_tile_producer_fifo_ptr[0],
                            &parse_results_wrapper_ptr);

        DecMTNode *context_ptr = (DecMTNode*)parse_results_wrapper_ptr->object_ptr;
        context_ptr->node_index = tile_num;

        // Post Parse Tile Job
        eb_post_full_object(parse_results_wrapper_ptr);
    }

    dec_handle_ptr->start_thread_process = EB_TRUE;
}

EbErrorType parse_tile_job(EbDecHandle *dec_handle_ptr, int32_t tile_num) {
    EbErrorType status = EB_ErrorNone;

    TilesInfo   *tiles_info = &dec_handle_ptr->frame_header.tiles_info;
    MasterParseCtxt *master_parse_ctxt =
                    (MasterParseCtxt *) dec_handle_ptr->pv_master_parse_ctxt;
    ParseCtxt *parse_ctxt = &master_parse_ctxt->tile_parse_ctxt[tile_num];

    parse_ctxt->seq_header = &dec_handle_ptr->seq_header;
    parse_ctxt->frame_header = &dec_handle_ptr->frame_header;

    parse_ctxt->parse_above_nbr4x4_ctxt = &master_parse_ctxt->
        parse_above_nbr4x4_ctxt[tile_num];
    parse_ctxt->parse_left_nbr4x4_ctxt = &master_parse_ctxt->
        parse_left_nbr4x4_ctxt[tile_num];

    start_parse_tile(dec_handle_ptr, parse_ctxt, tiles_info, tile_num, 1);

    return status;
}

void parse_frame_tiles(EbDecHandle *dec_handle_ptr) {
    DecMTFrameData  *dec_mt_frame_data =
        &dec_handle_ptr->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;
    EbObjectWrapper *parse_results_wrapper_ptr;
    DecMTNode *context_ptr;

    volatile EbBool *start_parse_frame = &dec_mt_frame_data->start_parse_frame;
    while (*start_parse_frame != EB_TRUE);

    while (1) {
        eb_get_full_object_non_blocking(dec_mt_frame_data->
            parse_tile_consumer_fifo_ptr[0],
            &parse_results_wrapper_ptr);

        if (NULL != parse_results_wrapper_ptr) {
            context_ptr = (DecMTNode*)parse_results_wrapper_ptr->object_ptr;

            if (EB_ErrorNone !=
                parse_tile_job(dec_handle_ptr, context_ptr->node_index))
            {
                printf("\nParse Issue for Tile %d", context_ptr->node_index);
                break;
            }

            EbObjectWrapper *recon_results_wrapper_ptr;
            // Get Empty Recon Tile Job
            eb_get_empty_object(dec_mt_frame_data->recon_tile_producer_fifo_ptr[0],
                &recon_results_wrapper_ptr);

            DecMTNode *recon_context_ptr =
                (DecMTNode*)recon_results_wrapper_ptr->object_ptr;
            recon_context_ptr->node_index = context_ptr->node_index;
            // Post Recon Tile Job
            eb_post_full_object(recon_results_wrapper_ptr);
            dec_mt_frame_data->start_decode_frame = EB_TRUE;
            // Release Parse Results
            eb_release_object(parse_results_wrapper_ptr);
        }
        else
            break;
    }
}

EbErrorType decode_tile_job(EbDecHandle *dec_handle_ptr,
    int32_t tile_num, DecModCtxt *dec_mod_ctxt)
{
    EbErrorType status = EB_ErrorNone;
    TilesInfo   *tiles_info = &dec_handle_ptr->frame_header.tiles_info;
    status = start_decode_tile(dec_handle_ptr, dec_mod_ctxt, tiles_info, tile_num);
    return status;
}

void decode_frame_tiles(EbDecHandle *dec_handle_ptr, DecThreadCtxt *thread_ctxt) {
    DecMTFrameData  *dec_mt_frame_data =
        &dec_handle_ptr->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;
    EbObjectWrapper *recon_results_wrapper_ptr;
    DecMTNode *context_ptr;

    volatile EbBool *start_decode_frame = &dec_mt_frame_data->start_decode_frame;
    while (*start_decode_frame != EB_TRUE);

    while (1) {
        eb_get_full_object_non_blocking(dec_mt_frame_data->
            recon_tile_consumer_fifo_ptr[0],
            &recon_results_wrapper_ptr);

        if (NULL != recon_results_wrapper_ptr) {
            context_ptr = (DecMTNode*)recon_results_wrapper_ptr->object_ptr;

            DecModCtxt *dec_mod_ctxt = (DecModCtxt*)dec_handle_ptr->pv_dec_mod_ctxt;
            if (thread_ctxt != NULL) {
                dec_mod_ctxt = thread_ctxt->dec_mod_ctxt;

                /* TODO : Calling this function at a tile level is
                   excessive. Move this call to operate at a frame level.*/
                setup_segmentation_dequant(thread_ctxt->dec_mod_ctxt);
            }

            if (EB_ErrorNone !=
                decode_tile_job(dec_handle_ptr, context_ptr->node_index, dec_mod_ctxt))
            {
                printf("\nDecode Issue for Tile %d", context_ptr->node_index);
                break;
            }
            eb_release_object(recon_results_wrapper_ptr);
        }
        else
            break;
    }
}

void svt_av1_queue_lf_jobs(EbDecHandle *dec_handle_ptr)
{
    DecMTLFFrameInfo *lf_frame_info = &dec_handle_ptr->master_frame_buf.
        cur_frame_bufs[0].dec_mt_frame_data.lf_frame_info;
    EbObjectWrapper *lf_results_wrapper_ptr;

    int32_t sb_size_h = block_size_high[dec_handle_ptr->seq_header.sb_size];
    uint32_t picture_height_in_sb = (dec_handle_ptr->frame_header.frame_size.
                                frame_height + sb_size_h - 1) / sb_size_h;
    uint32_t y_lcu_index;

    DecMTFrameData  *dec_mt_frame_data =
        &dec_handle_ptr->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;
    TilesInfo   *tiles_info = &dec_handle_ptr->frame_header.tiles_info;
    EB_MEMSET(dec_mt_frame_data->sb_recon_row_map, 0,
        picture_height_in_sb * tiles_info->tile_cols * sizeof(uint32_t));

    memset(lf_frame_info->sb_lf_completed_in_row, -1,
            picture_height_in_sb * sizeof(int32_t));

    for (y_lcu_index = 0; y_lcu_index < picture_height_in_sb; ++y_lcu_index) {
        // Get Empty LF Frame Row Job
        eb_get_empty_object(lf_frame_info->lf_row_producer_fifo_ptr[0],
            &lf_results_wrapper_ptr);

        DecMTNode *context_ptr = (DecMTNode*)lf_results_wrapper_ptr->object_ptr;
        context_ptr->node_index = y_lcu_index;

        // Post Parse Tile Job
        eb_post_full_object(lf_results_wrapper_ptr);
    }
}

/*Frame level function to trigger loop filter for each superblock*/
void dec_av1_loop_filter_frame_mt(
    EbDecHandle *dec_handle_ptr,
    EbPictureBufferDesc *recon_picture_buf,
    LFCtxt *lf_ctxt, LoopFilterInfoN *lf_info,
    int32_t plane_start, int32_t plane_end)
{
    DecMTFrameData  *dec_mt_frame_data1 =
        &dec_handle_ptr->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;
    volatile EbBool *start_lf_frame = &dec_mt_frame_data1->start_lf_frame;
    while (*start_lf_frame != EB_TRUE);

    FrameHeader *frm_hdr = &dec_handle_ptr->frame_header;

    lf_ctxt->delta_lf_stride = dec_handle_ptr->master_frame_buf.sb_cols *
        FRAME_LF_COUNT;
    frm_hdr->loop_filter_params.combine_vert_horz_lf = 1;
    /*init hev threshold const vectors*/
    for (int lvl = 0; lvl <= MAX_LOOP_FILTER; lvl++)
        memset(lf_info->lfthr[lvl].hev_thr, (lvl >> 4), SIMD_WIDTH);

    eb_av1_loop_filter_frame_init(frm_hdr, lf_info, plane_start, plane_end);

    DecMTFrameData  *dec_mt_frame_data =
        &dec_handle_ptr->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;
    EbObjectWrapper *lf_results_wrapper_ptr;
    DecMTNode *context_ptr;

    while (1) {
        eb_get_full_object_non_blocking(dec_mt_frame_data->lf_frame_info.
            lf_row_consumer_fifo_ptr[0], &lf_results_wrapper_ptr);

        if (NULL != lf_results_wrapper_ptr) {
            context_ptr = (DecMTNode*)lf_results_wrapper_ptr->object_ptr;

            TilesInfo   *tiles_info = &dec_handle_ptr->frame_header.tiles_info;

            int32_t start_lf = 0;
            int offset = ((int32_t)context_ptr->node_index == (dec_handle_ptr->
                master_frame_buf.sb_rows - 1)) ? 0 : 1;
            int32_t row_index = (context_ptr->node_index + offset) *
                tiles_info->tile_cols;
            while (!start_lf) {
                start_lf = 1;
                for (int i = 0; i < tiles_info->tile_cols; i++) {
                    start_lf &= dec_mt_frame_data->
                        sb_recon_row_map[row_index + i];
                }
            }
            if (!dec_handle_ptr->frame_header.allow_intrabc) {
                if (dec_handle_ptr->frame_header.loop_filter_params.
                    filter_level[0] || dec_handle_ptr->frame_header.
                    loop_filter_params.filter_level[1])
                {
                    dec_loop_filter_row(dec_handle_ptr, recon_picture_buf,
                        lf_ctxt, lf_info, context_ptr->node_index,
                        plane_start, plane_end);
                }
            }

            // Release LF Results
            eb_release_object(lf_results_wrapper_ptr);
        }
        else
            break;
    }

    eb_block_on_mutex(dec_mt_frame_data->temp_mutex);
    dec_mt_frame_data->start_parse_frame = EB_FALSE;
    dec_mt_frame_data->num_threads_lfed++;
    eb_release_mutex(dec_mt_frame_data->temp_mutex);
}

void* dec_all_stage_kernel(void *input_ptr) {
    // Context
    DecThreadCtxt   *thread_ctxt    = (DecThreadCtxt *)input_ptr;
    EbDecHandle     *dec_handle_ptr = thread_ctxt->dec_handle_ptr;
    volatile EbBool* start_thread = (volatile EbBool*)
        &dec_handle_ptr->start_thread_process;
    while (*start_thread == EB_FALSE);

    while(1)
    {
        /* Parse Tiles */
        parse_frame_tiles(dec_handle_ptr);

        /* Decode Tiles */
        decode_frame_tiles(dec_handle_ptr, thread_ctxt);

        dec_av1_loop_filter_frame_mt(dec_handle_ptr,
            dec_handle_ptr->cur_pic_buf[0]->ps_pic_buf,
            dec_handle_ptr->pv_lf_ctxt, &thread_ctxt->lf_info,
            AOM_PLANE_Y, MAX_MB_PLANE);

        //volatile uint32_t *num_threads_lfed = &dec_handle_ptr->
        //    master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data.num_threads_lfed;
        //while (*num_threads_lfed != dec_handle_ptr->dec_config.threads);
    }

    return EB_NULL;
}
