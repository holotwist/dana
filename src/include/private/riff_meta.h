#ifndef RIFF_META_H_INCLUDED
#define RIFF_META_H_INCLUDED

#include "DANAStdint.h"
#include "DANA.h"
#include <stdio.h>

#ifdef __cplusplus
extern "C" {
#endif

void RIFFMeta_ParseInfoChunk(const uint8_t* data, uint32_t size, struct DANAMetadata* meta);
void RIFFMeta_ParseId3Chunk(const uint8_t* data, uint32_t size, struct DANAMetadata* meta);

uint8_t* RIFFMeta_BuildInfoChunk(const struct DANAMetadata* meta, uint32_t* out_size);
uint8_t* RIFFMeta_BuildId3Chunk(const struct DANAMetadata* meta, uint32_t* out_size);

#ifdef __cplusplus
}
#endif

#endif /* RIFF_META_H_INCLUDED */