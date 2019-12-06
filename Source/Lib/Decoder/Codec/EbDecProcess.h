/*
* Copyright(c) 2019 Netflix, Inc.
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

#ifndef EbDecProcess_h
#define EbDecProcess_h

#ifdef __cplusplus
extern "C" {
#endif
#include "EbDefinitions.h"
#include "EbSystemResourceManager.h"

/* Node structure used in Decoder Queues. Can be used for tile/row idx */
typedef struct DecMTNode {
    EbDctor     dctor;

    uint32_t    node_index;

} DecMTNode;

/* Stores the Queue & other related info needed between
   Parse and Recon for a Tile */
typedef struct DecMTParseReconTileInfo {
    /* Tile info for the current Tile */
    TileInfo    *tile_info;

    /* EbFifo at Tile Row level */
    EbFifo      *parse_recon_sbrow_fifo;

    /* Latest SB Recon row picked up for processing in the Tile. This will be
       used for deciding which tile to be picked up for processing. */
    uint32_t    sb_recon_row_started;

    /* Array to store SBs completed in every SB row of Recon stage.
       Used for top-right sync */
    uint32_t    *sb_recon_completed_in_row;

} DecMTParseReconTileInfo;

typedef struct DecMTLFFrameInfo {

    // System Resource Managers
    EbSystemResource    *lf_resource_ptr;
    /* EbFifo at Frame Row level */
    EbFifo              **lf_row_producer_fifo_ptr;
    EbFifo              **lf_row_consumer_fifo_ptr;

    /* Array to store SBs completed in every SB row of LF stage.
       Used for top sync */
    int32_t    *sb_lf_completed_in_row;

} DecMTLFFrameInfo;

/* MT State information for each frame in parallel */
typedef struct DecMTFrameData {
    uint32_t            num_threads_lfed;
    EbBool              start_parse_frame;
    EbBool              start_decode_frame;
    EbBool              start_lf_frame;
    EbHandle            temp_mutex;

    TilesInfo           *tiles_info;

    // System Resource Managers
    EbSystemResource    *parse_tile_resource_ptr;
    /* EbFifo at Tile level : Parse Stage */
    EbFifo              **parse_tile_producer_fifo_ptr;
    EbFifo              **parse_tile_consumer_fifo_ptr;

    // System Resource Managers
    EbSystemResource    *recon_tile_resource_ptr;

    /* EbFifo at Tile level : Recon Stage */
    EbFifo              **recon_tile_producer_fifo_ptr;
    EbFifo              **recon_tile_consumer_fifo_ptr;

    /* To prevent more than 1 thread from mod. recon_row_started simult. */
    EbHandle                recon_mutex;
    /* Parse-Recon Stage structure */
    DecMTParseReconTileInfo *parse_recon_tile_info_array;

    uint32_t    *sb_recon_row_map;

    /* LF Stage structure */
    DecMTLFFrameInfo        lf_frame_info;

    /* EbFifo at Frame Row level : CDEF Stage */
    EbFifo                  *cdef_fifo_ptr;

    /* EbFifo at Frame Row level : SR Stage */
    EbFifo                  *sr_fifo_ptr;
    /* EbFifo at Frame Row level : LR Stage */
    EbFifo                  *lr_fifo_ptr;

    /* EbFifo at Frame Row level : Pad Stage */
    EbFifo                  *pad_fifo_ptr;

} DecMTFrameData;

#ifdef __cplusplus
}
#endif

#endif // EbDecProcess_h
