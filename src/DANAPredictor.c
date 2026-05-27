#include "DANAPredictor.h"
#include "DANAUtility.h"
#include "DANAInternal.h"

#include <math.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>
#include <float.h>

#ifdef __AVX2__
#include <immintrin.h>
#endif

#define LPC_LONGTERM_PITCH_RATIO_VS_MAX_THRESHOULD    (1.0)
#define DANAOPTIMALENCODEESTIMATOR_BIG_WEIGHT         (double)(1UL << 24)
#define DANAOPTIMALENCODEESTIMATOR_ESTIMATE_BLOCK_SIZE (50)

#define DANAOPTIMALENCODEESTIMATOR_CALCULATE_NUM_NODES(num_samples, delta_num_samples) \
    ((((num_samples) + ((delta_num_samples) - 1)) / (delta_num_samples)) + 1)

typedef enum {
    DANAPREDICTOR_ERROR_OK,
    DANAPREDICTOR_ERROR_NG,
    DANAPREDICTOR_ERROR_INVALID_ARGUMENT
} DANAPredictorError;

struct DANALPCCalculator {
    uint32_t max_order;
    uint32_t max_num_samples;
    double*  parcor_coef;
    double*  f_err;
    double*  b_err;
};

struct DANALPCSynthesizer {
    uint32_t max_order;
    int32_t* forward_residual;
    int32_t* backward_residual;
};

struct DANALongTermCalculator {
    uint32_t             fft_size;
    uint32_t             max_num_taps;
    double*              auto_corr;
    uint32_t             max_num_pitch_candidates;
    uint32_t             max_pitch_period;
    uint32_t*            pitch_candidate;
    struct DANALESolver* lesolver;
    double**             R_mat;
    double*              ltm_coef_vec;
};

struct DANALongTermSynthesizer {
    uint32_t num_input_samples;
    int32_t* signal_buffer;
    uint32_t signal_buffer_size;
    uint32_t signal_buffer_pos;
};

struct DANALMSFilter {
    int32_t* fir_coef;
    int32_t* iir_coef;
    uint32_t max_num_coef;
    int32_t* fir_sign_buffer;
    int32_t* iir_sign_buffer;
    int32_t* fir_buffer;
    int32_t* iir_buffer;
    uint32_t signal_sign_buffer_size;
    uint32_t buffer_pos;
    uint32_t num_input_samples;
};

struct DANAOptimalBlockPartitionEstimator {
    uint32_t  max_num_nodes;
    double**  adjacency_matrix;
    double*   cost;
    uint32_t* path;
};

struct DANAEmphasisFilter {
    int32_t prev_int32;
};

struct DANALPCCalculator* DANALPCCalculator_Create(uint32_t max_order, uint32_t max_num_samples) {
    struct DANALPCCalculator* lpc = malloc(sizeof(struct DANALPCCalculator));
    lpc->max_order = max_order;
    lpc->max_num_samples = max_num_samples;
    lpc->parcor_coef = malloc(sizeof(double) * (max_order + 1));
    lpc->f_err = malloc(sizeof(double) * max_num_samples);
    lpc->b_err = malloc(sizeof(double) * max_num_samples);
    return lpc;
}

void DANALPCCalculator_Destroy(struct DANALPCCalculator* lpcc) {
    if (lpcc != NULL) {
        NULLCHECK_AND_FREE(lpcc->parcor_coef);
        NULLCHECK_AND_FREE(lpcc->f_err);
        NULLCHECK_AND_FREE(lpcc->b_err);
        free(lpcc);
    }
}

static DANAPredictorError LPC_CalculateCoef(struct DANALPCCalculator* restrict lpc, const double* restrict data, uint32_t num_samples, uint32_t order) {
    if (lpc == NULL) return DANAPREDICTOR_ERROR_INVALID_ARGUMENT;
    if (num_samples > lpc->max_num_samples) return DANAPREDICTOR_ERROR_NG;

    if (num_samples < order) {
        for (uint32_t ord = 0; ord < order + 1; ord++) lpc->parcor_coef[ord] = 0.0;
        return DANAPREDICTOR_ERROR_OK;
    }

    double* f = lpc->f_err;
    double* b = lpc->b_err;
    lpc->parcor_coef[0] = 0.0;
  
    for (uint32_t i = 0; i < num_samples; i++) {
        f[i] = data[i];
        b[i] = data[i];
    }
  
    for (uint32_t m = 1; m <= order; m++) {
        double num = 0.0;
        double den = 0.0;
        uint32_t i = m;

#ifdef __AVX2__
        __m256d v_num = _mm256_setzero_pd();
        __m256d v_den = _mm256_setzero_pd();
        for (; i + 3 < num_samples; i += 4) {
            __m256d v_f = _mm256_loadu_pd(&f[i]);
            __m256d v_b = _mm256_loadu_pd(&b[i - 1]);
            v_num = _mm256_add_pd(v_num, _mm256_mul_pd(v_f, v_b));
            __m256d v_ff = _mm256_mul_pd(v_f, v_f);
            __m256d v_bb = _mm256_mul_pd(v_b, v_b);
            v_den = _mm256_add_pd(v_den, _mm256_add_pd(v_ff, v_bb));
        }
        double num_arr[4], den_arr[4];
        _mm256_storeu_pd(num_arr, v_num);
        _mm256_storeu_pd(den_arr, v_den);
        num += num_arr[0] + num_arr[1] + num_arr[2] + num_arr[3];
        den += den_arr[0] + den_arr[1] + den_arr[2] + den_arr[3];
#endif

        for (; i < num_samples; i++) {
            num += f[i] * b[i - 1];
            den += f[i] * f[i] + b[i - 1] * b[i - 1];
        }
        
        if (den <= FLT_EPSILON) {
            for (uint32_t k = m; k <= order; k++) lpc->parcor_coef[k] = 0.0;
            break;
        }
        
        double gamma = 2.0 * num / den;
        lpc->parcor_coef[m] = gamma;
        
        int32_t i_vec = (int32_t)(num_samples - 1);
#ifdef __AVX2__
        __m256d v_gamma = _mm256_set1_pd(gamma);
        for (; i_vec - 3 >= (int32_t)m; i_vec -= 4) {
            __m256d v_f = _mm256_loadu_pd(&f[i_vec - 3]);
            __m256d v_b_prev = _mm256_loadu_pd(&b[i_vec - 4]);
            __m256d v_new_f = _mm256_sub_pd(v_f, _mm256_mul_pd(v_gamma, v_b_prev));
            __m256d v_new_b = _mm256_sub_pd(v_b_prev, _mm256_mul_pd(v_gamma, v_f));
            _mm256_storeu_pd(&f[i_vec - 3], v_new_f);
            _mm256_storeu_pd(&b[i_vec - 3], v_new_b);
        }
#endif
        for (int32_t j = i_vec; j >= (int32_t)m; j--) {
            double tmp_f = f[j];
            double tmp_b = b[j - 1];
            f[j] = tmp_f - gamma * tmp_b;
            b[j] = tmp_b - gamma * tmp_f;
        }
    }
    return DANAPREDICTOR_ERROR_OK;
}

DANAPredictorApiResult DANALPCCalculator_CalculatePARCORCoefDouble(struct DANALPCCalculator* restrict lpc, const double* restrict data, uint32_t num_samples, double* restrict parcor_coef, uint32_t order) {
    if (lpc == NULL || data == NULL || parcor_coef == NULL) return DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
    if (order > lpc->max_order) return DANAPREDICTOR_APIRESULT_EXCEED_MAX_ORDER;

    if (LPC_CalculateCoef(lpc, data, num_samples, order) != DANAPREDICTOR_ERROR_OK) {
        return DANAPREDICTOR_APIRESULT_FAILED_TO_CALCULATION;
    }

    memmove(parcor_coef, lpc->parcor_coef, sizeof(double) * (order + 1));
    return DANAPREDICTOR_APIRESULT_OK;
}

