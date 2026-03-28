/*
  u8g2_d_memory.c — DreamDexed minimal version
  Only includes buffer allocation for 128x64 (16 tiles wide × 8 tiles tall).
  Replaces the full u8g2_d_memory.c which has allocators for every display size.
*/
#include "u8g2.h"

uint8_t *u8g2_m_16_8_f(uint8_t *page_cnt)
{
  static uint8_t buf[1024];  /* 128 × 64 / 8 = 1024 bytes */
  *page_cnt = 8;
  return buf;
}
