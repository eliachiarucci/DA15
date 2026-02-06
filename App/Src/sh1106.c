// SPDX-License-Identifier: GPL-3.0-only
// Copyright (c) 2026 Elia Chiarucci

#include "sh1106.h"
#include <string.h>

#define FB_SIZE (SH1106_WIDTH * SH1106_HEIGHT / 8)

// SH1106 has 132-column RAM but displays 128 columns, offset by 2
#define SH1106_COL_OFFSET 2

// Per-page DMA buffer: 3 command pairs (Co=1) + data prefix + 128 pixels
#define PAGE_HDR_SIZE 7
#define PAGE_BUF_SIZE (PAGE_HDR_SIZE + SH1106_WIDTH)

static I2C_HandleTypeDef *sh1106_i2c;

static uint8_t framebuffer[FB_SIZE];
static uint8_t page_buf[PAGE_BUF_SIZE]; // reused for each page DMA transfer

static uint8_t cursor_x;
static uint8_t cursor_y;
static uint8_t font_scale = 1;
static volatile uint8_t sh1106_dma_busy;
static volatile uint8_t current_page;
static volatile uint8_t dirty_pages;  // bitmask: bit N = page N needs sending

// 5x7 font for ASCII 32-126
static const uint8_t font5x7[][5] = {
    {0x00,0x00,0x00,0x00,0x00}, // 32 ' '
    {0x00,0x00,0x5F,0x00,0x00}, // 33 '!'
    {0x00,0x07,0x00,0x07,0x00}, // 34 '"'
    {0x14,0x7F,0x14,0x7F,0x14}, // 35 '#'
    {0x24,0x2A,0x7F,0x2A,0x12}, // 36 '$'
    {0x23,0x13,0x08,0x64,0x62}, // 37 '%'
    {0x36,0x49,0x55,0x22,0x50}, // 38 '&'
    {0x00,0x05,0x03,0x00,0x00}, // 39 '''
    {0x00,0x1C,0x22,0x41,0x00}, // 40 '('
    {0x00,0x41,0x22,0x1C,0x00}, // 41 ')'
    {0x08,0x2A,0x1C,0x2A,0x08}, // 42 '*'
    {0x08,0x08,0x3E,0x08,0x08}, // 43 '+'
    {0x00,0x50,0x30,0x00,0x00}, // 44 ','
    {0x08,0x08,0x08,0x08,0x08}, // 45 '-'
    {0x00,0x60,0x60,0x00,0x00}, // 46 '.'
    {0x20,0x10,0x08,0x04,0x02}, // 47 '/'
    {0x3E,0x51,0x49,0x45,0x3E}, // 48 '0'
    {0x00,0x42,0x7F,0x40,0x00}, // 49 '1'
    {0x42,0x61,0x51,0x49,0x46}, // 50 '2'
    {0x21,0x41,0x45,0x4B,0x31}, // 51 '3'
    {0x18,0x14,0x12,0x7F,0x10}, // 52 '4'
    {0x27,0x45,0x45,0x45,0x39}, // 53 '5'
    {0x3C,0x4A,0x49,0x49,0x30}, // 54 '6'
    {0x01,0x71,0x09,0x05,0x03}, // 55 '7'
    {0x36,0x49,0x49,0x49,0x36}, // 56 '8'
    {0x06,0x49,0x49,0x29,0x1E}, // 57 '9'
    {0x00,0x36,0x36,0x00,0x00}, // 58 ':'
    {0x00,0x56,0x36,0x00,0x00}, // 59 ';'
    {0x00,0x08,0x14,0x22,0x41}, // 60 '<'
    {0x14,0x14,0x14,0x14,0x14}, // 61 '='
    {0x41,0x22,0x14,0x08,0x00}, // 62 '>'
    {0x02,0x01,0x51,0x09,0x06}, // 63 '?'
    {0x32,0x49,0x79,0x41,0x3E}, // 64 '@'
    {0x7E,0x11,0x11,0x11,0x7E}, // 65 'A'
    {0x7F,0x49,0x49,0x49,0x36}, // 66 'B'
    {0x3E,0x41,0x41,0x41,0x22}, // 67 'C'
    {0x7F,0x41,0x41,0x22,0x1C}, // 68 'D'
    {0x7F,0x49,0x49,0x49,0x41}, // 69 'E'
    {0x7F,0x09,0x09,0x01,0x01}, // 70 'F'
    {0x3E,0x41,0x41,0x51,0x32}, // 71 'G'
    {0x7F,0x08,0x08,0x08,0x7F}, // 72 'H'
    {0x00,0x41,0x7F,0x41,0x00}, // 73 'I'
    {0x20,0x40,0x41,0x3F,0x01}, // 74 'J'
    {0x7F,0x08,0x14,0x22,0x41}, // 75 'K'
    {0x7F,0x40,0x40,0x40,0x40}, // 76 'L'
    {0x7F,0x02,0x04,0x02,0x7F}, // 77 'M'
    {0x7F,0x04,0x08,0x10,0x7F}, // 78 'N'
    {0x3E,0x41,0x41,0x41,0x3E}, // 79 'O'
    {0x7F,0x09,0x09,0x09,0x06}, // 80 'P'
    {0x3E,0x41,0x51,0x21,0x5E}, // 81 'Q'
    {0x7F,0x09,0x19,0x29,0x46}, // 82 'R'
    {0x46,0x49,0x49,0x49,0x31}, // 83 'S'
    {0x01,0x01,0x7F,0x01,0x01}, // 84 'T'
    {0x3F,0x40,0x40,0x40,0x3F}, // 85 'U'
    {0x1F,0x20,0x40,0x20,0x1F}, // 86 'V'
    {0x7F,0x20,0x18,0x20,0x7F}, // 87 'W'
    {0x63,0x14,0x08,0x14,0x63}, // 88 'X'
    {0x03,0x04,0x78,0x04,0x03}, // 89 'Y'
    {0x61,0x51,0x49,0x45,0x43}, // 90 'Z'
    {0x00,0x00,0x7F,0x41,0x41}, // 91 '['
    {0x02,0x04,0x08,0x10,0x20}, // 92 '\'
    {0x41,0x41,0x7F,0x00,0x00}, // 93 ']'
    {0x04,0x02,0x01,0x02,0x04}, // 94 '^'
    {0x40,0x40,0x40,0x40,0x40}, // 95 '_'
    {0x00,0x01,0x02,0x04,0x00}, // 96 '`'
    {0x20,0x54,0x54,0x54,0x78}, // 97 'a'
    {0x7F,0x48,0x44,0x44,0x38}, // 98 'b'
    {0x38,0x44,0x44,0x44,0x20}, // 99 'c'
    {0x38,0x44,0x44,0x48,0x7F}, //100 'd'
    {0x38,0x54,0x54,0x54,0x18}, //101 'e'
    {0x08,0x7E,0x09,0x01,0x02}, //102 'f'
    {0x08,0x14,0x54,0x54,0x3C}, //103 'g'
    {0x7F,0x08,0x04,0x04,0x78}, //104 'h'
    {0x00,0x44,0x7D,0x40,0x00}, //105 'i'
    {0x20,0x40,0x44,0x3D,0x00}, //106 'j'
    {0x00,0x7F,0x10,0x28,0x44}, //107 'k'
    {0x00,0x41,0x7F,0x40,0x00}, //108 'l'
    {0x7C,0x04,0x18,0x04,0x78}, //109 'm'
    {0x7C,0x08,0x04,0x04,0x78}, //110 'n'
    {0x38,0x44,0x44,0x44,0x38}, //111 'o'
    {0x7C,0x14,0x14,0x14,0x08}, //112 'p'
    {0x08,0x14,0x14,0x18,0x7C}, //113 'q'
    {0x7C,0x08,0x04,0x04,0x08}, //114 'r'
    {0x48,0x54,0x54,0x54,0x20}, //115 's'
    {0x04,0x3F,0x44,0x40,0x20}, //116 't'
    {0x3C,0x40,0x40,0x20,0x7C}, //117 'u'
    {0x1C,0x20,0x40,0x20,0x1C}, //118 'v'
    {0x3C,0x40,0x30,0x40,0x3C}, //119 'w'
    {0x44,0x28,0x10,0x28,0x44}, //120 'x'
    {0x0C,0x50,0x50,0x50,0x3C}, //121 'y'
    {0x44,0x64,0x54,0x4C,0x44}, //122 'z'
    {0x00,0x08,0x36,0x41,0x00}, //123 '{'
    {0x00,0x00,0x7F,0x00,0x00}, //124 '|'
    {0x00,0x41,0x36,0x08,0x00}, //125 '}'
    {0x08,0x08,0x2A,0x1C,0x08}, //126 '~'
};

