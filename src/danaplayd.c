#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <pthread.h>
#include <time.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <alsa/asoundlib.h>
#include <stdatomic.h>
#include <stdbool.h>
#include <signal.h>

#include "DANADecoder.h"
#include "DANAUtility.h"

#define SOCKET_PATH "/tmp/danaplayd.sock"
#define BUFFER_CHUNK_SIZE 16384

typedef enum {
    STATE_STOPPED = 0,
    STATE_PLAYING,
    STATE_PAUSED
} PlayState;

typedef enum {
    CMD_NONE,
    CMD_PLAY,
    CMD_PAUSE,
    CMD_STOP,
    CMD_QUIT,
    CMD_SEEK
} PlayerCommand;

/* Global Playback State */
static atomic_bool daemon_running = true;
static pthread_mutex_t state_mutex = PTHREAD_MUTEX_INITIALIZER;

static char playing_filepath[1024] = "";
static atomic_int play_state_atomic = STATE_STOPPED;
static atomic_int current_cmd_atomic = CMD_NONE;
static atomic_int volume = 100;
static atomic_int seek_target_sec = -1;

static atomic_uint p_current_sec = 0;
static atomic_uint p_total_sec = 0;
static struct DANAHeaderInfo p_header = {0};

typedef struct {
    FILE *fp;
    struct DANAStreamingDecoder *decoder;
    uint8_t *pending_chunk;
    size_t pending_bytes;
    int pending_append;
    struct { uint8_t *ptr; size_t size; size_t consumed; } allocs[256];
    int alloc_head, alloc_tail;
    DANAApiResult last_res;
    struct DANAHeaderInfo header;
} StreamState;

static void silent_alsa_error_handler(const char *file, int line, const char *function, int err, const char *fmt, ...) {
    (void)file; (void)line; (void)function; (void)err; (void)fmt;
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
        }
        if (DANAStreamingDecoder_AppendDataFragment(ss->decoder, chunk, (uint32_t)bytes) != DANA_APIRESULT_OK) {
            ss->pending_chunk = chunk; ss->pending_bytes = bytes; break;
        }
        ss->allocs[ss->alloc_tail].ptr = chunk; ss->allocs[ss->alloc_tail].size = bytes; ss->allocs[ss->alloc_tail].consumed = 0;
        ss->alloc_tail = (ss->alloc_tail + 1) % 256;
    }

    ss->last_res = DANAStreamingDecoder_Decode(ss->decoder, pcm_out, out_max, out_samples);
    if (ss->last_res != DANA_APIRESULT_OK && ss->last_res != DANA_APIRESULT_INSUFFICIENT_DATA_SIZE) {
        *exit_loop = 1;
    }

    const uint8_t *consumed_ptr; uint32_t consumed_size;
    while (DANAStreamingDecoder_CollectDataFragment(ss->decoder, &consumed_ptr, &consumed_size) == DANA_APIRESULT_OK) {
        if (consumed_size > 0 && ss->alloc_head != ss->alloc_tail) {
            ss->allocs[ss->alloc_head].consumed += consumed_size;
            if (ss->allocs[ss->alloc_head].consumed == ss->allocs[ss->alloc_head].size) {
                free(ss->allocs[ss->alloc_head].ptr); ss->alloc_head = (ss->alloc_head + 1) % 256;
            }
        }
    }
}

