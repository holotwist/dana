#include "DANADecoder.h"
#include "DANAUtility.h"
#include "DANAPredictor.h"
#include "DANACoder.h"
#include "DANABitStream.h"
#include "DANAByteArray.h"
#include "DANAInternal.h"

#include <stdlib.h>
#include <string.h>
#include <math.h>

#define DANADECODER_STATUS_FLAG_SET_WAVE_FORMAT      (1 << 0)
#define DANADECODER_STATUS_FLAG_SET_ENCODE_PARAMETER (1 << 1)

struct DANABlockHeaderInfo {
    uint32_t block_size;
    uint32_t block_num_samples;
};

struct DANADecoder {
    struct DANAWaveFormat            wave_format;
    struct DANAEncodeParameter       encode_param;
    uint32_t                         max_num_channels;
    uint32_t                         max_num_block_samples;
    uint32_t                         max_parcor_order;
    uint32_t                         max_longterm_order;
    uint32_t                         max_lms_order_per_filter;
    uint8_t                          enable_crc_check;
    struct DANABitStream             strm;
    struct DANACoder*                coder;
    struct DANALPCSynthesizer**      lpcs;
    struct DANALongTermSynthesizer** ltms;
    struct DANALMSFilter***          nlmsc;
    struct DANAEmphasisFilter**      emp;
    int32_t**                        parcor_coef;
    int32_t**                        longterm_coef;
    uint32_t*                        pitch_period;
    DANABlockDataType                block_data_type;
    int32_t**                        residual;
    int32_t**                        output;
    uint32_t                         current_parcor_order[DANA_MAX_CHANNELS];
    DANAChannelProcessMethod         current_ch_mode;
    uint32_t                         status_flag;
    uint8_t                          verpose_flag;
    uint8_t*                         filter_flags;
    int32_t                          current_alpha_q;
    uint8_t                          lms_num_stages[DANA_MAX_CHANNELS];
    uint8_t                          lms_step_idx[DANA_MAX_CHANNELS][DANA_MAX_LMS_STAGES];
};

struct DANAStreamingDecoder {
    struct DANADecoder*         decoder_core;
    uint8_t*                    data_buffer;
    uint32_t                    data_buffer_size;
    uint32_t                    data_buffer_provided_size;
    uint32_t                    num_output_samples_per_decode;
    struct DANABlockHeaderInfo  current_block_header;
    uint32_t                    current_block_sample_offset;
    float                       estimated_bytes_per_sample;
    float                       decode_interval_hz;
    uint32_t                    max_bit_per_sample;
    struct DANADataPacketQueue* queue;
    int32_t**                   pcm_cache;
    uint32_t                    pcm_cache_num_samples;
};

struct DANADecoder* DANADecoder_Create(const struct DANADecoderConfig* config) {
    if (config == NULL) return NULL;

    struct DANADecoder* decoder = calloc(1, sizeof(struct DANADecoder));
    if (!decoder) return NULL;

    decoder->max_num_channels         = config->max_num_channels;
    decoder->max_num_block_samples    = config->max_num_block_samples;
    decoder->max_parcor_order         = config->max_parcor_order;
    decoder->max_longterm_order       = config->max_longterm_order;
    decoder->max_lms_order_per_filter = config->max_lms_order_per_filter;
    decoder->enable_crc_check         = config->enable_crc_check;
    decoder->verpose_flag             = config->verpose_flag;

    decoder->parcor_coef   = malloc(sizeof(int32_t*) * config->max_num_channels);
    decoder->longterm_coef = malloc(sizeof(int32_t*) * config->max_num_channels);
    decoder->pitch_period  = malloc(sizeof(uint32_t) * config->max_num_channels);
    decoder->residual      = malloc(sizeof(int32_t*) * config->max_num_channels);
    decoder->output        = malloc(sizeof(int32_t*) * config->max_num_channels);
    decoder->filter_flags  = malloc(sizeof(uint8_t) * config->max_num_channels);
    
    for (uint32_t ch = 0; ch < config->max_num_channels; ch++) {
        decoder->parcor_coef[ch]   = malloc(sizeof(int32_t) * (config->max_parcor_order + 1));
        decoder->longterm_coef[ch] = malloc(sizeof(int32_t) * config->max_longterm_order);
        decoder->residual[ch]      = malloc(sizeof(int32_t) * config->max_num_block_samples);
        decoder->output[ch]        = malloc(sizeof(int32_t) * config->max_num_block_samples);
        decoder->lms_num_stages[ch]= 0;
        for (int stage=0; stage<DANA_MAX_LMS_STAGES; stage++) decoder->lms_step_idx[ch][stage] = 0;
    }

    decoder->coder = DANACoder_Create(config->max_num_channels, DANACODER_NUM_RECURSIVERICE_PARAMETER);
    decoder->lpcs  = malloc(sizeof(struct DANALPCSynthesizer *) * config->max_num_channels);
    decoder->ltms  = malloc(sizeof(struct DANALongTermSynthesizer *) * config->max_num_channels);
    decoder->nlmsc = malloc(sizeof(struct DANALMSFilter **) * config->max_num_channels);
    decoder->emp   = malloc(sizeof(struct DANAEmphasisFilter *) * config->max_num_channels);
    
    for (uint32_t ch = 0; ch < config->max_num_channels; ch++) {
        decoder->lpcs[ch]  = DANALPCSynthesizer_Create(config->max_parcor_order);
        decoder->ltms[ch]  = DANALongTermSynthesizer_Create(config->max_longterm_order, DANALONGTERM_MAX_PERIOD);
        decoder->emp[ch]   = DANAEmphasisFilter_Create();
        
        decoder->nlmsc[ch] = malloc(sizeof(struct DANALMSFilter *) * DANA_MAX_LMS_STAGES);
        uint32_t ord = config->max_lms_order_per_filter;
        decoder->nlmsc[ch][0] = DANALMSFilter_Create(ord);
        ord = DANAUTILITY_MAX(4, DANAUTILITY_ROUNDUP2POWERED(ord / 2));
        decoder->nlmsc[ch][1] = DANALMSFilter_Create(ord);
        ord = DANAUTILITY_MAX(4, DANAUTILITY_ROUNDUP2POWERED(ord / 2));
        decoder->nlmsc[ch][2] = DANALMSFilter_Create(ord);
    }

    decoder->status_flag = 0;
    return decoder;
}

