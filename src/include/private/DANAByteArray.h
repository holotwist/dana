#ifndef DANA_BYTEARRAY_H_INCLUDED
#define DANA_BYTEARRAY_H_INCLUDED

#include "DANAStdint.h"

static inline uint8_t DANAByteArray_ReadUint8(const uint8_t* p) { return p[0]; }
static inline uint16_t DANAByteArray_ReadUint16(const uint8_t* p) { return (uint16_t)((p[0] << 8) | p[1]); }
static inline uint32_t DANAByteArray_ReadUint32(const uint8_t* p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline void DANAByteArray_GetUint8(const uint8_t** p, uint8_t* out) { *out = DANAByteArray_ReadUint8(*p); *p += 1; }
static inline void DANAByteArray_GetUint16(const uint8_t** p, uint16_t* out) { *out = DANAByteArray_ReadUint16(*p); *p += 2; }
static inline void DANAByteArray_GetUint32(const uint8_t** p, uint32_t* out) { *out = DANAByteArray_ReadUint32(*p); *p += 4; }

static inline void DANAByteArray_WriteUint8(uint8_t* p, uint8_t val) { p[0] = val; }
static inline void DANAByteArray_WriteUint16(uint8_t* p, uint16_t val) { p[0] = (val >> 8) & 0xFF; p[1] = val & 0xFF; }
static inline void DANAByteArray_WriteUint32(uint8_t* p, uint32_t val) {
    p[0] = (val >> 24) & 0xFF;
    p[1] = (val >> 16) & 0xFF;
    p[2] = (val >> 8)  & 0xFF;
    p[3] = val         & 0xFF;
}

static inline void DANAByteArray_PutUint8(uint8_t** p, uint8_t val) { DANAByteArray_WriteUint8(*p, val); *p += 1; }
static inline void DANAByteArray_PutUint16(uint8_t** p, uint16_t val) { DANAByteArray_WriteUint16(*p, val); *p += 2; }
static inline void DANAByteArray_PutUint32(uint8_t** p, uint32_t val) { DANAByteArray_WriteUint32(*p, val); *p += 4; }

#endif /* DANA_BYTEARRAY_H_INCLUDED */