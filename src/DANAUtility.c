#include "DANAUtility.h"
#include "DANAInternal.h"
#include "DANA.h"

#include <math.h>
#include <stdlib.h>
#include <stddef.h>
#include <string.h>
#include <float.h>

struct DANALESolver {
    uint32_t  max_dim;
    double*   row_scale;
    uint32_t* change_index;
    double*   x_vec;
    double*   err_vec;
    double**  A_lu;
};

struct DANADataPacket {
    const uint8_t* data;
    uint32_t       data_size;
    uint32_t       used_size;
};

struct DANADataPacketQueue {
    struct DANADataPacket* packets;
    uint32_t write_pos;
    uint32_t read_pos;
    uint32_t collect_pos;
    uint32_t num_free_packets;
    uint32_t max_num_packets;
};

// Precomputed CRC16-IBM Polynomial
static const uint16_t st_crc16_ibm_byte_table[256] = {
    0x0000, 0xc0c1, 0xc181, 0x0140, 0xc301, 0x03c0, 0x0280, 0xc241, 0xc601, 0x06c0, 0x0780, 0xc741, 0x0500, 0xc5c1, 0xc481, 0x0440,
    0xcc01, 0x0cc0, 0x0d80, 0xcd41, 0x0f00, 0xcfc1, 0xce81, 0x0e40, 0x0a00, 0xcac1, 0xcb81, 0x0b40, 0xc901, 0x09c0, 0x0880, 0xc841,
    0xd801, 0x18c0, 0x1980, 0xd941, 0x1b00, 0xdbc1, 0xda81, 0x1a40, 0x1e00, 0xdec1, 0xdf81, 0x1f40, 0xdd01, 0x1dc0, 0x1c80, 0xdc41,
    0x1400, 0xd4c1, 0xd581, 0x1540, 0xd701, 0x17c0, 0x1680, 0xd641, 0xd201, 0x12c0, 0x1380, 0xd341, 0x1100, 0xd1c1, 0xd081, 0x1040,
    0xf001, 0x30c0, 0x3180, 0xf141, 0x3300, 0xf3c1, 0xf281, 0x3240, 0x3600, 0xf6c1, 0xf781, 0x3740, 0xf501, 0x35c0, 0x3480, 0xf441,
    0x3c00, 0xfcc1, 0xfd81, 0x3d40, 0xff01, 0x3fc0, 0x3e80, 0xfe41, 0xfa01, 0x3ac0, 0x3b80, 0xfb41, 0x3900, 0xf9c1, 0xf881, 0x3840,
    0x2800, 0xe8c1, 0xe981, 0x2940, 0xeb01, 0x2bc0, 0x2a80, 0xea41, 0xee01, 0x2ec0, 0x2f80, 0xef41, 0x2d00, 0xedc1, 0xec81, 0x2c40,
    0xe401, 0x24c0, 0x2580, 0xe541, 0x2700, 0xe7c1, 0xe681, 0x2640, 0x2200, 0xe2c1, 0xe381, 0x2340, 0xe101, 0x21c0, 0x2080, 0xe041,
    0xa001, 0x60c0, 0x6180, 0xa141, 0x6300, 0xa3c1, 0xa281, 0x6240, 0x6600, 0xa6c1, 0xa781, 0x6740, 0xa501, 0x65c0, 0x6480, 0xa441,
    0x6c00, 0xacc1, 0xad81, 0x6d40, 0xaf01, 0x6fc0, 0x6e80, 0xae41, 0xaa01, 0x6ac0, 0x6b80, 0xab41, 0x6900, 0xa9c1, 0xa881, 0x6840,
    0x7800, 0xb8c1, 0xb981, 0x7940, 0xbb01, 0x7bc0, 0x7a80, 0xba41, 0xbe01, 0x7ec0, 0x7f80, 0xbf41, 0x7d00, 0xbdc1, 0xbc81, 0x7c40,
    0xb401, 0x74c0, 0x7580, 0xb541, 0x7700, 0xb7c1, 0xb681, 0x7640, 0x7200, 0xb2c1, 0xb381, 0x7340, 0xb101, 0x71c0, 0x7080, 0xb041,
    0x5000, 0x90c1, 0x9181, 0x5140, 0x9301, 0x53c0, 0x5280, 0x9241, 0x9601, 0x56c0, 0x5780, 0x9741, 0x5500, 0x95c1, 0x9481, 0x5440,
    0x9c01, 0x5cc0, 0x5d80, 0x9d41, 0x5f00, 0x9fc1, 0x9e81, 0x5e40, 0x5a00, 0x9ac1, 0x9b81, 0x5b40, 0x9901, 0x59c0, 0x5880, 0x9841,
    0x8801, 0x48c0, 0x4980, 0x8941, 0x4b00, 0x8bc1, 0x8a81, 0x4a40, 0x4e00, 0x8ec1, 0x8f81, 0x4f40, 0x8d01, 0x4dc0, 0x4c80, 0x8c41,
    0x4400, 0x84c1, 0x8581, 0x4540, 0x8701, 0x47c0, 0x4680, 0x8641, 0x8201, 0x42c0, 0x4380, 0x8341, 0x4100, 0x81c1, 0x8081, 0x4040
};