void DANADecoder_Destroy(struct DANADecoder* decoder) {
    if (decoder != NULL) {
        for (uint32_t ch = 0; ch < decoder->max_num_channels; ch++) {
            NULLCHECK_AND_FREE(decoder->output[ch]);
            NULLCHECK_AND_FREE(decoder->residual[ch]);
            NULLCHECK_AND_FREE(decoder->parcor_coef[ch]);
            NULLCHECK_AND_FREE(decoder->longterm_coef[ch]);
            
            DANALPCSynthesizer_Destroy(decoder->lpcs[ch]);
            DANALongTermSynthesizer_Destroy(decoder->ltms[ch]);
            for (int stage=0; stage<DANA_MAX_LMS_STAGES; stage++) DANALMSFilter_Destroy(decoder->nlmsc[ch][stage]);
            NULLCHECK_AND_FREE(decoder->nlmsc[ch]);
            DANAEmphasisFilter_Destroy(decoder->emp[ch]);
        }
        NULLCHECK_AND_FREE(decoder->residual); NULLCHECK_AND_FREE(decoder->output);
        NULLCHECK_AND_FREE(decoder->parcor_coef); NULLCHECK_AND_FREE(decoder->longterm_coef);
        NULLCHECK_AND_FREE(decoder->filter_flags);
        NULLCHECK_AND_FREE(decoder->lpcs); NULLCHECK_AND_FREE(decoder->ltms);
        NULLCHECK_AND_FREE(decoder->nlmsc); NULLCHECK_AND_FREE(decoder->emp);
        DANACoder_Destroy(decoder->coder);
        free(decoder);
    }
}

DANAApiResult DANADecoder_DecodeHeader(const uint8_t* restrict data, uint32_t data_size, struct DANAHeaderInfo* restrict header_info, uint32_t* restrict header_size_out) {
    if (data == NULL || header_info == NULL) return DANA_APIRESULT_INVALID_ARGUMENT;
    if (data_size < DANA_HEADER_SIZE) return DANA_APIRESULT_INSUFFICIENT_DATA_SIZE;

    const uint8_t* data_pos = data;
    DANAApiResult ret = DANA_APIRESULT_OK;
    struct DANAHeaderInfo tmp_header;
    DANAMetadata_Init(&tmp_header.metadata);

    uint8_t signature[4];
    DANAByteArray_GetUint8(&data_pos, &signature[0]);
    DANAByteArray_GetUint8(&data_pos, &signature[1]);
    DANAByteArray_GetUint8(&data_pos, &signature[2]);
    DANAByteArray_GetUint8(&data_pos, &signature[3]);
    if ((signature[0] != 'D') || (signature[1] != 'A') || (signature[2] != 0xFF) || (signature[3] != 0x00)) {
        DANAMetadata_Release(&tmp_header.metadata);
        return DANA_APIRESULT_INVALID_HEADER_FORMAT;
    }

    uint32_t header_offset;
    DANAByteArray_GetUint32(&data_pos, &header_offset);
    
    uint16_t u16buf;
    DANAByteArray_GetUint16(&data_pos, &u16buf);
    if (u16buf != DANAUtility_CalculateCRC16(&data[DANA_HEADER_CRC16_CALC_START_OFFSET], DANA_HEADER_SIZE - DANA_HEADER_CRC16_CALC_START_OFFSET)) {
        ret = DANA_APIRESULT_DETECT_DATA_CORRUPTION;
    }

    uint32_t u32buf;
    DANAByteArray_GetUint32(&data_pos, &u32buf);
    if (u32buf != DANA_FORMAT_VERSION) {
        DANAMetadata_Release(&tmp_header.metadata);
        return DANA_APIRESULT_INVALID_HEADER_FORMAT;
    }

    uint8_t u8buf;
    DANAByteArray_GetUint8(&data_pos, &u8buf); tmp_header.wave_format.num_channels = (uint32_t)u8buf;
    DANAByteArray_GetUint32(&data_pos, &tmp_header.num_samples);
    DANAByteArray_GetUint32(&data_pos, &tmp_header.wave_format.sampling_rate);
    DANAByteArray_GetUint8(&data_pos, &u8buf); tmp_header.wave_format.bit_per_sample = (uint32_t)u8buf;
    DANAByteArray_GetUint8(&data_pos, &tmp_header.wave_format.offset_lshift);
    DANAByteArray_GetUint8(&data_pos, &u8buf); tmp_header.encode_param.parcor_order = (uint32_t)u8buf;
    DANAByteArray_GetUint8(&data_pos, &u8buf); tmp_header.encode_param.longterm_order = (uint32_t)u8buf;
    DANAByteArray_GetUint8(&data_pos, &u8buf); tmp_header.encode_param.lms_order_per_filter = (uint32_t)u8buf;
    DANAByteArray_GetUint8(&data_pos, &u8buf); tmp_header.encode_param.ch_process_method = (uint32_t)u8buf;
    DANAByteArray_GetUint32(&data_pos, &tmp_header.num_blocks);
    DANAByteArray_GetUint16(&data_pos, &u16buf); tmp_header.encode_param.max_num_block_samples = (uint32_t)u16buf;
    DANAByteArray_GetUint32(&data_pos, &tmp_header.max_block_size);
    DANAByteArray_GetUint32(&data_pos, &tmp_header.max_bit_per_second);

    DANA_Assert((data_pos - data) == DANA_HEADER_SIZE);

    uint32_t first_block_offset = header_offset + 8;
    if (header_size_out) *header_size_out = first_block_offset;

    if (first_block_offset > DANA_HEADER_SIZE && data_size >= first_block_offset) {
        const uint8_t* meta_ptr = data + DANA_HEADER_SIZE;
        if (memcmp(meta_ptr, "DNID", 4) == 0) {
            meta_ptr += 4;
            uint32_t total_size = DANAByteArray_ReadUint32(meta_ptr); meta_ptr += 4;
            const uint8_t* end_ptr = data + DANA_HEADER_SIZE + 8 + total_size;
            
            if (end_ptr <= data + data_size) {
                while (meta_ptr + 8 <= end_ptr) {
                    char tag[5] = {0};
                    memcpy(tag, meta_ptr, 4); meta_ptr += 4;
                    uint32_t t_size = DANAByteArray_ReadUint32(meta_ptr); meta_ptr += 4;
                    
                    if (meta_ptr + t_size > end_ptr) {
                        ret = DANA_APIRESULT_DETECT_DATA_CORRUPTION;
                        break;
                    }
                    
                    if (strcmp(tag, "TITL") == 0) tmp_header.metadata.title = DANAUtility_StrNDup((const char*)meta_ptr, t_size);
                    else if (strcmp(tag, "ARTS") == 0) tmp_header.metadata.artist = DANAUtility_StrNDup((const char*)meta_ptr, t_size);
                    else if (strcmp(tag, "ALBM") == 0) tmp_header.metadata.album = DANAUtility_StrNDup((const char*)meta_ptr, t_size);
                    else if (strcmp(tag, "YEAR") == 0) tmp_header.metadata.year = DANAUtility_StrNDup((const char*)meta_ptr, t_size);
                    else if (strcmp(tag, "GENR") == 0) tmp_header.metadata.genre = DANAUtility_StrNDup((const char*)meta_ptr, t_size);
                    else if (strcmp(tag, "TRCK") == 0) tmp_header.metadata.track = DANAUtility_StrNDup((const char*)meta_ptr, t_size);
                    else if (strcmp(tag, "BPM ") == 0) tmp_header.metadata.bpm = DANAUtility_StrNDup((const char*)meta_ptr, t_size);
                    else if (strcmp(tag, "KEY ") == 0) tmp_header.metadata.key = DANAUtility_StrNDup((const char*)meta_ptr, t_size);
                    else if (strcmp(tag, "LYRC") == 0) tmp_header.metadata.lyrics = DANAUtility_StrNDup((const char*)meta_ptr, t_size);
                    else if (strcmp(tag, "COVR") == 0) { 
                        tmp_header.metadata.cover_size = t_size; 
                        tmp_header.metadata.cover_data = malloc(t_size); 
                        if (tmp_header.metadata.cover_data) memcpy(tmp_header.metadata.cover_data, meta_ptr, t_size); 
                    }
                    else if (strcmp(tag, "SKTB") == 0) {
                        tmp_header.metadata.seek_table_size = t_size;
                        tmp_header.metadata.seek_table = malloc(t_size);
                        if (tmp_header.metadata.seek_table) memcpy(tmp_header.metadata.seek_table, meta_ptr, t_size);
                    }
                    meta_ptr += t_size;
                }
            }
        }
    }

    *header_info = tmp_header;
    if (ret != DANA_APIRESULT_OK) {
        DANAMetadata_Release(&header_info->metadata);
    }
    return ret;
}

