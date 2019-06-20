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

#ifndef EbDecHandle_h
#define EbDecHandle_h

#ifdef __cplusplus
extern "C" {
#endif

#include "EbDecStruct.h"
#include "EbDecBlock.h"

/* Maximum number of frames in parallel */
#define DEC_MAX_NUM_FRM_PRLL    1
/** Maximum picture buffers needed **/
#define MAX_PIC_BUFS (REF_FRAMES + 1 + DEC_MAX_NUM_FRM_PRLL)

/** Picture Structure **/
typedef struct EbDecPicBuf {

    uint8_t             is_free;

    size_t              size;

    /* Number of reference for this frame */
    uint8_t             ref_count;

    uint32_t            order_hint;
    uint32_t            ref_order_hints[INTER_REFS_PER_FRAME];

    EbPictureBufferDesc *ps_pic_buf;

    FRAME_CONTEXT       final_frm_ctx;

    GlobalMotionParams  global_motion[REF_FRAMES];
    /* MV at 8x8 lvl */
    /* seg map */
    /* order hint */
    /* film grain */

} EbDecPicBuf;

/* Frame level buffers */
typedef struct CurFrameBuf {
    SBInfo          *sb_info;

    ModeInfo_t      *mode_info;

    int32_t         *luma_coeff;
    int32_t         *chroma_coeff;

    TransformInfo_t *luma_trans_info;
    TransformInfo_t *chroma_trans_info;

    int8_t          *cdef_strength;
    int32_t         *delta_q;
    int32_t         *delta_lf;

    /* Tile Map at SB level : TODO. Can be removed? */
    uint8_t         *tile_map_sb;
} CurFrameBuf;

#define FRAME_MI_MAP 1
/* Frame level buffers */
typedef struct FrameMiMap {
#if FRAME_MI_MAP
    /* SBInfo pointers for entire frame */
    SBInfo      **pps_sb_info;
    /* ModeInfo offset wrt it's SB start */
    uint16_t    *p_mi_offset;
    /*!< superblock size inlog2 unit */
    uint8_t     sb_size_log2;

    int32_t         mi_cols_algnsb;
    int32_t         mi_rows_algnsb;
    int32_t         sb_cols;
    int32_t         sb_rows;
#else
    /* For cur SB> Allocated worst case 128x128 SB => 128/4 = 32.
      +1 for 1 top & left 4x4s */
    int16_t      cur_sb_mi_map[33][33];

    /* 2(for 4x4 chroma case) Top SB 4x4 row MI map */
    int16_t      *top_sbrow_mi_map;
#endif
    /*  number of MI in SB width,
        is same as number of MI in SB height */
    int32_t     num_mis_in_sb_wd;
} FrameMiMap;

/* Master Frame Buf containing all frame level bufs like ModeInfo
       for all the frames in parallel */
typedef struct MasterFrameBuf {
    CurFrameBuf     cur_frame_bufs[DEC_MAX_NUM_FRM_PRLL];

    int32_t         num_mis_in_sb;

    int32_t         sb_cols;
    int32_t         sb_rows;

    /* TODO : Should be moved to thread ctxt */
    FrameMiMap      frame_mi_map;

    TemporalMvRef   *tpl_mvs;
    int32_t         tpl_mvs_size;
    int8_t          ref_frame_side[REF_FRAMES];

} MasterFrameBuf;

/**************************************
 * Component Private Data
 **************************************/
typedef struct EbDecHandle {
    uint32_t size;
    uint32_t dec_cnt;

    /** Num frames in parallel */
    int32_t num_frms_prll;

    /** Flag to signal seq_header done */
    int32_t seq_header_done;

    /** Flag to signal decoder memory init is done */
    int32_t mem_init_done;

    /** Dec Configuration parameters */
    EbSvtAv1DecConfiguration    dec_config;

    SeqHeader   seq_header;
    FrameHeader frame_header;

    uint8_t seen_frame_header;
    uint8_t show_existing_frame;

    // Thread Handles

    // Module Contexts
    void   *pv_parse_ctxt;

    void   *pv_dec_mod_ctxt;

    /** Pointer to Picture manager structure **/
    void   *pv_pic_mgr;

    // * 'remapped_ref_idx[i - 1]' maps reference type �i� (range: LAST_FRAME ...
    // EXTREF_FRAME) to a remapped index �j� (in range: 0 ... REF_FRAMES - 1)
    // * Later, 'cm->ref_frame_map[j]' maps the remapped index �j� to a pointer to
    // the reference counted buffer structure RefCntBuffer, taken from the buffer
    // pool cm->buffer_pool->frame_bufs.
    //
    // LAST_FRAME,                        ...,      EXTREF_FRAME
    //      |                                           |
    //      v                                           v
    // remapped_ref_idx[LAST_FRAME - 1],  ...,  remapped_ref_idx[EXTREF_FRAME - 1]
    //      |                                           |
    //      v                                           v
    // ref_frame_map[],                   ...,     ref_frame_map[]
    //
    // Note: INTRA_FRAME always refers to the current frame, so there's no need to
    // have a remapped index for the same.
    int32_t remapped_ref_idx[REF_FRAMES];

    /* TODO:  Move ref_frame_map, remapped_ref_idx, cur_pic_buf and frame_header to a FrameStr! */
    EbDecPicBuf *ref_frame_map[REF_FRAMES];
    // Prepare ref_frame_map for the next frame.
    EbDecPicBuf *next_ref_frame_map[REF_FRAMES];

    /* TODO: Move to buffer pool. */
    EbDecPicBuf *cur_pic_buf[DEC_MAX_NUM_FRM_PRLL];

    // Callbacks

    //DPB + MV, ... buf

    /* Master Frame Buf containing all frame level bufs like ModeInfo
       for all the frames in parallel */
    MasterFrameBuf master_frame_buf;

    // Memory Map
#if MEM_MAP_OPT
    EbMemoryMapEntry            *memory_map_init_address;
#endif
    EbMemoryMapEntry            *memory_map;
    uint32_t                     memory_map_index;
    uint64_t                     total_lib_memory;
}EbDecHandle;

#ifdef __cplusplus
    }
#endif
#endif // EbEncHandle_h
