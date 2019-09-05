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

#include "stdlib.h"

#include "EbDefinitions.h"
#include "EbUtility.h"
#include "EbEntropyCoding.h"

#include"EbAv1Structs.h"
#include "EbDecStruct.h"
#include "EbDecBlock.h"

#include "EbDecHandle.h"

#include "EbObuParse.h"
#include "EbDecMemInit.h"
#include "EbDecPicMgr.h"
#include "EbDecRestoration.h"

#include "EbDecParseObuUtil.h"

/*TODO : Should be removed */
#include "EbDecInverseQuantize.h"
#include "EbDecProcessFrame.h"

#include "EbDecNbr.h"
#include "EbDecUtils.h"
#include "EbDecLF.h"

#include "EbDecCdef.h"


#define CONFIG_MAX_DECODE_PROFILE 2
#define INT_MAX       2147483647    // maximum (signed) int value

int remap_lr_type[4] = {
    RESTORE_NONE, RESTORE_SWITCHABLE, RESTORE_WIENER, RESTORE_SGRPROJ };

/* Checks that the remaining bits start with a 1 and ends with 0s.
 * It consumes an additional byte, if already byte aligned before the check. */
int av1_check_trailing_bits(bitstrm_t *bs)
{
    // bit_offset is set to 0 (mod 8) when the reader is already byte aligned
    int bits_before_alignment = 8 - bs->bit_ofst % 8;
    int trailing = dec_get_bits(bs, bits_before_alignment);
    if (trailing != (1 << (bits_before_alignment - 1)))
        return EB_Corrupt_Frame;
    return 0;
}

int byte_alignment(bitstrm_t *bs)
{
    while (bs->bit_ofst & 7) {
        if (dec_get_bits(bs, 1))
            return EB_Corrupt_Frame;
    }
    return 0;
}

void compute_image_size(FrameHeader *frm)
{
    frm->mi_cols = 2 * ((frm->frame_size.frame_width + 7 - 1) >> 3);
    frm->mi_rows = 2 * ((frm->frame_size.frame_height + 7 - 1) >> 3);
    frm->mi_stride = ALIGN_POWER_OF_TWO(frm->mi_cols, MAX_MIB_SIZE_LOG2);
}

/*TODO: Harmonize with encoder function */
// Find smallest k>=0 such that (blk_size << k) >= target
static int32_t dec_tile_log2(int32_t blk_size, int32_t target) {
    int32_t k;
    for (k = 0; (blk_size << k) < target; k++) {
    }
    return k;
}

// Returns 1 when OBU type is valid, and 0 otherwise.
static int is_valid_obu_type(int obu_type) {
    int valid_type = 0;
    switch (obu_type) {
    case OBU_SEQUENCE_HEADER:
    case OBU_TEMPORAL_DELIMITER:
    case OBU_FRAME_HEADER:
    case OBU_TILE_GROUP:
    case OBU_METADATA:
    case OBU_FRAME:
    case OBU_REDUNDANT_FRAME_HEADER:
        //case OBU_TILE_LIST:
    case OBU_PADDING: valid_type = 1; break;
    default: break;
    }
    return valid_type;
}
// Read Operating point parameters
void read_operating_params_info(bitstrm_t *bs, EbOperatingParametersInfo   *op_info,
    DecoderModelInfo   *model_info, int index)
{
    if (index > MAX_NUM_OPERATING_POINTS)
        return; // EB_DecUnsupportedBitstream;
    op_info->decoder_buffer_delay =
        dec_get_bits(bs, model_info->buffer_delay_length_minus_1 + 1);
    PRINT("decoder_buffer_delay", op_info->decoder_buffer_delay);
    op_info->encoder_buffer_delay =
        dec_get_bits(bs, model_info->buffer_delay_length_minus_1 + 1);
    PRINT("encoder_buffer_delay", op_info->encoder_buffer_delay);
    op_info->low_delay_mode_flag = dec_get_bits(bs, 1);
    PRINT("low_delay_mode_flag", op_info->low_delay_mode_flag);
}

// Read Timing information
void read_timing_info(bitstrm_t *bs, EbTimingInfo   *timing_info)
{
    timing_info->num_units_in_display_tick = dec_get_bits(bs, 32);
    PRINT("num_units_in_display_tick", timing_info->num_units_in_display_tick);
    timing_info->time_scale = dec_get_bits(bs, 32);
    PRINT("time_scale", timing_info->time_scale);
    if (timing_info->num_units_in_display_tick <= 0 || timing_info->time_scale <= 0)
        return; // EB_DecUnsupportedBitstream;
    timing_info->equal_picture_interval = dec_get_bits(bs, 1);
    PRINT("equal_picture_interval", timing_info->equal_picture_interval);
    if (timing_info->equal_picture_interval) {
        timing_info->num_ticks_per_picture = dec_get_bits_uvlc(bs) + 1;
        if ((timing_info->num_ticks_per_picture) == UINT32_MAX)
            return; // EB_DecUnsupportedBitstream;
    }
}

// Read Decoder model information
void read_decoder_model_info(bitstrm_t *bs, DecoderModelInfo   *model_info)
{
    model_info->buffer_delay_length_minus_1 = dec_get_bits(bs, 5);
    PRINT("buffer_delay_length_minus_1", model_info->buffer_delay_length_minus_1);
    model_info->num_units_in_decoding_tick = dec_get_bits(bs, 32);
    PRINT("num_units_in_decoding_tick", model_info->num_units_in_decoding_tick);
    model_info->buffer_delay_length_minus_1 = dec_get_bits(bs, 5);
    PRINT("buffer_delay_length_minus_1", model_info->buffer_delay_length_minus_1);
    model_info->frame_presentation_time_length_minus_1 = dec_get_bits(bs, 5);
    PRINT("frame_presentation_time_length_minus_1",
        model_info->frame_presentation_time_length_minus_1);
}

// Read bit depth
void read_bit_depth(bitstrm_t *bs, EbColorConfig   *color_info, SeqHeader   *seq_header)
{
    uint8_t twelve_bit, high_bitdepth;
    high_bitdepth = dec_get_bits(bs, 1);
    PRINT("high_bitdepth",high_bitdepth);
    if (seq_header->seq_profile == PROFESSIONAL_PROFILE && high_bitdepth) {
        twelve_bit = dec_get_bits(bs, 1);
        PRINT("twelve_bit", twelve_bit);
        color_info->bit_depth = twelve_bit ? AOM_BITS_12 : AOM_BITS_10;
    }
    else if (seq_header->seq_profile <= PROFESSIONAL_PROFILE)
        color_info->bit_depth = high_bitdepth ? AOM_BITS_10 : AOM_BITS_8;
    else
        return; // EB_DecUnsupportedBitstream;
}

// Read Color configuration
void read_color_config(bitstrm_t *bs, EbColorConfig   *color_info, SeqHeader   *seq_header)
{
    read_bit_depth(bs, color_info, seq_header);
    color_info->mono_chrome =
        (seq_header->seq_profile != HIGH_PROFILE) ? dec_get_bits(bs, 1) : 0;
    PRINT("mono_chrome", color_info->mono_chrome);
    color_info->color_description_present_flag = dec_get_bits(bs, 1);
    PRINT("color_description_present_flag", color_info->color_description_present_flag);
    if (color_info->color_description_present_flag) {
        color_info->color_primaries = dec_get_bits(bs, 8);
        PRINT("color_primaries", color_info->color_primaries);
        color_info->transfer_characteristics = dec_get_bits(bs, 8);
        PRINT("transfer_characteristics", color_info->transfer_characteristics);
        color_info->matrix_coefficients = dec_get_bits(bs, 8);
        PRINT("matrix_coefficients", color_info->matrix_coefficients);
    }
    else {
        color_info->color_primaries = EB_CICP_CP_UNSPECIFIED;
        color_info->transfer_characteristics = EB_CICP_TC_UNSPECIFIED;
        color_info->matrix_coefficients = EB_CICP_MC_UNSPECIFIED;
    }
    if (color_info->mono_chrome) {
        color_info->color_range = dec_get_bits(bs, 1);
        PRINT("color_range", color_info->color_range);
        color_info->subsampling_y = color_info->subsampling_x = 1;
        color_info->chroma_sample_position = EB_CSP_UNKNOWN;
        color_info->separate_uv_delta_q = 0;
        return;
    }
    else if (color_info->color_primaries == EB_CICP_CP_BT_709 &&
        color_info->transfer_characteristics == EB_CICP_TC_SRGB &&
        color_info->matrix_coefficients == EB_CICP_MC_IDENTITY) {
        color_info->subsampling_x = color_info->subsampling_y = 0;
        color_info->color_range = 1;  // assume full color-range
        if (!(seq_header->seq_profile == HIGH_PROFILE ||
            (seq_header->seq_profile == PROFESSIONAL_PROFILE &&
                color_info->bit_depth == AOM_BITS_12)))
            return; // EB_DecUnsupportedBitstream;
    }
    else
    {
        color_info->color_range = dec_get_bits(bs, 1);
        PRINT("color_range", color_info->color_range);
        if (seq_header->seq_profile == MAIN_PROFILE)
            color_info->subsampling_x = color_info->subsampling_y = 1; // 420 only
        else if (seq_header->seq_profile == HIGH_PROFILE)
            color_info->subsampling_x = color_info->subsampling_y = 0; // 444 only
        else {
            if (color_info->bit_depth == AOM_BITS_12) {
                color_info->subsampling_x = dec_get_bits(bs, 1);
                PRINT("subsampling_x", color_info->subsampling_x);
                if (color_info->subsampling_x) {
                    color_info->subsampling_y = dec_get_bits(bs, 1);
                    PRINT("subsampling_y", color_info->subsampling_y);
                }
                else
                    color_info->subsampling_y = 0;
            }
            else { // 422
                color_info->subsampling_x = 1;
                color_info->subsampling_y = 0;
            }
        }
        if (color_info->matrix_coefficients == EB_CICP_MC_IDENTITY &&
            (color_info->subsampling_x || color_info->subsampling_y))
            return; // EB_DecUnsupportedBitstream;
        if (color_info->subsampling_x && color_info->subsampling_y)
            color_info->chroma_sample_position = dec_get_bits(bs, 2);
        PRINT("chroma_sample_position", color_info->chroma_sample_position);
    }
    color_info->separate_uv_delta_q = dec_get_bits(bs, 1);
    PRINT("separate_uv_delta_q", color_info->separate_uv_delta_q);
}

void read_temporal_delimitor_obu(uint8_t *seen_frame_header) {
    *seen_frame_header = 0;
}