DANAApiResult DANAMetadata_Copy(struct DANAMetadata* restrict dst, const struct DANAMetadata* restrict src) {
    if (!dst || !src) return DANA_APIRESULT_INVALID_ARGUMENT;
    DANAMetadata_Init(dst);

#define COPY_FIELD(f) if(src->f) dst->f = DANAUtility_StrDup(src->f)
    COPY_FIELD(title); COPY_FIELD(artist); COPY_FIELD(album); COPY_FIELD(year);
    COPY_FIELD(genre); COPY_FIELD(track); COPY_FIELD(bpm); COPY_FIELD(key); COPY_FIELD(lyrics);
#undef COPY_FIELD

    if (src->cover_data && src->cover_size > 0) {
        dst->cover_size = src->cover_size;
        dst->cover_data = malloc(src->cover_size);
        if (dst->cover_data) memcpy(dst->cover_data, src->cover_data, src->cover_size);
    }
    if (src->seek_table && src->seek_table_size > 0) {
        dst->seek_table_size = src->seek_table_size;
        dst->seek_table = malloc(src->seek_table_size);
        if (dst->seek_table) memcpy(dst->seek_table, src->seek_table, src->seek_table_size);
    }
    return DANA_APIRESULT_OK;
}

static inline uint32_t get_varint(const uint8_t** p) {
    uint32_t val = 0;
    uint32_t shift = 0;
    while (1) {
        uint8_t b = **p;
        *p += 1;
        val |= (b & 0x7F) << shift;
        if (!(b & 0x80)) break;
        shift += 7;
    }
    return val;
}

DANAApiResult DANADecoder_GetSeekPoint(const struct DANAMetadata* meta, uint32_t target_sample, uint32_t* out_sample, uint32_t* out_byte_offset) {
    if (!meta || !meta->seek_table || meta->seek_table_size < 4 || !out_sample || !out_byte_offset) return DANA_APIRESULT_INVALID_ARGUMENT;
    
    const uint8_t* p = meta->seek_table;
    uint32_t num_entries = DANAByteArray_ReadUint32(p); p += 4;
    
    uint32_t current_sample = 0;
    uint32_t current_offset = 0;
    uint32_t best_sample = 0;
    uint32_t best_offset = 0;
    
    for (uint32_t i = 0; i < num_entries; i++) {
        if (p >= meta->seek_table + meta->seek_table_size) break;
        uint32_t d_sample = get_varint(&p);
        uint32_t d_offset = get_varint(&p);
        
        current_sample += d_sample;
        current_offset += d_offset;
        
        if (current_sample <= target_sample) {
            best_sample = current_sample;
            best_offset = current_offset;
        } else {
            break;
        }
    }
    
    *out_sample = best_sample;
    *out_byte_offset = best_offset;
    return DANA_APIRESULT_OK;
}

DANAApiResult DANADecoder_SetWaveFormat(struct DANADecoder* decoder, const struct DANAWaveFormat* wave_format) {
    if (decoder == NULL || wave_format == NULL) return DANA_APIRESULT_INVALID_ARGUMENT;
    if ((wave_format->num_channels > decoder->max_num_channels) || (wave_format->bit_per_sample > 32)) return DANA_APIRESULT_EXCEED_HANDLE_CAPACITY;

    decoder->wave_format = *wave_format;
    decoder->status_flag |= DANADECODER_STATUS_FLAG_SET_WAVE_FORMAT;
    return DANA_APIRESULT_OK;
}

DANAApiResult DANADecoder_SetEncodeParameter(struct DANADecoder* decoder, const struct DANAEncodeParameter* encode_param) {
    if (decoder == NULL || encode_param == NULL) return DANA_APIRESULT_INVALID_ARGUMENT;
    if ((encode_param->parcor_order > decoder->max_parcor_order) ||
        (encode_param->longterm_order > decoder->max_longterm_order) ||
        (encode_param->lms_order_per_filter > decoder->max_lms_order_per_filter) ||
        (encode_param->max_num_block_samples > decoder->max_num_block_samples) ||
        (encode_param->max_num_block_samples < DANA_MIN_BLOCK_NUM_SAMPLES)) {
        return DANA_APIRESULT_EXCEED_HANDLE_CAPACITY;
    }

    decoder->encode_param = *encode_param;
    decoder->status_flag |= DANADECODER_STATUS_FLAG_SET_ENCODE_PARAMETER;
    return DANA_APIRESULT_OK;
}

