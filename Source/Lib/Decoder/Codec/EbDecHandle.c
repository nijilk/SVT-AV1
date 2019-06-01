/*
* Copyright(c) 2019 Netflix, Inc.
* SPDX - License - Identifier: BSD - 2 - Clause - Patent
*/

// SUMMARY
//   Contains the API component functions

/**************************************
 * Includes
 **************************************/
#include <stdlib.h>

#include "EbDefinitions.h"
#include "EbPictureBufferDesc.h"

#include "EbSvtAv1Dec.h"
#include "EbDecHandle.h"
#include "EbDecMemInit.h"

#if defined(__linux__) || defined(__APPLE__)
#include <pthread.h>
#include <errno.h>
#include <sys/stat.h>
#include <fcntl.h>
#endif

#define RTCD_C
#include "aom_dsp_rtcd.h"

/**************************************
* Globals
**************************************/

EbMemoryMapEntry                 *svt_dec_memory_map;
uint32_t                         *svt_dec_memory_map_index;
uint64_t                         *svt_dec_total_lib_memory;

uint32_t                         svt_dec_lib_malloc_count = 0;

//TODO: Should be removed! Check
EbMemoryMapEntry                 *memory_map;
uint32_t                         *memory_map_index;
uint64_t                         *total_lib_memory;

uint32_t                         lib_malloc_count = 0;
uint32_t                         lib_semaphore_count = 0;
uint32_t                         lib_mutex_count = 0;

void init_intra_dc_predictors_c_internal(void);
void init_intra_predictors_internal(void);
EbErrorType decode_multiple_obu(EbDecHandle *dec_handle_ptr,
            const uint8_t *data, uint32_t data_size);

void SwitchToRealTime(){
#if defined(__linux__) || defined(__APPLE__)

    struct sched_param schedParam = {
        .sched_priority = 99
    };

    int32_t retValue = pthread_setschedparam(pthread_self(), SCHED_FIFO, &schedParam);
    UNUSED(retValue);
#endif
}

/***********************************
* Decoder Library Handle Constructor
************************************/
/*TODO : Add more features*/
static EbErrorType eb_dec_handle_ctor(
    EbDecHandle     **decHandleDblPtr,
    EbComponentType * ebHandlePtr)
{
    (void)ebHandlePtr;
    EbErrorType return_error = EB_ErrorNone;

    // Allocate Memory
    EbDecHandle   *dec_handle_ptr = (EbDecHandle  *)malloc(sizeof(EbDecHandle  ));
    *decHandleDblPtr = dec_handle_ptr;
    if (dec_handle_ptr == (EbDecHandle  *)EB_NULL)
        return EB_ErrorInsufficientResources;
#if MEM_MAP_OPT
    dec_handle_ptr->memory_map = (EbMemoryMapEntry*)malloc(sizeof(EbMemoryMapEntry));
    dec_handle_ptr->memory_map_index = 0;
    dec_handle_ptr->total_lib_memory = sizeof(EbComponentType) + sizeof(EbDecHandle) + sizeof(EbMemoryMapEntry);
    dec_handle_ptr->memory_map_init_address = dec_handle_ptr->memory_map;
#else
    dec_handle_ptr->memory_map = (EbMemoryMapEntry*)malloc(sizeof(EbMemoryMapEntry) * MAX_NUM_PTR);
    dec_handle_ptr->memory_map_index = 0;
    dec_handle_ptr->total_lib_memory = sizeof(EbDecHandle) + sizeof(EbMemoryMapEntry) * MAX_NUM_PTR;
#endif
    // Save Memory Map Pointers
    svt_dec_total_lib_memory = &dec_handle_ptr->total_lib_memory;
    svt_dec_memory_map = dec_handle_ptr->memory_map;
    svt_dec_memory_map_index = &dec_handle_ptr->memory_map_index;
    svt_dec_lib_malloc_count = 0;

    return return_error;
}

