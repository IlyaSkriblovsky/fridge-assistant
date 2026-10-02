#pragma once
#include <stdint.h>
#include <stddef.h>
#include <stdlib.h>
#define PROGMEM
#define F(value) value
#define pgm_read_byte(address) (*(const uint8_t*)(address))
#define OUTPUT 1
#define LOW 0
#define HIGH 1
#define ESP_ARDUINO_VERSION 0x030307
class Print {
 public:
  template<class T> void print(T) {}
  template<class T> void println(T) {}
  void println() {}
};
inline long random(long low, long) { return low; }
unsigned long millis();
void delay(unsigned long ms);
bool ledcAttach(uint8_t pin, uint32_t hz, uint8_t resolution);
bool ledcDetach(uint8_t pin);
uint32_t ledcWriteTone(uint8_t pin, uint32_t hz);
void pinMode(uint8_t pin, uint8_t mode);
void digitalWrite(uint8_t pin, uint8_t level);