static DANAPredictorApiResult DANALPCCalculator_CalculateVarianceRatio(const double* parcor_coef, uint32_t order, double* variance_ratio) {
    if (parcor_coef == NULL || variance_ratio == NULL) return DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
    double tmp_var_ratio = 1.0;
    for (uint32_t ord = 1; ord <= order; ord++) {
        tmp_var_ratio *= (1.0 - parcor_coef[ord] * parcor_coef[ord]);
    }
    *variance_ratio = tmp_var_ratio;
    return DANAPREDICTOR_APIRESULT_OK;
}

DANAPredictorApiResult DANALPCCalculator_EstimateCodeLength(const double* restrict data, uint32_t num_samples, uint32_t bits_per_sample, const double* restrict parcor_coef, uint32_t order, double* restrict length_per_sample) {
    if (data == NULL || parcor_coef == NULL || length_per_sample == NULL) return DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT;

    double log2_mean_res_power = 0.0;
    for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
        log2_mean_res_power += data[smpl] * data[smpl];
    }
    
    log2_mean_res_power *= pow(2.0, (double)(2 * (bits_per_sample - 1)));
    if (fabs(log2_mean_res_power) <= FLT_MIN) {
        *length_per_sample = 0.0;
        return DANAPREDICTOR_APIRESULT_OK;
    } 
    log2_mean_res_power = DANAUtility_Log2(log2_mean_res_power) - DANAUtility_Log2((double)num_samples);

    double log2_var_ratio = 0.0;
    for (uint32_t ord = 1; ord <= order; ord++) {
        log2_var_ratio += DANAUtility_Log2(1.0 - parcor_coef[ord] * parcor_coef[ord]);
    }

    *length_per_sample = 1.9426950408889634 + 0.5 * (log2_mean_res_power + log2_var_ratio);
    *length_per_sample /= 8.0;

    if ((*length_per_sample) <= 0.0) {
        *length_per_sample = 1.0 / 8.0;
    }
    return DANAPREDICTOR_APIRESULT_OK;
}

DANAPredictorApiResult DANALPCCalculator_CalculateResidualPower(const double* restrict data, uint32_t num_samples, const double* restrict parcor_coef, uint32_t order, double* restrict residual_power) {
    if (data == NULL || parcor_coef == NULL || residual_power == NULL) return DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT;

    double tmp_res_power = 0.0;
    for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
        tmp_res_power += data[smpl] * data[smpl];
    }
    tmp_res_power /= num_samples;

    double var_ratio;
    DANAPredictorApiResult ret;
    if ((ret = DANALPCCalculator_CalculateVarianceRatio(parcor_coef, order, &var_ratio)) != DANAPREDICTOR_APIRESULT_OK) {
        return ret;
    }

    *residual_power = tmp_res_power * var_ratio;
    return DANAPREDICTOR_APIRESULT_OK;
}

struct DANALPCSynthesizer* DANALPCSynthesizer_Create(uint32_t max_order) {
    struct DANALPCSynthesizer* lpcs = malloc(sizeof(struct DANALPCSynthesizer));
    lpcs->max_order = max_order;
    lpcs->forward_residual = malloc(sizeof(int32_t) * (max_order + 1));
    lpcs->backward_residual = malloc(sizeof(int32_t) * (max_order + 1));

    if (DANALPCSynthesizer_Reset(lpcs) != DANAPREDICTOR_APIRESULT_OK) {
        free(lpcs->forward_residual);
        free(lpcs->backward_residual);
        free(lpcs);
        return NULL;
    }
    return lpcs;
}

void DANALPCSynthesizer_Destroy(struct DANALPCSynthesizer* lpc) {
    if (lpc != NULL) {
        NULLCHECK_AND_FREE(lpc->forward_residual);
        NULLCHECK_AND_FREE(lpc->backward_residual);
        free(lpc);
    }
}

DANAPredictorApiResult DANALPCSynthesizer_Reset(struct DANALPCSynthesizer* lpc) {
    if (lpc == NULL) return DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
    for (uint32_t ord = 0; ord < lpc->max_order + 1; ord++) {
        lpc->forward_residual[ord] = lpc->backward_residual[ord] = 0;
    }
    return DANAPREDICTOR_APIRESULT_OK;
}

DANAPredictorApiResult DANALPCSynthesizer_PredictByParcorCoefInt32(struct DANALPCSynthesizer* restrict lpc, const int32_t* restrict data, uint32_t num_samples, const int32_t* restrict parcor_coef, uint32_t order, int32_t* restrict residual) {
    if (lpc == NULL || data == NULL || parcor_coef == NULL || residual == NULL) return DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
    if (order > lpc->max_order) return DANAPREDICTOR_APIRESULT_EXCEED_MAX_ORDER;

    int32_t* restrict forward_residual = lpc->forward_residual;
    int32_t* restrict backward_residual = lpc->backward_residual;
    const int32_t half = (1UL << 14);

    uint32_t s = 0;
    for (; s < order && s < num_samples; s++) {
        forward_residual[0] = data[s];
        for (uint32_t ord = 1; ord <= s; ord++) {
            int32_t mul_temp = (int32_t)DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(parcor_coef[ord] * backward_residual[ord - 1] + half, 15);
            forward_residual[ord] = forward_residual[ord - 1] - mul_temp;
        }
        for (uint32_t ord = s; ord >= 1; ord--) {
            int32_t mul_temp = (int32_t)DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(parcor_coef[ord] * forward_residual[ord - 1] + half, 15);
            backward_residual[ord] = backward_residual[ord - 1] - mul_temp;
        }
        backward_residual[0] = data[s];
        residual[s] = forward_residual[s];
    }
    
    for (; s < num_samples; s++) {
        forward_residual[0] = data[s];
        #pragma GCC unroll 8
        for (uint32_t ord = 1; ord <= order; ord++) {
            int32_t mul_temp = (int32_t)DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(parcor_coef[ord] * backward_residual[ord - 1] + half, 15);
            forward_residual[ord] = forward_residual[ord - 1] - mul_temp;
        }
        #pragma GCC unroll 8
        for (uint32_t ord = order; ord >= 1; ord--) {
            int32_t mul_temp = (int32_t)DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(parcor_coef[ord] * forward_residual[ord - 1] + half, 15);
            backward_residual[ord] = backward_residual[ord - 1] - mul_temp;
        }
        backward_residual[0] = data[s];
        residual[s] = forward_residual[order];
    }
    return DANAPREDICTOR_APIRESULT_OK;
}

DANAPredictorApiResult DANALPCSynthesizer_SynthesizeByParcorCoefInt32(struct DANALPCSynthesizer* restrict lpc, const int32_t* restrict residual, uint32_t num_samples, const int32_t* restrict parcor_coef, uint32_t order, int32_t* restrict output) {
    if (lpc == NULL || residual == NULL || parcor_coef == NULL || output == NULL) return DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
    if (order > lpc->max_order) return DANAPREDICTOR_APIRESULT_EXCEED_MAX_ORDER;

    int32_t* restrict backward_residual = lpc->backward_residual;
    const int32_t half = (1UL << 14); 

    uint32_t s = 0;
    for (; s < order && s < num_samples; s++) {
        int32_t f_res = residual[s];
        for (uint32_t o = s; o >= 1; o--) {
            int32_t p = parcor_coef[o];
            int32_t bw = backward_residual[o - 1];
            f_res += (int32_t)DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(p * bw + half, 15);
            backward_residual[o] = bw - (int32_t)DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(p * f_res + half, 15);
        }
        output[s] = f_res;
        backward_residual[0] = f_res;
    }
    
    for (; s < num_samples; s++) {
        int32_t f_res = residual[s];
        #pragma GCC unroll 8
        for (uint32_t o = order; o >= 1; o--) {
            int32_t p = parcor_coef[o];
            int32_t bw = backward_residual[o - 1];
            f_res += (int32_t)DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(p * bw + half, 15);
            backward_residual[o] = bw - (int32_t)DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(p * f_res + half, 15);
        }
        output[s] = f_res;
        backward_residual[0] = f_res;
    }
    return DANAPREDICTOR_APIRESULT_OK;
}

