#pragma once
#include "Arduino.h"
// 1 KB EEPROM persisted to a file so "power cycles" can be simulated.
extern uint8_t g_eeprom[1024];
struct EEPROMMock {
  uint8_t read(int a) { return g_eeprom[a]; }
  void update(int a, uint8_t v) { if (g_eeprom[a] != v) g_eeprom[a] = v; }
  void write(int a, uint8_t v) { g_eeprom[a] = v; }
  template <typename T> T &get(int a, T &t) { memcpy(&t, &g_eeprom[a], sizeof(T)); return t; }
  template <typename T> const T &put(int a, const T &t) { memcpy(&g_eeprom[a], &t, sizeof(T)); return t; }
};
extern EEPROMMock EEPROM;
