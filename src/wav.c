#include "wav.h"
#include "riff_meta.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>
#include <stdbool.h>

#define WAVBITBUFFER_BUFFER_SIZE (10 * 1024)

typedef enum {
    WAV_ERROR_OK = 0,
    WAV_ERROR_NG,
    WAV_ERROR_IO,
    WAV_ERROR_INVALID_PARAMETER,
    WAV_ERROR_INVALID_FORMAT
} WAVError;

struct WAVBitBuffer {
    uint8_t  bytes[WAVBITBUFFER_BUFFER_SIZE];
    uint32_t bit_count;
    int32_t  byte_pos;
};

struct WAVParser {
    FILE*               fp;
    struct WAVBitBuffer buffer;
};

struct WAVWriter {
    FILE*               fp;
    uint32_t            bit_buffer;
    uint32_t            bit_count;
    struct WAVBitBuffer buffer;
};

static void WAVParser_Initialize(struct WAVParser* parser, FILE* fp) {
    parser->fp = fp;
    memset(&parser->buffer, 0, sizeof(struct WAVBitBuffer));
    parser->buffer.byte_pos = -1;
}

static void WAVParser_Finalize(struct WAVParser* parser) {
    parser->fp = NULL;
    memset(&parser->buffer, 0, sizeof(struct WAVBitBuffer));
    parser->buffer.byte_pos = -1;
}

static WAVError WAVParser_GetBits(struct WAVParser* parser, uint32_t n_bits, uint64_t* bitsbuf) {
    if (parser == NULL || bitsbuf == NULL || n_bits > 64) return WAV_ERROR_INVALID_PARAMETER;

    struct WAVBitBuffer* buf = &parser->buffer;
    if (buf->byte_pos == -1) {
        if (fread(buf->bytes, 1, WAVBITBUFFER_BUFFER_SIZE, parser->fp) == 0) return WAV_ERROR_IO;
        buf->byte_pos = 0;
        buf->bit_count = 8;
    }

    uint64_t tmp = 0;
    while (n_bits > buf->bit_count) {
        n_bits -= buf->bit_count;
        tmp |= (uint64_t)(buf->bytes[buf->byte_pos] & ((1U << buf->bit_count) - 1)) << n_bits;

        buf->byte_pos++;
        buf->bit_count = 8;

        if (buf->byte_pos == WAVBITBUFFER_BUFFER_SIZE) {
            if (fread(buf->bytes, 1, WAVBITBUFFER_BUFFER_SIZE, parser->fp) == 0) return WAV_ERROR_IO;
            buf->byte_pos = 0;
        }
    }

    buf->bit_count -= n_bits;
    tmp |= (uint64_t)((buf->bytes[buf->byte_pos] >> buf->bit_count) & ((1U << n_bits) - 1));

    *bitsbuf = tmp;
    return WAV_ERROR_OK;
}

static WAVError WAVParser_Seek(struct WAVParser* parser, int32_t offset, int32_t wherefrom) {
    if (parser->buffer.byte_pos != -1) {
        offset -= (WAVBITBUFFER_BUFFER_SIZE - (parser->buffer.byte_pos + 1));
    }
    fseek(parser->fp, offset, wherefrom);
    parser->buffer.byte_pos = -1;
    return WAV_ERROR_OK;
}

static WAVError WAVParser_GetLittleEndianBytes(struct WAVParser* parser, uint32_t nbytes, uint64_t* bitsbuf) {
    uint64_t tmp, ret = 0;
    if (WAVParser_GetBits(parser, nbytes * 8, &tmp) != WAV_ERROR_OK) return WAV_ERROR_IO;
    for (uint32_t i = 0; i < nbytes; i++) {
        ret |= ((tmp >> (8 * (nbytes - i - 1))) & 0xFFULL) << (8 * i);
    }
    *bitsbuf = ret;
    return WAV_ERROR_OK;
}

static WAVError WAVParser_GetString(struct WAVParser* parser, char* string_buffer, uint32_t string_length) {
    for (uint32_t i = 0; i < string_length; i++) {
        uint64_t bitsbuf;
        if (WAVParser_GetBits(parser, 8, &bitsbuf) != WAV_ERROR_OK) return WAV_ERROR_IO;
        string_buffer[i] = (char)bitsbuf;
    }
    return WAV_ERROR_OK;
}

