#ifndef DANACODER_H_INCLUDED
#define DANACODER_H_INCLUDED

#include "DANABitStream.h"
#include "DANAStdint.h"

struct DANACoder;

#ifdef __cplusplus
extern "C" {
#endif

struct DANACoder* DANACoder_Create(uint32_t max_num_channels, uint32_t max_num_parameters);
void DANACoder_Destroy(struct DANACoder* coder);

void DANACoder_CalculateInitialRecursiveRiceParameter(struct DANACoder* restrict coder, uint32_t num_parameters, const int32_t** restrict data, uint32_t num_channels, uint32_t num_samples);
void DANACoder_PutInitialRecursiveRiceParameter(struct DANACoder* restrict coder, struct DANABitStream* restrict strm, uint32_t num_parameters, uint32_t bitwidth, uint32_t channel_index);
void DANACoder_GetInitialRecursiveRiceParameter(struct DANACoder* restrict coder, struct DANABitStream* restrict strm, uint32_t num_parameters, uint32_t bitwidth, uint32_t channel_index);

void DANACoder_PutDataArray(struct DANACoder* restrict coder, struct DANABitStream* restrict strm, uint32_t num_parameters, const int32_t** restrict data, uint32_t num_channels, uint32_t num_samples);
void DANACoder_GetDataArray(struct DANACoder* restrict coder, struct DANABitStream* restrict strm, uint32_t num_parameters, int32_t** restrict data, uint32_t num_channels, uint32_t num_samples);

#ifdef __cplusplus
}
#endif

#endif /* DANACODER_H_INCLUDED */