struct DANALongTermCalculator* DANALongTermCalculator_Create(uint32_t fft_size, uint32_t max_pitch_period, uint32_t max_num_pitch_candidates, uint32_t max_num_taps) {
    if (fft_size & (fft_size - 1)) return NULL;

    struct DANALongTermCalculator* ltm = malloc(sizeof(struct DANALongTermCalculator));
    ltm->fft_size = fft_size;
    ltm->auto_corr = malloc(sizeof(double) * fft_size);
    ltm->max_num_pitch_candidates = max_num_pitch_candidates;
    ltm->max_pitch_period = max_pitch_period;
    ltm->pitch_candidate = malloc(sizeof(uint32_t) * max_num_pitch_candidates);
    ltm->max_num_taps = max_num_taps;
    ltm->lesolver = DANALESolver_Create(max_num_taps);
    ltm->ltm_coef_vec = malloc(sizeof(double) * max_num_taps);
    ltm->R_mat = malloc(sizeof(double *) * max_num_taps);
    for (uint32_t dim = 0; dim < max_num_taps; dim++) {
        ltm->R_mat[dim] = malloc(sizeof(double) * max_num_taps);
    }
    return ltm;
}

void DANALongTermCalculator_Destroy(struct DANALongTermCalculator* ltm_calculator) {
    if (ltm_calculator != NULL) {
        NULLCHECK_AND_FREE(ltm_calculator->auto_corr);
        NULLCHECK_AND_FREE(ltm_calculator->pitch_candidate);
        DANALESolver_Destroy(ltm_calculator->lesolver);
        NULLCHECK_AND_FREE(ltm_calculator->ltm_coef_vec);
        for (uint32_t dim = 0; dim < ltm_calculator->max_num_taps; dim++) {
            NULLCHECK_AND_FREE(ltm_calculator->R_mat[dim]);
        }
        NULLCHECK_AND_FREE(ltm_calculator->R_mat);
        free(ltm_calculator);
    }
}

DANAPredictorApiResult DANALongTermCalculator_CalculateCoef(struct DANALongTermCalculator* restrict ltm_calculator, const int32_t* restrict data, uint32_t num_samples, uint32_t* restrict pitch_period, double* restrict ltm_coef, uint32_t num_taps) {
    if ((ltm_calculator == NULL) || (data == NULL) || (pitch_period == NULL) || (ltm_coef == NULL)) return DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
    if (!(num_taps & 1)) return DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
    if (num_taps > ltm_calculator->max_num_taps) return DANAPREDICTOR_APIRESULT_EXCEED_MAX_ORDER;
    if (2 * num_samples > ltm_calculator->fft_size) return DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT;

    uint32_t fft_size = ltm_calculator->fft_size;
    double* auto_corr = ltm_calculator->auto_corr;

    for (uint32_t i = 0; i < fft_size; i++) {
        if (i < num_samples) auto_corr[i] = (double)data[i] * pow(2.0, -31.0);
        else auto_corr[i] = 0.0;
    }
    
    DANAUtility_FFT(auto_corr, fft_size, 1);
    auto_corr[0] *= auto_corr[0]; 
    auto_corr[1] *= auto_corr[1];
    
    for (uint32_t i = 1; i < fft_size / 2; i++) {
        double re = auto_corr[2 * i];
        double im = auto_corr[2 * i + 1];
        auto_corr[2 * i] = re * re + im * im;
        auto_corr[2 * i + 1] = 0.0;
    }
    DANAUtility_FFT(auto_corr, fft_size, -1);

    if (fabs(auto_corr[0]) <= FLT_MIN) {
        *pitch_period = 0;
        for (uint32_t i = 0; i < num_taps; i++) ltm_coef[i] = 0.0;
        return DANAPREDICTOR_APIRESULT_OK;
    }

    double max_peak = 0.0;
    uint32_t num_peak = 0;
    uint32_t i = 1;
    
    while ((i < ltm_calculator->max_pitch_period) && (num_peak < ltm_calculator->max_num_pitch_candidates)) {
        uint32_t start, end;
        for (start = i; start < ltm_calculator->max_pitch_period; start++) {
            if ((auto_corr[start - 1] < 0.0) && (auto_corr[start] > 0.0)) break;
        }

        for (end = start + 1; end < ltm_calculator->max_pitch_period; end++) {
            if ((auto_corr[end] > 0.0) && (auto_corr[end + 1] < 0.0)) break;
        }

        uint32_t local_peak_index = 0; 
        double local_peak = 0.0;
        for (uint32_t j = start; j <= end; j++) {
            if ((auto_corr[j] > auto_corr[j - 1]) && (auto_corr[j] > auto_corr[j + 1])) {
                if (auto_corr[j] > local_peak) {
                    local_peak_index = j;
                    local_peak = auto_corr[j];
                }
            }
        }
        
        if (local_peak_index != 0) {
            ltm_calculator->pitch_candidate[num_peak] = local_peak_index;
            num_peak++;
            if (local_peak > max_peak) max_peak = local_peak;
        }
        i = end + 1;
    }

    if (num_peak == 0) return DANAPREDICTOR_APIRESULT_FAILED_TO_CALCULATION;

    for (i = 0; i < num_peak; i++) {
        if (auto_corr[ltm_calculator->pitch_candidate[i]] >= LPC_LONGTERM_PITCH_RATIO_VS_MAX_THRESHOULD * max_peak) break;
    }
    uint32_t tmp_pitch_period = ltm_calculator->pitch_candidate[i];

    if (tmp_pitch_period < ((num_taps / 2) + 1)) return DANAPREDICTOR_APIRESULT_FAILED_TO_CALCULATION;

    for (uint32_t j = 0; j < num_taps; j++) {
        for (uint32_t k = 0; k < num_taps; k++) {
            uint32_t lag = (j >= k) ? (j - k) : (k - j);
            ltm_calculator->R_mat[j][k] = auto_corr[lag];
        }
    }

    for (uint32_t j = 0; j < num_taps; j++) {
        ltm_calculator->ltm_coef_vec[j] = auto_corr[j + tmp_pitch_period - num_taps / 2];
    }

    if (DANALESolver_Solve(ltm_calculator->lesolver, (const double **)ltm_calculator->R_mat, ltm_calculator->ltm_coef_vec, num_taps, 2) != 0) {
        return DANAPREDICTOR_APIRESULT_FAILED_TO_CALCULATION;
    }

    double ltm_coef_sum = 0.0;
    for (uint32_t j = 0; j < num_taps; j++) {
        ltm_coef_sum += fabs(ltm_calculator->ltm_coef_vec[j]);
    }
    
    if (ltm_coef_sum >= 1.0) {
        for (uint32_t j = 0; j < num_taps; j++) ltm_calculator->ltm_coef_vec[j] = 0.0;
        ltm_calculator->ltm_coef_vec[num_taps / 2] = auto_corr[tmp_pitch_period] / auto_corr[0];
    }

    *pitch_period = tmp_pitch_period;
    for (uint32_t j = 0; j < num_taps; j++) {
        ltm_coef[j] = ltm_calculator->ltm_coef_vec[j];
    }
    return DANAPREDICTOR_APIRESULT_OK;
}