static DANAApiResult DANADecoder_DecodeBlockHeader(struct DANADecoder* decoder, const uint8_t* restrict data, uint32_t data_size, struct DANABlockHeaderInfo* restrict block_header_info, uint32_t* restrict block_header_size) {
    if ((decoder == NULL) || (data == NULL) || (block_header_info == NULL)) return DANA_APIRESULT_INVALID_ARGUMENT;
    if ((!(decoder->status_flag & DANADECODER_STATUS_FLAG_SET_WAVE_FORMAT)) || (!(decoder->status_flag & DANADECODER_STATUS_FLAG_SET_ENCODE_PARAMETER))) return DANA_APIRESULT_PARAMETER_NOT_SET;
    if (data_size < DANA_MINIMUM_BLOCK_HEADER_SIZE) return DANA_APIRESULT_INSUFFICIENT_DATA_SIZE;

    uint64_t bitsbuf;
    DANABitReader_GetBits(&decoder->strm, &bitsbuf, 16);
    if (bitsbuf != DANA_BLOCK_SYNC_CODE) return DANA_APIRESULT_FAILED_TO_FIND_SYNC_CODE;

    DANABitReader_GetBits(&decoder->strm, &bitsbuf, 24);
    block_header_info->block_size = (uint32_t)bitsbuf + 2 + 3;

    DANABitReader_GetBits(&decoder->strm, &bitsbuf, 16);
    uint16_t crc16 = (uint16_t)bitsbuf;
    if ((decoder->enable_crc_check == 1) && (data_size >= block_header_info->block_size)) {
        DANA_Assert(block_header_info->block_size >= DANA_BLOCK_CRC16_CALC_START_OFFSET);
        uint16_t calc_crc16 = DANAUtility_CalculateCRC16(&data[DANA_BLOCK_CRC16_CALC_START_OFFSET], block_header_info->block_size - DANA_BLOCK_CRC16_CALC_START_OFFSET);
        if (calc_crc16 != crc16) return DANA_APIRESULT_DETECT_DATA_CORRUPTION;
    }

    DANABitReader_GetBits(&decoder->strm, &bitsbuf, 16);
    block_header_info->block_num_samples = (uint32_t)bitsbuf;
    if (block_header_info->block_num_samples > decoder->max_num_block_samples) return DANA_APIRESULT_DETECT_DATA_CORRUPTION;

    DANABitReader_GetBits(&decoder->strm, &bitsbuf, 2);
    decoder->block_data_type = (DANABlockDataType)bitsbuf;
    if (decoder->block_data_type >= DANA_BLOCK_DATA_TYPE_INVAILD) return DANA_APIRESULT_DETECT_DATA_CORRUPTION;

    DANABitReader_GetBits(&decoder->strm, &bitsbuf, 3);
    decoder->current_ch_mode = (DANAChannelProcessMethod)bitsbuf;

    DANABitReader_GetBits(&decoder->strm, &bitsbuf, 3); /* Padding */

    if (decoder->current_ch_mode == DANA_CHPROCESSMETHOD_STEREO_ADAPTIVE) {
        DANABitReader_GetBits(&decoder->strm, &bitsbuf, 8);
        decoder->current_alpha_q = (int8_t)bitsbuf;
    }

    for (uint32_t ch = 0; ch < decoder->wave_format.num_channels; ch++) {
        if (decoder->block_data_type != DANA_BLOCK_DATA_TYPE_COMPRESSDATA) break;

        DANABitReader_GetBits(&decoder->strm, &bitsbuf, 6);
        decoder->current_parcor_order[ch] = (uint32_t)bitsbuf;
        if (decoder->current_parcor_order[ch] > decoder->max_parcor_order) return DANA_APIRESULT_DETECT_DATA_CORRUPTION;

        DANABitReader_GetBits(&decoder->strm, &bitsbuf, 3);
        decoder->filter_flags[ch] = (uint8_t)bitsbuf;
        decoder->lms_num_stages[ch] = (decoder->filter_flags[ch] >> 1) & 3;

        for (int stage = 0; stage < decoder->lms_num_stages[ch]; stage++) {
            DANABitReader_GetBits(&decoder->strm, &bitsbuf, 2);
            decoder->lms_step_idx[ch][stage] = (uint8_t)bitsbuf;
        }

        DANABitReader_GetBits(&decoder->strm, &bitsbuf, 4);
        uint32_t rshift = (uint32_t)bitsbuf;

        decoder->parcor_coef[ch][0] = 0;
        for (uint32_t ord = 1; ord < decoder->current_parcor_order[ch] + 1; ord++) {
            uint32_t qbits = (uint32_t)DANA_GET_PARCOR_QUANTIZE_BIT_WIDTH(ord);
            DANABitReader_GetBits(&decoder->strm, &bitsbuf, qbits);
            
            int32_t code = DANAUTILITY_UINT32_TO_SINT32(bitsbuf);
            double decoded_z = (double)code * 5.0 / (double)(1 << (qbits - 1));
            double decoded_r = tanh(decoded_z); 
            int32_t r_int = (int32_t)DANAUtility_Round(decoded_r * 32768.0);
            decoder->parcor_coef[ch][ord] = DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(r_int, rshift);
        }

        DANABitReader_GetBits(&decoder->strm, &bitsbuf, 1);
        if (bitsbuf == 0) {
            decoder->pitch_period[ch] = 0;
        } else {
            DANABitReader_GetBits(&decoder->strm, &bitsbuf, DANALONGTERM_PERIOD_NUM_BITS);
            decoder->pitch_period[ch] = (uint32_t)bitsbuf;
            if (decoder->pitch_period[ch] >= DANALONGTERM_MAX_PERIOD) return DANA_APIRESULT_DETECT_DATA_CORRUPTION;
            
            for (uint32_t ord = 0; ord < decoder->encode_param.longterm_order; ord++) {
                DANABitReader_GetBits(&decoder->strm, &bitsbuf, 16);
                decoder->longterm_coef[ch][ord] = DANAUTILITY_UINT32_TO_SINT32(bitsbuf);
                decoder->longterm_coef[ch][ord] <<= 16;
            }
        }
        DANACoder_GetInitialRecursiveRiceParameter(decoder->coder, &decoder->strm, DANACODER_NUM_RECURSIVERICE_PARAMETER, decoder->wave_format.bit_per_sample, ch);
    }

    DANABitStream_Flush(&decoder->strm);
    int32_t header_sz;
    DANABitStream_Tell(&decoder->strm, &header_sz);
    *block_header_size = (uint32_t)header_sz;

    return DANA_APIRESULT_OK;
}

