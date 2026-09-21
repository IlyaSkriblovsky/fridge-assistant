#pragma once

#include <Seeed_GFX.h>
#include <esp_heap_caps.h>

#include <initializer_list>
#include <stdlib.h>
#include <string.h>

#include "board/boards/reTerminal_EPaper_Boards.h"
#include "driver/epaper/Driver_SSD1677.h"
#include "panel/Panel_EPaper.h"

// Two corrections to the stock SSD1677 path and one wait taken off it, all
// applied where the mismatch actually is -- on the way to the controller -- so
// the library's frame buffer keeps its own conventions.
//
// The first two are bugs and the third is a cost: removing either correction
// brings back a broken image, while removing the third only makes every screen
// a tenth of a second later.
//
// 1. Polarity.
//    Seeed_GFX2's 1bpp frame buffer stores 1 = black, 0 = white
//    (Panel_EPaper.cpp writePixel/readPixel, and the memset(0x00) that seeds the
//    "previous frame is all white" buffer). Driver_SSD2677 honours that when it
//    expands the buffer to the controller's 2bpp format (black -> 0x03,
//    white -> 0x00), but Driver_SSD1677 streams the bytes into RAM 0x24/0x26
//    verbatim -- and on SSD1677 a RAM bit of 1 means *white*. So on a Sticky
//    with SSD1677 glass the whole image comes out inverted.
//
// 2. The previous-image plane on partial refresh.
//    An SSD1677 partial update (0x22 = 0xFF, display mode 2) is differential:
//    for every pixel the waveform is chosen from the pair (RAM 0x26, RAM 0x24),
//    i.e. (what is on the glass, what should be on it). Panel_EPaper::update()
//    pushes both planes, but Panel_EPaper::updatePartial() pushes only 0x24 and
//    never refreshes 0x26. So from the second partial refresh onwards the
//    controller is told the panel still shows whatever was there at the last
//    full refresh; every pixel that has gone black since is left alone instead
//    of being driven back to white, and successive frames pile up on top of
//    each other until the next full refresh clears them.
//
//    Seeed's own ESP-IDF driver writes both planes here -- previous to 0x26 and
//    current to 0x24, inside the same address window (reTerminal_Sticky_Bunny,
//    components/seeed_epaper/epaper_panel.c). This class supplies the missing
//    plane by shadowing what it streams to the controller: every push to 0x24 is
//    also recorded into a full-frame buffer, and that buffer is what goes into
//    0x26 ahead of the next partial update. The shadow is kept in controller
//    storage order -- already mirrored and row-reversed exactly as the panel
//    handed the bytes over -- so it stays correct whatever the window is.
//    After power loss, a reconstructed window alone is insufficient: 0x44/45
//    constrain RAM writes, not the display scan. Shadow priming therefore also
//    arms identical full-plane RAM initialization before the next partial. Only
//    the requested window then gets differing old/new data. See the silent-mode
//    hardware feedback in docs/implementation.md and test/test_epaper/.
//
// 3. The hundred milliseconds after the controller is already asleep.
//    Driver_SSD1677::sleep() writes 0x10/0x01 and then delay(100), and
//    Panel_EPaper::ePaperSleep() calls it and adds a second delay(100) of its
//    own -- E8 in docs/experiments.md measured both, on every refresh, with the
//    final image on the glass for all 200 ms of it. Nothing waits on the
//    driver's half. Deep sleep mode 1 can only be left through the reset pin,
//    so the only thing that ever follows this command is the hardwareReset()
//    at the top of the next refresh, which does not care what state it is
//    resetting; and on the way to the board's own sleep,
//    stickyPower::prepareDeepSleep() parks EPD_EN low and takes the panel's
//    rail with it, by which point the analog side has been off since the
//    power-down steps of 0x22 that the refresh waited out in full.
//
//    The panel's half of the wait cannot be reached from here: ePaperSleep()
//    is private and non-virtual. It stays, and it stays measured.
class Driver_SSD1677_Sticky : public Driver_SSD1677 {
public:
    Driver_SSD1677_Sticky(uint16_t w = 800, uint16_t h = 480, int8_t busyPin = -1)
        : Driver_SSD1677(w, h, busyPin),
          _nativeStride(static_cast<uint16_t>((w + 7) / 8)),
          _nativeRows(h) {
        setFullWindow();
    }