static WAVError WAVParser_CheckSignatureString(struct WAVParser* parser, const char* signature, uint32_t signature_length) {
    for (uint32_t i = 0; i < signature_length; i++) {
        uint64_t bitsbuf;
        if (WAVParser_GetBits(parser, 8, &bitsbuf) != WAV_ERROR_OK) return WAV_ERROR_IO;
        if (signature[i] != (char)bitsbuf) return WAV_ERROR_INVALID_FORMAT;
    }
    return WAV_ERROR_OK;
}

static WAVError WAVParser_GetWAVFormat(struct WAVParser* parser, struct WAVFileFormat* format) {
    if (parser == NULL || format == NULL) return WAV_ERROR_INVALID_PARAMETER;
    
    if (WAVParser_CheckSignatureString(parser, "RIFF", 4) != WAV_ERROR_OK) return WAV_ERROR_INVALID_FORMAT;
    
    uint64_t bitsbuf;
    if (WAVParser_GetLittleEndianBytes(parser, 4, &bitsbuf) != WAV_ERROR_OK) return WAV_ERROR_IO;
    if (WAVParser_CheckSignatureString(parser, "WAVE", 4) != WAV_ERROR_OK) return WAV_ERROR_INVALID_FORMAT;

    struct WAVFileFormat tmp_format = {0};
    int found_fmt = 0;

    while (1) {
        char chunk_id[4];
        if (WAVParser_GetString(parser, chunk_id, 4) != WAV_ERROR_OK) return WAV_ERROR_IO;
        if (WAVParser_GetLittleEndianBytes(parser, 4, &bitsbuf) != WAV_ERROR_OK) return WAV_ERROR_IO;
        
        uint32_t chunk_size = (uint32_t)bitsbuf;
        uint32_t padded_size = (chunk_size + 1) & ~1U;

        if (strncmp(chunk_id, "fmt ", 4) == 0) {
            if (chunk_size < 16) return WAV_ERROR_INVALID_FORMAT;
            if (WAVParser_GetLittleEndianBytes(parser, 2, &bitsbuf) != WAV_ERROR_OK) return WAV_ERROR_IO;
            
            uint32_t format_id = (uint32_t)bitsbuf;
            if (format_id != 1 && format_id != 0xFFFE) return WAV_ERROR_INVALID_FORMAT;
            
            tmp_format.data_format = WAV_DATA_FORMAT_PCM;
            if (WAVParser_GetLittleEndianBytes(parser, 2, &bitsbuf) != WAV_ERROR_OK) return WAV_ERROR_IO;
            tmp_format.num_channels = (uint32_t)bitsbuf;
            if (WAVParser_GetLittleEndianBytes(parser, 4, &bitsbuf) != WAV_ERROR_OK) return WAV_ERROR_IO;
            tmp_format.sampling_rate = (uint32_t)bitsbuf;
            if (WAVParser_GetLittleEndianBytes(parser, 4, &bitsbuf) != WAV_ERROR_OK) return WAV_ERROR_IO; /* skip byte/sec */
            if (WAVParser_GetLittleEndianBytes(parser, 2, &bitsbuf) != WAV_ERROR_OK) return WAV_ERROR_IO; /* skip block align */
            if (WAVParser_GetLittleEndianBytes(parser, 2, &bitsbuf) != WAV_ERROR_OK) return WAV_ERROR_IO;
            tmp_format.bits_per_sample = (uint32_t)bitsbuf;

            if (padded_size > 16) {
                if (WAVParser_Seek(parser, padded_size - 16, SEEK_CUR) != WAV_ERROR_OK) return WAV_ERROR_IO;
            }
            found_fmt = 1;
        } else if (strncmp(chunk_id, "data", 4) == 0) {
            if (!found_fmt) return WAV_ERROR_INVALID_FORMAT;
            tmp_format.num_samples = chunk_size / ((tmp_format.bits_per_sample / 8) * tmp_format.num_channels);
            
            long data_start_pos = ftell(parser->fp);
            if (data_start_pos >= 0) {
                long padded_data_size = (long)((chunk_size + 1) & ~1U);
                if (fseek(parser->fp, data_start_pos + padded_data_size, SEEK_SET) == 0) {
                    while (1) {
                        uint8_t post_hdr[8];
                        if (fread(post_hdr, 1, 8, parser->fp) < 8) break;
                        uint32_t post_sz = (uint32_t)post_hdr[4] |
                                           ((uint32_t)post_hdr[5] << 8) |
                                           ((uint32_t)post_hdr[6] << 16) |
                                           ((uint32_t)post_hdr[7] << 24);

                        if (memcmp(post_hdr, "id3 ", 4) == 0 || memcmp(post_hdr, "ID3 ", 4) == 0) {
                            uint8_t* id3_buf = malloc(post_sz);
                            if (id3_buf) {
                                if (fread(id3_buf, 1, post_sz, parser->fp) == post_sz) {
                                    RIFFMeta_ParseId3Chunk(id3_buf, post_sz, &tmp_format.metadata);
                                }
                                free(id3_buf);
                            }
                            if (post_sz & 1) fgetc(parser->fp);
                        } else if (memcmp(post_hdr, "LIST", 4) == 0 && post_sz >= 4) {
                            uint8_t* list_buf = malloc(post_sz);
                            if (list_buf) {
                                if (fread(list_buf, 1, post_sz, parser->fp) == post_sz) {
                                    RIFFMeta_ParseInfoChunk(list_buf, post_sz, &tmp_format.metadata);
                                }
                                free(list_buf);
                            }
                            if (post_sz & 1) fgetc(parser->fp);
                        } else {
                            long skip_sz = (long)((post_sz + 1) & ~1U);
                            if (fseek(parser->fp, skip_sz, SEEK_CUR) != 0) break;
                        }
                    }
                    fseek(parser->fp, data_start_pos, SEEK_SET);
                }
            }
            break;
        } else {
            if (WAVParser_Seek(parser, padded_size, SEEK_CUR) != WAV_ERROR_OK) return WAV_ERROR_IO;
        }
    }
    *format = tmp_format;
    return WAV_ERROR_OK;
}