static void sh1106_cmd(uint8_t cmd) {
    uint8_t buf[2] = {0x00, cmd}; // Co=0, D/C#=0 (command)
    HAL_I2C_Master_Transmit(sh1106_i2c, SH1106_I2C_ADDR, buf, 2, 100);
}

static void sh1106_send_page(uint8_t page) {
    // Command header: set page address + column address (with 2-col offset)
    page_buf[0] = 0x80; page_buf[1] = 0xB0 | page;               // page address
    page_buf[2] = 0x80; page_buf[3] = SH1106_COL_OFFSET & 0x0F;  // lower column nibble
    page_buf[4] = 0x80; page_buf[5] = 0x10 | (SH1106_COL_OFFSET >> 4); // upper column nibble
    page_buf[6] = 0x40;                                            // data follows

    memcpy(&page_buf[PAGE_HDR_SIZE], &framebuffer[page * SH1106_WIDTH], SH1106_WIDTH);
    if (HAL_I2C_Master_Transmit_DMA(sh1106_i2c, SH1106_I2C_ADDR, page_buf, PAGE_BUF_SIZE) != HAL_OK) {
        sh1106_dma_busy = 0; // Prevent lockup if DMA fails to start
    }
}

void sh1106_init(I2C_HandleTypeDef *hi2c) {
    sh1106_i2c = hi2c;

    HAL_Delay(100); // Wait for display power-up

    sh1106_cmd(0xAE); // Display OFF
    sh1106_cmd(0xD5); // Set display clock div
    sh1106_cmd(0x80); //   default ratio
    sh1106_cmd(0xA8); // Set multiplex
    sh1106_cmd(0x3F); //   64-1
    sh1106_cmd(0xD3); // Set display offset
    sh1106_cmd(0x00); //   no offset
    sh1106_cmd(0x40); // Set start line = 0
    sh1106_cmd(0xAD); // DC-DC control (SH1106-specific)
    sh1106_cmd(0x8B); //   DC-DC ON
    sh1106_cmd(0xA1); // Segment remap (flip horizontal)
    sh1106_cmd(0xC8); // COM scan direction (flip vertical)
    sh1106_cmd(0xDA); // Set COM pins
    sh1106_cmd(0x12); //   alternative COM pin config
    sh1106_cmd(0x81); // Set contrast
    sh1106_cmd(0xCF); //   max-ish
    sh1106_cmd(0xD9); // Set pre-charge period
    sh1106_cmd(0xF1);
    sh1106_cmd(0xDB); // Set VCOMH deselect level
    sh1106_cmd(0x40);
    sh1106_cmd(0xA4); // Entire display ON (follow RAM)
    sh1106_cmd(0xA6); // Normal display (not inverted)
    sh1106_cmd(0xAF); // Display ON

    sh1106_clear();
    // First update is blocking so init finishes with a clean screen
    for (uint8_t p = 0; p < 8; p++) {
        page_buf[0] = 0x80; page_buf[1] = 0xB0 | p;
        page_buf[2] = 0x80; page_buf[3] = SH1106_COL_OFFSET & 0x0F;
        page_buf[4] = 0x80; page_buf[5] = 0x10 | (SH1106_COL_OFFSET >> 4);
        page_buf[6] = 0x40;
        memcpy(&page_buf[PAGE_HDR_SIZE], &framebuffer[p * SH1106_WIDTH], SH1106_WIDTH);
        HAL_I2C_Master_Transmit(sh1106_i2c, SH1106_I2C_ADDR, page_buf, PAGE_BUF_SIZE, 100);
    }
}

