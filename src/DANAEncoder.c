#include "DANAEncoder.h"
#include "DANAUtility.h"
#include "DANAPredictor.h"
#include "DANACoder.h"
#include "DANABitStream.h"
#include "DANAByteArray.h"
#include "DANAInternal.h"

#include <stdlib.h>
#include <math.h>
#include <string.h>
#include <float.h>

#define DANAENCODER_STATUS_FLAG_SET_WAVE_FORMAT      (1 << 0)
#define DANAENCODER_STATUS_FLAG_SET_ENCODE_PARAMETER (1 << 1)

static const double DANA_SCALE_2_MINUS_31 = 1.0 / 2147483648.0; /* 2^-31 */
static const double DANA_SCALE_2_PLUS_15  = 32768.0;            /* 2^15  */

static inline void put_varint(uint8_t** p, uint32_t val) {
    while (val >= 0x80) {
        **p = (val & 0x7F) | 0x80;
        *p += 1;
        val >>= 7;
    }
    **p = val & 0x7F;
    *p += 1;
}

struct DANAEncoder {
    struct DANAWaveFormat          wave_format;
    struct DANAEncodeParameter     encode_param;
    struct DANAMetadata            metadata;
    uint32_t                       max_num_channels;
    uint32_t                       max_num_block_samples;
    uint32_t                       max_parcor_order;
    uint32_t                       max_longterm_order;
    uint32_t                       max_lms_order_per_filter;
    struct DANABitStream           strm;
    struct DANACoder*              coder;
    struct DANALPCCalculator**       lpcc;   
    struct DANALongTermCalculator**  ltc;
    struct DANALPCSynthesizer**      lpcs;
    struct DANALongTermSynthesizer** ltms;
    struct DANALMSFilter***        nlmsc;
    struct DANAEmphasisFilter**      emp;
    struct DANAOptimalBlockPartitionEstimator* oee;
    DANAChannelProcessMethod       ch_proc_method;
    DANAChannelProcessMethod       current_ch_mode;
    DANAWindowFunctionType         window_type;
    double**                       input_double;
    int32_t**                      input_int32;
    int32_t**                      res_ltp;
    int32_t**                      scratch1;
    int32_t**                      scratch2;
    uint8_t*                       filter_flags;
    double**                       parcor_coef;
    int32_t**                      parcor_coef_int32;
    int32_t**                      parcor_coef_code;
    uint32_t*                      parcor_rshift;
    double**                       longterm_coef;
    int32_t**                      longterm_coef_int32;
    uint32_t*                      pitch_period;
    double*                        window;
    DANABlockDataType              block_data_type;
    int32_t**                      residual;
    uint32_t*                      num_block_partition_samples;
    uint32_t                       status_flag;
    uint8_t                        verpose_flag;
    int32_t                        current_alpha_q;
    uint8_t                        lms_num_stages[DANA_MAX_CHANNELS];
    uint8_t                        lms_step_idx[DANA_MAX_CHANNELS][DANA_MAX_LMS_STAGES];
    uint8_t                        enable_seek_table;
};

struct DANAEncoder* DANAEncoder_Create(const struct DANAEncoderConfig* config) {
    if (config == NULL) return NULL;

    struct DANAEncoder* encoder = calloc(1, sizeof(struct DANAEncoder));
    if (!encoder) return NULL;

    encoder->max_num_channels         = config->max_num_channels;
    encoder->enable_seek_table        = config->enable_seek_table;
    encoder->max_num_block_samples    = config->max_num_block_samples;
    encoder->max_parcor_order         = config->max_parcor_order;
    encoder->max_longterm_order       = config->max_longterm_order;
    encoder->max_lms_order_per_filter = config->max_lms_order_per_filter;
    encoder->verpose_flag             = config->verpose_flag;

    encoder->input_double           = malloc(sizeof(double *) * config->max_num_channels);
    encoder->input_int32            = malloc(sizeof(int32_t *) * config->max_num_channels);
    encoder->res_ltp                = malloc(sizeof(int32_t *) * config->max_num_channels);
    encoder->scratch1               = malloc(sizeof(int32_t *) * config->max_num_channels);
    encoder->scratch2               = malloc(sizeof(int32_t *) * config->max_num_channels);
    encoder->filter_flags           = malloc(sizeof(uint8_t) * config->max_num_channels);
    encoder->residual               = malloc(sizeof(int32_t *) * config->max_num_channels);
    encoder->parcor_coef            = malloc(sizeof(double *) * config->max_num_channels);
    encoder->parcor_coef_int32      = malloc(sizeof(int32_t *) * config->max_num_channels);
    encoder->parcor_coef_code       = malloc(sizeof(int32_t *) * config->max_num_channels);
    encoder->parcor_rshift          = malloc(sizeof(uint32_t) * config->max_num_channels);
    encoder->longterm_coef          = malloc(sizeof(double *) * config->max_num_channels);
    encoder->longterm_coef_int32    = malloc(sizeof(int32_t *) * config->max_num_channels);

    for (uint32_t ch = 0; ch < config->max_num_channels; ch++) {
        encoder->input_double[ch]         = malloc(sizeof(double) * config->max_num_block_samples);
        encoder->input_int32[ch]          = malloc(sizeof(int32_t) * config->max_num_block_samples);
        encoder->res_ltp[ch]              = malloc(sizeof(int32_t) * config->max_num_block_samples);
        encoder->scratch1[ch]             = malloc(sizeof(int32_t) * config->max_num_block_samples);
        encoder->scratch2[ch]             = malloc(sizeof(int32_t) * config->max_num_block_samples);
        encoder->parcor_coef_code[ch]     = malloc(sizeof(int32_t) * config->max_num_block_samples);
        encoder->residual[ch]             = malloc(sizeof(int32_t) * config->max_num_block_samples);
        encoder->parcor_coef[ch]          = malloc(sizeof(double) * (config->max_parcor_order + 1));
        encoder->parcor_coef_int32[ch]    = malloc(sizeof(int32_t) * (config->max_parcor_order + 1));
        encoder->longterm_coef[ch]        = malloc(sizeof(double) * config->max_longterm_order);
        encoder->longterm_coef_int32[ch]  = malloc(sizeof(int32_t) * config->max_longterm_order);
        encoder->lms_num_stages[ch]       = 0;
        for (int stage=0; stage<DANA_MAX_LMS_STAGES; stage++) encoder->lms_step_idx[ch][stage] = 0;
    }

    encoder->pitch_period = malloc(sizeof(uint32_t) * config->max_num_channels);
    encoder->window = malloc(sizeof(double) * config->max_num_block_samples);
    encoder->num_block_partition_samples = malloc(sizeof(uint32_t) * DANAOptimalEncodeEstimator_CalculateMaxNumPartitions(config->max_num_block_samples, DANA_SEARCH_BLOCK_NUM_SAMPLES_DELTA));

    encoder->coder = DANACoder_Create(config->max_num_channels, DANACODER_NUM_RECURSIVERICE_PARAMETER);
    encoder->oee = DANAOptimalEncodeEstimator_Create(config->max_num_block_samples, DANA_SEARCH_BLOCK_NUM_SAMPLES_DELTA);

    encoder->lpcc  = malloc(sizeof(struct DANALPCCalculator *) * config->max_num_channels);
    encoder->ltc   = malloc(sizeof(struct DANALongTermCalculator *) * config->max_num_channels);
    encoder->lpcs  = malloc(sizeof(struct DANALPCSynthesizer *) * config->max_num_channels);
    encoder->ltms  = malloc(sizeof(struct DANALongTermSynthesizer *) * config->max_num_channels);
    encoder->nlmsc = malloc(sizeof(struct DANALMSFilter **) * config->max_num_channels);
    encoder->emp   = malloc(sizeof(struct DANAEmphasisFilter *) * config->max_num_channels);
    