// Read Sequence header
EbErrorType read_sequence_header_obu(bitstrm_t *bs, SeqHeader   *seq_header)
{
    EbErrorType status;

    seq_header->seq_profile = (EbAv1SeqProfile)dec_get_bits(bs, 3);
    PRINT("seq_profile", seq_header->seq_profile);
    if (seq_header->seq_profile > CONFIG_MAX_DECODE_PROFILE)
        return EB_Corrupt_Frame;

    seq_header->still_picture = dec_get_bits(bs, 1);
    PRINT("still_picture", seq_header->still_picture);
    seq_header->reduced_still_picture_header = dec_get_bits(bs, 1);
    PRINT("reduced_still_picture_header", seq_header->reduced_still_picture_header);

    // Video must have reduced_still_picture_header = 0
    if (!seq_header->still_picture && seq_header->reduced_still_picture_header)
        return EB_DecUnsupportedBitstream;

    if (seq_header->reduced_still_picture_header) {
        seq_header->timing_info.timing_info_present = 0;
        seq_header->decoder_model_info_present_flag = 0;
        seq_header->initial_display_delay_present_flag = 0;
        seq_header->operating_points_cnt_minus_1 = 0;
        seq_header->operating_point[0].op_idc = 0;
        seq_header->operating_point[0].seq_level_idx = dec_get_bits(bs, LEVEL_BITS);
        PRINT("seq_level_idx", seq_header->operating_point[0].seq_level_idx);
        if (!is_valid_seq_level_idx(seq_header->operating_point->seq_level_idx))
            return EB_Corrupt_Frame;
        seq_header->operating_point[0].seq_tier = 0;
        seq_header->operating_point[0].decoder_model_present_for_this_op = 0;
        seq_header->operating_point[0].initial_display_delay_present_for_this_op = 0;
    }
    else {
        seq_header->timing_info.timing_info_present = dec_get_bits(bs, 1);
        PRINT("timing_info_present_flag", seq_header->timing_info.timing_info_present);
        if (seq_header->timing_info.timing_info_present)
        {
            read_timing_info(bs, &seq_header->timing_info);
            seq_header->decoder_model_info_present_flag = dec_get_bits(bs, 1);
            PRINT("decoder_model_info_present_flag",
                seq_header->decoder_model_info_present_flag);
            if (seq_header->decoder_model_info_present_flag)
                read_decoder_model_info(bs, &seq_header->decoder_model_info);
        }
        else
            seq_header->decoder_model_info_present_flag = 0;
    }

    seq_header->initial_display_delay_present_flag = dec_get_bits(bs, 1);
    PRINT("initial_display_delay_present_flag",
        seq_header->initial_display_delay_present_flag);
    seq_header->operating_points_cnt_minus_1 =
        dec_get_bits(bs, OP_POINTS_CNT_MINUS_1_BITS);
    PRINT("operating_points_cnt_minus_1", seq_header->operating_points_cnt_minus_1);
    for (int i = 0; i <= seq_header->operating_points_cnt_minus_1; i++)
    {
        seq_header->operating_point[i].op_idc =
            dec_get_bits(bs, OP_POINTS_IDC_BITS);
        PRINT("operating_point_idc",
            seq_header->operating_point[i].op_idc);
        seq_header->operating_point[i].seq_level_idx = dec_get_bits(bs, LEVEL_BITS);
        PRINT("seq_level_idx", seq_header->operating_point[i].seq_level_idx);
        if (!is_valid_seq_level_idx(seq_header->operating_point[i].seq_level_idx))
            return EB_Corrupt_Frame;
        if (seq_header->operating_point[i].seq_level_idx > 7) {
            seq_header->operating_point[i].seq_tier = dec_get_bits(bs, 1);
            PRINT("seq_tier", seq_header->operating_point[i].seq_tier);
        }
        else
            seq_header->operating_point[i].seq_tier = 0;

        if (seq_header->decoder_model_info_present_flag) {
            seq_header->operating_point[i].decoder_model_present_for_this_op =
                dec_get_bits(bs, 1);
            PRINT("decoder_model_present_for_this_op",
                seq_header->operating_point[i].decoder_model_present_for_this_op);
            if (seq_header->operating_point[i].decoder_model_present_for_this_op)
                read_operating_params_info(bs,
                    &seq_header->operating_point[i].operating_parameters_info,
                    &seq_header->decoder_model_info, i);
        }
        else
            seq_header->operating_point[i].decoder_model_present_for_this_op = 0;

        if (seq_header->initial_display_delay_present_flag) {
            seq_header->operating_point[i].initial_display_delay_present_for_this_op
                = dec_get_bits(bs, 1);
            PRINT("initial_display_delay_present_for_this_op",
                seq_header->operating_point[i].initial_display_delay_present_for_this_op);
            if (seq_header->operating_point[i].initial_display_delay_present_for_this_op)
                seq_header->operating_point[i].initial_display_delay
                = dec_get_bits(bs, 4) + 1;
            PRINT("initial_display_delay_minus_1",
                seq_header->operating_point[i].initial_display_delay - 1);
        }
    }

    // TODO: operating_point = choose_operating_point( )

    seq_header->frame_width_bits = dec_get_bits(bs, 4) + 1;
    PRINT("frame_width_bits", seq_header->frame_width_bits);
    seq_header->frame_height_bits = dec_get_bits(bs, 4) + 1;
    PRINT("frame_height_bits", seq_header->frame_height_bits);
    seq_header->max_frame_width = dec_get_bits(bs, (seq_header->frame_width_bits)) + 1;
    PRINT("max_frame_width", seq_header->max_frame_width);
    seq_header->max_frame_height = dec_get_bits(bs, (seq_header->frame_height_bits)) + 1;
    PRINT("max_frame_height", seq_header->max_frame_height);

    if (seq_header->reduced_still_picture_header)
        seq_header->frame_id_numbers_present_flag = 0;
    else {
        seq_header->frame_id_numbers_present_flag = dec_get_bits(bs, 1);
        PRINT("frame_id_numbers_present_flag", seq_header->frame_id_numbers_present_flag);
    }
    if (seq_header->frame_id_numbers_present_flag) {
        seq_header->delta_frame_id_length = dec_get_bits(bs, 4) + 2;
        PRINT("delta_frame_id_length", seq_header->delta_frame_id_length);
        seq_header->frame_id_length = dec_get_bits(bs, 3) + 1;
        PRINT("additional_frame_id_length_minus_1",
            seq_header->frame_id_length - 1);
        if (seq_header->frame_id_length - 1 > 16)
            return EB_Corrupt_Frame;
    }

    seq_header->use_128x128_superblock = dec_get_bits(bs, 1);
    seq_header->sb_size         = seq_header->use_128x128_superblock ?
                                  BLOCK_128X128 : BLOCK_64X64;
    seq_header->sb_mi_size      = seq_header->use_128x128_superblock ? 32 : 16;
    seq_header->sb_size_log2    = seq_header->use_128x128_superblock ? 7  :  6;
    PRINT("use_128x128_superblock", seq_header->use_128x128_superblock);
    seq_header->enable_filter_intra = dec_get_bits(bs, 1);
    PRINT("enable_filter_intra", seq_header->enable_filter_intra);
    seq_header->enable_intra_edge_filter = dec_get_bits(bs, 1);
    PRINT("enable_intra_edge_filter", seq_header->enable_intra_edge_filter);

    if (seq_header->reduced_still_picture_header) {
        seq_header->enable_interintra_compound = 0;
        seq_header->enable_masked_compound = 0;
        seq_header->enable_warped_motion = 0;
        seq_header->enable_dual_filter = 0;
        seq_header->order_hint_info.enable_jnt_comp = 0;
        seq_header->order_hint_info.enable_ref_frame_mvs = 0;
        seq_header->seq_force_screen_content_tools = 2;
        seq_header->seq_force_integer_mv = 2;
        seq_header->order_hint_info.order_hint_bits = 0;
    }
    else {
        seq_header->enable_interintra_compound = dec_get_bits(bs, 1);
        PRINT("enable_interintra_compound", seq_header->enable_interintra_compound);
        seq_header->enable_masked_compound = dec_get_bits(bs, 1);
        PRINT("enable_masked_compound", seq_header->enable_masked_compound);
        seq_header->enable_warped_motion = dec_get_bits(bs, 1);
        PRINT("enable_warped_motion", seq_header->enable_warped_motion);
        seq_header->enable_dual_filter = dec_get_bits(bs, 1);
        PRINT("enable_dual_filter", seq_header->enable_dual_filter);
        seq_header->order_hint_info.enable_order_hint = dec_get_bits(bs, 1);
        PRINT("enable_order_hint", seq_header->order_hint_info.enable_order_hint);
        if (seq_header->order_hint_info.enable_order_hint) {
            seq_header->order_hint_info.enable_jnt_comp = dec_get_bits(bs, 1);
            PRINT("enable_jnt_comp", seq_header->order_hint_info.enable_jnt_comp);
            seq_header->order_hint_info.enable_ref_frame_mvs = dec_get_bits(bs, 1);
            PRINT("enable_ref_frame_mvs",
                seq_header->order_hint_info.enable_ref_frame_mvs);
        }
        else {
            seq_header->order_hint_info.enable_jnt_comp = 0;
            seq_header->order_hint_info.enable_ref_frame_mvs = 0;
        }
        if (dec_get_bits(bs, 1)) {
            PRINT_NAME("seq_choose_screen_content_tools");
            seq_header->seq_force_screen_content_tools = SELECT_SCREEN_CONTENT_TOOLS;
        }
        else
            seq_header->seq_force_screen_content_tools = dec_get_bits(bs, 1);
        PRINT("seq_force_screen_content_tools",
            seq_header->seq_force_screen_content_tools);
        if (seq_header->seq_force_screen_content_tools > 0) {
            if (dec_get_bits(bs, 1)) {
                PRINT_NAME("seq_choose_screen_content_tools");
                seq_header->seq_force_integer_mv = SELECT_INTEGER_MV;
            }
            else
                seq_header->seq_force_integer_mv = dec_get_bits(bs, 1);
            PRINT("seq_force_integer_mv", seq_header->seq_force_integer_mv);
        }
        else
            seq_header->seq_force_integer_mv = SELECT_INTEGER_MV;

        if (seq_header->order_hint_info.enable_order_hint) {
            seq_header->order_hint_info.order_hint_bits = dec_get_bits(bs, 3) + 1;
            PRINT("order_hint_bits", seq_header->order_hint_info.order_hint_bits - 1);
        }
        else
            seq_header->order_hint_info.order_hint_bits = 0;
    }
    seq_header->enable_superres = dec_get_bits(bs, 1);
    PRINT("enable_superres", seq_header->enable_superres);
    seq_header->enable_cdef = dec_get_bits(bs, 1);
    PRINT("enable_cdef", seq_header->enable_cdef);
    seq_header->enable_restoration = dec_get_bits(bs, 1);
    PRINT("enable_restoration", seq_header->enable_restoration);

    read_color_config(bs, &seq_header->color_config, seq_header);
    seq_header->film_grain_params_present = dec_get_bits(bs, 1);
    PRINT("film_grain_params_present", seq_header->film_grain_params_present);
    status = av1_check_trailing_bits(bs);
    if (status != EB_ErrorNone)
        return status;
    return EB_ErrorNone;
}

// Read OBU header
EbErrorType read_obu_header(bitstrm_t *bs, ObuHeader   *header)
{
    PRINT_NL;
    if (!bs || !header) return EB_ErrorBadParameter;

    header->size = 1;

    if (dec_get_bits(bs, 1) != 0) {
        // obu_forbidden_bit must be set to 0.
        return EB_Corrupt_Frame;
    }
    PRINT_NAME("obu_forbidden_bit");
    header->obu_type = (obuType)dec_get_bits(bs, 4);
    PRINT("obu_type", header->obu_type);
    if (!is_valid_obu_type(header->obu_type))
        return EB_Corrupt_Frame;

    header->obu_extension_flag = dec_get_bits(bs, 1);
    PRINT("obu_extension_flag", header->obu_extension_flag);
    header->obu_has_size_field = dec_get_bits(bs, 1);
    PRINT("obu_has_size_field", header->obu_has_size_field);
    if (!header->obu_has_size_field)
        return EB_Corrupt_Frame;

    if (dec_get_bits(bs, 1) != 0) {
        // obu_reserved_1bit must be set to 0
        return EB_Corrupt_Frame;
    }
    PRINT_NAME("obu_reserved_1bit");

    if (header->obu_extension_flag) {
        header->size += 1;

        header->temporal_id = dec_get_bits(bs, 3);
        PRINT("temporal_id", header->temporal_id);
        header->spatial_id = dec_get_bits(bs, 2);
        PRINT("spatial_id", header->spatial_id);

        if (dec_get_bits(bs, 3) != 0) {
            // extension_header_reserved_3bits must be set to 0
            return EB_Corrupt_Frame;
        }
        PRINT_NAME("obu_extension_header_reserved_3bits");
    }
    else
        header->temporal_id = header->spatial_id = 0;

    return EB_ErrorNone;
}

// Read OBU size
EbErrorType read_obu_size(bitstrm_t *bs, size_t bytes_available,
    size_t *const obu_size, size_t *const length_field_size)
{
    size_t u_obu_size = 0;
    dec_get_bits_leb128(bs, bytes_available, &u_obu_size, length_field_size);

    if (u_obu_size > UINT32_MAX) return EB_Corrupt_Frame;
    *obu_size = u_obu_size;
    PRINT("obu_size", *obu_size);
    return EB_ErrorNone;
}

/** Reads OBU header and size */
EbErrorType open_bistream_unit(bitstrm_t *bs, ObuHeader *header, size_t size,
    size_t *const length_size)
{
    EbErrorType status;

    status = read_obu_header(bs, header);
    if (status != EB_ErrorNone)
        return status;

    status = read_obu_size(bs, size, &header->payload_size, length_size);
    if (status != EB_ErrorNone)
        return status;

    return EB_ErrorNone;
}

void temporal_point_info(bitstrm_t *bs, DecoderModelInfo   *model_info,
    FrameHeader   *frame_info) {
    int n = model_info->frame_presentation_time_length_minus_1 + 1;
    frame_info->frame_presentation_time = dec_get_bits(bs, n);
}

void superres_params(bitstrm_t *bs, SeqHeader   *seq_header, FrameHeader   *frame_info)
{
    int use_superres, coded_denom;
    if (seq_header->enable_superres)
        use_superres = dec_get_bits(bs, 1);
    else
        use_superres = 0;

    PRINT_NAME("use_superres");
    if (use_superres) {
        coded_denom = dec_get_bits(bs, SUPERRES_SCALE_BITS);
        PRINT_FRAME("coded_denom", coded_denom);
        frame_info->frame_size.superres_denominator = coded_denom +
            SUPERRES_SCALE_DENOMINATOR_MIN;
    }
    else
        frame_info->frame_size.superres_denominator = SCALE_NUMERATOR;
    frame_info->frame_size.superres_upscaled_width = frame_info->frame_size.frame_width;
    frame_info->frame_size.frame_width = (frame_info->frame_size.superres_upscaled_width
        * SCALE_NUMERATOR + (frame_info->frame_size.superres_denominator / 2))
        / frame_info->frame_size.superres_denominator;
}

// Read frame size
static void read_frame_size(bitstrm_t *bs, SeqHeader   *seq_header, FrameHeader
    *frame_info, int frame_size_override_flag)
{
    if (frame_size_override_flag) {
        frame_info->frame_size.frame_width =
            dec_get_bits(bs, seq_header->frame_width_bits + 1) + 1;
        frame_info->frame_size.frame_height
            = dec_get_bits(bs, seq_header->frame_height_bits + 1) + 1;
    }
    else {
        frame_info->frame_size.frame_width = seq_header->max_frame_width;
        frame_info->frame_size.frame_height = seq_header->max_frame_height;
    }
    PRINT_FRAME("frame_width", frame_info->frame_size.frame_width);
    PRINT_FRAME("frame_height", frame_info->frame_size.frame_height);
    superres_params(bs, seq_header, frame_info);
    compute_image_size(frame_info);

    assert((frame_info->frame_size.frame_width) <= seq_header->max_frame_width);
    assert((frame_info->frame_size.frame_height) <= seq_header->max_frame_height);
}

void read_render_size(bitstrm_t *bs, FrameHeader   *frame_info)
{
    uint8_t render_and_frame_size_different;
    render_and_frame_size_different = dec_get_bits(bs, 1);
    PRINT_NAME("render_and_frame_size_different");
    if (render_and_frame_size_different == 1) {
        frame_info->frame_size.render_width = dec_get_bits(bs, 16) + 1;
        frame_info->frame_size.render_height = dec_get_bits(bs, 16) + 1;
    }
    else {
        frame_info->frame_size.render_width
            = frame_info->frame_size.superres_upscaled_width;
        frame_info->frame_size.render_height = frame_info->frame_size.frame_height;
    }
    PRINT_FRAME("render_width", frame_info->frame_size.render_width);
    PRINT_FRAME("render_height", frame_info->frame_size.render_height);
}

static void frame_size_with_refs(bitstrm_t *bs, SeqHeader   *seq_header, FrameHeader
    *frame_info, int frame_size_override_flag)
{
    int found_ref;
    for (int i = 0; i < REFS_PER_FRAME; i++) {
        found_ref = dec_get_bits(bs, 1);
        PRINT_FRAME("found_ref", found_ref);
        if (found_ref == 1) {
            // width = frame_info->frame_size.superres_upscaled_width;
            // height = frame_info->frame_size.superres_upscaled_height;
            // TODO: Assign FrameWidth/Height
            break;
        }
    }
    if (found_ref == 0)
    {
        read_frame_size(bs, seq_header, frame_info, frame_size_override_flag);
        read_render_size(bs, frame_info);
    }
    else
    {
        superres_params(bs, seq_header, frame_info);
        compute_image_size(frame_info);
    }
}

// Read IP filter parameters
void read_interpolation_filter(bitstrm_t *bs, FrameHeader *frame_info)
{
    uint8_t is_filter_switchable;
    is_filter_switchable = dec_get_bits(bs, 1);
    PRINT_NAME("is_filter_switchable");
    if (is_filter_switchable == 1)
        frame_info->interpolation_filter = SWITCHABLE;
    else
        frame_info->interpolation_filter = dec_get_bits(bs, 2);
    PRINT_FRAME("interpolation_filter", frame_info->interpolation_filter);
}

