#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <dirent.h>
#include <sys/stat.h>
#include <pthread.h>
#include <time.h>
#include <math.h>
#include <complex.h>
#include <alsa/asoundlib.h>
#include <ncurses.h>
#include <locale.h>
#include <stdatomic.h>
#include <stdbool.h>

#include "DANADecoder.h"
#include "DANAUtility.h"

#define BUFFER_CHUNK_SIZE 16384
#define MAX_FILES 1024

#define VIS_BUF_SIZE  65536u
#define VIS_BUF_MASK  (VIS_BUF_SIZE - 1u)
#define FFT_SIZE      1024

typedef struct {
    char name[256];
    int is_dir;
} FileEntry;

typedef enum {
    STATE_STOPPED,
    STATE_PLAYING,
    STATE_PAUSED
} PlayState;

typedef enum {
    CMD_NONE,
    CMD_PLAY,
    CMD_PAUSE,
    CMD_NEXT,
    CMD_PREV,
    CMD_STOP,
    CMD_QUIT,
    CMD_SEEK
} PlayerCommand;

static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

static char current_dir[1024] = ".";
static FileEntry files[MAX_FILES];
static int num_files = 0;
static int selected_file_idx = 0;
static int scroll_offset = 0;

static char playing_filepath[1024] = "";
static char playing_filename[256] = "<Empty>";
static int playing_file_idx = -1;
static struct DANAHeaderInfo p_header = {0};

static atomic_int  header_ready_for_idx = -1;
static atomic_int  play_state_atomic = STATE_STOPPED;
static atomic_int  current_cmd_atomic = CMD_NONE;
static atomic_int  volume = 100;
static atomic_int  seek_target_sec = -1;

static atomic_uint p_decoded_blocks = 0;
static atomic_uint p_played_buffers = 0;
static atomic_uint p_lost_buffers = 0;
static atomic_uint p_media_data_size_kib = 0;
static atomic_uint p_input_bitrate_kbs = 0;
static atomic_uint p_demuxed_data_size_kib = 0;
static atomic_uint p_content_bitrate_kbs = 0;
static atomic_uint p_discarded = 0;
static atomic_uint p_dropped = 0;

static atomic_uint p_current_sec = 0;
static atomic_uint p_total_sec = 0;

static int active_tab = 1;
static int current_vis_mode = 0;
static bool is_fullscreen = false;
static bool force_redraw = true;

static float       vis_ring_l[VIS_BUF_SIZE] = {0};
static float       vis_ring_r[VIS_BUF_SIZE] = {0};
static atomic_uint vis_wpos     = 0;   
static atomic_uint vis_play_pos = 0;   
static atomic_uint vis_srate    = 44100;

typedef struct {
    FILE *fp;
    struct DANAStreamingDecoder *decoder;
    uint8_t *pending_chunk;
    size_t pending_bytes;
    int pending_append;
    struct { uint8_t *ptr; size_t size; size_t consumed; } allocs[256];
    int alloc_head, alloc_tail;
    size_t total_bytes_read, total_consumed_bytes;
    DANAApiResult last_res;
    struct DANAHeaderInfo header;
} StreamState;