#define UNUSED 99
static const uint32_t st_nlz10_table[64] = {
    32, 20, 19, UNUSED, UNUSED, 18, UNUSED, 7, 10, 17, UNUSED, UNUSED, 14, UNUSED, 6, UNUSED,
    UNUSED, 9, UNUSED, 16, UNUSED, UNUSED, 1, 26, UNUSED, 13, UNUSED, UNUSED, 24, 5, UNUSED, UNUSED,
    UNUSED, 21, UNUSED, 8, 11, UNUSED, 15, UNUSED, UNUSED, UNUSED, UNUSED, 2, 27, 0, 25, UNUSED,
    22, UNUSED, 12, UNUSED, UNUSED, 3, 28, UNUSED, 23, UNUSED, 4, 29, UNUSED, UNUSED, 30, 31
};
#undef UNUSED

void DANAUtility_ApplyWindow(const double* restrict window, double* restrict data, uint32_t num_samples) {
    DANA_Assert(window != NULL && data != NULL);
    for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
        data[smpl] *= window[smpl];
    }
}

void DANAUtility_MakeRectangularWindow(double* restrict window, uint32_t window_size) {
    DANA_Assert(window != NULL);
    for (uint32_t smpl = 0; smpl < window_size; smpl++) {
        window[smpl] = 1.0;
    }
}

void DANAUtility_MakeHannWindow(double* restrict window, uint32_t window_size) {
    DANA_Assert(window != NULL);
    if (window_size == 1) { window[0] = 1.0; return; }
    for (uint32_t smpl = 0; smpl < window_size; smpl++) {
        double x = (double)smpl / (window_size - 1);
        window[smpl] = 0.5 - 0.5 * cos(2.0 * DANA_PI * x);
    }
}

void DANAUtility_MakeBlackmanWindow(double* restrict window, uint32_t window_size) {
    DANA_Assert(window != NULL);
    if (window_size == 1) { window[0] = 1.0; return; }
    for (uint32_t smpl = 0; smpl < window_size; smpl++) {
        double x = (double)smpl / (window_size - 1);
        window[smpl] = 0.42 - 0.5 * cos(2.0 * DANA_PI * x) + 0.08 * cos(4.0 * DANA_PI * x);
    }
}

void DANAUtility_MakeSinWindow(double* restrict window, uint32_t window_size) {
    DANA_Assert(window != NULL);
    if (window_size == 1) { window[0] = 1.0; return; }
    for (uint32_t smpl = 0; smpl < window_size; smpl++) {
        double x = (double)smpl / (window_size - 1);
        window[smpl] = sin(DANA_PI * x);
    }
}

