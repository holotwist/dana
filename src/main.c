#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "DANAEncoder.h"
#include "DANADecoder.h"
#include "DANAInternal.h"
#include "wav.h"
#include "command_line_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>

// Should go in utility?
#define MIN(a, b) (((a) < (b)) ? (a) : (b))

static struct CommandLineParserSpecification command_line_spec[] = {
    { 'e', "encode", COMMAND_LINE_PARSER_FALSE, "Encode mode", NULL, COMMAND_LINE_PARSER_FALSE },
    { 'd', "decode", COMMAND_LINE_PARSER_FALSE, "Decode mode", NULL, COMMAND_LINE_PARSER_FALSE },
    { 'm', "mode", COMMAND_LINE_PARSER_TRUE, "Specify compress mode: 0(fast decode), ..., 4(high compression) default:2", NULL, COMMAND_LINE_PARSER_FALSE },
    { 'x', "hybrid", COMMAND_LINE_PARSER_TRUE, "Use Dana Hybrid mode and specify bit shift (e.g. 6) to create .dahl & .dahc", NULL, COMMAND_LINE_PARSER_FALSE },
    { 'p', "verpose", COMMAND_LINE_PARSER_FALSE, "Verpose mode(try to display all information)", NULL, COMMAND_LINE_PARSER_FALSE },
    { 'q', "quiet", COMMAND_LINE_PARSER_FALSE, "Quiet mode(suppress outputs)", NULL, COMMAND_LINE_PARSER_FALSE },
    { 'c', "crc-check", COMMAND_LINE_PARSER_TRUE, "Whether to check CRC16 at decoding(yes or no) default:yes", NULL, COMMAND_LINE_PARSER_FALSE },
    { 'z', "seek-table", COMMAND_LINE_PARSER_TRUE, "Enable seek table generation (yes or no) default:yes", NULL, COMMAND_LINE_PARSER_FALSE },
    { 'h', "help", COMMAND_LINE_PARSER_FALSE, "Show command help message", NULL, COMMAND_LINE_PARSER_FALSE },
    { 'v', "version", COMMAND_LINE_PARSER_FALSE, "Show version information", NULL, COMMAND_LINE_PARSER_FALSE },
    { 's', "streaming", COMMAND_LINE_PARSER_FALSE, "Use streaming decode(for debug; 120fps)", NULL, COMMAND_LINE_PARSER_FALSE },
    { 0, "title", COMMAND_LINE_PARSER_TRUE, "Set Title", NULL, COMMAND_LINE_PARSER_FALSE },
    { 0, "artist", COMMAND_LINE_PARSER_TRUE, "Set Artist", NULL, COMMAND_LINE_PARSER_FALSE },
    { 0, "album", COMMAND_LINE_PARSER_TRUE, "Set Album", NULL, COMMAND_LINE_PARSER_FALSE },
    { 0, "year", COMMAND_LINE_PARSER_TRUE, "Set Year", NULL, COMMAND_LINE_PARSER_FALSE },
    { 0, "genre", COMMAND_LINE_PARSER_TRUE, "Set Genre", NULL, COMMAND_LINE_PARSER_FALSE },
    { 0, "track", COMMAND_LINE_PARSER_TRUE, "Set Track number", NULL, COMMAND_LINE_PARSER_FALSE },
    { 0, "bpm", COMMAND_LINE_PARSER_TRUE, "Set BPM", NULL, COMMAND_LINE_PARSER_FALSE },
    { 0, "key", COMMAND_LINE_PARSER_TRUE, "Set Key", NULL, COMMAND_LINE_PARSER_FALSE },
    { 0, "lyrics", COMMAND_LINE_PARSER_TRUE, "Set Lyrics", NULL, COMMAND_LINE_PARSER_FALSE },
    { 0, "cover", COMMAND_LINE_PARSER_TRUE, "Set Cover image file path", NULL, COMMAND_LINE_PARSER_FALSE },
    { 0, }
};