struct DANALongTermSynthesizer* DANALongTermSynthesizer_Create(uint32_t max_num_taps, uint32_t max_pitch_period) {
    struct DANALongTermSynthesizer* ltm = malloc(sizeof(struct DANALongTermSynthesizer));
    uint32_t tmp_buffer_size = 2 * (max_num_taps + max_pitch_period);
    ltm->signal_buffer_size = tmp_buffer_size;
    ltm->signal_buffer = malloc(sizeof(int32_t) * tmp_buffer_size);
  
    if (DANALongTermSynthesizer_Reset(ltm) != DANAPREDICTOR_APIRESULT_OK) {
        free(ltm->signal_buffer);
        free(ltm);
        return NULL;
    }
    return ltm;
}

void DANALongTermSynthesizer_Destroy(struct DANALongTermSynthesizer* ltm) {
    if (ltm) {
        NULLCHECK_AND_FREE(ltm->signal_buffer);
        free(ltm);
    }
}

DANAPredictorApiResult DANALongTermSynthesizer_Reset(struct DANALongTermSynthesizer* ltm) {
    if (ltm == NULL) return DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
    ltm->num_input_samples = 0;
    ltm->signal_buffer_pos = 0;
    memset(ltm->signal_buffer, 0, sizeof(int32_t) * ltm->signal_buffer_size);
    return DANAPREDICTOR_APIRESULT_OK;
}

static DANAPredictorApiResult DANALongTermSynthesizer_ProcessCore(struct DANALongTermSynthesizer* restrict ltm, const int32_t* restrict input, uint32_t num_samples, uint32_t pitch_period, const int32_t* restrict ltm_coef, uint32_t num_taps, int32_t* restrict output, uint8_t is_predict) {
    if ((ltm == NULL) || (input == NULL) || (ltm_coef == NULL) || (output == NULL)) return DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
    if (pitch_period == 0) {
        memcpy(output, input, sizeof(int32_t) * num_samples);
        return DANAPREDICTOR_APIRESULT_OK;
    }

    memcpy(output, input, sizeof(int32_t) * num_samples);

    int32_t* signal_buffer = ltm->signal_buffer;
    uint32_t buffer_pos = ltm->signal_buffer_pos;
    const uint32_t max_delay = pitch_period + (num_taps >> 1);
    const int32_t half = (1UL << 30);
    uint32_t smpl = 0;

    if (ltm->num_input_samples < max_delay) {
        uint32_t num_buffering_samples = DANAUTILITY_MIN(max_delay - ltm->num_input_samples, num_samples);
        uint32_t buffer_offset = (max_delay > (num_samples + ltm->num_input_samples)) ? (max_delay - (num_samples + ltm->num_input_samples)) : 0;
        
        for (smpl = 0; smpl < num_buffering_samples; smpl++) {
            signal_buffer[buffer_offset + smpl] = signal_buffer[buffer_offset + smpl + max_delay] = input[num_buffering_samples - smpl - 1];
        }
        buffer_pos += num_buffering_samples;
    }

    for (; smpl < num_samples; smpl++) {
        int64_t predict = half;
        #pragma GCC unroll 8
        for (uint32_t j = 0; j < num_taps; j++) {
            predict += (int64_t)ltm_coef[j] * signal_buffer[buffer_pos + max_delay - 1 - j];
        }
        predict = DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(predict, 31);

        if (is_predict == 1) output[smpl] -= (int32_t)predict;
        else output[smpl] += (int32_t)predict;

        buffer_pos = (buffer_pos == 0) ? (max_delay - 1) : (buffer_pos - 1);
        signal_buffer[buffer_pos] = signal_buffer[buffer_pos + max_delay] = (is_predict == 1) ? input[smpl] : output[smpl];
    }

    ltm->signal_buffer_pos = buffer_pos;
    ltm->num_input_samples += num_samples;
    return DANAPREDICTOR_APIRESULT_OK;
}

DANAPredictorApiResult DANALongTermSynthesizer_PredictInt32(struct DANALongTermSynthesizer* restrict ltm, const int32_t* restrict data, uint32_t num_samples, uint32_t pitch_period, const int32_t* restrict ltm_coef, uint32_t num_taps, int32_t* restrict residual) {
    return DANALongTermSynthesizer_ProcessCore(ltm, data, num_samples, pitch_period, ltm_coef, num_taps, residual, 1);
}
	
DANAPredictorApiResult DANALongTermSynthesizer_SynthesizeInt32(struct DANALongTermSynthesizer* restrict ltm, const int32_t* restrict residual, uint32_t num_samples, uint32_t pitch_period, const int32_t* restrict ltm_coef, uint32_t num_taps, int32_t* restrict output) {
    return DANALongTermSynthesizer_ProcessCore(ltm, residual, num_samples, pitch_period, ltm_coef, num_taps, output, 0);
}

struct DANALMSFilter* DANALMSFilter_Create(uint32_t max_num_coef) {
    struct DANALMSFilter* nlms = malloc(sizeof(struct DANALMSFilter));
    nlms->max_num_coef = max_num_coef;
    nlms->signal_sign_buffer_size = DANAUTILITY_ROUNDUP2POWERED(max_num_coef);

    nlms->fir_coef = malloc(sizeof(int32_t) * max_num_coef);
    nlms->iir_coef = malloc(sizeof(int32_t) * max_num_coef);
    nlms->fir_sign_buffer = malloc(sizeof(int32_t) * 2 * max_num_coef);
    nlms->iir_sign_buffer = malloc(sizeof(int32_t) * 2 * max_num_coef);
    nlms->fir_buffer = malloc(sizeof(int32_t) * 2 * max_num_coef);
    nlms->iir_buffer = malloc(sizeof(int32_t) * 2 * max_num_coef);

    if (DANALMSFilter_Reset(nlms) != DANAPREDICTOR_APIRESULT_OK) {
        free(nlms->fir_coef); free(nlms->iir_coef);
        free(nlms->fir_sign_buffer); free(nlms->iir_sign_buffer);
        free(nlms->fir_buffer); free(nlms->iir_buffer);
        free(nlms);
        return NULL;
    }
    return nlms;
}

void DANALMSFilter_Destroy(struct DANALMSFilter* nlms) {
    if (nlms != NULL) {
        NULLCHECK_AND_FREE(nlms->fir_coef);
        NULLCHECK_AND_FREE(nlms->iir_coef);
        NULLCHECK_AND_FREE(nlms->fir_sign_buffer);
        NULLCHECK_AND_FREE(nlms->iir_sign_buffer);
        NULLCHECK_AND_FREE(nlms->fir_buffer);
        NULLCHECK_AND_FREE(nlms->iir_buffer);
        free(nlms);
    }
}

DANAPredictorApiResult DANALMSFilter_Reset(struct DANALMSFilter* nlms) {
    if (nlms == NULL) return DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
    nlms->num_input_samples = 0;
    nlms->buffer_pos = 0;

    memset(nlms->fir_coef, 0, sizeof(int32_t) * nlms->max_num_coef);
    memset(nlms->iir_coef, 0, sizeof(int32_t) * nlms->max_num_coef);
    memset(nlms->fir_buffer, 0, sizeof(int32_t) * 2 * nlms->max_num_coef);
    memset(nlms->iir_buffer, 0, sizeof(int32_t) * 2 * nlms->max_num_coef);
    memset(nlms->fir_sign_buffer, 0, sizeof(int32_t) * 2 * nlms->max_num_coef);
    memset(nlms->iir_sign_buffer, 0, sizeof(int32_t) * 2 * nlms->max_num_coef);
    return DANAPREDICTOR_APIRESULT_OK;
}