    for (uint32_t ch = 0; ch < config->max_num_channels; ch++) {
        encoder->lpcc[ch]   = DANALPCCalculator_Create(config->max_parcor_order, config->max_num_block_samples);
        encoder->ltc[ch]    = DANALongTermCalculator_Create(DANAUTILITY_ROUNDUP2POWERED(config->max_num_block_samples * 2), DANALONGTERM_MAX_PERIOD, DANALONGTERM_NUM_PITCH_CANDIDATES, config->max_longterm_order);
        encoder->lpcs[ch]   = DANALPCSynthesizer_Create(config->max_parcor_order);
        encoder->ltms[ch]   = DANALongTermSynthesizer_Create(config->max_longterm_order, DANALONGTERM_MAX_PERIOD);
        encoder->emp[ch]    = DANAEmphasisFilter_Create();
        
        encoder->nlmsc[ch] = malloc(sizeof(struct DANALMSFilter *) * DANA_MAX_LMS_STAGES);
        uint32_t ord = config->max_lms_order_per_filter;
        encoder->nlmsc[ch][0] = DANALMSFilter_Create(ord);
        ord = DANAUTILITY_MAX(4, DANAUTILITY_ROUNDUP2POWERED(ord / 2));
        encoder->nlmsc[ch][1] = DANALMSFilter_Create(ord);
        ord = DANAUTILITY_MAX(4, DANAUTILITY_ROUNDUP2POWERED(ord / 2));
        encoder->nlmsc[ch][2] = DANALMSFilter_Create(ord);
    }

    DANAMetadata_Init(&encoder->metadata);
    return encoder;
}

void DANAEncoder_Destroy(struct DANAEncoder* encoder) {
    if (encoder != NULL) {
        for (uint32_t ch = 0; ch < encoder->max_num_channels; ch++) {
            NULLCHECK_AND_FREE(encoder->input_double[ch]);
            NULLCHECK_AND_FREE(encoder->input_int32[ch]);
            NULLCHECK_AND_FREE(encoder->res_ltp[ch]);
            NULLCHECK_AND_FREE(encoder->scratch1[ch]);
            NULLCHECK_AND_FREE(encoder->scratch2[ch]);
            NULLCHECK_AND_FREE(encoder->residual[ch]);
            NULLCHECK_AND_FREE(encoder->parcor_coef[ch]);
            NULLCHECK_AND_FREE(encoder->parcor_coef_int32[ch]);
            NULLCHECK_AND_FREE(encoder->parcor_coef_code[ch]);
            NULLCHECK_AND_FREE(encoder->longterm_coef[ch]);
            NULLCHECK_AND_FREE(encoder->longterm_coef_int32[ch]);
            
            DANALPCCalculator_Destroy(encoder->lpcc[ch]);
            DANALongTermCalculator_Destroy(encoder->ltc[ch]);
            DANALPCSynthesizer_Destroy(encoder->lpcs[ch]);
            DANALongTermSynthesizer_Destroy(encoder->ltms[ch]);
            for (int i=0; i<DANA_MAX_LMS_STAGES; i++) DANALMSFilter_Destroy(encoder->nlmsc[ch][i]);
            NULLCHECK_AND_FREE(encoder->nlmsc[ch]);
            DANAEmphasisFilter_Destroy(encoder->emp[ch]);
        }
        NULLCHECK_AND_FREE(encoder->input_double); NULLCHECK_AND_FREE(encoder->input_int32);
        NULLCHECK_AND_FREE(encoder->res_ltp); NULLCHECK_AND_FREE(encoder->scratch1);
        NULLCHECK_AND_FREE(encoder->scratch2); NULLCHECK_AND_FREE(encoder->filter_flags);
        NULLCHECK_AND_FREE(encoder->residual); 
        NULLCHECK_AND_FREE(encoder->parcor_coef); NULLCHECK_AND_FREE(encoder->parcor_coef_int32);
        NULLCHECK_AND_FREE(encoder->parcor_coef_code); NULLCHECK_AND_FREE(encoder->longterm_coef);
        NULLCHECK_AND_FREE(encoder->longterm_coef_int32); NULLCHECK_AND_FREE(encoder->num_block_partition_samples);
        NULLCHECK_AND_FREE(encoder->parcor_rshift);
        
        DANAOptimalEncodeEstimator_Destroy(encoder->oee);
        NULLCHECK_AND_FREE(encoder->lpcc); NULLCHECK_AND_FREE(encoder->ltc);
        DANACoder_Destroy(encoder->coder);
        NULLCHECK_AND_FREE(encoder->lpcs); NULLCHECK_AND_FREE(encoder->ltms);
        NULLCHECK_AND_FREE(encoder->nlmsc); NULLCHECK_AND_FREE(encoder->emp);
        
        DANAMetadata_Release(&encoder->metadata);
        free(encoder);
    }
}

DANAApiResult DANAEncoder_SetWaveFormat(struct DANAEncoder* encoder, const struct DANAWaveFormat* wave_format) {
    if (encoder == NULL || wave_format == NULL) return DANA_APIRESULT_INVALID_ARGUMENT;
    if ((wave_format->num_channels > encoder->max_num_channels) || (wave_format->bit_per_sample > 32)) return DANA_APIRESULT_EXCEED_HANDLE_CAPACITY;

    encoder->wave_format = *wave_format;
    encoder->status_flag |= DANAENCODER_STATUS_FLAG_SET_WAVE_FORMAT;
    return DANA_APIRESULT_OK;
}

DANAApiResult DANAEncoder_SetEncodeParameter(struct DANAEncoder* encoder, const struct DANAEncodeParameter* encode_param) {
    if (encoder == NULL || encode_param == NULL) return DANA_APIRESULT_INVALID_ARGUMENT;
    if ((encode_param->parcor_order > encoder->max_parcor_order) ||
        (encode_param->longterm_order > encoder->max_longterm_order) ||
        (encode_param->lms_order_per_filter > encoder->max_lms_order_per_filter) ||
        (encode_param->max_num_block_samples > encoder->max_num_block_samples) ||
        (encode_param->max_num_block_samples < DANA_MIN_BLOCK_NUM_SAMPLES)) {
        return DANA_APIRESULT_EXCEED_HANDLE_CAPACITY;
    }

    encoder->encode_param = *encode_param;
    encoder->status_flag |= DANAENCODER_STATUS_FLAG_SET_ENCODE_PARAMETER;
    return DANA_APIRESULT_OK;
}

DANAApiResult DANAEncoder_SetMetadata(struct DANAEncoder* encoder, const struct DANAMetadata* metadata) {
    if (encoder == NULL || metadata == NULL) return DANA_APIRESULT_INVALID_ARGUMENT;
    DANAMetadata_Release(&encoder->metadata);
  
#define COPY_STR(field) if (metadata->field) encoder->metadata.field = DANAUtility_StrDup(metadata->field)
    COPY_STR(title); COPY_STR(artist); COPY_STR(album); COPY_STR(year);
    COPY_STR(genre); COPY_STR(track); COPY_STR(bpm); COPY_STR(key); COPY_STR(lyrics);
#undef COPY_STR

    if (metadata->cover_data && metadata->cover_size > 0) {
        encoder->metadata.cover_size = metadata->cover_size;
        encoder->metadata.cover_data = malloc(metadata->cover_size);
        if (encoder->metadata.cover_data) memcpy(encoder->metadata.cover_data, metadata->cover_data, metadata->cover_size);
    }
    return DANA_APIRESULT_OK;
}

