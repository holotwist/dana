#include "DANABitStream.h"
#include "DANAStdint.h"
#include "DANAInternal.h"
#include "DANAUtility.h"

#include <string.h>
#include <stdlib.h>

const uint32_t g_dana_bitstream_lower_bits_mask[33] = {
    0x00000000UL, 0x00000001UL, 0x00000003UL, 0x00000007UL, 0x0000000FUL,
    0x0000001FUL, 0x0000003FUL, 0x0000007FUL, 0x000000FFUL, 0x000001FFUL,
    0x000003FFUL, 0x000007FFUL, 0x00000FFFUL, 0x00001FFFUL, 0x00003FFFUL,
    0x00007FFFUL, 0x0000FFFFUL, 0x0001FFFFUL, 0x0003FFFFUL, 0x0007FFFFUL,
    0x000FFFFFUL, 0x001FFFFFUL, 0x003FFFFFUL, 0x007FFFFFUL, 0x00FFFFFFUL,
    0x01FFFFFFUL, 0x03FFFFFFUL, 0x07FFFFFFUL, 0x0FFFFFFFUL, 0x1FFFFFFFUL,
    0x3FFFFFFFUL, 0x7FFFFFFFUL, 0xFFFFFFFFUL
};

const uint32_t g_dana_bitstream_zerobit_runlength_table[256] = {
    8, 7, 6, 6, 5, 5, 5, 5, 4, 4, 4, 4, 4, 4, 4, 4, 
    3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 3, 
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 
    2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 2, 
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 
    1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 1, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 
    0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0
};

void DANABitReader_Open(struct DANABitStream* restrict stream, uint8_t* restrict memory_image, size_t memory_size) {
    DANA_Assert(stream != NULL && memory_image != NULL);
    stream->flags        = DANABITSTREAM_FLAGS_MODE_READ;
    stream->bit_count    = 0;
    stream->bit_buffer   = 0;
    stream->memory_image = memory_image;
    stream->memory_size  = memory_size;
    stream->memory_p     = memory_image;
}

void DANABitWriter_Open(struct DANABitStream* restrict stream, uint8_t* restrict memory_image, size_t memory_size) {
    DANA_Assert(stream != NULL && memory_image != NULL);
    stream->flags        = 0; /* Clear READ bit */
    stream->bit_count    = 8;
    stream->bit_buffer   = 0;
    stream->memory_image = memory_image;
    stream->memory_size  = memory_size;
    stream->memory_p     = memory_image;
}

void DANABitStream_Close(struct DANABitStream* stream) {
    DANA_Assert(stream != NULL);
    DANABitStream_Flush(stream);
    stream->bit_buffer   = 0;
    stream->memory_image = NULL;
    stream->memory_size  = 0;
    stream->memory_p     = NULL;
    stream->flags        = 0;
}

void DANABitStream_Seek(struct DANABitStream* stream, int32_t offset, int32_t origin) {
    DANA_Assert(stream != NULL);
    DANABitStream_Flush(stream);
    
    uint8_t* pos = NULL;
    switch (origin) {
        case DANABITSTREAM_SEEK_CUR: pos = stream->memory_p; break;
        case DANABITSTREAM_SEEK_SET: pos = (uint8_t *)stream->memory_image; break;
        case DANABITSTREAM_SEEK_END: pos = (uint8_t *)(stream->memory_image + stream->memory_size - 1); break;
        default: DANA_Assert(0);
    }
    
    pos += offset;
    DANA_Assert(pos <= (stream->memory_image + stream->memory_size));
    DANA_Assert(pos >= stream->memory_image);
    stream->memory_p = pos;
}

void DANABitStream_Tell(struct DANABitStream* restrict stream, int32_t* restrict result) {
    DANA_Assert(stream != NULL && result != NULL);
    *result = (int32_t)(stream->memory_p - stream->memory_image);
}

void DANABitWriter_PutBits(struct DANABitStream* restrict stream, uint64_t val, uint32_t nbits) {
    DANA_Assert(stream != NULL);
    DANA_Assert(!(stream->flags & DANABITSTREAM_FLAGS_MODE_READ));
    DANA_Assert(nbits <= 64 && nbits > 0);
    
    while (nbits >= stream->bit_count) {
        nbits -= stream->bit_count;
        stream->bit_buffer |= (uint32_t)DANABITSTREAM_GETLOWERBITS(val >> nbits, stream->bit_count);
        
        if (stream->memory_p >= (stream->memory_image + stream->memory_size)) {
            stream->flags |= DANABITSTREAM_FLAGS_EOS;
            break;
        }
        
        *stream->memory_p++ = (stream->bit_buffer & 0xFF);
        stream->bit_buffer = 0;
        stream->bit_count = 8;
    }
    
    DANA_Assert(nbits <= 8);
    stream->bit_count -= nbits;
    stream->bit_buffer |= (uint32_t)DANABITSTREAM_GETLOWERBITS(val, nbits) << stream->bit_count;
}

void DANABitReader_GetBits(struct DANABitStream* restrict stream, uint64_t* restrict val, uint32_t nbits) {
    DANA_Assert(stream != NULL && val != NULL);
    DANA_Assert(stream->flags & DANABITSTREAM_FLAGS_MODE_READ);
    DANA_Assert(nbits <= 64);
    
    uint64_t tmp = 0;
    while (nbits > stream->bit_count) {
        nbits -= stream->bit_count;
        tmp |= (uint64_t)DANABITSTREAM_GETLOWERBITS(stream->bit_buffer, stream->bit_count) << nbits;
        
        if (stream->memory_p >= (stream->memory_image + stream->memory_size)) {
            stream->flags |= DANABITSTREAM_FLAGS_EOS;
            break;
        }
        
        stream->bit_buffer = *stream->memory_p++;
        stream->bit_count = 8;
    }
    
    stream->bit_count -= nbits;
    tmp |= (uint64_t)DANABITSTREAM_GETLOWERBITS(stream->bit_buffer >> stream->bit_count, nbits);
    *val = tmp;
}

void DANABitReader_GetZeroRunLength(struct DANABitStream* restrict stream, uint32_t* restrict runlength) {
    DANA_Assert(stream != NULL && runlength != NULL);
    
    uint32_t run = DANAUTILITY_NLZ(
        (uint32_t)((stream->bit_buffer << (32 - stream->bit_count)) | (1UL << (31 - stream->bit_count)))
    );
    stream->bit_count -= run;
    
    while (stream->bit_count == 0) {
        if (stream->memory_p >= (stream->memory_image + stream->memory_size)) {
            stream->flags |= DANABITSTREAM_FLAGS_EOS;
            break;
        }
        
        stream->bit_buffer = *stream->memory_p++;
        uint32_t tmp_run = g_dana_bitstream_zerobit_runlength_table[stream->bit_buffer];
        stream->bit_count = 8 - tmp_run;
        run += tmp_run;
    }
    
    stream->bit_count -= 1;
    *runlength = run;
}

void DANABitStream_Flush(struct DANABitStream* stream) {
    DANA_Assert(stream != NULL);
    if (stream->bit_count < 8) {
        if (stream->flags & DANABITSTREAM_FLAGS_MODE_READ) {
            uint64_t dummy;
            DANABitReader_GetBits(stream, &dummy, stream->bit_count);
        } else {
            DANABitWriter_PutBits(stream, 0, stream->bit_count);
        }
    }
}