// Direct conversion handles 8/16/24/32-bit inline

static WAVError WAVParser_GetWAVPcmData(struct WAVParser* parser, struct WAVFile* wavfile) {
    if (parser == NULL || wavfile == NULL) return WAV_ERROR_INVALID_PARAMETER;

    WAVParser_Seek(parser, 0, SEEK_CUR);

    uint32_t num_channels = wavfile->format.num_channels;
    uint32_t num_samples = wavfile->format.num_samples;
    uint32_t bits_per_sample = wavfile->format.bits_per_sample;
    uint32_t bytes_per_sample = bits_per_sample / 8;
    uint32_t frame_bytes = bytes_per_sample * num_channels;
    if (frame_bytes == 0) return WAV_ERROR_INVALID_FORMAT;

    const uint32_t CHUNK_SAMPLES = 16384;
    uint8_t* raw_buf = malloc(CHUNK_SAMPLES * frame_bytes);
    if (!raw_buf) return WAV_ERROR_IO;

    uint32_t sample = 0;
    while (sample < num_samples) {
        uint32_t to_read = (num_samples - sample < CHUNK_SAMPLES) ? (num_samples - sample) : CHUNK_SAMPLES;
        size_t n_read = fread(raw_buf, frame_bytes, to_read, parser->fp);
        if (n_read < to_read) {
            free(raw_buf);
            return WAV_ERROR_IO;
        }

        if (bits_per_sample == 16) {
            const int16_t* src16 = (const int16_t*)raw_buf;
            if (num_channels == 2) {
                int32_t* dst0 = &wavfile->data[0][sample];
                int32_t* dst1 = &wavfile->data[1][sample];
                for (uint32_t s = 0; s < to_read; s++) {
                    dst0[s] = (int32_t)src16[2 * s] << 16;
                    dst1[s] = (int32_t)src16[2 * s + 1] << 16;
                }
            } else {
                for (uint32_t s = 0; s < to_read; s++) {
                    for (uint32_t ch = 0; ch < num_channels; ch++) {
                        wavfile->data[ch][sample + s] = (int32_t)src16[s * num_channels + ch] << 16;
                    }
                }
            }
        } else if (bits_per_sample == 24) {
            const uint8_t* src24 = raw_buf;
            for (uint32_t s = 0; s < to_read; s++) {
                for (uint32_t ch = 0; ch < num_channels; ch++) {
                    uint32_t b0 = *src24++;
                    uint32_t b1 = *src24++;
                    uint32_t b2 = *src24++;
                    wavfile->data[ch][sample + s] = (int32_t)((b2 << 24) | (b1 << 16) | (b0 << 8));
                }
            }
        } else if (bits_per_sample == 32) {
            const int32_t* src32 = (const int32_t*)raw_buf;
            for (uint32_t s = 0; s < to_read; s++) {
                for (uint32_t ch = 0; ch < num_channels; ch++) {
                    wavfile->data[ch][sample + s] = src32[s * num_channels + ch];
                }
            }
        } else if (bits_per_sample == 8) {
            const uint8_t* src8 = raw_buf;
            for (uint32_t s = 0; s < to_read; s++) {
                for (uint32_t ch = 0; ch < num_channels; ch++) {
                    wavfile->data[ch][sample + s] = ((int32_t)*src8++ - 128) << 24;
                }
            }
        } else {
            free(raw_buf);
            return WAV_ERROR_INVALID_FORMAT;
        }

        sample += to_read;
    }

    free(raw_buf);
    return WAV_ERROR_OK;
}