DANAApiResult DANAEncoder_EncodeHeader(const struct DANAHeaderInfo* restrict header, uint8_t* restrict data, uint32_t data_size, uint32_t* restrict header_size_out) {
    if (header == NULL || data == NULL) return DANA_APIRESULT_INVALID_ARGUMENT;

    uint32_t dana_id_size = 0;
    if (header->metadata.title) dana_id_size += 8 + strlen(header->metadata.title);
    if (header->metadata.artist) dana_id_size += 8 + strlen(header->metadata.artist);
    if (header->metadata.album) dana_id_size += 8 + strlen(header->metadata.album);
    if (header->metadata.year) dana_id_size += 8 + strlen(header->metadata.year);
    if (header->metadata.genre) dana_id_size += 8 + strlen(header->metadata.genre);
    if (header->metadata.track) dana_id_size += 8 + strlen(header->metadata.track);
    if (header->metadata.bpm) dana_id_size += 8 + strlen(header->metadata.bpm);
    if (header->metadata.key) dana_id_size += 8 + strlen(header->metadata.key);
    if (header->metadata.lyrics) dana_id_size += 8 + strlen(header->metadata.lyrics);
    if (header->metadata.cover_data && header->metadata.cover_size > 0) dana_id_size += 8 + header->metadata.cover_size;
    if (header->metadata.seek_table && header->metadata.seek_table_size > 0) dana_id_size += 8 + header->metadata.seek_table_size;

    uint32_t total_meta_size = (dana_id_size > 0) ? (8 + dana_id_size) : 0;
    if (data_size < DANA_HEADER_SIZE + total_meta_size) return DANA_APIRESULT_INSUFFICIENT_BUFFER_SIZE;

    uint8_t* data_pos = data;
    DANAByteArray_PutUint8(&data_pos, 'D'); DANAByteArray_PutUint8(&data_pos, 'A');
    DANAByteArray_PutUint8(&data_pos, 0xFF); DANAByteArray_PutUint8(&data_pos, 0x00);

    DANAByteArray_PutUint32(&data_pos, (DANA_HEADER_SIZE + total_meta_size) - 8);
    DANAByteArray_PutUint16(&data_pos, 0); /* CRC16 temp */
    DANAByteArray_PutUint32(&data_pos, DANA_FORMAT_VERSION);
    DANAByteArray_PutUint8(&data_pos, (uint8_t)header->wave_format.num_channels);
    DANAByteArray_PutUint32(&data_pos, header->num_samples);
    DANAByteArray_PutUint32(&data_pos, header->wave_format.sampling_rate);
    DANAByteArray_PutUint8(&data_pos, (uint8_t)header->wave_format.bit_per_sample);
    DANAByteArray_PutUint8(&data_pos, header->wave_format.offset_lshift);
    DANAByteArray_PutUint8(&data_pos, (uint8_t)header->encode_param.parcor_order);
    DANAByteArray_PutUint8(&data_pos, (uint8_t)header->encode_param.longterm_order);
    DANAByteArray_PutUint8(&data_pos, (uint8_t)header->encode_param.lms_order_per_filter);
    DANAByteArray_PutUint8(&data_pos, (uint8_t)header->encode_param.ch_process_method);
    DANAByteArray_PutUint32(&data_pos, header->num_blocks);
    DANAByteArray_PutUint16(&data_pos, (uint16_t)header->encode_param.max_num_block_samples);
    DANAByteArray_PutUint32(&data_pos, header->max_block_size);
    DANAByteArray_PutUint32(&data_pos, header->max_bit_per_second);

    DANA_Assert((data_pos - data) == DANA_HEADER_SIZE);

    uint16_t crc16 = DANAUtility_CalculateCRC16(&data[DANA_HEADER_CRC16_CALC_START_OFFSET], DANA_HEADER_SIZE - DANA_HEADER_CRC16_CALC_START_OFFSET);
    DANAByteArray_WriteUint16(&data[DANA_HEADER_CRC16_CALC_START_OFFSET - 2], crc16);

    if (total_meta_size > 0) {
        DANAByteArray_PutUint8(&data_pos, 'D'); DANAByteArray_PutUint8(&data_pos, 'N');
        DANAByteArray_PutUint8(&data_pos, 'I'); DANAByteArray_PutUint8(&data_pos, 'D');
        DANAByteArray_PutUint32(&data_pos, dana_id_size);

#define WRITE_TAG(tag, str) \
        if (str) { \
            uint32_t len = strlen(str); memcpy(data_pos, tag, 4); data_pos += 4; \
            DANAByteArray_PutUint32(&data_pos, len); memcpy(data_pos, str, len); data_pos += len; \
        }
        WRITE_TAG("TITL", header->metadata.title); WRITE_TAG("ARTS", header->metadata.artist);
        WRITE_TAG("ALBM", header->metadata.album); WRITE_TAG("YEAR", header->metadata.year);
        WRITE_TAG("GENR", header->metadata.genre); WRITE_TAG("TRCK", header->metadata.track);
        WRITE_TAG("BPM ", header->metadata.bpm);   WRITE_TAG("KEY ", header->metadata.key);
        WRITE_TAG("LYRC", header->metadata.lyrics);
#undef WRITE_TAG

        if (header->metadata.cover_data && header->metadata.cover_size > 0) {
            memcpy(data_pos, "COVR", 4); data_pos += 4;
            DANAByteArray_PutUint32(&data_pos, header->metadata.cover_size);
            memcpy(data_pos, header->metadata.cover_data, header->metadata.cover_size);
            data_pos += header->metadata.cover_size;
        }
        if (header->metadata.seek_table && header->metadata.seek_table_size > 0) {
            memcpy(data_pos, "SKTB", 4); data_pos += 4;
            DANAByteArray_PutUint32(&data_pos, header->metadata.seek_table_size);
            memcpy(data_pos, header->metadata.seek_table, header->metadata.seek_table_size);
            data_pos += header->metadata.seek_table_size;
        }
    }

    if (header_size_out) *header_size_out = DANA_HEADER_SIZE + total_meta_size;
    return DANA_APIRESULT_OK;
}

static DANAApiResult DANAEncoder_MakeWindow(struct DANAEncoder* encoder, uint32_t num_samples) {
    DANA_Assert(encoder != NULL);
    DANA_Assert(num_samples <= encoder->encode_param.max_num_block_samples);

    switch (encoder->encode_param.window_function_type) {
        case DANA_WINDOWFUNCTIONTYPE_RECTANGULAR: DANAUtility_MakeRectangularWindow(encoder->window, num_samples); break;
        case DANA_WINDOWFUNCTIONTYPE_SIN: DANAUtility_MakeSinWindow(encoder->window, num_samples); break;
        case DANA_WINDOWFUNCTIONTYPE_HANN: DANAUtility_MakeHannWindow(encoder->window, num_samples); break;
        case DANA_WINDOWFUNCTIONTYPE_BLACKMAN: DANAUtility_MakeBlackmanWindow(encoder->window, num_samples); break;
        case DANA_WINDOWFUNCTIONTYPE_VORBIS: DANAUtility_MakeVorbisWindow(encoder->window, num_samples); break;
        case DANA_WINDOWFUNCTIONTYPE_TUKEY: DANAUtility_MakeTukeyWindow(encoder->window, num_samples, 0.5); break;
        default: return DANA_APIRESULT_INVALID_WINDOWFUNCTION_TYPE;
    }
    return DANA_APIRESULT_OK;
}

