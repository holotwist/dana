#ifndef DANA_INTERNAL_H_INCLUDED
#define DANA_INTERNAL_H_INCLUDED

#include <assert.h>
#include <stdlib.h>

#define DANA_MAX_CHANNELS                            8
#define DANA_BLOCK_SYNC_CODE                         0xFFFF
#define DANALONGTERM_MAX_PERIOD                      2048
#define DANALONGTERM_PERIOD_NUM_BITS                 11
#define DANALONGTERM_NUM_PITCH_CANDIDATES            DANALONGTERM_MAX_PERIOD
#define DANAPARCOR_COEF_LOW_ORDER_THRESHOULD         4
#define DANALONGTERM_MIN_PITCH_THRESHOULD            3
#define DANA_MIN_BLOCK_NUM_SAMPLES                   1024
#define DANA_SEARCH_BLOCK_NUM_SAMPLES_DELTA          512
#define DANA_PRE_EMPHASIS_COEFFICIENT_SHIFT          5
#define DANACODER_NUM_RECURSIVERICE_PARAMETER        2
#define DANACODER_LOW_THRESHOULD_PARAMETER           8 
#define DANACODER_QUOTPART_THRESHOULD                16
#define DANA_STREAMING_DECODE_NUM_SAMPLES_MARGIN     1.05f
#define DANA_STREAMING_DECODE_MAX_NUM_PACKETS        8

#define DANA_CH_FLAG_PRE_EMPHASIS                    (1 << 0)
#define DANA_CH_FLAG_LMS_STAGES_MASK                 (3 << 1) // 0 to 3 LMS stages

#define DANA_MAX_LMS_STAGES                          3

#define DANAOPTIMALENCODEESTIMATOR_LONGPATH_PENALTY  300
#define DANA_ESTIMATE_CODELENGTH_THRESHOLD           1.05f

#define DANA_HEADER_CRC16_CALC_START_OFFSET          (1 * 4 + 4 + 2)
#define DANA_BLOCK_CRC16_CALC_START_OFFSET           (2 + 3 + 2)
#define DANA_MINIMUM_BLOCK_HEADER_SIZE               10

#define DANA_GET_PARCOR_QUANTIZE_BIT_WIDTH(order)  (((order) < DANAPARCOR_COEF_LOW_ORDER_THRESHOULD) ? 16 : 8)

#define NULLCHECK_AND_FREE(ptr) do { \
    if ((ptr) != NULL) {             \
        free(ptr);                   \
        (ptr) = NULL;                \
    }                                \
} while (0)

#ifdef NDEBUG
#define DANA_Assert(condition) ((void)(condition))
#else
#define DANA_Assert(condition) assert(condition)
#endif

#ifdef NDEBUG
#define DANA_CHECK_RETURN_IF_TRUE(condition, return_value)
#else
#define DANA_CHECK_RETURN_IF_TRUE(condition, return_value) \
    if (condition) return (return_value);
#endif

typedef enum {
    DANA_BLOCK_DATA_TYPE_COMPRESSDATA  = 0,
    DANA_BLOCK_DATA_TYPE_SILENT        = 1,
    DANA_BLOCK_DATA_TYPE_RAWDATA       = 2,
    DANA_BLOCK_DATA_TYPE_INVAILD       = 3
} DANABlockDataType;

#endif /* DANA_INTERNAL_H_INCLUDED */