DANAPredictorApiResult DANALMSFilter_PredictInt32(struct DANALMSFilter* restrict nlms, uint32_t num_coef, const int32_t* restrict data, uint32_t num_samples, int32_t* restrict residual, uint8_t step_idx) {
    if ((nlms == NULL) || (data == NULL) || (residual == NULL)) return DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
    if (num_coef > nlms->max_num_coef) return DANAPREDICTOR_APIRESULT_EXCEED_MAX_ORDER;

    const int32_t shift_table[4] = {6, 4, 2, 1}; // Dynamic convergence step index
    int32_t shift = shift_table[step_idx & 3];

    DANA_Assert(num_coef >= 4 && DANAUTILITY_IS_POWERED_OF_2(num_coef));
    memcpy(residual, data, sizeof(int32_t) * num_samples);

    uint32_t buffer_pos = nlms->buffer_pos;
    const uint32_t buffer_pos_mask = (num_coef - 1);
    uint32_t smpl = 0;

    if (nlms->num_input_samples < num_coef) {
        uint32_t num_buffering_samples = DANAUTILITY_MIN(num_coef - nlms->num_input_samples, num_samples);
        uint32_t buffer_offset = (num_coef > (num_samples + nlms->num_input_samples)) ? (num_coef - (num_samples + nlms->num_input_samples)) : 0;
        
        for (smpl = 0; smpl < num_buffering_samples; smpl++) {
            nlms->fir_sign_buffer[buffer_offset + smpl] = nlms->fir_sign_buffer[buffer_offset + smpl + num_coef] =
            nlms->iir_sign_buffer[buffer_offset + smpl] = nlms->iir_sign_buffer[buffer_offset + smpl + num_coef] = DANAUTILITY_SIGN(data[num_buffering_samples - smpl - 1]) + 1;
            nlms->iir_buffer[buffer_offset + smpl] = nlms->iir_buffer[buffer_offset + smpl + num_coef] =
            nlms->fir_buffer[buffer_offset + smpl] = nlms->fir_buffer[buffer_offset + smpl + num_coef] = data[num_buffering_samples - smpl - 1];
        }
        buffer_pos += num_buffering_samples;
    }

    for (; smpl < num_samples; smpl++) {
        int32_t predict = (1 << 9);
        uint32_t i = 0;

#ifdef __AVX2__
        __m256i v_pred = _mm256_setzero_si256();
        for (; i + 7 < num_coef; i += 8) {
            __m256i v_f_c = _mm256_loadu_si256((__m256i const*)&nlms->fir_coef[i]);
            __m256i v_f_b = _mm256_loadu_si256((__m256i const*)&nlms->fir_buffer[buffer_pos + i]);
            __m256i v_i_c = _mm256_loadu_si256((__m256i const*)&nlms->iir_coef[i]);
            __m256i v_i_b = _mm256_loadu_si256((__m256i const*)&nlms->iir_buffer[buffer_pos + i]);
            v_pred = _mm256_add_epi32(v_pred, _mm256_add_epi32(_mm256_mullo_epi32(v_f_c, v_f_b), _mm256_mullo_epi32(v_i_c, v_i_b)));
        }
        __m128i v_pred_low = _mm256_castsi256_si128(v_pred);
        __m128i v_pred_high = _mm256_extracti128_si256(v_pred, 1);
        __m128i v1 = _mm_add_epi32(v_pred_low, v_pred_high);
        __m128i v2 = _mm_add_epi32(v1, _mm_srli_si128(v1, 8)); 
        __m128i v3 = _mm_add_epi32(v2, _mm_srli_si128(v2, 4));
        predict += _mm_cvtsi128_si32(v3);
#endif

        for (; i < num_coef; i++) {
            predict += nlms->fir_coef[i] * nlms->fir_buffer[buffer_pos + i];
            predict += nlms->iir_coef[i] * nlms->iir_buffer[buffer_pos + i];
        }
        predict = DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(predict, 10);

        residual[smpl] -= predict;
        
        int32_t abs_res = DANAUTILITY_ABS(residual[smpl]);
        int32_t log2res = DANAUTILITY_LOG2CEIL(abs_res + 1);
        int32_t signres = DANAUTILITY_SIGN(residual[smpl]);
        int32_t delta = signres * ((log2res << shift) >> 5);

        if (delta != 0) {
#ifdef __AVX2__
            __m256i v_delta = _mm256_set1_epi32(delta);
            __m256i v_ones = _mm256_set1_epi32(1);
            for (i = 0; i + 7 < num_coef; i += 8) {
                __m256i v_f_sb = _mm256_loadu_si256((__m256i const*)&nlms->fir_sign_buffer[buffer_pos + i]);
                __m256i v_i_sb = _mm256_loadu_si256((__m256i const*)&nlms->iir_sign_buffer[buffer_pos + i]);
                
                __m256i v_f_c = _mm256_loadu_si256((__m256i const*)&nlms->fir_coef[i]);
                __m256i v_i_c = _mm256_loadu_si256((__m256i const*)&nlms->iir_coef[i]);
                
                v_f_sb = _mm256_sub_epi32(v_f_sb, v_ones);
                v_i_sb = _mm256_sub_epi32(v_i_sb, v_ones);
                
                v_f_c = _mm256_add_epi32(v_f_c, _mm256_mullo_epi32(v_f_sb, v_delta));
                v_i_c = _mm256_add_epi32(v_i_c, _mm256_mullo_epi32(v_i_sb, v_delta));
                
                _mm256_storeu_si256((__m256i*)&nlms->fir_coef[i], v_f_c);
                _mm256_storeu_si256((__m256i*)&nlms->iir_coef[i], v_i_c);
            }
            for (; i < num_coef; i += 4) {
                nlms->fir_coef[i + 0] += (nlms->fir_sign_buffer[buffer_pos + i + 0] - 1) * delta;
                nlms->fir_coef[i + 1] += (nlms->fir_sign_buffer[buffer_pos + i + 1] - 1) * delta;
                nlms->fir_coef[i + 2] += (nlms->fir_sign_buffer[buffer_pos + i + 2] - 1) * delta;
                nlms->fir_coef[i + 3] += (nlms->fir_sign_buffer[buffer_pos + i + 3] - 1) * delta;
                
                nlms->iir_coef[i + 0] += (nlms->iir_sign_buffer[buffer_pos + i + 0] - 1) * delta;
                nlms->iir_coef[i + 1] += (nlms->iir_sign_buffer[buffer_pos + i + 1] - 1) * delta;
                nlms->iir_coef[i + 2] += (nlms->iir_sign_buffer[buffer_pos + i + 2] - 1) * delta;
                nlms->iir_coef[i + 3] += (nlms->iir_sign_buffer[buffer_pos + i + 3] - 1) * delta;
            }
#else
            for (i = 0; i < num_coef; i += 4) {
                nlms->fir_coef[i + 0] += (nlms->fir_sign_buffer[buffer_pos + i + 0] - 1) * delta;
                nlms->fir_coef[i + 1] += (nlms->fir_sign_buffer[buffer_pos + i + 1] - 1) * delta;
                nlms->fir_coef[i + 2] += (nlms->fir_sign_buffer[buffer_pos + i + 2] - 1) * delta;
                nlms->fir_coef[i + 3] += (nlms->fir_sign_buffer[buffer_pos + i + 3] - 1) * delta;
                
                nlms->iir_coef[i + 0] += (nlms->iir_sign_buffer[buffer_pos + i + 0] - 1) * delta;
                nlms->iir_coef[i + 1] += (nlms->iir_sign_buffer[buffer_pos + i + 1] - 1) * delta;
                nlms->iir_coef[i + 2] += (nlms->iir_sign_buffer[buffer_pos + i + 2] - 1) * delta;
                nlms->iir_coef[i + 3] += (nlms->iir_sign_buffer[buffer_pos + i + 3] - 1) * delta;
            }
#endif
        }

        buffer_pos = (buffer_pos - 1) & buffer_pos_mask;
        nlms->fir_buffer[buffer_pos] = nlms->fir_buffer[buffer_pos + num_coef] = data[smpl];
        nlms->iir_buffer[buffer_pos] = nlms->iir_buffer[buffer_pos + num_coef] = predict;

        nlms->iir_sign_buffer[buffer_pos] = nlms->iir_sign_buffer[buffer_pos + num_coef] = DANAUTILITY_SIGN(nlms->iir_buffer[buffer_pos]) + 1;
        nlms->fir_sign_buffer[buffer_pos] = nlms->fir_sign_buffer[buffer_pos + num_coef] = DANAUTILITY_SIGN(data[smpl]) + 1;
    }

    nlms->buffer_pos = buffer_pos;
    nlms->num_input_samples += num_samples;
    return DANAPREDICTOR_APIRESULT_OK;
}