static DANAApiResult DANAEncoder_ApplyChProcessing(struct DANAEncoder* encoder, uint32_t num_samples) {
    DANA_Assert(encoder != NULL);
    DANA_Assert(num_samples <= encoder->encode_param.max_num_block_samples);

    switch (encoder->encode_param.ch_process_method) {
        case DANA_CHPROCESSMETHOD_STEREO_MS:
        case DANA_CHPROCESSMETHOD_STEREO_LS:
        case DANA_CHPROCESSMETHOD_STEREO_ADAPTIVE:
            if (encoder->wave_format.num_channels != 2) return DANA_APIRESULT_INVAILD_CHPROCESSMETHOD;
            break;
        default: break;
    }

    switch (encoder->encode_param.ch_process_method) {
        case DANA_CHPROCESSMETHOD_STEREO_MS:
            DANAUtility_LRtoMSDouble(encoder->input_double, encoder->wave_format.num_channels, num_samples);
            DANAUtility_LRtoMSInt32(encoder->input_int32, encoder->wave_format.num_channels, num_samples);
            break;
        case DANA_CHPROCESSMETHOD_STEREO_LS:
            DANAUtility_LRtoLSDouble(encoder->input_double, encoder->wave_format.num_channels, num_samples);
            DANAUtility_LRtoLSInt32(encoder->input_int32, encoder->wave_format.num_channels, num_samples);
            break;
        case DANA_CHPROCESSMETHOD_STEREO_ADAPTIVE:
            for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
                double left = encoder->input_double[0][smpl];
                double right = encoder->input_double[1][smpl];
                encoder->input_double[1][smpl] = right - (encoder->current_alpha_q / 128.0) * left;
                
                int32_t left_i = encoder->input_int32[0][smpl];
                int32_t right_i = encoder->input_int32[1][smpl];
                encoder->input_int32[1][smpl] = right_i - (int32_t)(((int64_t)left_i * encoder->current_alpha_q) >> 7);
            }
            break;
        default: break;
    }
    return DANA_APIRESULT_OK;
}

static DANAApiResult DANAEncoder_SearchOptimalBlockPartitions(struct DANAEncoder* encoder, const int32_t* const* restrict input, uint32_t num_samples, uint32_t min_num_block_samples, uint32_t delta_num_samples, uint32_t max_num_block_samples, uint32_t *optimal_num_partitions, uint32_t *optimal_num_block_samples) {
    if (encoder == NULL || input == NULL || optimal_num_partitions == NULL || optimal_num_block_samples == NULL) return DANA_APIRESULT_INVALID_ARGUMENT;
    if (max_num_block_samples < min_num_block_samples) return DANA_APIRESULT_INVALID_ARGUMENT;

    uint32_t num_channels = encoder->wave_format.num_channels;
    uint32_t parcor_order = encoder->encode_param.parcor_order;

    for (uint32_t ch = 0; ch < num_channels; ch++) {
        for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
            encoder->input_double[ch][smpl] = (double)input[ch][smpl] * DANA_SCALE_2_MINUS_31;
            encoder->input_int32[ch][smpl] = input[ch][smpl] >> (32 - encoder->wave_format.bit_per_sample);
        }
    }

    DANAApiResult api_ret;
    if ((api_ret = DANAEncoder_ApplyChProcessing(encoder, max_num_block_samples)) != DANA_APIRESULT_OK) return api_ret;

    uint32_t silent_smpl;
    for (silent_smpl = 0; silent_smpl < num_samples; silent_smpl++) {
        for (uint32_t ch = 0; ch < num_channels; ch++) {
            if (encoder->input_int32[ch][silent_smpl] != 0) goto DETECT_NOT_SILENCE;
        }
    }

DETECT_NOT_SILENCE:
    if (silent_smpl >= min_num_block_samples) {
        *optimal_num_partitions = 1;
        optimal_num_block_samples[0] = silent_smpl;
        return DANA_APIRESULT_OK;
    }

    uint32_t search_parcor_order = DANAUTILITY_MIN(parcor_order, 8);

    if (DANAOptimalEncodeEstimator_SearchOptimalBlockPartitions(
          encoder->oee, encoder->lpcc[0], (const double* const*)encoder->input_double, 
          num_channels, num_samples, min_num_block_samples, delta_num_samples, max_num_block_samples,
          encoder->wave_format.bit_per_sample, search_parcor_order, optimal_num_partitions, optimal_num_block_samples) != DANAPREDICTOR_APIRESULT_OK) {
        return DANA_APIRESULT_FAILED_TO_CALCULATE_COEF;
    }

    return DANA_APIRESULT_OK;
}

static uint32_t DANAEncoder_CalculateLeftShiftOffset(struct DANAEncoder* encoder, const int32_t* const* restrict input, uint32_t num_samples) {
    DANA_Assert(encoder != NULL && input != NULL);

    uint32_t mask = 0;
    for (uint32_t ch = 0; ch < encoder->wave_format.num_channels; ch++) {
        for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
            mask |= (uint32_t)input[ch][smpl];
        }
    }

    if (mask == 0) return 0;

    uint32_t minabs_bits = 1 + DANAUTILITY_LOG2FLOOR(~mask & (mask - 1));
    DANA_Assert(minabs_bits <= 31);
    DANA_Assert(encoder->wave_format.bit_per_sample >= (32 - minabs_bits));
    return encoder->wave_format.bit_per_sample - (32 - minabs_bits);
}

DANAApiResult DANAEncoder_EncodeBlock(struct DANAEncoder* restrict encoder, const int32_t* const* restrict input, uint32_t num_samples, uint8_t* restrict data, uint32_t data_size, uint32_t* restrict output_size) {
    if (encoder == NULL || input == NULL || data == NULL || output_size == NULL) return DANA_APIRESULT_INVALID_ARGUMENT;
    if ((!(encoder->status_flag & DANAENCODER_STATUS_FLAG_SET_WAVE_FORMAT)) || (!(encoder->status_flag & DANAENCODER_STATUS_FLAG_SET_ENCODE_PARAMETER))) return DANA_APIRESULT_PARAMETER_NOT_SET;
    if (num_samples > encoder->max_num_block_samples) return DANA_APIRESULT_EXCEED_HANDLE_CAPACITY;
    if (data_size <= DANA_BLOCK_HEADER_SIZE) return DANA_APIRESULT_INSUFFICIENT_DATA_SIZE;

    uint32_t num_channels = encoder->wave_format.num_channels;
    uint32_t longterm_order = encoder->encode_param.longterm_order;

    DANAApiResult api_ret;
    if ((api_ret = DANAEncoder_MakeWindow(encoder, num_samples)) != DANA_APIRESULT_OK) return api_ret;

    DANA_Assert(encoder->wave_format.bit_per_sample > encoder->wave_format.offset_lshift);
    DANA_Assert((encoder->wave_format.bit_per_sample - encoder->wave_format.offset_lshift) < 32);

    uint32_t best_parcor_order[DANA_MAX_CHANNELS] = {0};
    uint32_t best_ch_mode = DANA_CHPROCESSMETHOD_NONE;
    int32_t best_alpha_q = 0;

    encoder->block_data_type = DANA_BLOCK_DATA_TYPE_SILENT;
    for (uint32_t ch = 0; ch < num_channels; ch++) {
        for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
            int32_t val = DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(input[ch][smpl], 32 - encoder->wave_format.bit_per_sample + encoder->wave_format.offset_lshift);
            if (val != 0) {
                encoder->block_data_type = DANA_BLOCK_DATA_TYPE_COMPRESSDATA;
                goto DETECT_NOT_SILENCE;
            }
        }
    }