static const struct DANAEncodeParameter encode_preset[] = {
    {  8, 1, 4, DANA_CHPROCESSMETHOD_NONE,      DANA_WINDOWFUNCTIONTYPE_RECTANGULAR,  4096 },
    {  8, 1, 8, DANA_CHPROCESSMETHOD_STEREO_MS, DANA_WINDOWFUNCTIONTYPE_TUKEY,       12288 },
    { 16, 1, 8, DANA_CHPROCESSMETHOD_STEREO_MS, DANA_WINDOWFUNCTIONTYPE_TUKEY,       12288 },
    { 32, 3, 8, DANA_CHPROCESSMETHOD_STEREO_MS, DANA_WINDOWFUNCTIONTYPE_TUKEY,       12288 },
    { 40, 3, 8, DANA_CHPROCESSMETHOD_STEREO_MS, DANA_WINDOWFUNCTIONTYPE_TUKEY,       16384 }
};

static const uint32_t num_encode_preset = sizeof(encode_preset) / sizeof(encode_preset[0]);
static const uint32_t default_preset_no = 2;

static int do_encode(const char* in_filename, const char* out_filename, uint32_t encode_preset_no, uint8_t verpose_flag, int hybrid_shift, uint8_t enable_seek_table) {
    struct DANAEncoderConfig config = {
        .max_num_channels = 8,
        .max_num_block_samples = 16384,
        .max_parcor_order = 48,
        .max_longterm_order = 5,
        .max_lms_order_per_filter = 40,
        .verpose_flag = verpose_flag,
        .enable_seek_table = enable_seek_table
    };

    struct DANAEncoder* encoder = DANAEncoder_Create(&config);
    if (!encoder) {
        fprintf(stderr, "Failed to create encoder handle.\n");
        return 1;
    }

    struct WAVFile* in_wav = WAV_CreateFromFile(in_filename);
    if (!in_wav) {
        fprintf(stderr, "Failed to open %s\n", in_filename);
        DANAEncoder_Destroy(encoder);
        return 1;
    }

    struct DANAWaveFormat wave_format = {
        .num_channels = in_wav->format.num_channels,
        .bit_per_sample = in_wav->format.bits_per_sample,
        .sampling_rate = in_wav->format.sampling_rate
    };
    
    if (DANAEncoder_SetWaveFormat(encoder, &wave_format) != DANA_APIRESULT_OK) {
        fprintf(stderr, "Failed to set wave parameter.\n");
        return 1;
    }

    const struct DANAEncodeParameter* ppreset = &encode_preset[encode_preset_no];
    struct DANAEncodeParameter enc_param = *ppreset;
    if (in_wav->format.num_channels == 2 && ppreset->ch_process_method == DANA_CHPROCESSMETHOD_STEREO_MS) {
        enc_param.ch_process_method = DANA_CHPROCESSMETHOD_STEREO_MS;
    } else {
        enc_param.ch_process_method = DANA_CHPROCESSMETHOD_NONE;
    }

    if (DANAEncoder_SetEncodeParameter(encoder, &enc_param) != DANA_APIRESULT_OK) {
        fprintf(stderr, "Failed to set encode parameter.\n");
        return 1;
    }

    struct DANAMetadata meta;
    DANAMetadata_Init(&meta);
    if (CommandLineParser_GetOptionAcquired(command_line_spec, "title")) meta.title = (char*)CommandLineParser_GetArgumentString(command_line_spec, "title");
    if (CommandLineParser_GetOptionAcquired(command_line_spec, "artist")) meta.artist = (char*)CommandLineParser_GetArgumentString(command_line_spec, "artist");
    if (CommandLineParser_GetOptionAcquired(command_line_spec, "album")) meta.album = (char*)CommandLineParser_GetArgumentString(command_line_spec, "album");
    if (CommandLineParser_GetOptionAcquired(command_line_spec, "year")) meta.year = (char*)CommandLineParser_GetArgumentString(command_line_spec, "year");
    if (CommandLineParser_GetOptionAcquired(command_line_spec, "genre")) meta.genre = (char*)CommandLineParser_GetArgumentString(command_line_spec, "genre");
    if (CommandLineParser_GetOptionAcquired(command_line_spec, "track")) meta.track = (char*)CommandLineParser_GetArgumentString(command_line_spec, "track");
    if (CommandLineParser_GetOptionAcquired(command_line_spec, "bpm")) meta.bpm = (char*)CommandLineParser_GetArgumentString(command_line_spec, "bpm");
    if (CommandLineParser_GetOptionAcquired(command_line_spec, "key")) meta.key = (char*)CommandLineParser_GetArgumentString(command_line_spec, "key");
    if (CommandLineParser_GetOptionAcquired(command_line_spec, "lyrics")) meta.lyrics = (char*)CommandLineParser_GetArgumentString(command_line_spec, "lyrics");
    
    if (CommandLineParser_GetOptionAcquired(command_line_spec, "cover")) {
        const char* cover_path = CommandLineParser_GetArgumentString(command_line_spec, "cover");
        FILE* f = fopen(cover_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long file_size = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (file_size > 20 * 1024 * 1024) {
                fprintf(stderr, "Warning: Cover image %s is too large (%.2f MB). Max allowed is 20 MB. Skipping cover.\n", cover_path, (double)file_size / (1024 * 1024));
            } else {
                meta.cover_size = (uint32_t)file_size;
                meta.cover_data = malloc(meta.cover_size);
                if (meta.cover_data) fread(meta.cover_data, 1, meta.cover_size, f);
            }
            fclose(f);
        } else {
            fprintf(stderr, "Warning: Could not open cover image %s\n", cover_path);
        }
    }

    if (DANAEncoder_SetMetadata(encoder, &meta) != DANA_APIRESULT_OK) {
        fprintf(stderr, "Failed to set metadata.\n");
    }
    
    struct stat fstat;
    stat(in_filename, &fstat);
    uint32_t buffer_size = (uint32_t)(2 * fstat.st_size) + meta.cover_size + (1024 * 1024);
    uint8_t* buffer = malloc(buffer_size);

    if (hybrid_shift > 0) {
        char file_dahl[1024], file_dahc[1024];
        strcpy(file_dahl, out_filename);
        char* dot = strrchr(file_dahl, '.');
        if (dot && (strcmp(dot, ".dahl") == 0 || strcmp(dot, ".dahc") == 0 || strcmp(dot, ".dana") == 0)) *dot = '\0';
        strcpy(file_dahc, file_dahl);
        strcat(file_dahl, ".dahl"); strcat(file_dahc, ".dahc");

        struct WAVFile* lossy_wav = WAV_Create(&in_wav->format);
        struct WAVFile* corr_wav = WAV_Create(&in_wav->format);

        uint32_t physical_shift = (32 - in_wav->format.bits_per_sample) + hybrid_shift;
        int32_t offset = (1 << (physical_shift - 1));

        for (uint32_t smpl = 0; smpl < in_wav->format.num_samples; smpl++) {
            for (uint32_t ch = 0; ch < in_wav->format.num_channels; ch++) {
                int32_t orig = in_wav->data[ch][smpl];
                int64_t lossy64 = (((int64_t)orig + offset) >> physical_shift) << physical_shift;
                if (lossy64 > 2147483647LL) lossy64 = ((2147483647LL) >> physical_shift) << physical_shift;
                if (lossy64 < -2147483648LL) lossy64 = ((-2147483648LL) >> physical_shift) << physical_shift;
                lossy_wav->data[ch][smpl] = (int32_t)lossy64;
                corr_wav->data[ch][smpl] = orig - (int32_t)lossy64;
            }
        }

        uint32_t enc_size = 0;
        DANAApiResult ret = DANAEncoder_EncodeWhole(encoder, (const int32_t* const*)lossy_wav->data, lossy_wav->format.num_samples, buffer, buffer_size, &enc_size);
        if (ret == DANA_APIRESULT_OK) {
            FILE* fp = fopen(file_dahl, "wb");
            if (fp) { fwrite(buffer, 1, enc_size, fp); fclose(fp); }
        }

        DANAEncoder_Destroy(encoder);
        encoder = DANAEncoder_Create(&config);
        DANAEncoder_SetWaveFormat(encoder, &wave_format);
        DANAEncoder_SetEncodeParameter(encoder, &enc_param);
        DANAEncoder_SetMetadata(encoder, &meta);

        ret = DANAEncoder_EncodeWhole(encoder, (const int32_t* const*)corr_wav->data, corr_wav->format.num_samples, buffer, buffer_size, &enc_size);
        if (ret == DANA_APIRESULT_OK) {
            FILE* fp = fopen(file_dahc, "wb");
            if (fp) { fwrite(buffer, 1, enc_size, fp); fclose(fp); }
        }

        if (verpose_flag) printf("Dana Hybrid created %s and %s\n", file_dahl, file_dahc);
        WAV_Destroy(lossy_wav); WAV_Destroy(corr_wav);
    } else {
        uint32_t encoded_data_size = 0;
        DANAApiResult ret = DANAEncoder_EncodeWhole(encoder, (const int32_t* const*)in_wav->data, in_wav->format.num_samples, buffer, buffer_size, &encoded_data_size);
        if (ret != DANA_APIRESULT_OK) {
            fprintf(stderr, "Encoding error! %d\n", ret);
            free(buffer); WAV_Destroy(in_wav); DANAEncoder_Destroy(encoder);
            return 1;
        }

        FILE* out_fp = fopen(out_filename, "wb");
        if (out_fp) { fwrite(buffer, 1, encoded_data_size, out_fp); fclose(out_fp); }

        if (verpose_flag) printf("Encode success! size: %u -> %u\n", (uint32_t)fstat.st_size, encoded_data_size);
    }

    if (meta.cover_data) free(meta.cover_data);
    free(buffer);
    WAV_Destroy(in_wav);
    DANAEncoder_Destroy(encoder);
    return 0;
}

