#pragma once

#include <ESP_I2S.h>

#include <stdint.h>

// reTerminal Sticky (E1005) onboard PDM microphone.
//
//   Function       ESP32-S3 GPIO
//   PDM CLK        19   (also native USB-Serial-JTAG D-)
//   PDM DATA       20   (also native USB-Serial-JTAG D+)
//   MIC power EN   38   (TPS22916 load switch, active high)
//
// Two things about this wiring drive the implementation:
//
//  * The mic is behind a load switch, so GPIO38 has to be driven high and the
//    rail given time to settle before the first sample means anything.
//  * The data and clock lines are the two pins the ESP32-S3 reserves for its
//    native USB-Serial-JTAG PHY, which this board's Arduino profile starts on
//    boot (ARDUINO_USB_CDC_ON_BOOT=1). That PHY owns the pads directly rather
//    than through the GPIO matrix, so it must be switched off by hand or the
//    mic reads silence. Nothing is lost: flashing and logging on the Sticky go
//    through the CH343P bridge on UART0 (GPIO43/44), which is what Serial1 in
//    this sketch already uses.

struct MicLevel {
  float rms;        // RMS amplitude after DC removal, 0 .. 32768 (16-bit FS)
  float dbfs;       // 20*log10(rms / 32768); kSilenceDbfs when rms is 0
  float peak;       // largest deviation from DC in the block, same scale
  uint32_t samples; // how many samples actually went into the numbers
};

class StickyMic {
 public:
  static constexpr int kPinClk = 19;
  static constexpr int kPinData = 20;
  static constexpr int kPinPowerEnable = 38;
  static constexpr uint32_t kSampleRate = 16000;

  // Audio discarded right after the mic rail comes up, in milliseconds.
  //
  // Measured -- docs/experiments.md, E3. The power-up transient is above speech
  // level for the first 8 ms only; after 16 ms it sits more than 13 dB below
  // speech and keeps falling. The level does not reach the room's noise floor
  // for about 200 ms, but everything after the first few milliseconds is usable
  // audio. Three 8 ms blocks of margin is the compromise.
  //
  // begin() takes it as an argument so the experiment can set it to zero.
  static constexpr uint32_t kSettleMs = 24;

  // Floor reported instead of -inf dBFS for a digitally silent block.
  static constexpr float kSilenceDbfs = -120.0f;

  // Powers the mic rail, frees GPIO19/20 from the USB PHY and starts I2S in
  // PDM-RX mode (16-bit mono, hardware PDM->PCM filter). The first settleMs of
  // audio is read and dropped: the MEMS mic and the PDM2PCM filter both need
  // to settle, and until they do every block reads as a loud transient.
  bool begin(uint32_t sampleRate = kSampleRate, uint32_t settleMs = kSettleMs);

  // Stops I2S and cuts power to the mic.
  void end();

  uint32_t sampleRate() const { return _sampleRate; }

  // Reads `samples` PCM samples and reduces them to one level reading. Blocks
  // for samples/sampleRate seconds, so 1024 samples at 16 kHz is a 64 ms call.
  //
  // The DC offset is measured and removed per block. The S3's PDM2PCM path has
  // no high-pass filter stage, so the raw samples ride on the microphone's own
  // bias; without subtracting it the "silence" reading would sit at whatever
  // that bias happens to be instead of near zero.
  bool readLevel(MicLevel& out, uint32_t samples = 1024);

  // Reads PCM straight into the caller's buffer -- one I2S read, no averaging
  // and no DC removal, so the samples arrive carrying the microphone's own bias
  // exactly as the hardware delivered them. Returns how many samples were
  // written, or 0 with lastError() set.
  //
  // Blocks until the buffer is full: the read underneath loops until it has
  // every sample asked for, so the call takes maxSamples/sampleRate seconds.
  // The I2S DMA holds 6 x 240 frames -- 90 ms at 16 kHz -- which is the whole
  // budget a caller has to spend between two of these.
  uint32_t readSamples(int16_t* dest, uint32_t maxSamples);

  // Human-readable reason the last begin()/readLevel() returned false.
  const char* lastError() const { return _lastError; }

 private:
  I2SClass _i2s;
  bool _started = false;
  uint32_t _sampleRate = kSampleRate;
  const char* _lastError = "";
};