// Read Tile information
void read_tile_info(bitstrm_t *bs, TilesInfo *tile_info, SeqHeader *seq_header,
    FrameHeader *frame_info)
{
    int start_sb, i, max_width, size_sb, max_height;
    uint32_t width_in_sbs_minus_1 = 0, height_in_sbs_minus_1 = 0;
    int sb_cols = seq_header->use_128x128_superblock ? ((frame_info->mi_cols + 31) >> 5)
        : ((frame_info->mi_cols + 15) >> 4);
    int sb_rows = seq_header->use_128x128_superblock ? ((frame_info->mi_rows + 31) >> 5)
        : ((frame_info->mi_rows + 15) >> 4);
    int sb_shift = seq_header->use_128x128_superblock ? 5 : 4;
    int sb_size = sb_shift + 2;
    int max_tile_area_sb = MAX_TILE_AREA >> (2 * sb_size);

    tile_info->max_tile_width_sb = MAX_TILE_WIDTH >> sb_size;
    tile_info->max_tile_height_sb = (MAX_TILE_AREA / MAX_TILE_WIDTH) >> sb_size;
    tile_info->min_log2_tile_cols = dec_tile_log2(tile_info->max_tile_width_sb, sb_cols);
    tile_info->max_log2_tile_cols = dec_tile_log2(1, MIN(sb_cols, MAX_TILE_COLS));
    tile_info->max_log2_tile_rows = dec_tile_log2(1, MIN(sb_rows, MAX_TILE_ROWS));
    tile_info->min_log2_tiles = MAX(tile_info->min_log2_tile_cols,
        dec_tile_log2(max_tile_area_sb, sb_rows * sb_cols));
    tile_info->uniform_tile_spacing_flag = dec_get_bits(bs, 1);
    PRINT_FRAME("uniform_tile_spacing_flag", tile_info->uniform_tile_spacing_flag);
    if (tile_info->uniform_tile_spacing_flag) {
        tile_info->tile_cols_log2 = tile_info->min_log2_tile_cols;
        while (tile_info->tile_cols_log2 < tile_info->max_log2_tile_cols) {
            PRINT_NAME("increment_tile_cols_log2");
            if (dec_get_bits(bs, 1) == 1)
                tile_info->tile_cols_log2++;
            else
                break;
        }
        int tile_width_sb = (sb_cols + (1 << tile_info->tile_cols_log2) - 1) >>
            tile_info->tile_cols_log2;
        assert(tile_width_sb <= tile_info->max_tile_width_sb); // Bitstream conformance
        i = 0;
        for (start_sb = 0; start_sb < sb_cols; start_sb += tile_width_sb) {
            tile_info->tile_col_start_sb[i] = start_sb << sb_shift;
            i += 1;
        }
        tile_info->tile_col_start_sb[i] = frame_info->mi_cols;
        tile_info->tile_cols = i;

        tile_info->min_log2_tile_rows = MAX(tile_info->min_log2_tiles -
            tile_info->tile_cols_log2, 0);
        tile_info->tile_rows_log2 = tile_info->min_log2_tile_rows;
        while (tile_info->tile_rows_log2 < tile_info->max_log2_tile_rows) {
            PRINT_NAME("Some read")
                if (dec_get_bits(bs, 1) == 1)
                    tile_info->tile_rows_log2++;
                else
                    break;
        }
        int tile_height_sb = (sb_rows + (1 << tile_info->tile_rows_log2) - 1) >>
            tile_info->tile_rows_log2;
        assert(tile_height_sb <= tile_info->max_tile_height_sb); // Bitstream conformance
        i = 0;
        for (start_sb = 0; start_sb < sb_rows; start_sb += tile_height_sb) {
            tile_info->tile_row_start_sb[i] = start_sb << sb_shift;
            i += 1;
        }
        tile_info->tile_row_start_sb[i] = frame_info->mi_rows;
        tile_info->tile_rows = i;
    }
    else {
        int widest_tile_sb = 0;
        start_sb = 0;
        for (i = 0; start_sb < sb_cols; i++) {
            tile_info->tile_col_start_sb[i] = start_sb << sb_shift;
            max_width = MIN(sb_cols - start_sb, tile_info->max_tile_width_sb);
            width_in_sbs_minus_1 = dec_get_bits_ns(bs, max_width);
            PRINT("width_in_sbs_minus_1", width_in_sbs_minus_1)
                size_sb = width_in_sbs_minus_1 + 1;
            widest_tile_sb = MAX(size_sb, widest_tile_sb);
            start_sb += size_sb;
        }
        assert(start_sb == sb_cols); // Bitstream conformance

        tile_info->tile_col_start_sb[i] = frame_info->mi_cols;
        tile_info->tile_cols = i;
        tile_info->tile_cols_log2 = dec_tile_log2(1, tile_info->tile_cols);
        if (tile_info->min_log2_tiles > 0)
            max_tile_area_sb = (sb_rows * sb_cols) >> (tile_info->min_log2_tiles + 1);
        else
            max_tile_area_sb = sb_rows * sb_cols;
        tile_info->max_tile_height_sb = MAX(max_tile_area_sb / widest_tile_sb, 1);

        start_sb = 0;
        for (i = 0; start_sb < sb_rows; i++) {
            tile_info->tile_row_start_sb[i] = start_sb << sb_shift;
            max_height = MIN(sb_rows - start_sb, tile_info->max_tile_height_sb);
            height_in_sbs_minus_1 = dec_get_bits_ns(bs, max_height);
            PRINT("height_in_sbs_minus_1", height_in_sbs_minus_1)
                size_sb = height_in_sbs_minus_1 + 1;
            start_sb += size_sb;
        }
        assert(start_sb == sb_rows); // Bitstream conformance

        tile_info->tile_row_start_sb[i] = frame_info->mi_rows;
        tile_info->tile_rows = i;
        tile_info->tile_rows_log2 = dec_tile_log2(1, tile_info->tile_rows);
    }

    // Bitstream conformance
    assert(tile_info->tile_cols <= MAX_TILE_ROWS);
    assert(tile_info->tile_rows <= MAX_TILE_COLS);

    if (tile_info->tile_cols_log2 > 0 || tile_info->tile_rows_log2 > 0) {
        tile_info->context_update_tile_id = dec_get_bits(bs,
            tile_info->tile_rows_log2 + tile_info->tile_cols_log2);
        PRINT("context_update_tile_id", tile_info->context_update_tile_id)
            tile_info->tile_size_bytes = dec_get_bits(bs, 2) + 1;
        PRINT("tile_size_bytes", tile_info->tile_size_bytes)
    }
    else
        tile_info->context_update_tile_id = 0;
    assert(tile_info->context_update_tile_id < (tile_info->tile_cols * tile_info->tile_rows));
}

uint8_t read_delta_q(bitstrm_t *bs)
{
    uint8_t delta_q;
    if (dec_get_bits(bs, 1))
        delta_q = dec_get_bits_su(bs, 7);
    else
        delta_q = 0;
    return delta_q;
}

void read_frame_delta_q_params(bitstrm_t *bs, FrameHeader *frame_info)
{
    frame_info->delta_q_params.delta_q_res = 0;
    frame_info->delta_q_params.delta_q_present = 0;
    if (frame_info->quantization_params.base_q_idx > 0) {
        frame_info->delta_q_params.delta_q_present = dec_get_bits(bs, 1);
    }
    if (frame_info->delta_q_params.delta_q_present) {
        frame_info->delta_q_params.delta_q_res = dec_get_bits(bs, 2);
        PRINT_FRAME("delta_q_res", 1 << frame_info->delta_q_params.delta_q_res);
    }
    PRINT_FRAME("delta_q_present", frame_info->delta_q_params.delta_q_present);
}

void read_frame_delta_lf_params(bitstrm_t *bs, FrameHeader *frame_info)
{
    frame_info->delta_lf_params.delta_lf_present = 0;
    frame_info->delta_lf_params.delta_lf_res = 0;
    frame_info->delta_lf_params.delta_lf_multi = 0;
    if (frame_info->delta_q_params.delta_q_present) {
        if (!frame_info->allow_intrabc) {
            frame_info->delta_lf_params.delta_lf_present = dec_get_bits(bs, 1);
            PRINT_FRAME("delta_lf_present_flag", frame_info->delta_lf_params.delta_lf_present);
        }
        if (frame_info->delta_lf_params.delta_lf_present) {
            frame_info->delta_lf_params.delta_lf_res = dec_get_bits(bs, 2);
            frame_info->delta_lf_params.delta_lf_multi = dec_get_bits(bs, 1);
            PRINT_FRAME("delta_lf_res", frame_info->delta_lf_params.delta_lf_res);
            PRINT_FRAME("delta_lf_multi", frame_info->delta_lf_params.delta_lf_multi);
        }
    }
}

void read_quantization_params(bitstrm_t *bs, QuantizationParams *quant_params,
    EbColorConfig *color_info, int num_planes)
{
    uint8_t diff_uv_delta;
    quant_params->base_q_idx = dec_get_bits(bs, 8);
    PRINT_FRAME("base_q_idx", quant_params->base_q_idx);
    quant_params->delta_q_y_dc = read_delta_q(bs);
    PRINT_FRAME("delta_q_y_dc", quant_params->delta_q_y_dc);
    if (num_planes > 1)
    {
        if (color_info->separate_uv_delta_q) {
            diff_uv_delta = dec_get_bits(bs, 1);
            PRINT_FRAME("diff_uv_delta", diff_uv_delta);
        }
        else
            diff_uv_delta = 0;
        quant_params->delta_q_u_dc = read_delta_q(bs);
        quant_params->delta_q_u_ac = read_delta_q(bs);
        if (diff_uv_delta) {
            quant_params->delta_q_v_dc = read_delta_q(bs);
            quant_params->delta_q_v_ac = read_delta_q(bs);
        }
        else {
            quant_params->delta_q_v_dc = quant_params->delta_q_u_dc;
            quant_params->delta_q_v_ac = quant_params->delta_q_u_ac;
        }
    }
    else {
        quant_params->delta_q_u_dc = 0;
        quant_params->delta_q_u_ac = 0;
        quant_params->delta_q_v_dc = 0;
        quant_params->delta_q_v_ac = 0;
    }
    PRINT_FRAME("u_dc_delta_q", quant_params->delta_q_u_dc);
    PRINT_FRAME("u_ac_delta_q", quant_params->delta_q_u_ac);
    PRINT_FRAME("v_dc_delta_q", quant_params->delta_q_v_dc);
    PRINT_FRAME("v_ac_delta_q", quant_params->delta_q_v_ac);
    quant_params->using_qmatrix = dec_get_bits(bs, 1);
    PRINT_FRAME("using_qmatrix", quant_params->using_qmatrix);
    if (quant_params->using_qmatrix) {
        quant_params->qm_y = dec_get_bits(bs, 4);
        quant_params->qm_u = dec_get_bits(bs, 4);
        if (!color_info->separate_uv_delta_q)
            quant_params->qm_v = quant_params->qm_u;
        else
            quant_params->qm_v = dec_get_bits(bs, 4);
    }
    else {
        quant_params->qm_y = 0;
        quant_params->qm_u = 0;
        quant_params->qm_v = 0;
    }
    PRINT_FRAME("qm_y", quant_params->qm_y);
    PRINT_FRAME("qm_u", quant_params->qm_u);
    PRINT_FRAME("qm_v", quant_params->qm_v);
}

void read_segmentation_params(bitstrm_t *bs, SegmentationParams *seg_params, FrameHeader *frame_info)
{
    int feature_value, feature_enabled, clippedValue, bitsToRead, limit, i, j;
    seg_params->segmentation_enabled = dec_get_bits(bs, 1);
    PRINT_FRAME("segmentation_enabled", seg_params->segmentation_enabled);
    if (seg_params->segmentation_enabled == 1) {
        if (frame_info->primary_ref_frame == PRIMARY_REF_NONE) {
            seg_params->segmentation_update_map = 1;
            seg_params->segmentation_temporal_update = 0;
            seg_params->segmentation_update_data = 1;
        }
        else {
            seg_params->segmentation_update_map = dec_get_bits(bs, 1);
            if (seg_params->segmentation_update_map)
                seg_params->segmentation_temporal_update = dec_get_bits(bs, 1);
            else
                seg_params->segmentation_temporal_update = 0;
            seg_params->segmentation_update_data = dec_get_bits(bs, 1);
        }
        PRINT_FRAME("segmentation_update_map", seg_params->segmentation_update_map);
        PRINT_FRAME("segmentation_temporal_update",
            seg_params->segmentation_temporal_update);
        PRINT_FRAME("segmentation_update_data", seg_params->segmentation_update_data);
        if (seg_params->segmentation_update_data == 1) {
            for (i = 0; i < MAX_SEGMENTS; i++) {
                for (j = 0; j < SEG_LVL_MAX; j++) {
                    feature_value = 0;
                    feature_enabled = dec_get_bits(bs, 1);
                    PRINT_FRAME("feature_enabled", feature_enabled);
                    seg_params->feature_enabled[i][j] = feature_enabled;
                    clippedValue = 0;
                    if (feature_enabled == 1) {
                        bitsToRead = segmentation_feature_bits[j];
                        limit = segmentation_feature_max[j];
                        if (segmentation_feature_signed[j] == 1) {
                            feature_value = dec_get_bits_su(bs, 1 + bitsToRead);
                            clippedValue = CLIP3(-limit, limit, feature_value);
                        }
                        else {
                            feature_value = dec_get_bits(bs, bitsToRead);
                            clippedValue = CLIP3(0, limit, feature_value);
                        }
                        PRINT_FRAME("feature_value", feature_value)
                    }
                    if (clippedValue < 0) {
                        assert(segmentation_feature_signed[j]);
                        assert(-clippedValue <= segmentation_feature_max[j]);
                    }
                    else
                        assert(clippedValue <= segmentation_feature_max[j]);
                    seg_params->feature_data[i][j] = clippedValue;
                }
            }
        }
    }
    else {
        seg_params->seg_id_pre_skip = 0;
        for (i = 0; i < MAX_SEGMENTS; i++) {
            for (j = 0; j < SEG_LVL_MAX; j++) {
                seg_params->feature_enabled[i][j] = 0;
                seg_params->feature_data[i][j] = 0;
            }
        }
    }

    seg_params->last_active_seg_id = 0;
    seg_params->seg_id_pre_skip = 0;
    for (i = 0; i < MAX_SEGMENTS; i++) {
        for (j = 0; j < SEG_LVL_MAX; j++) {
            if (seg_params->feature_enabled[i][j]) {
                seg_params->last_active_seg_id = i;
                if (j >= SEG_LVL_REF_FRAME)
                    seg_params->seg_id_pre_skip = 1;
            }
        }
    }
}

void read_loop_filter_params(bitstrm_t *bs, FrameHeader *frame_info, int num_planes)
{
    int i;
    if (frame_info->coded_lossless || frame_info->allow_intrabc) {
        frame_info->loop_filter_params.filter_level[0] = 0;
        frame_info->loop_filter_params.filter_level[1] = 0;
        frame_info->loop_filter_params.ref_deltas[INTRA_FRAME] = 1;
        frame_info->loop_filter_params.ref_deltas[LAST_FRAME] = 0;
        frame_info->loop_filter_params.ref_deltas[LAST2_FRAME] = 0;
        frame_info->loop_filter_params.ref_deltas[LAST3_FRAME] = 0;
        frame_info->loop_filter_params.ref_deltas[BWDREF_FRAME] = 0;
        frame_info->loop_filter_params.ref_deltas[GOLDEN_FRAME] = -1;
        frame_info->loop_filter_params.ref_deltas[ALTREF_FRAME] = -1;
        frame_info->loop_filter_params.ref_deltas[ALTREF2_FRAME] = -1;
        for (i = 0; i < 2; i++)
            frame_info->loop_filter_params.mode_deltas[i] = 0;
        return;
    }
    frame_info->loop_filter_params.filter_level[0] = dec_get_bits(bs, 6);
    frame_info->loop_filter_params.filter_level[1] = dec_get_bits(bs, 6);
    PRINT_FRAME("loop_filter_level[0]", frame_info->loop_filter_params.filter_level[0]);
    PRINT_FRAME("loop_filter_level[1]", frame_info->loop_filter_params.filter_level[1]);
    if (num_planes > 1) {
        if (frame_info->loop_filter_params.filter_level[0] ||
            frame_info->loop_filter_params.filter_level[1]) {
            frame_info->loop_filter_params.filter_level_u = dec_get_bits(bs, 6);
            frame_info->loop_filter_params.filter_level_v = dec_get_bits(bs, 6);
            PRINT_FRAME("loop_filter_level[2]",
                frame_info->loop_filter_params.filter_level_u);
            PRINT_FRAME("loop_filter_level[3]",
                frame_info->loop_filter_params.filter_level_v);
        }
    }
    frame_info->loop_filter_params.sharpness_level = dec_get_bits(bs, 3);
    frame_info->loop_filter_params.mode_ref_delta_enabled = dec_get_bits(bs, 1);
    PRINT_FRAME("loop_filter_sharpness",
        frame_info->loop_filter_params.sharpness_level);
    PRINT_FRAME("loop_filter_delta_enabled",
        frame_info->loop_filter_params.mode_ref_delta_enabled);

    if (frame_info->loop_filter_params.mode_ref_delta_enabled == 1) {
        frame_info->loop_filter_params.mode_ref_delta_update = dec_get_bits(bs, 1);
        PRINT_FRAME("loop_filter_delta_update",
            frame_info->loop_filter_params.mode_ref_delta_update);

        if (frame_info->loop_filter_params.mode_ref_delta_update == 1) {
            for (i = 0; i < TOTAL_REFS_PER_FRAME; i++) {
                PRINT_NAME("Some read");
                if (dec_get_bits(bs, 1) == 1) {
                    frame_info->loop_filter_params.ref_deltas[i]
                        = dec_get_bits_su(bs, 1 + 6);
                    PRINT_FRAME("frame_info->loop_filter_params.loop_filter_ref_deltas[i]",
                        frame_info->loop_filter_params.ref_deltas[i]);
                }
            }
            for (i = 0; i < 2; i++) {
                PRINT_NAME("Some read");
                if (dec_get_bits(bs, 1) == 1) {
                    frame_info->loop_filter_params.mode_deltas[i]
                        = dec_get_bits_su(bs, 1 + 6);
                    PRINT_FRAME("loop_filter_mode_deltas[i]",
                        frame_info->loop_filter_params.mode_deltas[i]);
                }
            }
        }
    }
}