static inline void mark_page_dirty(uint8_t page) {
    dirty_pages |= (1 << page);
}

void sh1106_clear(void) {
    memset(framebuffer, 0, FB_SIZE);
    dirty_pages = 0xFF;
    cursor_x = 0;
    cursor_y = 0;
}

void sh1106_clear_region(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    if (x >= SH1106_WIDTH || y >= SH1106_HEIGHT) return;
    if (x + w > SH1106_WIDTH) w = SH1106_WIDTH - x;
    if (y + h > SH1106_HEIGHT) h = SH1106_HEIGHT - y;

    uint8_t y_end = y + h;
    uint8_t page_start = y / 8;
    uint8_t page_end = (y_end - 1) / 8;

    for (uint8_t page = page_start; page <= page_end; page++) {
        // Build mask for bits within this page that fall inside the region
        uint8_t page_y_top = page * 8;
        uint8_t bit_lo = (y > page_y_top) ? (y - page_y_top) : 0;
        uint8_t bit_hi = ((y_end - 1) < (page_y_top + 7)) ? (y_end - 1 - page_y_top) : 7;
        uint8_t mask = 0;
        for (uint8_t b = bit_lo; b <= bit_hi; b++)
            mask |= (1 << b);
        uint8_t inv_mask = ~mask;

        uint16_t base = page * SH1106_WIDTH;
        for (uint8_t col = x; col < x + w; col++) {
            framebuffer[base + col] &= inv_mask;
        }
        mark_page_dirty(page);
    }
}