static int do_decode(const char* in_filename, const char* out_filename, uint8_t enable_crc_check, uint8_t verpose_flag) {
    struct DANADecoderConfig config = {
        .max_num_channels = 8,
        .max_num_block_samples = 16384,
        .max_parcor_order = 48,
        .max_longterm_order = 5,
        .max_lms_order_per_filter = 40,
        .enable_crc_check = enable_crc_check,
        .verpose_flag = verpose_flag
    };

    struct DANADecoder* decoder = DANADecoder_Create(&config);
    if (!decoder) {
        fprintf(stderr, "Failed to create decoder handle.\n");
        return 1;
    }

    FILE* in_fp = fopen(in_filename, "rb");
    if (!in_fp) {
        DANADecoder_Destroy(decoder);
        return 1;
    }

    struct stat fstat;
    stat(in_filename, &fstat);
    uint32_t buffer_size = (uint32_t)fstat.st_size;
    uint8_t* buffer = malloc(buffer_size);
    fread(buffer, 1, buffer_size, in_fp);
    fclose(in_fp);

    struct DANAHeaderInfo header = {0};
    uint32_t parsed_header_size = 0;
    if (DANADecoder_DecodeHeader(buffer, buffer_size, &header, &parsed_header_size) != DANA_APIRESULT_OK) {
        fprintf(stderr, "Failed to get header information.\n");
        free(buffer);
        DANADecoder_Destroy(decoder);
        return 1;
    }

    // Why verpose instead of verbose?, able to use -p
    if (verpose_flag) {
        if (header.metadata.title) printf("Title: %s\n", header.metadata.title);
        if (header.metadata.artist) printf("Artist: %s\n", header.metadata.artist);
        printf("Num Channels:                %u\n", header.wave_format.num_channels);
        printf("Bit Per Sample:              %u\n", header.wave_format.bit_per_sample);
        printf("Sampling Rate:               %u\n", header.wave_format.sampling_rate);
        if (header.metadata.seek_table) printf("Seek Table:                  Available (%u bytes)\n", header.metadata.seek_table_size);
    }

    struct WAVFileFormat wav_format = {
        .data_format = WAV_DATA_FORMAT_PCM,
        .num_channels = header.wave_format.num_channels,
        .sampling_rate = header.wave_format.sampling_rate,
        .bits_per_sample = header.wave_format.bit_per_sample,
        .num_samples = header.num_samples
    };

    struct WAVFile* out_wav = WAV_Create(&wav_format);
    if (!out_wav) {
        fprintf(stderr, "Failed to create wav handle.\n");
        free(buffer);
        DANADecoder_Destroy(decoder);
        return 1;
    }

    DANADecoder_SetWaveFormat(decoder, &header.wave_format);
    DANADecoder_SetEncodeParameter(decoder, &header.encode_param);

    uint32_t decode_num_samples = 0;
    if (DANADecoder_DecodeWhole(decoder, buffer, buffer_size, (int32_t**)out_wav->data, out_wav->format.num_samples, &decode_num_samples) != DANA_APIRESULT_OK) {
        fprintf(stderr, "Decoding error!\n");
        free(buffer); WAV_Destroy(out_wav); DANADecoder_Destroy(decoder);
        return 1;
    }

    int hybrid_mode = 0;
    char dahc_filepath[1024];
    size_t len = strlen(in_filename);
    if (len > 5 && strcasecmp(in_filename + len - 5, ".dahl") == 0) {
        strcpy(dahc_filepath, in_filename);
        strcpy(dahc_filepath + len - 5, ".dahc");
        FILE* fp_corr = fopen(dahc_filepath, "rb");
        if (fp_corr) { hybrid_mode = 1; fclose(fp_corr); }
    }

    if (hybrid_mode) {
        FILE* fp_corr = fopen(dahc_filepath, "rb");
        if (fp_corr) {
            struct stat fstat_corr;
            stat(dahc_filepath, &fstat_corr);
            uint32_t buf_size_corr = (uint32_t)fstat_corr.st_size;
            uint8_t* buf_corr = malloc(buf_size_corr);
            fread(buf_corr, 1, buf_size_corr, fp_corr);
            fclose(fp_corr);

            struct DANADecoder* dec_corr = DANADecoder_Create(&config);
            struct DANAHeaderInfo hdr_corr = {0};
            uint32_t parsed_hdr_corr = 0;
            if (DANADecoder_DecodeHeader(buf_corr, buf_size_corr, &hdr_corr, &parsed_hdr_corr) == DANA_APIRESULT_OK) {
                struct WAVFile* wav_corr = WAV_Create(&wav_format);
                DANADecoder_SetWaveFormat(dec_corr, &hdr_corr.wave_format);
                DANADecoder_SetEncodeParameter(dec_corr, &hdr_corr.encode_param);
                uint32_t dec_samples_corr = 0;
                if (DANADecoder_DecodeWhole(dec_corr, buf_corr, buf_size_corr, (int32_t**)wav_corr->data, wav_corr->format.num_samples, &dec_samples_corr) == DANA_APIRESULT_OK) {
                    for (uint32_t ch = 0; ch < out_wav->format.num_channels; ch++) {
                        for (uint32_t s = 0; s < dec_samples_corr && s < decode_num_samples; s++) {
                            out_wav->data[ch][s] += wav_corr->data[ch][s];
                        }
                    }
                    if (verpose_flag) printf("Applied hybrid lossless corrections from %s\n", dahc_filepath);
                }
                WAV_Destroy(wav_corr);
                DANAMetadata_Release(&hdr_corr.metadata);
            }
            DANADecoder_Destroy(dec_corr);
            free(buf_corr);
        }
    }

    if (WAV_WriteToFile(out_filename, out_wav) != WAV_APIRESULT_OK) {
        fprintf(stderr, "Failed to write wav file.\n");
    }

    free(buffer);
    WAV_Destroy(out_wav);
    DANADecoder_Destroy(decoder);
    DANAMetadata_Release(&header.metadata);
    return 0;
}