    ~Driver_SSD1677_Sticky() override { heap_caps_free(_shadow); }

    const char* name() const override { return "SSD1677 (inverted)"; }

    // 0x10/0x01 without the library's delay(100) after it. See note 3 above.
    void sleep() override {
        if (!_bus) return;
        _bus->writeCommand(0x10);
        _bus->writeData(0x01);
    }

    // Panel_EPaper enters partial mode through wakePartial() and only ever
    // pushes a previous plane itself on the full-refresh path, so this pair of
    // hooks is enough to tell the two paths apart.
    void wakePartial() override {
        _partial = true;
        Driver_SSD1677::wakePartial();
    }

    void pushOldColors(const uint8_t* data, size_t len) override {
        // Full refresh: the panel supplies its own previous frame, and init()
        // has just reset the controller's address window to the whole panel.
        _partial = false;
        setFullWindow();
        pushInvertedRows(0x26, data, len, len, 1);
    }

    // Feed a reconstructed window through the panel's normal transforms,
    // recording its bytes without sending an image or triggering a waveform.
    // Register setup/sleep still run; endShadowPrime() restores normal updates.
    bool beginShadowPrime() {
        if (!ensureShadow()) return false;
        _primeShadow = true;
        return true;
    }
    void endShadowPrime() {
        _primeShadow = false;
        _seedController = true;
    }

    void updatePartial() override {
        if (!_primeShadow) Driver_SSD1677::updatePartial();
    }

    void pushNewColors(const uint8_t* data, size_t len) override {
        if (_primeShadow) {
            recordDisplayed(data, len);
            return;
        }
        if (_partial && _seedController) {
            // The address window limits RAM writes, not the optical scan.
            // Panel power was lost in deep sleep: RAM outside the reconstructed
            // window is undefined. Give BOTH planes the same baseline before
            // installing the changed window. Equal pairs outside it request no
            // transition in the panel's differential waveform, even though the
            // baseline there is not a copy of the image retained on the glass.
            seedControllerPlanes();
            _seedController = false;
        }
        if (_partial) pushShadowAsPrevious(len);
        pushInvertedRows(0x24, data, len, len, 1);
        recordDisplayed(data, len);
    }

    void setAddrWindow(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye) override {
        _winX = xs;
        _winY = ys;
        _winW = static_cast<uint16_t>(xe - xs + 1);
        _winH = static_cast<uint16_t>(ye - ys + 1);
        Driver_SSD1677::setAddrWindow(xs, ys, xe, ye);
    }

private:
    void seedControllerPlanes() {
        // Call the base method so the bookkeeping still describes the small
        // window. Reset the RAM counters separately for each full-plane write,
        // then restore the window/counters before the normal old/new upload.
        const size_t bytes = static_cast<size_t>(_nativeStride) * _nativeRows;
        for (uint8_t command : {uint8_t(0x26), uint8_t(0x24)}) {
            Driver_SSD1677::setAddrWindow(0, 0, _nativeStride * 8 - 1, _nativeRows - 1);
            pushInvertedRows(command, _shadow, bytes, bytes, 1);
        }
        Driver_SSD1677::setAddrWindow(_winX, _winY, _winX + _winW - 1, _winY + _winH - 1);
    }

    void setFullWindow() {
        _winX = 0;
        _winY = 0;
        _winW = static_cast<uint16_t>(_nativeStride * 8);
        _winH = _nativeRows;
    }

