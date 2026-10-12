#pragma once
#include "Arduino.h"
extern char g_lcd[2][17];
extern int g_cur_row;
struct LiquidCrystal_I2C {
  LiquidCrystal_I2C(uint8_t, uint8_t, uint8_t) {}
  void init() {}
  void backlight() {}
  void clear() { memset(g_lcd, ' ', sizeof(g_lcd)); }
  void setCursor(int, int r) { g_cur_row = r; }
  void print(const char *s) { snprintf(g_lcd[g_cur_row], 17, "%-16s", s); }
};
