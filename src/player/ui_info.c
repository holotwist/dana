#include "ui_common.h"
#include <string.h>

void draw_info_panel(int y, int x, int h, int w) {
    if (h < 3 || w < 2) return;
    ui_draw_box(y, x, h, w, "information", 1);
    
    int text_w = w - 4;
    if (text_w < 10) return;
    
    uint32_t tot_sec = atomic_load(&p_total_sec);
    int line = y + 1;
    
    if (++line < y + h - 1) mvprintw(line, x + 2, "Artist      : %.*s", text_w - 14, (ui_cache.meta.artist && strlen(ui_cache.meta.artist) > 0) ? ui_cache.meta.artist : "<Empty>");
    if (++line < y + h - 1) mvprintw(line, x + 2, "Title       : %.*s", text_w - 14, (ui_cache.meta.title  && strlen(ui_cache.meta.title)  > 0) ? ui_cache.meta.title  : "<Empty>");
    if (++line < y + h - 1) mvprintw(line, x + 2, "Album       : %.*s", text_w - 14, (ui_cache.meta.album  && strlen(ui_cache.meta.album)  > 0) ? ui_cache.meta.album  : "<Empty>");
    if (++line < y + h - 1) mvprintw(line, x + 2, "Channels    : %-9u", ui_cache.fmt.num_channels);
    if (++line < y + h - 1) mvprintw(line, x + 2, "Sample rate : %-9u", ui_cache.fmt.sampling_rate);
    if (++line < y + h - 1) mvprintw(line, x + 2, "Bit rate    : %u kbps", ui_cache.bps / 1000);
    if (++line < y + h - 1) mvprintw(line, x + 2, "Bits/sample : %-9u", ui_cache.fmt.bit_per_sample);
    if (++line < y + h - 1) mvprintw(line, x + 2, "Duration    : %02u:%02u", tot_sec / 60, tot_sec % 60);
}