/* Copy from recon buffer to out buffer! */
int svt_dec_out_buf(
    EbDecHandle         *dec_handle_ptr,
    EbBufferHeaderType  *p_buffer)
{
    EbPictureBufferDesc *recon_picture_buf = dec_handle_ptr->cur_pic_buf[0]->ps_pic_buf;
    EbSvtIOFormat       *out_img = (EbSvtIOFormat*)p_buffer->p_buffer;

    int wd = dec_handle_ptr->frame_header.frame_size.frame_width;
    int ht = dec_handle_ptr->frame_header.frame_size.frame_height;
    int i, sx, sy;

    switch (recon_picture_buf->color_format) {
        case EB_YUV420 :
            sx = 1;
            sy = 1;
            break;
        default :
            assert(0);
    }

    if (recon_picture_buf->bit_depth == EB_8BIT) {
    uint8_t *dst;
    uint8_t *src;

    /* Luma */
    dst = out_img->luma + out_img->origin_x +
            (out_img->origin_y * out_img->y_stride);
    src = recon_picture_buf->buffer_y + recon_picture_buf->origin_x +
        (recon_picture_buf->origin_y * recon_picture_buf->stride_y);

    for (i = 0; i < ht; i++) {
        memcpy(dst, src, wd);
        dst += out_img->y_stride;
        src += recon_picture_buf->stride_y;
    }

    /* Cb */
        dst = out_img->cb + (out_img->origin_x >> sx) +
            ((out_img->origin_y >> sy) * out_img->cb_stride);
        src = recon_picture_buf->buffer_cb + (recon_picture_buf->origin_x >> sx) +
            ((recon_picture_buf->origin_y >> sy) * recon_picture_buf->stride_cb);

        for (i = 0; i < ht >> sy; i++) {
            memcpy(dst, src, wd >> sx);
        dst += out_img->cb_stride;
        src += recon_picture_buf->stride_cb;
    }

    /* Cr */
        dst = out_img->cr + (out_img->origin_x >> sx) +
            ((out_img->origin_y >> sy) * out_img->cr_stride);
        src = recon_picture_buf->buffer_cr + (recon_picture_buf->origin_x >> sx) +
            ((recon_picture_buf->origin_y >> sy)* recon_picture_buf->stride_cr);

        for (i = 0; i < ht >> sy; i++) {
            memcpy(dst, src, wd >> sx);
        dst += out_img->cr_stride;
        src += recon_picture_buf->stride_cr;
    }
    } else {
        uint16_t *pu2_dst;
        uint16_t *pu2_src;

        /* Luma */
        pu2_dst = (uint16_t *)out_img->luma + out_img->origin_x +
                (out_img->origin_y * out_img->y_stride);
        pu2_src = (uint16_t *)recon_picture_buf->buffer_y + recon_picture_buf->origin_x +
            (recon_picture_buf->origin_y * recon_picture_buf->stride_y);

        for (i = 0; i < ht; i++) {
            memcpy(pu2_dst, pu2_src, sizeof(uint16_t) * wd);
            pu2_dst += out_img->y_stride;
            pu2_src += recon_picture_buf->stride_y;
        }

        /* Cb */
        pu2_dst = (uint16_t *)out_img->cb + (out_img->origin_x >> sx) +
            ((out_img->origin_y >> sy) * out_img->cb_stride);
        pu2_src = (uint16_t *)recon_picture_buf->buffer_cb + (recon_picture_buf->origin_x >> sx) +
            ((recon_picture_buf->origin_y >> sy) * recon_picture_buf->stride_cb);

        for (i = 0; i < ht >> sy; i++) {
            memcpy(pu2_dst, pu2_src, sizeof(uint16_t) * wd >> sx);
            pu2_dst += out_img->cb_stride;
            pu2_src += recon_picture_buf->stride_cb;
        }

        /* Cr */
        pu2_dst = (uint16_t *)out_img->cr + (out_img->origin_x >> sx) +
            ((out_img->origin_y >> sy) * out_img->cr_stride);
        pu2_src = (uint16_t *)recon_picture_buf->buffer_cr + (recon_picture_buf->origin_x >> sx) +
            ((recon_picture_buf->origin_y >> sy)* recon_picture_buf->stride_cr);

        for (i = 0; i < ht >> sy; i++) {
            memcpy(pu2_dst, pu2_src, sizeof(uint16_t) * wd >> sx);
            pu2_dst += out_img->cr_stride;
            pu2_src += recon_picture_buf->stride_cr;
        }
    }
    return 1;
}

