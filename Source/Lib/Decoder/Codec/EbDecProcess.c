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

#include "EbDecBitstream.h"
#include "EbTime.h"

#if MT_SUPPORT

void* dec_all_stage_kernel(void *input_ptr);

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

    /************************************
    * Contexts
    ************************************/

    /************************************
    * Thread Handles
    ************************************/

    /* Decode Library Threads */
    /* ToDo: change this logic. Only for Parse */
    int32_t num_lib_threads = 
        (MIN(dec_handle_ptr->dec_config.threads, num_tiles)) - 1;
    if (num_lib_threads > 0) {
        DecThreadCtxt *thread_ctxt_pa;

        EB_MALLOC_DEC(DecThreadCtxt *, thread_ctxt_pa,
            num_lib_threads * sizeof(DecThreadCtxt), EB_N_PTR);
        for (int32_t i = 0; i < num_lib_threads; i++) {
            thread_ctxt_pa[i].thread_cnt     = i + 1;
            thread_ctxt_pa[i].dec_handle_ptr = dec_handle_ptr;
        }
        EB_CREATE_THREAD_ARRAY(dec_handle_ptr->parse_thread_handle_array,
            num_lib_threads, dec_all_stage_kernel, (void **)&thread_ctxt_pa);
    }
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

EbErrorType parse_tile_job(EbDecHandle *dec_handle_ptr,
    int32_t tile_num, int32_t thread_index)
{
    EbErrorType status = EB_ErrorNone;
    int32_t tile_row;

    TilesInfo   *tiles_info = &dec_handle_ptr->frame_header.tiles_info;
    MasterParseCtxt *master_parse_ctxt =
                    (MasterParseCtxt *) dec_handle_ptr->pv_master_parse_ctxt;
    ParseCtxt *parse_ctxt = &master_parse_ctxt->tile_parse_ctxt[thread_index];
    parse_ctxt->seq_header = &dec_handle_ptr->seq_header;
    parse_ctxt->frame_header = &dec_handle_ptr->frame_header;
    ParseTileData *parse_tile_data = &master_parse_ctxt->
                                                parse_tile_data[tile_num];
    FrameHeader *frame_header = parse_ctxt->frame_header;

    tile_row = tile_num / tiles_info->tile_cols;
    parse_ctxt->parse_above_nbr4x4_ctxt = &master_parse_ctxt->parse_above_nbr4x4_ctxt[tile_row];
    parse_ctxt->parse_left_nbr4x4_ctxt = &master_parse_ctxt->parse_left_nbr4x4_ctxt[thread_index];

    start_parse_tile(dec_handle_ptr, parse_ctxt, tiles_info, tile_num, 1);

    return status;
}

void parse_frame_tiles(EbDecHandle     *dec_handle_ptr, int th_cnt) {
    DecMTFrameData  *dec_mt_frame_data =
        &dec_handle_ptr->master_frame_buf.cur_frame_bufs[0].dec_mt_frame_data;
    EbObjectWrapper *parse_results_wrapper_ptr;
    DecMTNode *context_ptr;
    while (1) {
        eb_get_full_object_non_blocking(dec_mt_frame_data->
            parse_tile_consumer_fifo_ptr[0],
            &parse_results_wrapper_ptr);

        if (NULL != parse_results_wrapper_ptr) {
            context_ptr = (DecMTNode*)parse_results_wrapper_ptr->object_ptr;

            MasterParseCtxt *master_parse_ctxt =
                (MasterParseCtxt *)dec_handle_ptr->pv_master_parse_ctxt;
            ParseTileData *parse_tile_data = &master_parse_ctxt->
                parse_tile_data[context_ptr->node_index];
            printf("\nThread id : %d Tile id : %d",
                th_cnt, context_ptr->node_index);
            if (EB_ErrorNone !=
                parse_tile_job(dec_handle_ptr, context_ptr->node_index, th_cnt))
            {
                printf("\nParse Issue for Tile %d", context_ptr->node_index);
                break;
            }
            printf("\nThread id : %d Tile id : %d done \n",
                th_cnt, context_ptr->node_index);

            // Release Parse Results
            eb_release_object(parse_results_wrapper_ptr);
        }
        else
            break;
    }
}

void* dec_all_stage_kernel(void *input_ptr) {
    // Context
    DecThreadCtxt   *thread_ctxt    = (DecThreadCtxt *)input_ptr;
    EbDecHandle     *dec_handle_ptr = thread_ctxt->dec_handle_ptr;
    volatile EbBool* start_thread = (volatile EbBool*)
        &dec_handle_ptr->start_thread_process;
    while (*start_thread == EB_FALSE)
        EbSleepMs(5);

    while(1)
    {
        /* Parse Tiles */
        parse_frame_tiles(dec_handle_ptr, thread_ctxt->thread_cnt);
    }

    return EB_NULL;
}

#endif // MT_SUPPORT
