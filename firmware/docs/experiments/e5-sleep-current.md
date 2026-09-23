# E5 -- Deep sleep idle current

[Experiment index](../experiments.md)

**Status and conclusion.** Not taken. Baseline sleep current and battery-gauge accuracy remain unmeasured; do not infer them from a displayed percentage.

The record below preserves the conditions and decisions at the time of the
measurement. S-numbers identify historical implementation stages; D4/D6 were
resolved by streaming and partial refresh. Current behavior is described in
[implementation.md](../implementation.md). Commands and plain `src/` / `tools/`
paths are relative to `firmware/`.

**Why.** The device spends nearly all of its life asleep, so idle current is
what battery life is made of. It is also the one number behind a choice
`stickyPower::prepareDeepSleep()` already makes: the AI button is active low and
its pull-up has to survive the sleep, so the RTC peripheral domain is kept
powered with `esp_sleep_pd_config(ESP_PD_DOMAIN_RTC_PERIPH, ESP_PD_OPTION_ON)`.
That was the safe option rather than a measured one. The board may well have an
external pull-up on GPIO4, in which case the domain can go down and the line
comes out.

**What to measure.** Deep sleep current in two configurations:

1. as it stands, with the RTC peripheral domain powered;
2. with that call removed -- checking first that the button still wakes the
   board at all, which is the whole reason the line is there.

Battery life follows from the first number, the wake budget from E1 and however
many questions a day the device is asked.

**How.** No meter, no opening the case, nothing to solder: the board carries its
own instrument. The BQ27220 fuel gauge on the sensor I2C bus sits in the battery
path with its own sense resistor and keeps integrating charge while the ESP32 is
asleep. Standard commands, from the TRM (SLUUBD4A, Table 2-1); all of these are
readable with the gauge sealed:

| Command | Code | Unit |
| --- | --- | --- |
| `Voltage()` | 0x08 | mV |
| `Current()` | 0x0C | mA |
| `RemainingCapacity()` | 0x10 | mAh |
| `AverageCurrent()` | 0x14 | mA |
| `RawCoulombCount()` | 0x22 | mAh |
| `RelativeStateOfCharge()` | 0x2C | % |

**The current registers are the wrong tool.** They report whole milliamps, and a
sleeping ESP32-S3 is expected to draw tens or hundreds of microamps, which reads
as zero. Worse, the gauge can only be read with the host awake, and an awake
host is not the thing being measured.

So the number comes from charge accumulated over a long window instead:
`RemainingCapacity()` or `RawCoulombCount()` at the start, again at the end,
divided by the elapsed time. The device can time that itself -- the RTC counter
runs through deep sleep, so `esp_rtc_get_time_us()` at each wake gives the
interval with no external clock.

Shape of the run: press the button once to take the opening reading, leave the
device asleep for a day or more, press again for the closing one. At 1 mAh
resolution, a day at 100 uA is about 2.4 mAh, so the window has to be long --
and the longer it is, the less the reading wakes themselves matter. Then repeat
with the `esp_sleep_pd_config()` line removed, having first checked the button
still wakes the board at all.

**Watch for.**

- USB feeds the rail, so this only means anything on battery -- and a monitor
  attached afterwards resets the board, so the result has to survive that.
  E-paper holds an image without power and is the natural place to put it; NVS
  is the other. RTC memory will not do, for the reason in E1's battery
  procedure.
- The gauge measures what the battery delivers, so the figure covers the gauge
  itself and anything else always on, not only the ESP32. That is the right
  number for battery life. For the RTC-domain question the two runs are compared
  and whatever is common to both cancels.
- Absolute capacity from a gauge that has never learned this pack can be well
  off. What matters here is the difference between two readings, not either one.
- Nobody has talked to this gauge yet on this unit. Confirming it answers at
  0x55 and returns a plausible voltage is the first step -- and the answer
  screen needs exactly that code anyway, which is why it waits for this
  experiment rather than bringing the bus up on the path where a failure costs
  the user their answer ([D3](../deferred.md)).
