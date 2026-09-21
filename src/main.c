#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "DANAEncoder.h"
#include "DANADecoder.h"
#include "DANAInternal.h"
#include "DANAByteArray.h"
#include "DANAUtility.h"
#include "wav.h"
#include "command_line_parser.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <pthread.h>
#include <stdatomic.h>
#include <unistd.h>

#define MIN(a, b) (((a) < (b)) ? (a) : (b))
#define STREAM_RING_SLOTS 16

static struct CommandLineParserSpecification command_line_spec[] = {
    { 'e', "encode", COMMAND_LINE_PARSER_FALSE, "Encode mode", NULL, COMMAND_LINE_PARSER_FALSE },
    { 'd', "decode", COMMAND_LINE_PARSER_FALSE, "Decode mode", NULL, COMMAND_LINE_PARSER_FALSE },
    { 'm', "mode", COMMAND_LINE_PARSER_TRUE, "Specify compress mode: 0(fast decode), ..., 4(high compression) default:2", NULL, COMMAND_LINE_PARSER_FALSE },
    { 'x', "hybrid", COMMAND_LINE_PARSER_TRUE, "Use Dana Hybrid mode and specify bit shift (e.g. 6) to create .dahl & .dahc", NULL, COMMAND_LINE_PARSER_FALSE },
    { 'p', "verpose", COMMAND_LINE_PARSER_FALSE, "Verpose mode(try to display all information)", NULL, COMMAND_LINE_PARSER_FALSE },
    { 'q', "quiet", COMMAND_LINE_PARSER_FALSE, "Quiet mode(suppress outputs)", NULL, COMMAND_LINE_PARSER_FALSE },
    { 'c', "crc-check", COMMAND_LINE_PARSER_TRUE, "Whether to check CRC16 at decoding(yes or no) default:yes", NULL, COMMAND_LINE_PARSER_FALSE },
    { 'z', "seek-table", COMMAND_LINE_PARSER_TRUE, "Enable seek table generation (yes or no) default:yes", NULL, COMMAND_LINE_PARSER_FALSE },
    { 't', "threads", COMMAND_LINE_PARSER_TRUE, "Number of worker threads (default: auto)", NULL, COMMAND_LINE_PARSER_FALSE },
    { 'h', "help", COMMAND_LINE_PARSER_FALSE, "Show command help message", NULL, COMMAND_LINE_PARSER_FALSE },
    { 'v', "version", COMMAND_LINE_PARSER_FALSE, "Show version information", NULL, COMMAND_LINE_PARSER_FALSE },
    { 's', "streaming", COMMAND_LINE_PARSER_FALSE, "Use streaming decode(for debug; 120fps)", NULL, COMMAND_LINE_PARSER_FALSE },
    { 0, "legacy-wav", COMMAND_LINE_PARSER_FALSE, "Include RIFF LIST-INFO along with id3 when decoding to WAV", NULL, COMMAND_LINE_PARSER_FALSE },
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
    {  8, 0, 4, DANA_CHPROCESSMETHOD_STEREO_MS, DANA_WINDOWFUNCTIONTYPE_TUKEY,        4096, 0 },
    {  8, 0, 8, DANA_CHPROCESSMETHOD_STEREO_MS, DANA_WINDOWFUNCTIONTYPE_TUKEY,        8192, 0 },
    { 16, 1, 8, DANA_CHPROCESSMETHOD_STEREO_MS, DANA_WINDOWFUNCTIONTYPE_TUKEY,       12288, 1 },
    { 32, 3, 8, DANA_CHPROCESSMETHOD_STEREO_MS, DANA_WINDOWFUNCTIONTYPE_TUKEY,       12288, 2 },
    { 40, 3, 8, DANA_CHPROCESSMETHOD_STEREO_MS, DANA_WINDOWFUNCTIONTYPE_TUKEY,       16384, 2 }
};

static const uint32_t num_encode_preset = sizeof(encode_preset) / sizeof(encode_preset[0]);
static const uint32_t default_preset_no = 2;

enum SlotState { SLOT_EMPTY = 0, SLOT_QUEUED, SLOT_PROCESSING, SLOT_READY };

typedef struct {
    int32_t* pcm[DANA_MAX_CHANNELS];
    uint32_t num_samples;
    uint32_t seq_id;
    uint32_t start_sample;
    uint8_t* compressed;
    uint32_t compressed_size;
    uint32_t num_blocks;
    uint32_t max_block_size;
    uint32_t max_bps;
    DANAApiResult res;
    int state;
} StreamSlot;

typedef struct {
    struct DANAWaveFormat wave_format;
    struct DANAEncodeParameter enc_param;
    StreamSlot slots[STREAM_RING_SLOTS];
    pthread_mutex_t mutex;
    pthread_cond_t cond_empty;
    pthread_cond_t cond_queued;
    pthread_cond_t cond_ready;
    bool reader_finished;
    atomic_int error_code;
    uint32_t queued_count;
} StreamPipeContext;

