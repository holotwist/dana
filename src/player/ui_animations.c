#include "ui_animations.h"
#include "ui_common.h"
#include <string.h>

void get_marquee_text(const char *text, int max_disp_len, unsigned long frame_counter, char *out_buf, size_t out_buf_size) {
    if (!text || max_disp_len <= 0 || out_buf_size == 0) {
        if (out_buf_size > 0) out_buf[0] = '\0';
        return;
    }

    int text_width = utf8_display_width(text);
    int width_offset = 0;

    if (text_width > max_disp_len) {
        int max_scroll = text_width - max_disp_len;
        int scroll_speed = 6; 
        int hold_frames = 50; 
        int cycle_frames = hold_frames * 2 + max_scroll * scroll_speed;
        int cycle_pos = frame_counter % cycle_frames;

        if (cycle_pos < hold_frames) width_offset = 0;
        else if (cycle_pos < hold_frames + max_scroll * scroll_speed) width_offset = (cycle_pos - hold_frames) / scroll_speed;
        else width_offset = max_scroll;
    }

    int start_byte = utf8_byte_offset_for_width(text, width_offset);
    int end_byte = start_byte + utf8_byte_offset_for_width(text + start_byte, max_disp_len);
    
    int copy_bytes = end_byte - start_byte;
    if ((size_t)copy_bytes >= out_buf_size) copy_bytes = out_buf_size - 1;
    
    memset(out_buf, 0, out_buf_size);
    strncpy(out_buf, text + start_byte, copy_bytes);
}