#ifndef UI_ANIMATIONS_H
#define UI_ANIMATIONS_H

#include <stddef.h>

// Fills `out_buf` with a marquee-animated version of `text`
// if `text` is longer than `max_disp_len` characters.
void get_marquee_text(const char *text, int max_disp_len, unsigned long frame_counter, char *out_buf, size_t out_buf_size);

#endif // UI_ANIMATIONS_H