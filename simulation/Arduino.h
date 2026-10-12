// Minimal Arduino API mock so the sketch logic can be tested on a PC.
#pragma once
#include <stdint.h>
#include <stdio.h>
#include <string.h>
#include <string>
#include <deque>
typedef uint8_t byte;
#define HIGH 1
#define LOW 0
#define INPUT 0
#define OUTPUT 1
#define INPUT_PULLUP 2
#define HEX 16
#define F(x) (x)

// ---- virtual hardware state, driven by the test harness ----
extern unsigned long g_millis;
extern int g_pin[32];            // pin levels (inputs are written by the test)
extern int g_mode[32];
extern std::deque<char> g_serial_in;

inline unsigned long millis() { return g_millis; }
inline void pinMode(uint8_t p, uint8_t m) { g_mode[p] = m; }
inline void digitalWrite(uint8_t p, uint8_t v) { g_pin[p] = v; }
inline int digitalRead(uint8_t p) { return g_pin[p]; }

struct SerialMock {
  void begin(int) {}
  int available() { return (int)g_serial_in.size(); }
  char read() { char c = g_serial_in.front(); g_serial_in.pop_front(); return c; }
  void print(const char *s) { printf("%s", s); }
  void print(char c) { printf("%c", c); }
  void print(int v) { printf("%d", v); }
  void print(unsigned v) { printf("%u", v); }
  void print(unsigned long v) { printf("%lu", v); }
  void print(byte v, int base) { printf(base == 16 ? "%X" : "%u", v); }
  void println(const char *s) { printf("%s\n", s); }
  void println(int v) { printf("%d\n", v); }
  void println(unsigned v) { printf("%u\n", v); }
};
extern SerialMock Serial;