WAVApiResult WAV_GetWAVFormatFromFP(FILE* fp, struct WAVFileFormat* format) {
    if (!fp || !format) return WAV_APIRESULT_INVALID_PARAMETER;

    uint8_t riff_hdr[12];
    if (fread(riff_hdr, 1, 12, fp) < 12) return WAV_APIRESULT_INVALID_FORMAT;
    if (memcmp(riff_hdr, "RIFF", 4) != 0 || memcmp(riff_hdr + 8, "WAVE", 4) != 0) {
        return WAV_APIRESULT_INVALID_FORMAT;
    }

    struct WAVFileFormat tmp = {0};
    tmp.data_format = WAV_DATA_FORMAT_PCM;
    bool found_fmt = false;

    while (1) {
        uint8_t chunk_hdr[8];
        if (fread(chunk_hdr, 1, 8, fp) < 8) return WAV_APIRESULT_INVALID_FORMAT;

        uint32_t chunk_size = (uint32_t)chunk_hdr[4] |
                              ((uint32_t)chunk_hdr[5] << 8) |
                              ((uint32_t)chunk_hdr[6] << 16) |
                              ((uint32_t)chunk_hdr[7] << 24);

        if (memcmp(chunk_hdr, "fmt ", 4) == 0) {
            if (chunk_size < 16) return WAV_APIRESULT_INVALID_FORMAT;
            uint8_t fmt_buf[16];
            if (fread(fmt_buf, 1, 16, fp) < 16) return WAV_APIRESULT_INVALID_FORMAT;

            uint16_t fmt_id = (uint16_t)fmt_buf[0] | ((uint16_t)fmt_buf[1] << 8);
            if (fmt_id != 1 && fmt_id != 0xFFFE) return WAV_APIRESULT_INVALID_FORMAT;

            tmp.num_channels    = (uint32_t)fmt_buf[2] | ((uint32_t)fmt_buf[3] << 8);
            tmp.sampling_rate   = (uint32_t)fmt_buf[4] | ((uint32_t)fmt_buf[5] << 8) |
                                  ((uint32_t)fmt_buf[6] << 16) | ((uint32_t)fmt_buf[7] << 24);
            tmp.bits_per_sample = (uint32_t)fmt_buf[14] | ((uint32_t)fmt_buf[15] << 8);
            found_fmt = true;

            uint32_t remaining = chunk_size - 16;
            if (chunk_size & 1) remaining++;
            while (remaining > 0) {
                uint8_t skip[256];
                uint32_t n = (remaining < sizeof(skip)) ? remaining : sizeof(skip);
                if (fread(skip, 1, n, fp) < n) return WAV_APIRESULT_IOERROR;
                remaining -= n;
            }
        } else if (memcmp(chunk_hdr, "id3 ", 4) == 0 || memcmp(chunk_hdr, "ID3 ", 4) == 0) {
            uint8_t* id3_buf = malloc(chunk_size);
            if (id3_buf) {
                if (fread(id3_buf, 1, chunk_size, fp) == chunk_size) {
                    RIFFMeta_ParseId3Chunk(id3_buf, chunk_size, &tmp.metadata);
                }
                free(id3_buf);
            }
            if (chunk_size & 1) fgetc(fp);
        } else if (memcmp(chunk_hdr, "LIST", 4) == 0 && chunk_size >= 4) {
            uint8_t* list_buf = malloc(chunk_size);
            if (list_buf) {
                if (fread(list_buf, 1, chunk_size, fp) == chunk_size) {
                    RIFFMeta_ParseInfoChunk(list_buf, chunk_size, &tmp.metadata);
                }
                free(list_buf);
            }
            if (chunk_size & 1) fgetc(fp);
        } else if (memcmp(chunk_hdr, "data", 4) == 0) {
            if (!found_fmt) return WAV_APIRESULT_INVALID_FORMAT;
            uint32_t frame_bytes = (tmp.bits_per_sample / 8) * tmp.num_channels;
            if (frame_bytes == 0) return WAV_APIRESULT_INVALID_FORMAT;
            tmp.num_samples = chunk_size / frame_bytes;

            // Check if stream is seekable
            long data_start_pos = ftell(fp);
            if (data_start_pos >= 0) {
                // Seek past PCM data chunk to scan trailing id3/LIST metadata
                long padded_data_size = (long)((chunk_size + 1) & ~1U);
                if (fseek(fp, data_start_pos + padded_data_size, SEEK_SET) == 0) {
                    while (1) {
                        uint8_t post_hdr[8];
                        if (fread(post_hdr, 1, 8, fp) < 8) break;
                        uint32_t post_sz = (uint32_t)post_hdr[4] |
                                           ((uint32_t)post_hdr[5] << 8) |
                                           ((uint32_t)post_hdr[6] << 16) |
                                           ((uint32_t)post_hdr[7] << 24);

                        if (memcmp(post_hdr, "id3 ", 4) == 0 || memcmp(post_hdr, "ID3 ", 4) == 0) {
                            uint8_t* id3_buf = malloc(post_sz);
                            if (id3_buf) {
                                if (fread(id3_buf, 1, post_sz, fp) == post_sz) {
                                    RIFFMeta_ParseId3Chunk(id3_buf, post_sz, &tmp.metadata);
                                }
                                free(id3_buf);
                            }
                            if (post_sz & 1) fgetc(fp);
                        } else if (memcmp(post_hdr, "LIST", 4) == 0 && post_sz >= 4) {
                            uint8_t* list_buf = malloc(post_sz);
                            if (list_buf) {
                                if (fread(list_buf, 1, post_sz, fp) == post_sz) {
                                    RIFFMeta_ParseInfoChunk(list_buf, post_sz, &tmp.metadata);
                                }
                                free(list_buf);
                            }
                            if (post_sz & 1) fgetc(fp);
                        } else {
                            long skip_sz = (long)((post_sz + 1) & ~1U);
                            if (fseek(fp, skip_sz, SEEK_CUR) != 0) break;
                        }
                    }
                    // Restore file position to beginning of PCM data for decoding
                    fseek(fp, data_start_pos, SEEK_SET);
                } else {
                    fseek(fp, data_start_pos, SEEK_SET);
                }
            }

            *format = tmp;
            return WAV_APIRESULT_OK;
        } else {
            uint32_t remaining = (chunk_size + 1) & ~1U;
            while (remaining > 0) {
                uint8_t skip[256];
                uint32_t n = (remaining < sizeof(skip)) ? remaining : sizeof(skip);
                if (fread(skip, 1, n, fp) < n) return WAV_APIRESULT_IOERROR;
                remaining -= n;
            }
        }
    }
}

