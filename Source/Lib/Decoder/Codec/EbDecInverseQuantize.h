/*
* Copyright(c) 2019 Netflix, Inc.
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/
#ifndef EbDecInverseQuantize_h
#define EbDecInverseQuantize_h

int av1_num_planes(EbColorConfig *color_info);
int16_t av1_dc_quant_Q3(int32_t qindex, int32_t delta, aom_bit_depth_t bit_depth);
int16_t av1_ac_quant_Q3(int32_t qindex, int32_t delta, aom_bit_depth_t bit_depth);
int16_t get_dc_quant(int32_t qindex, int32_t delta, aom_bit_depth_t bit_depth);
int16_t get_ac_quant(int32_t qindex, int32_t delta, aom_bit_depth_t bit_depth);
void setup_segmentation_dequant(FrameHeader *frame_info, SeqHeader *seq_header,
    EbColorConfig *color_config);
void av1_inverse_qm_init(FrameHeader *iquant_matrix, EbColorConfig *color_config);
void av1_init_sb(FrameHeader *frame);
void update_dequant(EbDecHandle *dec_handle, PartitionInfo_t *part);
int get_dqv(const int16_t *dequant, int coeff_idx, const qm_val_t *iqmatrix);
int32_t inverse_quantize(EbDecHandle * dec_handle, PartitionInfo_t *part, ModeInfo_t *mode,
    int32_t *level, int32_t *qcoeffs, TxType tx_type, TxSize tx_size, int plane);

#endif // EbDecInverseQuantize_h