DANAPredictorApiResult DANALMSFilter_SynthesizeInt32(struct DANALMSFilter* restrict nlms, uint32_t num_coef, const int32_t* restrict residual, uint32_t num_samples, int32_t* restrict output, uint8_t step_idx) {
    if ((nlms == NULL) || (residual == NULL) || (output == NULL)) return DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
    if (num_coef > nlms->max_num_coef) return DANAPREDICTOR_APIRESULT_EXCEED_MAX_ORDER;

    const int32_t shift_table[4] = {6, 4, 2, 1};
    int32_t shift = shift_table[step_idx & 3];

    DANA_Assert(num_coef >= 4 && DANAUTILITY_IS_POWERED_OF_2(num_coef));
    memcpy(output, residual, sizeof(int32_t) * num_samples);

    uint32_t buffer_pos = nlms->buffer_pos;
    const uint32_t buffer_pos_mask = (num_coef - 1);
    uint32_t smpl = 0;

    if (nlms->num_input_samples < num_coef) {
        uint32_t num_buffering_samples = DANAUTILITY_MIN(num_coef - nlms->num_input_samples, num_samples);
        uint32_t buffer_offset = (num_coef > (num_samples + nlms->num_input_samples)) ? (num_coef - (num_samples + nlms->num_input_samples)) : 0;
        
        for (smpl = 0; smpl < num_buffering_samples; smpl++) {
            nlms->fir_sign_buffer[buffer_offset + smpl] = nlms->fir_sign_buffer[buffer_offset + smpl + num_coef] =
            nlms->iir_sign_buffer[buffer_offset + smpl] = nlms->iir_sign_buffer[buffer_offset + smpl + num_coef] = DANAUTILITY_SIGN(residual[num_buffering_samples - smpl - 1]) + 1;
            nlms->iir_buffer[buffer_offset + smpl] = nlms->iir_buffer[buffer_offset + smpl + num_coef] =
            nlms->fir_buffer[buffer_offset + smpl] = nlms->fir_buffer[buffer_offset + smpl + num_coef] = residual[num_buffering_samples - smpl - 1];
        }
        buffer_pos += num_buffering_samples;
    }

    for (; smpl < num_samples; smpl++) {
        int32_t predict = (1 << 9);
        uint32_t i = 0;

#ifdef __AVX2__
        __m256i v_pred = _mm256_setzero_si256();
        for (; i + 7 < num_coef; i += 8) {
            __m256i v_f_c = _mm256_loadu_si256((__m256i const*)&nlms->fir_coef[i]);
            __m256i v_f_b = _mm256_loadu_si256((__m256i const*)&nlms->fir_buffer[buffer_pos + i]);
            __m256i v_i_c = _mm256_loadu_si256((__m256i const*)&nlms->iir_coef[i]);
            __m256i v_i_b = _mm256_loadu_si256((__m256i const*)&nlms->iir_buffer[buffer_pos + i]);
            v_pred = _mm256_add_epi32(v_pred, _mm256_add_epi32(_mm256_mullo_epi32(v_f_c, v_f_b), _mm256_mullo_epi32(v_i_c, v_i_b)));
        }
        __m128i v_pred_low = _mm256_castsi256_si128(v_pred);
        __m128i v_pred_high = _mm256_extracti128_si256(v_pred, 1);
        __m128i v1 = _mm_add_epi32(v_pred_low, v_pred_high);
        __m128i v2 = _mm_add_epi32(v1, _mm_srli_si128(v1, 8)); 
        __m128i v3 = _mm_add_epi32(v2, _mm_srli_si128(v2, 4));
        predict += _mm_cvtsi128_si32(v3);
#endif

        for (; i < num_coef; i++) {
            predict += nlms->fir_coef[i] * nlms->fir_buffer[buffer_pos + i];
            predict += nlms->iir_coef[i] * nlms->iir_buffer[buffer_pos + i];
        }
        predict = DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(predict, 10);

        output[smpl] += predict;
        
        int32_t abs_res = DANAUTILITY_ABS(residual[smpl]);
        int32_t log2res = DANAUTILITY_LOG2CEIL(abs_res + 1);
        int32_t signres = DANAUTILITY_SIGN(residual[smpl]);
        int32_t delta = signres * ((log2res << shift) >> 5);

        if (delta != 0) {
#ifdef __AVX2__
            __m256i v_delta = _mm256_set1_epi32(delta);
            __m256i v_ones = _mm256_set1_epi32(1);
            for (i = 0; i + 7 < num_coef; i += 8) {
                __m256i v_f_sb = _mm256_loadu_si256((__m256i const*)&nlms->fir_sign_buffer[buffer_pos + i]);
                __m256i v_i_sb = _mm256_loadu_si256((__m256i const*)&nlms->iir_sign_buffer[buffer_pos + i]);
                
                __m256i v_f_c = _mm256_loadu_si256((__m256i const*)&nlms->fir_coef[i]);
                __m256i v_i_c = _mm256_loadu_si256((__m256i const*)&nlms->iir_coef[i]);
                
                v_f_sb = _mm256_sub_epi32(v_f_sb, v_ones);
                v_i_sb = _mm256_sub_epi32(v_i_sb, v_ones);
                
                v_f_c = _mm256_add_epi32(v_f_c, _mm256_mullo_epi32(v_f_sb, v_delta));
                v_i_c = _mm256_add_epi32(v_i_c, _mm256_mullo_epi32(v_i_sb, v_delta));
                
                _mm256_storeu_si256((__m256i*)&nlms->fir_coef[i], v_f_c);
                _mm256_storeu_si256((__m256i*)&nlms->iir_coef[i], v_i_c);
            }
            for (; i < num_coef; i += 4) {
                nlms->fir_coef[i + 0] += (nlms->fir_sign_buffer[buffer_pos + i + 0] - 1) * delta;
                nlms->fir_coef[i + 1] += (nlms->fir_sign_buffer[buffer_pos + i + 1] - 1) * delta;
                nlms->fir_coef[i + 2] += (nlms->fir_sign_buffer[buffer_pos + i + 2] - 1) * delta;
                nlms->fir_coef[i + 3] += (nlms->fir_sign_buffer[buffer_pos + i + 3] - 1) * delta;
                
                nlms->iir_coef[i + 0] += (nlms->iir_sign_buffer[buffer_pos + i + 0] - 1) * delta;
                nlms->iir_coef[i + 1] += (nlms->iir_sign_buffer[buffer_pos + i + 1] - 1) * delta;
                nlms->iir_coef[i + 2] += (nlms->iir_sign_buffer[buffer_pos + i + 2] - 1) * delta;
                nlms->iir_coef[i + 3] += (nlms->iir_sign_buffer[buffer_pos + i + 3] - 1) * delta;
            }
#else
            for (i = 0; i < num_coef; i += 4) {
                nlms->fir_coef[i + 0] += (nlms->fir_sign_buffer[buffer_pos + i + 0] - 1) * delta;
                nlms->fir_coef[i + 1] += (nlms->fir_sign_buffer[buffer_pos + i + 1] - 1) * delta;
                nlms->fir_coef[i + 2] += (nlms->fir_sign_buffer[buffer_pos + i + 2] - 1) * delta;
                nlms->fir_coef[i + 3] += (nlms->fir_sign_buffer[buffer_pos + i + 3] - 1) * delta;
                
                nlms->iir_coef[i + 0] += (nlms->iir_sign_buffer[buffer_pos + i + 0] - 1) * delta;
                nlms->iir_coef[i + 1] += (nlms->iir_sign_buffer[buffer_pos + i + 1] - 1) * delta;
                nlms->iir_coef[i + 2] += (nlms->iir_sign_buffer[buffer_pos + i + 2] - 1) * delta;
                nlms->iir_coef[i + 3] += (nlms->iir_sign_buffer[buffer_pos + i + 3] - 1) * delta;
            }
#endif
        }

        buffer_pos = (buffer_pos - 1) & buffer_pos_mask;
        nlms->fir_buffer[buffer_pos] = nlms->fir_buffer[buffer_pos + num_coef] = output[smpl];
        nlms->iir_buffer[buffer_pos] = nlms->iir_buffer[buffer_pos + num_coef] = predict;

        nlms->iir_sign_buffer[buffer_pos] = nlms->iir_sign_buffer[buffer_pos + num_coef] = DANAUTILITY_SIGN(nlms->iir_buffer[buffer_pos]) + 1;
        nlms->fir_sign_buffer[buffer_pos] = nlms->fir_sign_buffer[buffer_pos + num_coef] = DANAUTILITY_SIGN(output[smpl]) + 1;
    }

    nlms->buffer_pos = buffer_pos;
    nlms->num_input_samples += num_samples;
    return DANAPREDICTOR_APIRESULT_OK;
}