void DANAUtility_MakeVorbisWindow(double* restrict window, uint32_t window_size) {
    DANA_Assert(window != NULL);
    if (window_size == 1) { window[0] = 1.0; return; }
    for (uint32_t smpl = 0; smpl < window_size; smpl++) {
        double x = (double)smpl / (window_size - 1);
        window[smpl] = sin((DANA_PI / 2.0) * sin(DANA_PI * x) * sin(DANA_PI * x));
    }
}

void DANAUtility_MakeTukeyWindow(double* restrict window, uint32_t window_size, double alpha) {
    DANA_Assert(window != NULL);
    if (window_size == 1) { window[0] = 1.0; return; }
    for (uint32_t smpl = 0; smpl < window_size; smpl++) {
        double x = (double)smpl / (window_size - 1);
        if (x < alpha / 2.0) {
            window[smpl] = 0.5 * (1.0 + cos(DANA_PI * ((2.0 / alpha) * x - 1.0)));
        } else if (x > (1.0 - alpha / 2.0)) {
            window[smpl] = 0.5 * (1.0 + cos(DANA_PI * ((2.0 / alpha) * x - (2.0 / alpha) + 1.0)));
        } else {
            window[smpl] = 1.0;
        }
    }
}

static void four1(double data[], unsigned long nn, int isign) {
    unsigned long n = nn << 1;
    unsigned long j = 1;
    for (unsigned long i = 1; i < n; i += 2) {
        if (j > i) {
            double tempr = data[j]; data[j] = data[i]; data[i] = tempr;
            tempr = data[j + 1]; data[j + 1] = data[i + 1]; data[i + 1] = tempr;
        }
        unsigned long m = n >> 1;
        while (m >= 2 && j > m) { j -= m; m >>= 1; }
        j += m;
    }
    
    unsigned long mmax = 2;
    while (n > mmax) {
        unsigned long istep = mmax << 1;
        double theta = isign * (6.28318530717959 / (double)mmax);
        double wtemp = sin(0.5 * theta);
        double wpr = -2.0 * wtemp * wtemp;
        double wpi = sin(theta);
        double wr = 1.0;
        double wi = 0.0;
        
        for (unsigned long m = 1; m < mmax; m += 2) {
            for (unsigned long i = m; i <= n; i += istep) {
                j = i + mmax;
                double tempr = wr * data[j] - wi * data[j + 1];
                double tempi = wr * data[j + 1] + wi * data[j];
                data[j] = data[i] - tempr;
                data[j + 1] = data[i + 1] - tempi;
                data[i] += tempr;
                data[i + 1] += tempi;
            }
            wtemp = wr;
            wr = wr * wpr - wi * wpi + wr;
            wi = wi * wpr + wtemp * wpi + wi;
        }
        mmax = istep;
    }
}

static void realft(double data[], unsigned long n, int isign) {
    double theta = 3.141592653589793 / (double)(n >> 1);
    double c1 = 0.5, c2;
    
    if (isign == 1) {
        c2 = -0.5;
        four1(data, n >> 1, 1);
    } else {
        c2 = 0.5;
        theta = -theta;
    }
    
    double wtemp = sin(0.5 * theta);
    double wpr = -2.0 * wtemp * wtemp;
    double wpi = sin(theta);
    double wr = 1.0 + wpr;
    double wi = wpi;
    unsigned long np3 = n + 3;
    
    for (unsigned long i = 2; i <= (n >> 2); i++) {
        unsigned long i1 = i + i - 1;
        unsigned long i2 = 1 + i1;
        unsigned long i3 = np3 - i2;
        unsigned long i4 = 1 + i3;
        
        double h1r = c1 * (data[i1] + data[i3]);
        double h1i = c1 * (data[i2] - data[i4]);
        double h2r = -c2 * (data[i2] + data[i4]);
        double h2i = c2 * (data[i1] - data[i3]);
        
        data[i1] = h1r + wr * h2r - wi * h2i;
        data[i2] = h1i + wr * h2i + wi * h2r;
        data[i3] = h1r - wr * h2r + wi * h2i;
        data[i4] = -h1i + wr * h2i + wi * h2r;
        
        wtemp = wr;
        wr = wr * wpr - wi * wpi + wr;
        wi = wi * wpr + wtemp * wpi + wi;
    }
    
    if (isign == 1) {
        double h1r = data[1];
        data[1] = h1r + data[2];
        data[2] = h1r - data[2];
    } else {
        double h1r = data[1];
        data[1] = c1 * (h1r + data[2]);
        data[2] = c1 * (h1r - data[2]);
        four1(data, n >> 1, -1);
    }
}