/**********************************
Set Default Library Params
**********************************/
EbErrorType eb_svt_dec_set_default_parameter(
    EbSvtAv1DecConfiguration    *config_ptr)
{
    EbErrorType                  return_error = EB_ErrorNone;

    if (config_ptr == NULL)
        return EB_ErrorBadParameter;

    config_ptr->operating_point = -1;
    config_ptr->output_all_layers = 0;
    config_ptr->skip_film_grain = 0;
    config_ptr->skip_frames = 0;
    config_ptr->frames_to_be_decoded = 0;
    config_ptr->compressed_ten_bit_format = 0;
    config_ptr->eight_bit_output = 0;

    /* Picture parameters */
    config_ptr->max_picture_width = 0;
    config_ptr->max_picture_height = 0;
    config_ptr->max_bit_depth = EB_EIGHT_BIT;
    config_ptr->max_color_format = EB_YUV420;
    config_ptr->asm_type = 0;
    config_ptr->threads = 1;

    // Application Specific parameters
    config_ptr->channel_id = 0;
    config_ptr->active_channel_count = 1;
    config_ptr->stat_report = 0;

    return return_error;
}

/**********************************
* Decoder Handle Initialization
**********************************/
static EbErrorType init_svt_av1_decoder_handle(
    EbComponentType     *hComponent)
{
    EbErrorType       return_error = EB_ErrorNone;
    EbComponentType  *svt_dec_component = (EbComponentType*)hComponent;

    printf("SVT [version]:\tSVT-AV1 Decoder Lib v%d.%d.%d\n",
        SVT_VERSION_MAJOR, SVT_VERSION_MINOR, SVT_VERSION_PATCHLEVEL);
#if ( defined( _MSC_VER ) && (_MSC_VER < 1910) )
    printf("SVT [build]  : Visual Studio 2013");
#elif ( defined( _MSC_VER ) && (_MSC_VER >= 1910) )
    printf("SVT [build]  :\tVisual Studio 2017");
#elif defined(__GNUC__)
    printf("SVT [build]  :\tGCC %d.%d.%d\t", __GNUC__, __GNUC_MINOR__, __GNUC_PATCHLEVEL__);
#else
    printf("SVT [build]  :\tunknown compiler");
#endif
    printf(" %u bit\n", (unsigned) sizeof(void*) * 8);
    printf("LIB Build date: %s %s\n", __DATE__, __TIME__);
    printf("-------------------------------------------\n");

    SwitchToRealTime();

    // Set Component Size & Version
    svt_dec_component->size = sizeof(EbComponentType);

    // Decoder Private Handle Ctor
    return_error = (EbErrorType)eb_dec_handle_ctor(
        (EbDecHandle  **) &(svt_dec_component->p_component_private),
        svt_dec_component);

    return return_error;
}

#if defined(__linux__) || defined(__APPLE__)
__attribute__((visibility("default")))
#endif
EB_API EbErrorType eb_dec_init_handle(
    EbComponentType             **p_handle,
    void                         *p_app_data,
    EbSvtAv1DecConfiguration     *config_ptr)
{
    EbErrorType           return_error = EB_ErrorNone;

    if (p_handle == NULL)
        return EB_ErrorBadParameter;

    *p_handle = (EbComponentType*) malloc(sizeof(EbComponentType));

    if (*p_handle != (EbComponentType*)NULL) {
        // Init Component OS objects (threads, semaphores, etc.)
        // also links the various Component control functions
        return_error = init_svt_av1_decoder_handle(*p_handle);

        if (return_error == EB_ErrorNone)
            ((EbComponentType*)(*p_handle))->p_application_private = p_app_data;
        else if (return_error == EB_ErrorInsufficientResources) {
            eb_deinit_decoder((EbComponentType*)NULL);
            *p_handle = (EbComponentType*)NULL;
        }
        else
            return_error = EB_ErrorInvalidComponent;
    }
    else {
        //SVT_LOG("Error: Component Struct Malloc Failed\n");
        return_error = EB_ErrorInsufficientResources;
    }

    if(return_error == EB_ErrorNone)
        return_error = eb_svt_dec_set_default_parameter(config_ptr);

    return return_error;
}

#if defined(__linux__) || defined(__APPLE__)
__attribute__((visibility("default")))
#endif
EB_API EbErrorType eb_svt_dec_set_parameter(
    EbComponentType              *svt_dec_component,
    EbSvtAv1DecConfiguration     *pComponentParameterStructure)
{
    if (svt_dec_component == NULL || pComponentParameterStructure == NULL)
        return EB_ErrorBadParameter;

    EbDecHandle     *dec_handle_ptr = (EbDecHandle   *)svt_dec_component->p_component_private;

    dec_handle_ptr->dec_config = *pComponentParameterStructure;

    return EB_ErrorNone;
}

