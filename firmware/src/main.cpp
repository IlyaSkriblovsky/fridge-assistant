#include <Arduino.h>
#include <HTTPClient.h>
#include <WiFi.h>
#include <Seeed_GFX.h>
#include <driver/gpio.h>

#include <algorithm>
#include <vector>

#include "secrets.h"
#include "sticky_epaper.h"
#include "sticky_mic.h"
#include "sticky_power.h"

constexpr int PIN_LOG_RX = 44;
constexpr int PIN_LOG_TX = 43;

// reTerminal Sticky (E1005) panel wiring is owned by Board_reTerminal_Sticky:
//   SCK 13, MOSI 14, MISO 12 (shared with microSD), CS 15, DC 16,
//   RST 17, BUSY 18, panel power enable 47, microSD CS 8.
// The sketch must not touch those pins by hand.
//
// This unit has SSD1677 glass, so it is bound directly to the corrected
// SSD1677 driver instead of Seeed_Product::reTerminal_Sticky. The catalog
// entry would auto-detect SSD1677 vs SSD2677, but its SSD1677 path renders
// inverted -- see sticky_epaper.h.
Seeed_GFX display;
StickyMic mic;

void setupDisplay() {
  if (!display.begin<Board_reTerminal_Sticky, Config_Sticky_SSD1677_Fixed>()) {
    Serial1.printf("EPaper begin FAILED: %s\n", display.lastResult().message);
    return;
  }

  Serial1.printf("EPaper driver: %s, %dx%d\n", display.driverPtr()->name(),
                 display.width(), display.height());

  // Pre-clear. The controller powers up with unknown data in its previous-image
  // RAM, so the first refresh has to physically flush the panel to white before
  // anything drawn on top of it is trustworthy.
  display.fillScreen(TFT_WHITE);
  display.refresh();

  Serial1.println("EPaper initialized");
}

bool setupWifi() {
  WiFi.persistent(false);
  if (!WiFi.mode(WIFI_STA)) {
    Serial1.println("Failed to enable the WiFi station interface!");
    return false;
  }
  WiFi.setAutoReconnect(false);
  WiFi.disconnect(/*wifioff=*/false, /*eraseap=*/false);
  delay(100);

  Serial1.printf("WiFi status: %d\n", WiFi.status());
  Serial1.printf("MAC: %s\n", WiFi.macAddress().c_str());

  if (secrets::kWifiSsid[0] == '\0') {
    Serial1.println(
        "WiFi credentials are empty -- fill in src/secrets.h "
        "(template: src/secrets.example.h)");
    return false;
  }

  WiFi.mode(WIFI_STA);
  WiFi.begin(secrets::kWifiSsid, secrets::kWifiPassword);
  wl_status_t result = static_cast<wl_status_t>(WiFi.waitForConnectResult());
  Serial1.printf("WiFi connect result: %d\n", result);

  return result == WL_CONNECTED;
}

// Short HTTP demo: plain GET over http:// (no TLS, so no CA bundle needed),
// with the status line and the head of the body logged to Serial1.
void httpGetExample() {
  const char* kUrl = "http://example.com/";

  HTTPClient http;
  if (!http.begin(kUrl)) {
    Serial1.println("HTTP begin failed");
    return;
  }
  http.setTimeout(10000);
  http.setUserAgent("sticky-test-2");

  int status = http.GET();
  if (status <= 0) {
    // Negative codes are client-side failures (DNS, connect, timeout), not
    // HTTP responses, so there is nothing to read out of the stream.
    Serial1.printf("HTTP GET failed: %d (%s)\n", status,
                   HTTPClient::errorToString(status).c_str());
    http.end();
    return;
  }

  Serial1.printf("HTTP %d, content-length: %d\n", status, http.getSize());

  String body = http.getString();
  Serial1.printf("Body (%u bytes), first 200:\n", body.length());
  Serial1.println(body.substring(0, 200));

  http.end();
}

bool micReady = false;

void setup() {
  // First thing on boot -- everything below depends on the board staying alive.
  stickyPower::holdLatch();

  Serial1.begin(115200, SERIAL_8N1, PIN_LOG_RX, PIN_LOG_TX);
  delay(50);
  Serial1.println("Sticky Test 2");
  Serial1.printf("Power latch held: HOLD GPIO%d, LOCK GPIO%d\n",
                 stickyPower::kPinHold, stickyPower::kPinLock);

  setupDisplay();

  if (setupWifi()) {
    Serial1.printf("IP: %s\n", WiFi.localIP().toString().c_str());
    httpGetExample();
  }

  // Started last: begin() switches off the USB-Serial-JTAG PHY to take
  // GPIO19/20, and nothing above needs those pins.
  micReady = mic.begin();
  if (!micReady) {
    Serial1.printf("Microphone begin FAILED: %s\n", mic.lastError());
    return;
  }
  Serial1.printf("Microphone ready: PDM %lu Hz on CLK %d / DATA %d\n",
                 static_cast<unsigned long>(mic.sampleRate()),
                 StickyMic::kPinClk, StickyMic::kPinData);
}

void loop() {
}