WAVApiResult WAV_GetWAVFormatFromFile(const char* filename, struct WAVFileFormat* format) {
    if (!filename || !format) return WAV_APIRESULT_NG;
    FILE* fp = fopen(filename, "rb");
    if (!fp) return WAV_APIRESULT_NG;
    WAVApiResult res = WAV_GetWAVFormatFromFP(fp, format);
    fclose(fp);
    return res;
}

struct WAVFile* WAV_CreateFromFile(const char* filename) {
    if (!filename) return NULL;
    FILE* fp = fopen(filename, "rb");
    if (!fp) return NULL;
    setvbuf(fp, NULL, _IOFBF, 256 * 1024);

    struct WAVParser parser;
    WAVParser_Initialize(&parser, fp);

    struct WAVFileFormat format;
    if (WAVParser_GetWAVFormat(&parser, &format) != WAV_ERROR_OK) {
        WAVParser_Finalize(&parser); fclose(fp); return NULL;
    }

    struct WAVFile* wavfile = WAV_Create(&format);
    if (!wavfile) {
        WAVParser_Finalize(&parser); fclose(fp); return NULL;
    }

    if (WAVParser_GetWAVPcmData(&parser, wavfile) != WAV_ERROR_OK) {
        WAV_Destroy(wavfile); WAVParser_Finalize(&parser); fclose(fp); return NULL;
    }

