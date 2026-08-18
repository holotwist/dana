#define _DEFAULT_SOURCE
#define _XOPEN_SOURCE 600

#include "ui.h"
#include "state.h"
#include "file_list.h"
#include "vis_math.h"
#include <ncurses.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <math.h>

static void draw_box(int y, int x, int h, int w, const char* title, int color_pair) {
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

static void ui_loop(void) {
    if (force_redraw) {
        erase();
        force_redraw = false;
    }

    int max_y, max_x;
    getmaxyx(stdscr, max_y, max_x);

    int left_w  = is_fullscreen ? 0 : 35;
    int right_w = max_x - left_w;
    int bottom_h = is_fullscreen ? 0 : 12;
    int top_h   = max_y - bottom_h;

    int draw_w = right_w - 4;
    int draw_h = top_h - 4;
    if (draw_w < 1) draw_w = 1;
    if (draw_h < 1) draw_h = 1;

    int px_w = draw_w * 2;
    int px_h = draw_h * 4;

    static struct DANAMetadata cached_meta = {0};
    static struct DANAWaveFormat cached_fmt = {0};
    static uint32_t cached_bps = 0;
    static int cached_idx = -2; 
    static int header_loaded_for_idx = -2;
    static char cached_filename[256] = "";

    pthread_mutex_lock(&state_mutex);
    if (playing_file_idx != cached_idx) {
        DANAMetadata_Release(&cached_meta);
        memset(&cached_meta, 0, sizeof(cached_meta));
        memset(&cached_fmt, 0, sizeof(cached_fmt));
        cached_bps = 0;
        cached_filename[0] = '\0';
        cached_idx = playing_file_idx;
        header_loaded_for_idx = -2;
        force_redraw = true;
    }

    if (header_loaded_for_idx != playing_file_idx) {
        if (atomic_load(&header_ready_for_idx) == playing_file_idx) {
            DANAMetadata_Release(&cached_meta);
            memset(&cached_meta, 0, sizeof(cached_meta));
            DANAMetadata_Copy(&cached_meta, &p_header.metadata);
            cached_fmt = p_header.wave_format;
            cached_bps = p_header.max_bit_per_second;
            strncpy(cached_filename, playing_filename, 255);
            header_loaded_for_idx = playing_file_idx;
            force_redraw = true;
        }
    }
    pthread_mutex_unlock(&state_mutex);

    static float *display_buf = NULL;
    static uint8_t *grid = NULL;
    static float smooth_bars[4096] = {0};
    static int last_draw_w = 0, last_draw_h = 0;

    if (draw_w != last_draw_w || draw_h != last_draw_h) {
        if (display_buf) free(display_buf);
        if (grid) free(grid);
        display_buf = malloc(sizeof(float) * 8192);
        grid = calloc((size_t)(draw_w * draw_h), sizeof(uint8_t));
        last_draw_w = draw_w; last_draw_h = draw_h;
        memset(smooth_bars, 0, sizeof(smooth_bars));
    } else if (grid) {
        memset(grid, 0, (size_t)(draw_w * draw_h));
    }

    static uint32_t smooth_rpos = 0;
    uint32_t target_play_pos = atomic_load(&vis_play_pos);
    uint32_t srate = atomic_load(&vis_srate);
    if (srate == 0) srate = 44100;
    uint32_t nominal_advance = (srate * 15u) / 1000u;
    int32_t diff = (int32_t)(target_play_pos - smooth_rpos);
    if (abs(diff) > (int32_t)srate) smooth_rpos = target_play_pos;
    else smooth_rpos += nominal_advance + (diff / 5); 

    uint32_t tot_sec = atomic_load(&p_total_sec);
    
    if (!is_fullscreen) {
        draw_box(0, 0, top_h, left_w, "files", 1);
        mvprintw(1, 1, " %s", current_dir);
        attron(COLOR_PAIR(4)); mvhline(2, 1, ACS_HLINE, left_w-2); attroff(COLOR_PAIR(4));
        
        int list_h = top_h - 4;
        for (int i = 0; i < list_h; i++) mvhline(i + 3, 1, ' ', left_w - 2);
        for (int i = 0; i < list_h && i + scroll_offset < num_files; i++) {
            int idx = i + scroll_offset;
            if (idx == selected_file_idx) attron(A_REVERSE | COLOR_PAIR(1));
            else if (files[idx].is_dir) attron(COLOR_PAIR(3));
            else attron(COLOR_PAIR(2));
            mvprintw(i + 3, 2, "%-30.30s", files[idx].name);
            if (idx == selected_file_idx) attroff(A_REVERSE | COLOR_PAIR(1));
            else if (files[idx].is_dir) attroff(COLOR_PAIR(3));
            else attroff(COLOR_PAIR(2));
        }

        draw_box(top_h, 0, bottom_h, left_w, "information", 1);
        mvprintw(top_h + 2, 2, "Artist      : %-30.30s", (cached_meta.artist && strlen(cached_meta.artist) > 0) ? cached_meta.artist : "<Empty>");
        mvprintw(top_h + 3, 2, "Title       : %-30.30s", (cached_meta.title  && strlen(cached_meta.title)  > 0) ? cached_meta.title  : "<Empty>");
        mvprintw(top_h + 4, 2, "Album       : %-30.30s", (cached_meta.album  && strlen(cached_meta.album)  > 0) ? cached_meta.album  : "<Empty>");
        mvprintw(top_h + 5, 2, "Channels    : %-9u", cached_fmt.num_channels);
        mvprintw(top_h + 6, 2, "Sample rate : %-9u", cached_fmt.sampling_rate);
        mvprintw(top_h + 7, 2, "Bit rate    : %u kbps", cached_bps / 1000);
        mvprintw(top_h + 8, 2, "Bits/sample : %-9u", cached_fmt.bit_per_sample);
        mvprintw(top_h + 9, 2, "Duration    : %02u:%02u", tot_sec / 60, tot_sec % 60);
    }

    draw_box(0, left_w, top_h, right_w, is_fullscreen ? (playing_file_idx >= 0 ? cached_filename : "player") : NULL, 1);
    mvprintw(0, left_w + 2, " ");
    attron(active_tab == 1 ? A_REVERSE : A_NORMAL); 
    if (current_vis_mode == 0) printw("1:Oscilloscope"); else printw("1:Spectrum");
    attroff(active_tab == 1 ? A_REVERSE : A_NORMAL); printw(" ");
    attron(active_tab == 2 ? A_REVERSE : A_NORMAL); printw("2:Codec info"); attroff(active_tab == 2 ? A_REVERSE : A_NORMAL); printw(" ");
    attron(active_tab == 3 ? A_REVERSE : A_NORMAL); printw("3:lyric"); attroff(active_tab == 3 ? A_REVERSE : A_NORMAL);
    printw(" -["); attron(COLOR_PAIR(5)); printw("C:Switch Visualizer"); attroff(COLOR_PAIR(5)); printw("] ");
    printw("-["); attron(COLOR_PAIR(5)); printw(is_fullscreen ? "F:Windowed" : "F:Fullscreen"); attroff(COLOR_PAIR(5)); printw("] ");

    if (active_tab == 1) {
        if (draw_w > 0 && draw_h > 0 && display_buf && grid) {
            for (int y = 2; y < top_h - 1; y++) mvhline(y, left_w + 2, ' ', draw_w);
            if (current_vis_mode == 0) {
                uint32_t window_samples = (srate * 40u) / 1000u;
                int num_samples = px_w * 2; if (num_samples > 8192) num_samples = 8192;
                for (int x = 0; x < num_samples; x++) {
                    uint32_t neg_off = window_samples - (uint32_t)((x * window_samples) / num_samples);
                    if (neg_off > smooth_rpos) display_buf[x] = 0.0f; 
                    else display_buf[x] = (vis_ring_l[(smooth_rpos - neg_off) & VIS_BUF_MASK] + vis_ring_r[(smooth_rpos - neg_off) & VIS_BUF_MASK]) * 0.5f;
                }
                float peak = 0.01f;
                for (int i = 0; i < num_samples; i++) { float v = fabsf(display_buf[i]); if (v > peak) peak = v; }
                float scale = (1.0f / peak) * ((float)px_h / 2.2f);
                int center_y_px = px_h / 2;
                for (int i = 0; i < num_samples - 1; i++) {
                    int x0 = (i * px_w) / num_samples; int x1 = ((i + 1) * px_w) / num_samples;
                    int y0 = center_y_px - (int)(display_buf[i] * scale);
                    int y1 = center_y_px - (int)(display_buf[i+1] * scale);
                    draw_braille_line(grid, draw_w, draw_h, x0, y0, x1, y1);
                }
            } else {
                float complex X[FFT_SIZE];
                for (int i = 0; i < FFT_SIZE; i++) {
                    uint32_t idx = (smooth_rpos + VIS_BUF_SIZE - FFT_SIZE + i) & VIS_BUF_MASK;
                    float hann = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (FFT_SIZE - 1)));
                    float val = (vis_ring_l[idx] + vis_ring_r[idx]) * 0.5f * hann;
                    // Anti-denormal clamping
                    if (fabsf(val) < 1e-15f) val = 0.0f;
                    X[i] = val;
                }
                compute_fft(X, FFT_SIZE);
                float min_f = log10f(1.0f); float max_f = log10f((float)(FFT_SIZE / 2));
                for (int x = 0; x < px_w; x++) {
                    float low_bin = powf(10.0f, min_f + (max_f - min_f) * ((float)x / px_w));
                    float high_bin = powf(10.0f, min_f + (max_f - min_f) * ((float)(x + 1) / px_w));
                    int b1 = (int)low_bin; int b2 = (int)high_bin; if (b2 <= b1) b2 = b1 + 1;
                    float peak = 0.0f;
                    for (int b = b1; b < b2 && b < FFT_SIZE / 2; b++) { float mag = cabsf(X[b]); if (mag > peak) peak = mag; }
                    float norm_mag = peak / (FFT_SIZE / 4.0f); 
                    float val = log10f(1.0f + norm_mag * 100.0f) / 2.0f; if (val > 1.0f) val = 1.0f;
                    if (val >= smooth_bars[x]) smooth_bars[x] = val; else { smooth_bars[x] -= 0.04f; if (smooth_bars[x] < 0.0f) smooth_bars[x] = 0.0f; }
                    int bar_h = (int)(smooth_bars[x] * px_h); if (bar_h > px_h) bar_h = px_h;
                    int y1 = px_h - 1; int y0 = px_h - bar_h; if (y0 < 0) y0 = 0;
                    draw_braille_line(grid, draw_w, draw_h, x, y0, x, y1);
                }
            }
            for (int y = 0; y < draw_h; y++) {
                for (int x = 0; x < draw_w; x++) {
                    uint8_t v = grid[y * draw_w + x]; if (v == 0) continue;
                    int color_idx = 6 + (x * 5) / draw_w;
                    if (color_idx > 10) color_idx = 10; if (color_idx < 6) color_idx = 6;
                    attron(COLOR_PAIR(color_idx));
                    char utf8_braille[4] = {(char)0xE2, (char)(0xA0 | (v >> 6)), (char)(0x80 | (v & 0x3F)), '\0'};
                    mvprintw(2 + y, left_w + 2 + x, "%s", utf8_braille);
                    attroff(COLOR_PAIR(color_idx));
                }
            }
        }
    } else if (active_tab == 2) {
        mvprintw(2,  left_w + 4, "Blocks:");
        mvprintw(3,  left_w + 6, "Decoded %u blocks", atomic_load(&p_decoded_blocks));
        mvprintw(4,  left_w + 6, "Played %u buffers", atomic_load(&p_played_buffers));
        mvprintw(5,  left_w + 6, "Lost %u buffers", atomic_load(&p_lost_buffers));
        mvprintw(7,  left_w + 4, "Input/Read:");
        mvprintw(8,  left_w + 6, "Media data size %u KiB", atomic_load(&p_media_data_size_kib));
        mvprintw(9,  left_w + 6, "Input bitrate %u kb/s", atomic_load(&p_input_bitrate_kbs));
        mvprintw(10, left_w + 6, "Demuxed data size %u KiB", atomic_load(&p_demuxed_data_size_kib));
        mvprintw(11, left_w + 6, "Content bitrate %u kb/s", atomic_load(&p_content_bitrate_kbs));
        mvprintw(12, left_w + 6, "Discarded (corrupt) %u", atomic_load(&p_discarded));
        mvprintw(13, left_w + 6, "Dropped (discontinued) %u", atomic_load(&p_dropped));
    } else if (active_tab == 3) {
        for (int y = 2; y < top_h - 1; y++) mvhline(y, left_w + 2, ' ', right_w - 4);
        if (cached_meta.lyrics) {
            int ly = 2;
            char* lyrics_copy = strdup(cached_meta.lyrics);
            char* line = strtok(lyrics_copy, "\n");
            while (line && ly < top_h - 1) {
                mvprintw(ly++, left_w + 4, "%.*s", right_w - 6, line);
                line = strtok(NULL, "\n");
            }
            free(lyrics_copy);
        } else {
            mvprintw(2, left_w + 4, "DanaID: No Lyrics available");
        }
    }

    if (!is_fullscreen) {
        draw_box(top_h, left_w, bottom_h, right_w, "player", 1);
        int center_x = left_w + (right_w / 2);

        uint32_t vu_window = (srate * 50u) / 1000u; // 50ms window
        float peak_l = 0.0f;
        float peak_r = 0.0f;
        for (uint32_t i = 0; i < vu_window; i++) {
            if (i > smooth_rpos) break;
            uint32_t idx = (smooth_rpos - i) & VIS_BUF_MASK;
            float l = fabsf(vis_ring_l[idx]);
            float r = fabsf(vis_ring_r[idx]);
            if (l > peak_l) peak_l = l;
            if (r > peak_r) peak_r = r;
        }
        
        int clip_l = (peak_l > 1.0f);
        int clip_r = (peak_r > 1.0f);
        
        static int clip_hold_l = 0;
        static int clip_hold_r = 0;
        if (clip_l) clip_hold_l = 20;
        else if (clip_hold_l > 0) clip_hold_l--;

        if (clip_r) clip_hold_r = 20;
        else if (clip_hold_r > 0) clip_hold_r--;

        float db_l = (peak_l < 0.001f) ? -60.0f : 20.0f * log10f(peak_l);
        float db_r = (peak_r < 0.001f) ? -60.0f : 20.0f * log10f(peak_r);
        peak_l = (db_l + 40.0f) / 40.0f;
        peak_r = (db_r + 40.0f) / 40.0f;
        if (peak_l < 0.0f) peak_l = 0.0f;
        if (peak_l > 1.0f) peak_l = 1.0f;
        if (peak_r < 0.0f) peak_r = 0.0f;
        if (peak_r > 1.0f) peak_r = 1.0f;

        static float smooth_peak_l = 0.0f;
        static float smooth_peak_r = 0.0f;

        if (peak_l > smooth_peak_l) smooth_peak_l = peak_l;
        else { smooth_peak_l -= 0.03f; if (smooth_peak_l < 0.0f) smooth_peak_l = 0.0f; }

        if (peak_r > smooth_peak_r) smooth_peak_r = peak_r;
        else { smooth_peak_r -= 0.03f; if (smooth_peak_r < 0.0f) smooth_peak_r = 0.0f; }

        int bar_len = (right_w - 10) / 2;
        if (bar_len > 24) bar_len = 24;
        if (bar_len < 5) bar_len = 5;

        int val_l = (int)(smooth_peak_l * bar_len);
        if (val_l > bar_len) val_l = bar_len;
        int val_r = (int)(smooth_peak_r * bar_len);
        if (val_r > bar_len) val_r = bar_len;

        attron(COLOR_PAIR(2));
        mvaddstr(top_h + 3, center_x, "|");
        mvaddstr(top_h + 3, center_x - 1 - bar_len - 2, "L");
        mvaddstr(top_h + 3, center_x + 1 + bar_len + 2, "R");
        attroff(COLOR_PAIR(2));

        for (int i = 0; i < bar_len; i++) {
            int color;
            float pct = (float)i / bar_len;
            if (pct < 0.6f) color = 3; // green
            else if (pct < 0.85f) color = 4; // yellow
            else color = 10; // red
            
            attron(COLOR_PAIR(color));
            if (i < val_l) {
                mvaddstr(top_h + 3, center_x - 1 - i, "\xE2\x96\x88");
            } else {
                mvaddstr(top_h + 3, center_x - 1 - i, " ");
            }
            
            if (i < val_r) {
                mvaddstr(top_h + 3, center_x + 1 + i, "\xE2\x96\x88");
            } else {
                mvaddstr(top_h + 3, center_x + 1 + i, " ");
            }
            attroff(COLOR_PAIR(color));
        }

        if (clip_hold_l > 0) {
            attron(COLOR_PAIR(10) | A_REVERSE);
            mvaddstr(top_h + 3, center_x - 1 - bar_len, " ");
            attroff(COLOR_PAIR(10) | A_REVERSE);
        } else {
            mvaddstr(top_h + 3, center_x - 1 - bar_len, " ");
        }

        if (clip_hold_r > 0) {
            attron(COLOR_PAIR(10) | A_REVERSE);
            mvaddstr(top_h + 3, center_x + 1 + bar_len, " ");
            attroff(COLOR_PAIR(10) | A_REVERSE);
        } else {
            mvaddstr(top_h + 3, center_x + 1 + bar_len, " ");
        }

        mvprintw(top_h + 5, max_x - 14, "Vol: %3d%%", atomic_load(&volume));
        uint32_t cur_sec = atomic_load(&p_current_sec);
        int bar_width = right_w - 18;
        float progress = (tot_sec > 0) ? (float)cur_sec / (float)tot_sec : 0.0f;
        if (progress > 1.0f) progress = 1.0f;
        int pos = (int)(progress * (float)bar_width);
        mvprintw(top_h + 7, left_w + 4, "%02u:%02u ", cur_sec / 60, cur_sec % 60);
        attron(COLOR_PAIR(5));
        for (int i = 0; i < bar_width; i++) {
            if (i < pos) addstr("━"); else if (i == pos) addstr("●"); else addstr("─");
        }
        attroff(COLOR_PAIR(5));
        printw(" %02u:%02u", tot_sec / 60, tot_sec % 60);
        int text_len  = (int)strlen(cached_filename);
        int txt_start = center_x - (text_len / 2);
        if (txt_start < left_w + 2) txt_start = left_w + 2;
        mvprintw(top_h + 9, txt_start, "%.*s", right_w - 4, (playing_file_idx >= 0) ? cached_filename : "<No Song Selected>");
    } else {
        uint32_t cur_sec = atomic_load(&p_current_sec);
        int bar_width = right_w - 26;
        if (bar_width > 0) {
            float progress = (tot_sec > 0) ? (float)cur_sec / (float)tot_sec : 0.0f;
            if (progress > 1.0f) progress = 1.0f;
            int pos = (int)(progress * (float)bar_width);
            mvprintw(top_h - 2, left_w + 4, "%02u:%02u ", cur_sec / 60, cur_sec % 60);
            attron(COLOR_PAIR(5));
            for (int i = 0; i < bar_width; i++) {
                if (i < pos) addstr("━"); else if (i == pos) addstr("●"); else addstr("─");
            }
            attroff(COLOR_PAIR(5));
            printw(" %02u:%02u", tot_sec / 60, tot_sec % 60);
            mvprintw(top_h - 2, max_x - 12, "Vol:%3d%%", atomic_load(&volume));
        }
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
                if (selected_file_idx < scroll_offset) scroll_offset = selected_file_idx; break;
            case KEY_DOWN:
                if (selected_file_idx < num_files - 1) selected_file_idx++;
                if (selected_file_idx >= scroll_offset + ((LINES - 12) - 4)) scroll_offset = selected_file_idx - ((LINES - 12) - 4) + 1; break;
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
            case '3': active_tab = 3; break;
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
    endwin();
}