void DANAUtility_FFT(double* restrict data, uint32_t n, int32_t sign) {
    DANA_Assert(data != NULL);
    realft(&data[-1], n, (int)sign);
}

uint16_t DANAUtility_CalculateCRC16(const uint8_t* restrict data, uint64_t data_size) {
    DANA_Assert(data != NULL);
    uint16_t crc16 = 0x0000;
    while (data_size--) {
        crc16 = (crc16 >> 8) ^ st_crc16_ibm_byte_table[(crc16 ^ (*data++)) & 0xFF];
    }
    return crc16;
}

// Thanks Hacker's Delight
uint32_t DANAUtility_NLZSoft(uint32_t x) {
    x |= (x >> 1);
    x |= (x >> 2);
    x |= (x >> 4);
    x |= (x >> 8);
    x &= ~(x >> 16);
    x = (x << 9) - x;
    x = (x << 11) - x;
    x = (x << 14) - x;
    return st_nlz10_table[x >> 26];
}

uint32_t DANAUtility_RoundUp2PoweredSoft(uint32_t val) {
    val--;
    val |= val >> 1;
    val |= val >> 2;
    val |= val >> 4;
    val |= val >> 8;
    val |= val >> 16;
    return val + 1;
}

void DANAUtility_LRtoMSDouble(double **restrict data, uint32_t num_channels, uint32_t num_samples) {
    DANA_Assert(data != NULL && data[0] != NULL && data[1] != NULL && num_channels >= 2);
    DANAUTILITY_UNUSED_ARGUMENT(num_channels);
    double * restrict d0 = data[0];
    double * restrict d1 = data[1];
    #pragma GCC ivdep
    for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
        double mid = (d0[smpl] + d1[smpl]) * 0.5;
        double side = d0[smpl] - d1[smpl];
        d0[smpl] = mid;
        d1[smpl] = side;
    }
}

void DANAUtility_LRtoMSInt32(int32_t **restrict data, uint32_t num_channels, uint32_t num_samples) {
    DANA_Assert(data != NULL && data[0] != NULL && data[1] != NULL && num_channels >= 2);
    DANAUTILITY_UNUSED_ARGUMENT(num_channels);
    int32_t * restrict d0 = data[0];
    int32_t * restrict d1 = data[1];
    #pragma GCC ivdep
    for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
        int32_t left = d0[smpl];
        int32_t right = d1[smpl];
        d0[smpl] = (left + right) >> 1;
        d1[smpl] = left - right;
    }
}

void DANAUtility_MStoLRInt32(int32_t **restrict data, uint32_t num_channels, uint32_t num_samples) {
    DANA_Assert(data != NULL && data[0] != NULL && data[1] != NULL && num_channels >= 2);
    DANAUTILITY_UNUSED_ARGUMENT(num_channels);
    int32_t * restrict d0 = data[0];
    int32_t * restrict d1 = data[1];
    #pragma GCC ivdep
    for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
        int32_t side = d1[smpl];
        int32_t mid = (d0[smpl] << 1) | (side & 1);
        d0[smpl] = (mid + side) >> 1;
        d1[smpl] = (mid - side) >> 1;
    }
}

