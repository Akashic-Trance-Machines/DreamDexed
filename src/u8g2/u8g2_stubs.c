/*
  u8g2_stubs.c — DreamDexed stubs for unused u8g2 functions
  These functions are referenced but never called in our use case.
  Providing stubs avoids linker errors without pulling in extra source files.
*/
#include "u8g2.h"

/* Kerning stubs (referenced by u8g2_font.c DrawExtendedUTF8) */
uint8_t u8g2_GetKerning(u8g2_t *u8g2, u8g2_kerning_t *kerning, uint16_t e1, uint16_t e2)
{
  (void)u8g2; (void)kerning; (void)e1; (void)e2;
  return 0;
}

uint8_t u8g2_GetKerningByTable(u8g2_t *u8g2, const uint16_t *kt, uint16_t e1, uint16_t e2)
{
  (void)u8g2; (void)kt; (void)e1; (void)e2;
  return 0;
}

/* Screen capture stubs (referenced by u8g2_buffer.c PBM/XBM export) */
uint8_t u8x8_capture_get_pixel_1(uint16_t x, uint16_t y, uint8_t *dest_ptr, uint8_t tile_width)
{
  (void)x; (void)y; (void)dest_ptr; (void)tile_width;
  return 0;
}

uint8_t u8x8_capture_get_pixel_2(uint16_t x, uint16_t y, uint8_t *dest_ptr, uint8_t tile_width)
{
  (void)x; (void)y; (void)dest_ptr; (void)tile_width;
  return 0;
}

void u8x8_capture_write_pbm_pre(uint8_t tile_width, uint8_t tile_height, void (*out)(const char *s))
{
  (void)tile_width; (void)tile_height; (void)out;
}

void u8x8_capture_write_pbm_buffer(uint8_t *buffer, uint8_t tile_width, uint8_t tile_height,
                                    uint8_t (*get_pixel)(uint16_t x, uint16_t y, uint8_t *dest_ptr, uint8_t tile_width),
                                    void (*out)(const char *s))
{
  (void)buffer; (void)tile_width; (void)tile_height; (void)get_pixel; (void)out;
}

void u8x8_capture_write_xbm_pre(uint8_t tile_width, uint8_t tile_height, void (*out)(const char *s))
{
  (void)tile_width; (void)tile_height; (void)out;
}

void u8x8_capture_write_xbm_buffer(uint8_t *buffer, uint8_t tile_width, uint8_t tile_height,
                                    uint8_t (*get_pixel)(uint16_t x, uint16_t y, uint8_t *dest_ptr, uint8_t tile_width),
                                    void (*out)(const char *s))
{
  (void)buffer; (void)tile_width; (void)tile_height; (void)get_pixel; (void)out;
}

