#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "audio.h"
#include "state.h"
#include "DANADecoder.h"
#include "DANAUtility.h"

#define MINIAUDIO_IMPLEMENTATION
#include "thirdparty/miniaudio.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>
#include <time.h>

#define BUFFER_CHUNK_SIZE 16384

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

static void data_callback(ma_device* pDevice, void* pOutput, const void* pInput, ma_uint32 frameCount) {
    (void)pInput;
    ma_pcm_rb* pRingBuffer = (ma_pcm_rb*)pDevice->pUserData;
    ma_uint32 framesReadTotal = 0;
    ma_uint32 bpf = ma_get_bytes_per_frame(pDevice->playback.format, pDevice->playback.channels);
    uint8_t* pOut = (uint8_t*)pOutput;

    while (framesReadTotal < frameCount) {
        ma_uint32 framesToRead = frameCount - framesReadTotal;
        void* pReadBuffer;
        
        ma_pcm_rb_acquire_read(pRingBuffer, &framesToRead, &pReadBuffer);
        if (framesToRead == 0) break;
        
        memcpy(pOut, pReadBuffer, framesToRead * bpf);
        ma_pcm_rb_commit_read(pRingBuffer, framesToRead);
        
        pOut += framesToRead * bpf;
        framesReadTotal += framesToRead;
    }
    
    if (framesReadTotal < frameCount) {
        memset(pOut, 0, (frameCount - framesReadTotal) * bpf);
    }
    atomic_fetch_add(&p_frames_consumed, framesReadTotal);
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

void *audio_thread_func(void *arg) {
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
            atomic_store(&p_frames_consumed, 0);
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

        ma_pcm_rb ring_buffer;
        ma_pcm_rb_init(ma_format_s32, p_header.wave_format.num_channels, p_header.wave_format.sampling_rate / 2, NULL, NULL, &ring_buffer);
        
        ma_device_config deviceConfig = ma_device_config_init(ma_device_type_playback);
        deviceConfig.playback.format   = ma_format_s32;
        deviceConfig.playback.channels = p_header.wave_format.num_channels;
        deviceConfig.sampleRate        = p_header.wave_format.sampling_rate;
        deviceConfig.dataCallback      = data_callback;
        deviceConfig.pUserData         = &ring_buffer;
        
        ma_device device;
        if (ma_device_init(NULL, &deviceConfig, &device) != MA_SUCCESS) {
            ma_pcm_rb_uninit(&ring_buffer);
            free_stream_state(&ss1); if(hybrid_mode) free_stream_state(&ss2); atomic_store(&play_state_atomic, STATE_STOPPED); continue;
        }
        ma_device_start(&device);
        bool device_active = true;

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
                        ma_device_stop(&device);
                        ma_pcm_rb_uninit(&ring_buffer);
                        ma_pcm_rb_init(ma_format_s32, p_header.wave_format.num_channels, p_header.wave_format.sampling_rate / 2, NULL, NULL, &ring_buffer);
                        atomic_store(&p_frames_consumed, 0);
                        ma_device_start(&device);
                        device_active = true;
                    }
                }
                continue;
            }
            if (atomic_load(&play_state_atomic) == STATE_PAUSED) {
                if (device_active) {
                    ma_device_stop(&device);
                    device_active = false;
                }
                nanosleep(&sleep_ts, NULL); continue;
            } else if (!device_active) {
                ma_device_start(&device);
                device_active = true;
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
                    ma_uint32 framesToWrite = mix_samples - written;
                    void* pWriteBuffer;
                    
                    ma_pcm_rb_acquire_write(&ring_buffer, &framesToWrite, &pWriteBuffer);
                    
                    if (framesToWrite > 0) {
                        memcpy(pWriteBuffer, interleaved + (written * ss1.header.wave_format.num_channels), 
                               framesToWrite * sizeof(int32_t) * ss1.header.wave_format.num_channels);
                        ma_pcm_rb_commit_write(&ring_buffer, framesToWrite);
                        written += framesToWrite;
                    } else {
                        struct timespec sleep_ts_write = {0, 5000000L}; // 5ms sleep if full
                        nanosleep(&sleep_ts_write, NULL);
                    }
                }

                atomic_store(&vis_play_pos, atomic_load(&p_frames_consumed));

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
        
        ma_device_uninit(&device);
        ma_pcm_rb_uninit(&ring_buffer);
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