// A FFT to render the visualizer
static void compute_fft(float complex *X, int N) {
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float complex temp = X[i];
            X[i] = X[j];
            X[j] = temp;
        }
    }
    for (int len = 2; len <= N; len <<= 1) {
        float angle = -2.0f * (float)M_PI / len;
        float complex wlen = cosf(angle) + I * sinf(angle);
        for (int i = 0; i < N; i += len) {
            float complex w = 1.0f;
            for (int j = 0; j < len / 2; j++) {
                float complex u = X[i + j];
                float complex v = X[i + j + len / 2] * w;
                X[i + j] = u + v;
                X[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

static inline void set_braille_pixel(uint8_t *grid, int draw_w, int draw_h, int px_x, int px_y) {
    if (px_x < 0 || px_x >= draw_w * 2 || px_y < 0 || px_y >= draw_h * 4) return;
    int cell_x = px_x / 2;
    int cell_y = px_y / 4;
    int dot_x = px_x % 2;
    int dot_y = px_y % 4;
    static const uint8_t dot_mask[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
    grid[cell_y * draw_w + cell_x] |= dot_mask[dot_y][dot_x];
}

static void draw_braille_line(uint8_t *grid, int draw_w, int draw_h, int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (;;) {
        set_braille_pixel(grid, draw_w, draw_h, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void silent_alsa_error_handler(const char *file, int line, const char *function, int err, const char *fmt, ...) {
    (void)file; (void)line; (void)function; (void)err; (void)fmt;
}

static int file_cmp(const void *a, const void *b) {
    const FileEntry *fa = (const FileEntry *)a;
    const FileEntry *fb = (const FileEntry *)b;
    if (fa->is_dir != fb->is_dir) return fb->is_dir - fa->is_dir;
    return strcasecmp(fa->name, fb->name);
}

static void load_directory(const char *path) {
    DIR *dir = opendir(path);
    if (!dir) return;
    
    num_files = 0;
    strcpy(files[num_files].name, "..");
    files[num_files].is_dir = 1;
    num_files++;
    
    struct dirent *entry;
    while ((entry = readdir(dir)) != NULL && num_files < MAX_FILES) {
        if (strcmp(entry->d_name, ".") == 0 || strcmp(entry->d_name, "..") == 0) continue;
        char full_path[1024];
        snprintf(full_path, sizeof(full_path), "%s/%s", path, entry->d_name);
        
        struct stat st;
        if (stat(full_path, &st) == 0) {
            int is_dir = S_ISDIR(st.st_mode);
            char *ext = strrchr(entry->d_name, '.');
            if (is_dir || (ext && (strcasecmp(ext, ".dana") == 0 || strcasecmp(ext, ".dahl") == 0))) {
                strncpy(files[num_files].name, entry->d_name, 255);
                files[num_files].is_dir = is_dir;
                num_files++;
            }
        }
    }
    closedir(dir);
    if (num_files > 1) qsort(files + 1, (size_t)(num_files - 1), sizeof(FileEntry), file_cmp);
    
    selected_file_idx = 0;
    scroll_offset = 0;
}

static void init_stream_state(StreamState *ss, FILE *fp) {
    memset(ss, 0, sizeof(StreamState));
    ss->fp = fp;
    ss->pending_append = 1;
}

static void free_stream_state(StreamState *ss) {
    if (ss->decoder) DANAStreamingDecoder_Destroy(ss->decoder);
    while (ss->alloc_head != ss->alloc_tail) { free(ss->allocs[ss->alloc_head].ptr); ss->alloc_head = (ss->alloc_head + 1) % 256; }
    if (ss->pending_chunk) free(ss->pending_chunk);
    if (ss->fp) fclose(ss->fp);
    DANAMetadata_Release(&ss->header.metadata);
}

static void feed_and_decode(StreamState *ss, int32_t **pcm_out, uint32_t out_max, uint32_t *out_samples, int *exit_loop) {
    uint8_t dummy_byte = 0;
    DANAStreamingDecoder_AppendDataFragment(ss->decoder, &dummy_byte, 0);

    while (ss->pending_append) {
        if (((ss->alloc_tail + 1) % 256) == ss->alloc_head) break;
        uint8_t *chunk; size_t bytes;
        if (ss->pending_chunk) {
            chunk = ss->pending_chunk; bytes = ss->pending_bytes; ss->pending_chunk = NULL;
        } else {
            chunk = malloc(BUFFER_CHUNK_SIZE);
            if (!chunk) break;
            bytes = fread(chunk, 1, BUFFER_CHUNK_SIZE, ss->fp);
            if (bytes == 0) { free(chunk); ss->pending_append = 0; break; }
            ss->total_bytes_read += bytes;
        }
        if (DANAStreamingDecoder_AppendDataFragment(ss->decoder, chunk, (uint32_t)bytes) != DANA_APIRESULT_OK) {
            ss->pending_chunk = chunk; ss->pending_bytes = bytes; break;
        }
        ss->allocs[ss->alloc_tail].ptr = chunk; ss->allocs[ss->alloc_tail].size = bytes; ss->allocs[ss->alloc_tail].consumed = 0;
        ss->alloc_tail = (ss->alloc_tail + 1) % 256;
    }

    ss->last_res = DANAStreamingDecoder_Decode(ss->decoder, pcm_out, out_max, out_samples);
    if (ss->last_res != DANA_APIRESULT_OK && ss->last_res != DANA_APIRESULT_INSUFFICIENT_DATA_SIZE) {
        if (ss->last_res == DANA_APIRESULT_DETECT_DATA_CORRUPTION) atomic_fetch_add(&p_discarded, 1);
        else atomic_fetch_add(&p_dropped, 1);
        *exit_loop = 1;
    }

    const uint8_t *consumed_ptr; uint32_t consumed_size;
    while (DANAStreamingDecoder_CollectDataFragment(ss->decoder, &consumed_ptr, &consumed_size) == DANA_APIRESULT_OK) {
        ss->total_consumed_bytes += consumed_size;
        if (consumed_size > 0 && ss->alloc_head != ss->alloc_tail) {
            ss->allocs[ss->alloc_head].consumed += consumed_size;
            if (ss->allocs[ss->alloc_head].consumed == ss->allocs[ss->alloc_head].size) {
                free(ss->allocs[ss->alloc_head].ptr); ss->alloc_head = (ss->alloc_head + 1) % 256;
            }
        }
    }
}

// Dedicated Audio Thread
static void *audio_thread_func(void *arg) {
    (void)arg;
    struct timespec sleep_ts = {0, 50000000L}; // 50ms

    while (1) {
        PlayerCommand cmd = atomic_load(&current_cmd_atomic);
        char filepath[1024];
        int this_file_idx = -1;

        if (cmd == CMD_PLAY) {
            atomic_store(&header_ready_for_idx, -1);
            pthread_mutex_lock(&state_mutex);
            strncpy(filepath, playing_filepath, sizeof(filepath));
            this_file_idx = playing_file_idx;
            pthread_mutex_unlock(&state_mutex);
            
            atomic_store(&current_cmd_atomic, CMD_NONE);
            atomic_store(&play_state_atomic, STATE_PLAYING);
            
            atomic_store(&p_decoded_blocks, 0); atomic_store(&p_played_buffers, 0);
            atomic_store(&p_lost_buffers, 0); atomic_store(&p_media_data_size_kib, 0);
            atomic_store(&p_input_bitrate_kbs, 0); atomic_store(&p_demuxed_data_size_kib, 0);
            atomic_store(&p_content_bitrate_kbs, 0); atomic_store(&p_discarded, 0);
            atomic_store(&p_dropped, 0); atomic_store(&p_current_sec, 0);
            atomic_store(&p_total_sec, 0);
            
            memset(vis_ring_l, 0, sizeof(vis_ring_l));
            memset(vis_ring_r, 0, sizeof(vis_ring_r));
            atomic_store(&vis_wpos, 0);
            atomic_store(&vis_play_pos, 0);
        } else if (cmd == CMD_QUIT) {
            break;
        }

        PlayState state = atomic_load(&play_state_atomic);
        if (state != STATE_PLAYING && state != STATE_PAUSED) {
            nanosleep(&sleep_ts, NULL);
            continue;
        }

        StreamState ss1, ss2;
        init_stream_state(&ss1, NULL);
        init_stream_state(&ss2, NULL);

        ss1.fp = fopen(filepath, "rb");
        if (!ss1.fp) { atomic_store(&play_state_atomic, STATE_STOPPED); continue; }

        int hybrid_mode = 0;
        char dahc_filepath[1024];
        size_t len = strlen(filepath);
        // If .dahl, check if we have the correction
        if (len > 5 && strcasecmp(filepath + len - 5, ".dahl") == 0) {
            strcpy(dahc_filepath, filepath);
            strcpy(dahc_filepath + len - 5, ".dahc");
            ss2.fp = fopen(dahc_filepath, "rb");
            if (ss2.fp) hybrid_mode = 1;
        }

        uint8_t header_buf[43];
        if (fread(header_buf, 1, 43, ss1.fp) < 43) { free_stream_state(&ss1); if(hybrid_mode) free_stream_state(&ss2); atomic_store(&play_state_atomic, STATE_STOPPED); continue; }
        uint32_t offset = (((uint32_t)header_buf[4] << 24) | ((uint32_t)header_buf[5] << 16) | ((uint32_t)header_buf[6] << 8) | header_buf[7]);
        uint32_t full_header_size = offset + 8;
        uint32_t full_header_size2 = 0;
        
        uint8_t* full_header_buf = malloc(full_header_size);
        fseek(ss1.fp, 0, SEEK_SET);
        if (fread(full_header_buf, 1, full_header_size, ss1.fp) < full_header_size) {
            free(full_header_buf); free_stream_state(&ss1); if(hybrid_mode) free_stream_state(&ss2); atomic_store(&play_state_atomic, STATE_STOPPED); continue;
        }

        if (DANADecoder_DecodeHeader(full_header_buf, full_header_size, &ss1.header, NULL) != DANA_APIRESULT_OK) {
            free(full_header_buf); free_stream_state(&ss1); if(hybrid_mode) free_stream_state(&ss2); atomic_store(&play_state_atomic, STATE_STOPPED); continue;
        }
        free(full_header_buf);

        if (hybrid_mode) {
            if (fread(header_buf, 1, 43, ss2.fp) < 43) hybrid_mode = 0;
            else {
                uint32_t offset2 = (((uint32_t)header_buf[4] << 24) | ((uint32_t)header_buf[5] << 16) | ((uint32_t)header_buf[6] << 8) | header_buf[7]);
                full_header_size2 = offset2 + 8;
                uint8_t* full_header_buf2 = malloc(full_header_size2);
                fseek(ss2.fp, 0, SEEK_SET);
                if (fread(full_header_buf2, 1, full_header_size2, ss2.fp) < full_header_size2) hybrid_mode = 0;
                else if (DANADecoder_DecodeHeader(full_header_buf2, full_header_size2, &ss2.header, NULL) != DANA_APIRESULT_OK) hybrid_mode = 0;
                free(full_header_buf2);
            }
        }

        struct DANAMetadata old_metadata = {0};
        pthread_mutex_lock(&state_mutex);
        old_metadata = p_header.metadata;
        p_header = ss1.header; 
        memset(&ss1.header.metadata, 0, sizeof(struct DANAMetadata));
        pthread_mutex_unlock(&state_mutex);
        DANAMetadata_Release(&old_metadata);
        
        atomic_store(&p_total_sec, (p_header.wave_format.sampling_rate > 0) ? p_header.num_samples / p_header.wave_format.sampling_rate : 0);
        atomic_store(&vis_srate, p_header.wave_format.sampling_rate);
        atomic_store(&header_ready_for_idx, this_file_idx);

        snd_pcm_t *pcm_handle;
        if (snd_pcm_open(&pcm_handle, "default", SND_PCM_STREAM_PLAYBACK, 0) < 0) {
            free_stream_state(&ss1); if(hybrid_mode) free_stream_state(&ss2); atomic_store(&play_state_atomic, STATE_STOPPED); continue;
        }
        snd_pcm_set_params(pcm_handle, SND_PCM_FORMAT_S32_LE, SND_PCM_ACCESS_RW_INTERLEAVED,
                           p_header.wave_format.num_channels, p_header.wave_format.sampling_rate, 1, 500000);

        struct DANAStreamingDecoderConfig cfg = {
            .core_config = {
                .max_num_channels = 8,
                .max_num_block_samples = 16384,
                .max_parcor_order = 48,
                .max_longterm_order = 5,
                .max_lms_order_per_filter = 40,
                .enable_crc_check = 1,
                .verpose_flag = 0
            },
            .decode_interval_hz = 120.0f,
            .max_bit_per_sample = 32
        };

        ss1.decoder = DANAStreamingDecoder_Create(&cfg);
        DANAStreamingDecoder_SetWaveFormat(ss1.decoder, &ss1.header.wave_format);
        DANAStreamingDecoder_SetEncodeParameter(ss1.decoder, &ss1.header.encode_param);

        if (hybrid_mode) {
            ss2.decoder = DANAStreamingDecoder_Create(&cfg);
            DANAStreamingDecoder_SetWaveFormat(ss2.decoder, &ss2.header.wave_format);
            DANAStreamingDecoder_SetEncodeParameter(ss2.decoder, &ss2.header.encode_param);
        }

        uint32_t out_max = 16384;
        int32_t **pcm_out1 = malloc(sizeof(int32_t*) * ss1.header.wave_format.num_channels);
        int32_t **pcm_out2 = hybrid_mode ? malloc(sizeof(int32_t*) * ss1.header.wave_format.num_channels) : NULL;
        int32_t **q1 = malloc(sizeof(int32_t*) * ss1.header.wave_format.num_channels);
        int32_t **q2 = hybrid_mode ? malloc(sizeof(int32_t*) * ss1.header.wave_format.num_channels) : NULL;
        
        for(uint32_t i=0; i < ss1.header.wave_format.num_channels; i++) {
            pcm_out1[i] = malloc(sizeof(int32_t) * out_max);
            q1[i] = malloc(sizeof(int32_t) * 131072);
            if (hybrid_mode) {
                pcm_out2[i] = malloc(sizeof(int32_t) * out_max);
                q2[i] = malloc(sizeof(int32_t) * 131072);
            }
        }
        int32_t *interleaved = malloc(sizeof(int32_t) * 65536 * ss1.header.wave_format.num_channels);

        uint32_t q1_len = 0, q2_len = 0;
        uint32_t samples_played = 0;
        int exit_loop = 0;

        while (samples_played < ss1.header.num_samples && !exit_loop) {
            cmd = atomic_load(&current_cmd_atomic);
            if (cmd == CMD_STOP || cmd == CMD_NEXT || cmd == CMD_PREV || cmd == CMD_PLAY || cmd == CMD_QUIT) {
                exit_loop = 1; break;
            }
            if (cmd == CMD_PAUSE) {
                atomic_store(&play_state_atomic, (atomic_load(&play_state_atomic) == STATE_PLAYING) ? STATE_PAUSED : STATE_PLAYING);
                atomic_store(&current_cmd_atomic, CMD_NONE);
            }
            if (cmd == CMD_SEEK) {
                int target_sec = atomic_load(&seek_target_sec);
                atomic_store(&current_cmd_atomic, CMD_NONE);
                if (target_sec >= 0 && p_header.metadata.seek_table) {
                    uint32_t target_sample = target_sec * ss1.header.wave_format.sampling_rate;
                    uint32_t out_sample = 0, out_byte_offset = 0;
                    if (DANADecoder_GetSeekPoint(&p_header.metadata, target_sample, &out_sample, &out_byte_offset) == DANA_APIRESULT_OK) {
                        fseek(ss1.fp, full_header_size + out_byte_offset, SEEK_SET);
                        if (hybrid_mode && ss2.fp) fseek(ss2.fp, full_header_size2 + out_byte_offset, SEEK_SET);
                        
                        DANAStreamingDecoder_Destroy(ss1.decoder);
                        ss1.decoder = DANAStreamingDecoder_Create(&cfg);
                        DANAStreamingDecoder_SetWaveFormat(ss1.decoder, &ss1.header.wave_format);
                        DANAStreamingDecoder_SetEncodeParameter(ss1.decoder, &ss1.header.encode_param);
                        while (ss1.alloc_head != ss1.alloc_tail) { free(ss1.allocs[ss1.alloc_head].ptr); ss1.alloc_head = (ss1.alloc_head + 1) % 256; }
                        if (ss1.pending_chunk) { free(ss1.pending_chunk); ss1.pending_chunk = NULL; }
                        ss1.pending_append = 1;
                        ss1.total_bytes_read = out_byte_offset;
                        ss1.total_consumed_bytes = out_byte_offset;

                        if (hybrid_mode) {
                            DANAStreamingDecoder_Destroy(ss2.decoder);
                            ss2.decoder = DANAStreamingDecoder_Create(&cfg);
                            DANAStreamingDecoder_SetWaveFormat(ss2.decoder, &ss2.header.wave_format);
                            DANAStreamingDecoder_SetEncodeParameter(ss2.decoder, &ss2.header.encode_param);
                            while (ss2.alloc_head != ss2.alloc_tail) { free(ss2.allocs[ss2.alloc_head].ptr); ss2.alloc_head = (ss2.alloc_head + 1) % 256; }
                            if (ss2.pending_chunk) { free(ss2.pending_chunk); ss2.pending_chunk = NULL; }
                            ss2.pending_append = 1;
                            ss2.total_bytes_read = out_byte_offset;
                            ss2.total_consumed_bytes = out_byte_offset;
                        }

                        q1_len = 0;
                        if (hybrid_mode) q2_len = 0;
                        samples_played = out_sample;
                        
                        memset(vis_ring_l, 0, sizeof(vis_ring_l));
                        memset(vis_ring_r, 0, sizeof(vis_ring_r));
                        atomic_store(&vis_wpos, 0);
                        atomic_store(&vis_play_pos, 0);

                        atomic_store(&p_current_sec, samples_played / ss1.header.wave_format.sampling_rate);
                        snd_pcm_drop(pcm_handle);
                        snd_pcm_prepare(pcm_handle);
                    }
                }
                continue;
            }
            if (atomic_load(&play_state_atomic) == STATE_PAUSED) {
                nanosleep(&sleep_ts, NULL); continue;
            }
            
            uint32_t out_samples1 = 0;
            if (q1_len < 65536) {
                feed_and_decode(&ss1, pcm_out1, out_max, &out_samples1, &exit_loop);
                if (out_samples1 > 0) {
                    for(uint32_t c=0; c < ss1.header.wave_format.num_channels; c++) memcpy(q1[c] + q1_len, pcm_out1[c], out_samples1 * sizeof(int32_t));
                    q1_len += out_samples1;
                }
            }
            
            if (hybrid_mode) {
                uint32_t out_samples2 = 0;
                if (q2_len < 65536) {
                    feed_and_decode(&ss2, pcm_out2, out_max, &out_samples2, &exit_loop);
                    if (out_samples2 > 0) {
                        for(uint32_t c=0; c < ss1.header.wave_format.num_channels; c++) memcpy(q2[c] + q2_len, pcm_out2[c], out_samples2 * sizeof(int32_t));
                        q2_len += out_samples2;
                    }
                }
            }
            
            uint32_t mix_samples = hybrid_mode ? (q1_len < q2_len ? q1_len : q2_len) : q1_len;
            if (mix_samples > 65536) mix_samples = 65536; 
            
            if (mix_samples > 0) {
                int current_vol = atomic_load(&volume);
                for (uint32_t s = 0; s < mix_samples; s++) {
                    for (uint32_t c = 0; c < ss1.header.wave_format.num_channels; c++) {
                        int32_t v1 = q1[c][s];
                        int32_t v2 = hybrid_mode ? q2[c][s] : 0;
                        long long val64 = ((long long)(v1 + v2) * current_vol) / 100;
                        if (val64 > 2147483647LL) val64 = 2147483647LL;
                        else if (val64 < -2147483648LL) val64 = -2147483648LL;
                        interleaved[s * ss1.header.wave_format.num_channels + c] = (int32_t)val64;
                    }
                }
                
                uint32_t local_wpos = atomic_load(&vis_wpos);
                for (uint32_t s = 0; s < mix_samples; s++) {
                    float vl = 0.0f, vr = 0.0f;
                    if (ss1.header.wave_format.num_channels == 1) {
                        long long mixed = q1[0][s] + (hybrid_mode ? q2[0][s] : 0);
                        vl = vr = (float)((mixed * current_vol) / 100) / 2147483648.0f;
                    } else if (ss1.header.wave_format.num_channels >= 2) {
                        long long mixed_l = q1[0][s] + (hybrid_mode ? q2[0][s] : 0);
                        long long mixed_r = q1[1][s] + (hybrid_mode ? q2[1][s] : 0);
                        vl = (float)((mixed_l * current_vol) / 100) / 2147483648.0f;
                        vr = (float)((mixed_r * current_vol) / 100) / 2147483648.0f;
                    }
                    vis_ring_l[local_wpos & VIS_BUF_MASK] = vl;
                    vis_ring_r[local_wpos & VIS_BUF_MASK] = vr;
                    local_wpos++;
                }
                atomic_store(&vis_wpos, local_wpos);

                uint32_t written = 0;
                while (written < mix_samples && !exit_loop) {
                    snd_pcm_sframes_t frames = snd_pcm_writei(pcm_handle, interleaved + (written * ss1.header.wave_format.num_channels), mix_samples - written);
                    if (frames < 0) {
                        frames = snd_pcm_recover(pcm_handle, (int)frames, 0);
                        atomic_fetch_add(&p_lost_buffers, 1);
                        if (frames < 0) exit_loop = 1;
                    } else if (frames > 0) {
                        written += (uint32_t)frames;
                    }
                }

                snd_pcm_sframes_t delay = 0;
                if (snd_pcm_delay(pcm_handle, &delay) == 0 && delay >= 0) {
                    if (local_wpos > (uint32_t)delay) atomic_store(&vis_play_pos, local_wpos - delay);
                    else atomic_store(&vis_play_pos, 0);
                } else {
                    atomic_store(&vis_play_pos, local_wpos);
                }

                for (uint32_t c = 0; c < ss1.header.wave_format.num_channels; c++) {
                    memmove(q1[c], q1[c] + mix_samples, (q1_len - mix_samples) * sizeof(int32_t));
                    if (hybrid_mode) memmove(q2[c], q2[c] + mix_samples, (q2_len - mix_samples) * sizeof(int32_t));
                }
                q1_len -= mix_samples;
                if (hybrid_mode) q2_len -= mix_samples;

                samples_played += mix_samples;
                uint32_t cur_sec = samples_played / ss1.header.wave_format.sampling_rate;
                atomic_fetch_add(&p_decoded_blocks, 1); 
                atomic_fetch_add(&p_played_buffers, 1);
                atomic_store(&p_media_data_size_kib, (uint32_t)((ss1.total_bytes_read + (hybrid_mode ? ss2.total_bytes_read : 0)) / 1024));
                atomic_store(&p_demuxed_data_size_kib, atomic_load(&p_media_data_size_kib));
                if (cur_sec > 0) {
                    atomic_store(&p_input_bitrate_kbs, (uint32_t)(((ss1.total_bytes_read + (hybrid_mode ? ss2.total_bytes_read : 0)) * 8) / cur_sec / 1000));
                    atomic_store(&p_content_bitrate_kbs, (uint32_t)(((ss1.total_consumed_bytes + (hybrid_mode ? ss2.total_consumed_bytes : 0)) * 8) / cur_sec / 1000));
                }
                atomic_store(&p_current_sec, cur_sec);
            } else {
                if (!ss1.pending_append && out_samples1 == 0) {
                    uint32_t remain; 
                    DANAStreamingDecoder_GetRemainDataSize(ss1.decoder, &remain);
                    if (remain == 0 || ss1.last_res == DANA_APIRESULT_INSUFFICIENT_DATA_SIZE) break;
                }
            }
        }
        
        snd_pcm_drain(pcm_handle);
        snd_pcm_close(pcm_handle);
        free_stream_state(&ss1);
        if (hybrid_mode) free_stream_state(&ss2);
        
        for(uint32_t i=0; i < ss1.header.wave_format.num_channels; i++) {
            free(pcm_out1[i]);
            free(q1[i]);
            if (hybrid_mode) {
                free(pcm_out2[i]);
                free(q2[i]);
            }
        }
        free(pcm_out1); free(q1);
        if (hybrid_mode) { free(pcm_out2); free(q2); }
        free(interleaved);

        cmd = atomic_load(&current_cmd_atomic);
        if (atomic_load(&play_state_atomic) == STATE_PLAYING && cmd == CMD_NONE) {
            atomic_store(&play_state_atomic, STATE_STOPPED);
            atomic_store(&current_cmd_atomic, CMD_NEXT);
        } else if (cmd == CMD_STOP) {
            atomic_store(&play_state_atomic, STATE_STOPPED);
            atomic_store(&current_cmd_atomic, CMD_NONE);
        }
    }
    return NULL;
}

static void draw_box(int y, int x, int h, int w, const char* title, int color_pair) {
    attron(COLOR_PAIR(color_pair));
    mvhline(y, x+1, ACS_HLINE, w-2);
    mvhline(y+h-1, x+1, ACS_HLINE, w-2);
    mvvline(y+1, x, ACS_VLINE, h-2);
    mvvline(y+1, x+w-1, ACS_VLINE, h-2);
    mvaddch(y, x, ACS_ULCORNER);
    mvaddch(y, x+w-1, ACS_URCORNER);
    mvaddch(y+h-1, x, ACS_LLCORNER);
    mvaddch(y+h-1, x+w-1, ACS_LRCORNER);
    if (title) { attron(A_REVERSE); mvprintw(y, x + 2, " %s ", title); attroff(A_REVERSE); }
    attroff(COLOR_PAIR(color_pair));
}

static void ui_loop(void) {
    if (force_redraw) {
        erase();
        force_redraw = false;
    }

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int left_w  = is_fullscreen ? 0 : 35;
    int right_w = max_x - left_w;
    int bottom_h = is_fullscreen ? 0 : 12;
    int top_h   = max_y - bottom_h;

    int draw_w = right_w - 4;
    int draw_h = top_h - 4;
    if (draw_w < 1) draw_w = 1;
    if (draw_h < 1) draw_h = 1;

    int px_w = draw_w * 2;
    int px_h = draw_h * 4;

    static struct DANAMetadata cached_meta = {0};
    static struct DANAWaveFormat cached_fmt = {0};
    static uint32_t cached_bps = 0;
    static int cached_idx = -2; 
    static int header_loaded_for_idx = -2;
    static char cached_filename[256] = "";

    pthread_mutex_lock(&state_mutex);
    if (playing_file_idx != cached_idx) {
        DANAMetadata_Release(&cached_meta);
        memset(&cached_meta, 0, sizeof(cached_meta));
        memset(&cached_fmt, 0, sizeof(cached_fmt));
        cached_bps = 0;
        cached_filename[0] = '\0';
        cached_idx = playing_file_idx;
        header_loaded_for_idx = -2;
        force_redraw = true;
    }

    if (header_loaded_for_idx != playing_file_idx) {
        if (atomic_load(&header_ready_for_idx) == playing_file_idx) {
            DANAMetadata_Release(&cached_meta);
            memset(&cached_meta, 0, sizeof(cached_meta));
            DANAMetadata_Copy(&cached_meta, &p_header.metadata);
            cached_fmt = p_header.wave_format;
            cached_bps = p_header.max_bit_per_second;
            strncpy(cached_filename, playing_filename, 255);
            header_loaded_for_idx = playing_file_idx;
            force_redraw = true;
        }
    }
    pthread_mutex_unlock(&state_mutex);

    static float *display_buf = NULL;
    static uint8_t *grid = NULL;
    static float smooth_bars[4096] = {0};
    static int last_draw_w = 0, last_draw_h = 0;

    if (draw_w != last_draw_w || draw_h != last_draw_h) {
        if (display_buf) free(display_buf);
        if (grid) free(grid);
        display_buf = malloc(sizeof(float) * 8192);
        grid = calloc((size_t)(draw_w * draw_h), sizeof(uint8_t));
        last_draw_w = draw_w; last_draw_h = draw_h;
        memset(smooth_bars, 0, sizeof(smooth_bars));
    } else if (grid) {
        memset(grid, 0, (size_t)(draw_w * draw_h));
    }

    static uint32_t smooth_rpos = 0;
    uint32_t target_play_pos = atomic_load(&vis_play_pos);
    uint32_t srate = atomic_load(&vis_srate);
    if (srate == 0) srate = 44100;
    uint32_t nominal_advance = (srate * 15u) / 1000u;
    int32_t diff = (int32_t)(target_play_pos - smooth_rpos);
    if (abs(diff) > (int32_t)srate) smooth_rpos = target_play_pos;
    else smooth_rpos += nominal_advance + (diff / 5); 

    uint32_t tot_sec = atomic_load(&p_total_sec);
    
    if (!is_fullscreen) {
        draw_box(0, 0, top_h, left_w, "files", 1);
        mvprintw(1, 1, " %s", current_dir);
        attron(COLOR_PAIR(4)); mvhline(2, 1, ACS_HLINE, left_w-2); attroff(COLOR_PAIR(4));
        
        int list_h = top_h - 4;
        for (int i = 0; i < list_h; i++) mvhline(i + 3, 1, ' ', left_w - 2);
        for (int i = 0; i < list_h && i + scroll_offset < num_files; i++) {
            int idx = i + scroll_offset;
            if (idx == selected_file_idx) attron(A_REVERSE | COLOR_PAIR(1));
            else if (files[idx].is_dir) attron(COLOR_PAIR(3));
            else attron(COLOR_PAIR(2));
            mvprintw(i + 3, 2, "%-30.30s", files[idx].name);
            if (idx == selected_file_idx) attroff(A_REVERSE | COLOR_PAIR(1));
            else if (files[idx].is_dir) attroff(COLOR_PAIR(3));
            else attroff(COLOR_PAIR(2));
        }

        draw_box(top_h, 0, bottom_h, left_w, "information", 1);
        mvprintw(top_h + 2, 2, "Artist      : %-30.30s", (cached_meta.artist && strlen(cached_meta.artist) > 0) ? cached_meta.artist : "<Empty>");
        mvprintw(top_h + 3, 2, "Title       : %-30.30s", (cached_meta.title  && strlen(cached_meta.title)  > 0) ? cached_meta.title  : "<Empty>");
        mvprintw(top_h + 4, 2, "Album       : %-30.30s", (cached_meta.album  && strlen(cached_meta.album)  > 0) ? cached_meta.album  : "<Empty>");
        mvprintw(top_h + 5, 2, "Channels    : %-9u", cached_fmt.num_channels);
        mvprintw(top_h + 6, 2, "Sample rate : %-9u", cached_fmt.sampling_rate);
        mvprintw(top_h + 7, 2, "Bit rate    : %u kbps", cached_bps / 1000);
        mvprintw(top_h + 8, 2, "Bits/sample : %-9u", cached_fmt.bit_per_sample);
        mvprintw(top_h + 9, 2, "Duration    : %02u:%02u", tot_sec / 60, tot_sec % 60);
    }

    draw_box(0, left_w, top_h, right_w, is_fullscreen ? (playing_file_idx >= 0 ? cached_filename : "player") : NULL, 1);
    mvprintw(0, left_w + 2, " ");
    attron(active_tab == 1 ? A_REVERSE : A_NORMAL); 
    if (current_vis_mode == 0) printw("1:Oscilloscope"); else printw("1:Spectrum");
    attroff(active_tab == 1 ? A_REVERSE : A_NORMAL); printw(" ");
    attron(active_tab == 2 ? A_REVERSE : A_NORMAL); printw("2:Codec info"); attroff(active_tab == 2 ? A_REVERSE : A_NORMAL); printw(" ");
    attron(active_tab == 3 ? A_REVERSE : A_NORMAL); printw("3:lyric"); attroff(active_tab == 3 ? A_REVERSE : A_NORMAL);
    printw(" -["); attron(COLOR_PAIR(5)); printw("C:Switch Visualizer"); attroff(COLOR_PAIR(5)); printw("] ");
    printw("-["); attron(COLOR_PAIR(5)); printw(is_fullscreen ? "F:Windowed" : "F:Fullscreen"); attroff(COLOR_PAIR(5)); printw("] ");

    if (active_tab == 1) {
        if (draw_w > 0 && draw_h > 0 && display_buf && grid) {
            for (int y = 2; y < top_h - 1; y++) mvhline(y, left_w + 2, ' ', draw_w);
            if (current_vis_mode == 0) {
                uint32_t window_samples = (srate * 40u) / 1000u;
                int num_samples = px_w * 2; if (num_samples > 8192) num_samples = 8192;
                for (int x = 0; x < num_samples; x++) {
                    uint32_t neg_off = window_samples - (uint32_t)((x * window_samples) / num_samples);
                    if (neg_off > smooth_rpos) display_buf[x] = 0.0f; 
                    else display_buf[x] = (vis_ring_l[(smooth_rpos - neg_off) & VIS_BUF_MASK] + vis_ring_r[(smooth_rpos - neg_off) & VIS_BUF_MASK]) * 0.5f;
                }
                float peak = 0.01f;
                for (int i = 0; i < num_samples; i++) { float v = fabsf(display_buf[i]); if (v > peak) peak = v; }
                float scale = (1.0f / peak) * ((float)px_h / 2.2f);
                int center_y_px = px_h / 2;
                for (int i = 0; i < num_samples - 1; i++) {
                    int x0 = (i * px_w) / num_samples; int x1 = ((i + 1) * px_w) / num_samples;
                    int y0 = center_y_px - (int)(display_buf[i] * scale);
                    int y1 = center_y_px - (int)(display_buf[i+1] * scale);
                    draw_braille_line(grid, draw_w, draw_h, x0, y0, x1, y1);
                }
            } else {
                float complex X[FFT_SIZE];
                for (int i = 0; i < FFT_SIZE; i++) {
                    uint32_t idx = (smooth_rpos + VIS_BUF_SIZE - FFT_SIZE + i) & VIS_BUF_MASK;
                    float hann = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (FFT_SIZE - 1)));
                    X[i] = (vis_ring_l[idx] + vis_ring_r[idx]) * 0.5f * hann;
                }
                compute_fft(X, FFT_SIZE);
                float min_f = log10f(1.0f); float max_f = log10f((float)(FFT_SIZE / 2));
                for (int x = 0; x < px_w; x++) {
                    float low_bin = powf(10.0f, min_f + (max_f - min_f) * ((float)x / px_w));
                    float high_bin = powf(10.0f, min_f + (max_f - min_f) * ((float)(x + 1) / px_w));
                    int b1 = (int)low_bin; int b2 = (int)high_bin; if (b2 <= b1) b2 = b1 + 1;
                    float peak = 0.0f;
                    for (int b = b1; b < b2 && b < FFT_SIZE / 2; b++) { float mag = cabsf(X[b]); if (mag > peak) peak = mag; }
                    float norm_mag = peak / (FFT_SIZE / 4.0f); 
                    float val = log10f(1.0f + norm_mag * 100.0f) / 2.0f; if (val > 1.0f) val = 1.0f;
                    if (val >= smooth_bars[x]) smooth_bars[x] = val; else { smooth_bars[x] -= 0.04f; if (smooth_bars[x] < 0.0f) smooth_bars[x] = 0.0f; }
                    int bar_h = (int)(smooth_bars[x] * px_h); if (bar_h > px_h) bar_h = px_h;
                    int y1 = px_h - 1; int y0 = px_h - bar_h; if (y0 < 0) y0 = 0;
                    draw_braille_line(grid, draw_w, draw_h, x, y0, x, y1);
                }
            }
            for (int y = 0; y < draw_h; y++) {
                for (int x = 0; x < draw_w; x++) {
                    uint8_t v = grid[y * draw_w + x]; if (v == 0) continue;
                    int color_idx = 6 + (x * 5) / draw_w;
                    if (color_idx > 10) color_idx = 10; if (color_idx < 6) color_idx = 6;
                    attron(COLOR_PAIR(color_idx));
                    char utf8_braille[4] = {(char)0xE2, (char)(0xA0 | (v >> 6)), (char)(0x80 | (v & 0x3F)), '\0'};
                    mvprintw(2 + y, left_w + 2 + x, "%s", utf8_braille);
                    attroff(COLOR_PAIR(color_idx));
                }
            }
        }
    } else if (active_tab == 2) {
        mvprintw(2,  left_w + 4, "Blocks:");
        mvprintw(3,  left_w + 6, "Decoded %u blocks", atomic_load(&p_decoded_blocks));
        mvprintw(4,  left_w + 6, "Played %u buffers", atomic_load(&p_played_buffers));
        mvprintw(5,  left_w + 6, "Lost %u buffers", atomic_load(&p_lost_buffers));
        mvprintw(7,  left_w + 4, "Input/Read:");
        mvprintw(8,  left_w + 6, "Media data size %u KiB", atomic_load(&p_media_data_size_kib));
        mvprintw(9,  left_w + 6, "Input bitrate %u kb/s", atomic_load(&p_input_bitrate_kbs));
        mvprintw(10, left_w + 6, "Demuxed data size %u KiB", atomic_load(&p_demuxed_data_size_kib));
        mvprintw(11, left_w + 6, "Content bitrate %u kb/s", atomic_load(&p_content_bitrate_kbs));
        mvprintw(12, left_w + 6, "Discarded (corrupt) %u", atomic_load(&p_discarded));
        mvprintw(13, left_w + 6, "Dropped (discontinued) %u", atomic_load(&p_dropped));
    } else if (active_tab == 3) {
        for (int y = 2; y < top_h - 1; y++) mvhline(y, left_w + 2, ' ', right_w - 4);
        if (cached_meta.lyrics) {
            int ly = 2;
            char* lyrics_copy = strdup(cached_meta.lyrics);
            char* line = strtok(lyrics_copy, "\n");
            while (line && ly < top_h - 1) {
                mvprintw(ly++, left_w + 4, "%.*s", right_w - 6, line);
                line = strtok(NULL, "\n");
            }
            free(lyrics_copy);
        } else {
            mvprintw(2, left_w + 4, "DanaID: No Lyrics available");
        }
    }

    if (!is_fullscreen) {
        draw_box(top_h, left_w, bottom_h, right_w, "player", 1);
        int center_x = left_w + (right_w / 2);

        uint32_t vu_window = (srate * 50u) / 1000u; // 50ms window
        float peak_l = 0.0f;
        float peak_r = 0.0f;
        for (uint32_t i = 0; i < vu_window; i++) {
            if (i > smooth_rpos) break;
            uint32_t idx = (smooth_rpos - i) & VIS_BUF_MASK;
            float l = fabsf(vis_ring_l[idx]);
            float r = fabsf(vis_ring_r[idx]);
            if (l > peak_l) peak_l = l;
            if (r > peak_r) peak_r = r;
        }
        
        int clip_l = (peak_l > 1.0f);
        int clip_r = (peak_r > 1.0f);
        
        static int clip_hold_l = 0;
        static int clip_hold_r = 0;
        if (clip_l) clip_hold_l = 20;
        else if (clip_hold_l > 0) clip_hold_l--;

        if (clip_r) clip_hold_r = 20;
        else if (clip_hold_r > 0) clip_hold_r--;

        float db_l = (peak_l < 0.001f) ? -60.0f : 20.0f * log10f(peak_l);
        float db_r = (peak_r < 0.001f) ? -60.0f : 20.0f * log10f(peak_r);
        peak_l = (db_l + 40.0f) / 40.0f;
        peak_r = (db_r + 40.0f) / 40.0f;
        if (peak_l < 0.0f) peak_l = 0.0f;
        if (peak_l > 1.0f) peak_l = 1.0f;
        if (peak_r < 0.0f) peak_r = 0.0f;
        if (peak_r > 1.0f) peak_r = 1.0f;

        static float smooth_peak_l = 0.0f;
        static float smooth_peak_r = 0.0f;

        if (peak_l > smooth_peak_l) smooth_peak_l = peak_l;
        else { smooth_peak_l -= 0.03f; if (smooth_peak_l < 0.0f) smooth_peak_l = 0.0f; }

        if (peak_r > smooth_peak_r) smooth_peak_r = peak_r;
        else { smooth_peak_r -= 0.03f; if (smooth_peak_r < 0.0f) smooth_peak_r = 0.0f; }

        int bar_len = (right_w - 10) / 2;
        if (bar_len > 24) bar_len = 24;
        if (bar_len < 5) bar_len = 5;

        int val_l = (int)(smooth_peak_l * bar_len);
        if (val_l > bar_len) val_l = bar_len;
        int val_r = (int)(smooth_peak_r * bar_len);
        if (val_r > bar_len) val_r = bar_len;

        attron(COLOR_PAIR(2));
        mvaddstr(top_h + 3, center_x, "|");
        mvaddstr(top_h + 3, center_x - 1 - bar_len - 2, "L");
        mvaddstr(top_h + 3, center_x + 1 + bar_len + 2, "R");
        attroff(COLOR_PAIR(2));

        for (int i = 0; i < bar_len; i++) {
            int color;
            float pct = (float)i / bar_len;
            if (pct < 0.6f) color = 3; // green
            else if (pct < 0.85f) color = 4; // yellow
            else color = 10; // red
            
            attron(COLOR_PAIR(color));
            if (i < val_l) {
                mvaddstr(top_h + 3, center_x - 1 - i, "\xE2\x96\x88");
            } else {
                mvaddstr(top_h + 3, center_x - 1 - i, " ");
            }
            
            if (i < val_r) {
                mvaddstr(top_h + 3, center_x + 1 + i, "\xE2\x96\x88");
            } else {
                mvaddstr(top_h + 3, center_x + 1 + i, " ");
            }
            attroff(COLOR_PAIR(color));
        }

        if (clip_hold_l > 0) {
            attron(COLOR_PAIR(10) | A_REVERSE);
            mvaddstr(top_h + 3, center_x - 1 - bar_len, " ");
            attroff(COLOR_PAIR(10) | A_REVERSE);
        } else {
            mvaddstr(top_h + 3, center_x - 1 - bar_len, " ");
        }

        if (clip_hold_r > 0) {
            attron(COLOR_PAIR(10) | A_REVERSE);
            mvaddstr(top_h + 3, center_x + 1 + bar_len, " ");
            attroff(COLOR_PAIR(10) | A_REVERSE);
        } else {
            mvaddstr(top_h + 3, center_x + 1 + bar_len, " ");
        }

        mvprintw(top_h + 5, max_x - 14, "Vol: %3d%%", atomic_load(&volume));
        uint32_t cur_sec = atomic_load(&p_current_sec);
        int bar_width = right_w - 18;
        float progress = (tot_sec > 0) ? (float)cur_sec / (float)tot_sec : 0.0f;
        if (progress > 1.0f) progress = 1.0f;
        int pos = (int)(progress * (float)bar_width);
        mvprintw(top_h + 7, left_w + 4, "%02u:%02u ", cur_sec / 60, cur_sec % 60);
        attron(COLOR_PAIR(5));
        for (int i = 0; i < bar_width; i++) {
            if (i < pos) addstr("━"); else if (i == pos) addstr("●"); else addstr("─");
        }
        attroff(COLOR_PAIR(5));
        printw(" %02u:%02u", tot_sec / 60, tot_sec % 60);
        int text_len  = (int)strlen(cached_filename);
        int txt_start = center_x - (text_len / 2);
        if (txt_start < left_w + 2) txt_start = left_w + 2;
        mvprintw(top_h + 9, txt_start, "%.*s", right_w - 4, (playing_file_idx >= 0) ? cached_filename : "<No Song Selected>");
    } else {
        uint32_t cur_sec = atomic_load(&p_current_sec);
        int bar_width = right_w - 26;
        if (bar_width > 0) {
            float progress = (tot_sec > 0) ? (float)cur_sec / (float)tot_sec : 0.0f;
            if (progress > 1.0f) progress = 1.0f;
            int pos = (int)(progress * (float)bar_width);
            mvprintw(top_h - 2, left_w + 4, "%02u:%02u ", cur_sec / 60, cur_sec % 60);
            attron(COLOR_PAIR(5));
            for (int i = 0; i < bar_width; i++) {
                if (i < pos) addstr("━"); else if (i == pos) addstr("●"); else addstr("─");
            }
            attroff(COLOR_PAIR(5));
            printw(" %02u:%02u", tot_sec / 60, tot_sec % 60);
            mvprintw(top_h - 2, max_x - 12, "Vol:%3d%%", atomic_load(&volume));
        }
    }
    refresh();
}

int main(int argc, char **argv) {
    setlocale(LC_ALL, ""); 
    snd_lib_error_set_handler(silent_alsa_error_handler);
    if (argc > 1) { if (chdir(argv[1]) != 0) perror("chdir failed"); }
    load_directory(".");
    
    pthread_t audio_thread;
    pthread_create(&audio_thread, NULL, audio_thread_func, NULL);
    
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0); timeout(15); 
    start_color(); use_default_colors();
    init_pair(1, COLOR_CYAN,    -1); init_pair(2, COLOR_WHITE,   -1); init_pair(3, COLOR_GREEN,   -1);
    init_pair(4, COLOR_YELLOW,  -1); init_pair(5, COLOR_MAGENTA, -1); init_pair(6,  COLOR_BLUE,    -1);
    init_pair(7, COLOR_CYAN,    -1); init_pair(8, COLOR_GREEN,   -1); init_pair(9,  COLOR_YELLOW,  -1);
    init_pair(10, COLOR_RED,     -1);

    bool running = true;
    while (running) {
        ui_loop();
        int ch = getch();
        if (ch != ERR) force_redraw = true;
        switch (ch) {
            case 'q': case 'Q':
                running = false; atomic_store(&current_cmd_atomic, CMD_QUIT); break;
            case KEY_UP:
                if (selected_file_idx > 0) selected_file_idx--;
                if (selected_file_idx < scroll_offset) scroll_offset = selected_file_idx; break;
            case KEY_DOWN:
                if (selected_file_idx < num_files - 1) selected_file_idx++;
                if (selected_file_idx >= scroll_offset + ((LINES - 12) - 4)) scroll_offset = selected_file_idx - ((LINES - 12) - 4) + 1; break;
            case KEY_LEFT:
                if (atomic_load(&play_state_atomic) != STATE_STOPPED) {
                    int t = atomic_load(&p_current_sec) - 5;
                    if (t < 0) t = 0;
                    atomic_store(&seek_target_sec, t);
                    atomic_store(&current_cmd_atomic, CMD_SEEK);
                }
                break;
            case KEY_RIGHT:
                if (atomic_load(&play_state_atomic) != STATE_STOPPED) {
                    int t = atomic_load(&p_current_sec) + 5;
                    int tot = atomic_load(&p_total_sec);
                    if (t > tot) t = tot - 1;
                    if (t < 0) t = 0;
                    atomic_store(&seek_target_sec, t);
                    atomic_store(&current_cmd_atomic, CMD_SEEK);
                }
                break;
            case 10: 
                if (files[selected_file_idx].is_dir) {
                    char new_path[1024]; snprintf(new_path, sizeof(new_path), "%s/%s", current_dir, files[selected_file_idx].name);
                    if (chdir(new_path) == 0) { if (getcwd(current_dir, sizeof(current_dir)) != NULL) load_directory("."); }
                } else {
                    pthread_mutex_lock(&state_mutex);
                    snprintf(playing_filepath, sizeof(playing_filepath), "%s/%s", current_dir, files[selected_file_idx].name);
                    strncpy(playing_filename, files[selected_file_idx].name, 255);
                    playing_file_idx = selected_file_idx;
                    pthread_mutex_unlock(&state_mutex);
                    atomic_store(&current_cmd_atomic, CMD_PLAY);
                } break;
            case ' ': case 'p': atomic_store(&current_cmd_atomic, CMD_PAUSE); break;
            case 'n': case '>': atomic_store(&current_cmd_atomic, CMD_NEXT); break;
            case 'b': case '<': atomic_store(&current_cmd_atomic, CMD_PREV); break;
            case '1': active_tab = 1; break;
            case '2': active_tab = 2; break;
            case '3': active_tab = 3; break;
            case 'c': case 'C': if (active_tab == 1) current_vis_mode = (current_vis_mode + 1) % 2; break;
            case 'f': case 'F': is_fullscreen = !is_fullscreen; break;
            case '+': case '=': if (atomic_load(&volume) < 200) atomic_fetch_add(&volume, 5); break;
            case '-': case '_': if (atomic_load(&volume) > 0) atomic_fetch_sub(&volume, 5); break;
        }
        PlayerCommand cmd = atomic_load(&current_cmd_atomic);
        if (cmd == CMD_NEXT || cmd == CMD_PREV) {
            int step = (cmd == CMD_NEXT) ? 1 : -1; bool found = false;
            pthread_mutex_lock(&state_mutex);
            for (int i = playing_file_idx + step; i >= 0 && i < num_files; i += step) {
                if (!files[i].is_dir) {
                    snprintf(playing_filepath, sizeof(playing_filepath), "%s/%s", current_dir, files[i].name);
                    strncpy(playing_filename, files[i].name, 255);
                    playing_file_idx = i;
                    atomic_store(&current_cmd_atomic, CMD_PLAY);
                    found = true; break;
                }
            }
            pthread_mutex_unlock(&state_mutex);
            if (!found) atomic_store(&current_cmd_atomic, CMD_STOP); 
        }
    }
    endwin();
    pthread_join(audio_thread, NULL);
    DANAMetadata_Release(&p_header.metadata);
    return 0;
}