void read_tx_mode(bitstrm_t *bs, FrameHeader *frame_info)
{
    if (frame_info->coded_lossless == 1)
        frame_info->tx_mode = ONLY_4X4;
    else {
        if (dec_get_bits(bs, 1))
            frame_info->tx_mode = TX_MODE_SELECT;
        else
            frame_info->tx_mode = TX_MODE_LARGEST;
    }
    PRINT_FRAME("tx_mode", frame_info->tx_mode);
}

void read_lr_params(bitstrm_t *bs, FrameHeader *frame_info, SeqHeader *seq_header,
    int num_planes)
{
    int i, uses_lr, uses_chroma_lr, lr_type, lr_unit_shift, lr_unit_extra_shift,
        lr_uv_shift;

    if (frame_info->coded_lossless || frame_info->allow_intrabc ||
        !seq_header->enable_restoration) {
        frame_info->lr_params[0].frame_restoration_type = RESTORE_NONE;
        frame_info->lr_params[1].frame_restoration_type = RESTORE_NONE;
        frame_info->lr_params[2].frame_restoration_type = RESTORE_NONE;
        uses_lr = 0;
        return;
    }
    uses_lr = 0;
    uses_chroma_lr = 0;
    for (i = 0; i < num_planes; i++) {
        lr_type = dec_get_bits(bs, 2);
        PRINT_NAME("lr_type")
        frame_info->lr_params[i].frame_restoration_type = remap_lr_type[lr_type];
        PRINT_FRAME("frame_restoration_type", frame_info->lr_params[i].frame_restoration_type);
        if (frame_info->lr_params[i].frame_restoration_type != RESTORE_NONE) {
            uses_lr = 1;
            if (i > 0)
                uses_chroma_lr = 1;
        }
    }
    if (uses_lr) {
        if (seq_header->use_128x128_superblock) {
            lr_unit_shift = dec_get_bits(bs, 1);
            lr_unit_shift++;
        }
        else {
            lr_unit_shift = dec_get_bits(bs, 1);
            if (lr_unit_shift) {
                lr_unit_extra_shift = dec_get_bits(bs, 1);
                PRINT_FRAME("lr_unit_extra_shift", lr_unit_extra_shift);
                lr_unit_shift += lr_unit_extra_shift;
            }
        }
        frame_info->lr_params[0].loop_restoration_size
            = (RESTORATION_TILESIZE_MAX >> (2 - lr_unit_shift));
        PRINT_FRAME("restoration_unit_size", frame_info->lr_params[0].loop_restoration_size);
        if (seq_header->color_config.subsampling_x &&
            seq_header->color_config.subsampling_y && uses_chroma_lr) {
            lr_uv_shift = dec_get_bits(bs, 1);
        }
        else
            lr_uv_shift = 0;
        frame_info->lr_params[1].loop_restoration_size
            = frame_info->lr_params[0].loop_restoration_size >> lr_uv_shift;
        frame_info->lr_params[2].loop_restoration_size
            = frame_info->lr_params[0].loop_restoration_size >> lr_uv_shift;
    }
    else {
        frame_info->lr_params[0].loop_restoration_size = RESTORATION_TILESIZE_MAX;
        frame_info->lr_params[1].loop_restoration_size = RESTORATION_TILESIZE_MAX;
        frame_info->lr_params[2].loop_restoration_size = RESTORATION_TILESIZE_MAX;
    }
    PRINT_FRAME("cm->rst_info[1].restoration_unit_size", frame_info->lr_params[1].loop_restoration_size);
}

void read_frame_cdef_params(bitstrm_t *bs, FrameHeader *frame_info, SeqHeader *seq_header,
    int num_planes)
{
    int i;
    if (frame_info->coded_lossless || frame_info->allow_intrabc ||
        !seq_header->enable_cdef) {
        frame_info->CDEF_params.cdef_bits = 0;
        frame_info->CDEF_params.cdef_y_strength[0] = 0;
        frame_info->CDEF_params.cdef_y_strength[4] = 0;
        frame_info->CDEF_params.cdef_uv_strength[0] = 0;
        frame_info->CDEF_params.cdef_uv_strength[4] = 0;
        frame_info->CDEF_params.cdef_damping = 3;
        return;
    }
    frame_info->CDEF_params.cdef_damping = dec_get_bits(bs, 2) + 3;
    frame_info->CDEF_params.cdef_bits = dec_get_bits(bs, 2);
    PRINT_FRAME("cdef_damping", frame_info->CDEF_params.cdef_damping);
    PRINT_FRAME("cdef_bits", frame_info->CDEF_params.cdef_bits);
    for (i = 0; i < (1 << frame_info->CDEF_params.cdef_bits); i++) {
        frame_info->CDEF_params.cdef_y_strength[i] = dec_get_bits(bs, 6);
        PRINT_FRAME("Primary Y cdef", frame_info->CDEF_params.cdef_y_strength[i]);
        if (frame_info->CDEF_params.cdef_y_strength[i + 8] == 3)
            frame_info->CDEF_params.cdef_y_strength[i + 8] += 1;
        if (num_planes > 1) {
            frame_info->CDEF_params.cdef_uv_strength[i] = dec_get_bits(bs, 6);
            PRINT_FRAME("Primary UV cdef", frame_info->CDEF_params.cdef_uv_strength[i]);
            if (frame_info->CDEF_params.cdef_uv_strength[i + 8] == 3)
                frame_info->CDEF_params.cdef_uv_strength[i + 8] += 1;
        }
    }
}

int decode_subexp(bitstrm_t *bs, int numSyms)
{
    int i = 0, mk = 0, k = 3, b2, a;

    while (1) {
        b2 = i ? k + i - 1 : k;
        a = 1 << b2;
        if (numSyms <= mk + 3 * a) {
            PRINT_NAME("subexp_final_bits");
            return dec_get_bits_ns(bs, numSyms - mk) + mk;
        }
        else {
            PRINT_NAME("subexp_more_bits");
            if (dec_get_bits(bs, 1)) {
                i++;
                mk += a;
            }
            else {
                PRINT_NAME("subexp_bits");
                return dec_get_bits(bs, b2) + mk;
            }
        }
    }
    return 0;
}

int decode_unsigned_subexp_with_ref(bitstrm_t *bs, int mx, int r)
{
    int v = decode_subexp(bs, mx);
    if ((r << 1) <= mx)
        return inverse_recenter(r, v);
    else
        return mx - 1 - inverse_recenter(mx - 1 - r, v);
}

int decode_signed_subexp_with_ref(bitstrm_t *bs, int low, int high, int r)
{
    int x = decode_unsigned_subexp_with_ref(bs, high - low, r - low);
    return x + low;
}

void read_global_param(bitstrm_t *bs, EbDecHandle *dec_handle,
    TransformationType type, int ref_idx, int idx, FrameHeader *frame_info)
{
    GlobalMotionParams prev_gm_params[ALTREF_FRAME + 1]; // Need to initialize in setup_past_independence() section: 6.8.2
    for (int ref = LAST_FRAME; ref <= ALTREF_FRAME; ref++)
        for (int i = 0; i <= 5; i++)
            prev_gm_params[ref].gm_params[i] = ((i % 3 == 2) ? 1 << WARPEDMODEL_PREC_BITS : 0);

    int abs_bits = GM_ABS_ALPHA_BITS, prec_diff, round, sub, mx, r;
    int prec_bits = GM_ALPHA_PREC_BITS;
    if (idx < 2) {
        if (type == TRANSLATION) {
            abs_bits = GM_ABS_TRANS_ONLY_BITS - !frame_info->allow_high_precision_mv;
            prec_bits = GM_TRANS_ONLY_PREC_BITS - !frame_info->allow_high_precision_mv;
        }
        else {
            abs_bits = GM_ABS_TRANS_BITS;
            prec_bits = GM_TRANS_PREC_BITS;
        }
    }

    prec_diff = WARPEDMODEL_PREC_BITS - prec_bits;
    round = (idx % 3) == 2 ? (1 << WARPEDMODEL_PREC_BITS) : 0;
    sub = (idx % 3) == 2 ? (1 << prec_bits) : 0;
    mx = (1 << abs_bits);

    EbDecPicBuf *cur_buf = dec_handle->cur_pic_buf[0];
    EbDecPicBuf *prev_buf = NULL;
    if (frame_info->primary_ref_frame != PRIMARY_REF_NONE) {
        prev_buf = get_ref_frame_buf(dec_handle, frame_info->primary_ref_frame + 1);
        if (prev_buf == NULL)
            assert(0);
    }

    GlobalMotionParams *gm_params = prev_buf != NULL ? prev_buf->global_motion : prev_gm_params;
    r = (gm_params[ref_idx].gm_params[idx] >> prec_diff) - sub;
    cur_buf->global_motion[ref_idx].gm_params[idx] =
        (decode_signed_subexp_with_ref(bs, -mx, mx + 1, r) <<
         prec_diff) + round;
}

void read_global_motion_params(bitstrm_t *bs, EbDecHandle *dec_handle,
    FrameHeader *frame_info, int FrameIsIntra)
{
    int ref, i;
    TransformationType type;
    EbDecPicBuf *cur_buf = dec_handle->cur_pic_buf[0];
    for (ref = LAST_FRAME; ref <= ALTREF_FRAME; ref++) {
        cur_buf->global_motion[ref].gm_type = IDENTITY;
        for (i = 0; i < 6; i++) {
            cur_buf->global_motion[ref].gm_params[i] =
                ((i % 3 == 2) ? 1 << WARPEDMODEL_PREC_BITS : 0);
        }
    }
    if (FrameIsIntra) return;
    for (ref = LAST_FRAME; ref <= ALTREF_FRAME; ref++) {
        PRINT_NAME("Some read");
        if (dec_get_bits(bs, 1)) {
            PRINT_NAME("Some read");
            if (dec_get_bits(bs, 1))
                type = ROTZOOM;
            else
                type = dec_get_bits(bs, 1) ? TRANSLATION : AFFINE;
        }
        else
            type = IDENTITY;
        PRINT_FRAME("Transform_type", type);

        cur_buf->global_motion[ref].gm_type = type;

        if (type >= ROTZOOM) {
            read_global_param(bs, dec_handle, type, ref, 2, frame_info);
            read_global_param(bs, dec_handle, type, ref, 3, frame_info);
        }
        if (type >= AFFINE) {
            read_global_param(bs, dec_handle, type, ref, 4, frame_info);
            read_global_param(bs, dec_handle, type, ref, 5, frame_info);
        }
        else {
            cur_buf->global_motion[ref].gm_params[4]
                = -cur_buf->global_motion[ref].gm_params[3];
            cur_buf->global_motion[ref].gm_params[5]
                = cur_buf->global_motion[ref].gm_params[2];
        }
        if (type >= TRANSLATION) {
            read_global_param(bs, dec_handle, type, ref, 0, frame_info);
            read_global_param(bs, dec_handle, type, ref, 1, frame_info);
        }

        /* TODO: Can we remove one of the type? */
        /* Convert to EbWarpedMotionParams type */
        {
            EbWarpedMotionParams *wm_global = &dec_handle->master_frame_buf.
                                cur_frame_bufs[0].global_motion_warp[ref];
            wm_global->wmtype = cur_buf->global_motion[ref].gm_type;
            memcpy(wm_global->wmmat, cur_buf->global_motion[ref].gm_params,
                sizeof(cur_buf->global_motion[ref].gm_params));
            int return_val = eb_get_shear_params(wm_global);
            assert(1 == return_val);
            (void)return_val;
        }
    }
}

uint8_t read_frame_reference_mode(bitstrm_t *bs, int FrameIsIntra)
{
    if (FrameIsIntra)
        return SINGLE_REFERENCE;
    else
        return dec_get_bits(bs, 1);
}

// Read skip mode paramters
void read_skip_mode_params(bitstrm_t *bs, FrameHeader *frame_info, int FrameIsIntra,
    SeqHeader *seq_header, int reference_select)
{
    int forwardIdx = -1, backwardIdx = -1, secondForwardIdx = -1;
    int ref_hint, forwardHint = -1,
        backwardHint = INT_MAX , secondForwardHint = -1;
    int i;
    if (FrameIsIntra || !reference_select ||
        !seq_header->order_hint_info.enable_order_hint)
        frame_info->skip_mode_params.skip_mode_allowed = 0;
    else {
        forwardIdx = backwardIdx = -1;
        //frame_info->skip_mode_params.skip_mode_allowed = 1;
        for (i = 0; i < REFS_PER_FRAME; i++) {
            ref_hint = frame_info->ref_order_hint[frame_info->ref_frame_idx[i]];
            if (get_relative_dist(&seq_header->order_hint_info, ref_hint,
                                    frame_info->order_hint) < 0)
            {
                    if (forwardIdx < 0 || get_relative_dist(&seq_header->
                        order_hint_info, ref_hint, forwardHint) > 0)
                {
                        forwardIdx = i;
                        forwardHint = ref_hint;
                }
            }
            else if (get_relative_dist(&seq_header->order_hint_info, ref_hint,
                frame_info->order_hint) > 0)
            {
                    if (backwardIdx < 0 || get_relative_dist(&seq_header->order_hint_info,
                        ref_hint, backwardHint) < 0)
                {
                        backwardIdx = i;
                        backwardHint = ref_hint;
                }
            }
        }
        if (forwardIdx < 0)
            frame_info->skip_mode_params.skip_mode_allowed = 0;
        else if (backwardIdx >= 0) {
            frame_info->skip_mode_params.skip_mode_allowed = 1;
            frame_info->skip_mode_params.ref_frame_idx_0
                = LAST_FRAME + MIN(forwardIdx, backwardIdx);
            frame_info->skip_mode_params.ref_frame_idx_1
                = LAST_FRAME + MAX(forwardIdx, backwardIdx);
        }
        else {
            secondForwardIdx = -1;
            for (i = 0; i < REFS_PER_FRAME; i++) {
            ref_hint = frame_info->ref_order_hint[frame_info->ref_frame_idx[i]];
                if (get_relative_dist(&seq_header->order_hint_info, ref_hint,
                    forwardHint) < 0)
                {
                    if (secondForwardIdx < 0 || get_relative_dist(&seq_header->
                        order_hint_info, ref_hint, secondForwardHint) > 0)
                    {
                        secondForwardIdx = i;
                        secondForwardHint = ref_hint;
                    }
                }
            }
            if (secondForwardIdx < 0) {
                frame_info->skip_mode_params.skip_mode_allowed = 0;
            }
            else {
                frame_info->skip_mode_params.skip_mode_allowed = 1;
                frame_info->skip_mode_params.ref_frame_idx_0
                    = LAST_FRAME + MIN(forwardIdx, secondForwardIdx);
                frame_info->skip_mode_params.ref_frame_idx_1
                    = LAST_FRAME + MAX(forwardIdx, secondForwardIdx);
            }
        }
    }

    if (frame_info->skip_mode_params.skip_mode_allowed)
        frame_info->skip_mode_params.skip_mode_flag = dec_get_bits(bs, 1);
    else
        frame_info->skip_mode_params.skip_mode_flag = 0;
    PRINT_FRAME("skip_mode_present", frame_info->skip_mode_params.skip_mode_flag);
}