#if defined(__linux__) || defined(__APPLE__)
__attribute__((visibility("default")))
#endif
EB_API EbErrorType eb_init_decoder(
    EbComponentType         *svt_dec_component)
{
    EbErrorType return_error = EB_ErrorNone;
    if (svt_dec_component == NULL)
        return EB_ErrorBadParameter;

    EbDecHandle     *dec_handle_ptr = (EbDecHandle   *)svt_dec_component->p_component_private;

    dec_handle_ptr->dec_cnt = -1;
    dec_handle_ptr->num_frms_prll   = 1;
    if(dec_handle_ptr->num_frms_prll > DEC_MAX_NUM_FRM_PRLL)
        dec_handle_ptr->num_frms_prll = DEC_MAX_NUM_FRM_PRLL;
    dec_handle_ptr->seq_header_done = 0;
    dec_handle_ptr->mem_init_done   = 0;

    dec_handle_ptr->seen_frame_header = 0;
    dec_handle_ptr->show_existing_frame = 0;

    assert(0 == dec_handle_ptr->dec_config.asm_type);
    setup_rtcd_internal(dec_handle_ptr->dec_config.asm_type);

    init_intra_dc_predictors_c_internal();

    init_intra_predictors_internal();

    /************************************
    * Decoder Memory Init
    ************************************/
    return_error = dec_mem_init(dec_handle_ptr);
    if (return_error != EB_ErrorNone)
        return return_error;

    return return_error;
}

#if defined(__linux__) || defined(__APPLE__)
__attribute__((visibility("default")))
#endif
EB_API EbErrorType eb_svt_decode_frame(
    EbComponentType     *svt_dec_component,
    const uint8_t       *data,
    const uint32_t       data_size)
{
    EbErrorType return_error = EB_ErrorNone;
    if (svt_dec_component == NULL)
        return EB_ErrorBadParameter;

    EbDecHandle *dec_handle_ptr = (EbDecHandle *)svt_dec_component->p_component_private;
    /*TODO : Remove or move. For Test purpose only */
    dec_handle_ptr->dec_cnt++;
    printf("\n SVT-AV1 Dec : Decoding Pic #%d", dec_handle_ptr->dec_cnt);

    return_error = decode_multiple_obu(dec_handle_ptr, data, data_size);

    dec_pic_mgr_update_ref_pic(dec_handle_ptr, (EB_ErrorNone == return_error) ? 1 : 0,
                                dec_handle_ptr->frame_header.refresh_frame_flags);

    return return_error;
}

#if defined(__linux__) || defined(__APPLE__)
__attribute__((visibility("default")))
#endif
EB_API EbErrorType eb_svt_dec_get_picture(
    EbComponentType      *svt_dec_component,
    EbBufferHeaderType   *p_buffer,
    EbAV1StreamInfo      *stream_info,
    EbAV1FrameInfo       *frame_info)
{
    (void)stream_info;
    (void)frame_info;

    EbErrorType return_error = EB_ErrorNone;
    if (svt_dec_component == NULL)
        return EB_ErrorBadParameter;

    EbDecHandle     *dec_handle_ptr = (EbDecHandle   *)svt_dec_component->p_component_private;
    /* Copy from recon pointer and return! TODO: Should remove the memcpy! */
    if (0 == svt_dec_out_buf(dec_handle_ptr, p_buffer))
        return_error = EB_DecNoOutputPicture;
    return return_error;
}