static DANAApiResult DANADecoder_DecodeWaveData(struct DANADecoder* decoder, int32_t** restrict buffer, uint32_t num_decode_saples, uint32_t* restrict output_data_size) {
    if ((decoder == NULL) || (buffer == NULL)) return DANA_APIRESULT_INVALID_ARGUMENT;
    if ((!(decoder->status_flag & DANADECODER_STATUS_FLAG_SET_WAVE_FORMAT)) || (!(decoder->status_flag & DANADECODER_STATUS_FLAG_SET_ENCODE_PARAMETER))) return DANA_APIRESULT_PARAMETER_NOT_SET;

    int32_t start_data_offset, end_data_offset; 
    DANABitStream_Tell(&decoder->strm, &start_data_offset);

    uint32_t num_channels = decoder->wave_format.num_channels;

    switch (decoder->block_data_type) {
        case DANA_BLOCK_DATA_TYPE_SILENT:
            for (uint32_t ch = 0; ch < num_channels; ch++) {
                memset(decoder->output[ch], 0, sizeof(int32_t) * num_decode_saples);
            }
            break;
        case DANA_BLOCK_DATA_TYPE_RAWDATA:
        {
            uint32_t input_bits[DANA_MAX_CHANNELS];
            for (uint32_t ch = 0; ch < num_channels; ch++) {
                DANA_Assert(decoder->wave_format.bit_per_sample > decoder->wave_format.offset_lshift);
                input_bits[ch] = decoder->wave_format.bit_per_sample - decoder->wave_format.offset_lshift;
                if ((ch == 1) && (decoder->current_ch_mode == DANA_CHPROCESSMETHOD_STEREO_MS)) input_bits[ch] += 1;
                if ((ch == 1) && (decoder->current_ch_mode == DANA_CHPROCESSMETHOD_STEREO_ADAPTIVE)) input_bits[ch] += 1;
                if ((ch == 1) && (decoder->current_ch_mode == DANA_CHPROCESSMETHOD_STEREO_LS)) input_bits[ch] += 1;
            }
            for (uint32_t smpl = 0; smpl < num_decode_saples; smpl++) {
                for (uint32_t ch = 0; ch < num_channels; ch++) {
                    uint64_t bitsbuf;
                    DANABitReader_GetBits(&decoder->strm, &bitsbuf, input_bits[ch]);
                    decoder->output[ch][smpl] = DANAUTILITY_UINT32_TO_SINT32((uint32_t)bitsbuf);
                }
            }
        }
        break;
        case DANA_BLOCK_DATA_TYPE_COMPRESSDATA:
            DANACoder_GetDataArray(decoder->coder, &decoder->strm, DANACODER_NUM_RECURSIVERICE_PARAMETER, decoder->residual, num_channels, num_decode_saples);
            break;
        default:
            DANA_Assert(0);
            break;
    }

    int error_flag = 0;
    for (uint32_t ch = 0; ch < num_channels; ch++) {
        if (error_flag) break;
        if (decoder->block_data_type != DANA_BLOCK_DATA_TYPE_COMPRESSDATA) continue;

        uint32_t lms_orders[DANA_MAX_LMS_STAGES];
        lms_orders[0] = decoder->encode_param.lms_order_per_filter;
        lms_orders[1] = DANAUTILITY_MAX(4, DANAUTILITY_ROUNDUP2POWERED(lms_orders[0] / 2));
        lms_orders[2] = DANAUTILITY_MAX(4, DANAUTILITY_ROUNDUP2POWERED(lms_orders[1] / 2));

        uint8_t num_lms_stages = decoder->lms_num_stages[ch];
        if (num_lms_stages > 0) {
            for (int stage = num_lms_stages - 1; stage >= 0; stage--) {
                uint32_t order = lms_orders[stage];
                if (DANALMSFilter_SynthesizeInt32(decoder->nlmsc[ch][stage], order, decoder->residual[ch], num_decode_saples, decoder->output[ch], decoder->lms_step_idx[ch][stage]) != DANAPREDICTOR_APIRESULT_OK) {
                    error_flag = 1; break;
                }
                memcpy(decoder->residual[ch], decoder->output[ch], sizeof(int32_t) * num_decode_saples);
            }
        }

        if (error_flag) break;

        if (decoder->pitch_period[ch] != 0) {
            if (DANALongTermSynthesizer_SynthesizeInt32(decoder->ltms[ch], decoder->residual[ch], num_decode_saples, decoder->pitch_period[ch], decoder->longterm_coef[ch], decoder->encode_param.longterm_order, decoder->output[ch]) != DANAPREDICTOR_APIRESULT_OK) {
                error_flag = 1; break;
            }
            memcpy(decoder->residual[ch], decoder->output[ch], sizeof(int32_t) * num_decode_saples);
        }

        if (DANALPCSynthesizer_SynthesizeByParcorCoefInt32(decoder->lpcs[ch], decoder->residual[ch], num_decode_saples, decoder->parcor_coef[ch], decoder->current_parcor_order[ch], decoder->output[ch]) != DANAPREDICTOR_APIRESULT_OK) {
            error_flag = 1; break;
        }

        if (decoder->filter_flags[ch] & DANA_CH_FLAG_PRE_EMPHASIS) {
            if (DANAEmphasisFilter_DeEmphasisInt32(decoder->emp[ch], decoder->output[ch], num_decode_saples, DANA_PRE_EMPHASIS_COEFFICIENT_SHIFT) != DANAPREDICTOR_APIRESULT_OK) {
                error_flag = 1; break;
            }
        }
    }
    if (error_flag) return DANA_APIRESULT_FAILED_TO_SYNTHESIZE;

    switch (decoder->current_ch_mode) {
        case DANA_CHPROCESSMETHOD_STEREO_MS: DANAUtility_MStoLRInt32(decoder->output, decoder->wave_format.num_channels, num_decode_saples); break;
        case DANA_CHPROCESSMETHOD_STEREO_LS: DANAUtility_LStoLRInt32(decoder->output, decoder->wave_format.num_channels, num_decode_saples); break;
        case DANA_CHPROCESSMETHOD_STEREO_ADAPTIVE:
        {
            int32_t * restrict out0 = decoder->output[0];
            int32_t * restrict out1 = decoder->output[1];
            int32_t alpha = decoder->current_alpha_q;
            #pragma GCC ivdep
            for (uint32_t smpl = 0; smpl < num_decode_saples; smpl++) {
                int32_t left = out0[smpl];
                int32_t right = out1[smpl];
                out1[smpl] = right + (int32_t)(((int64_t)left * alpha) >> 7);
            }
            break;
        }
        default: break;
    }

    DANA_Assert(decoder->wave_format.bit_per_sample > decoder->wave_format.offset_lshift);
    DANA_Assert((decoder->wave_format.bit_per_sample - decoder->wave_format.offset_lshift) < 32);
    
    for (uint32_t ch = 0; ch < num_channels; ch++) {
        int32_t * restrict buf_ch = buffer[ch];
        int32_t * restrict out_ch = decoder->output[ch];
        uint32_t shift = (32 - decoder->wave_format.bit_per_sample + decoder->wave_format.offset_lshift);
        #pragma GCC ivdep
        for (uint32_t smpl = 0; smpl < num_decode_saples; smpl++) {
            buf_ch[smpl] = out_ch[smpl] << shift;
        }
    }

    DANABitStream_Tell(&decoder->strm, &end_data_offset);
    DANA_Assert(end_data_offset >= start_data_offset);
    *output_data_size = (uint32_t)(end_data_offset - start_data_offset);

    return DANA_APIRESULT_OK;
}