// Find next dirty page starting from 'from' (inclusive). Returns 8 if none.
static uint8_t next_dirty_page(uint8_t from) {
    for (uint8_t p = from; p < 8; p++) {
        if (dirty_pages & (1 << p)) return p;
    }
    return 8;
}

void sh1106_update(void) {
    if (sh1106_dma_busy) return;
    if (dirty_pages == 0) return;  // nothing changed

    sh1106_dma_busy = 1;
    current_page = next_dirty_page(0);
    sh1106_send_page(current_page);
}

uint8_t sh1106_is_busy(void) {
    return sh1106_dma_busy;
}

void sh1106_set_brightness(uint8_t val) {
    sh1106_cmd(0x81);  // Set contrast command
    sh1106_cmd(val);   // 0x00 = dimmest, 0xFF = brightest
}

void sh1106_invert_region(uint8_t x, uint8_t y, uint8_t w, uint8_t h) {
    if (x >= SH1106_WIDTH || y >= SH1106_HEIGHT) return;
    if (x + w > SH1106_WIDTH) w = SH1106_WIDTH - x;
    if (y + h > SH1106_HEIGHT) h = SH1106_HEIGHT - y;

    uint8_t y_end = y + h;
    uint8_t page_start = y / 8;
    uint8_t page_end = (y_end - 1) / 8;

    for (uint8_t page = page_start; page <= page_end; page++) {
        uint8_t page_y_top = page * 8;
        uint8_t bit_lo = (y > page_y_top) ? (y - page_y_top) : 0;
        uint8_t bit_hi = ((y_end - 1) < (page_y_top + 7)) ? (y_end - 1 - page_y_top) : 7;
        uint8_t mask = 0;
        for (uint8_t b = bit_lo; b <= bit_hi; b++)
            mask |= (1 << b);

        uint16_t base = page * SH1106_WIDTH;
        for (uint8_t col = x; col < x + w; col++) {
            framebuffer[base + col] ^= mask;
        }
        mark_page_dirty(page);
    }
}

void sh1106_display_off(void) {
    sh1106_cmd(0xAE);
}

void sh1106_display_on(void) {
    sh1106_cmd(0xAF);
}

void sh1106_set_cursor(uint8_t x, uint8_t y) {
    cursor_x = x;
    cursor_y = y;
}

void sh1106_set_font_scale(uint8_t scale) {
    if (scale < 1) scale = 1;
    if (scale > 4) scale = 4;
    font_scale = scale;
}