#if defined(__linux__) || defined(__APPLE__)
__attribute__((visibility("default")))
#endif
EB_API EbErrorType eb_deinit_decoder(
    EbComponentType     *svt_dec_component)
{
    if (svt_dec_component == NULL)
        return EB_ErrorBadParameter;
#if MEM_MAP_OPT
    EbDecHandle *dec_handle_ptr = (EbDecHandle*)svt_dec_component->p_component_private;
    EbErrorType return_error    = EB_ErrorNone;

    if (dec_handle_ptr) {
        if (svt_dec_memory_map) {
            // Loop through the ptr table and free all malloc'd pointers per channel
            EbMemoryMapEntry*    memory_entry = svt_dec_memory_map;
            if (memory_entry){
                do {
                    switch (memory_entry->ptr_type) {
                        case EB_N_PTR:
                            free(memory_entry->ptr);
                            break;
                        case EB_A_PTR:
#ifdef _WIN32
                            _aligned_free(memory_entry->ptr);
#else
                            free(memory_entry->ptr);
#endif
                            break;
                        case EB_SEMAPHORE:
                            eb_destroy_semaphore(memory_entry->ptr);
                            break;
                        case EB_THREAD:
                            eb_destroy_thread(memory_entry->ptr);
                            break;
                        case EB_MUTEX:
                            eb_destroy_mutex(memory_entry->ptr);
                            break;
                        default:
                            return_error = EB_ErrorMax;
                            break;
                    }
                    EbMemoryMapEntry*    tmp_memory_entry = memory_entry;
                    memory_entry = (EbMemoryMapEntry*)tmp_memory_entry->prev_entry;
                    if (tmp_memory_entry) free(tmp_memory_entry);
                } while(memory_entry != dec_handle_ptr->memory_map_init_address && memory_entry);
                if (dec_handle_ptr->memory_map_init_address) free(dec_handle_ptr->memory_map_init_address);
            }
        }
    }
#else
    EbDecHandle *dec_handle_ptr = (EbDecHandle*)svt_dec_component->p_component_private;
    EbErrorType return_error = EB_ErrorNone;
    int32_t              ptrIndex = 0;
    EbMemoryMapEntry*   memoryEntry = (EbMemoryMapEntry*)EB_NULL;

    if (dec_handle_ptr) {
        if (dec_handle_ptr->memory_map_index) {
            // Loop through the ptr table and free all malloc'd pointers per channel
            for (ptrIndex = (dec_handle_ptr->memory_map_index) - 1; ptrIndex >= 0; --ptrIndex) {
                memoryEntry = &dec_handle_ptr->memory_map[ptrIndex];
                switch (memoryEntry->ptr_type) {
                case EB_N_PTR:
                    free(memoryEntry->ptr);
                    break;
                case EB_A_PTR:
#ifdef _WIN32
                    _aligned_free(memoryEntry->ptr);
#else
                    free(memoryEntry->ptr);
#endif
                    break;
                case EB_SEMAPHORE:
                    eb_destroy_semaphore(memoryEntry->ptr);
                    break;
                case EB_THREAD:
                    eb_destroy_thread(memoryEntry->ptr);
                    break;
                case EB_MUTEX:
                    eb_destroy_mutex(memoryEntry->ptr);
                    break;
                default:
                    return_error = EB_ErrorMax;
                    break;
                }
            }
            if (dec_handle_ptr->memory_map != (EbMemoryMapEntry*)NULL)
                free(dec_handle_ptr->memory_map);
        }
    }
#endif
    return return_error;
}

/**********************************
* Encoder Componenet DeInit
**********************************/
EbErrorType eb_dec_component_de_init(EbComponentType  *svt_dec_component)
{
    EbErrorType       return_error = EB_ErrorNone;

    if (svt_dec_component->p_component_private)
        free((EbDecHandle *)svt_dec_component->p_component_private);
    else
        return_error = EB_ErrorUndefined;
    return return_error;
}

#if defined(__linux__) || defined(__APPLE__)
__attribute__((visibility("default")))
#endif
EB_API EbErrorType eb_dec_deinit_handle(
    EbComponentType     *svt_dec_component)
{
    EbErrorType return_error = EB_ErrorNone;

    if (svt_dec_component) {
        return_error = eb_dec_component_de_init(svt_dec_component);

        free(svt_dec_component);
    }
    else
        return_error = EB_ErrorInvalidComponent;
    return return_error;
}

#if defined(__linux__) || defined(__APPLE__)
__attribute__((visibility("default")))
#endif
EB_API EbErrorType eb_dec_set_frame_buffer_callbacks(
  EbComponentType             *svt_dec_component,
  eb_allocate_frame_buffer    allocate_buffer,
  eb_release_frame_buffer     release_buffer,
  void                        *priv_data)
{
    (void)svt_dec_component;
    (void)allocate_buffer;
    (void)release_buffer;
    (void)priv_data;

    return EB_ErrorNone;
}
