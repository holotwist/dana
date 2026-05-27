#ifndef DANAPREDICTOR_H_INCLUDED
#define DANAPREDICTOR_H_INCLUDED

#include "DANAStdint.h"

struct DANALPCCalculator;
struct DANALPCSynthesizer;
struct DANALongTermCalculator;
struct DANALongTermSynthesizer;
struct DANALMSFilter;
struct DANAOptimalBlockPartitionEstimator;
struct DANAEmphasisFilter;

typedef enum {
    DANAPREDICTOR_APIRESULT_OK,
    DANAPREDICTOR_APIRESULT_NG,
    DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT,
    DANAPREDICTOR_APIRESULT_EXCEED_MAX_ORDER,
    DANAPREDICTOR_APIRESULT_FAILED_TO_CALCULATION
} DANAPredictorApiResult;

#ifdef __cplusplus
extern "C" {
#endif

struct DANALPCCalculator* DANALPCCalculator_Create(uint32_t max_order, uint32_t max_num_samples);
void DANALPCCalculator_Destroy(struct DANALPCCalculator* lpc);
DANAPredictorApiResult DANALPCCalculator_CalculatePARCORCoefDouble(struct DANALPCCalculator* restrict lpcc, const double* restrict data, uint32_t num_samples, double* restrict parcor_coef, uint32_t order);
DANAPredictorApiResult DANALPCCalculator_EstimateCodeLength(const double* restrict data, uint32_t num_samples, uint32_t bits_per_sample, const double* restrict parcor_coef, uint32_t order, double* restrict length_per_sample);
DANAPredictorApiResult DANALPCCalculator_CalculateResidualPower(const double* restrict data, uint32_t num_samples, const double* restrict parcor_coef, uint32_t order, double* restrict residual_power);

struct DANALPCSynthesizer* DANALPCSynthesizer_Create(uint32_t max_order);
void DANALPCSynthesizer_Destroy(struct DANALPCSynthesizer* lpc);
DANAPredictorApiResult DANALPCSynthesizer_Reset(struct DANALPCSynthesizer* lpc);
DANAPredictorApiResult DANALPCSynthesizer_PredictByParcorCoefInt32(struct DANALPCSynthesizer* restrict lpcs, const int32_t* restrict data, uint32_t num_samples, const int32_t* restrict parcor_coef, uint32_t order, int32_t* restrict residual);
DANAPredictorApiResult DANALPCSynthesizer_SynthesizeByParcorCoefInt32(struct DANALPCSynthesizer* restrict lpcs, const int32_t* restrict residual, uint32_t num_samples, const int32_t* restrict parcor_coef, uint32_t order, int32_t* restrict output);

struct DANALongTermCalculator* DANALongTermCalculator_Create(uint32_t fft_size, uint32_t max_pitch_period, uint32_t max_num_pitch_candidates, uint32_t max_num_taps);
void DANALongTermCalculator_Destroy(struct DANALongTermCalculator* ltm_calculator);
DANAPredictorApiResult DANALongTermCalculator_CalculateCoef(struct DANALongTermCalculator* restrict ltm_calculator, const int32_t* restrict data, uint32_t num_samples, uint32_t* restrict pitch_num_samples, double* restrict ltm_coef, uint32_t num_taps);

struct DANALongTermSynthesizer* DANALongTermSynthesizer_Create(uint32_t max_num_taps, uint32_t max_pitch_period);
void DANALongTermSynthesizer_Destroy(struct DANALongTermSynthesizer* ltm);
DANAPredictorApiResult DANALongTermSynthesizer_Reset(struct DANALongTermSynthesizer* ltm);
DANAPredictorApiResult DANALongTermSynthesizer_PredictInt32(struct DANALongTermSynthesizer* restrict ltm, const int32_t* restrict data, uint32_t num_samples, uint32_t pitch_period, const int32_t* restrict ltm_coef, uint32_t num_taps, int32_t* restrict residual);
DANAPredictorApiResult DANALongTermSynthesizer_SynthesizeInt32(struct DANALongTermSynthesizer* restrict ltm, const int32_t* restrict residual, uint32_t num_samples, uint32_t pitch_period, const int32_t* restrict ltm_coef, uint32_t num_taps, int32_t* restrict output);

struct DANALMSFilter* DANALMSFilter_Create(uint32_t max_num_coef);
void DANALMSFilter_Destroy(struct DANALMSFilter* nlms);
DANAPredictorApiResult DANALMSFilter_Reset(struct DANALMSFilter* nlms);
DANAPredictorApiResult DANALMSFilter_PredictInt32(struct DANALMSFilter* restrict nlms, uint32_t num_coef, const int32_t* restrict data, uint32_t num_samples, int32_t* restrict residual, uint8_t step_idx);
DANAPredictorApiResult DANALMSFilter_SynthesizeInt32(struct DANALMSFilter* restrict nlms, uint32_t num_coef, const int32_t* restrict residual, uint32_t num_samples, int32_t* restrict output, uint8_t step_idx);

struct DANAOptimalBlockPartitionEstimator* DANAOptimalEncodeEstimator_Create(uint32_t max_num_samples, uint32_t delta_num_samples);
void DANAOptimalEncodeEstimator_Destroy(struct DANAOptimalBlockPartitionEstimator* oee);
DANAPredictorApiResult DANAOptimalEncodeEstimator_SearchOptimalBlockPartitions(struct DANAOptimalBlockPartitionEstimator* restrict oee, struct DANALPCCalculator* restrict lpcc, const double* const* restrict data, uint32_t num_channels, uint32_t num_samples, uint32_t min_num_block_samples, uint32_t delta_num_samples, uint32_t max_num_block_samples, uint32_t bits_per_sample, uint32_t parcor_order, uint32_t* restrict optimal_num_partitions, uint32_t* restrict optimal_block_partition);
uint32_t DANAOptimalEncodeEstimator_CalculateMaxNumPartitions(uint32_t max_num_samples, uint32_t delta_num_samples);

struct DANAEmphasisFilter* DANAEmphasisFilter_Create(void);
DANAPredictorApiResult DANAEmphasisFilter_Reset(struct DANAEmphasisFilter* emp);
void DANAEmphasisFilter_Destroy(struct DANAEmphasisFilter* emp);
DANAPredictorApiResult DANAEmphasisFilter_PreEmphasisInt32(struct DANAEmphasisFilter* restrict emp, int32_t* restrict data, uint32_t num_samples, int32_t coef_shift);
DANAPredictorApiResult DANAEmphasisFilter_DeEmphasisInt32(struct DANAEmphasisFilter* restrict emp, int32_t* restrict data, uint32_t num_samples, int32_t coef_shift);
void DANAEmphasisFilter_PreEmphasisDouble(double* restrict data, uint32_t num_samples, int32_t coef_shift);

#ifdef __cplusplus
}
#endif

#endif /* DANAPREDICTOR_H_INCLUDED */