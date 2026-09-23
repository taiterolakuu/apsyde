#include "fb.h"
#include "font8x16.h"
#include <stdint.h>

/* ============ UTF-8 → CP1251 ============ */

static int unicode_to_cp1251(int uni) {
    if (uni < 0x80) return uni;

    if (uni >= 0x0410 && uni <= 0x044F) {
        return 0xC0 + (uni - 0x0410);
    }

    switch (uni) {
        case 0x0402: return 0x80;
        case 0x0403: return 0x81;
        case 0x201A: return 0x82;
        case 0x0453: return 0x83;
        case 0x201E: return 0x84;
        case 0x2026: return 0x85;
        case 0x2020: return 0x86;
        case 0x2021: return 0x87;
        case 0x20AC: return 0x88;
        case 0x2030: return 0x89;
        case 0x0409: return 0x8A;
        case 0x2039: return 0x8B;
        case 0x040A: return 0x8C;
        case 0x040C: return 0x8D;
        case 0x040B: return 0x8E;
        case 0x040F: return 0x8F;
        case 0x0452: return 0x90;
        case 0x2018: return 0x91;
        case 0x2019: return 0x92;
        case 0x201C: return 0x93;
        case 0x201D: return 0x94;
        case 0x2022: return 0x95;
        case 0x2013: return 0x96;
        case 0x2014: return 0x97;
        case 0x2122: return 0x99;
        case 0x0459: return 0x9A;
        case 0x203A: return 0x9B;
        case 0x045A: return 0x9C;
        case 0x045C: return 0x9D;
        case 0x045B: return 0x9E;
        case 0x045F: return 0x9F;
        case 0x00A0: return 0xA0;
        case 0x040E: return 0xA1;
        case 0x045E: return 0xA2;
        case 0x0408: return 0xA3;
        case 0x00A4: return 0xA4;
        case 0x0490: return 0xA5;
        case 0x00A6: return 0xA6;
        case 0x00A7: return 0xA7;
        case 0x0401: return 0xA8;
        case 0x00A9: return 0xA9;
        case 0x0404: return 0xAA;
        case 0x00AB: return 0xAB;
        case 0x00AC: return 0xAC;
        case 0x00AD: return 0xAD;
        case 0x00AE: return 0xAE;
        case 0x0407: return 0xAF;
        case 0x00B0: return 0xB0;
        case 0x00B1: return 0xB1;
        case 0x0406: return 0xB2;
        case 0x0456: return 0xB3;
        case 0x0491: return 0xB4;
        case 0x00B5: return 0xB5;
        case 0x00B6: return 0xB6;
        case 0x00B7: return 0xB7;
        case 0x0451: return 0xB8;
        case 0x2116: return 0xB9;
        case 0x0454: return 0xBA;
        case 0x00BB: return 0xBB;
        case 0x0458: return 0xBC;
        case 0x0405: return 0xBD;
        case 0x0455: return 0xBE;
        case 0x0457: return 0xBF;
    }

    return '?';
}

/* ============ Framebuffer ============ */

static uint32_t *g_fb       = 0;
static uint32_t  g_width    = 0;
static uint32_t  g_height   = 0;
static uint32_t  g_pitch    = 0;
static uint32_t  g_format   = 0;
static int       g_enabled  = 0;

static uint32_t g_col = 0;
static uint32_t g_row = 0;

static uint32_t g_cols = 0;
static uint32_t g_rows = 0;

#define COLOR_BLACK  0x00000000u
#define COLOR_WHITE  0x00FFFFFFu

void fb_init(const boot_info_t *bi) {
    if (!bi || bi->framebuffer_base == 0) {
        g_enabled = 0;
        return;
    }

    g_fb     = (uint32_t *)(uintptr_t)bi->framebuffer_base;
    g_width  = bi->horizontal_resolution;
    g_height = bi->vertical_resolution;
    g_pitch  = bi->pixels_per_scanline;
    g_format = bi->pixel_format;

    g_cols = g_width  / FONT_WIDTH;
    g_rows = g_height / FONT_HEIGHT;

    g_col = 0;
    g_row = 0;

    g_enabled = 1;
    fb_clear();
}

int      fb_enabled(void)  { return g_enabled; }
uint32_t fb_width(void)    { return g_width; }
uint32_t fb_height(void)   { return g_height; }

void fb_put_pixel(uint32_t x, uint32_t y, uint32_t color) {
    if (!g_enabled) return;
    if (x >= g_width || y >= g_height) return;
    g_fb[y * g_pitch + x] = color;
}

void fb_fill(uint32_t color) {
    if (!g_enabled) return;
    for (uint32_t y = 0; y < g_height; y++) {
        for (uint32_t x = 0; x < g_width; x++) {
            g_fb[y * g_pitch + x] = color;
        }
    }
}