// Read film grain parameters
void read_film_grain_params(bitstrm_t *bs, aom_film_grain_t *grain_params,
    SeqHeader *seq_header, FrameHeader *frame_info)
{
    int /*film_grain_params_ref_idx,*/ temp_grain_seed, i, numPosLuma, numPosChroma;

    if (!seq_header->film_grain_params_present || (!frame_info->show_frame &&
        !frame_info->showable_frame)) {
        memset(grain_params, 0, sizeof(*grain_params));
        return;
    }
    grain_params->apply_grain = dec_get_bits(bs, 1);
    PRINT_FRAME("apply_grain", grain_params->apply_grain);

    if (!grain_params->apply_grain) {
        memset(grain_params, 0, sizeof(*grain_params));
        return;
    }

    grain_params->random_seed = dec_get_bits(bs, 16);
    PRINT_FRAME("grain_seed", grain_params->random_seed);
    if (frame_info->frame_type == INTER_FRAME)
        grain_params->update_parameters = dec_get_bits(bs, 1);
    else
        grain_params->update_parameters = 1;
    PRINT_FRAME("update_parameters", grain_params->update_parameters);
    if (!grain_params->update_parameters) {
        /*film_grain_params_ref_idx = */dec_get_bits(bs, 3);
        /*PRINT_FRAME("film_grain_params_ref_idx", film_grain_params_ref_idx);*/
        temp_grain_seed = grain_params->random_seed;
        // TODO: Handle while implementing Inter
        // load_grain_params( film_grain_params_ref_idx );
        grain_params->random_seed = temp_grain_seed;
        return;
    }
    grain_params->num_y_points = dec_get_bits(bs, 4);
    assert(grain_params->num_y_points <= 14);
    PRINT_FRAME("num_y_points", grain_params->num_y_points);
    for (i = 0; i < grain_params->num_y_points; i++) {
        grain_params->scaling_points_y[i][0] = dec_get_bits(bs, 8);
        grain_params->scaling_points_y[i][1] = dec_get_bits(bs, 8);
        if (i > 0)
            assert(grain_params->scaling_points_y[i][0] > grain_params->scaling_points_y[i - 1][0]);
        PRINT_FRAME("scaling_points_y[i][0]", grain_params->scaling_points_y[i][0]);
        PRINT_FRAME("scaling_points_y[i][1]", grain_params->scaling_points_y[i][1]);
    }
    if (seq_header->color_config.mono_chrome)
        grain_params->chroma_scaling_from_luma = 0;
    else
        grain_params->chroma_scaling_from_luma = dec_get_bits(bs, 1);
    PRINT_FRAME("chroma_scaling_from_luma", grain_params->chroma_scaling_from_luma);

    if (seq_header->color_config.mono_chrome || grain_params->chroma_scaling_from_luma
        || ( (seq_header->color_config.subsampling_y == 1) &&
        (seq_header->color_config.subsampling_x == 1) && grain_params->num_y_points == 0)) {
        grain_params->num_cb_points = 0;
        grain_params->num_cr_points = 0;
    }
    else {
        grain_params->num_cb_points = dec_get_bits(bs, 4);
        PRINT_FRAME("num_cb_points", grain_params->num_cb_points);
        assert(grain_params->num_cb_points <= 10);
        for (i = 0; i < grain_params->num_cb_points; i++) {
            grain_params->scaling_points_cb[i][0] = dec_get_bits(bs, 8);
            grain_params->scaling_points_cb[i][1] = dec_get_bits(bs, 8);
            PRINT_FRAME("scaling_points_cb[i][0]", grain_params->scaling_points_cb[i][0]);
            PRINT_FRAME("scaling_points_cb[i][1]", grain_params->scaling_points_cb[i][1]);
            if (i > 0)
                assert(grain_params->scaling_points_cb[i][0] >
                    grain_params->scaling_points_cb[i - 1][0]);
        }
        grain_params->num_cr_points = dec_get_bits(bs, 4);
        PRINT_FRAME("num_cr_points", grain_params->num_cr_points);
        assert(grain_params->num_cr_points <= 14);
        for (i = 0; i < grain_params->num_cr_points; i++) {
            grain_params->scaling_points_cr[i][0] = dec_get_bits(bs, 8);
            grain_params->scaling_points_cr[i][1] = dec_get_bits(bs, 8);
            PRINT_FRAME("scaling_points_cr[i][0]", grain_params->scaling_points_cr[i][0]);
            PRINT_FRAME("scaling_points_cr[i][1]", grain_params->scaling_points_cr[i][1]);
            if (i > 0)
                assert(grain_params->scaling_points_cr[i][0] >
                    grain_params->scaling_points_cr[i - 1][0]);
        }
    }

    if ((seq_header->color_config.subsampling_x == 1) &&
        (seq_header->color_config.subsampling_y == 1) &&
        (((grain_params->num_cb_points == 0) && (grain_params->num_cr_points != 0)) ||
        ((grain_params->num_cb_points != 0) && (grain_params->num_cr_points == 0))))
        return;// EB_DecUnsupportedBitstream;

    grain_params->scaling_shift = dec_get_bits(bs, 2) + 8;
    grain_params->ar_coeff_lag = dec_get_bits(bs, 2);
    PRINT_FRAME("scaling_shift", grain_params->grain_scale_shift);
    PRINT_FRAME("ar_coeff_lag", grain_params->ar_coeff_lag);

    numPosLuma = 2 * grain_params->ar_coeff_lag * (grain_params->ar_coeff_lag + 1);
    if (grain_params->num_y_points) {
        numPosChroma = numPosLuma + 1;
        for (i = 0; i < numPosLuma; i++) {
            grain_params->ar_coeffs_y[i] = dec_get_bits(bs, 8) - 128;
            PRINT_FRAME("ar_coeffs_y[i]", grain_params->ar_coeffs_y[i]);
        }
    }
    else
        numPosChroma = numPosLuma;
    if (grain_params->chroma_scaling_from_luma || grain_params->num_cb_points) {
        for (i = 0; i < numPosChroma; i++) {
            grain_params->ar_coeffs_cb[i] = dec_get_bits(bs, 8) - 128;
            PRINT_FRAME("ar_coeffs_cb[i]", grain_params->ar_coeffs_cb[i]);
        }
    }
    if (grain_params->chroma_scaling_from_luma || grain_params->num_cr_points) {
        for (i = 0; i < numPosChroma; i++) {
            grain_params->ar_coeffs_cr[i] = dec_get_bits(bs, 8) - 128;
            PRINT_FRAME("ar_coeffs_cr[i]", grain_params->ar_coeffs_cr[i]);
        }
    }
    grain_params->ar_coeff_shift = dec_get_bits(bs, 2) + 6;
    grain_params->grain_scale_shift = dec_get_bits(bs, 2);
    PRINT_FRAME("ar_coeff_shift", grain_params->ar_coeff_shift);
    PRINT_FRAME("grain_scale_shift", grain_params->grain_scale_shift);
    if (grain_params->num_cb_points) {
        grain_params->cb_mult = dec_get_bits(bs, 8);
        grain_params->cb_luma_mult = dec_get_bits(bs, 8);
        grain_params->cb_offset = dec_get_bits(bs, 9);
        PRINT_FRAME("cb_mult", grain_params->cb_mult);
        PRINT_FRAME("cb_luma_mult", grain_params->cb_luma_mult);
        PRINT_FRAME("cb_offset", grain_params->cb_offset);
    }
    if (grain_params->num_cr_points) {
        grain_params->cr_mult = dec_get_bits(bs, 8);
        grain_params->cr_luma_mult = dec_get_bits(bs, 8);
        grain_params->cr_offset = dec_get_bits(bs, 9);
        PRINT_FRAME("cr_mult", grain_params->cr_mult);
        PRINT_FRAME("cr_luma_mult", grain_params->cr_luma_mult);
        PRINT_FRAME("cr_offset", grain_params->cr_offset);
    }
    grain_params->overlap_flag = dec_get_bits(bs, 1);
    grain_params->clip_to_restricted_range = dec_get_bits(bs, 1);
    PRINT_FRAME("overlap_flag", grain_params->overlap_flag);
    PRINT_FRAME("clip_to_restricted_range", grain_params->clip_to_restricted_range);
}

int seg_feature_active_idx(SegmentationParams *seg_params, int segment_id,
    SEG_LVL_FEATURES feature_id)
{
    return seg_params->segmentation_enabled &&
        (seg_params->feature_enabled[segment_id][feature_id]);
}

int get_qindex(SegmentationParams *seg_params, int segment_id, int base_q_idx)
{
    if (seg_feature_active_idx(seg_params, segment_id, SEG_LVL_ALT_Q)) {
        int data = seg_params->feature_data[segment_id][SEG_LVL_ALT_Q];
        int q_index = base_q_idx + data;
        return clamp(q_index, 0, MAXQ);
    }
    else
        return base_q_idx;
}

EbErrorType reset_parse_ctx(FRAME_CONTEXT *frm_ctx, uint8_t base_qp) {
    EbErrorType return_error = EB_ErrorNone;

    eb_av1_default_coef_probs(frm_ctx, base_qp);
    init_mode_probs(frm_ctx);

    return return_error;
}

void setup_frame_sign_bias(EbDecHandle *dec_handle) {
    MvReferenceFrame ref_frame;
    for (ref_frame = LAST_FRAME; ref_frame <= ALTREF_FRAME; ++ref_frame) {
        const EbDecPicBuf *const buf = get_ref_frame_buf(dec_handle, ref_frame);
        if (dec_handle->seq_header.order_hint_info.enable_order_hint && buf != NULL) {
            const int ref_order_hint = buf->order_hint;
            dec_handle->frame_header.ref_frame_sign_bias[ref_frame] =
                (get_relative_dist(&dec_handle->seq_header.order_hint_info, ref_order_hint,
                (int)dec_handle->cur_pic_buf[0]->order_hint) <= 0)
                ? 0
                : 1;
        }
        else {
            dec_handle->frame_header.ref_frame_sign_bias[ref_frame] = 0;
        }
    }
}

void setup_past_independence(EbDecHandle *dec_handle_ptr,
                             FrameHeader *frame_info)
{
    int ref, i, j;
    EbDecPicBuf *cur_buf = dec_handle_ptr->cur_pic_buf[0];
    SegmentationParams seg = dec_handle_ptr->frame_header.segmentation_params;

    for (i = 0; i < MAX_SEGMENTS; i++)
        for (j = 0; j < SEG_LVL_MAX; j++)
        {
            seg.feature_data[i][j] = 0;
            seg.feature_enabled[i][j] = 0;
        }
    UNUSED(seg);

    for (ref = LAST_FRAME; ref <= ALTREF_FRAME; ref++)
        cur_buf->global_motion[ref].gm_type = IDENTITY;

    frame_info->loop_filter_params.mode_ref_delta_enabled = 1;
    frame_info->loop_filter_params.ref_deltas[INTRA_FRAME] = 1;
    frame_info->loop_filter_params.ref_deltas[LAST_FRAME] = 0;
    frame_info->loop_filter_params.ref_deltas[LAST2_FRAME] = 0;
    frame_info->loop_filter_params.ref_deltas[LAST3_FRAME] = 0;
    frame_info->loop_filter_params.ref_deltas[BWDREF_FRAME] = 0;
    frame_info->loop_filter_params.ref_deltas[GOLDEN_FRAME] = -1;
    frame_info->loop_filter_params.ref_deltas[ALTREF_FRAME] = -1;
    frame_info->loop_filter_params.ref_deltas[ALTREF2_FRAME] = -1;

    frame_info->loop_filter_params.mode_deltas[0] = 0;
    frame_info->loop_filter_params.mode_deltas[1] = 0;
}

