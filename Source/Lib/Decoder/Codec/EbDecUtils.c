/*
* Copyright(c) 2019 Netflix, Inc.
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

// SUMMARY
//   Contains the Decoder Utility functions

/**************************************
 * Includes
 **************************************/
#include <stdlib.h>

#include "EbDefinitions.h"
#include "EbUtility.h"
#include "EbEntropyCoding.h"

#include "EbDecStruct.h"
#include "EbDecBlock.h"

#include "EbDecHandle.h"
#include "EbDecMemInit.h"

#include "EbDecUtils.h"

EbErrorType check_add_tplmv_buf(EbDecHandle *dec_handle_ptr) {

    FrameHeader *ps_frm_hdr = &dec_handle_ptr->frame_header;
    const int32_t tpl_size = ((ps_frm_hdr->mi_rows + MAX_MIB_SIZE) >> 1) *
                              (ps_frm_hdr->mi_stride >> 1);

    int32_t realloc = (dec_handle_ptr->master_frame_buf.tpl_mvs == NULL) ||
                (dec_handle_ptr->master_frame_buf.tpl_mvs_size < tpl_size);

    if (realloc) {
        /*TODO: Add free now itself */
        EB_MALLOC_DEC(TemporalMvRef *, dec_handle_ptr->master_frame_buf.tpl_mvs,
         tpl_size * sizeof(*dec_handle_ptr->master_frame_buf.tpl_mvs), EB_N_PTR);
        dec_handle_ptr->master_frame_buf.tpl_mvs_size = tpl_size;
    }
    return EB_ErrorNone;
}