uint32_t DANAOptimalEncodeEstimator_CalculateMaxNumPartitions(uint32_t max_num_samples, uint32_t delta_num_samples) {
    return DANAOPTIMALENCODEESTIMATOR_CALCULATE_NUM_NODES(max_num_samples, delta_num_samples);
}

struct DANAOptimalBlockPartitionEstimator* DANAOptimalEncodeEstimator_Create(uint32_t max_num_samples, uint32_t delta_num_samples) {
    if (max_num_samples < delta_num_samples) return NULL;

    struct DANAOptimalBlockPartitionEstimator* oee = malloc(sizeof(struct DANAOptimalBlockPartitionEstimator));
    uint32_t tmp_max_num_nodes = DANAOPTIMALENCODEESTIMATOR_CALCULATE_NUM_NODES(max_num_samples, delta_num_samples);
    oee->max_num_nodes = tmp_max_num_nodes;

    oee->adjacency_matrix = malloc(sizeof(double *) * tmp_max_num_nodes);
    oee->cost = malloc(sizeof(double) * tmp_max_num_nodes);
    oee->path = malloc(sizeof(uint32_t) * tmp_max_num_nodes);
    
    for (uint32_t i = 0; i < tmp_max_num_nodes; i++) {
        oee->adjacency_matrix[i] = malloc(sizeof(double) * tmp_max_num_nodes);
    }
    return oee;
}

void DANAOptimalEncodeEstimator_Destroy(struct DANAOptimalBlockPartitionEstimator* oee) {
    if (oee != NULL) {
        for (uint32_t i = 0; i < oee->max_num_nodes; i++) {
            NULLCHECK_AND_FREE(oee->adjacency_matrix[i]);
        }
        NULLCHECK_AND_FREE(oee->adjacency_matrix);
        NULLCHECK_AND_FREE(oee->cost);
        NULLCHECK_AND_FREE(oee->path);
        free(oee);
    }
}

/* DAG shortest-path O(V+E) */
static DANAPredictorApiResult DANAOptimalEncodeEstimator_ApplyDAGShortestPath(
        struct DANAOptimalBlockPartitionEstimator* restrict oee,
        uint32_t num_nodes,
        uint32_t start_node,
        uint32_t goal_node,
        double* restrict min_cost)
{
    if (oee == NULL || min_cost == NULL) return DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT;
    if (num_nodes > oee->max_num_nodes) return DANAPREDICTOR_APIRESULT_EXCEED_MAX_ORDER;

    /* Initialise costs and paths */
    for (uint32_t i = 0; i < num_nodes; i++) {
        oee->cost[i] = DANAOPTIMALENCODEESTIMATOR_BIG_WEIGHT;
        oee->path[i] = 0xFFFFFFFFUL;
    }
    oee->cost[start_node] = 0.0;

    /*
     * Single forward pass through topological order.
     * For each node u (in order), relax all outgoing edges u -> v where v > u.
     */
    for (uint32_t u = start_node; u < goal_node; u++) {
        double cost_u = oee->cost[u];
        /* Skip nodes that are unreachable */
        if (cost_u >= DANAOPTIMALENCODEESTIMATOR_BIG_WEIGHT) continue;

        const double* row = oee->adjacency_matrix[u];
        for (uint32_t v = u + 1; v <= goal_node; v++) {
            double edge = row[v];
            /* Skip sentinel (unreachable/invalid) edges */
            if (edge >= DANAOPTIMALENCODEESTIMATOR_BIG_WEIGHT) continue;

            double new_cost = cost_u + edge;
            if (new_cost < oee->cost[v]) {
                oee->cost[v] = new_cost;
                oee->path[v] = u;
            }
        }
    }

    *min_cost = oee->cost[goal_node];
    return DANAPREDICTOR_APIRESULT_OK;
}