void read_uncompressed_header(bitstrm_t *bs, EbDecHandle *dec_handle_ptr,
                              ObuHeader *obu_header, int num_planes)
{
    SeqHeader *seq_header = &dec_handle_ptr->seq_header;
    FrameHeader *frame_info = &dec_handle_ptr->frame_header;
    int id_len=0, allFrames, FrameIsIntra = 0, i, frame_size_override_flag = 0;
    uint32_t diff_len;
    int delta_frame_id_length_minus_1, frame_refs_short_signaling;
    int gold_frame_idx, frame_to_show_map_idx;
    int have_prev_frame_id, diff_frame_id;
    int last_frame_idx;
    uint32_t prev_frame_id = 0;
    uint8_t expected_frame_id, display_frame_id;

    if (seq_header->frame_id_numbers_present_flag) {
        id_len = seq_header->frame_id_length - 1 +
            seq_header->delta_frame_id_length -2 + 3;
        assert(id_len <= 16);
    }
    allFrames = (1 << NUM_REF_FRAMES) - 1;
    if (seq_header->reduced_still_picture_header) {
        frame_info->show_existing_frame = 0;
        frame_info->frame_type = KEY_FRAME;
        FrameIsIntra = 1;
        frame_info->show_frame = 1;
        frame_info->showable_frame = 0;
        frame_info->error_resilient_mode = 1;
    }
    else {
        frame_info->show_existing_frame = dec_get_bits(bs, 1);
        PRINT_FRAME("show_existing_frame", frame_info->show_existing_frame);
        if (frame_info->show_existing_frame) {
            frame_to_show_map_idx = dec_get_bits(bs, 3);
            PRINT_FRAME("frame_to_show_map_idx", frame_to_show_map_idx);
            if (seq_header->decoder_model_info_present_flag &&
                !seq_header->timing_info.equal_picture_interval)
                temporal_point_info(bs, &seq_header->decoder_model_info, frame_info);
            frame_info->refresh_frame_flags = 0;
            if (seq_header->frame_id_numbers_present_flag) {
                display_frame_id = dec_get_bits(bs, id_len);
                PRINT_FRAME("display_frame_id", display_frame_id);
                if (display_frame_id != frame_info->ref_frame_idx[frame_to_show_map_idx]
                    && frame_info->ref_valid[frame_to_show_map_idx] == 1)
                    return; // EB_Corrupt_Frame;
            }

            //frame_type = RefFrameType[ frame_to_show_map_idx ]
            if (frame_info->frame_type == KEY_FRAME)
                frame_info->refresh_frame_flags = allFrames;
            if (seq_header->film_grain_params_present)
                // TODO: Handle while implementing Inter
                // load_grain_params(frame_to_show_map_idx);
                assert(0);

            dec_handle_ptr->cur_pic_buf[0] = dec_handle_ptr->
                ref_frame_map[frame_to_show_map_idx];
            generate_next_ref_frame_map(dec_handle_ptr);
            return;
        }

        frame_info->frame_type = dec_get_bits(bs, 2);

        FrameIsIntra = (frame_info->frame_type == INTRA_ONLY_FRAME ||
            frame_info->frame_type == KEY_FRAME);
        frame_info->show_frame = dec_get_bits(bs, 1);
        if (frame_info->show_frame && seq_header->decoder_model_info_present_flag
            && !seq_header->timing_info.equal_picture_interval)
            temporal_point_info(bs, &seq_header->decoder_model_info, frame_info);
        if (frame_info->show_frame)
            frame_info->showable_frame = (frame_info->frame_type != KEY_FRAME);
        else
            frame_info->showable_frame = dec_get_bits(bs, 1);
        if (frame_info->frame_type == S_FRAME ||
            (frame_info->frame_type == KEY_FRAME && frame_info->show_frame))
            frame_info->error_resilient_mode = 1;
        else
            frame_info->error_resilient_mode = dec_get_bits(bs, 1);
    }
    PRINT_FRAME("frame_type", frame_info->frame_type);
    PRINT_FRAME("show_frame", frame_info->show_frame);
    PRINT_FRAME("showable_frame", frame_info->showable_frame);
    PRINT_FRAME("error_resilient_mode", frame_info->error_resilient_mode);
    if (frame_info->frame_type == KEY_FRAME && frame_info->show_frame) {
        for (i = 0; i < NUM_REF_FRAMES; i++) {
            frame_info->ref_valid[i] = 0;
            // TODO: Need to differentate RefOrderHint and ref_order_hint
            frame_info->ref_order_hint[i] = 0;
        }
        for (i = 0; i < REFS_PER_FRAME; i++)
            frame_info->order_hints[LAST_FRAME + i] = 0;
    }
    frame_info->disable_cdf_update = dec_get_bits(bs, 1);
    PRINT_FRAME("disable_cdf_update", frame_info->disable_cdf_update);
    if (seq_header->seq_force_screen_content_tools == SELECT_SCREEN_CONTENT_TOOLS)
        frame_info->allow_screen_content_tools = dec_get_bits(bs, 1);
    else
        frame_info->allow_screen_content_tools
        = seq_header->seq_force_screen_content_tools;
    PRINT_FRAME("allow_screen_content_tools", frame_info->allow_screen_content_tools);

    if (frame_info->allow_screen_content_tools) {
        if (seq_header->seq_force_integer_mv == SELECT_INTEGER_MV)
            frame_info->force_integer_mv = dec_get_bits(bs, 1);
        else
            frame_info->force_integer_mv = seq_header->seq_force_integer_mv;
    }
    else
        frame_info->force_integer_mv = 0;
    PRINT_FRAME("force_integer_mv", frame_info->force_integer_mv);

    if (FrameIsIntra)
        frame_info->force_integer_mv = 1;

    have_prev_frame_id = /*!pbi->decoding_first_frame && */
        !(frame_info->frame_type == KEY_FRAME && frame_info->show_frame);
    if (have_prev_frame_id)
        prev_frame_id = frame_info->current_frame_id;

    if (seq_header->frame_id_numbers_present_flag) {
        // int PrevFrameID = frame_info->current_frame_id;
        frame_info->current_frame_id = dec_get_bits(bs, id_len);
        PRINT_FRAME("current_frame_id", frame_info->current_frame_id);

        if (have_prev_frame_id) {
            if (frame_info->current_frame_id > prev_frame_id)
                diff_frame_id = frame_info->current_frame_id - prev_frame_id;
            else {
                diff_frame_id =
                    (1 << id_len) + frame_info->current_frame_id - prev_frame_id;
            }
        // Bitstream conformance
        if (frame_info->current_frame_id == prev_frame_id || diff_frame_id >=
            1 << (id_len - 1))
            return; // EB_Corrupt_Frame;
        }

        //mark_ref_frames( id_len )
        diff_len = seq_header->delta_frame_id_length;
        for (i = 0; i < REF_FRAMES; i++) {
            if (frame_info->current_frame_id > (uint32_t)(1 << diff_len)) {
                if (frame_info->ref_frame_idx[i] > frame_info->current_frame_id ||
                    frame_info->ref_frame_idx[i] > (frame_info->current_frame_id -
                    (1 - diff_len)))
                    frame_info->ref_valid[i] = 0;
            }
            else {
                if (frame_info->ref_frame_idx[i] > frame_info->current_frame_id &&
                    frame_info->ref_frame_idx[i] < (uint32_t)((1 << id_len) +
                        frame_info->current_frame_id - (1 << diff_len)))
                    frame_info->ref_valid[i] = 0;
            }
        }
    }
    else
        frame_info->current_frame_id = 0;
    if (frame_info->frame_type == S_FRAME)
        frame_size_override_flag = 1;
    else if (seq_header->reduced_still_picture_header)
        frame_size_override_flag = 0;
    else
        frame_size_override_flag = dec_get_bits(bs, 1);
    PRINT_FRAME("frame_size_override_flag", frame_size_override_flag);
    frame_info->order_hint = dec_get_bits(bs,
        seq_header->order_hint_info.order_hint_bits);
    PRINT_FRAME("order_hint", frame_info->order_hint);

    uint16_t opPtIdc; int inTemporalLayer, inSpatialLayer;

    if (FrameIsIntra || frame_info->error_resilient_mode)
        frame_info->primary_ref_frame = PRIMARY_REF_NONE;
    else {
        frame_info->primary_ref_frame = dec_get_bits(bs, PRIMARY_REF_BITS);
        PRINT_FRAME("primary_ref_frame", frame_info->primary_ref_frame)
    }
    if (seq_header->decoder_model_info_present_flag) {
        frame_info->buffer_removal_time_present_flag = dec_get_bits(bs, 1);
        PRINT_FRAME("buffer_removal_time_present_flag",
            frame_info->buffer_removal_time_present_flag);
        if (frame_info->buffer_removal_time_present_flag) {
            for (int opNum = 0;
                opNum <= seq_header->operating_points_cnt_minus_1; opNum++) {
                if (seq_header->operating_point[opNum].
                    decoder_model_present_for_this_op)
                {
                    opPtIdc = seq_header->operating_point[opNum].op_idc;
                    inTemporalLayer = (opPtIdc >> obu_header->temporal_id) & 1;
                    inSpatialLayer = (opPtIdc >> (obu_header->spatial_id + 8)) & 1;
                    if (opPtIdc == 0 || (inTemporalLayer && inSpatialLayer))
                        frame_info->buffer_removal_time[opNum] = dec_get_bits(bs,
                            seq_header->decoder_model_info.
                            buffer_removal_time_length_minus_1 + 1);
                }
                else
                    frame_info->buffer_removal_time[opNum] = 0;
                PRINT_FRAME("buffer_removal_time[opNum]",
                    frame_info->buffer_removal_time[opNum]);
            }
        }
    }

    frame_info->allow_high_precision_mv = 0;
    frame_info->use_ref_frame_mvs = 0;
    frame_info->allow_intrabc = 0;
    if (frame_info->frame_type == S_FRAME || (frame_info->frame_type ==
        KEY_FRAME && frame_info->show_frame))
        frame_info->refresh_frame_flags = 0xFF;
    else
        frame_info->refresh_frame_flags = dec_get_bits(bs, 8);

    if (frame_info->frame_type == INTRA_ONLY_FRAME)
        assert(frame_info->refresh_frame_flags != 0xFF);

    PRINT_FRAME("refresh_frame_flags", frame_info->refresh_frame_flags);
    if (!FrameIsIntra || (frame_info->refresh_frame_flags != 0xFF)) {
        if (frame_info->error_resilient_mode &&
            seq_header->order_hint_info.enable_order_hint) {
            int ref_order_hint;
            for (i = 0; i < NUM_REF_FRAMES; i++) {
                ref_order_hint = dec_get_bits(bs,
                    seq_header->order_hint_info.order_hint_bits);
                PRINT_FRAME("ref_order_hint[i]", ref_order_hint);

                if (ref_order_hint != (int)frame_info->ref_order_hint[i])
                    frame_info->ref_valid[i] = 0;
            }
        }
    }
    if (FrameIsIntra) {
        read_frame_size(bs, seq_header, frame_info, frame_size_override_flag);
        read_render_size(bs, frame_info);
        if (frame_info->allow_screen_content_tools &&
            frame_info->frame_size.render_width) {
            if (frame_info->allow_screen_content_tools &&
                frame_info->frame_size.frame_width ==
                frame_info->frame_size.superres_upscaled_width) {
                frame_info->allow_intrabc = dec_get_bits(bs, 1);
                PRINT_FRAME("allow_intrabc", frame_info->allow_intrabc);
            }
        }
    }
    else {
        if (!seq_header->order_hint_info.enable_order_hint)
            frame_refs_short_signaling = 0;
        else {
            frame_refs_short_signaling = dec_get_bits(bs, 1);
            PRINT_FRAME("frame_refs_short_signaling", frame_refs_short_signaling);
            if (frame_refs_short_signaling) {
                last_frame_idx = dec_get_bits(bs, 3);
                gold_frame_idx = dec_get_bits(bs, 3);
                PRINT_FRAME("last_frame_idx", last_frame_idx);
                PRINT_FRAME("gold_frame_idx", gold_frame_idx);
                (void)last_frame_idx;
                (void)gold_frame_idx;
                assert(0); //svt_set_frame_refs();
            }
        }

        //int DeltaFrameId;
        for (i = 0; i < INTER_REFS_PER_FRAME; i++) {
            if (!frame_refs_short_signaling) {
                frame_info->ref_frame_idx[i] = dec_get_bits(bs, 3);
                PRINT_FRAME("ref_frame_idx", frame_info->ref_frame_idx[i]);
                dec_handle_ptr->remapped_ref_idx[i] = frame_info->ref_frame_idx[i];
            }

            frame_info->ref_frame_sign_bias[LAST_FRAME + i] = 0;

            if (seq_header->frame_id_numbers_present_flag) {
                delta_frame_id_length_minus_1 = dec_get_bits(bs,
                    seq_header->delta_frame_id_length);
                PRINT_FRAME("delta_frame_id_length_minus_1",
                    delta_frame_id_length_minus_1);
                expected_frame_id = ((frame_info->current_frame_id + (1 << id_len) -
                    (delta_frame_id_length_minus_1 + 1)) % (1 << id_len));
                if (expected_frame_id != frame_info->ref_frame_idx[i])
                    return; // EB_Corrupt_Frame;
            }
        }
        if (frame_size_override_flag && !frame_info->error_resilient_mode)
            frame_size_with_refs(bs, seq_header, frame_info,
                frame_size_override_flag);
        else {
            read_frame_size(bs, seq_header, frame_info, frame_size_override_flag);
            read_render_size(bs, frame_info);
        }
        if (frame_info->force_integer_mv)
            frame_info->allow_high_precision_mv = 0;
        else
            frame_info->allow_high_precision_mv = dec_get_bits(bs, 1);
        PRINT_FRAME("allow_high_precision_mv", frame_info->allow_high_precision_mv);
        read_interpolation_filter(bs, frame_info);
        frame_info->is_motion_mode_switchable = dec_get_bits(bs, 1);
        PRINT_FRAME("is_motion_mode_switchable", frame_info->is_motion_mode_switchable);
        if (frame_info->error_resilient_mode ||
            !seq_header->order_hint_info.enable_ref_frame_mvs)
            frame_info->use_ref_frame_mvs = 0;
        else
            frame_info->use_ref_frame_mvs = dec_get_bits(bs, 1);
        PRINT_FRAME("use_ref_frame_mvs", frame_info->use_ref_frame_mvs);
        //for (i = 0; i < INTER_REFS_PER_FRAME; i++) {
            //int refFrame = LAST_FRAME + i;
            // int hint = frame_info->ref_order_hint[frame_info->ref_frame_idx[i]];
            //frame_info->order_hint
            // RefOrderHint, RefFrameSignBias not available in structure
        //}
    }

    dec_handle_ptr->cur_pic_buf[0] = dec_pic_mgr_get_cur_pic(dec_handle_ptr->pv_pic_mgr,
        &dec_handle_ptr->seq_header, &dec_handle_ptr->frame_header,
        dec_handle_ptr->dec_config.max_color_format);
    svt_setup_frame_buf_refs(dec_handle_ptr);
    /*Temporal MVs allocation */
    check_add_tplmv_buf(dec_handle_ptr);

    setup_frame_sign_bias(dec_handle_ptr);

    if (seq_header->reduced_still_picture_header || frame_info->disable_cdf_update)
        frame_info->disable_frame_end_update_cdf = 1;
    else {
        frame_info->disable_frame_end_update_cdf = dec_get_bits(bs, 1);
        PRINT_FRAME("disable_frame_end_update_cdf",
            frame_info->disable_frame_end_update_cdf
            ? REFRESH_FRAME_CONTEXT_DISABLED : REFRESH_FRAME_CONTEXT_BACKWARD);
    }

    if (frame_info->primary_ref_frame == PRIMARY_REF_NONE)
        setup_past_independence(dec_handle_ptr, frame_info);

    generate_next_ref_frame_map(dec_handle_ptr);

    read_tile_info(bs, &frame_info->tiles_info, seq_header, frame_info);
    read_quantization_params(bs, &frame_info->quantization_params,
        &seq_header->color_config, num_planes);
    read_segmentation_params(bs, &frame_info->segmentation_params, frame_info);
    read_frame_delta_q_params(bs, frame_info);
    read_frame_delta_lf_params(bs, frame_info);
    setup_segmentation_dequant(dec_handle_ptr, &seq_header->color_config);

    ParseCtxt *parse_ctxt = (ParseCtxt *)dec_handle_ptr->pv_parse_ctxt;
    if (frame_info->primary_ref_frame == PRIMARY_REF_NONE)
        reset_parse_ctx(&parse_ctxt->init_frm_ctx,
            frame_info->quantization_params.base_q_idx);
    else
        /* Load CDF */
        parse_ctxt->init_frm_ctx = get_ref_frame_buf(dec_handle_ptr,
            frame_info->primary_ref_frame + 1)->final_frm_ctx;

    frame_info->coded_lossless = 1;
    for (int i = 0; i < MAX_SEGMENTS; ++i) {
        int qindex = get_qindex(&frame_info->segmentation_params, i,
            frame_info->quantization_params.base_q_idx);
        frame_info->lossless_array[i] = qindex == 0 &&
            frame_info->quantization_params.delta_q_y_dc == 0 &&
            frame_info->quantization_params.delta_q_u_ac == 0 &&
            frame_info->quantization_params.delta_q_u_dc == 0 &&
            frame_info->quantization_params.delta_q_v_ac == 0 &&
            frame_info->quantization_params.delta_q_v_dc == 0;
        if (!frame_info->lossless_array[i])
            frame_info->coded_lossless = 0;
        if (frame_info->quantization_params.using_qmatrix) {
            if (frame_info->lossless_array[i]) {
                frame_info->segmentation_params.seg_qm_level[0][i] = 15;
                frame_info->segmentation_params.seg_qm_level[1][i] = 15;
                frame_info->segmentation_params.seg_qm_level[2][i] = 15;
            }
            else {
                frame_info->segmentation_params.seg_qm_level[0][i]
                    = frame_info->quantization_params.qm_y;
                frame_info->segmentation_params.seg_qm_level[1][i]
                    = frame_info->quantization_params.qm_u;
                frame_info->segmentation_params.seg_qm_level[2][i]
                    = frame_info->quantization_params.qm_v;
            }
        }
    }

    if(frame_info->coded_lossless == 1)
        assert(frame_info->delta_q_params.delta_q_present == 0);

    frame_info->all_lossless = frame_info->coded_lossless &&
        (frame_info->frame_size.frame_width ==
            frame_info->frame_size.superres_upscaled_width);
    read_loop_filter_params(bs, frame_info, num_planes);
    read_frame_cdef_params(bs, frame_info, seq_header, num_planes);
    read_lr_params(bs, frame_info, seq_header, num_planes);
    read_tx_mode(bs, frame_info);

    frame_info->reference_mode = read_frame_reference_mode(bs, FrameIsIntra) ? REFERENCE_MODE_SELECT : SINGLE_REFERENCE;
    PRINT_FRAME("reference_mode", frame_info->reference_mode
        ? REFERENCE_MODE_SELECT : SINGLE_REFERENCE);
    read_skip_mode_params(bs, frame_info, FrameIsIntra, seq_header, frame_info->reference_mode);

    if (FrameIsIntra || frame_info->error_resilient_mode ||
        !seq_header->enable_warped_motion)
        frame_info->allow_warped_motion = 0;
    else
        frame_info->allow_warped_motion = dec_get_bits(bs, 1);
    frame_info->reduced_tx_set = dec_get_bits(bs, 1);
    PRINT_FRAME("allow_warped_motion", frame_info->allow_warped_motion);
    PRINT_FRAME("reduced_tx_set", frame_info->reduced_tx_set);
    read_global_motion_params(bs, dec_handle_ptr, frame_info, FrameIsIntra);
    read_film_grain_params(bs, &frame_info->film_grain_params, seq_header, frame_info);

    dec_handle_ptr->cur_pic_buf[0]->film_grain_params = dec_handle_ptr->frame_header.film_grain_params;

    dec_handle_ptr->show_existing_frame = frame_info->show_existing_frame;
    dec_handle_ptr->show_frame          = frame_info->show_frame;
    dec_handle_ptr->showable_frame      = frame_info->showable_frame;

    /* TODO: Should be moved to caller */
    if(!frame_info->show_existing_frame)
        svt_setup_motion_field(dec_handle_ptr);
}

