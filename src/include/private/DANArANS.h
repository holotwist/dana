#ifndef DANARANS_H_INCLUDED
#define DANARANS_H_INCLUDED

#include "DANAStdint.h"

typedef struct {
    uint32_t state;
} rANS_State;

void DANArANS_EncoderInit(rANS_State* st);
void DANArANS_EncodeSymbol(rANS_State* st, uint32_t start, uint32_t freq, uint32_t scale_bits, uint16_t** out_ptr);
void DANArANS_EncoderFlush(rANS_State* st, uint16_t** out_ptr);

void DANArANS_DecoderInit(rANS_State* st, uint16_t** in_ptr);
uint32_t DANArANS_DecoderGetCount(rANS_State* st, uint32_t scale_bits);
void DANArANS_DecoderAdvanceSymbol(rANS_State* st, uint32_t start, uint32_t freq, uint32_t scale_bits, uint16_t** in_ptr);

#endif /* DANARANS_H_INCLUDED */