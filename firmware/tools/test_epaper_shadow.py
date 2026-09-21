#!/usr/bin/env python3
"""Host regression for SSD1677 shadow priming; no physical waveform simulation.

Run with python3 tools/test_epaper_shadow.py. Compiles the actual driver header
against a fake bus with two RAM planes initially filled with different garbage.
"""
from pathlib import Path
import subprocess
import tempfile

HEADERS = {
    'Seeed_GFX.h': '#pragma once\n#include <cstdint>\n#include <cstddef>\n',
    'esp_heap_caps.h': '''#pragma once
#include <cstdlib>
constexpr int MALLOC_CAP_SPIRAM=1, MALLOC_CAP_8BIT=2;
inline void* heap_caps_malloc(size_t n, int) { return malloc(n); }
inline void heap_caps_free(void* p) { free(p); }
''',
    'board/boards/reTerminal_EPaper_Boards.h': '#pragma once\n',
    'panel/Panel_EPaper.h': '#pragma once\nclass Panel_EPaper {};\n',
    'driver/epaper/Driver_SSD1677.h': '''#pragma once
#include <vector>
#include <cassert>
struct Bus {
  uint8_t command=0;
  std::vector<uint8_t> oldRam=std::vector<uint8_t>(48000,0x35);
  std::vector<uint8_t> newRam=std::vector<uint8_t>(48000,0xca);
  int x0=0,y0=0,x1=99,y1=479,x=0,y=0;
  size_t imageBytes=0;
  void window(int xs,int ys,int xe,int ye) {
    x=x0=xs/8; y=y0=ys; x1=xe/8; y1=ye;
  }
  void writeCommand(uint8_t c) { command=c; }
  void writeData(uint8_t b) {
    if(command!=0x24 && command!=0x26) return;
    auto& ram=command==0x24 ? newRam : oldRam;
    ram.at(y*100+x)=b;
    ++imageBytes;
    if(++x>x1) { x=x0; if(++y>y1) y=y0; }
  }
  void writeData(const uint8_t* p,size_t n) { while(n--) writeData(*p++); }
};
class Driver_SSD1677 {
public:
  Driver_SSD1677(uint16_t,uint16_t,int8_t) {}
  virtual ~Driver_SSD1677()=default;
  virtual const char* name() const { return "stub"; }
  virtual void sleep() {}
  virtual void wakePartial() {}
  virtual void updatePartial() { ++updates; }
  virtual void pushOldColors(const uint8_t*,size_t) {}
  virtual void pushNewColors(const uint8_t*,size_t) {}
  virtual void setAddrWindow(uint16_t xs,uint16_t ys,uint16_t xe,uint16_t ye) {
    bus.window(xs,ys,xe,ye);
  }
  Bus bus;
  Bus* _bus=&bus;
  int updates=0;
};
''',
}
TEST = r'''
#include "sticky/epaper.h"
#include <cassert>
#include <iostream>
int main() {
  // Off/on and on/off, at the icon window and at both native RAM boundaries.
  for(bool wasOn : {false,true}) for(int pos : {0,1,2}) {
    Driver_SSD1677_Sticky d;
    const int x=pos==0 ? 0 : pos==1 ? 176 : 784;
    const int y=pos==0 ? 0 : pos==1 ? 8 : 478;
    const uint8_t ink[]={0x81,0x42,0x24,0x18};
    const uint8_t white[]={0,0,0,0};
    const auto* oldPixels=wasOn ? ink : white;
    const auto* newPixels=wasOn ? white : ink;
    assert(d.beginShadowPrime());
    d.wakePartial();
    d.setAddrWindow(x,y,x+15,y+1);
    d.pushNewColors(oldPixels,4);
    d.updatePartial();
    assert(d.updates==0 && d.bus.imageBytes==0);
    d.endShadowPrime();
    d.wakePartial();
    d.setAddrWindow(x,y,x+15,y+1);
    d.pushNewColors(newPixels,4);
    d.updatePartial();
    assert(d.updates==1 && d.bus.imageBytes==96008);
    for(int row=0;row<480;++row) for(int col=0;col<100;++col) {
      int address=row*100+col;
      if(row>=y && row<y+2 && col>=x/8 && col<x/8+2) {
        int i=(row-y)*2+col-x/8;
        assert(d.bus.oldRam[address]==uint8_t(~oldPixels[i]));
        assert(d.bus.newRam[address]==uint8_t(~newPixels[i]));
      } else {
        assert(d.bus.oldRam[address]==0xff);
        assert(d.bus.newRam[address]==d.bus.oldRam[address]);
      }
    }
    // Subsequent partial: no reseeding, previous image advances correctly.
    d.setAddrWindow(x,y,x+15,y+1);
    d.pushNewColors(oldPixels,4);
    assert(d.bus.imageBytes==96016);
    for(int i=0;i<4;++i)
      assert(d.bus.oldRam[(y+i/2)*100+x/8+i%2]==uint8_t(~newPixels[i]));
  }
  std::cout << "PASS: no refresh while priming; both RAM planes initialized; "
               "only window differs; polarity, row stride and next partial correct\n";
}
'''
with tempfile.TemporaryDirectory(prefix='sticky-shadow-') as directory:
    folder = Path(directory)
    for name, content in HEADERS.items():
        file = folder / name
        file.parent.mkdir(parents=True, exist_ok=True)
        file.write_text(content)
    (folder / 'test.cpp').write_text(TEST)
    src = Path(__file__).resolve().parents[1] / 'src'
    subprocess.run(['c++', '-std=c++17', f'-I{folder}', f'-I{src}',
                    str(folder / 'test.cpp'), '-o', str(folder / 'test')], check=True)
    subprocess.run([str(folder / 'test')], check=True)
