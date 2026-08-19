#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "ui_common.h"
#include <wchar.h>
#include <stdlib.h>

unsigned long ui_frame_counter = 0;
int ui_last_selected_idx = -1;
UICache ui_cache = { .idx = -2, .header_loaded_for_idx = -2 };

int utf8_display_width(const char *str) {
    if (!str) return 0;
    int width = 0;
    mbstate_t state = {0};
    const char *p = str;
    while (*p != '\0') {
        wchar_t wc;
        size_t len = mbrtowc(&wc, p, MB_CUR_MAX, &state);
        if (len == (size_t)-1 || len == (size_t)-2 || len == 0) {
            if (len == (size_t)-1 || len == (size_t)-2) p++;
            else break;
            continue;
        }
        int w = wcwidth(wc);
        if (w > 0) width += w;
        p += len;
    }
    return width;
}

int utf8_byte_offset_for_width(const char *str, int target_width) {
    if (!str) return 0;
    int current_width = 0;
    int byte_offset = 0;
    mbstate_t state = {0};
    const char *p = str;
    while (*p != '\0') {
        wchar_t wc;
        size_t len = mbrtowc(&wc, p, MB_CUR_MAX, &state);
        if (len == (size_t)-1 || len == (size_t)-2 || len == 0) {
            if (len == (size_t)-1 || len == (size_t)-2) { p++; byte_offset++; }
            else break;
            continue;
        }
        int w = wcwidth(wc);
        if (w < 0) w = 0;
        if (current_width + w > target_width) break;
        current_width += w;
        p += len;
        byte_offset += len;
    }
    return byte_offset;
}

int utf8_byte_offset_for_suffix(const char *str, int target_width) {
    if (!str) return 0;
    int current_width = utf8_display_width(str);
    if (current_width <= target_width) return 0;
    
    int byte_offset = 0;
    mbstate_t state = {0};
    const char *p = str;
    
    while (*p != '\0' && current_width > target_width) {
        wchar_t wc;
        size_t len = mbrtowc(&wc, p, MB_CUR_MAX, &state);
        if (len == (size_t)-1 || len == (size_t)-2 || len == 0) {
            if (len == (size_t)-1 || len == (size_t)-2) { p++; byte_offset++; }
            else break;
            continue;
        }
        int w = wcwidth(wc);
        if (w > 0) current_width -= w;
        p += len;
        byte_offset += len;
    }
    return byte_offset;
}

void ui_draw_box(int y, int x, int h, int w, const char* title, int color_pair) {
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