void DANAUtility_LRtoLSDouble(double **restrict data, uint32_t num_channels, uint32_t num_samples) {
    DANAUTILITY_UNUSED_ARGUMENT(num_channels);
    double * restrict d0 = data[0];
    double * restrict d1 = data[1];
    #pragma GCC ivdep
    for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
        double left = d0[smpl];
        double side = d0[smpl] - d1[smpl];
        d0[smpl] = left;
        d1[smpl] = side;
    }
}

void DANAUtility_LRtoLSInt32(int32_t **restrict data, uint32_t num_channels, uint32_t num_samples) {
    DANAUTILITY_UNUSED_ARGUMENT(num_channels);
    int32_t * restrict d0 = data[0];
    int32_t * restrict d1 = data[1];
    #pragma GCC ivdep
    for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
        int32_t left = d0[smpl];
        int32_t side = d0[smpl] - d1[smpl];
        d0[smpl] = left;
        d1[smpl] = side;
    }
}

void DANAUtility_LStoLRInt32(int32_t **restrict data, uint32_t num_channels, uint32_t num_samples) {
    DANAUTILITY_UNUSED_ARGUMENT(num_channels);
    int32_t * restrict d0 = data[0];
    int32_t * restrict d1 = data[1];
    #pragma GCC ivdep
    for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
        int32_t left = d0[smpl];
        int32_t side = d1[smpl];
        d0[smpl] = left;
        d1[smpl] = left - side;
    }
}

void DANAUtility_LRtoRSDouble(double **restrict data, uint32_t num_channels, uint32_t num_samples) {
    DANAUTILITY_UNUSED_ARGUMENT(num_channels);
    double * restrict d0 = data[0];
    double * restrict d1 = data[1];
    #pragma GCC ivdep
    for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
        double side = d0[smpl] - d1[smpl];
        double right = d1[smpl];
        d0[smpl] = side;
        d1[smpl] = right;
    }
}

void DANAUtility_LRtoRSInt32(int32_t **restrict data, uint32_t num_channels, uint32_t num_samples) {
    DANAUTILITY_UNUSED_ARGUMENT(num_channels);
    int32_t * restrict d0 = data[0];
    int32_t * restrict d1 = data[1];
    #pragma GCC ivdep
    for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
        int32_t side = d0[smpl] - d1[smpl];
        int32_t right = d1[smpl];
        d0[smpl] = side;
        d1[smpl] = right;
    }
}

void DANAUtility_RStoLRInt32(int32_t **restrict data, uint32_t num_channels, uint32_t num_samples) {
    DANAUTILITY_UNUSED_ARGUMENT(num_channels);
    int32_t * restrict d0 = data[0];
    int32_t * restrict d1 = data[1];
    #pragma GCC ivdep
    for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
        int32_t side = d0[smpl];
        int32_t right = d1[smpl];
        d0[smpl] = side + right;
        d1[smpl] = right;
    }
}

double DANAUtility_Round(double d) {
    return (d >= 0.0) ? floor(d + 0.5) : -floor(-d + 0.5);
}

double DANAUtility_Log2(double x) {
    return log(x) * 1.4426950408889634; /* 1 / log(2) */
}

struct DANALESolver* DANALESolver_Create(uint32_t max_dim) {
    struct DANALESolver* lesolver = malloc(sizeof(struct DANALESolver));
    lesolver->max_dim = max_dim;
    lesolver->row_scale = malloc(sizeof(double) * max_dim);
    lesolver->change_index = malloc(sizeof(uint32_t) * max_dim);
    lesolver->x_vec = malloc(sizeof(double) * max_dim);
    lesolver->err_vec = malloc(sizeof(double) * max_dim);
    lesolver->A_lu = malloc(sizeof(double*) * max_dim);
    
    for (uint32_t dim = 0; dim < max_dim; dim++) {
        lesolver->A_lu[dim] = malloc(sizeof(double) * max_dim);
    }
    return lesolver;
}

