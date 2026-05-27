#include "DANArANS.h"

/*
* Unused, maybe useful in future?
*/

#define RANS_L 0x10000 // Lower bound
#define RANS_B 0x10000 // Base (16 bits)

void DANArANS_EncoderInit(rANS_State* st) {
    st->state = RANS_L;
}

void DANArANS_EncodeSymbol(rANS_State* st, uint32_t start, uint32_t freq, uint32_t scale_bits, uint16_t** out_ptr) {
    uint32_t x_max = ((RANS_L >> scale_bits) << 16) * freq;
    if (st->state >= x_max) {
        *out_ptr -= 1;
        **out_ptr = (uint16_t)(st->state & 0xFFFF);
        st->state >>= 16;
    }
    st->state = ((st->state / freq) << scale_bits) + (st->state % freq) + start;
}

void DANArANS_EncoderFlush(rANS_State* st, uint16_t** out_ptr) {
    *out_ptr -= 1;
    **out_ptr = (uint16_t)(st->state >> 16);
    *out_ptr -= 1;
    **out_ptr = (uint16_t)(st->state & 0xFFFF);
}

void DANArANS_DecoderInit(rANS_State* st, uint16_t** in_ptr) {
    st->state = (uint32_t)(**in_ptr);
    *in_ptr += 1;
    st->state |= ((uint32_t)(**in_ptr) << 16);
    *in_ptr += 1;
}

uint32_t DANArANS_DecoderGetCount(rANS_State* st, uint32_t scale_bits) {
    return st->state & ((1u << scale_bits) - 1);
}

void DANArANS_DecoderAdvanceSymbol(rANS_State* st, uint32_t start, uint32_t freq, uint32_t scale_bits, uint16_t** in_ptr) {
    uint32_t mask = (1u << scale_bits) - 1;
    st->state = freq * (st->state >> scale_bits) + (st->state & mask) - start;
    if (st->state < RANS_L) {
        st->state = (st->state << 16) | **in_ptr;
        *in_ptr += 1;
    }
}