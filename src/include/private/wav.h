#ifndef WAV_INCLUDED
#define WAV_INCLUDED

#include <stdint.h>

typedef int32_t WAVPcmData;

typedef enum {
    WAV_DATA_FORMAT_PCM
} WAVDataFormat;

typedef enum {
    WAV_APIRESULT_OK = 0,
    WAV_APIRESULT_NG,
    WAV_APIRESULT_INVALID_FORMAT,
    WAV_APIRESULT_IOERROR,
    WAV_APIRESULT_INVALID_PARAMETER
} WAVApiResult;

struct WAVFileFormat {
    WAVDataFormat data_format;
    uint32_t      num_channels;
    uint32_t      sampling_rate;
    uint32_t      bits_per_sample;
    uint32_t      num_samples;
};

struct WAVFile {
    struct WAVFileFormat format;
    WAVPcmData**         data;
};

#define WAVFile_PCM(wavfile, samp, ch) ((wavfile)->data[(ch)][(samp)])

#ifdef __cplusplus
extern "C" {
#endif

struct WAVFile* WAV_CreateFromFile(const char* filename);
struct WAVFile* WAV_Create(const struct WAVFileFormat* format);
void WAV_Destroy(struct WAVFile* wavfile);
WAVApiResult WAV_WriteToFile(const char* filename, const struct WAVFile* wavfile);
WAVApiResult WAV_GetWAVFormatFromFile(const char* filename, struct WAVFileFormat* format);

#ifdef __cplusplus
}
#endif

#endif /* WAV_INCLUDED */