void DANALESolver_Destroy(struct DANALESolver* lesolver) {
    if (lesolver != NULL) {
        for (uint32_t dim = 0; dim < lesolver->max_dim; dim++) {
            NULLCHECK_AND_FREE(lesolver->A_lu[dim]);
        }
        NULLCHECK_AND_FREE(lesolver->A_lu);
        NULLCHECK_AND_FREE(lesolver->row_scale);
        NULLCHECK_AND_FREE(lesolver->change_index);
        NULLCHECK_AND_FREE(lesolver->x_vec);
        NULLCHECK_AND_FREE(lesolver->err_vec);
        free(lesolver);
    }
}

static int32_t DANALESolver_LUDecomposion(double* const* A, uint32_t dim, uint32_t *change_index, double *row_scale) {
    DANA_Assert(A != NULL && change_index != NULL && row_scale != NULL);
    
    for (uint32_t row = 0; row < dim; row++) {
        double max = 0.0;
        for (uint32_t col = 0; col < dim; col++) {
            if (fabs(A[row][col]) > max) {
                max = fabs(A[row][col]);
            }
        }
        if (fabs(max) <= FLT_EPSILON) return -1;
        row_scale[row] = 1.0 / max;
    }
    
    for (uint32_t col = 0; col < dim; col++) {
        for (uint32_t row = 0; row < col; row++) {
            double sum = A[row][col];
            for (uint32_t k = 0; k < row; k++) {
                sum -= A[row][k] * A[k][col];
            }
            A[row][col] = sum;
        }
        
        double max = 0.0;
        uint32_t max_index = col;
        for (uint32_t row = col; row < dim; row++) {
            double sum = A[row][col];
            for (uint32_t k = 0; k < col; ++k) {
                sum -= A[row][k] * A[k][col];
            }
            A[row][col] = sum;
            
            if ((row_scale[row] * fabs(sum)) >= max) {
                max = (row_scale[row] * fabs(sum));
                max_index = row;
            }
        }
        
        if (col != max_index) {
            for (uint32_t k = 0; k < dim; k++) {
                double tmp = A[max_index][k];
                A[max_index][k] = A[col][k];
                A[col][k] = tmp;
            }
            row_scale[max_index] = row_scale[col];
        }
        change_index[col] = max_index;
        
        if (fabs(A[col][col]) <= FLT_EPSILON) return -1;
        
        if (col != (dim - 1)) {
            double denom_elem = 1.0 / A[col][col];
            for (uint32_t row = col + 1; row < dim; row++) {
                A[row][col] *= denom_elem;
            }
        }
    }
    return 0;
}

static void DANALESolver_LUDecomposionForwardBack(double const* const* A, double* b, uint32_t dim, const uint32_t* change_index) {
    DANA_Assert(A != NULL && b != NULL && change_index != NULL);
    
    uint32_t nonzero_row = 0;
    for (uint32_t row = 0; row < dim; row++) {
        uint32_t pivod = change_index[row];
        double sum = b[pivod];
        b[pivod] = b[row];
        
        if (nonzero_row != 0) {
            for (uint32_t col = nonzero_row; col < row; col++) {
                sum -= A[row][col] * b[col];
            }
        } else if (sum != 0.0) {
            nonzero_row = row;
        }
        b[row] = sum;
    }
    
    for (uint32_t row = dim - 1; row < dim; row--) {
        double sum = b[row];
        for (uint32_t col = row + 1; col < dim; col++) {
            sum -= A[row][col] * b[col];
        }
        b[row] = sum / A[row][row];
        if (row == 0) break;
    }
}

