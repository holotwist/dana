#ifndef DANABITSTREAM_H_INCLUDED
#define DANABITSTREAM_H_INCLUDED

#include "DANAStdint.h"
#include "DANAUtility.h"
#include "DANAInternal.h"
#include <stdio.h>

#define DANABITSTREAM_SEEK_SET  (int32_t)SEEK_SET
#define DANABITSTREAM_SEEK_CUR  (int32_t)SEEK_CUR
#define DANABITSTREAM_SEEK_END  (int32_t)SEEK_END

#define DANABITSTREAM_FLAGS_MODE_READ  (1 << 0)
#define DANABITSTREAM_FLAGS_EOS        (1 << 1)

extern const uint32_t g_dana_bitstream_lower_bits_mask[33];
extern const uint32_t g_dana_bitstream_zerobit_runlength_table[0x100];

static inline uint32_t DANABITSTREAM_GETLOWERBITS(uint64_t val, uint32_t nbits) {
    return (uint32_t)(val & g_dana_bitstream_lower_bits_mask[nbits]);
}

struct DANABitStream {
    uint32_t        bit_buffer;
    uint32_t        bit_count;
    const uint8_t*  memory_image;
    size_t          memory_size;
    uint8_t*        memory_p;
    uint8_t         flags;
};

#ifdef __cplusplus
extern "C" {
#endif

void DANABitWriter_Open(struct DANABitStream* restrict stream, uint8_t* restrict memory_image, size_t memory_size);
void DANABitReader_Open(struct DANABitStream* restrict stream, uint8_t* restrict memory_image, size_t memory_size);
void DANABitStream_Close(struct DANABitStream* stream);
void DANABitStream_Seek(struct DANABitStream* stream, int32_t offset, int32_t origin);
void DANABitStream_Tell(struct DANABitStream* restrict stream, int32_t* restrict result);
void DANABitWriter_PutBits(struct DANABitStream* restrict stream, uint64_t val, uint32_t nbits);
void DANABitReader_GetBits(struct DANABitStream* restrict stream, uint64_t* restrict val, uint32_t nbits);
void DANABitReader_GetZeroRunLength(struct DANABitStream* restrict stream, uint32_t* restrict runlength);
void DANABitStream_Flush(struct DANABitStream* stream);

#ifdef __cplusplus
}
#endif

#endif /* DANABITSTREAM_H_INCLUDED */