static int do_streaming_decode(const char* in_filename, const char* out_filename, uint8_t enable_crc_check, uint8_t verpose_flag) {
    struct DANAStreamingDecoderConfig streaming_config = {
        .core_config = {
            .max_num_channels = 8,
            .max_num_block_samples = 16384,
            .max_parcor_order = 48,
            .max_longterm_order = 5,
            .max_lms_order_per_filter = 40,
            .enable_crc_check = enable_crc_check,
            .verpose_flag = verpose_flag
        },
        .decode_interval_hz = 120.0f,
        .max_bit_per_sample = 24
    };

    struct DANAStreamingDecoder* decoder = DANAStreamingDecoder_Create(&streaming_config);
    if (!decoder) {
        fprintf(stderr, "Failed to create streaming decoder handle.\n");
        return 1;
    }

    FILE* in_fp = fopen(in_filename, "rb");
    if (!in_fp) {
        DANAStreamingDecoder_Destroy(decoder);
        return 1;
    }

    struct stat fstat;
    stat(in_filename, &fstat);
    uint32_t buffer_size = (uint32_t)fstat.st_size;
    uint8_t* buffer = malloc(buffer_size);
    fread(buffer, 1, buffer_size, in_fp);
    fclose(in_fp);

    struct DANAHeaderInfo header = {0};
    uint32_t parsed_header_size = 0;
    if (DANADecoder_DecodeHeader(buffer, buffer_size, &header, &parsed_header_size) != DANA_APIRESULT_OK) {
        fprintf(stderr, "Failed to get header information.\n");
        free(buffer);
        DANAStreamingDecoder_Destroy(decoder);
        return 1;
    }

    struct WAVFileFormat wav_format = {
        .data_format = WAV_DATA_FORMAT_PCM,
        .num_channels = header.wave_format.num_channels,
        .sampling_rate = header.wave_format.sampling_rate,
        .bits_per_sample = header.wave_format.bit_per_sample,
        .num_samples = header.num_samples
    };

    struct WAVFile* out_wav = WAV_Create(&wav_format);
    if (!out_wav) {
        fprintf(stderr, "Failed to create wav handle.\n");
        free(buffer);
        DANAStreamingDecoder_Destroy(decoder);
        return 1;
    }

    DANAStreamingDecoder_SetWaveFormat(decoder, &header.wave_format);
    DANAStreamingDecoder_SetEncodeParameter(decoder, &header.encode_param);

    uint32_t sample_progress = 0;
    uint32_t data_progress = parsed_header_size;
    
    while (sample_progress < header.num_samples) {
        uint32_t estimate_min_data_size = (sample_progress == 0) ? header.max_block_size : 0;
        if (sample_progress > 0) DANAStreamingDecoder_EstimateMinimumNessesaryDataSize(decoder, &estimate_min_data_size);
        
        uint32_t put_data_size = MIN(estimate_min_data_size, buffer_size - data_progress);
        DANAStreamingDecoder_AppendDataFragment(decoder, &buffer[data_progress], put_data_size);

        int32_t* output_ptr[DANA_MAX_CHANNELS];
        for (uint32_t ch = 0; ch < header.wave_format.num_channels; ch++) {
            output_ptr[ch] = &out_wav->data[ch][sample_progress];
        }
        
        uint32_t tmp_output_samples = 0;
        DANAApiResult ret = DANAStreamingDecoder_Decode(decoder, output_ptr, header.num_samples - sample_progress, &tmp_output_samples);
        if (ret != DANA_APIRESULT_OK) {
            fprintf(stderr, "Streaming Decode failed! ret:%d\n", ret);
            free(buffer);
            WAV_Destroy(out_wav);
            DANAStreamingDecoder_Destroy(decoder);
            return 1;
        }

        const uint8_t* dummy_out_ptr;
        uint32_t dummy_out_size;
        DANAStreamingDecoder_CollectDataFragment(decoder, &dummy_out_ptr, &dummy_out_size);

        data_progress += put_data_size;
        sample_progress += tmp_output_samples;

        if (verpose_flag) {
            printf("progress: %4.1f %%\r", ((double)sample_progress / header.num_samples) * 100.0);
            fflush(stdout);
        }
    }

    if (WAV_WriteToFile(out_filename, out_wav) != WAV_APIRESULT_OK) {
        fprintf(stderr, "Failed to write wav file.\n");
    }

    free(buffer);
    WAV_Destroy(out_wav);
    DANAStreamingDecoder_Destroy(decoder);
    DANAMetadata_Release(&header.metadata);
    return 0;
}