/* Audio Thread */
static void *audio_thread_func(void *arg) {
    (void)arg;
    struct timespec sleep_ts = {0, 50000000L}; // 50ms idle sleep

    while (atomic_load(&daemon_running)) {
        PlayerCommand cmd = atomic_load(&current_cmd_atomic);
        char filepath[1024];

        if (cmd == CMD_PLAY) {
            pthread_mutex_lock(&state_mutex);
            strncpy(filepath, playing_filepath, sizeof(filepath));
            pthread_mutex_unlock(&state_mutex);
            
            atomic_store(&current_cmd_atomic, CMD_NONE);
            atomic_store(&play_state_atomic, STATE_PLAYING);
            atomic_store(&p_current_sec, 0);
            atomic_store(&p_total_sec, 0);
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

        pthread_mutex_lock(&state_mutex);
        DANAMetadata_Release(&p_header.metadata);
        p_header = ss1.header;
        memset(&ss1.header.metadata, 0, sizeof(struct DANAMetadata)); // transfer ownership
        pthread_mutex_unlock(&state_mutex);
        
        atomic_store(&p_total_sec, (p_header.wave_format.sampling_rate > 0) ? p_header.num_samples / p_header.wave_format.sampling_rate : 0);

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
            if (cmd == CMD_STOP || cmd == CMD_PLAY || cmd == CMD_QUIT) {
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

                        if (hybrid_mode) {
                            DANAStreamingDecoder_Destroy(ss2.decoder);
                            ss2.decoder = DANAStreamingDecoder_Create(&cfg);
                            DANAStreamingDecoder_SetWaveFormat(ss2.decoder, &ss2.header.wave_format);
                            DANAStreamingDecoder_SetEncodeParameter(ss2.decoder, &ss2.header.encode_param);
                            while (ss2.alloc_head != ss2.alloc_tail) { free(ss2.allocs[ss2.alloc_head].ptr); ss2.alloc_head = (ss2.alloc_head + 1) % 256; }
                            if (ss2.pending_chunk) { free(ss2.pending_chunk); ss2.pending_chunk = NULL; }
                            ss2.pending_append = 1;
                        }

                        q1_len = 0;
                        if (hybrid_mode) q2_len = 0;
                        samples_played = out_sample;
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

                uint32_t written = 0;
                while (written < mix_samples && !exit_loop) {
                    snd_pcm_sframes_t frames = snd_pcm_writei(pcm_handle, interleaved + (written * ss1.header.wave_format.num_channels), mix_samples - written);
                    if (frames < 0) {
                        frames = snd_pcm_recover(pcm_handle, (int)frames, 0);
                        if (frames < 0) exit_loop = 1;
                    } else if (frames > 0) {
                        written += (uint32_t)frames;
                    }
                }

                for (uint32_t c = 0; c < ss1.header.wave_format.num_channels; c++) {
                    memmove(q1[c], q1[c] + mix_samples, (q1_len - mix_samples) * sizeof(int32_t));
                    if (hybrid_mode) memmove(q2[c], q2[c] + mix_samples, (q2_len - mix_samples) * sizeof(int32_t));
                }
                q1_len -= mix_samples;
                if (hybrid_mode) q2_len -= mix_samples;

                samples_played += mix_samples;
                atomic_store(&p_current_sec, samples_played / ss1.header.wave_format.sampling_rate);
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
            free(pcm_out1[i]); free(q1[i]);
            if (hybrid_mode) { free(pcm_out2[i]); free(q2[i]); }
        }
        free(pcm_out1); free(q1);
        if (hybrid_mode) { free(pcm_out2); free(q2); }
        free(interleaved);

        cmd = atomic_load(&current_cmd_atomic);
        if (atomic_load(&play_state_atomic) == STATE_PLAYING && cmd == CMD_NONE) {
            atomic_store(&play_state_atomic, STATE_STOPPED);
        } else if (cmd == CMD_STOP) {
            atomic_store(&play_state_atomic, STATE_STOPPED);
            atomic_store(&current_cmd_atomic, CMD_NONE);
        }
    }
    return NULL;
}

/* JSON helper */
static void escape_json_string(const char *src, char *dest) {
    if (!src) { *dest = 0; return; }
    while (*src) {
        if (*src == '"') { *dest++ = '\\'; *dest++ = '"'; }
        else if (*src == '\\') { *dest++ = '\\'; *dest++ = '\\'; }
        else if (*src == '\n') { *dest++ = '\\'; *dest++ = 'n'; }
        else if (*src == '\r') { *dest++ = '\\'; *dest++ = 'r'; }
        else if (*src == '\t') { *dest++ = '\\'; *dest++ = 't'; }
        else { *dest++ = *src; }
        src++;
    }
    *dest = 0;
}

/* Socket Listener */
static void handle_client(int client_fd) {
    char buf[2048];
    memset(buf, 0, sizeof(buf));
    int n = read(client_fd, buf, sizeof(buf) - 1);
    if (n <= 0) return;

    buf[strcspn(buf, "\r\n")] = 0; // Strip newline
    
    if (strncmp(buf, "play ", 5) == 0) {
        pthread_mutex_lock(&state_mutex);
        strncpy(playing_filepath, buf + 5, sizeof(playing_filepath) - 1);
        pthread_mutex_unlock(&state_mutex);
        atomic_store(&current_cmd_atomic, CMD_PLAY);
        write(client_fd, "OK\n", 3);
    } 
    else if (strcmp(buf, "pause") == 0) {
        atomic_store(&current_cmd_atomic, CMD_PAUSE);
        write(client_fd, "OK\n", 3);
    } 
    else if (strcmp(buf, "stop") == 0) {
        atomic_store(&current_cmd_atomic, CMD_STOP);
        write(client_fd, "OK\n", 3);
    } 
    else if (strncmp(buf, "seek ", 5) == 0) {
        int s = atoi(buf + 5);
        if (s < 0) s = 0;
        atomic_store(&seek_target_sec, s);
        atomic_store(&current_cmd_atomic, CMD_SEEK);
        write(client_fd, "OK\n", 3);
    }
    else if (strncmp(buf, "set_vol ", 8) == 0) {
        int v = atoi(buf + 8);
        if (v < 0) v = 0;
        if (v > 200) v = 200;
        atomic_store(&volume, v);
        write(client_fd, "OK\n", 3);
    } 
    else if (strcmp(buf, "get_data") == 0) {
        const char *state_str = "STOPPED";
        PlayState s = atomic_load(&play_state_atomic);
        if (s == STATE_PLAYING) state_str = "PLAYING";
        else if (s == STATE_PAUSED) state_str = "PAUSED";
        
        char *resp = malloc(512 * 1024); // 512KB, maybe long lyrics?
        char safe_file[1024]="", e_title[1024]="Unknown", e_art[1024]="Unknown";
        char e_album[1024]="Unknown", e_year[256]="", e_genre[256]="";
        char e_track[256]="", e_bpm[256]="", e_key[256]="";
        char *e_lyrics = malloc(256 * 1024); e_lyrics[0] = 0;
        int has_cover = 0;

        pthread_mutex_lock(&state_mutex);
        escape_json_string(playing_filepath, safe_file);
        if (p_header.metadata.title) escape_json_string(p_header.metadata.title, e_title);
        if (p_header.metadata.artist) escape_json_string(p_header.metadata.artist, e_art);
        if (p_header.metadata.album) escape_json_string(p_header.metadata.album, e_album);
        if (p_header.metadata.year) escape_json_string(p_header.metadata.year, e_year);
        if (p_header.metadata.genre) escape_json_string(p_header.metadata.genre, e_genre);
        if (p_header.metadata.track) escape_json_string(p_header.metadata.track, e_track);
        if (p_header.metadata.bpm) escape_json_string(p_header.metadata.bpm, e_bpm);
        if (p_header.metadata.key) escape_json_string(p_header.metadata.key, e_key);
        if (p_header.metadata.lyrics) escape_json_string(p_header.metadata.lyrics, e_lyrics);
        if (p_header.metadata.cover_data && p_header.metadata.cover_size > 0) has_cover = 1;
        pthread_mutex_unlock(&state_mutex);

        snprintf(resp, 512 * 1024, 
            "{\"state\": \"%s\", \"file\": \"%s\", \"title\": \"%s\", \"artist\": \"%s\", "
            "\"album\": \"%s\", \"year\": \"%s\", \"genre\": \"%s\", \"track\": \"%s\", "
            "\"bpm\": \"%s\", \"key\": \"%s\", \"lyrics\": \"%s\", \"has_cover\": %s, "
            "\"time\": %u, \"duration\": %u, \"volume\": %d}\n",
            state_str, safe_file, e_title, e_art, e_album, e_year, e_genre, e_track, 
            e_bpm, e_key, e_lyrics, has_cover ? "true" : "false",
            atomic_load(&p_current_sec), atomic_load(&p_total_sec), atomic_load(&volume));
            
        write(client_fd, resp, strlen(resp));
        free(resp); free(e_lyrics);
    } 
    else if (strcmp(buf, "get_cover") == 0) {
        uint8_t *cover_buf = NULL;
        uint32_t cover_size = 0;
        
        // Deep copy out of mutex to avoid freezing playback thread
        pthread_mutex_lock(&state_mutex);
        if (p_header.metadata.cover_data && p_header.metadata.cover_size > 0) {
            cover_size = p_header.metadata.cover_size;
            cover_buf = malloc(cover_size);
            memcpy(cover_buf, p_header.metadata.cover_data, cover_size);
        }
        pthread_mutex_unlock(&state_mutex);

        if (cover_buf) {
            size_t total_written = 0;
            while (total_written < cover_size) {
                ssize_t w = write(client_fd, cover_buf + total_written, cover_size - total_written);
                if (w <= 0) break; // Client disconnected / Broken pipe
                total_written += w;
            }
            free(cover_buf);
        } else {
            write(client_fd, "NONE\n", 5);
        }
    }
    else if (strcmp(buf, "quit") == 0) {
        atomic_store(&daemon_running, false);
        atomic_store(&current_cmd_atomic, CMD_QUIT);
        write(client_fd, "SHUTTING DOWN\n", 14);
    } 
    else {
        write(client_fd, "ERROR: UNKNOWN COMMAND\n", 23);
    }
}

int main(void) {
    printf("Starting Dana Playback Daemon...\n");
    printf("Socket Path: %s\n", SOCKET_PATH);

    // Trap signals for graceful shutdown and broken pipes
    signal(SIGINT, SIG_IGN); 
    signal(SIGTERM, SIG_IGN);
    signal(SIGPIPE, SIG_IGN);
    snd_lib_error_set_handler(silent_alsa_error_handler);

    pthread_t audio_thread;
    pthread_create(&audio_thread, NULL, audio_thread_func, NULL);

    int server_fd = socket(AF_UNIX, SOCK_STREAM, 0);
    if (server_fd < 0) {
        perror("Socket creation failed");
        return 1;
    }

    struct sockaddr_un addr;
    memset(&addr, 0, sizeof(addr));
    addr.sun_family = AF_UNIX;
    strncpy(addr.sun_path, SOCKET_PATH, sizeof(addr.sun_path)-1);

    unlink(SOCKET_PATH);
    if (bind(server_fd, (struct sockaddr*)&addr, sizeof(addr)) == -1) {
        perror("Bind failed");
        close(server_fd);
        return 1;
    }

    if (listen(server_fd, 5) == -1) {
        perror("Listen failed");
        close(server_fd);
        return 1;
    }

    printf("Daemon ready. Listening...\n");

    while (atomic_load(&daemon_running)) {
        int client_fd = accept(server_fd, NULL, NULL);
        if (client_fd >= 0) {
            handle_client(client_fd);
            close(client_fd);
        }
    }

    printf("Daemon shutting down.\n");
    close(server_fd);
    unlink(SOCKET_PATH);
    
    pthread_join(audio_thread, NULL);
    DANAMetadata_Release(&p_header.metadata);

    return 0;
}