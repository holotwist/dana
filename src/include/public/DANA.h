#ifndef DANA_H_INCLUDED
#define DANA_H_INCLUDED

#include "DANAStdint.h"

#define DANA_VERSION_STRING          "1.0.0"
#define DANA_FORMAT_VERSION          1
#define DANA_HEADER_SIZE             43
#define DANA_BLOCK_HEADER_SIZE       9
#define DANA_NUM_SAMPLES_INVALID     0xFFFFFFFF
#define DANA_NUM_BLOCKS_INVALID      0xFFFFFFFF
#define DANA_MAX_BLOCK_SIZE_INVAILD  0xFFFFFFFF

static inline uint32_t DANA_CalculateSufficientBlockSize(uint32_t num_channels, uint32_t num_samples, uint32_t bit_per_sample) {
    return (2 * num_channels * num_samples * (bit_per_sample / 8));
}

typedef enum {
    DANA_APIRESULT_OK = 0,
    DANA_APIRESULT_NG,
    DANA_APIRESULT_INVALID_ARGUMENT,
    DANA_APIRESULT_EXCEED_HANDLE_CAPACITY,
    DANA_APIRESULT_INSUFFICIENT_BUFFER_SIZE,
    DANA_APIRESULT_INVAILD_CHPROCESSMETHOD,
    DANA_APIRESULT_FAILED_TO_CALCULATE_COEF,
    DANA_APIRESULT_FAILED_TO_PREDICT,
    DANA_APIRESULT_FAILED_TO_SYNTHESIZE,
    DANA_APIRESULT_INSUFFICIENT_DATA_SIZE,
    DANA_APIRESULT_INVALID_HEADER_FORMAT,
    DANA_APIRESULT_DETECT_DATA_CORRUPTION,
    DANA_APIRESULT_FAILED_TO_FIND_SYNC_CODE,
    DANA_APIRESULT_INVALID_WINDOWFUNCTION_TYPE,
    DANA_APIRESULT_NO_DATA_FRAGMENTS,
    DANA_APIRESULT_PARAMETER_NOT_SET
} DANAApiResult;

typedef enum {
    DANA_CHPROCESSMETHOD_NONE = 0,
    DANA_CHPROCESSMETHOD_STEREO_MS,
    DANA_CHPROCESSMETHOD_STEREO_LS,
    DANA_CHPROCESSMETHOD_STEREO_ADAPTIVE
} DANAChannelProcessMethod;

typedef enum {
    DANA_WINDOWFUNCTIONTYPE_RECTANGULAR = 0,
    DANA_WINDOWFUNCTIONTYPE_SIN,
    DANA_WINDOWFUNCTIONTYPE_HANN,
    DANA_WINDOWFUNCTIONTYPE_BLACKMAN,
    DANA_WINDOWFUNCTIONTYPE_VORBIS,
    DANA_WINDOWFUNCTIONTYPE_TUKEY
} DANAWindowFunctionType;

struct DANAMetadata {
    char* title;
    char* artist;
    char* album;
    char* year;
    char* genre;
    char* track;
    char* bpm;
    char* key;
    char* lyrics;
    uint8_t* cover_data;
    uint32_t cover_size;
    uint8_t* seek_table;
    uint32_t seek_table_size;
};

struct DANAWaveFormat {
    uint32_t num_channels;
    uint32_t bit_per_sample;
    uint32_t sampling_rate;
    uint8_t  offset_lshift;
};

struct DANAEncodeParameter {
    uint32_t parcor_order;
    uint32_t longterm_order;
    uint32_t lms_order_per_filter;
    DANAChannelProcessMethod ch_process_method;
    DANAWindowFunctionType window_function_type;
    uint32_t max_num_block_samples;
};

struct DANAHeaderInfo {
    struct DANAWaveFormat      wave_format;
    struct DANAEncodeParameter encode_param;
    uint32_t                   num_samples;
    uint32_t                   num_blocks;
    uint32_t                   max_block_size;
    uint32_t                   max_bit_per_second;
    struct DANAMetadata        metadata;
};

void DANAMetadata_Init(struct DANAMetadata* meta);
void DANAMetadata_Release(struct DANAMetadata* meta);

#endif /* DANA_H_INCLUDED */