#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "ui.h"
#include "ui_common.h"
#include "state.h"
#include "file_list.h"
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static void ui_update_state(void) {
    ui_frame_counter++;

    pthread_mutex_lock(&state_mutex);
    if (playing_file_idx != ui_cache.idx) {
        DANAMetadata_Release(&ui_cache.meta);
        memset(&ui_cache.meta, 0, sizeof(ui_cache.meta));
        memset(&ui_cache.fmt, 0, sizeof(ui_cache.fmt));
        ui_cache.bps = 0;
        ui_cache.filename[0] = '\0';
        
        if (playing_file_idx >= 0 && playing_file_idx < num_files) {
            selected_file_idx = playing_file_idx;
        }
        ui_cache.idx = playing_file_idx;
        ui_cache.header_loaded_for_idx = -2;
        force_redraw = true;
    }

    if (ui_cache.header_loaded_for_idx != playing_file_idx) {
        if (atomic_load(&header_ready_for_idx) == playing_file_idx) {
            DANAMetadata_Release(&ui_cache.meta);
            memset(&ui_cache.meta, 0, sizeof(ui_cache.meta));
            DANAMetadata_Copy(&ui_cache.meta, &p_header.metadata);
            ui_cache.fmt = p_header.wave_format;
            ui_cache.bps = p_header.max_bit_per_second;
            strncpy(ui_cache.filename, playing_filename, 255);
            ui_cache.header_loaded_for_idx = playing_file_idx;
            force_redraw = true;
        }
    }
    pthread_mutex_unlock(&state_mutex);

    // Auto-switch away from lyrics tab if lyrics aren't available
    if (active_tab == 3 && ui_cache.meta.lyrics == NULL) {
        active_tab = 1;
        force_redraw = true;
    }

    uint32_t target_play_pos = atomic_load(&vis_play_pos);
    uint32_t srate = atomic_load(&vis_srate);
    if (srate == 0) srate = 44100;
    uint32_t nominal_advance = (srate * 15u) / 1000u;
    int32_t diff = (int32_t)(target_play_pos - ui_cache.smooth_rpos);
    
    if (abs(diff) > (int32_t)srate) ui_cache.smooth_rpos = target_play_pos;
    else ui_cache.smooth_rpos += nominal_advance + (diff / 5); 
}

static void ui_loop(void) {
    if (force_redraw) {
        erase();
        force_redraw = false;
    }

    ui_update_state();

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    bool is_vertical = (max_y * 2 > max_x); // Check if we are in vertical/narrow screen

    if (is_fullscreen) {
        if (max_y > 4) {
            draw_vis_panel(0, 0, max_y - 3, max_x);
            draw_player_panel(max_y - 3, 0, 3, max_x);
        } else {
            draw_vis_panel(0, 0, max_y, max_x);
        }
    } else if (is_vertical) {
        // Vertical Split Structure
        int vis_h = max_y * 50 / 100;
        int files_h = max_y * 30 / 100;
        int bottom_h = max_y - vis_h - files_h;
        
        // If the screen is too narrow, hide the info panel entirely
        int info_w = (max_x < 65) ? 0 : (max_x * 35 / 100);
        int player_w = max_x - info_w;

        draw_vis_panel(0, 0, vis_h, max_x);
        draw_files_panel(vis_h, 0, files_h, max_x);
        
        if (info_w > 0) {
            draw_info_panel(vis_h + files_h, 0, bottom_h, info_w);
        }
        draw_player_panel(vis_h + files_h, info_w, bottom_h, player_w);
    } else {
        // Horizontal Split Structure
        int left_w  = 35;
        int right_w = max_x - left_w;
        int bottom_h = 12;
        int top_h   = max_y - bottom_h;

        draw_files_panel(0, 0, top_h, left_w);
        draw_info_panel(top_h, 0, bottom_h, left_w);
        draw_vis_panel(0, left_w, top_h, right_w);
        draw_player_panel(top_h, left_w, bottom_h, right_w);
    }

    refresh();
}