    WAVParser_Finalize(&parser);
    fclose(fp);
    return wavfile;
}

struct WAVFile* WAV_Create(const struct WAVFileFormat* format) {
    if (!format || format->data_format != WAV_DATA_FORMAT_PCM) return NULL;

    struct WAVFile* wavfile = malloc(sizeof(struct WAVFile));
    if (!wavfile) return NULL;

    wavfile->format = *format;
    wavfile->data = malloc(sizeof(WAVPcmData*) * format->num_channels);
    if (!wavfile->data) { free(wavfile); return NULL; }

    for (uint32_t ch = 0; ch < format->num_channels; ch++) {
        wavfile->data[ch] = calloc(format->num_samples, sizeof(WAVPcmData));
        if (!wavfile->data[ch]) { WAV_Destroy(wavfile); return NULL; }
    }
    return wavfile;
}

void WAV_Destroy(struct WAVFile* wavfile) {
    if (wavfile) {
        DANAMetadata_Release(&wavfile->format.metadata);
        if (wavfile->data) {
            for (uint32_t ch = 0; ch < wavfile->format.num_channels; ch++) {
                if (wavfile->data[ch]) free(wavfile->data[ch]);
            }
            free(wavfile->data);
        }
        free(wavfile);
    }
}

static void WAVWriter_Initialize(struct WAVWriter* writer, FILE* fp) {
    writer->fp = fp;
    writer->bit_count = 8;
    writer->bit_buffer = 0;
    memset(&writer->buffer, 0, sizeof(struct WAVBitBuffer));
    writer->buffer.byte_pos = 0;
}

static WAVError WAVWriter_PutBits(struct WAVWriter* writer, uint64_t val, uint32_t n_bits) {
    if (!writer) return WAV_ERROR_INVALID_PARAMETER;
    while (n_bits >= writer->bit_count) {
        n_bits -= writer->bit_count;
        writer->bit_buffer |= (uint8_t)((val >> n_bits) & ((1U << writer->bit_count) - 1));
        writer->buffer.bytes[writer->buffer.byte_pos++] = (uint8_t)(writer->bit_buffer & 0xFF);

        if (writer->buffer.byte_pos == WAVBITBUFFER_BUFFER_SIZE) {
            if (fwrite(writer->buffer.bytes, 1, WAVBITBUFFER_BUFFER_SIZE, writer->fp) < WAVBITBUFFER_BUFFER_SIZE) return WAV_ERROR_IO;
            writer->buffer.byte_pos = 0;
        }
        writer->bit_buffer = 0;
        writer->bit_count = 8;
    }
    writer->bit_count -= n_bits;
    writer->bit_buffer |= (uint8_t)((val & ((1U << n_bits) - 1)) << writer->bit_count);
    return WAV_ERROR_OK;
}

static WAVError WAVWriter_PutLittleEndianBytes(struct WAVWriter* writer, uint32_t nbytes, uint64_t data) {
    uint64_t out = 0;
    for (uint32_t i = 0; i < nbytes; i++) {
        out |= ((data >> (8 * (nbytes - i - 1))) & 0xFFULL) << (8 * i);
    }
    return WAVWriter_PutBits(writer, out, nbytes * 8);
}

static WAVError WAVWriter_Flush(struct WAVWriter* writer) {
    if (!writer) return WAV_ERROR_INVALID_PARAMETER;
    if (writer->bit_count != 8) {
        if (WAVWriter_PutBits(writer, 0, writer->bit_count) != WAV_ERROR_OK) return WAV_ERROR_IO;
        writer->bit_buffer = 0;
        writer->bit_count = 8;
    }
    if (fwrite(writer->buffer.bytes, 1, writer->buffer.byte_pos, writer->fp) < (size_t)writer->buffer.byte_pos) return WAV_ERROR_IO;
    writer->buffer.byte_pos = 0;
    return WAV_ERROR_OK;
}

static void WAVWriter_Finalize(struct WAVWriter* writer) {
    WAVWriter_Flush(writer);
    writer->fp = NULL;
    writer->bit_count = 8;
    writer->bit_buffer = 0;
    memset(&writer->buffer, 0, sizeof(struct WAVBitBuffer));
    writer->buffer.byte_pos = 0;
}

