#include "ui_common.h"
#include "file_list.h"
#include <string.h>

void draw_files_panel(int y, int x, int h, int w) {
    if (h < 5 || w < 2) return;
    ui_draw_box(y, x, h, w, "files", 1);
    
    int list_h = h - 4;
    
    // Auto-follow cursor
    if (list_h > 0) {
        if (selected_file_idx < scroll_offset) {
            scroll_offset = selected_file_idx;
        } else if (selected_file_idx >= scroll_offset + list_h) {
            scroll_offset = selected_file_idx - list_h + 1;
        }
    }

    mvprintw(y + 1, x + 1, " %s", current_dir);
    attron(COLOR_PAIR(4)); 
    mvhline(y + 2, x + 1, ACS_HLINE, w - 2); 
    attroff(COLOR_PAIR(4));
    
    for (int i = 0; i < list_h; i++) mvhline(y + i + 3, x + 1, ' ', w - 2);
    
    if (ui_last_selected_idx != selected_file_idx) {
        ui_last_selected_idx = selected_file_idx;
        ui_frame_counter = 0; // Reset scroll animation
    }

    int max_disp_len = w - 4;
    if (max_disp_len < 1) max_disp_len = 1;

    for (int i = 0; i < list_h && i + scroll_offset < num_files; i++) {
        int idx = i + scroll_offset;
        
        if (idx == selected_file_idx) attron(A_REVERSE | COLOR_PAIR(1));
        else if (files[idx].is_dir) attron(COLOR_PAIR(3));
        else attron(COLOR_PAIR(2));
        
        int name_chars = utf8_strlen(files[idx].name);
        int char_offset = 0;
        
        // Marquee Text Sliding Effect
        if (idx == selected_file_idx && name_chars > max_disp_len) {
            int max_scroll = name_chars - max_disp_len;
            int scroll_speed = 6; 
            int hold_frames = 50; 
            int cycle_frames = hold_frames * 2 + max_scroll * scroll_speed;
            int cycle_pos = ui_frame_counter % cycle_frames;

            if (cycle_pos < hold_frames) char_offset = 0;
            else if (cycle_pos < hold_frames + max_scroll * scroll_speed) char_offset = (cycle_pos - hold_frames) / scroll_speed;
            else char_offset = max_scroll;
        }

        int byte_off = utf8_byte_offset(files[idx].name, char_offset);
        int copy_bytes = utf8_byte_offset(files[idx].name + byte_off, max_disp_len);
        char disp_buf[256] = {0};
        if (copy_bytes > 255) copy_bytes = 255;
        strncpy(disp_buf, files[idx].name + byte_off, copy_bytes);
        
        int chars_copied = utf8_strlen(disp_buf);
        mvprintw(y + i + 3, x + 2, "%s", disp_buf);
        
        for (int p = chars_copied; p < max_disp_len; p++) printw(" ");

        if (idx == selected_file_idx) attroff(A_REVERSE | COLOR_PAIR(1));
        else if (files[idx].is_dir) attroff(COLOR_PAIR(3));
        else attroff(COLOR_PAIR(2));
    }
}