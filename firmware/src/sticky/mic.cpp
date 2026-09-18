#include "sticky/mic.h"

#include <Arduino.h>
#include <driver/gpio.h>
#include <math.h>
#include <soc/usb_serial_jtag_struct.h>

namespace {

// Samples pulled per I2S read inside readLevel(). Small enough to keep off the
// stack budget, large enough that the per-read overhead does not matter.
constexpr uint32_t kChunkSamples = 256;

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

// The arithmetic behind a MicLevel, kept apart from where the samples come
// from: readLevel() feeds it one I2S read at a time.
//
// One pass collects everything a level needs. The sums give mean and variance
// (variance = mean of squares - square of mean, which is the DC-free power),
// and the extremes give the peak deviation once the mean is known. int64
// accumulators, because 32768^2 per sample overflows int32 after two samples.
class MicLevelMeter {
 public:
  void add(const int16_t* samples, uint32_t count) {
    for (uint32_t i = 0; i < count; ++i) {
      const int32_t s = samples[i];
      _sum += s;
      _sumSquares += static_cast<int64_t>(s) * s;
      if (s < _smallest) _smallest = s;
      if (s > _largest) _largest = s;
    }
    _count += count;
  }

  MicLevel result() const {
    MicLevel out{0.0f, StickyMic::kSilenceDbfs, 0.0f, _count};
    if (_count == 0) return out;

    const double mean = static_cast<double>(_sum) / _count;
    double variance = static_cast<double>(_sumSquares) / _count - mean * mean;
    if (variance < 0.0) variance = 0.0;  // rounding can push a silent block below zero

    const double rms = sqrt(variance);
    const double peakHigh = _largest - mean;
    const double peakLow = mean - _smallest;

    out.rms = static_cast<float>(rms);
    out.peak = static_cast<float>(peakHigh > peakLow ? peakHigh : peakLow);
    if (rms > 0.0) out.dbfs = static_cast<float>(20.0 * log10(rms / 32768.0));
    return out;
  }

 private:
  int64_t _sum = 0;
  int64_t _sumSquares = 0;
  int32_t _smallest = INT16_MAX;
  int32_t _largest = INT16_MIN;
  uint32_t _count = 0;
};

}  // namespace

bool StickyMic::begin(uint32_t sampleRate, uint32_t settleMs) {
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
  if (settleMs > 0) {
    MicLevel discard;
    readLevel(discard, (sampleRate * settleMs) / 1000);
  }

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

  MicLevelMeter meter;
  uint32_t collected = 0;
  int16_t chunk[kChunkSamples];
  while (collected < samples) {
    uint32_t want = samples - collected;
    if (want > kChunkSamples) want = kChunkSamples;

    const uint32_t got = readSamples(chunk, want);
    if (got == 0) return false;

    meter.add(chunk, got);
    collected += got;
  }

  out = meter.result();
  return true;
}

uint32_t StickyMic::readSamples(int16_t* dest, uint32_t maxSamples) {
  if (!_started) {
    _lastError = "microphone not started";
    return 0;
  }
  if (dest == nullptr || maxSamples == 0) {
    _lastError = "zero-length read";
    return 0;
  }

  const size_t bytes =
      _i2s.readBytes(reinterpret_cast<char*>(dest), maxSamples * sizeof(int16_t));
  if (bytes == 0) {
    // readBytes() reports a failed read as nothing read at all, so there is no
    // partial result to salvage here.
    _lastError = "I2S read returned no data";
    return 0;
  }

  _lastError = "";
  return static_cast<uint32_t>(bytes / sizeof(int16_t));
}