void ui_run(void) {
    initscr(); cbreak(); noecho(); keypad(stdscr, TRUE); curs_set(0); timeout(15); 
    start_color(); use_default_colors();
    init_pair(1, COLOR_CYAN,    -1); init_pair(2, COLOR_WHITE,   -1); init_pair(3, COLOR_GREEN,   -1);
    init_pair(4, COLOR_YELLOW,  -1); init_pair(5, COLOR_MAGENTA, -1); init_pair(6,  COLOR_BLUE,    -1);
    init_pair(7, COLOR_CYAN,    -1); init_pair(8, COLOR_GREEN,   -1); init_pair(9,  COLOR_YELLOW,  -1);
    init_pair(10, COLOR_RED,     -1);

    bool running = true;
    while (running) {
        ui_loop();
        int ch = getch();
        if (ch != ERR) force_redraw = true;
        switch (ch) {
            case 'q': case 'Q':
                running = false; atomic_store(&current_cmd_atomic, CMD_QUIT); break;
            case KEY_UP:
                if (selected_file_idx > 0) selected_file_idx--;
                break;
            case KEY_DOWN:
                if (selected_file_idx < num_files - 1) selected_file_idx++;
                break;
            case KEY_LEFT:
                if (atomic_load(&play_state_atomic) != STATE_STOPPED) {
                    int t = atomic_load(&p_current_sec) - 5;
                    if (t < 0) t = 0;
                    atomic_store(&seek_target_sec, t);
                    atomic_store(&current_cmd_atomic, CMD_SEEK);
                }
                break;
            case KEY_RIGHT:
                if (atomic_load(&play_state_atomic) != STATE_STOPPED) {
                    int t = atomic_load(&p_current_sec) + 5;
                    int tot = atomic_load(&p_total_sec);
                    if (t > tot) t = tot - 1;
                    if (t < 0) t = 0;
                    atomic_store(&seek_target_sec, t);
                    atomic_store(&current_cmd_atomic, CMD_SEEK);
                }
                break;
            case 10: 
                if (files[selected_file_idx].is_dir) {
                    char new_path[1024]; snprintf(new_path, sizeof(new_path), "%s/%s", current_dir, files[selected_file_idx].name);
                    if (chdir(new_path) == 0) { if (getcwd(current_dir, sizeof(current_dir)) != NULL) load_directory("."); }
                } else {
                    pthread_mutex_lock(&state_mutex);
                    snprintf(playing_filepath, sizeof(playing_filepath), "%s/%s", current_dir, files[selected_file_idx].name);
                    strncpy(playing_filename, files[selected_file_idx].name, 255);
                    playing_file_idx = selected_file_idx;
                    pthread_mutex_unlock(&state_mutex);
                    atomic_store(&current_cmd_atomic, CMD_PLAY);
                } break;
            case ' ': case 'p': atomic_store(&current_cmd_atomic, CMD_PAUSE); break;
            case 'n': case '>': atomic_store(&current_cmd_atomic, CMD_NEXT); break;
            case 'b': case '<': atomic_store(&current_cmd_atomic, CMD_PREV); break;
            case '1': active_tab = 1; break;
            case '2': active_tab = 2; break;
            case '3': if (ui_cache.meta.lyrics != NULL) active_tab = 3; break;
            case 'c': case 'C': if (active_tab == 1) current_vis_mode = (current_vis_mode + 1) % 2; break;
            case 'f': case 'F': is_fullscreen = !is_fullscreen; break;
            case '+': case '=': if (atomic_load(&volume) < 200) atomic_fetch_add(&volume, 5); break;
            case '-': case '_': if (atomic_load(&volume) > 0) atomic_fetch_sub(&volume, 5); break;
        }
        PlayerCommand cmd = atomic_load(&current_cmd_atomic);
        if (cmd == CMD_NEXT || cmd == CMD_PREV) {
            int step = (cmd == CMD_NEXT) ? 1 : -1; bool found = false;
            pthread_mutex_lock(&state_mutex);
            for (int i = playing_file_idx + step; i >= 0 && i < num_files; i += step) {
                if (!files[i].is_dir) {
                    snprintf(playing_filepath, sizeof(playing_filepath), "%s/%s", current_dir, files[i].name);
                    strncpy(playing_filename, files[i].name, 255);
                    playing_file_idx = i;
                    atomic_store(&current_cmd_atomic, CMD_PLAY);
                    found = true; break;
                }
            }
            pthread_mutex_unlock(&state_mutex);
            if (!found) atomic_store(&current_cmd_atomic, CMD_STOP); 
        }
    }
    
    DANAMetadata_Release(&ui_cache.meta);
    endwin();
}