static WAVError WAVWriter_PutWAVHeader(struct WAVWriter* writer, const struct WAVFileFormat* format) {
    if (!writer || !format || format->data_format != WAV_DATA_FORMAT_PCM) return WAV_ERROR_INVALID_FORMAT;

    uint32_t pcm_data_size = format->num_samples * (format->bits_per_sample / 8) * format->num_channels;
    uint32_t filesize = pcm_data_size + 44;

    WAVWriter_PutBits(writer, 'R', 8); WAVWriter_PutBits(writer, 'I', 8); WAVWriter_PutBits(writer, 'F', 8); WAVWriter_PutBits(writer, 'F', 8);
    WAVWriter_PutLittleEndianBytes(writer, 4, filesize - 8);
    WAVWriter_PutBits(writer, 'W', 8); WAVWriter_PutBits(writer, 'A', 8); WAVWriter_PutBits(writer, 'V', 8); WAVWriter_PutBits(writer, 'E', 8);
    
    WAVWriter_PutBits(writer, 'f', 8); WAVWriter_PutBits(writer, 'm', 8); WAVWriter_PutBits(writer, 't', 8); WAVWriter_PutBits(writer, ' ', 8);
    WAVWriter_PutLittleEndianBytes(writer, 4, 16);
    WAVWriter_PutLittleEndianBytes(writer, 2, 1);
    WAVWriter_PutLittleEndianBytes(writer, 2, format->num_channels);
    WAVWriter_PutLittleEndianBytes(writer, 4, format->sampling_rate);
    WAVWriter_PutLittleEndianBytes(writer, 4, format->sampling_rate * (format->bits_per_sample / 8) * format->num_channels);
    WAVWriter_PutLittleEndianBytes(writer, 2, (format->bits_per_sample / 8) * format->num_channels);
    WAVWriter_PutLittleEndianBytes(writer, 2, format->bits_per_sample);
    
    WAVWriter_PutBits(writer, 'd', 8); WAVWriter_PutBits(writer, 'a', 8); WAVWriter_PutBits(writer, 't', 8); WAVWriter_PutBits(writer, 'a', 8);
    WAVWriter_PutLittleEndianBytes(writer, 4, pcm_data_size);

    return WAV_ERROR_OK;
}

static WAVError WAVWriter_PutWAVPcmData(struct WAVWriter* writer, const struct WAVFile* wavfile) {
    if (!writer || !wavfile) return WAV_ERROR_INVALID_PARAMETER;

    WAVWriter_Flush(writer);

    uint32_t num_channels = wavfile->format.num_channels;
    uint32_t num_samples = wavfile->format.num_samples;
    uint32_t bits_per_sample = wavfile->format.bits_per_sample;
    uint32_t bytes_per_sample = bits_per_sample / 8;
    uint32_t frame_bytes = bytes_per_sample * num_channels;
    if (frame_bytes == 0) return WAV_ERROR_INVALID_FORMAT;

    const uint32_t CHUNK_SAMPLES = 16384;
    uint8_t* raw_buf = malloc(CHUNK_SAMPLES * frame_bytes);
    if (!raw_buf) return WAV_ERROR_IO;

    uint32_t sample = 0;
    while (sample < num_samples) {
        uint32_t to_write = (num_samples - sample < CHUNK_SAMPLES) ? (num_samples - sample) : CHUNK_SAMPLES;

        if (bits_per_sample == 16) {
            int16_t* dst16 = (int16_t*)raw_buf;
            if (num_channels == 2) {
                const int32_t* src0 = &wavfile->data[0][sample];
                const int32_t* src1 = &wavfile->data[1][sample];
                for (uint32_t s = 0; s < to_write; s++) {
                    dst16[2 * s]     = (int16_t)(src0[s] >> 16);
                    dst16[2 * s + 1] = (int16_t)(src1[s] >> 16);
                }
            } else {
                for (uint32_t s = 0; s < to_write; s++) {
                    for (uint32_t ch = 0; ch < num_channels; ch++) {
                        dst16[s * num_channels + ch] = (int16_t)(wavfile->data[ch][sample + s] >> 16);
                    }
                }
            }
        } else if (bits_per_sample == 24) {
            uint8_t* dst24 = raw_buf;
            for (uint32_t s = 0; s < to_write; s++) {
                for (uint32_t ch = 0; ch < num_channels; ch++) {
                    int32_t val = wavfile->data[ch][sample + s] >> 8;
                    *dst24++ = (uint8_t)(val & 0xFF);
                    *dst24++ = (uint8_t)((val >> 8) & 0xFF);
                    *dst24++ = (uint8_t)((val >> 16) & 0xFF);
                }
            }
        } else if (bits_per_sample == 32) {
            int32_t* dst32 = (int32_t*)raw_buf;
            for (uint32_t s = 0; s < to_write; s++) {
                for (uint32_t ch = 0; ch < num_channels; ch++) {
                    dst32[s * num_channels + ch] = wavfile->data[ch][sample + s];
                }
            }
        } else if (bits_per_sample == 8) {
            uint8_t* dst8 = raw_buf;
            for (uint32_t s = 0; s < to_write; s++) {
                for (uint32_t ch = 0; ch < num_channels; ch++) {
                    *dst8++ = (uint8_t)((wavfile->data[ch][sample + s] >> 24) + 128);
                }
            }
        } else {
            free(raw_buf);
            return WAV_ERROR_INVALID_FORMAT;
        }

        if (fwrite(raw_buf, frame_bytes, to_write, writer->fp) < to_write) {
            free(raw_buf);
            return WAV_ERROR_IO;
        }

        sample += to_write;
    }

    free(raw_buf);
    return WAV_ERROR_OK;
}