DANAPredictorApiResult DANAOptimalEncodeEstimator_SearchOptimalBlockPartitions(
        struct DANAOptimalBlockPartitionEstimator* restrict oee,
        struct DANALPCCalculator* restrict lpcc,
        const double* const* restrict data,
        uint32_t num_channels, uint32_t num_samples,
        uint32_t min_num_block_samples, uint32_t delta_num_samples,
        uint32_t max_num_block_samples, uint32_t bits_per_sample,
        uint32_t parcor_order,
        uint32_t* restrict optimal_num_partitions,
        uint32_t* restrict optimal_block_partition)
{
    if (oee == NULL || lpcc == NULL || data == NULL || optimal_num_partitions == NULL || optimal_block_partition == NULL) return DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT;

    uint32_t num_nodes = DANAOPTIMALENCODEESTIMATOR_CALCULATE_NUM_NODES(num_samples, delta_num_samples);
    if (num_nodes > oee->max_num_nodes) return DANAPREDICTOR_APIRESULT_EXCEED_MAX_ORDER;

    uint8_t* is_candidate_node = calloc(num_nodes, sizeof(uint8_t));
    double* frame_energy = calloc(num_nodes, sizeof(double));

    /* Calculate energy on small delta segments to find transients */
    for (uint32_t k = 0; k < num_nodes - 1; k++) {
        uint32_t offset = k * delta_num_samples;
        uint32_t len = DANAUTILITY_MIN(delta_num_samples, num_samples - offset);
        double e = 0.0;
        for (uint32_t ch = 0; ch < num_channels; ch++) {
            for (uint32_t s = 0; s < len; s++) {
                e += data[ch][offset + s] * data[ch][offset + s];
            }
        }
        frame_energy[k] = e / ((double)len * num_channels + 1e-9);
    }

    is_candidate_node[0] = 1;
    is_candidate_node[num_nodes - 1] = 1;

    /* Place candidate split points at transients */
    for (uint32_t k = 1; k < num_nodes - 1; k++) {
        double e1 = frame_energy[k-1] + 1e-9;
        double e2 = frame_energy[k] + 1e-9;
        double ratio = e1 > e2 ? e1/e2 : e2/e1;
        if (ratio > 3.0) is_candidate_node[k] = 1;
    }

    /* Force candidates to guarantee valid graph edges matching max_block_samples */
    uint32_t max_steps = max_num_block_samples / delta_num_samples;
    uint32_t last_c = 0;
    for (uint32_t k = 1; k < num_nodes; k++) {
        if (is_candidate_node[k]) {
            uint32_t dist = k - last_c;
            if (dist > max_steps) {
                uint32_t num_splits = (dist + max_steps - 1) / max_steps;
                uint32_t split_dist = dist / num_splits;
                for (uint32_t s = 1; s < num_splits; s++) {
                    is_candidate_node[last_c + s * split_dist] = 1;
                }
            }
            last_c = k;
        }
    }

    int error_flag = 0;

    for (uint32_t i = 0; i < num_nodes; i++) {
        if (error_flag) break;
        if (!is_candidate_node[i]) {
            for (uint32_t j = i + 1; j < num_nodes; j++) oee->adjacency_matrix[i][j] = DANAOPTIMALENCODEESTIMATOR_BIG_WEIGHT;
            continue;
        }

        for (uint32_t j = i + 1; j < num_nodes; j++) {
            if (error_flag) break;
            if (!is_candidate_node[j]) {
                oee->adjacency_matrix[i][j] = DANAOPTIMALENCODEESTIMATOR_BIG_WEIGHT;
                continue;
            }

            uint32_t num_block_samples = (j - i) * delta_num_samples;
            uint32_t sample_offset = i * delta_num_samples;
            int local_err = 0;

            num_block_samples = DANAUTILITY_MIN(num_block_samples, num_samples - sample_offset);

            if ((num_block_samples < min_num_block_samples) || (num_block_samples > max_num_block_samples)) {
                oee->adjacency_matrix[i][j] = DANAOPTIMALENCODEESTIMATOR_BIG_WEIGHT;
                continue;
            }

            double estimated_code_length = 0.0;
            for (uint32_t ch = 0; ch < num_channels; ch++) {
                if (DANALPCCalculator_CalculatePARCORCoefDouble(lpcc, &data[ch][sample_offset], num_block_samples, lpcc->parcor_coef, parcor_order) != DANAPREDICTOR_APIRESULT_OK) {
                    local_err = 1; break;
                }
                double code_length;
                if (DANALPCCalculator_EstimateCodeLength(&data[ch][sample_offset], num_block_samples, bits_per_sample, lpcc->parcor_coef, parcor_order, &code_length) != DANAPREDICTOR_APIRESULT_OK) {
                    local_err = 1; break;
                }
                estimated_code_length += num_block_samples * code_length;
            }

            if (local_err) {
                error_flag = 1;
                break;
            }

            estimated_code_length += DANAOPTIMALENCODEESTIMATOR_ESTIMATE_BLOCK_SIZE;
            estimated_code_length += DANAOPTIMALENCODEESTIMATOR_LONGPATH_PENALTY;
            oee->adjacency_matrix[i][j] = estimated_code_length;
        }
    }

    if (error_flag) {
        free(is_candidate_node); free(frame_energy);
        return DANAPREDICTOR_APIRESULT_FAILED_TO_CALCULATION;
    }

    double min_code_length;
    if (DANAOptimalEncodeEstimator_ApplyDAGShortestPath(oee, num_nodes, 0, num_nodes - 1, &min_code_length) != DANAPREDICTOR_APIRESULT_OK) {
        free(is_candidate_node); free(frame_energy);
        return DANAPREDICTOR_APIRESULT_FAILED_TO_CALCULATION;
    }

    uint32_t tmp_optimal_num_partitions = 0;
    uint32_t tmp_node = num_nodes - 1;
    while (tmp_node != 0) {
        DANA_Assert(tmp_node > oee->path[tmp_node]);
        tmp_node = oee->path[tmp_node];
        tmp_optimal_num_partitions++;
    }

    tmp_node = num_nodes - 1;
    for (uint32_t i = 0; i < tmp_optimal_num_partitions; i++) {
        uint32_t num_block_samples = (tmp_node - oee->path[tmp_node]) * delta_num_samples;
        uint32_t sample_offset = oee->path[tmp_node] * delta_num_samples;
        num_block_samples = DANAUTILITY_MIN(num_block_samples, num_samples - sample_offset);

        optimal_block_partition[tmp_optimal_num_partitions - i - 1] = num_block_samples;
        tmp_node = oee->path[tmp_node];
    }

    *optimal_num_partitions = tmp_optimal_num_partitions;
    free(is_candidate_node); free(frame_energy);
    return DANAPREDICTOR_APIRESULT_OK;
}

struct DANAEmphasisFilter* DANAEmphasisFilter_Create(void) {
    struct DANAEmphasisFilter* emp = malloc(sizeof(struct DANAEmphasisFilter));
    if (DANAEmphasisFilter_Reset(emp) != DANAPREDICTOR_APIRESULT_OK) {
        free(emp);
        return NULL;
    }
    return emp;
}

void DANAEmphasisFilter_Destroy(struct DANAEmphasisFilter* emp) {
    NULLCHECK_AND_FREE(emp);
}

DANAPredictorApiResult DANAEmphasisFilter_Reset(struct DANAEmphasisFilter* emp) {
    if (emp != NULL) emp->prev_int32 = 0;
    return DANAPREDICTOR_APIRESULT_OK;
}

DANAPredictorApiResult DANAEmphasisFilter_PreEmphasisInt32(struct DANAEmphasisFilter* restrict emp, int32_t* restrict data, uint32_t num_samples, int32_t coef_shift) {
    if ((emp == NULL) || (data == NULL)) return DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT;

    int32_t prev_int32 = emp->prev_int32;
    const int32_t coef_numer = (int32_t)((1 << coef_shift) - 1);

    for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
        int32_t tmp_int32 = data[smpl];
        data[smpl] -= (int32_t)DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(prev_int32 * coef_numer, coef_shift);
        prev_int32 = tmp_int32;
    }

    emp->prev_int32 = prev_int32;
    return DANAPREDICTOR_APIRESULT_OK;
}

DANAPredictorApiResult DANAEmphasisFilter_DeEmphasisInt32(struct DANAEmphasisFilter* restrict emp, int32_t* restrict data, uint32_t num_samples, int32_t coef_shift) {
    if ((emp == NULL) || (data == NULL)) return DANAPREDICTOR_APIRESULT_INVALID_ARGUMENT;

    const int32_t coef_numer = (int32_t)((1 << coef_shift) - 1);
    data[0] += (int32_t)DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(emp->prev_int32 * coef_numer, coef_shift);

    for (uint32_t smpl = 1; smpl < num_samples; smpl++) {
        data[smpl] += (int32_t)DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(data[smpl - 1] * coef_numer, coef_shift);
    }

    emp->prev_int32 = data[num_samples - 1];
    return DANAPREDICTOR_APIRESULT_OK;
}

void DANAEmphasisFilter_PreEmphasisDouble(double* restrict data, uint32_t num_samples, int32_t coef_shift) {
    DANA_Assert(data != NULL);

    double coef = (pow(2.0, (double)coef_shift) - 1.0) * pow(2.0, (double)-coef_shift);
    double prev = 0.0;
    
    for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
        double tmp = data[smpl];
        data[smpl] -= prev * coef;
        prev = tmp;
    }
}