static void DANADecoder_ResetAllSynthesizer(struct DANADecoder* decoder) {
    DANA_Assert(decoder != NULL);
    for (uint32_t ch = 0; ch < decoder->wave_format.num_channels; ch++) {
        (void)DANAEmphasisFilter_Reset(decoder->emp[ch]);
        (void)DANALPCSynthesizer_Reset(decoder->lpcs[ch]);
        (void)DANALongTermSynthesizer_Reset(decoder->ltms[ch]);
        for (int stage = 0; stage < DANA_MAX_LMS_STAGES; stage++) {
            (void)DANALMSFilter_Reset(decoder->nlmsc[ch][stage]);
        }
    }
}

static DANAApiResult DANADecoder_DecodeBlock(struct DANADecoder* decoder, const uint8_t* restrict data, uint32_t data_size, int32_t** restrict buffer, uint32_t buffer_num_samples, uint32_t* restrict output_block_size, uint32_t* restrict output_num_samples) {
    if (decoder == NULL || data == NULL || buffer == NULL || output_block_size == NULL || output_num_samples == NULL) return DANA_APIRESULT_INVALID_ARGUMENT;
    if ((!(decoder->status_flag & DANADECODER_STATUS_FLAG_SET_WAVE_FORMAT)) || (!(decoder->status_flag & DANADECODER_STATUS_FLAG_SET_ENCODE_PARAMETER))) return DANA_APIRESULT_PARAMETER_NOT_SET;

    switch (decoder->encode_param.ch_process_method) {
        case DANA_CHPROCESSMETHOD_STEREO_MS:
        case DANA_CHPROCESSMETHOD_STEREO_ADAPTIVE:
            if (decoder->wave_format.num_channels != 2) return DANA_APIRESULT_INVAILD_CHPROCESSMETHOD;
            break;
        default: break;
    }

    DANABitReader_Open(&decoder->strm, (uint8_t *)data, data_size);
    DANABitStream_Seek(&decoder->strm, 0, DANABITSTREAM_SEEK_SET);

    struct DANABlockHeaderInfo block_header;
    uint32_t block_header_size;
    DANAApiResult ret;
    
    if ((ret = DANADecoder_DecodeBlockHeader(decoder, data, data_size, &block_header, &block_header_size)) != DANA_APIRESULT_OK) return ret;
    if (block_header.block_size > data_size) return DANA_APIRESULT_INSUFFICIENT_DATA_SIZE;
    if (block_header.block_num_samples > buffer_num_samples) return DANA_APIRESULT_INSUFFICIENT_BUFFER_SIZE;

    DANADecoder_ResetAllSynthesizer(decoder);

    uint32_t tmp_data_size;
    if ((ret = DANADecoder_DecodeWaveData(decoder, buffer, block_header.block_num_samples, &tmp_data_size)) != DANA_APIRESULT_OK) return ret;

    *output_num_samples = block_header.block_num_samples;
    *output_block_size = block_header.block_size;

    DANABitStream_Close(&decoder->strm);
    return DANA_APIRESULT_OK;
}

DANAApiResult DANADecoder_DecodeWhole(struct DANADecoder* restrict decoder, const uint8_t* restrict data, uint32_t data_size, int32_t** restrict buffer, uint32_t buffer_num_samples, uint32_t* restrict output_num_samples) {
    if (decoder == NULL || buffer == NULL || data == NULL || output_num_samples == NULL) return DANA_APIRESULT_INVALID_ARGUMENT;

    struct DANAHeaderInfo header;
    uint32_t header_size_out;
    DANAApiResult api_ret;
    
    if ((api_ret = DANADecoder_DecodeHeader(data, data_size, &header, &header_size_out)) != DANA_APIRESULT_OK) return api_ret;
    if ((api_ret = DANADecoder_SetWaveFormat(decoder, &header.wave_format)) != DANA_APIRESULT_OK) return api_ret;
    if ((api_ret = DANADecoder_SetEncodeParameter(decoder, &header.encode_param)) != DANA_APIRESULT_OK) return api_ret;

    uint32_t decode_offset_byte = header_size_out;
    uint32_t decode_offset_sample = 0;
    
    while (decode_offset_sample < header.num_samples) {
        if (decode_offset_byte > data_size) return DANA_APIRESULT_INSUFFICIENT_DATA_SIZE;

        int32_t* output_ptr[DANA_MAX_CHANNELS];
        for (uint32_t ch = 0; ch < decoder->wave_format.num_channels; ch++) {
            output_ptr[ch] = &buffer[ch][decode_offset_sample];
        }

        uint32_t block_size, block_num_samples;
        if ((api_ret = DANADecoder_DecodeBlock(decoder, &data[decode_offset_byte], data_size - decode_offset_byte, output_ptr, buffer_num_samples - decode_offset_sample, &block_size, &block_num_samples)) != DANA_APIRESULT_OK) {
            return api_ret;
        }

        decode_offset_byte += block_size;
        decode_offset_sample += block_num_samples;

        if (decoder->verpose_flag != 0) {
            printf("progress:%2u%% \r", (unsigned int)((100 * decode_offset_byte) / data_size));
            fflush(stdout);
        }
    }

    *output_num_samples = decode_offset_sample;
    DANAMetadata_Release(&header.metadata);
    return DANA_APIRESULT_OK;
}

static DANAApiResult DANAStreamingDecoder_Reset(struct DANAStreamingDecoder* decoder) {
    if (decoder == NULL) return DANA_APIRESULT_INVALID_ARGUMENT;
    decoder->current_block_sample_offset = 0;
    memset(decoder->data_buffer, 0, sizeof(uint8_t) * decoder->data_buffer_size);
    decoder->data_buffer_provided_size = 0;
    decoder->pcm_cache_num_samples = 0;
    return DANA_APIRESULT_OK;
}

struct DANAStreamingDecoder* DANAStreamingDecoder_Create(const struct DANAStreamingDecoderConfig* config) {
    if (config == NULL || config->decode_interval_hz <= 0.0f) return NULL;

