#include "ui_animations.h"
#include "ui_common.h"
#include <string.h>

void get_marquee_text(const char *text, int max_disp_len, unsigned long frame_counter, char *out_buf, size_t out_buf_size) {
    if (!text || max_disp_len <= 0 || out_buf_size == 0) {
        if (out_buf_size > 0) out_buf[0] = '\0';
        return;
    }

    int text_width = utf8_display_width(text);

    // If it fits entirely, just return the text
    if (text_width <= max_disp_len) {
        int bytes = utf8_byte_offset_for_width(text, max_disp_len);
        if ((size_t)bytes >= out_buf_size) bytes = out_buf_size - 1;
        memset(out_buf, 0, out_buf_size);
        strncpy(out_buf, text, bytes);
        return;
    }

    // Continuous circular scroll parameters
    int gap_width = 3;
    int virtual_width = text_width + gap_width;

    int scroll_speed = 6; 
    int hold_frames = 50; 
    int cycle_frames = hold_frames + virtual_width * scroll_speed;
    int cycle_pos = frame_counter % cycle_frames;

    int width_offset = 0;
    if (cycle_pos >= hold_frames) {
        width_offset = (cycle_pos - hold_frames) / scroll_speed;
    }

    // Duplicate string to seamlessly wrap (max size of names is 255 bytes, 1024 is safe enough)
    char repeated[1024];
    snprintf(repeated, sizeof(repeated), "%s   %s", text, text);

    int start_byte = utf8_byte_offset_for_width(repeated, width_offset);
    int end_byte = start_byte + utf8_byte_offset_for_width(repeated + start_byte, max_disp_len);

    int copy_bytes = end_byte - start_byte;
    if ((size_t)copy_bytes >= out_buf_size) copy_bytes = out_buf_size - 1;

    memset(out_buf, 0, out_buf_size);
    strncpy(out_buf, repeated + start_byte, copy_bytes);
}