#include "sticky_mic.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <math.h>
#include <soc/usb_serial_jtag_struct.h>

namespace {

// Samples pulled per I2S read inside readLevel(). Small enough to keep off the
// stack budget, large enough that the per-read overhead does not matter.
constexpr uint32_t kChunkSamples = 256;

// Audio discarded right after the mic rail comes up, in milliseconds.
constexpr uint32_t kSettleMs = 200;

// GPIO19/20 carry the native USB-Serial-JTAG D-/D+ signals. Clearing the GPIO
// matrix is not enough, because the USB function is a dedicated pad connection
// that bypasses the matrix entirely -- the pad enable in the peripheral itself
// has to go low first, and only then does gpio_reset_pin() hand the pins back.
void releaseUsbJtagPins() {
#if ARDUINO_USB_CDC_ON_BOOT
  // Shuts down the CDC driver and drops its peripheral-manager claim on the
  // two pins, so the I2S driver's own claim does not collide with it.
  Serial.end();
#endif
  USB_SERIAL_JTAG.conf0.usb_pad_enable = 0;
  gpio_reset_pin(GPIO_NUM_19);
  gpio_reset_pin(GPIO_NUM_20);
}

}  // namespace

bool StickyMic::begin(uint32_t sampleRate) {
  if (_started) return true;

  _sampleRate = sampleRate;

  pinMode(kPinPowerEnable, OUTPUT);
  digitalWrite(kPinPowerEnable, HIGH);
  delay(10);  // TPS22916 turn-on time plus mic power-up.

  releaseUsbJtagPins();

  _i2s.setPinsPdmRx(kPinClk, kPinData);
  if (!_i2s.begin(I2S_MODE_PDM_RX, sampleRate, I2S_DATA_BIT_WIDTH_16BIT,
                  I2S_SLOT_MODE_MONO)) {
    digitalWrite(kPinPowerEnable, LOW);
    _lastError = "I2S PDM-RX init failed";
    return false;
  }

  _started = true;
  _lastError = "";

  // Drain the settling period so the caller's first readLevel() is already
  // looking at real audio.
  MicLevel discard;
  readLevel(discard, (sampleRate * kSettleMs) / 1000);

  return true;
}

void StickyMic::end() {
  if (_started) {
    _i2s.end();
    _started = false;
  }
  digitalWrite(kPinPowerEnable, LOW);
}

bool StickyMic::readLevel(MicLevel& out, uint32_t samples) {
  out = MicLevel{0.0f, kSilenceDbfs, 0.0f, 0};

  if (!_started) {
    _lastError = "microphone not started";
    return false;
  }
  if (samples == 0) {
    _lastError = "zero-length read";
    return false;
  }

  // One pass over the block collects everything the level needs: the sums give
  // mean and variance (variance = mean of squares - square of mean, which is
  // the DC-free power), and the extremes give the peak deviation once the mean
  // is known. int64 accumulators because 32768^2 per sample overflows int32
  // after only two samples.
  int64_t sum = 0;
  int64_t sumSquares = 0;
  int32_t smallest = INT16_MAX;
  int32_t largest = INT16_MIN;
  uint32_t collected = 0;

  int16_t chunk[kChunkSamples];
  while (collected < samples) {
    uint32_t want = samples - collected;
    if (want > kChunkSamples) want = kChunkSamples;

    size_t bytes = _i2s.readBytes(reinterpret_cast<char*>(chunk), want * sizeof(int16_t));
    uint32_t got = bytes / sizeof(int16_t);
    if (got == 0) {
      _lastError = "I2S read returned no data";
      return false;
    }

    for (uint32_t i = 0; i < got; ++i) {
      const int32_t s = chunk[i];
      sum += s;
      sumSquares += static_cast<int64_t>(s) * s;
      if (s < smallest) smallest = s;
      if (s > largest) largest = s;
    }
    collected += got;
  }

  const double mean = static_cast<double>(sum) / collected;
  double variance = static_cast<double>(sumSquares) / collected - mean * mean;
  if (variance < 0.0) variance = 0.0;  // rounding can push a silent block below zero

  const double rms = sqrt(variance);
  const double peakHigh = largest - mean;
  const double peakLow = mean - smallest;

  out.rms = static_cast<float>(rms);
  out.peak = static_cast<float>(peakHigh > peakLow ? peakHigh : peakLow);
  out.dbfs = rms > 0.0 ? static_cast<float>(20.0 * log10(rms / 32768.0))
                       : kSilenceDbfs;
  out.samples = collected;

  _lastError = "";
  return true;
}