    struct DANAStreamingDecoder* decoder = malloc(sizeof(struct DANAStreamingDecoder));
    decoder->decoder_core = DANADecoder_Create(&config->core_config);
    if (decoder->decoder_core == NULL) { free(decoder); return NULL; }

    decoder->queue = DANADataPacketQueue_Create(DANA_STREAMING_DECODE_MAX_NUM_PACKETS);
    if (decoder->queue == NULL) { free(decoder->decoder_core); free(decoder); return NULL; }

    decoder->decode_interval_hz = config->decode_interval_hz;
    decoder->max_bit_per_sample = config->max_bit_per_sample;
    decoder->data_buffer_size = 2 * DANA_CalculateSufficientBlockSize(config->core_config.max_num_channels, config->core_config.max_num_block_samples, config->max_bit_per_sample);
    decoder->data_buffer = malloc(sizeof(uint8_t) * decoder->data_buffer_size);
    decoder->estimated_bytes_per_sample = (float)((double)config->core_config.max_num_channels * (config->max_bit_per_sample / 8.0));
    
    decoder->pcm_cache = malloc(sizeof(int32_t*) * config->core_config.max_num_channels);
    for (uint32_t ch = 0; ch < config->core_config.max_num_channels; ch++) {
        decoder->pcm_cache[ch] = malloc(sizeof(int32_t) * config->core_config.max_num_block_samples);
    }

    if (DANAStreamingDecoder_Reset(decoder) != DANA_APIRESULT_OK) {
        free(decoder->decoder_core); free(decoder->data_buffer); free(decoder->queue);
        if (decoder->pcm_cache) {
            for (uint32_t ch = 0; ch < config->core_config.max_num_channels; ch++) {
                free(decoder->pcm_cache[ch]);
            }
            free(decoder->pcm_cache);
        }
        free(decoder); return NULL;
    }
    return decoder;
}

void DANAStreamingDecoder_Destroy(struct DANAStreamingDecoder* decoder) {
    if (decoder != NULL) {
        DANADecoder_Destroy(decoder->decoder_core);
        DANADataPacketQueue_Destroy(decoder->queue);
        NULLCHECK_AND_FREE(decoder->data_buffer);
        if (decoder->pcm_cache) {
            for (uint32_t ch = 0; ch < decoder->decoder_core->max_num_channels; ch++) {
                free(decoder->pcm_cache[ch]);
            }
            free(decoder->pcm_cache);
        }
        free(decoder);
    }
}

DANAApiResult DANAStreamingDecoder_SetWaveFormat(struct DANAStreamingDecoder* decoder, const struct DANAWaveFormat* wave_format) {
    if (decoder == NULL) return DANA_APIRESULT_INVALID_ARGUMENT;
    DANAApiResult ret;
    if ((ret = DANADecoder_SetWaveFormat(decoder->decoder_core, wave_format)) != DANA_APIRESULT_OK) return ret;
    if (wave_format->bit_per_sample > decoder->max_bit_per_sample) return DANA_APIRESULT_EXCEED_HANDLE_CAPACITY;

    decoder->num_output_samples_per_decode = (uint32_t)ceil(DANA_STREAMING_DECODE_NUM_SAMPLES_MARGIN * (float)wave_format->sampling_rate / decoder->decode_interval_hz);
    return DANA_APIRESULT_OK;
}

DANAApiResult DANAStreamingDecoder_SetEncodeParameter(struct DANAStreamingDecoder* decoder, const struct DANAEncodeParameter* encode_param) {
    if (decoder == NULL) return DANA_APIRESULT_INVALID_ARGUMENT;
    return DANADecoder_SetEncodeParameter(decoder->decoder_core, encode_param);
}

DANAApiResult DANAStreamingDecoder_EstimateMinimumNessesaryDataSize(struct DANAStreamingDecoder* decoder, uint32_t* estimate_data_size) {
    if ((decoder == NULL) || (estimate_data_size == NULL)) return DANA_APIRESULT_INVALID_ARGUMENT;
    uint32_t tmp_estimate = (uint32_t)ceil((double)decoder->estimated_bytes_per_sample * decoder->num_output_samples_per_decode);
    *estimate_data_size = DANAUTILITY_MAX(tmp_estimate, DANA_MINIMUM_BLOCK_HEADER_SIZE);
    return DANA_APIRESULT_OK;
}

DANAApiResult DANAStreamingDecoder_EstimateDecodableNumSamples(struct DANAStreamingDecoder* decoder, uint32_t* estimate_num_samples) {
    if ((decoder == NULL) || (estimate_num_samples == NULL)) return DANA_APIRESULT_INVALID_ARGUMENT;
    uint32_t remain_data_size;
    DANAApiResult api_ret;
    if ((api_ret = DANAStreamingDecoder_GetRemainDataSize(decoder, &remain_data_size)) != DANA_APIRESULT_OK) return api_ret;
    *estimate_num_samples = (uint32_t)floor((float)remain_data_size / decoder->estimated_bytes_per_sample);
    return DANA_APIRESULT_OK;
}

DANAApiResult DANAStreamingDecoder_GetOutputNumSamplesPerDecode(struct DANAStreamingDecoder* decoder, uint32_t* output_num_samples) {
    if ((decoder == NULL) || (output_num_samples == NULL)) return DANA_APIRESULT_INVALID_ARGUMENT;
    *output_num_samples = decoder->num_output_samples_per_decode;
    return DANA_APIRESULT_OK;
}

DANAApiResult DANAStreamingDecoder_GetRemainDataSize(struct DANAStreamingDecoder* decoder, uint32_t* remain_data_size) {
    if ((decoder == NULL) || (remain_data_size == NULL)) return DANA_APIRESULT_INVALID_ARGUMENT;
    
    uint32_t queue_remain = DANADataPacketQueue_GetRemainDataSize(decoder->queue);
    uint32_t data_buffer_remain = decoder->data_buffer_provided_size;
    
    if (decoder->current_block_sample_offset > 0) {
        int32_t decoded_size;
        DANABitStream_Tell(&decoder->decoder_core->strm, &decoded_size);
        DANA_Assert(decoder->data_buffer_provided_size >= (uint32_t)decoded_size);
        data_buffer_remain -= (uint32_t)decoded_size;
    }

    *remain_data_size = queue_remain + data_buffer_remain;
    return DANA_APIRESULT_OK;
}