EbErrorType read_frame_header_obu(bitstrm_t *bs, EbDecHandle *dec_handle_ptr,
                                  ObuHeader *obu_header, int trailing_bit)
{
    EbErrorType status = EB_ErrorNone;

    int num_planes = av1_num_planes(&dec_handle_ptr->seq_header.color_config);
    uint32_t start_position, end_position, header_bytes;

    start_position = get_position(bs);
    read_uncompressed_header(bs, dec_handle_ptr, obu_header, num_planes);
    if (trailing_bit) {
        status = av1_check_trailing_bits(bs);
        if (status != EB_ErrorNone) return status;
    }

    byte_alignment(bs);

    end_position = get_position(bs);
    header_bytes = (end_position - start_position) / 8;
    obu_header->payload_size -= header_bytes;

    return status;
}

void clear_above_context(EbDecHandle *dec_handle_ptr, int mi_col_start,
                         int mi_col_end, const int tile_row)
{
    assert(0 == tile_row);

    ParseCtxt   *parse_ctxt = (ParseCtxt *)dec_handle_ptr->pv_parse_ctxt;
    SeqHeader   *seq_params = &dec_handle_ptr->seq_header;

    int num_planes  = av1_num_planes(&seq_params->color_config);
    const int width = mi_col_end - mi_col_start;

    const int offset_y  = mi_col_start;
    const int width_y   = width;
    const int offset_uv = offset_y >> seq_params->color_config.subsampling_y;
    const int width_uv  = width_y >> seq_params->color_config.subsampling_x;

    int8_t num4_64x64 = mi_size_wide[BLOCK_64X64];

    ZERO_ARRAY((&parse_ctxt->parse_nbr4x4_ctxt.above_level_ctx[0][tile_row]) +
        offset_y, width_y);
    ZERO_ARRAY((&parse_ctxt->parse_nbr4x4_ctxt.above_dc_ctx[0][tile_row]) +
        offset_y, width_y);
    ZERO_ARRAY((&parse_ctxt->parse_nbr4x4_ctxt.above_palette_colors[0][tile_row]),
        num4_64x64 * PALETTE_MAX_SIZE);

    if (num_planes > 1) {
        ZERO_ARRAY((&parse_ctxt->parse_nbr4x4_ctxt.above_level_ctx[1][tile_row]) +
            offset_uv, width_uv);
        ZERO_ARRAY((&parse_ctxt->parse_nbr4x4_ctxt.above_dc_ctx[1][tile_row]) +
            offset_uv, width_uv);
        ZERO_ARRAY((&parse_ctxt->parse_nbr4x4_ctxt.above_palette_colors[1][tile_row]),
            num4_64x64 * PALETTE_MAX_SIZE);
        ZERO_ARRAY((&parse_ctxt->parse_nbr4x4_ctxt.above_level_ctx[2][tile_row]) +
            offset_uv, width_uv);
        ZERO_ARRAY((&parse_ctxt->parse_nbr4x4_ctxt.above_dc_ctx[2][tile_row]) +
            offset_uv, width_uv);
        ZERO_ARRAY((&parse_ctxt->parse_nbr4x4_ctxt.above_palette_colors[2][tile_row]),
            num4_64x64 * PALETTE_MAX_SIZE);
    }

    ZERO_ARRAY((&parse_ctxt->parse_nbr4x4_ctxt.above_seg_pred_ctx[tile_row]) +
        mi_col_start, width_y);

    ZERO_ARRAY((&parse_ctxt->parse_nbr4x4_ctxt.above_part_wd[tile_row]) +
        mi_col_start, width_y);

    memset((&parse_ctxt->parse_nbr4x4_ctxt.above_tx_wd[tile_row]) + mi_col_start,
        tx_size_wide[TX_SIZES_LARGEST], width_y * sizeof(uint8_t));
}

void clear_left_context(EbDecHandle *dec_handle_ptr)
{
    ParseCtxt   *parse_ctxt = (ParseCtxt *)dec_handle_ptr->pv_parse_ctxt;
    SeqHeader   *seq_params = &dec_handle_ptr->seq_header;

    /* Maintained only for 1 left SB! */
    int blk_cnt = seq_params->sb_mi_size;
    int num_planes = av1_num_planes(&seq_params->color_config);
    int32_t num_4x4_neigh_sb = seq_params->sb_mi_size;

    /* TODO :  after Optimizing the allocation for Chroma fix here also */
    for (int i = 0; i < num_planes; i++) {
        ZERO_ARRAY(parse_ctxt->parse_nbr4x4_ctxt.left_level_ctx[i], blk_cnt);
        ZERO_ARRAY(parse_ctxt->parse_nbr4x4_ctxt.left_dc_ctx[i], blk_cnt);
    }

    ZERO_ARRAY(parse_ctxt->parse_nbr4x4_ctxt.left_seg_pred_ctx, blk_cnt);

    ZERO_ARRAY(parse_ctxt->parse_nbr4x4_ctxt.left_part_ht, blk_cnt);

    for (int i = 0; i < MAX_MB_PLANE; i++) {
        ZERO_ARRAY((parse_ctxt->parse_nbr4x4_ctxt.left_palette_colors[i]),
            num_4x4_neigh_sb * PALETTE_MAX_SIZE);
    }

    memset(parse_ctxt->parse_nbr4x4_ctxt.left_tx_ht, tx_size_high[TX_SIZES_LARGEST],
        blk_cnt*sizeof(parse_ctxt->parse_nbr4x4_ctxt.left_tx_ht[0]));
}

void clear_cdef(int8_t *sb_cdef_strength, int32_t cdef_factor)
{
    memset(sb_cdef_strength, -1, cdef_factor * sizeof(*sb_cdef_strength));
}

void clear_loop_filter_delta(EbDecHandle *dec_handle)
{
    ParseCtxt *parse_ctx = (ParseCtxt*)dec_handle->pv_parse_ctxt;
    for (int lf_id = 0; lf_id < FRAME_LF_COUNT; ++lf_id)
        parse_ctx->parse_nbr4x4_ctxt.delta_lf[lf_id] = 0;
}

void clear_loop_restoration(int num_planes, PartitionInfo_t *part_info)
{
    for (int p = 0; p < num_planes; ++p) {
        set_default_wiener(part_info->wiener_info + p);
        set_default_sgrproj(part_info->sgrproj_info + p);
    }
}

EbErrorType parse_tile(bitstrm_t *bs, EbDecHandle *dec_handle_ptr,
                       TilesInfo *tile_info, int32_t tile_row, int32_t tile_col)
{
    (void)bs;
    EbErrorType status = EB_ErrorNone;

    EbColorConfig *color_config = &dec_handle_ptr->seq_header.color_config;
    int num_planes = av1_num_planes(color_config);

    clear_above_context(dec_handle_ptr, tile_info->tile_col_start_sb[tile_col],
                        tile_info->tile_col_start_sb[tile_col + 1], 0 /*TODO: For MultiThread*/);
    clear_loop_filter_delta(dec_handle_ptr);

    /* Init ParseCtxt */
    ParseCtxt *parse_ctx = (ParseCtxt*)dec_handle_ptr->pv_parse_ctxt;
    RestorationUnitInfo *lr_unit[MAX_MB_PLANE];

    // Default initialization of Wiener and SGR Filter
    for (int p = 0; p < num_planes; ++p) {
        lr_unit[p] = &parse_ctx->ref_lr_unit[p];

        set_default_wiener(&lr_unit[p]->wiener_info);
        set_default_sgrproj(&lr_unit[p]->sgrproj_info);
    }

    // to-do access to wiener info that is currently part of PartitionInfo_t
    //clear_loop_restoration(num_planes, part_info);

    for (uint32_t mi_row = tile_info->tile_row_start_sb[tile_row];
         mi_row < tile_info->tile_row_start_sb[tile_row + 1];
         mi_row += dec_handle_ptr->seq_header.sb_mi_size)
    {
        int32_t sb_row = (mi_row << 2) >> dec_handle_ptr->seq_header.sb_size_log2;

        clear_left_context(dec_handle_ptr);

        /*TODO: Move CFL to thread ctxt! We need to access DecModCtxt from parse_tile function */
        /*add tile level cfl init */
        cfl_init(&((DecModCtxt*)dec_handle_ptr->pv_dec_mod_ctxt)->cfl_ctx, color_config);

        for (uint32_t mi_col = tile_info->tile_col_start_sb[tile_col];
            mi_col < tile_info->tile_col_start_sb[tile_col + 1];
            mi_col += dec_handle_ptr->seq_header.sb_mi_size)

        {
            int32_t sb_col = (mi_col << MI_SIZE_LOG2) >>
                dec_handle_ptr->seq_header.sb_size_log2;
            uint8_t     sx = color_config->subsampling_x;
            uint8_t     sy = color_config->subsampling_y;

            //clear_block_decoded_flags(r, c, sbSize4)
            MasterFrameBuf *master_frame_buf = &dec_handle_ptr->master_frame_buf;
            CurFrameBuf    *frame_buf        = &master_frame_buf->cur_frame_bufs[0];
            int32_t num_mis_in_sb = master_frame_buf->num_mis_in_sb;

            SBInfo  *sb_info = frame_buf->sb_info +
                    (sb_row * master_frame_buf->sb_cols) + sb_col;
#if !FRAME_MI_MAP
            SBInfo  *left_sb_info = NULL;
            if(mi_col != tile_info->tile_col_start_sb[tile_col])
                left_sb_info  = frame_buf->sb_info +
                    (sb_row * master_frame_buf->sb_cols) + sb_col - 1;
            SBInfo  *above_sb_info = NULL;
            if (mi_row != tile_info->tile_row_start_sb[tile_row])
                above_sb_info = frame_buf->sb_info +
                    ((sb_row-1) * master_frame_buf->sb_cols) + sb_col;
#else
            *(master_frame_buf->frame_mi_map.pps_sb_info + sb_row *
                master_frame_buf->frame_mi_map.sb_cols + sb_col) = sb_info;
#endif
            sb_info->sb_mode_info = frame_buf->mode_info +
                (sb_row * num_mis_in_sb * master_frame_buf->sb_cols) +
                 sb_col * num_mis_in_sb;

            sb_info->sb_trans_info[AOM_PLANE_Y] = frame_buf->trans_info[AOM_PLANE_Y] +
                (sb_row * num_mis_in_sb * master_frame_buf->sb_cols) +
                 sb_col * num_mis_in_sb;

            sb_info->sb_trans_info[AOM_PLANE_U] = frame_buf->trans_info[AOM_PLANE_U] +
                (sb_row * num_mis_in_sb * master_frame_buf->sb_cols >> sy) +
                (sb_col * num_mis_in_sb >> sx);
#if SINGLE_THRD_COEFF_BUF_OPT
            /*TODO : Change to macro */
            sb_info->sb_coeff[AOM_PLANE_Y] = frame_buf->coeff[AOM_PLANE_Y];
            sb_info->sb_coeff[AOM_PLANE_U] = frame_buf->coeff[AOM_PLANE_U];
            sb_info->sb_coeff[AOM_PLANE_V] = frame_buf->coeff[AOM_PLANE_V];
#else
            /*TODO : Change to macro */
            sb_info->sb_coeff[AOM_PLANE_Y] = frame_buf->coeff[AOM_PLANE_Y] +
                (sb_row * num_mis_in_sb * master_frame_buf->sb_cols * (16 + 1))
                + sb_col * num_mis_in_sb* (16 + 1);
            /*TODO : Change to macro */
            sb_info->sb_coeff[AOM_PLANE_U] = frame_buf->coeff[AOM_PLANE_U] +
                (sb_row * num_mis_in_sb * master_frame_buf->sb_cols * (16 + 1)
                    >> (sy + sx))
                + (sb_col * num_mis_in_sb * (16 + 1) >> (sy + sx));
            sb_info->sb_coeff[AOM_PLANE_V] = frame_buf->coeff[AOM_PLANE_V] +
                (sb_row * num_mis_in_sb * master_frame_buf->sb_cols * (16 + 1)
                    >> (sy + sx))
                + (sb_col * num_mis_in_sb * (16 + 1) >> (sy + sx));
#endif

            int cdef_factor = dec_handle_ptr->seq_header.use_128x128_superblock ? 4 : 1;
            sb_info->sb_cdef_strength = frame_buf->cdef_strength +
                (((sb_row * master_frame_buf->sb_cols) + sb_col) * cdef_factor);

            sb_info->sb_delta_lf = frame_buf->delta_lf +
                (sb_row * master_frame_buf->sb_cols) + sb_col;

            sb_info->sb_delta_q = frame_buf->delta_q +
                (sb_row * master_frame_buf->sb_cols) + sb_col;

            clear_cdef(sb_info->sb_cdef_strength, cdef_factor);

            // Loop restoration SB level buffer alignment
            sb_info->sb_lr_unit[AOM_PLANE_Y] = frame_buf->lr_unit[AOM_PLANE_Y] +
                (sb_row * master_frame_buf->sb_cols) + sb_col;
            sb_info->sb_lr_unit[AOM_PLANE_U] = frame_buf->lr_unit[AOM_PLANE_U] +
                (sb_row * master_frame_buf->sb_cols >> sy) + (sb_col >> sx);
            sb_info->sb_lr_unit[AOM_PLANE_V] = frame_buf->lr_unit[AOM_PLANE_V] +
                (sb_row * master_frame_buf->sb_cols >> sy) + (sb_col >> sx);

            parse_ctx->first_luma_tu_offset = 0;
            parse_ctx->first_chroma_tu_offset = 0;
            parse_ctx->cur_mode_info = sb_info->sb_mode_info;
            parse_ctx->cur_mode_info_cnt = 0;
            parse_ctx->sb_row_mi = mi_row;
            parse_ctx->sb_col_mi = mi_col;
            parse_ctx->cur_coeff_buf[AOM_PLANE_Y] = sb_info->sb_coeff[AOM_PLANE_Y];
            parse_ctx->cur_coeff_buf[AOM_PLANE_U] = sb_info->sb_coeff[AOM_PLANE_U];
            parse_ctx->cur_coeff_buf[AOM_PLANE_V] = sb_info->sb_coeff[AOM_PLANE_V];
#if !FRAME_MI_MAP
            parse_ctx->left_sb_info = left_sb_info;
            parse_ctx->above_sb_info= above_sb_info;
#endif
            parse_ctx->prev_blk_has_chroma = 1; //default at start of frame / tile

            /* Init DecModCtxt */
            DecModCtxt *dec_mod_ctxt = (DecModCtxt*)dec_handle_ptr->pv_dec_mod_ctxt;
#if !FRAME_MI_MAP
            dec_mod_ctxt->sb_row_mi = mi_row;
            dec_mod_ctxt->sb_col_mi = mi_col;

            dec_mod_ctxt->left_sb_info = left_sb_info;
            dec_mod_ctxt->above_sb_info = above_sb_info;
#endif
            dec_mod_ctxt->cur_coeff[AOM_PLANE_Y] = sb_info->sb_coeff[AOM_PLANE_Y];
            dec_mod_ctxt->cur_coeff[AOM_PLANE_U] = sb_info->sb_coeff[AOM_PLANE_U];
            dec_mod_ctxt->cur_coeff[AOM_PLANE_V] = sb_info->sb_coeff[AOM_PLANE_V];

            dec_mod_ctxt->cur_tile_info = &parse_ctx->cur_tile_info;
#if !FRAME_MI_MAP
            /* nbr updates before SB call */
            update_nbrs_before_sb(&master_frame_buf->frame_mi_map, sb_col);
#endif
            // Bit-stream parsing of the superblock
            parse_super_block(dec_handle_ptr, mi_row, mi_col, sb_info);

            /* TO DO : Will move later */
            // decoding of the superblock
            decode_super_block(dec_mod_ctxt, mi_row, mi_col, sb_info);
#if !FRAME_MI_MAP
            /* nbr updates at SB level */
            update_nbrs_after_sb(&master_frame_buf->frame_mi_map, sb_col);
#endif
        }
    }

    return status;
}

