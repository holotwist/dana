#ifndef PLAYER_STATE_H
#define PLAYER_STATE_H

#include <pthread.h>
#include <stdatomic.h>
#include <stdbool.h>
#include "DANADecoder.h"

#define VIS_BUF_SIZE  65536u
#define VIS_BUF_MASK  (VIS_BUF_SIZE - 1u)
#define FFT_SIZE      1024
#define MAX_FILES     1024

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

/* Shared Globals */
extern pthread_mutex_t state_mutex;

extern char current_dir[1024];
extern FileEntry files[MAX_FILES];
extern int num_files;
extern int selected_file_idx;
extern int scroll_offset;

extern char playing_filepath[1024];
extern char playing_filename[256];
extern int playing_file_idx;
extern struct DANAHeaderInfo p_header;

extern atomic_int  header_ready_for_idx;
extern atomic_int  play_state_atomic;
extern atomic_int  current_cmd_atomic;
extern atomic_int  volume;
extern atomic_int  seek_target_sec;

extern atomic_uint p_decoded_blocks;
extern atomic_uint p_played_buffers;
extern atomic_uint p_lost_buffers;
extern atomic_uint p_media_data_size_kib;
extern atomic_uint p_input_bitrate_kbs;
extern atomic_uint p_demuxed_data_size_kib;
extern atomic_uint p_content_bitrate_kbs;
extern atomic_uint p_discarded;
extern atomic_uint p_dropped;

extern atomic_uint p_current_sec;
extern atomic_uint p_total_sec;

extern int active_tab;
extern int current_vis_mode;
extern bool is_fullscreen;
extern bool force_redraw;

extern float       vis_ring_l[VIS_BUF_SIZE];
extern float       vis_ring_r[VIS_BUF_SIZE];
extern atomic_uint vis_wpos;
extern atomic_uint vis_play_pos;
extern atomic_uint vis_srate;
extern atomic_uint p_frames_consumed;

#endif // PLAYER_STATE_H