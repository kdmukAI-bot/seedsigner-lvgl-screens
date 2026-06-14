// lv_shape — stdin UTF-8 -> JSON array of the code points LVGL's Arabic/Persian
// shaper emits. See README.md for what it's for and why it's a standalone tool.
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include "lvgl.h"
#include "src/misc/lv_text_ap.h"  // private header, not in the lvgl.h umbrella

#if !LV_USE_ARABIC_PERSIAN_CHARS
#error "lv_shape requires LV_USE_ARABIC_PERSIAN_CHARS=1 (see CMakeLists.txt)"
#endif

#define CODEPOINT_LIMIT 0x110000u

static char *read_all_stdin(size_t *out_len) {
    size_t cap = 1 << 16;
    size_t len = 0;
    char *buf = (char *)malloc(cap);
    if (!buf) return NULL;

    for (;;) {
        if (len + 4096 > cap) {
            cap *= 2;
            char *grown = (char *)realloc(buf, cap);
            if (!grown) { free(buf); return NULL; }
            buf = grown;
        }
        size_t got = fread(buf + len, 1, 4096, stdin);
        len += got;
        if (got < 4096) break;
    }

    buf[len] = '\0';
    if (out_len) *out_len = len;
    return buf;
}

static void mark_codepoints(const char *utf8, uint8_t *seen) {
    const unsigned char *p = (const unsigned char *)utf8;
    while (*p) {
        uint32_t cp;
        int n;
        if (*p < 0x80)      { cp = *p;        n = 1; }
        else if (*p < 0xE0) { cp = *p & 0x1F; n = 2; }
        else if (*p < 0xF0) { cp = *p & 0x0F; n = 3; }
        else                { cp = *p & 0x07; n = 4; }

        for (int k = 1; k < n; ++k) {
            if ((p[k] & 0xC0) != 0x80) { n = k; cp = 0; break; }  // truncated UTF-8
            cp = (cp << 6) | (p[k] & 0x3F);
        }
        if (cp && cp < CODEPOINT_LIMIT) seen[cp] = 1;
        p += n;
    }
}

int main(void) {
    lv_init();  // gives the shaper's lv_malloc/lv_free an allocator; headless + silent

    size_t len = 0;
    char *in = read_all_stdin(&len);
    if (!in) {
        fprintf(stderr, "lv_shape: out of memory reading stdin\n");
        return 1;
    }

    uint8_t *seen = (uint8_t *)calloc(CODEPOINT_LIMIT, 1);
    if (!seen) {
        fprintf(stderr, "lv_shape: out of memory allocating codepoint set\n");
        free(in);
        return 1;
    }

    if (len > 0) {
        uint32_t need = lv_text_ap_calc_bytes_count(in);
        char *out = (char *)calloc(need + 1, 1);
        if (!out) {
            fprintf(stderr, "lv_shape: out of memory allocating shaped buffer\n");
            free(seen);
            free(in);
            return 1;
        }
        lv_text_ap_proc(in, out);
        mark_codepoints(out, seen);
        free(out);
    }

    putchar('[');
    int first = 1;
    for (uint32_t cp = 0; cp < CODEPOINT_LIMIT; ++cp) {
        if (!seen[cp]) continue;
        if (!first) putchar(',');
        printf("%u", cp);
        first = 0;
    }
    putchar(']');
    putchar('\n');

    free(seen);
    free(in);
    return 0;
}
