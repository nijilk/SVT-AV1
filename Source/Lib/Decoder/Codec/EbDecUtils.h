/*
* Copyright(c) 2019 Netflix, Inc.
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

#ifndef EbDecUtils_h
#define EbDecUtils_h

#ifdef __cplusplus
extern "C" {
#endif

static INLINE int get_relative_dist(OrderHintInfo *ps_order_hint_info,
                                    int ref_hint, int order_hint)
{
    int diff, m;
    if (!ps_order_hint_info->enable_order_hint)
        return 0;
    diff = ref_hint - order_hint;
    m = 1 << (ps_order_hint_info->order_hint_bits - 1);
    diff = (diff & (m - 1)) - (diff & m);
    return diff;
}

#ifdef __cplusplus
}
#endif
#endif // EbDecUtils_h