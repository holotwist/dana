#include "DANACoder.h"
#include "DANAUtility.h"
#include "DANAInternal.h"

#include <stdio.h>
#include <stdlib.h>

#define DANA_CODE_BITS 32
#define DANA_TOP_VALUE ((uint32_t)1 << (DANA_CODE_BITS - 1))
#define DANA_SHIFT_BITS (DANA_CODE_BITS - 9)
#define DANA_BOTTOM_VALUE (DANA_TOP_VALUE >> 8)

#define DANA_OVERFLOW_SIGNAL 1
#define DANA_OVERFLOW_PIVOT_VALUE 32768
#define DANA_MODEL_ELEMENTS 64
#define DANA_RANGE_OVERFLOW_SHIFT 16

static const uint32_t DANA_RANGE_TOTAL[65] = { 0,19578,36160,48417,56323,60899,63265,64435,64971,65232,65351,65416,65447,65466,65476,65482,65485,65488,65490,65491,65492,65493,65494,65495,65496,65497,65498,65499,65500,65501,65502,65503,65504,65505,65506,65507,65508,65509,65510,65511,65512,65513,65514,65515,65516,65517,65518,65519,65520,65521,65522,65523,65524,65525,65526,65527,65528,65529,65530,65531,65532,65533,65534,65535,65536 };
static const uint32_t DANA_RANGE_WIDTH[64] = { 19578,16582,12257,7906,4576,2366,1170,536,261,119,65,31,19,10,6,3,3,2,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1,1 };

typedef struct {
    uint32_t nKSum;
} DANARangeState;

struct DANACoder {
    DANARangeState state[DANA_MAX_CHANNELS];
    uint32_t max_num_channels;
};

typedef struct {
    uint32_t low;
    uint32_t range;
    uint32_t help;
    uint8_t  buffer;
} DANARangeEncoder;

typedef struct {
    uint32_t low;
    uint32_t range;
    uint32_t buffer;
} DANARangeDecoder;

static void DANARange_StreamAlign(struct DANABitStream* strm) {
    if (strm->bit_count < 8) {
        DANABitWriter_PutBits(strm, 0, strm->bit_count);
    }
}

static void DANARange_StreamAlignRead(struct DANABitStream* strm) {
    if (strm->bit_count < 8) {
        uint64_t dummy;
        DANABitReader_GetBits(strm, &dummy, strm->bit_count);
    }
}

static inline void DANARange_PutC(struct DANABitStream* strm, uint8_t val) {
    if (strm->bit_count == 8 && strm->memory_p < (strm->memory_image + strm->memory_size)) {
        *strm->memory_p++ = val;
        return;
    }
    DANABitWriter_PutBits(strm, val, 8);
}

static inline uint8_t DANARange_GetC(struct DANABitStream* strm) {
    if (strm->bit_count == 0 && strm->memory_p < (strm->memory_image + strm->memory_size)) {
        uint8_t val = *strm->memory_p++;
        strm->bit_buffer = val;
        return val;
    }
    uint64_t val = 0;
    DANABitReader_GetBits(strm, &val, 8);
    return (uint8_t)val;
}

/* Range Encoder */
static void DANARangeEncoder_Init(DANARangeEncoder* rc) {
    rc->low = 0;
    rc->range = DANA_TOP_VALUE;
    rc->buffer = 0;
    rc->help = 0;
}

static void DANARangeEncoder_Normalize(DANARangeEncoder* rc, struct DANABitStream* strm) {
    while (rc->range <= DANA_BOTTOM_VALUE) {
        if (rc->low < (0xFFU << DANA_SHIFT_BITS)) {
            DANARange_PutC(strm, rc->buffer);
            for (; rc->help; rc->help--) DANARange_PutC(strm, 0xFF);
            rc->buffer = (uint8_t)(rc->low >> DANA_SHIFT_BITS);
        } else if (rc->low & DANA_TOP_VALUE) {
            DANARange_PutC(strm, rc->buffer + 1);
            for (; rc->help; rc->help--) DANARange_PutC(strm, 0x00);
            rc->buffer = (uint8_t)(rc->low >> DANA_SHIFT_BITS);
        } else {
            rc->help++;
        }
        rc->low = (rc->low << 8) & (DANA_TOP_VALUE - 1);
        rc->range <<= 8;
    }
}

static void DANARangeEncoder_EncodeFast(DANARangeEncoder* rc, struct DANABitStream* strm, uint32_t range_width, uint32_t range_total, uint32_t shift) {
    DANARangeEncoder_Normalize(rc, strm);
    uint32_t temp = rc->range >> shift;
    rc->range = temp * range_width;
    rc->low += temp * range_total;
}

