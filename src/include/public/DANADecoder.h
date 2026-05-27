#ifndef DANA_DECODER_H_INCLUDED
#define DANA_DECODER_H_INCLUDED

#include "DANA.h"

#define DANA_DECODER_VERSION_STRING "0.0.1(beta)"

struct DANADecoder;
struct DANAStreamingDecoder;

struct DANADecoderConfig {
    uint32_t max_num_channels;
    uint32_t max_num_block_samples;
    uint32_t max_parcor_order;
    uint32_t max_longterm_order;
    uint32_t max_lms_order_per_filter;
    uint8_t  enable_crc_check;
    uint8_t  verpose_flag;
};

struct DANAStreamingDecoderConfig {
    struct DANADecoderConfig core_config;
    float                    decode_interval_hz;
    uint32_t                 max_bit_per_sample;
};

#ifdef __cplusplus
extern "C" {
#endif

DANAApiResult DANADecoder_DecodeHeader(const uint8_t* restrict data, uint32_t data_size, struct DANAHeaderInfo* restrict header_info, uint32_t* restrict header_size_out);
DANAApiResult DANAMetadata_Copy(struct DANAMetadata* restrict dst, const struct DANAMetadata* restrict src);

struct DANADecoder* DANADecoder_Create(const struct DANADecoderConfig* config);
void DANADecoder_Destroy(struct DANADecoder* decoder);

DANAApiResult DANADecoder_SetWaveFormat(struct DANADecoder* decoder, const struct DANAWaveFormat* wave_format);
DANAApiResult DANADecoder_SetEncodeParameter(struct DANADecoder* decoder, const struct DANAEncodeParameter* encode_param);

DANAApiResult DANADecoder_DecodeWhole(struct DANADecoder* restrict decoder, const uint8_t* restrict data, uint32_t data_size, int32_t** restrict buffer, uint32_t buffer_num_samples, uint32_t* restrict output_num_samples);

DANAApiResult DANADecoder_GetSeekPoint(const struct DANAMetadata* meta, uint32_t target_sample, uint32_t* out_sample, uint32_t* out_byte_offset);

struct DANAStreamingDecoder* DANAStreamingDecoder_Create(const struct DANAStreamingDecoderConfig* config);
void DANAStreamingDecoder_Destroy(struct DANAStreamingDecoder* decoder);

DANAApiResult DANAStreamingDecoder_SetWaveFormat(struct DANAStreamingDecoder* decoder, const struct DANAWaveFormat* wave_format);
DANAApiResult DANAStreamingDecoder_SetEncodeParameter(struct DANAStreamingDecoder* decoder, const struct DANAEncodeParameter* encode_param);

DANAApiResult DANAStreamingDecoder_EstimateMinimumNessesaryDataSize(struct DANAStreamingDecoder* decoder, uint32_t* estimate_data_size);
DANAApiResult DANAStreamingDecoder_EstimateDecodableNumSamples(struct DANAStreamingDecoder* decoder, uint32_t* estimate_num_samples);
DANAApiResult DANAStreamingDecoder_GetOutputNumSamplesPerDecode(struct DANAStreamingDecoder* decoder, uint32_t* output_num_samples);

DANAApiResult DANAStreamingDecoder_AppendDataFragment(struct DANAStreamingDecoder* restrict decoder, const uint8_t* restrict data, uint32_t data_size);
DANAApiResult DANAStreamingDecoder_CollectDataFragment(struct DANAStreamingDecoder* restrict decoder, const uint8_t** restrict data_ptr, uint32_t* restrict data_size);
DANAApiResult DANAStreamingDecoder_GetRemainDataSize(struct DANAStreamingDecoder* decoder, uint32_t* remain_data_size);

DANAApiResult DANAStreamingDecoder_Decode(struct DANAStreamingDecoder* restrict decoder, int32_t** restrict buffer, uint32_t buffer_num_samples, uint32_t* restrict num_output_samples);

#ifdef __cplusplus
}
#endif

#endif /* DANA_DECODER_H_INCLUDED */