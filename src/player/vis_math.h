#ifndef VIS_MATH_H
#define VIS_MATH_H

#include <complex.h>
#include <stdint.h>

void compute_fft(float complex *X, int N);
void draw_braille_line(uint8_t *grid, int draw_w, int draw_h, int x0, int y0, int x1, int y1);

#endif // VIS_MATH_H