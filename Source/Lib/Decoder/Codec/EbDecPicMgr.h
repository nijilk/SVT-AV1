/*
* Copyright(c) 2019 Netflix, Inc.
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

#ifndef EbDecPicMgr_h
#define EbDecPicMgr_h

#ifdef __cplusplus
extern "C" {
#endif

/** Decoder Picture Manager **/
typedef struct EbDecPicMgr {

    /* Array of picture buffers */
    EbDecPicBuf as_dec_pic[MAX_PIC_BUFS];

    /* number of picture buffers */
    uint8_t     num_pic_bufs;

} EbDecPicMgr;

typedef struct RefFrameInfo {
    int32_t     map_idx;    /* frame map index */
    EbDecPicBuf *pic_buf;   /* frame buffer */
    /* index based on the offset to be used for sorting */
    int32_t     sort_idx;
} RefFrameInfo;


EbErrorType dec_pic_mgr_init(EbDecPicMgr **pps_pic_mgr);

EbDecPicBuf * dec_pic_mgr_get_cur_pic(EbDecPicMgr *ps_pic_mgr,
                                      SeqHeader   *seq_header,
                                      FrameHeader *frame_info,
                                      EbColorFormat color_format);

void dec_pic_mgr_update_ref_pic(EbDecHandle *dec_handle_ptr, int32_t frame_decoded,
                                int32_t refresh_frame_flags);


void generate_next_ref_frame_map(EbDecHandle *dec_handle_ptr);

#ifdef __cplusplus
}
#endif
#endif // EbDecPicMgr_h