static void DANARangeEncoder_EncodeDirect(DANARangeEncoder* rc, struct DANABitStream* strm, uint32_t value, uint32_t shift) {
    DANARangeEncoder_Normalize(rc, strm);
    rc->range >>= shift;
    rc->low += rc->range * value;
}

static void DANARangeEncoder_EncodeValue(DANARangeEncoder* rc, struct DANABitStream* strm, int32_t value, DANARangeState* state) {
    int64_t nEncode = value;
    uint64_t uEncode = (nEncode > 0) ? (uint64_t)nEncode * 2 - 1 : (uint64_t)(-nEncode) * 2;
    
    uint32_t pivot = (state->nKSum / 32 > 1) ? (state->nKSum / 32) : 1;
    uint64_t overflow64 = uEncode / pivot;
    uint32_t overflow = (uint32_t)overflow64;
    
    if (overflow != overflow64) {
        pivot = DANA_OVERFLOW_PIVOT_VALUE;
        overflow = (uint32_t)(uEncode / pivot);
        DANARangeEncoder_EncodeFast(rc, strm, DANA_RANGE_WIDTH[DANA_MODEL_ELEMENTS - 1], DANA_RANGE_TOTAL[DANA_MODEL_ELEMENTS - 1], DANA_RANGE_OVERFLOW_SHIFT);
        DANARangeEncoder_EncodeDirect(rc, strm, (DANA_OVERFLOW_SIGNAL >> 16) & 0xFFFF, 16);
        DANARangeEncoder_EncodeDirect(rc, strm, DANA_OVERFLOW_SIGNAL & 0xFFFF, 16);
    }
    
    uint32_t base = (uint32_t)(uEncode - ((uint64_t)overflow * pivot));
    
    state->nKSum += (uint32_t)((uEncode + 1) / 2) - ((state->nKSum + 16) >> 5);
    
    if (overflow < (DANA_MODEL_ELEMENTS - 1)) {
        DANARangeEncoder_EncodeFast(rc, strm, DANA_RANGE_WIDTH[overflow], DANA_RANGE_TOTAL[overflow], DANA_RANGE_OVERFLOW_SHIFT);
    } else {
        DANARangeEncoder_EncodeFast(rc, strm, DANA_RANGE_WIDTH[DANA_MODEL_ELEMENTS - 1], DANA_RANGE_TOTAL[DANA_MODEL_ELEMENTS - 1], DANA_RANGE_OVERFLOW_SHIFT);
        DANARangeEncoder_EncodeDirect(rc, strm, (overflow >> 16) & 0xFFFF, 16);
        DANARangeEncoder_EncodeDirect(rc, strm, overflow & 0xFFFF, 16);
    }
    
    if (pivot >= (1 << 16)) {
        uint32_t pivot_bits = 32 - DANAUTILITY_NLZ(pivot);
        uint32_t shift = (pivot_bits >= 16) ? pivot_bits - 16 : 0;
        uint32_t split_factor = 1U << shift;
        
        uint32_t pivot_A = (pivot / split_factor) + 1;
        uint32_t pivot_B = split_factor;
        
        uint32_t base_A = base / split_factor;
        uint32_t base_B = base % split_factor;
        
        DANARangeEncoder_Normalize(rc, strm);
        uint32_t temp1 = rc->range / pivot_A;
        rc->range = temp1;
        rc->low += temp1 * base_A;
        
        DANARangeEncoder_Normalize(rc, strm);
        uint32_t temp2 = rc->range / pivot_B;
        rc->range = temp2;
        rc->low += temp2 * base_B;
    } else {
        DANARangeEncoder_Normalize(rc, strm);
        uint32_t temp = rc->range / pivot;
        rc->range = temp;
        rc->low += temp * base;
    }
}

static void DANARangeEncoder_Finalize(DANARangeEncoder* rc, struct DANABitStream* strm) {
    DANARangeEncoder_Normalize(rc, strm);
    
    uint32_t temp = (rc->low >> DANA_SHIFT_BITS) + 1;
    if (temp > 0xFF) {
        DANARange_PutC(strm, rc->buffer + 1);
        for (; rc->help; rc->help--) DANARange_PutC(strm, 0);
    } else {
        DANARange_PutC(strm, rc->buffer);
        for (; rc->help; rc->help--) DANARange_PutC(strm, 0xFF);
    }
    
    DANARange_PutC(strm, temp & 0xFF);
    DANARange_PutC(strm, 0);
    DANARange_PutC(strm, 0);
    DANARange_PutC(strm, 0);
}