void sh1106_write_char(char c) {
    if (c < 32 || c > 126) return;

    const uint8_t *glyph = font5x7[c - 32];
    if (font_scale == 1) {
        uint8_t page = cursor_y / 8;
        uint8_t bit_offset = cursor_y % 8;
        mark_page_dirty(page);
        if (bit_offset > 0 && page + 1 < SH1106_HEIGHT / 8)
            mark_page_dirty(page + 1);
        for (uint8_t col = 0; col < 5; col++) {
            if (cursor_x + col < SH1106_WIDTH) {
                uint16_t idx = page * SH1106_WIDTH + cursor_x + col;
                if (idx < FB_SIZE) {
                    framebuffer[idx] |= glyph[col] << bit_offset;
                    if (bit_offset > 0 && page + 1 < SH1106_HEIGHT / 8) {
                        framebuffer[idx + SH1106_WIDTH] |= glyph[col] >> (8 - bit_offset);
                    }
                }
            }
        }
    } else if (font_scale == 2) {
        uint8_t page = cursor_y / 8;
        uint8_t bit_offset = cursor_y % 8;
        mark_page_dirty(page);
        if (page + 1 < SH1106_HEIGHT / 8) mark_page_dirty(page + 1);
        if (bit_offset > 0 && page + 2 < SH1106_HEIGHT / 8) mark_page_dirty(page + 2);
        for (uint8_t col = 0; col < 5; col++) {
            uint16_t expanded = 0;
            uint8_t g = glyph[col];
            for (uint8_t i = 0; i < 7; i++) {
                if (g & (1 << i))
                    expanded |= (3u << (i * 2));
            }
            uint32_t shifted = (uint32_t)expanded << bit_offset;
            for (uint8_t dx = 0; dx < 2; dx++) {
                uint8_t x = cursor_x + col * 2 + dx;
                if (x >= SH1106_WIDTH) continue;
                uint16_t idx = page * SH1106_WIDTH + x;
                if (idx < FB_SIZE)
                    framebuffer[idx] |= (uint8_t)shifted;
                if (page + 1 < SH1106_HEIGHT / 8)
                    framebuffer[idx + SH1106_WIDTH] |= (uint8_t)(shifted >> 8);
                if (bit_offset > 0 && page + 2 < SH1106_HEIGHT / 8)
                    framebuffer[idx + 2 * SH1106_WIDTH] |= (uint8_t)(shifted >> 16);
            }
        }
    } else {
        // Scale 3 and 4: expand each glyph column into a tall bit pattern,
        // then write directly to framebuffer pages (no set_pixel overhead)
        uint8_t bit_offset = cursor_y % 8;
        uint8_t base_page = cursor_y / 8;
        uint8_t total_height = 7 * font_scale;  // max pixel height
        uint8_t max_page = (cursor_y + total_height - 1) / 8;
        if (max_page >= SH1106_HEIGHT / 8) max_page = SH1106_HEIGHT / 8 - 1;
        for (uint8_t p = base_page; p <= max_page; p++)
            mark_page_dirty(p);

        for (uint8_t col = 0; col < 5; col++) {
            // Build expanded column: each source bit becomes font_scale bits
            uint32_t expanded = 0;
            uint8_t g = glyph[col];
            for (uint8_t i = 0; i < 7; i++) {
                if (g & (1 << i)) {
                    uint32_t block = ((1u << font_scale) - 1);  // font_scale 1-bits
                    expanded |= block << (i * font_scale);
                }
            }

            // Shift to bit_offset within page and write to framebuffer columns
            uint64_t shifted = (uint64_t)expanded << bit_offset;
            for (uint8_t dx = 0; dx < font_scale; dx++) {
                uint8_t x = cursor_x + col * font_scale + dx;
                if (x >= SH1106_WIDTH) continue;
                for (uint8_t p = base_page; p <= max_page && p < SH1106_HEIGHT / 8; p++) {
                    uint16_t idx = p * SH1106_WIDTH + x;
                    uint8_t byte = (uint8_t)(shifted >> ((p - base_page) * 8));
                    framebuffer[idx] |= byte;
                }
            }
        }
    }
    cursor_x += (5 + 1) * font_scale;
}

void sh1106_write_string(const char *str) {
    while (*str) {
        sh1106_write_char(*str++);
    }
}

void sh1106_write_string_centered(const char *str, uint8_t y) {
    uint8_t len = 0;
    const char *p = str;
    while (*p++) len++;
    uint16_t text_w = (uint16_t)len * 6 * font_scale;
    cursor_x = (text_w < SH1106_WIDTH) ? (SH1106_WIDTH - text_w) / 2 : 0;
    cursor_y = y;
    while (*str) {
        sh1106_write_char(*str++);
    }
}

void HAL_I2C_MasterTxCpltCallback(I2C_HandleTypeDef *hi2c) {
    if (hi2c == sh1106_i2c) {
        dirty_pages &= ~(1 << current_page);  // mark sent page clean
        uint8_t next = next_dirty_page(current_page + 1);
        if (next < 8) {
            current_page = next;
            sh1106_send_page(current_page);
        } else {
            sh1106_dma_busy = 0;
        }
    }
}

void HAL_I2C_ErrorCallback(I2C_HandleTypeDef *hi2c) {
    if (hi2c == sh1106_i2c) {
        sh1106_dma_busy = 0;
    }
}