static int read_is_valid(const uint8_t *start, size_t len, const uint8_t *end) {
    return len != 0 && len <= (size_t)(end - start);
}

EbErrorType init_svt_reader(SvtReader   *r,
                            const uint8_t *data, const uint8_t *data_end,
                            const size_t read_size,
                            uint8_t allow_update_cdf)
{
    EbErrorType status = EB_ErrorNone;

    if (read_is_valid(data, read_size, data_end)) {
        if (0 == svt_reader_init(r, data, read_size))
            r->allow_update_cdf = allow_update_cdf;
        else
            status = EB_Corrupt_Frame;
    }
    else
        status = EB_Corrupt_Frame;
    return status;
}

/* Inititalizes prms for current tile from Master TilesInfo ! */
void svt_tile_init(TileInfo *cur_tile_info, FrameHeader *frame_header,
                   int32_t tile_row, int32_t tile_col)
{
    TilesInfo *tiles_info = &frame_header->tiles_info;

    /* tile_set_row */
    assert(tile_row < tiles_info->tile_rows);
    cur_tile_info->tile_row     = tile_row;
    cur_tile_info->mi_row_start = tiles_info->tile_row_start_sb[tile_row];
    cur_tile_info->mi_row_end   = AOMMIN(tiles_info->
                        tile_row_start_sb[tile_row+1], frame_header->mi_rows);

    assert(cur_tile_info->mi_row_end > cur_tile_info->mi_row_start);

    /* tile_set_col */
    assert(tile_col < tiles_info->tile_cols);

    cur_tile_info->tile_col = tile_col;
    cur_tile_info->mi_col_start = tiles_info->tile_col_start_sb[tile_col];
    cur_tile_info->mi_col_end = AOMMIN(tiles_info->
                        tile_col_start_sb[tile_col + 1], frame_header->mi_cols);

    assert(cur_tile_info->mi_col_end > cur_tile_info->mi_col_start);
}

// Read Tile group information
EbErrorType read_tile_group_obu(bitstrm_t *bs, EbDecHandle *dec_handle_ptr,
                                TilesInfo *tiles_info, ObuHeader *obu_header)
{
    EbErrorType status = EB_ErrorNone;

    ParseCtxt   *parse_ctxt = (ParseCtxt *)dec_handle_ptr->pv_parse_ctxt;

    FrameHeader *frame_header = &dec_handle_ptr->frame_header;

    int num_tiles, tg_start, tg_end, tile_bits, tile_start_and_end_present_flag = 0;
    int tile_row, tile_col;
    size_t tile_size;
    uint32_t start_position, end_position, header_bytes;
    num_tiles = tiles_info->tile_cols * tiles_info->tile_rows;

    start_position = get_position(bs);
    if (num_tiles > 1) {
        tile_start_and_end_present_flag = dec_get_bits(bs, 1);
        PRINT_FRAME("tile_start_and_end_present_flag", tile_start_and_end_present_flag);
    }

    if (obu_header->obu_type == OBU_FRAME)
        assert(tile_start_and_end_present_flag == 0);
    if (num_tiles == 1 || !tile_start_and_end_present_flag) {
        tg_start = 0;
        tg_end = num_tiles - 1;
    }
    else {
        tile_bits = tiles_info->tile_cols_log2 + tiles_info->tile_rows_log2;
        tg_start = dec_get_bits(bs, tile_bits);
        tg_end = dec_get_bits(bs, tile_bits);
    }
    assert(tg_end >= tg_start);
    PRINT_FRAME("tg_start", tg_start);
    PRINT_FRAME("tg_end", tg_end);

    byte_alignment(bs);
    end_position = get_position(bs);
    header_bytes = (end_position - start_position) / 8;
    obu_header->payload_size -= header_bytes;

    for (int tile_num = tg_start; tile_num <= tg_end; tile_num++) {
        tile_row = tile_num / tiles_info->tile_cols;
        tile_col = tile_num % tiles_info->tile_cols;

        if (tile_num == tg_end)
            tile_size = obu_header->payload_size;
        else {
            tile_size = dec_get_bits_le(bs, tiles_info->tile_size_bytes) + 1;
            obu_header->payload_size -= (tiles_info->tile_size_bytes + tile_size);
        }
        PRINT_FRAME("tile_size", (tile_size));
        svt_tile_init(&parse_ctxt->cur_tile_info, frame_header,
                        tile_row, tile_col);

        parse_ctxt->parse_nbr4x4_ctxt.cur_q_ind =
            frame_header->quantization_params.base_q_idx;

        //init_symbol(tileSize)

        status = init_svt_reader(&parse_ctxt->r,
            (const uint8_t *)get_bitsteam_buf(bs), bs->buf_max, tile_size,
            !(frame_header->disable_cdf_update));
        if (status != EB_ErrorNone)
            return status;
#if 0
        reset_parse_ctx(&parse_ctxt->frm_ctx[0],
            frame_header->quantization_params.base_q_idx);
#else
        parse_ctxt->cur_tile_ctx = parse_ctxt->init_frm_ctx;
#endif
        /* TO DO decode_tile() */
        status = parse_tile(bs, dec_handle_ptr, tiles_info, tile_row, tile_col);

        /* Save CDF */
        if (!frame_header->disable_frame_end_update_cdf &&
            (tile_num == tiles_info->context_update_tile_id))
        {
            dec_handle_ptr->cur_pic_buf[0]->final_frm_ctx =
                                        parse_ctxt->cur_tile_ctx;
            eb_av1_reset_cdf_symbol_counters(&dec_handle_ptr->cur_pic_buf[0]->final_frm_ctx);
        }

        dec_bits_init(bs, (uint8_t *)parse_ctxt->r.ec.bptr, obu_header->payload_size);

        if (status != EB_ErrorNone)
            return status;
    }

    if (!dec_handle_ptr->frame_header.allow_intrabc) {
        if (dec_handle_ptr->frame_header.loop_filter_params.filter_level[0] ||
            dec_handle_ptr->frame_header.loop_filter_params.filter_level[1])
        {
            /*LF Trigger function for each frame*/
            dec_av1_loop_filter_frame(&dec_handle_ptr->frame_header,
                &dec_handle_ptr->seq_header,
                dec_handle_ptr->cur_pic_buf[0]->ps_pic_buf,
                dec_handle_ptr->pv_lf_ctxt,
                AOM_PLANE_Y, MAX_MB_PLANE
            );
        }

        const int32_t do_cdef =
            !frame_header->coded_lossless &&
            (frame_header->CDEF_params.cdef_bits ||
             frame_header->CDEF_params.cdef_y_strength[0] ||
             frame_header->CDEF_params.cdef_uv_strength[0]);

        const int opt_lr = !do_cdef &&
            !av1_superres_scaled(&dec_handle_ptr->frame_header.frame_size);

        LRParams *lr_param = dec_handle_ptr->frame_header.lr_params;
        int do_loop_restoration =
            lr_param[AOM_PLANE_Y].frame_restoration_type != RESTORE_NONE ||
            lr_param[AOM_PLANE_U].frame_restoration_type != RESTORE_NONE ||
            lr_param[AOM_PLANE_V].frame_restoration_type != RESTORE_NONE;

        if (!opt_lr) {
            if (do_loop_restoration)
                dec_av1_loop_restoration_save_boundary_lines(dec_handle_ptr, 0);

            /*Calling cdef frame level function*/
            if (do_cdef) {
                if (dec_handle_ptr->cur_pic_buf[0]->ps_pic_buf->bit_depth == EB_8BIT)
                    svt_cdef_frame(dec_handle_ptr);
                else
                    svt_cdef_frame_hbd(dec_handle_ptr);
            }

            if (do_loop_restoration) {
                dec_av1_loop_restoration_save_boundary_lines(dec_handle_ptr, 1);

                /* Padded bits are required for filtering pixel around frame boundary */
                pad_pic(dec_handle_ptr->cur_pic_buf[0]->ps_pic_buf);
                dec_av1_loop_restoration_filter_frame(dec_handle_ptr, opt_lr);
            }
        }
        else {
            if (do_loop_restoration) {
                /* Padded bits are required for filtering pixel around frame boundary */
                pad_pic(dec_handle_ptr->cur_pic_buf[0]->ps_pic_buf);
                dec_av1_loop_restoration_filter_frame(dec_handle_ptr, opt_lr);
            }
        }
    }

    /* Save CDF */
    if (frame_header->disable_frame_end_update_cdf)
        dec_handle_ptr->cur_pic_buf[0]->final_frm_ctx = parse_ctxt->init_frm_ctx;

    pad_pic(dec_handle_ptr->cur_pic_buf[0]->ps_pic_buf);

    return status;
}

EbErrorType decode_obu(EbDecHandle *dec_handle_ptr, unsigned char *data, unsigned int data_size)
{
    (void)dec_handle_ptr;
    (void)data;
    (void)data_size;
    return 0;
}

// Decode all OBUs in a Frame
EbErrorType decode_multiple_obu(EbDecHandle *dec_handle_ptr, uint8_t **data, size_t data_size)
{
    bitstrm_t bs;
    EbErrorType status = EB_ErrorNone;
    ObuHeader obu_header;
    int frame_decoding_finished = 0;


#if ENABLE_ENTROPY_TRACE
    enable_dump = 1;
#if FRAME_LEVEL_TRACE
    if (enable_dump) {
        char str[1000];
        sprintf(str, "SVT_fr_%d.txt", dec_handle_ptr->dec_cnt);
        if (temp_fp == NULL) temp_fp = fopen(str, "w");
    }
#else
    if (temp_fp == NULL) temp_fp = fopen("SVT.txt", "w");
#endif
#endif

    while (!frame_decoding_finished)
    {
        size_t payload_size = 0, length_size = 0;

        /* Decoder memory init if not done */
        if (0 == dec_handle_ptr->mem_init_done && 1 == dec_handle_ptr->seq_header_done)
            status = dec_mem_init(dec_handle_ptr);
        if (status != EB_ErrorNone) return status;

        dec_bits_init(&bs, *data, data_size);

        status = open_bistream_unit(&bs, &obu_header, data_size, &length_size);
        if (status != EB_ErrorNone) return status;

        payload_size = obu_header.payload_size;

        *data += (obu_header.size + length_size);
        data_size -= (obu_header.size + length_size);

        if (data_size < payload_size)
            return EB_Corrupt_Frame;

        dec_bits_init(&bs, *data, payload_size);

        switch (obu_header.obu_type) {
        case OBU_TEMPORAL_DELIMITER:
            PRINT_NAME("**************OBU_TEMPORAL_DELIMITER*******************");
            read_temporal_delimitor_obu(&dec_handle_ptr->seen_frame_header);
            break;

        case OBU_SEQUENCE_HEADER:
            PRINT_NAME("**************OBU_SEQUENCE_HEADER*******************")
                status = read_sequence_header_obu(&bs, &dec_handle_ptr->seq_header);
            if (status != EB_ErrorNone)
                return status;
            dec_handle_ptr->seq_header_done = 1;
            break;

        case OBU_FRAME_HEADER:
        case OBU_REDUNDANT_FRAME_HEADER:
        case OBU_FRAME:
            if (obu_header.obu_type == OBU_FRAME) {
                PRINT_NAME("**************OBU_FRAME*******************");
                dec_handle_ptr->show_existing_frame = 0;
            }
            else if (obu_header.obu_type == OBU_FRAME_HEADER) {
                PRINT_NAME("**************OBU_FRAME_HEADER*******************");
                assert(dec_handle_ptr->seen_frame_header == 0);
            }
            else {
                PRINT_NAME("**************OBU_REDUNDANT_FRAME_HEADER*******************");
                assert(dec_handle_ptr->seen_frame_header == 1);
            }

            if (!dec_handle_ptr->seen_frame_header)
            {
                dec_handle_ptr->seen_frame_header = 1;
                status = read_frame_header_obu(&bs, dec_handle_ptr, &obu_header,
                                               obu_header.obu_type != OBU_FRAME);
                frame_decoding_finished = 1;
            }
            /*else {
                 For OBU_REDUNDANT_FRAME_HEADER, previous frame_header is taken from dec_handle_ptr->frame_header
                //frame_header_copy(); TODO()
            }*/

            if (obu_header.obu_type != OBU_FRAME) break; // For OBU_TILE_GROUP comes under OBU_FRAME

        case OBU_TILE_GROUP:
            PRINT_NAME("**************OBU_TILE_GROUP*******************");
            if (!dec_handle_ptr->seen_frame_header)
                return EB_Corrupt_Frame;
            status = read_tile_group_obu(&bs, dec_handle_ptr,
                &dec_handle_ptr->frame_header.tiles_info, &obu_header);
            if (status != EB_ErrorNone) return status;
            if (frame_decoding_finished)
                dec_handle_ptr->seen_frame_header = 0;
            break;

        default:
            PRINT_NAME("**************UNKNOWN OBU*******************");
            break;
        }

        *data += payload_size;
        data_size -= payload_size;
        if (!data_size)
            frame_decoding_finished = 1;
    }

#if ENABLE_ENTROPY_TRACE
#if FRAME_LEVEL_TRACE
    if (enable_dump) {
        fclose(temp_fp);
        temp_fp = NULL;
    }
#endif
#endif

    return status;
}

EB_API EbErrorType eb_get_sequence_info(
    const uint8_t *obu_data,
    size_t         size,
    SeqHeader     *sequence_info)
{
    if (obu_data == NULL || size == 0 || sequence_info == NULL)
        return EB_ErrorBadParameter;
    const uint8_t* frame_buf = obu_data;
    size_t frame_sz = size;
    EbErrorType status = EB_ErrorNone;
    do {
        bitstrm_t bs;
        dec_bits_init(&bs, frame_buf, frame_sz);

        ObuHeader ou;
        memset(&ou, 0, sizeof(ou));
        size_t length_size = 0;
        status = open_bistream_unit(&bs, &ou, frame_sz, &length_size);
        if (status != EB_ErrorNone)
            return status;

        frame_buf += ou.size + length_size;
        frame_sz -= (uint32_t)(ou.size + length_size);

        if (ou.obu_type == OBU_SEQUENCE_HEADER) {
            // check the ou type and parse sequence header
            status = read_sequence_header_obu(&bs, sequence_info);
            if (status == EB_ErrorNone)
                return status;
        }

        frame_buf += ou.payload_size;
        frame_sz -= ou.payload_size;
    } while (status == EB_ErrorNone && frame_sz > 0);
    return EB_ErrorUndefined;
}