DETECT_NOT_SILENCE:
    if (encoder->block_data_type == DANA_BLOCK_DATA_TYPE_COMPRESSDATA) {
        double min_total_estimate = DBL_MAX;
        uint32_t num_modes = (num_channels == 2 && (encoder->encode_param.ch_process_method == DANA_CHPROCESSMETHOD_STEREO_MS || encoder->encode_param.ch_process_method == DANA_CHPROCESSMETHOD_STEREO_ADAPTIVE)) ? 4 : 1;
        uint32_t modes_to_test[4] = { DANA_CHPROCESSMETHOD_NONE, DANA_CHPROCESSMETHOD_STEREO_MS, DANA_CHPROCESSMETHOD_STEREO_LS, DANA_CHPROCESSMETHOD_STEREO_ADAPTIVE };

        for (uint32_t mode_idx = 0; mode_idx < num_modes; mode_idx++) {
            uint32_t test_mode = modes_to_test[mode_idx];

            for (uint32_t ch = 0; ch < num_channels; ch++) {
                for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
                    encoder->input_double[ch][smpl] = (double)input[ch][smpl] * DANA_SCALE_2_MINUS_31;
                    encoder->input_int32[ch][smpl]  = DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(input[ch][smpl], 32 - encoder->wave_format.bit_per_sample + encoder->wave_format.offset_lshift);
                }
            }

            DANAChannelProcessMethod old_mode = encoder->encode_param.ch_process_method;
            encoder->encode_param.ch_process_method = test_mode;
            
            if (test_mode == DANA_CHPROCESSMETHOD_STEREO_ADAPTIVE) {
                double sum_l2 = 0.0, sum_lr = 0.0;
                for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
                    sum_l2 += encoder->input_double[0][smpl] * encoder->input_double[0][smpl];
                    sum_lr += encoder->input_double[0][smpl] * encoder->input_double[1][smpl];
                }
                double alpha = (sum_l2 > 0.0) ? (sum_lr / sum_l2) : 0.0;
                encoder->current_alpha_q = (int32_t)DANAUtility_Round(alpha * 128.0);
                encoder->current_alpha_q = DANAUTILITY_INNER_VALUE(encoder->current_alpha_q, -128, 127);
            }
            
            DANAEncoder_ApplyChProcessing(encoder, num_samples);
            encoder->encode_param.ch_process_method = old_mode;

            for (uint32_t ch = 0; ch < num_channels; ch++) {
                double sum_sq = 0.0, sum_cross = 0.0;
                for (uint32_t smpl = 1; smpl < num_samples; smpl++) {
                    sum_sq += encoder->input_double[ch][smpl] * encoder->input_double[ch][smpl];
                    sum_cross += encoder->input_double[ch][smpl] * encoder->input_double[ch][smpl - 1];
                }
                double r1 = (sum_sq > 0.0) ? (sum_cross / sum_sq) : 0.0;
                if (r1 > 0.5) {
                    DANAEmphasisFilter_PreEmphasisDouble(encoder->input_double[ch], num_samples, DANA_PRE_EMPHASIS_COEFFICIENT_SHIFT);
                }
            }

            for (uint32_t p_ch = 0; p_ch < num_channels; p_ch++) {
                DANALPCCalculator_CalculatePARCORCoefDouble(encoder->lpcc[p_ch], encoder->input_double[p_ch], num_samples, encoder->parcor_coef[p_ch], encoder->encode_param.parcor_order);
            }

            double mode_total_estimate = 0.0;
            uint32_t mode_best_order[DANA_MAX_CHANNELS] = {0};

            for (uint32_t p_ch = 0; p_ch < num_channels; p_ch++) {
                double min_ch_estimate = DBL_MAX;
                uint32_t ch_best_order = 0;
                
                for (uint32_t test_order = 0; test_order <= encoder->encode_param.parcor_order; test_order++) {
                    double est_code_len;
                    DANALPCCalculator_EstimateCodeLength(encoder->input_double[p_ch], num_samples, encoder->wave_format.bit_per_sample, encoder->parcor_coef[p_ch], test_order, &est_code_len);
                    double ch_estimate = est_code_len * num_samples;
                    
                    uint32_t coef_bits = 0;
                    for (uint32_t ord = 1; ord <= test_order; ord++) {
                        coef_bits += DANA_GET_PARCOR_QUANTIZE_BIT_WIDTH(ord);
                    }
                    ch_estimate += (double)coef_bits / 8.0;

                    if (ch_estimate < min_ch_estimate) {
                        min_ch_estimate = ch_estimate;
                        ch_best_order = test_order;
                    }
                }
                mode_total_estimate += min_ch_estimate;
                mode_best_order[p_ch] = ch_best_order;
            }

            if (mode_total_estimate < min_total_estimate) {
                min_total_estimate = mode_total_estimate;
                best_ch_mode = test_mode;
                best_alpha_q = encoder->current_alpha_q;
                for (uint32_t p_ch = 0; p_ch < num_channels; p_ch++) {
                    best_parcor_order[p_ch] = mode_best_order[p_ch];
                }
            }
        }
    }

    encoder->current_ch_mode = (DANAChannelProcessMethod)best_ch_mode;
    if (best_ch_mode == DANA_CHPROCESSMETHOD_STEREO_ADAPTIVE) encoder->current_alpha_q = best_alpha_q;

    for (uint32_t ch = 0; ch < num_channels; ch++) {
        for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
            encoder->input_double[ch][smpl] = (double)input[ch][smpl] * DANA_SCALE_2_MINUS_31;
            encoder->input_int32[ch][smpl]  = DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(input[ch][smpl], 32 - encoder->wave_format.bit_per_sample + encoder->wave_format.offset_lshift);
        }
    }

    DANAChannelProcessMethod old_mode = encoder->encode_param.ch_process_method;
    encoder->encode_param.ch_process_method = encoder->current_ch_mode;
    DANAEncoder_ApplyChProcessing(encoder, num_samples);
    encoder->encode_param.ch_process_method = old_mode;

    int error_flag = 0;
    int raw_fallback_flag = 0;
    for (uint32_t ch = 0; ch < num_channels; ch++) {
        if (error_flag || raw_fallback_flag) break;

        if (encoder->block_data_type != DANA_BLOCK_DATA_TYPE_COMPRESSDATA) continue;

        double sum_sq = 0.0, sum_cross = 0.0;
        for (uint32_t smpl = 1; smpl < num_samples; smpl++) {
            sum_sq += encoder->input_double[ch][smpl] * encoder->input_double[ch][smpl];
            sum_cross += encoder->input_double[ch][smpl] * encoder->input_double[ch][smpl - 1];
        }
        double r1 = (sum_sq > 0.0) ? (sum_cross / sum_sq) : 0.0;
        encoder->filter_flags[ch] = (r1 > 0.5) ? DANA_CH_FLAG_PRE_EMPHASIS : 0;
        
        for (int i=0; i<DANA_MAX_LMS_STAGES; i++) encoder->lms_step_idx[ch][i] = 0;
        encoder->lms_num_stages[ch] = 0;

        if (encoder->filter_flags[ch] & DANA_CH_FLAG_PRE_EMPHASIS) {
            DANAEmphasisFilter_PreEmphasisDouble(encoder->input_double[ch], num_samples, DANA_PRE_EMPHASIS_COEFFICIENT_SHIFT);
        }

        if (DANALPCCalculator_CalculatePARCORCoefDouble(encoder->lpcc[ch], encoder->input_double[ch], num_samples, encoder->parcor_coef[ch], best_parcor_order[ch]) != DANAPREDICTOR_APIRESULT_OK) {
            error_flag = 1; break;
        }

        double estimated_code_length;
        if (DANALPCCalculator_EstimateCodeLength(encoder->input_double[ch], num_samples, encoder->wave_format.bit_per_sample, encoder->parcor_coef[ch], best_parcor_order[ch], &estimated_code_length) != DANAPREDICTOR_APIRESULT_OK) {
            error_flag = 1; break;
        }
        estimated_code_length = (8.0 * estimated_code_length) / encoder->wave_format.bit_per_sample;

        if (estimated_code_length >= DANA_ESTIMATE_CODELENGTH_THRESHOLD) {
            raw_fallback_flag = 1; break;
        }

        uint32_t bitwidth = DANAUtility_GetDataBitWidth(encoder->input_int32[ch], num_samples);
        encoder->parcor_rshift[ch] = DANAUTILITY_CALC_RSHIFT_FOR_SINT32(bitwidth);

        encoder->parcor_coef_int32[ch][0] = 0;
        DANA_Assert(encoder->parcor_coef[ch][0] == 0.0);
        for (uint32_t ord = 1; ord < best_parcor_order[ch] + 1; ord++) {
            uint32_t qbits = (uint32_t)DANA_GET_PARCOR_QUANTIZE_BIT_WIDTH(ord);
            
            double r = encoder->parcor_coef[ch][ord];
            r = DANAUTILITY_INNER_VALUE(r, -0.9999, 0.9999);
            double z = atanh(r);
            double z_scaled = z * ((double)(1 << (qbits - 1)) / 5.0); 
            
            encoder->parcor_coef_code[ch][ord] = (int32_t)DANAUtility_Round(z_scaled);
            encoder->parcor_coef_code[ch][ord] = DANAUTILITY_INNER_VALUE(encoder->parcor_coef_code[ch][ord], -(1 << (qbits - 1)) + 1, (1 << (qbits - 1)) - 1);
            
            double decoded_z = (double)encoder->parcor_coef_code[ch][ord] * 5.0 / (double)(1 << (qbits - 1));
            double decoded_r = tanh(decoded_z);
            encoder->parcor_coef_int32[ch][ord] = (int32_t)DANAUtility_Round(decoded_r * 32768.0);
            encoder->parcor_coef_int32[ch][ord] = DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(encoder->parcor_coef_int32[ch][ord], encoder->parcor_rshift[ch]);
        } 

        if (DANAEmphasisFilter_Reset(encoder->emp[ch]) != DANAPREDICTOR_APIRESULT_OK) {
            error_flag = 1; break;
        }
        memcpy(encoder->scratch1[ch], encoder->input_int32[ch], sizeof(int32_t) * num_samples);
        if (encoder->filter_flags[ch] & DANA_CH_FLAG_PRE_EMPHASIS) {
            if (DANAEmphasisFilter_PreEmphasisInt32(encoder->emp[ch], encoder->scratch1[ch], num_samples, DANA_PRE_EMPHASIS_COEFFICIENT_SHIFT) != DANAPREDICTOR_APIRESULT_OK) {
                error_flag = 1; break;
            }
        }
        memcpy(encoder->residual[ch], encoder->scratch1[ch], sizeof(int32_t) * num_samples);

        if (DANALPCSynthesizer_Reset(encoder->lpcs[ch]) != DANAPREDICTOR_APIRESULT_OK) {
            error_flag = 1; break;
        }
        if (DANALPCSynthesizer_PredictByParcorCoefInt32(encoder->lpcs[ch], encoder->residual[ch], num_samples, encoder->parcor_coef_int32[ch], best_parcor_order[ch], encoder->scratch1[ch]) != DANAPREDICTOR_APIRESULT_OK) {
            error_flag = 1; break;
        }
        memcpy(encoder->residual[ch], encoder->scratch1[ch], sizeof(int32_t) * num_samples);

        DANAPredictorApiResult predictor_ret = DANALongTermCalculator_CalculateCoef(encoder->ltc[ch], encoder->residual[ch], num_samples, &encoder->pitch_period[ch], encoder->longterm_coef[ch], longterm_order);
        if ((predictor_ret != DANAPREDICTOR_APIRESULT_OK) && (predictor_ret != DANAPREDICTOR_APIRESULT_FAILED_TO_CALCULATION)) {
            error_flag = 1; break;
        }
        if ((predictor_ret == DANAPREDICTOR_APIRESULT_FAILED_TO_CALCULATION) || (encoder->pitch_period[ch] >= DANALONGTERM_MAX_PERIOD)) {
            encoder->pitch_period[ch] = 0;
        }

        for (uint32_t ord = 0; ord < longterm_order; ord++) {
            encoder->longterm_coef_int32[ch][ord] = (int32_t)DANAUtility_Round(encoder->longterm_coef[ch][ord] * DANA_SCALE_2_PLUS_15);
            encoder->longterm_coef_int32[ch][ord] <<= 16;
        }

        uint64_t sad_lpc = 0;
        for (uint32_t smpl = 0; smpl < num_samples; smpl++) sad_lpc += (uint32_t)DANAUTILITY_ABS(encoder->residual[ch][smpl]);
        
        uint64_t sad_ltp = UINT64_MAX;
        uint8_t ltp_avail = 0;

        if (encoder->pitch_period[ch] >= DANALONGTERM_MIN_PITCH_THRESHOULD) {
            if (DANALongTermSynthesizer_Reset(encoder->ltms[ch]) != DANAPREDICTOR_APIRESULT_OK) {
                error_flag = 1; break;
            }
            if (DANALongTermSynthesizer_PredictInt32(encoder->ltms[ch], encoder->residual[ch], num_samples, encoder->pitch_period[ch], encoder->longterm_coef_int32[ch], longterm_order, encoder->res_ltp[ch]) == DANAPREDICTOR_APIRESULT_OK) {
                sad_ltp = 0;
                for (uint32_t smpl = 0; smpl < num_samples; smpl++) sad_ltp += (uint32_t)DANAUTILITY_ABS(encoder->res_ltp[ch][smpl]);
                ltp_avail = 1;
            }
        }

        uint64_t best_overall_sad = sad_lpc;
        uint8_t best_overall_filter = 0; 
        uint8_t best_overall_lms_stages = 0;
        uint8_t best_overall_lms_steps[DANA_MAX_LMS_STAGES] = {0};

        if (ltp_avail && sad_ltp < best_overall_sad) {
            best_overall_sad = sad_ltp;
            best_overall_filter = 1;
            best_overall_lms_stages = 0;
        }

        uint32_t lms_orders[DANA_MAX_LMS_STAGES];
        lms_orders[0] = encoder->encode_param.lms_order_per_filter;
        lms_orders[1] = DANAUTILITY_MAX(4, DANAUTILITY_ROUNDUP2POWERED(lms_orders[0] / 2));
        lms_orders[2] = DANAUTILITY_MAX(4, DANAUTILITY_ROUNDUP2POWERED(lms_orders[1] / 2));

        for (int use_ltp = 0; use_ltp <= (ltp_avail ? 1 : 0); use_ltp++) {
            uint64_t current_sad = use_ltp ? sad_ltp : sad_lpc;
            int32_t* base_res = use_ltp ? encoder->res_ltp[ch] : encoder->residual[ch];
            
            int32_t* buf_in = base_res;
            int32_t* buf_out = encoder->scratch1[ch];
            int32_t* buf_temp = encoder->scratch2[ch];

            uint8_t stages_used = 0;
            uint8_t steps[DANA_MAX_LMS_STAGES] = {0};

            for (int stage = 0; stage < DANA_MAX_LMS_STAGES; stage++) {
                uint32_t order = lms_orders[stage];
                if (order < 4 || order > encoder->encode_param.lms_order_per_filter) break;

                uint64_t best_stage_sad = UINT64_MAX;
                uint8_t best_step = 0;

                for (uint8_t step = 0; step < 4; step++) {
                    DANALMSFilter_Reset(encoder->nlmsc[ch][stage]);
                    DANALMSFilter_PredictInt32(encoder->nlmsc[ch][stage], order, buf_in, num_samples, buf_out, step);
                    uint64_t sad = 0;
                    for (uint32_t i = 0; i < num_samples; i++) sad += (uint32_t)DANAUTILITY_ABS(buf_out[i]);

                    if (sad < best_stage_sad) {
                        best_stage_sad = sad;
                        best_step = step;
                    }
                }

                if (best_stage_sad < current_sad) {
                    current_sad = best_stage_sad;
                    steps[stage] = best_step;
                    stages_used++;
                    
                    DANALMSFilter_Reset(encoder->nlmsc[ch][stage]);
                    DANALMSFilter_PredictInt32(encoder->nlmsc[ch][stage], order, buf_in, num_samples, buf_out, best_step);
                    
                    int32_t* t = buf_in;
                    buf_in = buf_out;
                    buf_out = (t == base_res) ? buf_temp : t; 
                } else {
                    break; 
                }
            }

            if (stages_used > 0 && current_sad < best_overall_sad) {
                best_overall_sad = current_sad;
                best_overall_filter = use_ltp ? 3 : 2;
                best_overall_lms_stages = stages_used;
                for(int i=0; i<stages_used; i++) best_overall_lms_steps[i] = steps[i];
            }
        }

        if (best_overall_filter == 1) {
            memcpy(encoder->residual[ch], encoder->res_ltp[ch], sizeof(int32_t) * num_samples);
            encoder->lms_num_stages[ch] = 0;
        } else if (best_overall_filter == 2 || best_overall_filter == 3) {
            int32_t* buf_in = (best_overall_filter == 3) ? encoder->res_ltp[ch] : encoder->residual[ch];
            int32_t* buf_out = encoder->scratch1[ch];
            int32_t* buf_temp = encoder->scratch2[ch];

            for (int stage = 0; stage < best_overall_lms_stages; stage++) {
                uint32_t order = lms_orders[stage];
                DANALMSFilter_Reset(encoder->nlmsc[ch][stage]);
                DANALMSFilter_PredictInt32(encoder->nlmsc[ch][stage], order, buf_in, num_samples, buf_out, best_overall_lms_steps[stage]);
                
                int32_t* t = buf_in;
                buf_in = buf_out;
                buf_out = (t == ((best_overall_filter == 3) ? encoder->res_ltp[ch] : encoder->residual[ch])) ? buf_temp : t;
            }
            memcpy(encoder->residual[ch], buf_in, sizeof(int32_t) * num_samples);
            
            if (best_overall_filter == 2) encoder->pitch_period[ch] = 0;
            
            encoder->filter_flags[ch] |= (best_overall_lms_stages << 1);
            encoder->lms_num_stages[ch] = best_overall_lms_stages;
            for(int i=0; i<best_overall_lms_stages; i++) {
                encoder->lms_step_idx[ch][i] = best_overall_lms_steps[i];
            }
        } else {
            encoder->pitch_period[ch] = 0;
            encoder->lms_num_stages[ch] = 0;
        }
    }
    if (error_flag) return DANA_APIRESULT_FAILED_TO_PREDICT;
    if (raw_fallback_flag) encoder->block_data_type = DANA_BLOCK_DATA_TYPE_RAWDATA;

    DANACoder_CalculateInitialRecursiveRiceParameter(encoder->coder, DANACODER_NUM_RECURSIVERICE_PARAMETER, (const int32_t **)encoder->residual, num_channels, num_samples);

    DANABitWriter_Open(&encoder->strm, data, data_size);
    DANABitStream_Seek(&encoder->strm, 0, DANABITSTREAM_SEEK_SET);

    DANABitWriter_PutBits(&encoder->strm, DANA_BLOCK_SYNC_CODE, 16);
    DANABitWriter_PutBits(&encoder->strm, 0, 24); /* Block size temp */
    DANABitWriter_PutBits(&encoder->strm, 0, 16); /* CRC16 temp */
    DANABitWriter_PutBits(&encoder->strm, num_samples, 16);
    DANABitWriter_PutBits(&encoder->strm, encoder->block_data_type, 2);
    DANABitWriter_PutBits(&encoder->strm, encoder->current_ch_mode, 3);
    DANABitWriter_PutBits(&encoder->strm, 0, 3);

    if (encoder->current_ch_mode == DANA_CHPROCESSMETHOD_STEREO_ADAPTIVE) {
        DANABitWriter_PutBits(&encoder->strm, (uint32_t)(uint8_t)encoder->current_alpha_q, 8);
    }

    for (uint32_t ch = 0; ch < num_channels; ch++) {
        if (encoder->block_data_type != DANA_BLOCK_DATA_TYPE_COMPRESSDATA) break;

        DANABitWriter_PutBits(&encoder->strm, best_parcor_order[ch], 6);
        DANABitWriter_PutBits(&encoder->strm, encoder->filter_flags[ch], 3);
        
        for (int stage = 0; stage < encoder->lms_num_stages[ch]; stage++) {
            DANABitWriter_PutBits(&encoder->strm, encoder->lms_step_idx[ch][stage], 2);
        }
        
        DANA_Assert(encoder->parcor_rshift[ch] < (1UL << 4));
        DANABitWriter_PutBits(&encoder->strm, encoder->parcor_rshift[ch], 4);
        for (uint32_t ord = 1; ord < best_parcor_order[ch] + 1; ord++) {
            DANABitWriter_PutBits(&encoder->strm, DANAUTILITY_SINT32_TO_UINT32(encoder->parcor_coef_code[ch][ord]), (uint32_t)DANA_GET_PARCOR_QUANTIZE_BIT_WIDTH(ord));
        }

        if (encoder->pitch_period[ch] >= DANALONGTERM_MIN_PITCH_THRESHOULD) {
            DANABitWriter_PutBits(&encoder->strm, 1, 1);
            DANABitWriter_PutBits(&encoder->strm, encoder->pitch_period[ch], DANALONGTERM_PERIOD_NUM_BITS);
            for (uint32_t ord = 0; ord < longterm_order; ord++) {
                DANABitWriter_PutBits(&encoder->strm, DANAUTILITY_SINT32_TO_UINT32(DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(encoder->longterm_coef_int32[ch][ord], 16)), 16);
            }
        } else {
            DANABitWriter_PutBits(&encoder->strm, 0, 1);
        }
        DANACoder_PutInitialRecursiveRiceParameter(encoder->coder, &encoder->strm, DANACODER_NUM_RECURSIVERICE_PARAMETER, encoder->wave_format.bit_per_sample, ch);
    }

    DANABitStream_Flush(&encoder->strm);

    switch (encoder->block_data_type) {
        case DANA_BLOCK_DATA_TYPE_RAWDATA:
        {
            uint32_t output_bits[DANA_MAX_CHANNELS];
            for (uint32_t ch = 0; ch < num_channels; ch++) {
                DANA_Assert(encoder->wave_format.bit_per_sample > encoder->wave_format.offset_lshift);
                output_bits[ch] = encoder->wave_format.bit_per_sample - encoder->wave_format.offset_lshift;
                if ((ch == 1) && (encoder->current_ch_mode == DANA_CHPROCESSMETHOD_STEREO_MS)) output_bits[ch] += 1;
                if ((ch == 1) && (encoder->current_ch_mode == DANA_CHPROCESSMETHOD_STEREO_ADAPTIVE)) output_bits[ch] += 1;
                if ((ch == 1) && (encoder->current_ch_mode == DANA_CHPROCESSMETHOD_STEREO_LS)) output_bits[ch] += 1;
            }
            for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
                for (uint32_t ch = 0; ch < num_channels; ch++) {
                    DANABitWriter_PutBits(&encoder->strm, DANAUTILITY_SINT32_TO_UINT32(encoder->input_int32[ch][smpl]), output_bits[ch]);
                }
            }
        }
        break;
        case DANA_BLOCK_DATA_TYPE_COMPRESSDATA:
            DANACoder_PutDataArray(encoder->coder, &encoder->strm, DANACODER_NUM_RECURSIVERICE_PARAMETER, (const int32_t **)encoder->residual, num_channels, num_samples);
            break;
        case DANA_BLOCK_DATA_TYPE_SILENT:
            break;
        default:
            DANA_Assert(0);
            break;
    }

    DANABitStream_Flush(&encoder->strm);

    int32_t out_size_tmp;
    DANABitStream_Tell(&encoder->strm, &out_size_tmp);
    *output_size = (uint32_t)out_size_tmp;

    uint16_t crc16 = DANAUtility_CalculateCRC16(&data[DANA_BLOCK_CRC16_CALC_START_OFFSET], (*output_size) - DANA_BLOCK_CRC16_CALC_START_OFFSET);

    DANABitStream_Seek(&encoder->strm, DANA_BLOCK_CRC16_CALC_START_OFFSET - 2 - 3, DANABITSTREAM_SEEK_SET);
    DANABitWriter_PutBits(&encoder->strm, (*output_size) - 2 - 3, 24);
    DANABitWriter_PutBits(&encoder->strm, crc16, 16);
    DANABitStream_Close(&encoder->strm);

    return DANA_APIRESULT_OK;
}

