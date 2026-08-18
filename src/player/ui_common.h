#ifndef UI_COMMON_H
#define UI_COMMON_H

#include <ncurses.h>
#include <stdint.h>
#include "state.h"

extern unsigned long ui_frame_counter;
extern int ui_last_selected_idx;

// Cache structs to avoid re-fetching constantly
typedef struct {
    struct DANAMetadata meta;
    struct DANAWaveFormat fmt;
    uint32_t bps;
    int idx;
    char filename[256];
    uint32_t smooth_rpos;
    int header_loaded_for_idx;
} UICache;

extern UICache ui_cache;

// Utility functions
void ui_draw_box(int y, int x, int h, int w, const char* title, int color_pair);
int utf8_strlen(const char *str);
int utf8_byte_offset(const char *str, int char_offset);

// Component draw functions
void draw_files_panel(int y, int x, int h, int w);
void draw_info_panel(int y, int x, int h, int w);
void draw_vis_panel(int y, int x, int h, int w);
void draw_player_panel(int y, int x, int h, int w);

#endif // UI_COMMON_H