/* Range Decoder */
static void DANARangeDecoder_Init(DANARangeDecoder* rc, struct DANABitStream* strm) {
    DANARange_GetC(strm); // ignore first byte
    rc->buffer = DANARange_GetC(strm);
    rc->low = rc->buffer >> (8 - 7);
    rc->range = 1U << 7;
}

static uint32_t DANARangeDecoder_DecodeFast(DANARangeDecoder* rc, struct DANABitStream* strm, int shift) {
    while (rc->range <= DANA_BOTTOM_VALUE) {
        rc->buffer = (rc->buffer << 8) | DANARange_GetC(strm);
        rc->low = (rc->low << 8) | ((rc->buffer >> 1) & 0xFF);
        rc->range <<= 8;
        if (rc->range == 0) return 0;
    }
    rc->range >>= shift;
    return rc->low / rc->range;
}

static uint32_t DANARangeDecoder_DecodeFastWithUpdate(DANARangeDecoder* rc, struct DANABitStream* strm, int shift) {
    while (rc->range <= DANA_BOTTOM_VALUE) {
        rc->buffer = (rc->buffer << 8) | DANARange_GetC(strm);
        rc->low = (rc->low << 8) | ((rc->buffer >> 1) & 0xFF);
        rc->range <<= 8;
    }
    rc->range >>= shift;
    uint32_t res = rc->low / rc->range;
    rc->low -= res * rc->range;
    return res;
}

static uint8_t DANARangeDecoder_GetOverflow(uint32_t range_total) {
    uint8_t i = 0;
    while (range_total >= DANA_RANGE_TOTAL[i + 1]) i++;
    return i;
}

static uint32_t DANARangeDecoder_DecodeOverflow(DANARangeDecoder* rc, struct DANABitStream* strm, uint32_t* pivot_value) {
    uint32_t range_total = DANARangeDecoder_DecodeFast(rc, strm, DANA_RANGE_OVERFLOW_SHIFT);
    if (range_total >= 65536) return 0; // Error!
    uint32_t overflow = DANARangeDecoder_GetOverflow(range_total);
    
    rc->low -= rc->range * DANA_RANGE_TOTAL[overflow];
    rc->range = rc->range * DANA_RANGE_WIDTH[overflow];
    
    if (overflow == (DANA_MODEL_ELEMENTS - 1)) {
        uint32_t o_hi = DANARangeDecoder_DecodeFastWithUpdate(rc, strm, 16);
        uint32_t o_lo = DANARangeDecoder_DecodeFastWithUpdate(rc, strm, 16);
        overflow = (o_hi << 16) | o_lo;
        
        if (overflow == DANA_OVERFLOW_SIGNAL) {
            *pivot_value = DANA_OVERFLOW_PIVOT_VALUE;
            return DANARangeDecoder_DecodeOverflow(rc, strm, pivot_value);
        }
    }
    
    return overflow;
}

static int64_t DANARangeDecoder_DecodeValue(DANARangeDecoder* rc, struct DANABitStream* strm, DANARangeState* state) {
    uint32_t pivot = (state->nKSum / 32 > 1) ? (state->nKSum / 32) : 1;
    uint32_t overflow = DANARangeDecoder_DecodeOverflow(rc, strm, &pivot);
    
    uint32_t base = 0;
    if (pivot >= (1 << 16)) {
        uint32_t pivot_bits = 32 - DANAUTILITY_NLZ(pivot);
        uint32_t shift = (pivot_bits >= 16) ? pivot_bits - 16 : 0;
        uint32_t split_factor = 1U << shift;
        
        uint32_t pivot_A = (pivot / split_factor) + 1;
        uint32_t pivot_B = split_factor;
        
        while (rc->range <= DANA_BOTTOM_VALUE) {
            rc->buffer = (rc->buffer << 8) | DANARange_GetC(strm);
            rc->low = (rc->low << 8) | ((rc->buffer >> 1) & 0xFF);
            rc->range <<= 8;
        }
        rc->range /= pivot_A;
        uint32_t base_A = rc->low / rc->range;
        rc->low -= base_A * rc->range;
        
        while (rc->range <= DANA_BOTTOM_VALUE) {
            rc->buffer = (rc->buffer << 8) | DANARange_GetC(strm);
            rc->low = (rc->low << 8) | ((rc->buffer >> 1) & 0xFF);
            rc->range <<= 8;
        }
        rc->range /= pivot_B;
        uint32_t base_B = rc->low / rc->range;
        rc->low -= base_B * rc->range;
        
        base = base_A * split_factor + base_B;
    } else {
        while (rc->range <= DANA_BOTTOM_VALUE) {
            rc->buffer = (rc->buffer << 8) | DANARange_GetC(strm);
            rc->low = (rc->low << 8) | ((rc->buffer >> 1) & 0xFF);
            rc->range <<= 8;
            if (rc->range == 0) return 0;
        }
        rc->range /= pivot;
        base = rc->low / rc->range;
        rc->low -= base * rc->range;
    }
    
    uint64_t uValue = (uint64_t)base + ((uint64_t)overflow * pivot);
    
    state->nKSum += (uint32_t)((uValue + 1) / 2) - ((state->nKSum + 16) >> 5);
    
    return (uValue & 1) ? (int64_t)(uValue >> 1) + 1 : -(int64_t)(uValue >> 1);
}

