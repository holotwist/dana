#include "vis_math.h"
#include <math.h>
#include <stdlib.h>

void compute_fft(float complex *X, int N) {
    for (int i = 1, j = 0; i < N; i++) {
        int bit = N >> 1;
        for (; j & bit; bit >>= 1) j ^= bit;
        j ^= bit;
        if (i < j) {
            float complex temp = X[i];
            X[i] = X[j];
            X[j] = temp;
        }
    }
    for (int len = 2; len <= N; len <<= 1) {
        float angle = -2.0f * (float)M_PI / len;
        float complex wlen = cosf(angle) + I * sinf(angle);
        for (int i = 0; i < N; i += len) {
            float complex w = 1.0f;
            for (int j = 0; j < len / 2; j++) {
                float complex u = X[i + j];
                float complex v = X[i + j + len / 2] * w;
                X[i + j] = u + v;
                X[i + j + len / 2] = u - v;
                w *= wlen;
            }
        }
    }
}

static inline void set_braille_pixel(uint8_t *grid, int draw_w, int draw_h, int px_x, int px_y) {
    if (px_x < 0 || px_x >= draw_w * 2 || px_y < 0 || px_y >= draw_h * 4) return;
    int cell_x = px_x / 2;
    int cell_y = px_y / 4;
    int dot_x = px_x % 2;
    int dot_y = px_y % 4;
    static const uint8_t dot_mask[4][2] = {{0x01, 0x08}, {0x02, 0x10}, {0x04, 0x20}, {0x40, 0x80}};
    grid[cell_y * draw_w + cell_x] |= dot_mask[dot_y][dot_x];
}

void draw_braille_line(uint8_t *grid, int draw_w, int draw_h, int x0, int y0, int x1, int y1) {
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy, e2;
    for (;;) {
        set_braille_pixel(grid, draw_w, draw_h, x0, y0);
        if (x0 == x1 && y0 == y1) break;
        e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}