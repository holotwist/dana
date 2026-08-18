#include "ui_common.h"
#include <math.h>

void draw_player_panel(int y, int x, int h, int w) {
    if (h < 3 || w < 10) return;
    ui_draw_box(y, x, h, w, "player", 1);
    int center_x = x + (w / 2);

    int vu_y = -1, vol_y = -1, prog_y = -1, title_y = -1;
    if (h >= 10) {
        vu_y = y + 3; vol_y = y + 5; prog_y = y + 7; title_y = y + 9;
    } else if (h >= 6) {
        vu_y = y + 2; vol_y = y + 3; prog_y = y + 4; title_y = y + 5;
    } else if (h >= 4) {
        vu_y = -1; vol_y = y + 1; prog_y = y + 2; title_y = y + 3;
    } else if (h == 3) {
        vu_y = -1; vol_y = y + 1; prog_y = y + 1; title_y = -1; // Stack horizontally if extreme crunch
    }

    // VU Meter
    if (vu_y != -1) {
        uint32_t srate = atomic_load(&vis_srate);
        if (srate == 0) srate = 44100;
        uint32_t vu_window = (srate * 50u) / 1000u; // 50ms window
        float peak_l = 0.0f, peak_r = 0.0f;
        for (uint32_t i = 0; i < vu_window; i++) {
            if (i > ui_cache.smooth_rpos) break;
            uint32_t idx = (ui_cache.smooth_rpos - i) & VIS_BUF_MASK;
            float l = fabsf(vis_ring_l[idx]); float r = fabsf(vis_ring_r[idx]);
            if (l > peak_l) peak_l = l;
            if (r > peak_r) peak_r = r;
        }

        int clip_l = (peak_l > 1.0f);
        int clip_r = (peak_r > 1.0f);
        
        static int clip_hold_l = 0; static int clip_hold_r = 0;
        if (clip_l) clip_hold_l = 20; else if (clip_hold_l > 0) clip_hold_l--;
        if (clip_r) clip_hold_r = 20; else if (clip_hold_r > 0) clip_hold_r--;

        float db_l = (peak_l < 0.001f) ? -60.0f : 20.0f * log10f(peak_l);
        float db_r = (peak_r < 0.001f) ? -60.0f : 20.0f * log10f(peak_r);
        peak_l = (db_l + 40.0f) / 40.0f; peak_r = (db_r + 40.0f) / 40.0f;
        if (peak_l < 0.0f) peak_l = 0.0f; if (peak_l > 1.0f) peak_l = 1.0f;
        if (peak_r < 0.0f) peak_r = 0.0f; if (peak_r > 1.0f) peak_r = 1.0f;

        static float smooth_peak_l = 0.0f; static float smooth_peak_r = 0.0f;
        if (peak_l > smooth_peak_l) smooth_peak_l = peak_l; else { smooth_peak_l -= 0.03f; if (smooth_peak_l < 0.0f) smooth_peak_l = 0.0f; }
        if (peak_r > smooth_peak_r) smooth_peak_r = peak_r; else { smooth_peak_r -= 0.03f; if (smooth_peak_r < 0.0f) smooth_peak_r = 0.0f; }

        int bar_len = (w - 10) / 2;
        if (bar_len > 24) bar_len = 24; if (bar_len < 5) bar_len = 5;

        int val_l = (int)(smooth_peak_l * bar_len); if (val_l > bar_len) val_l = bar_len;
        int val_r = (int)(smooth_peak_r * bar_len); if (val_r > bar_len) val_r = bar_len;

        attron(COLOR_PAIR(2));
        mvaddstr(vu_y, center_x, "|");
        mvaddstr(vu_y, center_x - 1 - bar_len - 2, "L");
        mvaddstr(vu_y, center_x + 1 + bar_len + 2, "R");
        attroff(COLOR_PAIR(2));

        for (int i = 0; i < bar_len; i++) {
            int color; float pct = (float)i / bar_len;
            if (pct < 0.6f) color = 3; else if (pct < 0.85f) color = 4; else color = 10;
            
            attron(COLOR_PAIR(color));
            mvaddstr(vu_y, center_x - 1 - i, (i < val_l) ? "\xE2\x96\x88" : " ");
            mvaddstr(vu_y, center_x + 1 + i, (i < val_r) ? "\xE2\x96\x88" : " ");
            attroff(COLOR_PAIR(color));
        }

        if (clip_hold_l > 0) attron(COLOR_PAIR(10) | A_REVERSE);
        mvaddstr(vu_y, center_x - 1 - bar_len, " ");
        if (clip_hold_l > 0) attroff(COLOR_PAIR(10) | A_REVERSE);

        if (clip_hold_r > 0) attron(COLOR_PAIR(10) | A_REVERSE);
        mvaddstr(vu_y, center_x + 1 + bar_len, " ");
        if (clip_hold_r > 0) attroff(COLOR_PAIR(10) | A_REVERSE);
    }

    // Volume
    if (vol_y != -1 && h > 3) {
        mvprintw(vol_y, x + w - 14, "Vol: %3d%%", atomic_load(&volume));
    }

    // Progress
    if (prog_y != -1) {
        uint32_t cur_sec = atomic_load(&p_current_sec);
        uint32_t tot_sec = atomic_load(&p_total_sec);
        
        int bar_width = w - 18;
        if (h == 3) bar_width = w - 28; // Leave space for volume horizontally

        if (bar_width > 0) {
            float progress = (tot_sec > 0) ? (float)cur_sec / (float)tot_sec : 0.0f;
            if (progress > 1.0f) progress = 1.0f;
            int pos = (int)(progress * (float)bar_width);
            mvprintw(prog_y, x + 4, "%02u:%02u ", cur_sec / 60, cur_sec % 60);
            
            attron(COLOR_PAIR(5));
            for (int i = 0; i < bar_width; i++) {
                if (i < pos) addstr("━"); else if (i == pos) addstr("●"); else addstr("─");
            }
            attroff(COLOR_PAIR(5));
            printw(" %02u:%02u", tot_sec / 60, tot_sec % 60);

            if (h == 3) mvprintw(prog_y, x + w - 12, "Vol:%3d%%", atomic_load(&volume));
        }
    }

    // Title
    if (title_y != -1) {
        int text_len = utf8_strlen(ui_cache.filename);
        int txt_start = center_x - (text_len / 2);
        if (txt_start < x + 2) txt_start = x + 2;
        mvprintw(title_y, txt_start, "%.*s", w - 4, (playing_file_idx >= 0) ? ui_cache.filename : "<No Song Selected>");
    }
}