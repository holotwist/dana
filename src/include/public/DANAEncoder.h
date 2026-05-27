#ifndef DANA_ENCODER_H_INCLUDED
#define DANA_ENCODER_H_INCLUDED

#include "DANA.h"

#define DANA_ENCODER_VERSION_STRING "0.0.1(beta)"

struct DANAEncoder;

struct DANAEncoderConfig {
    uint32_t max_num_channels;
    uint32_t max_num_block_samples;
    uint32_t max_parcor_order;
    uint32_t max_longterm_order;
    uint32_t max_lms_order_per_filter;
    uint8_t  verpose_flag;
    uint8_t  enable_seek_table;
};

#ifdef __cplusplus
extern "C" {
#endif

struct DANAEncoder* DANAEncoder_Create(const struct DANAEncoderConfig* config);
void DANAEncoder_Destroy(struct DANAEncoder* encoder);

DANAApiResult DANAEncoder_SetWaveFormat(struct DANAEncoder* encoder, const struct DANAWaveFormat* wave_format);
DANAApiResult DANAEncoder_SetEncodeParameter(struct DANAEncoder* encoder, const struct DANAEncodeParameter* encode_param);
DANAApiResult DANAEncoder_SetMetadata(struct DANAEncoder* encoder, const struct DANAMetadata* metadata);

DANAApiResult DANAEncoder_EncodeHeader(const struct DANAHeaderInfo* restrict header, uint8_t* restrict data, uint32_t data_size, uint32_t* restrict header_size_out);
DANAApiResult DANAEncoder_EncodeBlock(struct DANAEncoder* restrict encoder, const int32_t* const* restrict input, uint32_t num_samples, uint8_t* restrict data, uint32_t data_size, uint32_t* restrict output_size);
DANAApiResult DANAEncoder_EncodeWhole(struct DANAEncoder* restrict encoder, const int32_t* const* restrict input, uint32_t num_samples, uint8_t* restrict data, uint32_t data_size, uint32_t* restrict output_size);

#ifdef __cplusplus
}
#endif

#endif /* DANA_ENCODER_H_INCLUDED */