void fb_fill_rect(uint32_t x, uint32_t y, uint32_t w, uint32_t h,
                  uint32_t color) {
    if (!g_enabled) return;
    if (x >= g_width || y >= g_height) return;
    if (x + w > g_width)  w = g_width  - x;
    if (y + h > g_height) h = g_height - y;

    for (uint32_t row = 0; row < h; row++) {
        uint32_t base = (y + row) * g_pitch + x;
        for (uint32_t col = 0; col < w; col++) {
            g_fb[base + col] = color;
        }
    }
}

void fb_clear(void) {
    fb_fill(COLOR_BLACK);
    g_col = 0;
    g_row = 0;
}

static void draw_glyph(uint32_t px, uint32_t py, uint8_t ch,
                       uint32_t fg, uint32_t bg) {
    const uint8_t *glyph = font8x16[ch];
    for (uint32_t row = 0; row < FONT_HEIGHT; row++) {
        uint8_t bits = glyph[row];
        for (uint32_t col = 0; col < FONT_WIDTH; col++) {
            int set = (bits >> (7 - col)) & 1;
            fb_put_pixel(px + col, py + row, set ? fg : bg);
        }
    }
}

static void scroll_up(void) {
    if (!g_enabled) return;

    uint32_t total_line_pixels = FONT_HEIGHT * g_pitch;
    uint8_t *fb_bytes = (uint8_t *)g_fb;
    uint32_t *src = (uint32_t *)(fb_bytes + total_line_pixels * 4);
    uint32_t *dst = (uint32_t *)fb_bytes;

    uint32_t copy_pixels = (g_height - FONT_HEIGHT) * g_pitch;
    for (uint32_t i = 0; i < copy_pixels; i++) {
        dst[i] = src[i];
    }

    uint32_t start = (g_height - FONT_HEIGHT) * g_pitch;
    uint32_t end   = g_height * g_pitch;
    for (uint32_t i = start; i < end; i++) {
        dst[i] = COLOR_BLACK;
    }
}

void fb_putc(char c) {
    /* Статические переменные для сборки UTF-8 по байтам */
    static int pending = 0;
    static int expect  = 0;

    if (!g_enabled) return;

    uint8_t b = (uint8_t)c;

    if (b == '\n') { pending = 0; expect = 0; g_col = 0; g_row++; goto scroll; }
    if (b == '\r') { pending = 0; expect = 0; g_col = 0; return; }
    if (b == '\b') {
        pending = 0; expect = 0;
        if (g_col > 0) {
            g_col--;
            draw_glyph(g_col * FONT_WIDTH, g_row * FONT_HEIGHT,
                       0x20, COLOR_WHITE, COLOR_BLACK);
        }
        return;
    }
    if (b == '\t') {
        pending = 0; expect = 0;
        g_col = (g_col + 8) & ~7u;
        if (g_col >= g_cols) { g_col = 0; g_row++; }
        goto scroll;
    }
    if (b < 0x20) return;

    if (b < 0x80 && expect == 0) {
        draw_glyph(g_col * FONT_WIDTH, g_row * FONT_HEIGHT,
                   b, COLOR_WHITE, COLOR_BLACK);
        g_col++;
        if (g_col >= g_cols) { g_col = 0; g_row++; }
        goto scroll;
    }

    if (expect == 0) {
        if      ((b & 0xE0) == 0xC0) { pending = b & 0x1F; expect = 1; return; }
        else if ((b & 0xF0) == 0xE0) { pending = b & 0x0F; expect = 2; return; }
        else if ((b & 0xF8) == 0xF0) { pending = b & 0x07; expect = 3; return; }
        else return;
    }

    if ((b & 0xC0) != 0x80) {
        pending = 0;
        expect = 0;
        return;
    }

    pending = (pending << 6) | (b & 0x3F);
    expect--;

    if (expect != 0) return;

    int cp1251 = unicode_to_cp1251(pending);
    if (cp1251 < 0) cp1251 = '?';

    draw_glyph(g_col * FONT_WIDTH, g_row * FONT_HEIGHT,
               (uint8_t)cp1251, COLOR_WHITE, COLOR_BLACK);
    g_col++;
    if (g_col >= g_cols) { g_col = 0; g_row++; }

    pending = 0;

scroll:
    if (g_row >= g_rows) {
        scroll_up();
        g_row = g_rows - 1;
    }
}

void fb_puts(const char *s) {
    while (*s) fb_putc(*s++);
}

void fb_set_cursor(uint32_t col, uint32_t row) {
    g_col = col;
    g_row = row;
}

void fb_info(uint32_t *out_width, uint32_t *out_height,
             uint32_t *out_pitch, uint32_t *out_format,
             uint32_t *out_col, uint32_t *out_row) {
    if (out_width)  *out_width  = g_width;
    if (out_height) *out_height = g_height;
    if (out_pitch)  *out_pitch  = g_pitch;
    if (out_format) *out_format = g_format;
    if (out_col)    *out_col    = g_col;
    if (out_row)    *out_row    = g_row;
}