static inline void put_fixed_varint(uint8_t** p, uint32_t val) {
    **p = (val & 0x7F) | 0x80; *p += 1; val >>= 7;
    **p = (val & 0x7F) | 0x80; *p += 1; val >>= 7;
    **p = (val & 0x7F) | 0x80; *p += 1; val >>= 7;
    **p = (val & 0x7F) | 0x80; *p += 1; val >>= 7;
    **p = (val & 0x7F);        *p += 1;
}

static void* stream_encode_worker(void* arg) {
    StreamPipeContext* ctx = (StreamPipeContext*)arg;

    struct DANAEncoderConfig cfg = {
        .max_num_channels         = ctx->wave_format.num_channels,
        .max_num_block_samples    = ctx->enc_param.max_num_block_samples,
        .max_parcor_order         = ctx->enc_param.parcor_order,
        .max_longterm_order       = ctx->enc_param.longterm_order,
        .max_lms_order_per_filter = ctx->enc_param.lms_order_per_filter,
        .verpose_flag             = 0,
        .enable_seek_table        = 0,
        .num_threads              = 1
    };

    struct DANAEncoder* enc = DANAEncoder_Create(&cfg);
    if (!enc) return NULL;
    DANAEncoder_SetWaveFormat(enc, &ctx->wave_format);
    DANAEncoder_SetEncodeParameter(enc, &ctx->enc_param);

    uint32_t max_buf = DANA_CalculateSufficientBlockSize(ctx->wave_format.num_channels, ctx->enc_param.max_num_block_samples, ctx->wave_format.bit_per_sample) + 8192;
    uint8_t* tmp_buf = malloc(max_buf);

    while (atomic_load(&ctx->error_code) == 0) {
        pthread_mutex_lock(&ctx->mutex);
        int slot_idx = -1;
        while (!ctx->reader_finished || ctx->queued_count > 0) {
            for (int i = 0; i < STREAM_RING_SLOTS; i++) {
                if (ctx->slots[i].state == SLOT_QUEUED) {
                    slot_idx = i;
                    ctx->slots[i].state = SLOT_PROCESSING;
                    ctx->queued_count--;
                    break;
                }
            }
            if (slot_idx != -1 || (ctx->reader_finished && ctx->queued_count == 0)) break;
            pthread_cond_wait(&ctx->cond_queued, &ctx->mutex);
        }
        pthread_mutex_unlock(&ctx->mutex);

        if (slot_idx == -1) break;

        StreamSlot* s = &ctx->slots[slot_idx];
        const int32_t* input_ptr[DANA_MAX_CHANNELS];
        for (uint32_t ch = 0; ch < ctx->wave_format.num_channels; ch++) input_ptr[ch] = s->pcm[ch];

        uint32_t bsize = 0;
        DANAApiResult ret = DANAEncoder_EncodeBlock(enc, input_ptr, s->num_samples, tmp_buf, max_buf, &bsize);
        s->res = ret;
        if (ret == DANA_APIRESULT_OK) {
            memcpy(s->compressed, tmp_buf, bsize);
            s->compressed_size = bsize;
            s->num_blocks = 1;
            s->max_block_size = bsize;
            s->max_bps = (8 * bsize * ctx->wave_format.sampling_rate) / s->num_samples;
        } else {
            atomic_store(&ctx->error_code, ret);
        }

        pthread_mutex_lock(&ctx->mutex);
        s->state = SLOT_READY;
        pthread_cond_signal(&ctx->cond_ready);
        pthread_mutex_unlock(&ctx->mutex);
    }

    free(tmp_buf);
    DANAEncoder_Destroy(enc);
    return NULL;
}