static void DANARangeDecoder_Finalize(DANARangeDecoder* rc, struct DANABitStream* strm) {
    while (rc->range <= DANA_BOTTOM_VALUE) {
        DANARange_GetC(strm);
        rc->range <<= 8;
        if (rc->range == 0) return;
    }
}

static void DANARangeState_Init(DANARangeState* state) {
    state->nKSum = (1U << 10) * 16;
}

/* Coder API */

struct DANACoder* DANACoder_Create(uint32_t max_num_channels, uint32_t max_num_parameters) {
    (void)max_num_parameters;
    struct DANACoder* coder = malloc(sizeof(struct DANACoder));
    coder->max_num_channels = max_num_channels;
    return coder;
}

void DANACoder_Destroy(struct DANACoder* coder) {
    if (coder != NULL) {
        free(coder);
    }
}

void DANACoder_CalculateInitialRecursiveRiceParameter(struct DANACoder* restrict coder, uint32_t num_parameters, const int32_t** restrict data, uint32_t num_channels, uint32_t num_samples) {
    (void)coder; (void)num_parameters; (void)data; (void)num_channels; (void)num_samples;
}

void DANACoder_PutInitialRecursiveRiceParameter(struct DANACoder* restrict coder, struct DANABitStream* restrict strm, uint32_t num_parameters, uint32_t bitwidth, uint32_t channel_index) {
    (void)coder; (void)strm; (void)num_parameters; (void)bitwidth; (void)channel_index;
}

void DANACoder_GetInitialRecursiveRiceParameter(struct DANACoder* restrict coder, struct DANABitStream* restrict strm, uint32_t num_parameters, uint32_t bitwidth, uint32_t channel_index) {
    (void)coder; (void)strm; (void)num_parameters; (void)bitwidth; (void)channel_index;
}

void DANACoder_PutDataArray(struct DANACoder* restrict coder, struct DANABitStream* restrict strm, uint32_t num_parameters, const int32_t** restrict data, uint32_t num_channels, uint32_t num_samples) {
    (void)num_parameters;
    DANARange_StreamAlign(strm);
    
    DANARangeEncoder rc;
    DANARangeEncoder_Init(&rc);
    
    for (uint32_t ch = 0; ch < num_channels; ch++) {
        DANARangeState_Init(&coder->state[ch]);
    }
    
    for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
        for (uint32_t ch = 0; ch < num_channels; ch++) {
            DANARangeEncoder_EncodeValue(&rc, strm, data[ch][smpl], &coder->state[ch]);
        }
    }
    
    DANARangeEncoder_Finalize(&rc, strm);
}

void DANACoder_GetDataArray(struct DANACoder* restrict coder, struct DANABitStream* restrict strm, uint32_t num_parameters, int32_t** restrict data, uint32_t num_channels, uint32_t num_samples) {
    (void)num_parameters;
    DANARange_StreamAlignRead(strm);
    
    DANARangeDecoder rc;
    DANARangeDecoder_Init(&rc, strm);
    
    for (uint32_t ch = 0; ch < num_channels; ch++) {
        DANARangeState_Init(&coder->state[ch]);
    }
    
    for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
        for (uint32_t ch = 0; ch < num_channels; ch++) {
            data[ch][smpl] = (int32_t)DANARangeDecoder_DecodeValue(&rc, strm, &coder->state[ch]);
        }
    }
    
    DANARangeDecoder_Finalize(&rc, strm);
}