int32_t DANALESolver_Solve(struct DANALESolver* restrict lesolver, const double** restrict A, double* restrict b, uint32_t dim, uint32_t iteration_count) {
    DANA_Assert(lesolver != NULL && A != NULL && b != NULL);
    
    for (uint32_t row = 0; row < dim; row++) {
        memcpy(lesolver->A_lu[row], A[row], sizeof(double) * dim);
    }
    memcpy(lesolver->x_vec, b, sizeof(double) * dim);
    
    if (DANALESolver_LUDecomposion(lesolver->A_lu, dim, lesolver->change_index, lesolver->row_scale) != 0) {
        return -1;
    }
    
    DANALESolver_LUDecomposionForwardBack((double const* const*)lesolver->A_lu, lesolver->x_vec, dim, lesolver->change_index);
    
    for (uint32_t count = 0; count < iteration_count; count++) {
        for (uint32_t row = 0; row < dim; row++) {
            long double error = -b[row];
            for (uint32_t col = 0; col < dim; col++) {
                error += A[row][col] * lesolver->x_vec[col];
            }
            lesolver->err_vec[row] = (double)error;
        }
        DANALESolver_LUDecomposionForwardBack((double const* const*)lesolver->A_lu, lesolver->err_vec, dim, lesolver->change_index);
        for (uint32_t row = 0; row < dim; row++) {
            lesolver->x_vec[row] -= lesolver->err_vec[row];
        }
    }
    memcpy(b, lesolver->x_vec, sizeof(double) * dim);
    return 0;
}

uint32_t DANAUtility_GetDataBitWidth(const int32_t* restrict data, uint32_t num_samples) {
    DANA_Assert(data != NULL);
    uint32_t maxabs = 0;
    for (uint32_t smpl = 0; smpl < num_samples; smpl++) {
        uint32_t abs_val = (uint32_t)DANAUTILITY_ABS(data[smpl]);
        if (abs_val > maxabs) maxabs = abs_val;
    }
    return (maxabs > 0) ? (DANAUTILITY_LOG2CEIL(maxabs) + 1) : 1;
}

struct DANADataPacketQueue* DANADataPacketQueue_Create(uint32_t max_num_packets) {
    struct DANADataPacketQueue* queue = malloc(sizeof(struct DANADataPacketQueue));
    queue->packets = malloc(sizeof(struct DANADataPacket) * max_num_packets);
    queue->max_num_packets = max_num_packets;
    queue->num_free_packets = max_num_packets;
    queue->write_pos = 0;
    queue->read_pos = 0;
    queue->collect_pos = 0;
    
    for (uint32_t pos = 0; pos < queue->max_num_packets; pos++) {
        queue->packets[pos].data = NULL;
        queue->packets[pos].data_size = 0;
        queue->packets[pos].used_size = 0;
    }
    return queue;
}

void DANADataPacketQueue_Destroy(struct DANADataPacketQueue* queue) {
    if (queue != NULL) {
        NULLCHECK_AND_FREE(queue->packets);
        free(queue);
    }
}

DANADataPacketQueueApiResult DANADataPacketQueue_EnqueueDataFragment(struct DANADataPacketQueue* restrict queue, const uint8_t* restrict data, uint32_t data_size) {
    DANA_Assert(queue != NULL && data != NULL);
    if (queue->num_free_packets == 0) return DANA_DATAPACKETQUEUE_APIRESULT_EXCEED_MAX_NUM_DATA_FRAGMENTS;
    if (data_size == 0) return DANA_DATAPACKETQUEUE_APIRESULT_OK;
    
    struct DANADataPacket* packet = &queue->packets[queue->write_pos];
    packet->data = data;
    packet->data_size = data_size;
    packet->used_size = 0;
    
    queue->write_pos = (queue->write_pos + 1) % queue->max_num_packets;
    queue->num_free_packets--;
    return DANA_DATAPACKETQUEUE_APIRESULT_OK;
}

