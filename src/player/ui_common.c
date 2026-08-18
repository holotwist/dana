#include "ui_common.h"

unsigned long ui_frame_counter = 0;
int ui_last_selected_idx = -1;
UICache ui_cache = { .idx = -2, .header_loaded_for_idx = -2 };

int utf8_strlen(const char *str) {
    int len = 0;
    for (int i = 0; str[i] != '\0'; ) {
        unsigned char c = (unsigned char)str[i];
        if ((c & 0x80) == 0) i += 1;
        else if ((c & 0xE0) == 0xC0) i += 2;
        else if ((c & 0xF0) == 0xE0) i += 3;
        else if ((c & 0xF8) == 0xF0) i += 4;
        else i += 1; 
        len++;
    }
    return len;
}

int utf8_byte_offset(const char *str, int char_offset) {
    int byte_offset = 0;
    for (int i = 0; i < char_offset && str[byte_offset] != '\0'; i++) {
        unsigned char c = (unsigned char)str[byte_offset];
        if ((c & 0x80) == 0) byte_offset += 1;
        else if ((c & 0xE0) == 0xC0) byte_offset += 2;
        else if ((c & 0xF0) == 0xE0) byte_offset += 3;
        else if ((c & 0xF8) == 0xF0) byte_offset += 4;
        else byte_offset += 1; 
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