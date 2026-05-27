#include "wav.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <assert.h>

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
            break;
        } else {
            if (WAVParser_Seek(parser, padded_size, SEEK_CUR) != WAV_ERROR_OK) return WAV_ERROR_IO;
        }
    }
    *format = tmp_format;
    return WAV_ERROR_OK;
}

static int32_t WAV_Convert8bitPCMto32bitPCM(int32_t in_8bit)   { return (in_8bit - 128) << 24; }
static int32_t WAV_Convert16bitPCMto32bitPCM(int32_t in_16bit) { return in_16bit << 16; }
static int32_t WAV_Convert24bitPCMto32bitPCM(int32_t in_24bit) { return in_24bit << 8; }
static int32_t WAV_Convert32bitPCMto32bitPCM(int32_t in_32bit) { return in_32bit; }
static int32_t WAV_Convert32bitPCMto8bitPCM(int32_t in_32bit)  { return ((in_32bit >> 24) + 128); }
static int32_t WAV_Convert32bitPCMto16bitPCM(int32_t in_32bit) { return (in_32bit >> 16); }
static int32_t WAV_Convert32bitPCMto24bitPCM(int32_t in_32bit) { return (in_32bit >> 8); }

static WAVError WAVParser_GetWAVPcmData(struct WAVParser* parser, struct WAVFile* wavfile) {
    if (parser == NULL || wavfile == NULL) return WAV_ERROR_INVALID_PARAMETER;

    int32_t (*conv_func)(int32_t) = NULL;
    switch (wavfile->format.bits_per_sample) {
        case 8:  conv_func = WAV_Convert8bitPCMto32bitPCM; break;
        case 16: conv_func = WAV_Convert16bitPCMto32bitPCM; break;
        case 24: conv_func = WAV_Convert24bitPCMto32bitPCM; break;
        case 32: conv_func = WAV_Convert32bitPCMto32bitPCM; break;
        default: return WAV_ERROR_INVALID_FORMAT;
    }

    uint32_t bytes_per_sample = wavfile->format.bits_per_sample / 8;
    for (uint32_t sample = 0; sample < wavfile->format.num_samples; sample++) {
        for (uint32_t ch = 0; ch < wavfile->format.num_channels; ch++) {
            uint64_t bitsbuf;
            if (WAVParser_GetLittleEndianBytes(parser, bytes_per_sample, &bitsbuf) != WAV_ERROR_OK) return WAV_ERROR_IO;
            wavfile->data[ch][sample] = conv_func((int32_t)bitsbuf);
        }
    }
    return WAV_ERROR_OK;
}

WAVApiResult WAV_GetWAVFormatFromFile(const char* filename, struct WAVFileFormat* format) {
    if (!filename || !format) return WAV_APIRESULT_NG;
    FILE* fp = fopen(filename, "rb");
    if (!fp) return WAV_APIRESULT_NG;

    struct WAVParser parser;
    WAVParser_Initialize(&parser, fp);
    WAVApiResult res = (WAVParser_GetWAVFormat(&parser, format) == WAV_ERROR_OK) ? WAV_APIRESULT_OK : WAV_APIRESULT_NG;
    WAVParser_Finalize(&parser);
    fclose(fp);
    return res;
}

struct WAVFile* WAV_CreateFromFile(const char* filename) {
    if (!filename) return NULL;
    FILE* fp = fopen(filename, "rb");
    if (!fp) return NULL;

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
    int32_t (*conv_func)(int32_t) = NULL;
    switch (wavfile->format.bits_per_sample) {
        case 8:  conv_func = WAV_Convert32bitPCMto8bitPCM; break;
        case 16: conv_func = WAV_Convert32bitPCMto16bitPCM; break;
        case 24: conv_func = WAV_Convert32bitPCMto24bitPCM; break;
        case 32: conv_func = WAV_Convert32bitPCMto32bitPCM; break;
        default: return WAV_ERROR_INVALID_FORMAT;
    }

    uint32_t bytes_per_sample = wavfile->format.bits_per_sample / 8;
    for (uint32_t sample = 0; sample < wavfile->format.num_samples; sample++) {
        for (uint32_t ch = 0; ch < wavfile->format.num_channels; ch++) {
            if (WAVWriter_PutLittleEndianBytes(writer, bytes_per_sample, (uint64_t)conv_func(WAVFile_PCM(wavfile, sample, ch))) != WAV_ERROR_OK) {
                return WAV_ERROR_IO;
            }
        }
    }
    return WAV_ERROR_OK;
}

WAVApiResult WAV_WriteToFile(const char* filename, const struct WAVFile* wavfile) {
    if (!filename || !wavfile) return WAV_APIRESULT_INVALID_PARAMETER;
    FILE* fp = fopen(filename, "wb");
    if (!fp) return WAV_APIRESULT_NG;

    struct WAVWriter writer;
    WAVWriter_Initialize(&writer, fp);
    if (WAVWriter_PutWAVHeader(&writer, &wavfile->format) != WAV_ERROR_OK) { WAVWriter_Finalize(&writer); fclose(fp); return WAV_APIRESULT_NG; }
    if (WAVWriter_PutWAVPcmData(&writer, wavfile) != WAV_ERROR_OK) { WAVWriter_Finalize(&writer); fclose(fp); return WAV_APIRESULT_NG; }

    WAVWriter_Finalize(&writer);
    fclose(fp);
    return WAV_APIRESULT_OK;
}