DANAApiResult DANAStreamingDecoder_AppendDataFragment(struct DANAStreamingDecoder* restrict decoder, const uint8_t* restrict data, uint32_t data_size) {
    if ((decoder == NULL) || (data == NULL)) return DANA_APIRESULT_INVALID_ARGUMENT;
    DANAApiResult ret = DANA_APIRESULT_OK;

    if (data_size > 0) {
        if (DANADataPacketQueue_EnqueueDataFragment(decoder->queue, data, data_size) != DANA_DATAPACKETQUEUE_APIRESULT_OK) {
            ret = DANA_APIRESULT_EXCEED_HANDLE_CAPACITY;
        }
    }

    DANA_Assert(decoder->data_buffer_size >= decoder->data_buffer_provided_size);
    const uint8_t* append_data;
    uint32_t append_data_size;
    
    while (DANADataPacketQueue_GetDataFragment(decoder->queue, &append_data, &append_data_size, decoder->data_buffer_size - decoder->data_buffer_provided_size) != DANA_DATAPACKETQUEUE_APIRESULT_NO_DATA_FRAGMENTS) {
        memcpy(&decoder->data_buffer[decoder->data_buffer_provided_size], append_data, append_data_size);
        decoder->data_buffer_provided_size += append_data_size;
        DANA_Assert(decoder->data_buffer_size >= decoder->data_buffer_provided_size);
    }
    return ret;
}

DANAApiResult DANAStreamingDecoder_CollectDataFragment(struct DANAStreamingDecoder* restrict decoder, const uint8_t** restrict data_ptr, uint32_t* restrict data_size) {
    if ((decoder == NULL) || (data_ptr == NULL) || (data_size == NULL)) return DANA_APIRESULT_INVALID_ARGUMENT;
    if (DANADataPacketQueue_DequeueDataFragment(decoder->queue, data_ptr, data_size) != DANA_DATAPACKETQUEUE_APIRESULT_OK) {
        return DANA_APIRESULT_NO_DATA_FRAGMENTS;
    }
    return DANA_APIRESULT_OK;
}

static DANAApiResult DANAStreamingDecoder_DecodeCore(struct DANAStreamingDecoder* decoder, int32_t** buffer, uint32_t buffer_num_samples, uint32_t* num_output_samples) {
    DANA_Assert((decoder != NULL) && (buffer != NULL) && (num_output_samples != NULL) && (buffer_num_samples > 0));

    uint32_t goal_num_samples = DANAUTILITY_MIN(buffer_num_samples, decoder->num_output_samples_per_decode);
    uint32_t sample_progress = 0;
    DANAApiResult ret;

    while (sample_progress < goal_num_samples) {
        if (decoder->current_block_sample_offset == 0) {
            if (decoder->data_buffer_provided_size < 5) {
                if (sample_progress > 0) break;
                return DANA_APIRESULT_INSUFFICIENT_DATA_SIZE;
            }

            uint16_t sync_code = (((uint16_t)decoder->data_buffer[0] << 8) | decoder->data_buffer[1]);
            if (sync_code != DANA_BLOCK_SYNC_CODE) return DANA_APIRESULT_FAILED_TO_FIND_SYNC_CODE;

            uint32_t block_size = (((uint32_t)decoder->data_buffer[2] << 16) | ((uint32_t)decoder->data_buffer[3] << 8) | ((uint32_t)decoder->data_buffer[4])) + 5;
            if (block_size > decoder->data_buffer_size) return DANA_APIRESULT_DETECT_DATA_CORRUPTION;
            if (decoder->data_buffer_provided_size < block_size) {
                if (sample_progress > 0) break;
                return DANA_APIRESULT_INSUFFICIENT_DATA_SIZE;
            }

            DANABitReader_Open(&decoder->decoder_core->strm, decoder->data_buffer, decoder->data_buffer_size);
            
            uint32_t block_header_size;
            if ((ret = DANADecoder_DecodeBlockHeader(decoder->decoder_core, decoder->data_buffer, decoder->data_buffer_provided_size, &decoder->current_block_header, &block_header_size)) != DANA_APIRESULT_OK) {
                DANABitStream_Close(&decoder->decoder_core->strm);
                return ret;
            }
            
            decoder->estimated_bytes_per_sample = (float)((double)decoder->current_block_header.block_size / decoder->current_block_header.block_num_samples);
            DANADecoder_ResetAllSynthesizer(decoder->decoder_core);
            
            uint32_t output_wavedata_size;
            if ((ret = DANADecoder_DecodeWaveData(decoder->decoder_core, decoder->pcm_cache, decoder->current_block_header.block_num_samples, &output_wavedata_size)) != DANA_APIRESULT_OK) {
                DANABitStream_Close(&decoder->decoder_core->strm);
                return ret;
            }
            decoder->pcm_cache_num_samples = decoder->current_block_header.block_num_samples;
            
            DANA_Assert(decoder->data_buffer_provided_size >= decoder->current_block_header.block_size);
            
            memmove(decoder->data_buffer, &decoder->data_buffer[decoder->current_block_header.block_size], decoder->data_buffer_provided_size - decoder->current_block_header.block_size);
            decoder->data_buffer_provided_size -= decoder->current_block_header.block_size;
            
            DANABitStream_Close(&decoder->decoder_core->strm);
        }

        uint32_t num_copy_samples = DANAUTILITY_MIN(goal_num_samples - sample_progress, decoder->pcm_cache_num_samples - decoder->current_block_sample_offset);
        if (buffer_num_samples < (sample_progress + num_copy_samples)) return DANA_APIRESULT_INSUFFICIENT_BUFFER_SIZE;

        for (uint32_t ch = 0; ch < decoder->decoder_core->wave_format.num_channels; ch++) {
            memcpy(&buffer[ch][sample_progress], &decoder->pcm_cache[ch][decoder->current_block_sample_offset], sizeof(int32_t) * num_copy_samples);
        }

        sample_progress += num_copy_samples;
        decoder->current_block_sample_offset += num_copy_samples;

        if (decoder->current_block_sample_offset >= decoder->pcm_cache_num_samples) {
            decoder->current_block_sample_offset = 0;
        }
    }

    *num_output_samples = sample_progress;
    return DANA_APIRESULT_OK;
}

DANAApiResult DANAStreamingDecoder_Decode(struct DANAStreamingDecoder* restrict decoder, int32_t** restrict buffer, uint32_t buffer_num_samples, uint32_t* restrict num_output_samples) {
    if ((decoder == NULL) || (buffer == NULL)) return DANA_APIRESULT_INVALID_ARGUMENT;

    uint32_t tmp_num_output_samples;
    DANAApiResult ret;
    if ((ret = DANAStreamingDecoder_DecodeCore(decoder, buffer, buffer_num_samples, &tmp_num_output_samples)) != DANA_APIRESULT_OK) {
        return ret;
    }

    *num_output_samples = tmp_num_output_samples;
    return DANA_APIRESULT_OK;
}