static void print_usage(char** argv) {
    printf("Usage: %s [options] INPUT_FILE_NAME OUTPUT_FILE_NAME\n", argv[0]);
}

static void print_version_info(void) {
    printf("DANA - Dana Audio Non-lossy Archive Version %s\n", DANA_VERSION_STRING);
}

int main(int argc, char** argv) {
    const char* filename_ptr[2] = { NULL, NULL };
    uint8_t verbose_flag = 1;

    if (argc == 1) {
        print_usage(argv);
        return 1;
    }

    if (CommandLineParser_ParseArguments(command_line_spec, argc, argv, filename_ptr, 2) != COMMAND_LINE_PARSER_RESULT_OK) {
        return 1;
    }

    if (CommandLineParser_GetOptionAcquired(command_line_spec, "help")) {
        print_usage(argv);
        printf("options:\n");
        CommandLineParser_PrintDescription(command_line_spec);
        return 0;
    } else if (CommandLineParser_GetOptionAcquired(command_line_spec, "version")) {
        print_version_info();
        return 0;
    }

    const char* input_file = filename_ptr[0];
    const char* output_file = filename_ptr[1];

    if (!input_file || !output_file) {
        fprintf(stderr, "%s: input and output files must be specified.\n", argv[0]);
        return 1;
    }

    // I did the following, then, needed to check it
    if (CommandLineParser_GetOptionAcquired(command_line_spec, "decode") && CommandLineParser_GetOptionAcquired(command_line_spec, "encode")) {
        fprintf(stderr, "%s: encode and decode mode cannot be specified simultaneously.\n", argv[0]);
        return 1;
    }

    if (CommandLineParser_GetOptionAcquired(command_line_spec, "verpose")) verbose_flag = 1;
    else if (CommandLineParser_GetOptionAcquired(command_line_spec, "quiet")) verbose_flag = 0;

    if (CommandLineParser_GetOptionAcquired(command_line_spec, "decode")) {
        uint8_t enable_crc_check = 1;
        if (CommandLineParser_GetOptionAcquired(command_line_spec, "crc-check")) {
            const char* crc_check_arg = CommandLineParser_GetArgumentString(command_line_spec, "crc-check");
            enable_crc_check = (strcmp(crc_check_arg, "yes") == 0) ? 1 : 0;
        }

        if (CommandLineParser_GetOptionAcquired(command_line_spec, "streaming")) {
            if (do_streaming_decode(input_file, output_file, enable_crc_check, verbose_flag) != 0) {
                fprintf(stderr, "%s: failed to streaming decode %s.\n", argv[0], input_file);
                return 1;
            }
        } else {
            if (do_decode(input_file, output_file, enable_crc_check, verbose_flag) != 0) {
                fprintf(stderr, "%s: failed to decode %s.\n", argv[0], input_file);
                return 1;
            }
        }
    } else if (CommandLineParser_GetOptionAcquired(command_line_spec, "encode")) {
        uint32_t encode_preset_no = default_preset_no;
        int hybrid_shift = 0;

        if (CommandLineParser_GetOptionAcquired(command_line_spec, "mode")) {
            encode_preset_no = (uint32_t)strtol(CommandLineParser_GetArgumentString(command_line_spec, "mode"), NULL, 10);
            if (encode_preset_no >= num_encode_preset) {
                fprintf(stderr, "%s: encode preset number is out of range.\n", argv[0]);
                return 1;
            }
        }

        if (CommandLineParser_GetOptionAcquired(command_line_spec, "hybrid")) {
            hybrid_shift = atoi(CommandLineParser_GetArgumentString(command_line_spec, "hybrid"));
            if (hybrid_shift < 1 || hybrid_shift >= 32) {
                fprintf(stderr, "Invalid hybrid shift value: %d\n", hybrid_shift);
                return 1;
            }
        }
        
        uint8_t enable_seek_table = 1;
        if (CommandLineParser_GetOptionAcquired(command_line_spec, "seek-table")) {
            const char* arg = CommandLineParser_GetArgumentString(command_line_spec, "seek-table");
            enable_seek_table = (strcmp(arg, "yes") == 0) ? 1 : 0;
        }

        if (do_encode(input_file, output_file, encode_preset_no, verbose_flag, hybrid_shift, enable_seek_table) != 0) {
            return 1;
        }
    } else {
        fprintf(stderr, "%s: decode(-d) or encode(-e) option must be specified.\n", argv[0]);
        return 1;
    }

    return 0;
}