static int do_encode(const char* in_filename, const char* out_filename, uint32_t encode_preset_no, uint8_t verpose_flag, int hybrid_shift, uint8_t enable_seek_table) {
    (void)hybrid_shift;
    (void)enable_seek_table;
    bool is_in_pipe = (strcmp(in_filename, "-") == 0);
    bool is_out_pipe = (strcmp(out_filename, "-") == 0);

    FILE* in_fp = is_in_pipe ? stdin : fopen(in_filename, "rb");
    if (!in_fp) { fprintf(stderr, "Failed to open input: %s\n", in_filename); return 1; }
    setvbuf(in_fp, NULL, _IOFBF, 256 * 1024);

    struct WAVFileFormat wav_fmt;
    if (WAV_GetWAVFormatFromFP(in_fp, &wav_fmt) != WAV_APIRESULT_OK) {
        if (!is_in_pipe) fclose(in_fp);
        return 1;
    }

    FILE* out_fp = is_out_pipe ? stdout : fopen(out_filename, "wb");
    if (!out_fp) { if (!is_in_pipe) fclose(in_fp); return 1; }
    setvbuf(out_fp, NULL, _IOFBF, 256 * 1024);

    const struct DANAEncodeParameter* ppreset = &encode_preset[encode_preset_no];
    struct DANAEncodeParameter enc_param = *ppreset;
    if (wav_fmt.num_channels == 2 && ppreset->ch_process_method == DANA_CHPROCESSMETHOD_STEREO_MS) {
        enc_param.ch_process_method = DANA_CHPROCESSMETHOD_STEREO_MS;
    } else {
        enc_param.ch_process_method = DANA_CHPROCESSMETHOD_NONE;
    }

    struct DANAWaveFormat wave_fmt = {
        .num_channels   = wav_fmt.num_channels,
        .bit_per_sample = wav_fmt.bits_per_sample,
        .sampling_rate  = wav_fmt.sampling_rate,
        .offset_lshift  = 0
    };

    struct DANAHeaderInfo header;
    memset(&header, 0, sizeof(header));
    header.wave_format  = wave_fmt;
    header.encode_param = enc_param;
    header.num_samples  = wav_fmt.num_samples;

    DANAMetadata_Copy(&header.metadata, &wav_fmt.metadata);
    DANAMetadata_Release(&wav_fmt.metadata);

    // CLI overrides take top priority
#define OVERRIDE_CLI_TAG(opt, field) \
    if (CommandLineParser_GetOptionAcquired(command_line_spec, opt)) { \
        if (header.metadata.field) free(header.metadata.field); \
        header.metadata.field = DANAUtility_StrDup(CommandLineParser_GetArgumentString(command_line_spec, opt)); \
    }
    OVERRIDE_CLI_TAG("title",  title);
    OVERRIDE_CLI_TAG("artist", artist);
    OVERRIDE_CLI_TAG("album",  album);
    OVERRIDE_CLI_TAG("year",   year);
    OVERRIDE_CLI_TAG("genre",  genre);
    OVERRIDE_CLI_TAG("track",  track);
    OVERRIDE_CLI_TAG("bpm",    bpm);
    OVERRIDE_CLI_TAG("key",    key);
    OVERRIDE_CLI_TAG("lyrics", lyrics);
#undef OVERRIDE_CLI_TAG

    if (CommandLineParser_GetOptionAcquired(command_line_spec, "cover")) {
        const char* cover_path = CommandLineParser_GetArgumentString(command_line_spec, "cover");
        FILE* f = fopen(cover_path, "rb");
        if (f) {
            fseek(f, 0, SEEK_END);
            long file_size = ftell(f);
            fseek(f, 0, SEEK_SET);
            if (file_size > 20 * 1024 * 1024) {
                fprintf(stderr, "Warning: Cover image %s is too large (%.2f MB). Max is 20 MB. Skipping.\n", cover_path, (double)file_size / (1024 * 1024));
            } else if (file_size > 0) {
                if (header.metadata.cover_data) free(header.metadata.cover_data);
                header.metadata.cover_size = (uint32_t)file_size;
                header.metadata.cover_data = malloc(header.metadata.cover_size);
                if (header.metadata.cover_data) {
                    fread(header.metadata.cover_data, 1, header.metadata.cover_size, f);
                }
            }
            fclose(f);
        } else {
            fprintf(stderr, "Warning: Could not open cover image %s\n", cover_path);
        }
    }

    // Pre-reserve exact seek table size for disk files
    uint32_t num_seek_points = 0;
    uint32_t sktb_size = 0;
    uint8_t* sktb_buf = NULL;
    uint32_t* seek_samples = NULL;
    uint32_t* seek_offsets = NULL;

    if (!is_out_pipe && enable_seek_table && wav_fmt.num_samples != DANA_NUM_SAMPLES_INVALID && wav_fmt.sampling_rate > 0) {
        num_seek_points = (wav_fmt.num_samples / wav_fmt.sampling_rate) + 1;
        sktb_size = 4 + (num_seek_points * 10);
        sktb_buf = calloc(1, sktb_size);
        seek_samples = malloc(sizeof(uint32_t) * (num_seek_points + 1));
        seek_offsets = malloc(sizeof(uint32_t) * (num_seek_points + 1));

        header.metadata.seek_table = sktb_buf;
        header.metadata.seek_table_size = sktb_size;
    }

    uint32_t header_buf_size = DANA_HEADER_SIZE + sktb_size + header.metadata.cover_size + (64 * 1024);
    uint8_t* header_buf = malloc(header_buf_size);
    uint32_t header_size = 0;
    if (DANAEncoder_EncodeHeader(&header, header_buf, header_buf_size, &header_size) != DANA_APIRESULT_OK) {
        fprintf(stderr, "Error: Failed to encode header (metadata or cover too large)\n");
        free(header_buf);
        if (seek_samples) free(seek_samples);
        if (seek_offsets) free(seek_offsets);
        DANAMetadata_Release(&header.metadata);
        if (!is_in_pipe) fclose(in_fp);
        if (!is_out_pipe) fclose(out_fp);
        return 1;
    }
    fwrite(header_buf, 1, header_size, out_fp);

    uint32_t chunk_samples = enc_param.max_num_block_samples;
    uint32_t max_buf = DANA_CalculateSufficientBlockSize(wave_fmt.num_channels, chunk_samples, wave_fmt.bit_per_sample) + 8192;

    StreamPipeContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.wave_format = wave_fmt;
    ctx.enc_param   = enc_param;
    pthread_mutex_init(&ctx.mutex, NULL);
    pthread_cond_init(&ctx.cond_empty, NULL);
    pthread_cond_init(&ctx.cond_queued, NULL);
    pthread_cond_init(&ctx.cond_ready, NULL);

    for (int i = 0; i < STREAM_RING_SLOTS; i++) {
        for (uint32_t ch = 0; ch < wave_fmt.num_channels; ch++) {
            ctx.slots[i].pcm[ch] = malloc(sizeof(int32_t) * chunk_samples);
        }
        ctx.slots[i].compressed = malloc(max_buf);
        ctx.slots[i].state = SLOT_EMPTY;
    }

    uint32_t num_threads = 0;
    if (CommandLineParser_GetOptionAcquired(command_line_spec, "threads")) {
        num_threads = (uint32_t)atoi(CommandLineParser_GetArgumentString(command_line_spec, "threads"));
    }
    if (num_threads == 0) {
        long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
        num_threads = (nprocs > 0) ? (uint32_t)nprocs : 4;
    }
    if (num_threads > 64) num_threads = 64;

    pthread_t worker_threads[64];
    for (uint32_t i = 0; i < num_threads; i++) {
        pthread_create(&worker_threads[i], NULL, stream_encode_worker, &ctx);
    }

    uint32_t frame_bytes = (wav_fmt.bits_per_sample / 8) * wav_fmt.num_channels;
    uint8_t* raw_io_buf = malloc(chunk_samples * frame_bytes);

    uint32_t seq_in = 0, seq_out = 0;
    uint32_t sample_pos = 0;
    uint32_t total_blocks = 0, max_block_size = 0, max_bps = 0;
    uint32_t audio_bytes_written = 0;
    uint32_t current_seek_idx = 0;
    uint32_t next_seek_sample = 0;

    bool reached_eof = false;

    while (!reached_eof || seq_out < seq_in) {
        if (!reached_eof) {
            pthread_mutex_lock(&ctx.mutex);
            int slot_idx = -1;
            for (int i = 0; i < STREAM_RING_SLOTS; i++) {
                if (ctx.slots[i].state == SLOT_EMPTY) { slot_idx = i; break; }
            }
            pthread_mutex_unlock(&ctx.mutex);

            if (slot_idx != -1) {
                uint32_t to_read = chunk_samples;
                if (wav_fmt.num_samples != DANA_NUM_SAMPLES_INVALID) {
                    uint32_t remaining = wav_fmt.num_samples - sample_pos;
                    if (to_read > remaining) to_read = remaining;
                }

                if (to_read == 0) {
                    reached_eof = true;
                    pthread_mutex_lock(&ctx.mutex);
                    ctx.reader_finished = true;
                    pthread_cond_broadcast(&ctx.cond_queued);
                    pthread_mutex_unlock(&ctx.mutex);
                } else {
                    size_t n_read = fread(raw_io_buf, frame_bytes, to_read, in_fp);
                    if (n_read > 0) {
                        StreamSlot* s = &ctx.slots[slot_idx];
                        s->num_samples  = (uint32_t)n_read;
                        s->seq_id       = seq_in++;
                        s->start_sample = sample_pos;
                        sample_pos     += (uint32_t)n_read;

                        if (wav_fmt.bits_per_sample == 16) {
                            const int16_t* src16 = (const int16_t*)raw_io_buf;
                            for (size_t smp = 0; smp < n_read; smp++) {
                                for (uint32_t ch = 0; ch < wave_fmt.num_channels; ch++) {
                                    s->pcm[ch][smp] = (int32_t)src16[smp * wave_fmt.num_channels + ch] << 16;
                                }
                            }
                        } else if (wav_fmt.bits_per_sample == 24) {
                            const uint8_t* p = raw_io_buf;
                            for (size_t smp = 0; smp < n_read; smp++) {
                                for (uint32_t ch = 0; ch < wave_fmt.num_channels; ch++) {
                                    uint32_t b0 = *p++; uint32_t b1 = *p++; uint32_t b2 = *p++;
                                    s->pcm[ch][smp] = (int32_t)((b2 << 24) | (b1 << 16) | (b0 << 8));
                                }
                            }
                        }

                        pthread_mutex_lock(&ctx.mutex);
                        s->state = SLOT_QUEUED;
                        ctx.queued_count++;
                        pthread_cond_signal(&ctx.cond_queued);
                        pthread_mutex_unlock(&ctx.mutex);

                        if (wav_fmt.num_samples != DANA_NUM_SAMPLES_INVALID && sample_pos >= wav_fmt.num_samples) {
                            reached_eof = true;
                            pthread_mutex_lock(&ctx.mutex);
                            ctx.reader_finished = true;
                            pthread_cond_broadcast(&ctx.cond_queued);
                            pthread_mutex_unlock(&ctx.mutex);
                        }
                    } else {
                        reached_eof = true;
                        pthread_mutex_lock(&ctx.mutex);
                        ctx.reader_finished = true;
                        pthread_cond_broadcast(&ctx.cond_queued);
                        pthread_mutex_unlock(&ctx.mutex);
                    }
                }
            }
        }

        pthread_mutex_lock(&ctx.mutex);
        int emit_idx = -1;
        for (int i = 0; i < STREAM_RING_SLOTS; i++) {
            if (ctx.slots[i].state == SLOT_READY && ctx.slots[i].seq_id == seq_out) {
                emit_idx = i;
                break;
            }
        }

        if (emit_idx == -1) {
            if (seq_out < seq_in) {
                pthread_cond_wait(&ctx.cond_ready, &ctx.mutex);
            }
            pthread_mutex_unlock(&ctx.mutex);
            continue;
        }
        pthread_mutex_unlock(&ctx.mutex);

        StreamSlot* s = &ctx.slots[emit_idx];

        if (sktb_buf && current_seek_idx < num_seek_points) {
            if (s->start_sample >= next_seek_sample) {
                seek_samples[current_seek_idx] = s->start_sample;
                seek_offsets[current_seek_idx] = audio_bytes_written;
                current_seek_idx++;
                next_seek_sample += wave_fmt.sampling_rate;
            }
        }

        fwrite(s->compressed, 1, s->compressed_size, out_fp);
        audio_bytes_written += s->compressed_size;
        total_blocks        += s->num_blocks;
        if (s->max_block_size > max_block_size) max_block_size = s->max_block_size;
        if (s->max_bps > max_bps) max_bps = s->max_bps;

        pthread_mutex_lock(&ctx.mutex);
        s->state = SLOT_EMPTY;
        seq_out++;
        pthread_cond_signal(&ctx.cond_empty);
        pthread_mutex_unlock(&ctx.mutex);
    }

    for (uint32_t i = 0; i < num_threads; i++) {
        pthread_join(worker_threads[i], NULL);
    }

    // Finalize header and populate seek table
    if (!is_out_pipe) {
        header.num_samples        = sample_pos;
        header.num_blocks         = total_blocks;
        header.max_block_size     = max_block_size;
        header.max_bit_per_second = max_bps;

        if (sktb_buf) {
            uint8_t* p = sktb_buf;
            DANAByteArray_PutUint32(&p, current_seek_idx);
            uint32_t prev_s = 0, prev_off = 0;
            for (uint32_t i = 0; i < current_seek_idx; i++) {
                put_fixed_varint(&p, seek_samples[i] - prev_s);
                put_fixed_varint(&p, seek_offsets[i] - prev_off);
                prev_s = seek_samples[i];
                prev_off = seek_offsets[i];
            }
            while ((uint32_t)(p - sktb_buf) < sktb_size) {
                *p++ = 0x80;
            }
        }

        fseek(out_fp, 0, SEEK_SET);
        uint32_t final_header_size = 0;
        DANAEncoder_EncodeHeader(&header, header_buf, header_buf_size, &final_header_size);
        fwrite(header_buf, 1, header_size, out_fp);
    }

    if (seek_samples) free(seek_samples);
    if (seek_offsets) free(seek_offsets);
    header.metadata.seek_table = NULL;
    if (sktb_buf) free(sktb_buf);

    DANAMetadata_Release(&header.metadata);

    if (verpose_flag) {
        fprintf(stderr, "Encode success! size: -> %u bytes\n", header_size + audio_bytes_written);
    }

    free(raw_io_buf);
    free(header_buf);
    for (int i = 0; i < STREAM_RING_SLOTS; i++) {
        for (uint32_t ch = 0; ch < wave_fmt.num_channels; ch++) free(ctx.slots[i].pcm[ch]);
        free(ctx.slots[i].compressed);
    }

    if (!is_in_pipe) fclose(in_fp);
    if (!is_out_pipe) fclose(out_fp);
    return 0;
}