    // True when the current window maps onto whole shadow bytes, which is the
    // only shape this bookkeeping models. Partial windows are 8-pixel aligned by
    // Panel_EPaper, so in practice this always holds.
    bool windowIsAddressable(size_t len) const {
        if ((_winX % 8) != 0 || (_winW % 8) != 0 || _winW == 0 || _winH == 0) return false;
        const uint16_t rowBytes = static_cast<uint16_t>(_winW / 8);
        if (len != static_cast<size_t>(rowBytes) * _winH) return false;
        return (_winX / 8) + rowBytes <= _nativeStride &&
               static_cast<uint32_t>(_winY) + _winH <= _nativeRows;
    }

    // Lazily allocated, seeded all-white (0x00) to match the assumption
    // Panel_EPaper makes about a panel it has not refreshed yet.
    bool ensureShadow() {
        if (_shadow) return true;
        const size_t bytes = static_cast<size_t>(_nativeStride) * _nativeRows;
        _shadow = static_cast<uint8_t*>(
            heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT));
        if (!_shadow) {
            _shadow = static_cast<uint8_t*>(heap_caps_malloc(bytes, MALLOC_CAP_8BIT));
        }
        if (_shadow) memset(_shadow, 0x00, bytes);
        return _shadow != nullptr;
    }

    void recordDisplayed(const uint8_t* data, size_t len) {
        if (!data || !windowIsAddressable(len) || !ensureShadow()) return;
        const uint16_t rowBytes = static_cast<uint16_t>(_winW / 8);
        for (uint16_t row = 0; row < _winH; ++row) {
            memcpy(_shadow + static_cast<size_t>(_winY + row) * _nativeStride + (_winX / 8),
                   data + static_cast<size_t>(row) * rowBytes, rowBytes);
        }
    }

    void pushShadowAsPrevious(size_t len) {
        if (!_shadow || !windowIsAddressable(len)) return;
        const uint16_t rowBytes = static_cast<uint16_t>(_winW / 8);
        pushInvertedRows(0x26, _shadow + static_cast<size_t>(_winY) * _nativeStride + (_winX / 8),
                         _nativeStride, rowBytes, _winH);
        // Streaming the previous plane left the RAM address counter at the end
        // of the window; put it back so the 0x24 stream starts at the origin.
        Driver_SSD1677::setAddrWindow(_winX, _winY,
                                      static_cast<uint16_t>(_winX + _winW - 1),
                                      static_cast<uint16_t>(_winY + _winH - 1));
    }

    // Streams `rows` runs of `rowBytes` bytes, taken `srcStride` apart, into the
    // given RAM with the polarity flipped on the way out.
    void pushInvertedRows(uint8_t command, const uint8_t* src, size_t srcStride,
                          size_t rowBytes, uint16_t rows) {
        if (!_bus || !src) return;
        _bus->writeCommand(command);
        uint8_t chunk[256];
        size_t filled = 0;
        for (uint16_t row = 0; row < rows; ++row) {
            const uint8_t* in = src + static_cast<size_t>(row) * srcStride;
            for (size_t i = 0; i < rowBytes; ++i) {
                chunk[filled++] = static_cast<uint8_t>(~in[i]);
                if (filled == sizeof(chunk)) {
                    _bus->writeData(chunk, filled);
                    filled = 0;
                }
            }
        }
        if (filled) _bus->writeData(chunk, filled);
    }

    const uint16_t _nativeStride;
    const uint16_t _nativeRows;

    uint8_t* _shadow = nullptr;
    bool _partial = false;
    bool _primeShadow = false;
    bool _seedController = false;
    uint16_t _winX = 0, _winY = 0, _winW = 0, _winH = 0;
};

// Same geometry as the library's Config_reTerminal_Sticky_SSD1677; only the
// driver differs. No `mirror` member on purpose -- the horizontal mirror comes
// from Board_reTerminal_Sticky, exactly as on the product-catalog path.
struct Config_Sticky_SSD1677_Fixed {
    using Driver = Driver_SSD1677_Sticky;
    using Panel  = Panel_EPaper;
    static constexpr uint16_t width      = 800;
    static constexpr uint16_t height     = 480;
    static constexpr uint8_t  colorDepth = 1;
};