DANAApiResult DANAEncoder_EncodeWhole(struct DANAEncoder* restrict encoder, const int32_t* const* restrict input, uint32_t num_samples, uint8_t* restrict data, uint32_t data_size, uint32_t* restrict output_size) {
    if (encoder == NULL || input == NULL || data == NULL || output_size == NULL) return DANA_APIRESULT_INVALID_ARGUMENT;

    struct DANAHeaderInfo header;
    header.wave_format    = encoder->wave_format;
    header.encode_param   = encoder->encode_param;
    header.num_samples    = num_samples;
    header.max_block_size = DANA_MAX_BLOCK_SIZE_INVAILD;
    header.metadata       = encoder->metadata;

    uint32_t header_size_out = 0;
    DANAApiResult api_ret;
    if ((api_ret = DANAEncoder_EncodeHeader(&header, data, data_size, &header_size_out)) != DANA_APIRESULT_OK) return api_ret;

    header.wave_format.offset_lshift = encoder->wave_format.offset_lshift = (uint8_t)DANAEncoder_CalculateLeftShiftOffset(encoder, input, num_samples);
    DANA_Assert(encoder->wave_format.bit_per_sample > encoder->wave_format.offset_lshift);

    uint32_t cur_output_size = header_size_out;
    uint32_t max_block_size = 0;
    uint32_t encode_offset_sample = 0;
    uint32_t num_blocks = 0;
    uint32_t max_bit_per_second = 0;

    uint32_t max_seek_points = (num_samples / encoder->wave_format.sampling_rate) + 2;
    uint32_t* seek_samples = NULL;
    uint32_t* seek_offsets = NULL;
    uint32_t num_seek_points = 0;
    uint32_t next_seek_target = 0;

    if (encoder->enable_seek_table) {
        seek_samples = malloc(sizeof(uint32_t) * max_seek_points);
        seek_offsets = malloc(sizeof(uint32_t) * max_seek_points);
        if (!seek_samples || !seek_offsets) {
            free(seek_samples); free(seek_offsets);
            return DANA_APIRESULT_NG;
        }
    }

    while (encode_offset_sample < num_samples) {
        if (cur_output_size >= data_size) {
            free(seek_samples); free(seek_offsets);
            return DANA_APIRESULT_INSUFFICIENT_BUFFER_SIZE;
        }

        if (encoder->enable_seek_table && encode_offset_sample >= next_seek_target) {
            if (num_seek_points < max_seek_points) {
                seek_samples[num_seek_points] = encode_offset_sample;
                seek_offsets[num_seek_points] = cur_output_size - header_size_out;
                num_seek_points++;
                next_seek_target += encoder->wave_format.sampling_rate;
            }
        }

        const int32_t* input_ptr[DANA_MAX_CHANNELS];
        for (uint32_t ch = 0; ch < encoder->wave_format.num_channels; ch++) {
            input_ptr[ch] = &input[ch][encode_offset_sample];
        }

        uint32_t num_remain_samples = num_samples - encode_offset_sample;
        uint32_t num_partitions;

        if ((api_ret = DANAEncoder_SearchOptimalBlockPartitions(encoder, input_ptr, DANAUTILITY_MIN(encoder->encode_param.max_num_block_samples, num_remain_samples), (uint32_t)DANAUTILITY_MIN(DANA_MIN_BLOCK_NUM_SAMPLES, num_remain_samples), DANA_SEARCH_BLOCK_NUM_SAMPLES_DELTA, DANAUTILITY_MIN(encoder->encode_param.max_num_block_samples, num_remain_samples), &num_partitions, encoder->num_block_partition_samples)) != DANA_APIRESULT_OK) {
            free(seek_samples); free(seek_offsets);
            return api_ret;
        }

        for (uint32_t part = 0; part < num_partitions; part++) {
            uint32_t num_encode_samples = encoder->num_block_partition_samples[part];
            for (uint32_t ch = 0; ch < encoder->wave_format.num_channels; ch++) {
                input_ptr[ch] = &input[ch][encode_offset_sample];
            }
            
            uint32_t block_size;
            if ((api_ret = DANAEncoder_EncodeBlock(encoder, input_ptr, num_encode_samples, &data[cur_output_size], data_size - cur_output_size, &block_size)) != DANA_APIRESULT_OK) {
                free(seek_samples); free(seek_offsets);
                return api_ret;
            }
            
            cur_output_size += block_size;
            encode_offset_sample += num_encode_samples;
            if (block_size > max_block_size) max_block_size = block_size;
            
            uint32_t block_bit_per_second = (8 * block_size * encoder->wave_format.sampling_rate) / num_encode_samples;
            if (block_bit_per_second > max_bit_per_second) max_bit_per_second = block_bit_per_second;
            num_blocks++;
        }

        if (encoder->verpose_flag != 0) {
            uint32_t output_original_size = encode_offset_sample * encoder->wave_format.num_channels * encoder->wave_format.bit_per_sample / 8;
            printf("progress:%2u%% (compress ratio:%3.1f %%)\r", (unsigned int)((100 * encode_offset_sample) / num_samples), ((double)cur_output_size / output_original_size) * 100.0);
            fflush(stdout);
        }
    }

    if (cur_output_size > data_size) {
        free(seek_samples); free(seek_offsets);
        return DANA_APIRESULT_INSUFFICIENT_DATA_SIZE;
    }

    if (encoder->enable_seek_table) {
        uint32_t seek_table_max_size = 4 + num_seek_points * 10;
        uint8_t* sktb_buf = malloc(seek_table_max_size);
        if (sktb_buf) {
            uint8_t* sktb_p = sktb_buf;
            DANAByteArray_PutUint32(&sktb_p, num_seek_points);
            
            uint32_t prev_sample = 0;
            uint32_t prev_offset = 0;
            for(uint32_t i=0; i<num_seek_points; i++) {
                uint32_t d_sample = seek_samples[i] - prev_sample;
                uint32_t d_offset = seek_offsets[i] - prev_offset;
                put_varint(&sktb_p, d_sample);
                put_varint(&sktb_p, d_offset);
                prev_sample = seek_samples[i];
                prev_offset = seek_offsets[i];
            }
            header.metadata.seek_table_size = (uint32_t)(sktb_p - sktb_buf);
            header.metadata.seek_table = sktb_buf;
            
            uint8_t* dummy_hdr = malloc(1024 * 1024);
            uint32_t new_header_size = 0;
            DANAEncoder_EncodeHeader(&header, dummy_hdr, 1024 * 1024, &new_header_size);
            free(dummy_hdr);
            
            uint32_t audio_data_size = cur_output_size - header_size_out;
            if (new_header_size + audio_data_size > data_size) {
                free(seek_samples); free(seek_offsets);
                free(sktb_buf);
                return DANA_APIRESULT_INSUFFICIENT_BUFFER_SIZE;
            }
            
            memmove(data + new_header_size, data + header_size_out, audio_data_size);
            cur_output_size = new_header_size + audio_data_size;
        }
        free(seek_samples);
        free(seek_offsets);
    }

    header.num_blocks = num_blocks;
    header.max_block_size = max_block_size;
    header.max_bit_per_second = max_bit_per_second;
    if ((api_ret = DANAEncoder_EncodeHeader(&header, data, data_size, NULL)) != DANA_APIRESULT_OK) {
        if (encoder->enable_seek_table && header.metadata.seek_table) free(header.metadata.seek_table);
        return api_ret;
    }

    *output_size = cur_output_size;
    if (encoder->enable_seek_table && header.metadata.seek_table) {
        free(header.metadata.seek_table);
    }
    return DANA_APIRESULT_OK;
}