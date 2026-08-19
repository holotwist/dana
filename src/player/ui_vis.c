#include "ui_common.h"
#include "vis_math.h"
#include <math.h>
#include <stdlib.h>
#include <string.h>

void draw_vis_panel(int y, int x, int h, int w) {
    if (h < 4 || w < 4) return;
    ui_draw_box(y, x, h, w, is_fullscreen ? (playing_file_idx >= 0 ? ui_cache.filename : "player") : NULL, 1);

    // Tabs Menu
    mvprintw(y, x + 2, " ");
    attron(active_tab == 1 ? A_REVERSE : A_NORMAL); 
    if (current_vis_mode == 0) printw("1:Oscilloscope"); else printw("1:Spectrum");
    attroff(active_tab == 1 ? A_REVERSE : A_NORMAL); printw(" ");
    
    attron(active_tab == 2 ? A_REVERSE : A_NORMAL); printw("2:Codec info"); attroff(active_tab == 2 ? A_REVERSE : A_NORMAL); printw(" ");
    
    if (ui_cache.meta.lyrics != NULL) {
        attron(active_tab == 3 ? A_REVERSE : A_NORMAL); printw("3:lyric"); attroff(active_tab == 3 ? A_REVERSE : A_NORMAL); printw(" ");
    }
    printw(" -["); attron(COLOR_PAIR(5)); printw("C:Switch Visualizer"); attroff(COLOR_PAIR(5)); printw("] ");
    printw("-["); attron(COLOR_PAIR(5)); printw(is_fullscreen ? "F:Windowed" : "F:Fullscreen"); attroff(COLOR_PAIR(5)); printw("] ");

    int draw_w = w - 4;
    int draw_h = h - 4;
    if (draw_w < 1 || draw_h < 1) return;

    if (active_tab == 1) {
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

        int px_w = draw_w * 2; int px_h = draw_h * 4;
        uint32_t srate = atomic_load(&vis_srate);
        if (srate == 0) srate = 44100;

        if (display_buf && grid) {
            for (int clear_y = 0; clear_y < draw_h; clear_y++) mvhline(y + 2 + clear_y, x + 2, ' ', draw_w);

            if (current_vis_mode == 0) {
                uint32_t window_samples = (srate * 40u) / 1000u;
                int num_samples = px_w * 2; if (num_samples > 8192) num_samples = 8192;
                for (int i = 0; i < num_samples; i++) {
                    uint32_t neg_off = window_samples - (uint32_t)((i * window_samples) / num_samples);
                    if (neg_off > ui_cache.smooth_rpos) display_buf[i] = 0.0f; 
                    else display_buf[i] = (vis_ring_l[(ui_cache.smooth_rpos - neg_off) & VIS_BUF_MASK] + vis_ring_r[(ui_cache.smooth_rpos - neg_off) & VIS_BUF_MASK]) * 0.5f;
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
                    uint32_t idx = (ui_cache.smooth_rpos + VIS_BUF_SIZE - FFT_SIZE + i) & VIS_BUF_MASK;
                    float hann = 0.5f * (1.0f - cosf(2.0f * (float)M_PI * i / (FFT_SIZE - 1)));
                    float val = (vis_ring_l[idx] + vis_ring_r[idx]) * 0.5f * hann;
                    if (fabsf(val) < 1e-15f) val = 0.0f;
                    X[i] = val;
                }
                compute_fft(X, FFT_SIZE);
                float min_f = log10f(1.0f); float max_f = log10f((float)(FFT_SIZE / 2));
                for (int px = 0; px < px_w; px++) {
                    float low_bin = powf(10.0f, min_f + (max_f - min_f) * ((float)px / px_w));
                    float high_bin = powf(10.0f, min_f + (max_f - min_f) * ((float)(px + 1) / px_w));
                    int b1 = (int)low_bin; int b2 = (int)high_bin; if (b2 <= b1) b2 = b1 + 1;
                    float peak = 0.0f;
                    for (int b = b1; b < b2 && b < FFT_SIZE / 2; b++) { float mag = cabsf(X[b]); if (mag > peak) peak = mag; }
                    float norm_mag = peak / (FFT_SIZE / 4.0f); 
                    float val = log10f(1.0f + norm_mag * 100.0f) / 2.0f; if (val > 1.0f) val = 1.0f;
                    if (val >= smooth_bars[px]) smooth_bars[px] = val; else { smooth_bars[px] -= 0.04f; if (smooth_bars[px] < 0.0f) smooth_bars[px] = 0.0f; }
                    int bar_h = (int)(smooth_bars[px] * px_h); if (bar_h > px_h) bar_h = px_h;
                    int y1 = px_h - 1; int y0 = px_h - bar_h; if (y0 < 0) y0 = 0;
                    draw_braille_line(grid, draw_w, draw_h, px, y0, px, y1);
                }
            }
            for (int gy = 0; gy < draw_h; gy++) {
                for (int gx = 0; gx < draw_w; gx++) {
                    uint8_t v = grid[gy * draw_w + gx]; if (v == 0) continue;
                    int color_idx = 6 + (gx * 5) / draw_w;
                    if (color_idx > 10) color_idx = 10; if (color_idx < 6) color_idx = 6;
                    attron(COLOR_PAIR(color_idx));
                    char utf8_braille[4] = {(char)0xE2, (char)(0xA0 | (v >> 6)), (char)(0x80 | (v & 0x3F)), '\0'};
                    mvprintw(y + 2 + gy, x + 2 + gx, "%s", utf8_braille);
                    attroff(COLOR_PAIR(color_idx));
                }
            }
        }
    } else if (active_tab == 2) {
        int line = y + 1;
        if (++line < y + h - 1) mvprintw(line,  x + 4, "Blocks:");
        if (++line < y + h - 1) mvprintw(line,  x + 6, "Decoded %u blocks", atomic_load(&p_decoded_blocks));
        if (++line < y + h - 1) mvprintw(line,  x + 6, "Played %u buffers", atomic_load(&p_played_buffers));
        if (++line < y + h - 1) mvprintw(line,  x + 6, "Lost %u buffers", atomic_load(&p_lost_buffers));
        if (++line < y + h - 1); // blank
        if (++line < y + h - 1) mvprintw(line,  x + 4, "Input/Read:");
        if (++line < y + h - 1) mvprintw(line,  x + 6, "Media data size %u KiB", atomic_load(&p_media_data_size_kib));
        if (++line < y + h - 1) mvprintw(line,  x + 6, "Input bitrate %u kb/s", atomic_load(&p_input_bitrate_kbs));
        if (++line < y + h - 1) mvprintw(line,  x + 6, "Demuxed data size %u KiB", atomic_load(&p_demuxed_data_size_kib));
        if (++line < y + h - 1) mvprintw(line,  x + 6, "Content bitrate %u kb/s", atomic_load(&p_content_bitrate_kbs));
        if (++line < y + h - 1) mvprintw(line,  x + 6, "Discarded (corrupt) %u", atomic_load(&p_discarded));
        if (++line < y + h - 1) mvprintw(line,  x + 6, "Dropped (discont) %u", atomic_load(&p_dropped));
    } else if (active_tab == 3) {
        for (int gy = 0; gy < draw_h; gy++) mvhline(y + 2 + gy, x + 2, ' ', draw_w);
        if (ui_cache.meta.lyrics) {
            int ly = y + 2;
            char* lyrics_copy = strdup(ui_cache.meta.lyrics);
            char* line = strtok(lyrics_copy, "\n");
            while (line && ly < y + h - 1) {
                mvprintw(ly++, x + 4, "%.*s", draw_w - 2, line);
                line = strtok(NULL, "\n");
            }
            free(lyrics_copy);
        } else {
            mvprintw(y + 2, x + 4, "DanaID: No Lyrics available");
        }
    }
}