WAVApiResult WAV_WriteToFile(const char* filename, const struct WAVFile* wavfile) {
    if (!filename || !wavfile) return WAV_APIRESULT_INVALID_PARAMETER;
    FILE* fp = fopen(filename, "wb");
    if (!fp) return WAV_APIRESULT_NG;
    setvbuf(fp, NULL, _IOFBF, 256 * 1024);

    struct WAVWriter writer;
    WAVWriter_Initialize(&writer, fp);
    if (WAVWriter_PutWAVHeader(&writer, &wavfile->format) != WAV_ERROR_OK) { WAVWriter_Finalize(&writer); fclose(fp); return WAV_APIRESULT_NG; }
    if (WAVWriter_PutWAVPcmData(&writer, wavfile) != WAV_ERROR_OK) { WAVWriter_Finalize(&writer); fclose(fp); return WAV_APIRESULT_NG; }

    WAVWriter_Finalize(&writer);
    fclose(fp);
    return WAV_APIRESULT_OK;
}

WAVApiResult WAV_WriteWAVHeaderToFP(FILE* fp, const struct WAVFileFormat* format) {
    if (!fp || !format) return WAV_APIRESULT_INVALID_PARAMETER;
    struct WAVWriter writer;
    WAVWriter_Initialize(&writer, fp);
    WAVError err = WAVWriter_PutWAVHeader(&writer, format);
    WAVWriter_Finalize(&writer);
    return (err == WAV_ERROR_OK) ? WAV_APIRESULT_OK : WAV_APIRESULT_NG;
}

WAVApiResult WAV_WriteMetadataToFP(FILE* fp, const struct DANAMetadata* meta, bool legacy_info) {
    if (!fp || !meta) return WAV_APIRESULT_INVALID_PARAMETER;

    if (legacy_info) {
        uint32_t info_size = 0;
        uint8_t* info_chunk = RIFFMeta_BuildInfoChunk(meta, &info_size);
        if (info_chunk && info_size > 0) {
            fwrite("LIST", 1, 4, fp);
            uint8_t sz_buf[4] = {
                (uint8_t)(info_size & 0xFF),
                (uint8_t)((info_size >> 8) & 0xFF),
                (uint8_t)((info_size >> 16) & 0xFF),
                (uint8_t)((info_size >> 24) & 0xFF)
            };
            fwrite(sz_buf, 1, 4, fp);
            fwrite(info_chunk, 1, info_size, fp);
            if (info_size & 1) fputc(0, fp);
            free(info_chunk);
        }
    }

    uint32_t id3_size = 0;
    uint8_t* id3_chunk = RIFFMeta_BuildId3Chunk(meta, &id3_size);
    if (id3_chunk && id3_size > 0) {
        fwrite("id3 ", 1, 4, fp);
        uint8_t sz_buf[4] = {
            (uint8_t)(id3_size & 0xFF),
            (uint8_t)((id3_size >> 8) & 0xFF),
            (uint8_t)((id3_size >> 16) & 0xFF),
            (uint8_t)((id3_size >> 24) & 0xFF)
        };
        fwrite(sz_buf, 1, 4, fp);
        fwrite(id3_chunk, 1, id3_size, fp);
        if (id3_size & 1) fputc(0, fp);
        free(id3_chunk);
    }

    return WAV_APIRESULT_OK;
}