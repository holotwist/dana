#include "ui_animations.h"
#include "ui_common.h"
#include <string.h>

void get_marquee_text(const char *text, int max_disp_len, unsigned long frame_counter, char *out_buf, size_t out_buf_size) {
    if (!text || max_disp_len <= 0 || out_buf_size == 0) {
        if (out_buf_size > 0) out_buf[0] = '\0';
        return;
    }

    int name_chars = utf8_strlen(text);
    int char_offset = 0;

    if (name_chars > max_disp_len) {
        int max_scroll = name_chars - max_disp_len;
        int scroll_speed = 6; 
        int hold_frames = 50; 
        int cycle_frames = hold_frames * 2 + max_scroll * scroll_speed;
        int cycle_pos = frame_counter % cycle_frames;

        if (cycle_pos < hold_frames) char_offset = 0;
        else if (cycle_pos < hold_frames + max_scroll * scroll_speed) char_offset = (cycle_pos - hold_frames) / scroll_speed;
        else char_offset = max_scroll;
    }

    int byte_off = utf8_byte_offset(text, char_offset);
    int copy_bytes = utf8_byte_offset(text + byte_off, max_disp_len);
    
    if ((size_t)copy_bytes >= out_buf_size) copy_bytes = out_buf_size - 1;
    
    memset(out_buf, 0, out_buf_size);
    strncpy(out_buf, text + byte_off, copy_bytes);
}