typedef struct {
    struct DANAWaveFormat wave_format;
    struct DANAEncodeParameter enc_param;
    uint8_t enable_crc_check;
    StreamSlot slots[STREAM_RING_SLOTS];
    pthread_mutex_t mutex;
    pthread_cond_t cond_empty;
    pthread_cond_t cond_queued;
    pthread_cond_t cond_ready;
    bool reader_finished;
    atomic_int error_code;
    uint32_t queued_count;
} StreamDecodeContext;

static void* stream_decode_worker(void* arg) {
    StreamDecodeContext* ctx = (StreamDecodeContext*)arg;

    struct DANADecoderConfig cfg = {
        .max_num_channels         = ctx->wave_format.num_channels,
        .max_num_block_samples    = ctx->enc_param.max_num_block_samples,
        .max_parcor_order         = ctx->enc_param.parcor_order,
        .max_longterm_order       = ctx->enc_param.longterm_order,
        .max_lms_order_per_filter = ctx->enc_param.lms_order_per_filter,
        .enable_crc_check         = ctx->enable_crc_check,
        .verpose_flag             = 0,
        .num_threads              = 1
    };

    struct DANADecoder* dec = DANADecoder_Create(&cfg);
    if (!dec) return NULL;
    DANADecoder_SetWaveFormat(dec, &ctx->wave_format);
    DANADecoder_SetEncodeParameter(dec, &ctx->enc_param);

    while (atomic_load(&ctx->error_code) == 0) {
        pthread_mutex_lock(&ctx->mutex);
        int slot_idx = -1;
        while (!ctx->reader_finished || ctx->queued_count > 0) {
            for (int i = 0; i < STREAM_RING_SLOTS; i++) {
                if (ctx->slots[i].state == SLOT_QUEUED) {
                    slot_idx = i;
                    ctx->slots[i].state = SLOT_PROCESSING;
                    ctx->queued_count--;
                    break;
                }
            }
            if (slot_idx != -1 || (ctx->reader_finished && ctx->queued_count == 0)) break;
            pthread_cond_wait(&ctx->cond_queued, &ctx->mutex);
        }
        pthread_mutex_unlock(&ctx->mutex);

        if (slot_idx == -1) break;

        StreamSlot* s = &ctx->slots[slot_idx];
        int32_t* out_ptr[DANA_MAX_CHANNELS];
        for (uint32_t ch = 0; ch < ctx->wave_format.num_channels; ch++) out_ptr[ch] = s->pcm[ch];

        uint32_t out_bsize = 0, out_nsamples = 0;
        DANAApiResult ret = DANADecoder_DecodeBlock(
            dec, s->compressed, s->compressed_size,
            out_ptr, ctx->enc_param.max_num_block_samples,
            &out_bsize, &out_nsamples);

        s->num_samples = out_nsamples;
        s->res = ret;
        if (ret != DANA_APIRESULT_OK) {
            atomic_store(&ctx->error_code, ret);
        }

        pthread_mutex_lock(&ctx->mutex);
        s->state = SLOT_READY;
        pthread_cond_signal(&ctx->cond_ready);
        pthread_mutex_unlock(&ctx->mutex);
    }

    DANADecoder_Destroy(dec);
    return NULL;
}

