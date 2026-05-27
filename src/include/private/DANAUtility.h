#ifndef DANAUTILITY_H_INCLUDED
#define DANAUTILITY_H_INCLUDED

#include "DANAStdint.h"
#include <stddef.h> 

#if defined(__SSE4_1__)
#define USE_SSE
#include <x86intrin.h>
#endif

#define DANA_PI 3.1415926535897932384626433832795029

#define DANAUTILITY_UNUSED_ARGUMENT(arg)  ((void)(arg))

#if ((((int32_t)-1) >> 1) == ((int32_t)-1))
#define DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(sint32, rshift) ((sint32) >> (rshift))
#else
#define DANAUTILITY_SHIFT_RIGHT_ARITHMETIC(sint32, rshift) ((((uint64_t)(sint32) + 0x80000000UL) >> (rshift)) - (0x80000000UL >> (rshift)))
#endif

#define DANAUTILITY_SIGN(val) (((val) > 0) - ((val) < 0))
#define DANAUTILITY_MAX(a,b)   (((a) > (b)) ? (a) : (b))
#define DANAUTILITY_MIN(a,b)   (((a) < (b)) ? (a) : (b))
#define DANAUTILITY_INNER_VALUE(val, min, max) (DANAUTILITY_MIN((max), DANAUTILITY_MAX((min), (val))))
#define DANAUTILITY_IS_POWERED_OF_2(val) (!((val) & ((val) - 1)))
#define DANAUTILITY_SINT32_TO_UINT32(sint) (((int32_t)(sint) < 0) ? ((uint32_t)((-((sint) << 1)) - 1)) : ((uint32_t)(((sint) << 1))))
#define DANAUTILITY_UINT32_TO_SINT32(uint) ((int32_t)((uint) >> 1) ^ -(int32_t)((uint) & 1))
#define DANAUTILITY_ABS(val)               (((val) > 0) ? (val) : -(val))
#define DANAUTILITY_CALC_RSHIFT_FOR_SINT32(bitwidth) (((bitwidth) > 16) ? ((bitwidth) - 16) : 0)

#if defined(__GNUC__)
#define DANAUTILITY_NLZ(x) (((x) > 0) ? (uint32_t)__builtin_clz(x) : 32U)
#define DANAUTILITY_ROUNDUP2POWERED(x) (1U << DANAUTILITY_LOG2CEIL(x))
#else
#define DANAUTILITY_NLZ(x) DANAUtility_NLZSoft(x)
#define DANAUTILITY_ROUNDUP2POWERED(x) DANAUtility_RoundUp2PoweredSoft(x)
#endif

#define DANAUTILITY_LOG2CEIL(x) (32U - DANAUTILITY_NLZ((uint32_t)((x) - 1U)))
#define DANAUTILITY_LOG2FLOOR(x) (31U - DANAUTILITY_NLZ(x))

typedef enum {
    DANA_DATAPACKETQUEUE_APIRESULT_OK = 0,
    DANA_DATAPACKETQUEUE_APIRESULT_NG,
    DANA_DATAPACKETQUEUE_APIRESULT_EXCEED_MAX_NUM_DATA_FRAGMENTS,
    DANA_DATAPACKETQUEUE_APIRESULT_NO_DATA_FRAGMENTS
} DANADataPacketQueueApiResult;

struct DANALESolver;
struct DANADataPacketQueue;

#ifdef __cplusplus
extern "C" {
#endif

void DANAUtility_ApplyWindow(const double* restrict window, double* restrict data, uint32_t num_samples);
void DANAUtility_MakeRectangularWindow(double* restrict window, uint32_t window_size);
void DANAUtility_MakeHannWindow(double* restrict window, uint32_t window_size);
void DANAUtility_MakeBlackmanWindow(double* restrict window, uint32_t window_size);
void DANAUtility_MakeSinWindow(double* restrict window, uint32_t window_size);
void DANAUtility_MakeVorbisWindow(double* restrict window, uint32_t window_size);
void DANAUtility_MakeTukeyWindow(double* restrict window, uint32_t window_size, double alpha);

void DANAUtility_FFT(double* restrict data, uint32_t n, int32_t sign);
uint16_t DANAUtility_CalculateCRC16(const uint8_t* restrict data, uint64_t data_size);

uint32_t DANAUtility_NLZSoft(uint32_t val);
uint32_t DANAUtility_RoundUp2PoweredSoft(uint32_t val);

void DANAUtility_LRtoMSDouble(double **restrict data, uint32_t num_channels, uint32_t num_samples);
void DANAUtility_LRtoMSInt32(int32_t **restrict data, uint32_t num_channels, uint32_t num_samples);
void DANAUtility_MStoLRInt32(int32_t **restrict data, uint32_t num_channels, uint32_t num_samples);
void DANAUtility_LRtoLSDouble(double **restrict data, uint32_t num_channels, uint32_t num_samples);
void DANAUtility_LRtoLSInt32(int32_t **restrict data, uint32_t num_channels, uint32_t num_samples);
void DANAUtility_LStoLRInt32(int32_t **restrict data, uint32_t num_channels, uint32_t num_samples);
void DANAUtility_LRtoRSDouble(double **restrict data, uint32_t num_channels, uint32_t num_samples);
void DANAUtility_LRtoRSInt32(int32_t **restrict data, uint32_t num_channels, uint32_t num_samples);
void DANAUtility_RStoLRInt32(int32_t **restrict data, uint32_t num_channels, uint32_t num_samples);

double DANAUtility_Round(double d);
double DANAUtility_Log2(double x);

struct DANALESolver* DANALESolver_Create(uint32_t max_dim);
void DANALESolver_Destroy(struct DANALESolver* lesolver);
int32_t DANALESolver_Solve(struct DANALESolver* restrict lesolver, const double** restrict A, double* restrict b, uint32_t dim, uint32_t iteration_count);

uint32_t DANAUtility_GetDataBitWidth(const int32_t* restrict data, uint32_t num_samples);

struct DANADataPacketQueue* DANADataPacketQueue_Create(uint32_t max_num_packets);
void DANADataPacketQueue_Destroy(struct DANADataPacketQueue* queue);

DANADataPacketQueueApiResult DANADataPacketQueue_EnqueueDataFragment(struct DANADataPacketQueue* restrict queue, const uint8_t* restrict data, uint32_t data_size);
DANADataPacketQueueApiResult DANADataPacketQueue_GetDataFragment(struct DANADataPacketQueue* restrict queue, const uint8_t** restrict data_ptr, uint32_t* restrict data_size, uint32_t max_data_size);
DANADataPacketQueueApiResult DANADataPacketQueue_DequeueDataFragment(struct DANADataPacketQueue* restrict queue, const uint8_t** restrict data_ptr, uint32_t* restrict data_size);
uint32_t DANADataPacketQueue_GetRemainDataSize(const struct DANADataPacketQueue* queue);

char* DANAUtility_StrDup(const char* s);
char* DANAUtility_StrNDup(const char* s, size_t n);

#ifdef __cplusplus
}
#endif

#endif /* DANAUTILITY_H_INCLUDED */