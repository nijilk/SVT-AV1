/*
* Copyright(c) 2019 Netflix, Inc.
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

#ifndef EbDecProcess_h
#define EbDecProcess_h

#ifdef __cplusplus
extern "C" {
#endif
#define ENABLE_ROW_MT_DECODE 1
#include "EbDefinitions.h"
#include "EbSystemResourceManager.h"

#define SEM_CHANGE      1
#define TILE_GROUP      1
#define LR_PAD_MT       0

/* Node structure used in Decoder Queues. Can be used for tile/row idx */
typedef struct DecMTNode {
    EbDctor     dctor;

    uint32_t    node_index;

} DecMTNode;

/* Stores the Queue & other related info needed between
   Parse and Recon for a Tile */
typedef struct DecMTParseReconTileInfo {
    /* Tile info for the current Tile */
    TileInfo    tile_info;

    /* System Resource Managers at Tile Row level */ 
    EbSystemResource    *recon_tile_sbrow_resource_ptr;

    /* EbFifo at SB row level : Recon Stage */
    EbFifo      **recon_tile_sbrow_producer_fifo_ptr;
    EbFifo      **recon_tile_sbrow_consumer_fifo_ptr;    

    /* Array to store SB Recon rows picked up for processing in the Tile. This will be
       used for deciding which tile to be picked up for processing. */
    uint32_t    *sb_recon_row_started;

    /* Array to store SBs completed in every SB row of Recon stage.
       Used for top-right sync */
    uint32_t    *sb_recon_completed_in_row;

    /* Number of SB rows in tile */
    int32_t tile_num_sb_rows;

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
#if LR_PAD_MT
    uint32_t            num_threads_paded;
#else
    uint32_t            num_threads_cdefed;
#endif
    uint32_t            num_threads_exited;
    EbBool              end_flag;
    EbBool              start_parse_frame;
    EbBool              start_decode_frame;
    EbBool              start_lf_frame;
    EbBool              start_cdef_frame;
#if LR_PAD_MT
    EbBool              start_lr_frame;
    EbBool              start_pad_frame;
#endif
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
    EbHandle                tile_switch_mutex;

    /*Bhavna: Comment*/
    uint32_t    *sb_recon_row_map;

    /*Bhavna: Comment*/
    uint32_t    *lf_row_map;

    /* LF Stage structure */
    DecMTLFFrameInfo        lf_frame_info;

    /* EbFifo at Frame Row level : CDEF Stage */
    // System Resource Managers
    EbSystemResource        *cdef_resource_ptr;
    /* EbFifo at Frame Row level */
    EbFifo                  **cdef_row_producer_fifo_ptr;
    EbFifo                  **cdef_row_consumer_fifo_ptr;
    /* Array to store 64x64s completed in every 64x64 row of CDEF stage.
       Used for top-right sync */
    uint32_t                *cdef_completed_in_row;
    /* line buffers for every 64x64 row, to store lf o/p*/
    uint16_t                ***cdef_linebuf;
    int32_t                 cdef_linebuf_stride;
    /* cdef map for every 64x64row+1 to track whether cdef is performed or not */
    uint8_t                 *row_cdef_map;
    /*Siva:*/
    uint32_t                cdef_map_stride;
    EbFifo                  *cdef_fifo_ptr;

    /* EbFifo at Frame Row level : SR Stage */
    EbFifo                  *sr_fifo_ptr;
#if LR_PAD_MT
    // System Resource Managers
    EbSystemResource        *lr_resource_ptr;
    /* EbFifo at Frame Row level */
    EbFifo                  **lr_row_producer_fifo_ptr;
    EbFifo                  **lr_row_consumer_fifo_ptr;
    /* Array to store SBs completed in every SB row of LR stage.
       Used for top sync */
    int32_t                 *sb_lr_completed_in_row;
    /* LR SB row level map for rows finished LR */
    uint32_t                *lr_row_map;
#endif
    /* EbFifo at Frame Row level : LR Stage */
    EbFifo                  *lr_fifo_ptr;

#if LR_PAD_MT
    // System Resource Managers
    EbSystemResource        *pad_resource_ptr;
    /* EbFifo at Frame Row level */
    EbFifo                  **pad_row_producer_fifo_ptr;
    EbFifo                  **pad_row_consumer_fifo_ptr;
#endif
    /* EbFifo at Frame Row level : Pad Stage */
    EbFifo                  *pad_fifo_ptr;

} DecMTFrameData;

#ifdef __cplusplus
}
#endif

#endif // EbDecProcess_h