DANADataPacketQueueApiResult DANADataPacketQueue_GetDataFragment(struct DANADataPacketQueue* restrict queue, const uint8_t** restrict data_ptr, uint32_t* restrict data_size, uint32_t max_data_size) {
    DANA_Assert(queue != NULL && data_ptr != NULL && data_size != NULL);
    if (queue->num_free_packets == queue->max_num_packets || max_data_size == 0) {
        return DANA_DATAPACKETQUEUE_APIRESULT_NO_DATA_FRAGMENTS;
    }
    
    struct DANADataPacket* packet = &queue->packets[queue->read_pos];
    if (queue->read_pos == queue->write_pos && packet->data_size == packet->used_size) {
        return DANA_DATAPACKETQUEUE_APIRESULT_NO_DATA_FRAGMENTS;
    }
    
    *data_ptr = &packet->data[packet->used_size];
    *data_size = DANAUTILITY_MIN(max_data_size, packet->data_size - packet->used_size);
    packet->used_size += *data_size;
    
    if (packet->data_size == packet->used_size) {
        queue->read_pos = (queue->read_pos + 1) % queue->max_num_packets;
    }
    return DANA_DATAPACKETQUEUE_APIRESULT_OK;
}

DANADataPacketQueueApiResult DANADataPacketQueue_DequeueDataFragment(struct DANADataPacketQueue* restrict queue, const uint8_t** restrict data_ptr, uint32_t* restrict data_size) {
    DANA_Assert(queue != NULL && data_ptr != NULL && data_size != NULL);
    if (queue->num_free_packets == queue->max_num_packets) return DANA_DATAPACKETQUEUE_APIRESULT_NO_DATA_FRAGMENTS;
    
    struct DANADataPacket* packet = &queue->packets[queue->collect_pos];
    if (packet->used_size == 0) return DANA_DATAPACKETQUEUE_APIRESULT_NO_DATA_FRAGMENTS;
    
    *data_ptr = packet->data;
    *data_size = packet->used_size;
    
    packet->data_size -= packet->used_size;
    packet->data += packet->used_size;
    packet->used_size = 0;
    
    if (packet->data_size == 0) {
        queue->collect_pos = (queue->collect_pos + 1) % queue->max_num_packets;
        queue->num_free_packets++;
    }
    return DANA_DATAPACKETQUEUE_APIRESULT_OK;
}

uint32_t DANADataPacketQueue_GetRemainDataSize(const struct DANADataPacketQueue* queue) {
    DANA_Assert(queue != NULL);
    if (queue->num_free_packets == queue->max_num_packets) return 0;
    
    uint32_t size = 0;
    uint32_t pos = queue->read_pos;
    do {
        const struct DANADataPacket* packet = &queue->packets[pos];
        size += (packet->data_size - packet->used_size);
        pos = (pos + 1) % queue->max_num_packets;
    } while (pos != queue->write_pos);
    return size;
}

char* DANAUtility_StrDup(const char* s) {
    if (!s) return NULL;
    size_t len = strlen(s);
    char* p = malloc(len + 1);
    if (p) { memcpy(p, s, len); p[len] = '\0'; }
    return p;
}

char* DANAUtility_StrNDup(const char* s, size_t n) {
    if (!s) return NULL;
    char* p = malloc(n + 1);
    if (p) { memcpy(p, s, n); p[n] = '\0'; }
    return p;
}

void DANAMetadata_Init(struct DANAMetadata* meta) {
    if (!meta) return;
    memset(meta, 0, sizeof(struct DANAMetadata));
}

void DANAMetadata_Release(struct DANAMetadata* meta) {
    if (!meta) return;
    NULLCHECK_AND_FREE(meta->title);
    NULLCHECK_AND_FREE(meta->artist);
    NULLCHECK_AND_FREE(meta->album);
    NULLCHECK_AND_FREE(meta->year);
    NULLCHECK_AND_FREE(meta->genre);
    NULLCHECK_AND_FREE(meta->track);
    NULLCHECK_AND_FREE(meta->bpm);
    NULLCHECK_AND_FREE(meta->key);
    NULLCHECK_AND_FREE(meta->lyrics);
    NULLCHECK_AND_FREE(meta->cover_data);
    meta->cover_size = 0;
    NULLCHECK_AND_FREE(meta->seek_table);
    meta->seek_table_size = 0;
}