static int do_decode(const char* in_filename, const char* out_filename, uint8_t enable_crc_check, uint8_t verpose_flag, bool legacy_info) {
    bool is_in_pipe = (strcmp(in_filename, "-") == 0);
    bool is_out_pipe = (strcmp(out_filename, "-") == 0);

    FILE* in_fp = is_in_pipe ? stdin : fopen(in_filename, "rb");
    if (!in_fp) { fprintf(stderr, "Failed to open %s\n", in_filename); return 1; }
    setvbuf(in_fp, NULL, _IOFBF, 256 * 1024);

    uint8_t header_buf[43];
    if (fread(header_buf, 1, 43, in_fp) < 43) { if (!is_in_pipe) fclose(in_fp); return 1; }
    uint32_t offset = (((uint32_t)header_buf[4] << 24) | ((uint32_t)header_buf[5] << 16) | ((uint32_t)header_buf[6] << 8) | header_buf[7]);
    uint32_t full_hdr_size = offset + 8;

    uint8_t* full_hdr = malloc(full_hdr_size);
    memcpy(full_hdr, header_buf, 43);
    if (full_hdr_size > 43) {
        if (fread(full_hdr + 43, 1, full_hdr_size - 43, in_fp) < full_hdr_size - 43) {
            free(full_hdr); if (!is_in_pipe) fclose(in_fp); return 1;
        }
    }

    struct DANAHeaderInfo header;
    if (DANADecoder_DecodeHeader(full_hdr, full_hdr_size, &header, NULL) != DANA_APIRESULT_OK) {
        free(full_hdr); if (!is_in_pipe) fclose(in_fp); return 1;
    }
    free(full_hdr);

    if (verpose_flag) {
        if (header.metadata.title)  fprintf(stderr, "Title:                       %s\n", header.metadata.title);
        if (header.metadata.artist) fprintf(stderr, "Artist:                      %s\n", header.metadata.artist);
        if (header.metadata.album)  fprintf(stderr, "Album:                       %s\n", header.metadata.album);
        if (header.metadata.cover_data) fprintf(stderr, "Cover Art:                   Available (%u bytes)\n", header.metadata.cover_size);
        fprintf(stderr, "Num Channels:                %u\n", header.wave_format.num_channels);
        fprintf(stderr, "Bit Per Sample:              %u\n", header.wave_format.bit_per_sample);
        fprintf(stderr, "Sampling Rate:               %u\n", header.wave_format.sampling_rate);
        if (header.metadata.seek_table) {
            fprintf(stderr, "Seek Table:                  Available (%u bytes)\n", header.metadata.seek_table_size);
        }
    }

    FILE* out_fp = is_out_pipe ? stdout : fopen(out_filename, "wb");
    if (!out_fp) { if (!is_in_pipe) fclose(in_fp); return 1; }
    setvbuf(out_fp, NULL, _IOFBF, 256 * 1024);

    struct WAVFileFormat wav_fmt = {
        .data_format     = WAV_DATA_FORMAT_PCM,
        .num_channels    = header.wave_format.num_channels,
        .sampling_rate   = header.wave_format.sampling_rate,
        .bits_per_sample = header.wave_format.bit_per_sample,
        .num_samples     = header.num_samples
    };

    WAV_WriteWAVHeaderToFP(out_fp, &wav_fmt);

    uint32_t chunk_samples = header.encode_param.max_num_block_samples;
    uint32_t max_buf = DANA_CalculateSufficientBlockSize(header.wave_format.num_channels, chunk_samples, header.wave_format.bit_per_sample) + 8192;

    StreamDecodeContext ctx;
    memset(&ctx, 0, sizeof(ctx));
    ctx.wave_format      = header.wave_format;
    ctx.enc_param        = header.encode_param;
    ctx.enable_crc_check = enable_crc_check;
    pthread_mutex_init(&ctx.mutex, NULL);
    pthread_cond_init(&ctx.cond_empty, NULL);
    pthread_cond_init(&ctx.cond_queued, NULL);
    pthread_cond_init(&ctx.cond_ready, NULL);

    for (int i = 0; i < STREAM_RING_SLOTS; i++) {
        for (uint32_t ch = 0; ch < header.wave_format.num_channels; ch++) {
            ctx.slots[i].pcm[ch] = malloc(sizeof(int32_t) * chunk_samples);
        }
        ctx.slots[i].compressed = malloc(max_buf);
        ctx.slots[i].state = SLOT_EMPTY;
    }

    uint32_t num_threads = 0;
    if (CommandLineParser_GetOptionAcquired(command_line_spec, "threads")) {
        num_threads = (uint32_t)atoi(CommandLineParser_GetArgumentString(command_line_spec, "threads"));
    }
    if (num_threads == 0) {
        long nprocs = sysconf(_SC_NPROCESSORS_ONLN);
        num_threads = (nprocs > 0) ? (uint32_t)nprocs : 4;
    }
    if (num_threads > 64) num_threads = 64;

    pthread_t worker_threads[64];
    for (uint32_t i = 0; i < num_threads; i++) {
        pthread_create(&worker_threads[i], NULL, stream_decode_worker, &ctx);
    }

    uint32_t frame_bytes = (header.wave_format.bit_per_sample / 8) * header.wave_format.num_channels;
    uint8_t* raw_io_buf = malloc(chunk_samples * frame_bytes);

    uint32_t seq_in = 0, seq_out = 0;
    uint32_t total_samples_decoded = 0;
    bool reached_eof = false;

    while (!reached_eof || seq_out < seq_in) {
        /* Read next compressed block if space in ring */
        if (!reached_eof) {
            pthread_mutex_lock(&ctx.mutex);
            int slot_idx = -1;
            for (int i = 0; i < STREAM_RING_SLOTS; i++) {
                if (ctx.slots[i].state == SLOT_EMPTY) { slot_idx = i; break; }
            }
            pthread_mutex_unlock(&ctx.mutex);

            if (slot_idx != -1) {
                uint8_t b_hdr[5];
                size_t n = fread(b_hdr, 1, 5, in_fp);
                if (n == 5) {
                    uint16_t sync = ((uint16_t)b_hdr[0] << 8) | b_hdr[1];
                    if (sync == DANA_BLOCK_SYNC_CODE) {
                        uint32_t bsize = (((uint32_t)b_hdr[2] << 16) | ((uint32_t)b_hdr[3] << 8) | b_hdr[4]) + 5;
                        StreamSlot* s = &ctx.slots[slot_idx];
                        memcpy(s->compressed, b_hdr, 5);
                        if (fread(s->compressed + 5, 1, bsize - 5, in_fp) == bsize - 5) {
                            s->compressed_size = bsize;
                            s->seq_id          = seq_in++;

                            pthread_mutex_lock(&ctx.mutex);
                            s->state = SLOT_QUEUED;
                            ctx.queued_count++;
                            pthread_cond_signal(&ctx.cond_queued);
                            pthread_mutex_unlock(&ctx.mutex);
                        } else {
                            reached_eof = true;
                            pthread_mutex_lock(&ctx.mutex);
                            ctx.reader_finished = true;
                            pthread_cond_broadcast(&ctx.cond_queued);
                            pthread_mutex_unlock(&ctx.mutex);
                        }
                    } else {
                        reached_eof = true;
                        pthread_mutex_lock(&ctx.mutex);
                        ctx.reader_finished = true;
                        pthread_cond_broadcast(&ctx.cond_queued);
                        pthread_mutex_unlock(&ctx.mutex);
                    }
                } else {
                    reached_eof = true;
                    pthread_mutex_lock(&ctx.mutex);
                    ctx.reader_finished = true;
                    pthread_cond_broadcast(&ctx.cond_queued);
                    pthread_mutex_unlock(&ctx.mutex);
                }
            }
        }

        // Emit PCM in strict sequential order
        pthread_mutex_lock(&ctx.mutex);
        int emit_idx = -1;
        for (int i = 0; i < STREAM_RING_SLOTS; i++) {
            if (ctx.slots[i].state == SLOT_READY && ctx.slots[i].seq_id == seq_out) {
                emit_idx = i;
                break;
            }
        }

        if (emit_idx == -1) {
            if (seq_out < seq_in) {
                pthread_cond_wait(&ctx.cond_ready, &ctx.mutex);
            }
            pthread_mutex_unlock(&ctx.mutex);
            continue;
        }
        pthread_mutex_unlock(&ctx.mutex);

        StreamSlot* s = &ctx.slots[emit_idx];
        if (header.wave_format.bit_per_sample == 16) {
            int16_t* dst16 = (int16_t*)raw_io_buf;
            for (uint32_t smp = 0; smp < s->num_samples; smp++) {
                for (uint32_t ch = 0; ch < header.wave_format.num_channels; ch++) {
                    dst16[smp * header.wave_format.num_channels + ch] = (int16_t)(s->pcm[ch][smp] >> 16);
                }
            }
        } else if (header.wave_format.bit_per_sample == 24) {
            uint8_t* p = raw_io_buf;
            for (uint32_t smp = 0; smp < s->num_samples; smp++) {
                for (uint32_t ch = 0; ch < header.wave_format.num_channels; ch++) {
                    int32_t v = s->pcm[ch][smp] >> 8;
                    *p++ = (uint8_t)(v & 0xFF);
                    *p++ = (uint8_t)((v >> 8) & 0xFF);
                    *p++ = (uint8_t)((v >> 16) & 0xFF);
                }
            }
        }

        fwrite(raw_io_buf, frame_bytes, s->num_samples, out_fp);
        total_samples_decoded += s->num_samples;

        pthread_mutex_lock(&ctx.mutex);
        s->state = SLOT_EMPTY;
        seq_out++;
        pthread_cond_signal(&ctx.cond_empty);
        pthread_mutex_unlock(&ctx.mutex);
    }

    for (uint32_t i = 0; i < num_threads; i++) {
        pthread_join(worker_threads[i], NULL);
    }

    WAV_WriteMetadataToFP(out_fp, &header.metadata, legacy_info);

    if (!is_out_pipe) {
        wav_fmt.num_samples = total_samples_decoded;
        long total_file_size = ftell(out_fp);
        fseek(out_fp, 0, SEEK_SET);
        WAV_WriteWAVHeaderToFP(out_fp, &wav_fmt);

        // Update RIFF total size to include metadata chunks
        if (total_file_size >= 8) {
            uint32_t riff_payload_size = (uint32_t)(total_file_size - 8);
            uint8_t sz[4] = {
                (uint8_t)(riff_payload_size & 0xFF),
                (uint8_t)((riff_payload_size >> 8) & 0xFF),
                (uint8_t)((riff_payload_size >> 16) & 0xFF),
                (uint8_t)((riff_payload_size >> 24) & 0xFF)
            };
            fseek(out_fp, 4, SEEK_SET);
            fwrite(sz, 1, 4, out_fp);
        }
    }

    free(raw_io_buf);
    for (int i = 0; i < STREAM_RING_SLOTS; i++) {
        for (uint32_t ch = 0; ch < header.wave_format.num_channels; ch++) free(ctx.slots[i].pcm[ch]);
        free(ctx.slots[i].compressed);
    }

    DANAMetadata_Release(&header.metadata);
    if (!is_in_pipe) fclose(in_fp);
    if (!is_out_pipe) fclose(out_fp);
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
    printf("DANA - Digital Audio Non-lossy Archive, Version %s\n", DANA_VERSION_STRING);
    printf("Copyright (c) 2026 holotwist. All rights reserved.\n");
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

    if (verbose_flag) {
        fprintf(stderr, "DANA - Digital Audio Non-lossy Archive, Version %s\n", DANA_VERSION_STRING);
        fprintf(stderr, "Copyright (c) 2026 holotwist. All rights reserved.\n\n");
    }

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
            bool legacy_info = CommandLineParser_GetOptionAcquired(command_line_spec, "legacy-wav");
            if (do_decode(input_file, output_file, enable_crc_check, verbose_flag, legacy_info) != 0) {
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